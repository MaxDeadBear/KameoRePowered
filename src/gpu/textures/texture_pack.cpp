#include "gpu/textures/texture_pack.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#define XXH_INLINE_ALL
#include <xxhash.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_TGA
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#include "stb/stb_image.h"

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include "gpu/log.h"
#include "gpu/textures/texture_codec.h"

REXCVAR_DEFINE_BOOL(kameo_gfx_dump_textures, true, "Kameo", "Write each distinct texture the game draws with to <kameo_gfx_texture_dir>/dump/<level it loaded in, or global>/<width>x<height>/, named by a hash of its contents so a copy placed in replace/ swaps it in.");
REXCVAR_DEFINE_STRING(kameo_gfx_dump_texture_format, "png", "Kameo", "File format for dumped textures: png or tga.");
REXCVAR_DEFINE_BOOL(kameo_gfx_texture_replacements, true, "Kameo", "Replace textures with images from <kameo_gfx_texture_dir>/replace/ whose file name starts with the 16-digit hash of a dumped texture; png, tga, jpg and bmp at any resolution.");
REXCVAR_DEFINE_BOOL(kameo_gfx_texture_live_reload, true, "Kameo", "Watch the replace/ folder while the game runs: an added or edited image swaps in once it has finished saving, and a deleted one restores the original texture.");
REXCVAR_DEFINE_STRING(kameo_gfx_texture_dir, "textures", "Kameo", "Folder holding the dump/ and replace/ texture folders, relative to the working directory.");
REXCVAR_DEFINE_COMMAND(kameo_gfx_rescan_textures, []() { kameo::gpu::RescanTextureReplacements(); }, "Kameo", "Re-read the replace/ folder, for images added while the game is running.");

namespace kameo::gpu {
namespace {

class JobQueue {
 public:
  JobQueue(uint32_t threads) {
    for (uint32_t i = 0; i < threads; ++i) std::thread([this] { Run(); }).detach();
  }

  bool Push(std::function<void()> job, size_t limit) {
    {
      std::lock_guard lock(mutex_);
      if (jobs_.size() >= limit) return false;
      jobs_.push_back(std::move(job));
    }
    wake_.notify_one();
    return true;
  }

 private:
  void Run() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock lock(mutex_);
        wake_.wait(lock, [this] { return !jobs_.empty(); });
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      job();
    }
  }

  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<std::function<void()>> jobs_;
};

// Never destroyed: the workers are detached and the process hard-exits on close.
JobQueue& DumpQueue() {
  static auto* queue = new JobQueue(std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u));
  return *queue;
}

JobQueue& DecodeQueue() {
  static auto* queue = new JobQueue(std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u));
  return *queue;
}

std::filesystem::path TextureDir(const char* sub) { return std::filesystem::path(REXCVAR_GET(kameo_gfx_texture_dir)) / sub; }

std::mutex g_dumpMutex;
// Every hash whose pixel hash was registered, and the subset whose file was actually written; dumping can be off the first time a texture is seen.
std::unordered_set<uint64_t> g_dumped;
std::unordered_set<uint64_t> g_dumpWritten;
std::atomic<bool> g_dumpLoadedRequested{false};
// The level each texture asset was loaded for, keyed by its texture object; the loader passes no asset names.
std::mutex g_assetLevelMutex;
std::unordered_map<uint32_t, std::string> g_assetLevels;

std::string FolderName(const std::string& text) {
  std::string out;
  for (char c : text.substr(0, 64)) out.push_back(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ? c : '_');
  return out.empty() ? "global" : out;
}
std::atomic<uint64_t> g_dumpsWritten{0};
std::atomic<uint64_t> g_dumpsDropped{0};
std::atomic<uint64_t> g_dumpQueuedBytes{0};

struct ReplacementFile {
  uint64_t hash = 0;
  std::filesystem::file_time_type written;
  uintmax_t size = 0;
  bool operator==(const ReplacementFile&) const = default;
};
using ReplacementScan = std::unordered_map<std::filesystem::path::string_type, ReplacementFile>;

std::mutex g_packMutex;
bool g_indexed = false;
ReplacementScan g_files;
std::unordered_map<uint64_t, std::filesystem::path> g_index;
std::unordered_map<uint64_t, uint64_t> g_generation;
// The same art can reach the GPU as different texel bytes, so textures whose decoded pixels match share one replacement.
std::unordered_map<uint64_t, uint64_t> g_pixelHashOf;
std::unordered_map<uint64_t, std::vector<uint64_t>> g_contentHashesOf;
std::vector<uint64_t> g_changed;
std::unordered_map<uint64_t, std::shared_ptr<DecodedReplacement>> g_ready;
std::unordered_set<uint64_t> g_inFlight;
std::unordered_set<uint64_t> g_failed;
std::atomic<uint64_t> g_decodesFailed{0};
std::atomic<uint64_t> g_decodeNs{0};

bool ParseHash(const std::string& stem, uint64_t* out) {
  if (stem.size() < 16) return false;
  uint64_t value = 0;
  for (size_t i = 0; i < 16; ++i) {
    const char c = stem[i];
    const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
    if (digit < 0) return false;
    value = (value << 4) | uint64_t(digit);
  }
  *out = value;
  return true;
}

ReplacementScan ScanReplaceDir() {
  ReplacementScan scan;
  const std::filesystem::path dir = TextureDir("replace");
  std::error_code ec;
  if (!std::filesystem::is_directory(dir, ec)) return scan;
  for (auto it = std::filesystem::recursive_directory_iterator(dir, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
    std::error_code entry_ec;
    if (!it->is_regular_file(entry_ec)) continue;
    std::string ext = it->path().extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (ext != ".png" && ext != ".tga" && ext != ".jpg" && ext != ".jpeg" && ext != ".bmp") continue;
    ReplacementFile file;
    if (!ParseHash(it->path().stem().string(), &file.hash)) continue;
    file.written = it->last_write_time(entry_ec);
    file.size = it->file_size(entry_ec);
    scan[it->path().native()] = file;
  }
  return scan;
}

void MarkOneChangedLocked(uint64_t hash, bool notify) {
  ++g_generation[hash];
  g_ready.erase(hash);
  g_failed.erase(hash);
  if (notify) g_changed.push_back(hash);
}

void MarkChangedLocked(uint64_t hash, bool notify) {
  const auto pixels = g_pixelHashOf.find(hash);
  if (pixels == g_pixelHashOf.end()) {
    MarkOneChangedLocked(hash, notify);
    return;
  }
  for (uint64_t sibling : g_contentHashesOf[pixels->second]) MarkOneChangedLocked(sibling, notify);
}

const std::filesystem::path* ResolveReplacementLocked(uint64_t hash) {
  if (const auto indexed = g_index.find(hash); indexed != g_index.end()) return &indexed->second;
  const auto pixels = g_pixelHashOf.find(hash);
  if (pixels == g_pixelHashOf.end()) return nullptr;
  for (uint64_t sibling : g_contentHashesOf[pixels->second]) {
    if (const auto indexed = g_index.find(sibling); indexed != g_index.end()) return &indexed->second;
  }
  return nullptr;
}

std::string ReplacementName(const std::filesystem::path::string_type& path) {
  std::error_code ec;
  const std::filesystem::path relative = std::filesystem::relative(std::filesystem::path(path), TextureDir("replace"), ec);
  return (ec ? std::filesystem::path(path) : relative).string();
}

// Only a file seen identically in `previous` counts, so an image still being saved is left alone until its size and time stop moving.
void ApplyScanLocked(const ReplacementScan& current, const ReplacementScan* previous, bool notify) {
  uint32_t changed = 0, removed = 0;
  constexpr uint32_t kNamedChanges = 16;
  for (const auto& [path, file] : current) {
    if (previous) {
      const auto seen = previous->find(path);
      if (seen == previous->end() || !(seen->second == file)) continue;
    }
    const auto known = g_files.find(path);
    if (known != g_files.end() && known->second == file) continue;
    const bool added = known == g_files.end();
    if (!added && known->second.hash != file.hash) MarkChangedLocked(known->second.hash, notify);
    g_files[path] = file;
    g_index[file.hash] = path;
    MarkChangedLocked(file.hash, notify);
    if (notify && ++changed <= kNamedChanges) KGFX_INFO("texture pack: {} {}", added ? "added" : "changed", ReplacementName(path));
  }
  for (auto it = g_files.begin(); it != g_files.end();) {
    if (current.contains(it->first) || (previous && previous->contains(it->first))) {
      ++it;
      continue;
    }
    const uint64_t hash = it->second.hash;
    const std::filesystem::path path = it->first;
    it = g_files.erase(it);
    if (auto indexed = g_index.find(hash); indexed != g_index.end() && indexed->second == path) {
      g_index.erase(indexed);
      for (const auto& [other_path, other] : g_files) {
        if (other.hash == hash) g_index[hash] = other_path;
      }
    }
    MarkChangedLocked(hash, notify);
    if (notify && ++removed <= kNamedChanges) KGFX_INFO("texture pack: removed {}", ReplacementName(path.native()));
  }
  if (notify && changed + removed > kNamedChanges) KGFX_INFO("texture pack: {} replacement image(s) added or changed, {} removed", changed, removed);
}

void WatchReplaceDir() {
  ReplacementScan previous;
  bool have_previous = false;
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (!REXCVAR_GET(kameo_gfx_texture_live_reload) || !REXCVAR_GET(kameo_gfx_texture_replacements)) {
      have_previous = false;
      continue;
    }
    ReplacementScan current = ScanReplaceDir();
    if (have_previous) {
      std::lock_guard lock(g_packMutex);
      ApplyScanLocked(current, &previous, true);
    }
    previous = std::move(current);
    have_previous = true;
  }
}

void BuildIndexLocked() {
  g_indexed = true;
  ApplyScanLocked(ScanReplaceDir(), nullptr, false);
  KGFX_INFO("texture pack: {} replacement image(s) in {}", g_index.size(), TextureDir("replace").string());
  std::thread(WatchReplaceDir).detach();
}

bool ReadFile(const std::filesystem::path& path, std::vector<uint8_t>* out) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) return false;
  out->resize(size_t(file.tellg()));
  file.seekg(0);
  return bool(file.read(reinterpret_cast<char*>(out->data()), std::streamsize(out->size())));
}

void Downsample(const ReplacementLevel& src, ReplacementLevel* dst) {
  dst->width = std::max(1u, src.width / 2);
  dst->height = std::max(1u, src.height / 2);
  dst->rgba.resize(size_t(dst->width) * dst->height * 4);
  for (uint32_t y = 0; y < dst->height; ++y) {
    const uint32_t y0 = std::min(y * 2, src.height - 1), y1 = std::min(y * 2 + 1, src.height - 1);
    for (uint32_t x = 0; x < dst->width; ++x) {
      const uint32_t x0 = std::min(x * 2, src.width - 1), x1 = std::min(x * 2 + 1, src.width - 1);
      const uint8_t* a = src.rgba.data() + (size_t(y0) * src.width + x0) * 4;
      const uint8_t* b = src.rgba.data() + (size_t(y0) * src.width + x1) * 4;
      const uint8_t* c = src.rgba.data() + (size_t(y1) * src.width + x0) * 4;
      const uint8_t* d = src.rgba.data() + (size_t(y1) * src.width + x1) * 4;
      uint8_t* o = dst->rgba.data() + (size_t(y) * dst->width + x) * 4;
      for (int ch = 0; ch < 4; ++ch) o[ch] = uint8_t((uint32_t(a[ch]) + b[ch] + c[ch] + d[ch] + 2) / 4);
    }
  }
}

std::shared_ptr<DecodedReplacement> Decode(const std::filesystem::path& path, ReplacementChannels channels) {
  std::vector<uint8_t> file;
  if (!ReadFile(path, &file) || file.size() > size_t(INT32_MAX)) return nullptr;
  int width = 0, height = 0, components = 0;
  stbi_uc* pixels = stbi_load_from_memory(file.data(), int(file.size()), &width, &height, &components, 4);
  if (!pixels) return nullptr;
  auto decoded = std::make_shared<DecodedReplacement>();
  ReplacementLevel top;
  top.width = uint32_t(width);
  top.height = uint32_t(height);
  top.rgba.assign(pixels, pixels + size_t(width) * height * 4);
  stbi_image_free(pixels);
  // A replacement uploads as RGBA8, so channels the original's BC4 or BC5 format never stored must sample as zero like they did.
  if (channels != ReplacementChannels::Rgba) {
    for (size_t i = 0; i < top.rgba.size(); i += 4) {
      if (channels == ReplacementChannels::RedOnly) top.rgba[i + 1] = 0;
      top.rgba[i + 2] = 0;
      top.rgba[i + 3] = 255;
    }
  }
  decoded->levels.push_back(std::move(top));
  while (decoded->levels.back().width > 1 || decoded->levels.back().height > 1) {
    ReplacementLevel next;
    Downsample(decoded->levels.back(), &next);
    decoded->levels.push_back(std::move(next));
  }
  for (const ReplacementLevel& level : decoded->levels) decoded->bytes += level.rgba.size();
  return decoded;
}

void RegisterPixelHash(uint64_t hash, uint64_t pixel_hash) {
  std::lock_guard lock(g_packMutex);
  g_pixelHashOf[hash] = pixel_hash;
  std::vector<uint64_t>& siblings = g_contentHashesOf[pixel_hash];
  if (std::find(siblings.begin(), siblings.end(), hash) != siblings.end()) return;
  siblings.push_back(hash);
  // A texture uploaded before its twin's image was known has to be told it now has a replacement.
  if (!g_index.contains(hash) && siblings.size() > 1 && ResolveReplacementLocked(hash)) MarkOneChangedLocked(hash, true);
}

void WriteDump(uint64_t hash, const std::string& level, uint32_t width, uint32_t height, uint32_t format, const std::vector<uint8_t>& pixels, uint32_t src_row_bytes, bool block_compressed, bool write_file) {
  const std::string kind = REXCVAR_GET(kameo_gfx_dump_texture_format) == "tga" ? "tga" : "png";
  const std::filesystem::path dir = TextureDir("dump") / FolderName(level) / fmt::format("{}x{}", width, height);
  const std::filesystem::path path = dir / fmt::format("{:016X}_{}x{}_fmt{}.{}", hash, width, height, format, kind);
  std::error_code ec;
  std::vector<uint8_t> rgba;
  if (block_compressed) {
    rgba.assign(size_t(width) * height * 4, 0xFF);
    DecodeBcToRgba(pixels.data(), src_row_bytes, rgba.data(), width * 4, width, height, format);
  } else {
    rgba.resize(size_t(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) std::memcpy(rgba.data() + size_t(y) * width * 4, pixels.data() + size_t(y) * src_row_bytes, size_t(width) * 4);
  }
  RegisterPixelHash(hash, XXH3_64bits_withSeed(rgba.data(), rgba.size(), (uint64_t(width) << 32) | height));
  if (!write_file || std::filesystem::exists(path, ec)) return;
  std::vector<uint8_t> encoded;
  const auto append = [](void* context, void* data, int size) { auto* out = static_cast<std::vector<uint8_t>*>(context); out->insert(out->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size); };
  const int ok = kind == "tga" ? stbi_write_tga_to_func(append, &encoded, int(width), int(height), 4, rgba.data()) : stbi_write_png_to_func(append, &encoded, int(width), int(height), 4, rgba.data(), int(width) * 4);
  if (!ok) return;
  std::filesystem::create_directories(dir, ec);
  std::ofstream file(path, std::ios::binary);
  if (file.write(reinterpret_cast<const char*>(encoded.data()), std::streamsize(encoded.size()))) ++g_dumpsWritten;
}

}  // namespace

uint64_t TextureContentHash(const uint8_t* data, size_t bytes, uint32_t width, uint32_t height, uint32_t format, uint32_t swizzle) {
  const uint64_t seed = (uint64_t(width) << 40) ^ (uint64_t(height) << 20) ^ (uint64_t(format) << 12) ^ swizzle;
  return XXH3_64bits_withSeed(data, bytes, seed);
}

void NoteTextureAssetLevel(uint32_t object, const std::string& level) {
  std::lock_guard lock(g_assetLevelMutex);
  g_assetLevels[object] = level;
}

void QueueTextureDump(uint64_t hash, uint32_t object, uint32_t width, uint32_t height, uint32_t format, std::vector<uint8_t>&& pixels, uint32_t src_row_bytes, bool block_compressed, bool write_file) {
  bool newly_registered = false;
  bool newly_written = false;
  {
    std::lock_guard lock(g_dumpMutex);
    if (g_dumped.contains(hash) && (!write_file || g_dumpWritten.contains(hash))) return;
    newly_registered = g_dumped.insert(hash).second;
    newly_written = write_file && g_dumpWritten.insert(hash).second;
  }
  std::string level;
  {
    std::lock_guard lock(g_assetLevelMutex);
    if (const auto loaded = g_assetLevels.find(object); loaded != g_assetLevels.end()) level = loaded->second;
  }
  // A level load uploads hundreds of textures at once, so the queue is bounded by the texel bytes it holds rather than by a job count that silently dropped them.
  constexpr uint64_t kMaxQueuedDumpBytes = 2ull << 30;
  const uint64_t bytes = pixels.size();
  if (g_dumpQueuedBytes.load() + bytes > kMaxQueuedDumpBytes) {
    std::lock_guard lock(g_dumpMutex);
    if (newly_registered) g_dumped.erase(hash);
    if (newly_written) g_dumpWritten.erase(hash);
    if (++g_dumpsDropped == 1) KGFX_WARN("texture pack: dump queue is full, so textures are going undumped until it drains");
    return;
  }
  g_dumpQueuedBytes += bytes;
  auto job = [hash, width, height, format, src_row_bytes, block_compressed, write_file, bytes, level = std::move(level), pixels = std::move(pixels)] {
    WriteDump(hash, level, width, height, format, pixels, src_row_bytes, block_compressed, write_file);
    g_dumpQueuedBytes -= bytes;
  };
  DumpQueue().Push(std::move(job), SIZE_MAX);
}

bool HasTextureReplacement(uint64_t hash) {
  if (!REXCVAR_GET(kameo_gfx_texture_replacements)) return false;
  std::lock_guard lock(g_packMutex);
  if (!g_indexed) BuildIndexLocked();
  return ResolveReplacementLocked(hash) != nullptr;
}

std::shared_ptr<DecodedReplacement> RequestTextureReplacement(uint64_t hash, ReplacementChannels channels) {
  std::filesystem::path path;
  uint64_t generation = 0;
  {
    std::lock_guard lock(g_packMutex);
    if (!g_indexed) BuildIndexLocked();
    if (auto ready = g_ready.find(hash); ready != g_ready.end()) {
      auto decoded = std::move(ready->second);
      g_ready.erase(ready);
      return decoded;
    }
    const std::filesystem::path* indexed = ResolveReplacementLocked(hash);
    if (!indexed || g_inFlight.contains(hash) || g_failed.contains(hash)) return nullptr;
    path = *indexed;
    generation = g_generation[hash];
    g_inFlight.insert(hash);
  }
  DecodeQueue().Push([hash, channels, path, generation] {
    const auto start = std::chrono::steady_clock::now();
    auto decoded = Decode(path, channels);
    g_decodeNs += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
    std::lock_guard lock(g_packMutex);
    g_inFlight.erase(hash);
    // The image changed while it decoded, so this result is stale and the next request decodes the new file.
    if (g_generation[hash] != generation) return;
    if (decoded) {
      g_ready[hash] = std::move(decoded);
    } else {
      g_failed.insert(hash);
      ++g_decodesFailed;
      KGFX_WARN("texture pack: could not decode {}", path.string());
    }
  }, SIZE_MAX);
  return nullptr;
}

void RescanTextureReplacements() {
  ReplacementScan current = ScanReplaceDir();
  std::lock_guard lock(g_packMutex);
  g_failed.clear();
  if (!g_indexed) {
    BuildIndexLocked();
    return;
  }
  ApplyScanLocked(current, nullptr, true);
}

void RefreshAllTextureReplacements() {
  std::lock_guard lock(g_packMutex);
  g_failed.clear();
  for (const auto& [hash, path] : g_index) MarkChangedLocked(hash, true);
}

bool TextureWasDumped(uint64_t hash) {
  std::lock_guard lock(g_dumpMutex);
  return g_dumpWritten.contains(hash);
}

void RequestDumpOfLoadedTextures() { g_dumpLoadedRequested.store(true); }

bool TakeDumpOfLoadedTexturesRequest() { return g_dumpLoadedRequested.exchange(false); }

std::vector<uint64_t> TakeChangedReplacements() {
  std::lock_guard lock(g_packMutex);
  std::vector<uint64_t> changed;
  changed.swap(g_changed);
  return changed;
}

}  // namespace kameo::gpu
