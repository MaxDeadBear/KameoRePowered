#include "shader_editor/game_textures.h"

#include <d3d11.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "shader_editor/shader_build.h"

namespace kameo::shader_editor {

namespace fs = std::filesystem;

namespace {

constexpr size_t kOpenArchives = 3;

fs::path Assets() { return EditorFolder() / "assets"; }
fs::path IndexCache() { return fs::temp_directory_path() / "KameoShaderEditor_types.txt"; }

template <typename T>
bool Ready(const std::future<T>& f) {
  return f.valid() && f.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

std::string Key(const std::string& archive, uint32_t entry) { return archive + "|" + std::to_string(entry); }

// A file's size and time, so the cache notices a different copy of the game.
std::string Stamp(const fs::path& file) {
  std::error_code ec;
  return std::to_string(fs::file_size(file, ec)) + ":" + std::to_string(fs::last_write_time(file, ec).time_since_epoch().count());
}

}  // namespace

GameTextureLibrary::~GameTextureLibrary() {
  if (indexing_.valid()) indexing_.wait();
  for (auto& [key, future] : reading_types_) future.wait();
  for (auto& [key, future] : reading_pixels_) future.wait();
  for (auto& [key, view] : views_)
    if (view) view->Release();
}

void GameTextureLibrary::Attach(ID3D11Device* device, ID3D11DeviceContext* context) {
  device_ = device;
  context_ = context;
}

void GameTextureLibrary::StartIndex() {
  if (indexing_.valid() || !types_.empty()) return;
  indexing_ = std::async(std::launch::async, [this] {
    // The cache: one line per file, its name, stamp and types.
    std::map<std::string, std::pair<std::string, std::vector<std::string>>> cached;
    {
      std::ifstream in(IndexCache());
      for (std::string line; std::getline(in, line);) {
        std::stringstream fields(line);
        std::string file, stamp, list;
        if (!std::getline(fields, file, '\t') || !std::getline(fields, stamp, '\t')) continue;
        std::getline(fields, list);
        std::vector<std::string> names;
        std::stringstream items(list);
        for (std::string name; std::getline(items, name, ',');) names.push_back(name);
        cached[file] = {stamp, std::move(names)};
      }
    }
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(Assets(), ec))
      if (entry.path().extension() == ".lvl") files.push_back(entry.path());
    std::sort(files.begin(), files.end());
    files_to_index_ = files.size();
    std::map<std::string, std::vector<std::string>> index;
    bool changed = false;
    for (const fs::path& file : files) {
      const std::string name = file.filename().string(), stamp = Stamp(file);
      if (auto found = cached.find(name); found != cached.end() && found->second.first == stamp) {
        index[name] = found->second.second;
      } else {
        game_archive::Archive archive;
        std::string error;
        if (archive.Load(file, error)) index[name] = game_archive::ObjectTypes(archive);
        changed = true;
      }
      ++indexed_files_;
    }
    if (changed) {
      std::ofstream out(IndexCache(), std::ios::trunc);
      for (const auto& [file, names] : index) {
        out << file << '\t' << Stamp(Assets() / file) << '\t';
        for (size_t i = 0; i < names.size(); ++i) out << (i ? "," : "") << names[i];
        out << '\n';
      }
    }
    return index;
  });
}

void GameTextureLibrary::Collect() {
  if (Ready(indexing_)) types_ = indexing_.get();
  for (auto it = reading_types_.begin(); it != reading_types_.end();) {
    if (!Ready(it->second)) {
      ++it;
      continue;
    }
    type_textures_[it->first] = it->second.get();
    it = reading_types_.erase(it);
  }
}

std::vector<GameTextureLibrary::Found> GameTextureLibrary::Find(const std::string& filter, size_t limit) {
  Collect();
  std::vector<Found> found;
  const std::string want = Lower(filter);
  // One of each type, from the first level that has it.
  std::map<std::string, std::string> first;
  for (const auto& [archive, names] : types_)
    for (const std::string& type : names)
      if (!type.empty() && Lower(type).find(want) != std::string::npos) first.emplace(type, archive);
  for (const auto& [type, archive] : first) {
    if (found.size() >= limit) break;
    found.push_back({type, archive});
  }
  return found;
}

std::shared_ptr<const game_archive::Archive> GameTextureLibrary::Open(const std::string& archive) {
  {
    std::lock_guard lock(mutex_);
    for (const auto& [name, open] : open_)
      if (name == archive) return open;
  }
  auto loaded = std::make_shared<game_archive::Archive>();
  std::string error;
  if (!loaded->Load(Assets() / archive, error)) return nullptr;
  std::lock_guard lock(mutex_);
  open_.emplace_back(archive, loaded);
  if (open_.size() > kOpenArchives) open_.erase(open_.begin());
  return loaded;
}

const std::vector<GameTextureRef>* GameTextureLibrary::TexturesOf(const std::string& archive, const std::string& type) {
  Collect();
  const std::string key = archive + "|" + type;
  if (const auto found = type_textures_.find(key); found != type_textures_.end()) return &found->second;
  if (!reading_types_.contains(key)) {
    reading_types_[key] = std::async(std::launch::async, [this, archive, type] {
      std::vector<GameTextureRef> textures;
      const std::shared_ptr<const game_archive::Archive> a = Open(archive);
      if (!a) return textures;
      for (const uint32_t entry : game_archive::TypeTextures(*a, type)) {
        // Only those the game's mods can read, as the game reads them the same way.
        if (!game_archive::ReadTexture(*a, {entry, 0})) continue;
        textures.push_back({archive, entry, a->U16({entry, 0}, 36), a->U16({entry, 0}, 38), type});
      }
      return textures;
    });
  }
  return nullptr;
}

ID3D11ShaderResourceView* GameTextureLibrary::View(const GameTextureRef& texture) {
  const std::string key = Key(texture.archive, texture.entry);
  if (const auto found = views_.find(key); found != views_.end()) return found->second;
  auto reading = reading_pixels_.find(key);
  if (reading == reading_pixels_.end()) {
    reading_pixels_[key] = std::async(std::launch::async, [this, texture]() -> std::shared_ptr<game_archive::Texture> {
      const std::shared_ptr<const game_archive::Archive> a = Open(texture.archive);
      if (!a || texture.entry > a->entry_count() || a->TypeName(texture.entry) != "texture") return nullptr;
      std::optional<game_archive::Texture> pixels = game_archive::ReadTexture(*a, {texture.entry, 0});
      return pixels ? std::make_shared<game_archive::Texture>(std::move(*pixels)) : nullptr;
    });
    return nullptr;
  }
  if (!Ready(reading->second)) return nullptr;
  const std::shared_ptr<game_archive::Texture> pixels = reading->second.get();
  reading_pixels_.erase(reading);
  ID3D11ShaderResourceView* view = nullptr;
  if (pixels && device_ && context_) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = pixels->width;
    desc.Height = pixels->height;
    desc.MipLevels = 0;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    ID3D11Texture2D* handle = nullptr;
    device_->CreateTexture2D(&desc, nullptr, &handle);
    if (handle) {
      context_->UpdateSubresource(handle, 0, nullptr, pixels->rgba.data(), pixels->width * 4, 0);
      device_->CreateShaderResourceView(handle, nullptr, &view);
      if (view) context_->GenerateMips(view);
      handle->Release();
    }
  }
  views_[key] = view;
  return view;
}

}  // namespace kameo::shader_editor
