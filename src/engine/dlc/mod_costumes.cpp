// Costumes made with the Blender add-on (tools/modkit/kameo_modkit/costume.py): each is a copy of one of the game's skin
// models in costumes/ beside the game, named <model type>_m<six hex digits>.mdl, with its name in costumes/costumes.toml.
// At start they are gathered into a package of their own under the content root, shaped like a downloadable one (a
// folder, its KameoDLCList.txt and a header), so the game lists each as a costume after the downloadable ones and no
// file of the game's is touched. The game mounts packages once, so a new costume shows after a restart; a costume file
// written again while the game runs is copied over and, if a character wears it, reloaded: the character changes to
// its standard look until the old model is freed, then back.

#include "engine/dlc/mod_costumes.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <vector>

#include <toml++/toml.hpp>

#include <regex>

#include <rex/logging/log_category.h>
#include <rex/platform/file.h>
#include <rex/kernel/types/content.h>
#include <rex/xam/device.h>

#include "core/guest.h"
#include "engine/dlc/dlc.h"
#include "engine/dlc/dlc_swap.h"
#include "engine/guest_addresses.h"
#include "kameorepowered_init.h"

namespace kameo::engine {
namespace {

constexpr const char* kSource = "costumes";
// Beside the executable, not in the working directory: a game started from elsewhere would see no costumes at all.
std::filesystem::path SourceFolder() { return rex::platform::GetExecutableFolder() / kSource; }
constexpr const char* kNamesFile = "costumes.toml";
// "KAMEOREPOWEREDCOSTUME" in hex: a content file name of the same 42 characters the downloadable packs have.
constexpr const char* kPackage = "4B414D454F5245504F5745524544434F5354554D45";
constexpr const char16_t* kPackageName = u"Kameo RePowered Costumes";
constexpr uint32_t kKameoTitleId = 0x4D5307D2;
// String ids no table carries, one per costume, after the shader mod costumes' (dlc_skin_names.cpp).
constexpr uint32_t kModCostumeString = 35300;
// objectAlternativeModelLists: a node per model type links on at +4 and keys its type at +8; its skins hang off a list
// at +36, each linking on at +4 and named at +51 (dlc_skin_names.cpp).
constexpr uint32_t kAlternativeModelListFirst = 0x8280D104;
constexpr uint32_t kPollTicks = 60;
constexpr uint32_t kReloadTimeoutTicks = 600;
constexpr uint32_t kSettleTicks = 30;

struct Costume {
  std::string model;  // "007183BF_m1A2B3C"
  std::u16string name;
  std::string name_text;  // as written in costumes.toml (UTF-8)
  bool worn = false;      // seen on the character at the last poll
  bool force = false;     // a reload asked for from the Models window
  uint32_t model_type = 0;
  bool listed = false;  // seen in the character's skin list, which the package's KameoDLCList.txt fills
  std::filesystem::file_time_type written;
  uintmax_t size = 0;
};

std::mutex g_mutex;
std::vector<Costume> g_costumes;
std::filesystem::path g_packageFolder;

struct Reload {
  uint32_t model_type = 0;
  std::string model;
  uint32_t stage = 0;
  uint32_t ticks = 0;
};
std::vector<Reload> g_reloads;

bool CostumeFileName(const std::string& name) {
  if (name.size() != 20 || name[8] != '_' || name[9] != 'm' || name.substr(16) != ".mdl") return false;
  for (size_t i : {0, 1, 2, 3, 4, 5, 6, 7, 10, 11, 12, 13, 14, 15}) {
    if (!std::isxdigit(static_cast<unsigned char>(name[i]))) return false;
  }
  return true;
}

std::u16string Utf16(const std::string& text) {
  std::u16string out;
  for (size_t i = 0; i < text.size();) {
    const uint8_t c = uint8_t(text[i]);
    uint32_t code = c, extra = 0;
    if (c >= 0xF0) code = c & 0x07, extra = 3;
    else if (c >= 0xE0) code = c & 0x0F, extra = 2;
    else if (c >= 0xC0) code = c & 0x1F, extra = 1;
    ++i;
    for (; extra && i < text.size(); --extra, ++i) code = (code << 6) | (uint8_t(text[i]) & 0x3F);
    if (code >= 0x10000) {
      code -= 0x10000;
      out.push_back(char16_t(0xD800 + (code >> 10)));
      out.push_back(char16_t(0xDC00 + (code & 0x3FF)));
    } else {
      out.push_back(char16_t(code));
    }
  }
  return out;
}

void WriteHeader(const std::filesystem::path& path) {
  rex::kernel::XCONTENT_AGGREGATE_DATA data;
  data.device_id = static_cast<uint32_t>(rex::xam::DummyDeviceID::kHDD);
  data.content_type = rex::kernel::XContentType::kMarketplaceContent;
  data.title_id = kKameoTitleId;
  data.xuid = 0;
  data.SetFileName(kPackage);
  data.SetDisplayName(kPackageName);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(&data), sizeof(data));
  // Every licence bit, so the title update counts the package as owned (dlc_packages.cpp).
  const uint32_t licence = 0xFFFFFFFFu;
  file.write(reinterpret_cast<const char*>(&licence), sizeof(licence));
}

std::string GuestName(uint8_t* base, uint32_t at) {
  std::string name;
  for (uint32_t i = 0; i < 24; ++i) {
    const char c = char(REX_LOAD_U8(at + i));
    if (!c) break;
    name.push_back(c);
  }
  return name;
}

uint32_t SkinNode(uint8_t* base, uint32_t model_type) {
  for (uint32_t node = REX_LOAD_U32(kAlternativeModelListFirst); node; node = REX_LOAD_U32(node + 4)) {
    if (REX_LOAD_U32(node + 8) == model_type) return node;
  }
  return 0;
}

// The skin record of that name in the character's list, or 0.
uint32_t SkinRecord(uint8_t* base, uint32_t node, const std::string& model) {
  for (uint32_t entry = REX_LOAD_U32(node + 36); entry; entry = REX_LOAD_U32(entry + 4)) {
    if (GuestName(base, entry + 51) == model) return entry;
  }
  return 0;
}

void SetFileInUse(PPCContext& ctx, uint8_t* base, uint32_t model_type, const std::string& suffix) {
  // In guest memory of its own: below the stack pointer, the function's own frame would write over it.
  static uint32_t text = 0;
  if (!text) text = GuestSystemAlloc(64);
  if (!text) return;
  PPCContext call = ctx;
  call.r3.u64 = model_type;
  WriteGuestStringAt(base, text, suffix.c_str());
  call.r4.u64 = text;
  // The title update's version also takes (have, flag), which the save's own calls pass as 0; the original ignores them.
  call.r5.u64 = 0;
  call.r6.u64 = 0;
  // A skin pinned by the hot-swap (kameo_dev_skin) would take the place of this one, standard look included (dlc.cpp).
  const int pinned = g_kameo_dlc_swap_enabled.exchange(0, std::memory_order_acq_rel);
  ObjectAlternativeSetFileInUse(call, base);
  g_kameo_dlc_swap_enabled.store(pinned, std::memory_order_release);
  REXLOG_INFO("[kameo] costumes: {:08X} set to '{}' -> {:08X}", model_type, suffix, call.r3.u32);
}

}  // namespace

void BuildModCostumePackage(const std::filesystem::path& user_data_root) {
  std::error_code ec;
  const std::filesystem::path content = user_data_root / "0000000000000000" / "4D5307D2";
  const std::filesystem::path folder = content / "00000002" / kPackage;
  const std::filesystem::path header = content / "Headers" / "00000002" / (std::string(kPackage) + ".header");

  // costumes.toml: "<file>" = "<name>", in the order the costumes were made.
  std::vector<std::pair<std::string, std::string>> named;
  if (std::filesystem::exists(SourceFolder() / kNamesFile, ec)) {
    try {
      const toml::table table = toml::parse_file((SourceFolder() / kNamesFile).string());
      for (const auto& [key, value] : table) {
        if (auto name = value.value<std::string>()) named.emplace_back(std::string(key.str()), *name);
      }
    } catch (const toml::parse_error& error) {
      REXLOG_WARN("[kameo] costumes: {} could not be read: {}", kNamesFile, error.description());
    }
  }
  std::vector<std::string> files;
  for (auto it = std::filesystem::directory_iterator(SourceFolder(), ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (it->is_regular_file() && CostumeFileName(name)) files.push_back(name);
  }
  std::sort(files.begin(), files.end(), [&](const std::string& a, const std::string& b) {
    const auto rank = [&](const std::string& f) { return std::find_if(named.begin(), named.end(), [&](const auto& n) { return n.first == f; }) - named.begin(); };
    return std::make_pair(rank(a), a) < std::make_pair(rank(b), b);
  });

  std::lock_guard lock(g_mutex);
  g_costumes.clear();
  if (files.empty()) {
    if (std::filesystem::exists(folder, ec)) {
      std::filesystem::remove_all(folder, ec);
      std::filesystem::remove(header, ec);
      REXLOG_INFO("[kameo] costumes: none left, package removed");
    }
    return;
  }
  std::filesystem::create_directories(folder, ec);
  std::filesystem::create_directories(header.parent_path(), ec);
  for (auto it = std::filesystem::directory_iterator(folder, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
    const std::string name = it->path().filename().string();
    if (name != "KameoDLCList.txt" && std::find(files.begin(), files.end(), name) == files.end()) std::filesystem::remove(it->path(), ec);
  }
  std::ofstream list(folder / "KameoDLCList.txt", std::ios::binary | std::ios::trunc);
  for (const std::string& file : files) {
    const std::filesystem::path source = SourceFolder() / file;
    std::filesystem::copy_file(source, folder / file, std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
      REXLOG_WARN("[kameo] costumes: could not copy {}: {}", file, ec.message());
      continue;
    }
    list << file << "\r\n";
    Costume costume;
    costume.model = file.substr(0, 16);
    costume.model_type = uint32_t(std::stoul(file.substr(0, 8), nullptr, 16));
    const auto name = std::find_if(named.begin(), named.end(), [&](const auto& n) { return n.first == file; });
    costume.name_text = name != named.end() ? name->second : costume.model;
    costume.name = Utf16(costume.name_text);
    costume.written = std::filesystem::last_write_time(source, ec);
    costume.size = std::filesystem::file_size(source, ec);
    g_costumes.push_back(std::move(costume));
  }
  list.close();
  WriteHeader(header);
  g_packageFolder = folder;
  REXLOG_INFO("[kameo] costumes: {} in the package", g_costumes.size());
}

std::u16string ModCostumeName(std::string_view model) {
  std::lock_guard lock(g_mutex);
  for (const Costume& costume : g_costumes) {
    if (costume.model == model) return costume.name;
  }
  return {};
}

uint32_t ModCostumeStringId(std::string_view model) {
  std::lock_guard lock(g_mutex);
  for (size_t i = 0; i < g_costumes.size(); ++i) {
    if (g_costumes[i].model == model) return kModCostumeString + uint32_t(i);
  }
  return 0;
}

std::u16string ModCostumeNameOfString(uint32_t string_id) {
  std::lock_guard lock(g_mutex);
  if (string_id < kModCostumeString || string_id - kModCostumeString >= g_costumes.size()) return {};
  return g_costumes[string_id - kModCostumeString].name;
}

std::filesystem::path ModCostumeFolder() { return SourceFolder(); }

std::vector<ModCostumeInfo> ListModCostumes() {
  std::lock_guard lock(g_mutex);
  std::vector<ModCostumeInfo> out;
  for (const Costume& costume : g_costumes) {
    out.push_back({costume.model + ".mdl", costume.model, costume.name_text, costume.model_type, costume.listed, costume.worn});
  }
  return out;
}

void RequestModCostumeReload(std::string_view model) {
  std::lock_guard lock(g_mutex);
  for (Costume& costume : g_costumes) {
    if (costume.model == model) costume.force = true;
  }
}

namespace {
uint32_t CostumeIdOf(const std::string& name) {
  uint32_t hash = 2166136261u;
  for (const char16_t c : Utf16(name)) hash = (hash ^ c) * 16777619u;
  hash &= 0xFFFFFF;
  return hash ? hash : 1;
}

// costumes.toml as the Blender add-on writes it, in order: "<file>" = "<name>".
std::vector<std::pair<std::string, std::string>> ReadNameLines(const std::filesystem::path& path) {
  std::vector<std::pair<std::string, std::string>> out;
  std::ifstream in(path, std::ios::binary);
  static const std::regex line(R"re(\s*"([^"]+)"\s*=\s*"((?:[^"\\]|\\.)*)")re");
  std::string text;
  while (std::getline(in, text)) {
    std::smatch m;
    if (!std::regex_search(text, m, line) || m.position() != 0) continue;
    std::string name;
    const std::string raw = m[2];
    for (size_t i = 0; i < raw.size(); ++i) name.push_back(raw[i] == '\\' && i + 1 < raw.size() ? raw[++i] : raw[i]);
    out.emplace_back(m[1], name);
  }
  return out;
}
}  // namespace

std::string CreateModCostume(const std::filesystem::path& base_mdl, const std::string& name, std::string* file_out) {
  if (name.empty()) return "Give the costume a name.";
  const std::string base = base_mdl.filename().string();
  if (base.size() < 8 || !std::all_of(base.begin(), base.begin() + 8, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); })) {
    return "That file is not one of the game's models (its name should start with eight hex digits).";
  }
  std::string type = base.substr(0, 8);
  for (char& c : type) c = char(std::toupper(static_cast<unsigned char>(c)));
  char id[8];
  std::snprintf(id, sizeof(id), "%06X", CostumeIdOf(name));
  const std::string file = type + "_m" + id + ".mdl";
  const std::filesystem::path folder = SourceFolder();
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  if (std::filesystem::exists(folder / file, ec)) return "A costume with that name (or one that hashes the same) already exists: " + file;
  std::filesystem::copy_file(base_mdl, folder / (file + ".tmp"), std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) return "Could not copy " + base + ": " + ec.message();
  std::filesystem::rename(folder / (file + ".tmp"), folder / file, ec);
  if (ec) return "Could not write " + file + ": " + ec.message();
  auto names = ReadNameLines(folder / kNamesFile);
  names.emplace_back(file, name);
  const auto quoted = [](const std::string& text) {
    std::string out = "\"";
    for (char c : text) {
      if (c == '\\' || c == '"') out.push_back('\\');
      out.push_back(c);
    }
    return out + "\"";
  };
  std::ofstream out(folder / kNamesFile, std::ios::binary | std::ios::trunc);
  out << "# Costumes made with the Blender add-on: file = the name the game shows.\n";
  for (const auto& [f, n] : names) out << quoted(f) << " = " << quoted(n) << "\n";
  if (file_out) *file_out = file;
  return {};
}

void TickModCostumes(PPCContext& ctx, uint8_t* base) {
  static uint32_t ticks = 0;
  if (++ticks % kPollTicks == 0) {
    std::lock_guard lock(g_mutex);
    for (Costume& costume : g_costumes) {
      if (!costume.listed) {
        const uint32_t node = SkinNode(base, costume.model_type);
        if (node && SkinRecord(base, node, costume.model)) {
          costume.listed = true;
          REXLOG_INFO("[kameo] costumes: {} is listed as a costume", costume.model);
        }
      }
      {
        const uint32_t node = SkinNode(base, costume.model_type);
        const uint32_t worn = node ? REX_LOAD_U32(node + 12) : 0;
        costume.worn = worn && GuestName(base, worn + 51) == costume.model;
      }
      std::error_code ec;
      const std::filesystem::path source = SourceFolder() / (costume.model + ".mdl");
      const auto written = std::filesystem::last_write_time(source, ec);
      const auto size = std::filesystem::file_size(source, ec);
      if (!costume.force && (ec || (written == costume.written && size == costume.size))) continue;
      costume.force = false;
      std::filesystem::copy_file(source, g_packageFolder / (costume.model + ".mdl"), std::filesystem::copy_options::overwrite_existing, ec);
      if (ec) continue;  // tried again next time
      costume.written = written;
      costume.size = size;
      const bool wearing = costume.worn;
      REXLOG_INFO("[kameo] costumes: {} written again{}", costume.model, wearing ? ", reloading it" : "");
      if (wearing && std::none_of(g_reloads.begin(), g_reloads.end(), [&](const Reload& r) { return r.model == costume.model; })) {
        g_reloads.push_back({costume.model_type, costume.model});
      }
    }
  }
  for (auto it = g_reloads.begin(); it != g_reloads.end();) {
    Reload& reload = *it;
    ++reload.ticks;
    const uint32_t node = SkinNode(base, reload.model_type);
    if (!node) {
      it = g_reloads.erase(it);
      continue;
    }
    // A change made while the last one is still loading (busy +20, loading +28) is dropped, so each step waits for the
    // character to settle.
    const bool settled = REX_LOAD_U32(node + 20) == 0 && REX_LOAD_U32(node + 28) == 0 && reload.ticks >= kSettleTicks;
    const uint32_t worn = REX_LOAD_U32(node + 12);
    const std::string wearing = worn ? GuestName(base, worn + 51) : std::string();
    if (reload.stage == 0) {
      SetFileInUse(ctx, base, reload.model_type, "");
      reload.stage = 1;
      reload.ticks = 0;
    } else if (reload.stage == 1 && settled) {
      // Back on once the old model's level is freed, or after a while without: a level still loaded would be used as
      // it is rather than read again.
      const uint32_t record = SkinRecord(base, node, reload.model);
      PPCContext call = ctx;
      call.r3.u64 = record + 51;
      if (record) sub_822C6618(call, base);  // the level's object count, 0 once freed
      if (record && call.r3.u32 != 0 && reload.ticks < kReloadTimeoutTicks) {
        ++it;
        continue;
      }
      if (record && call.r3.u32 != 0) REXLOG_WARN("[kameo] costumes: {} still loaded; it may show as it was", reload.model);
      SetFileInUse(ctx, base, reload.model_type, reload.model.substr(9));
      reload.stage = 2;
      reload.ticks = 0;
    } else if (reload.stage >= 2 && settled) {
      if (wearing == reload.model) {
        REXLOG_INFO("[kameo] costumes: {} reloaded", reload.model);
        it = g_reloads.erase(it);
        continue;
      }
      if (reload.stage == 2) {
        REXLOG_INFO("[kameo] costumes: {} didn't go back on ({}); trying again", reload.model, KameoSkinState(base));
        SetFileInUse(ctx, base, reload.model_type, reload.model.substr(9));
        reload.stage = 3;
        reload.ticks = 0;
      } else {
        REXLOG_WARN("[kameo] costumes: {} didn't go back on ({}); pick it again on the Bonus page", reload.model, KameoSkinState(base));
        it = g_reloads.erase(it);
        continue;
      }
    }
    ++it;
  }
}

}  // namespace kameo::engine
