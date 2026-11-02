#include "core/localization.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <unordered_map>

#ifdef _WIN32
#include <windows.h>
#endif

namespace kameo::l10n {

namespace {

constexpr const char* kCodes[kLanguageCount] = {"", "", "ja", "de", "fr", "es", "it", "ko", "zh-Hant"};

std::atomic<int> g_language{kEnglish};

struct Table {
  bool loaded = false;
  std::unordered_map<std::string, std::string> utf8;
  // The UTF-16 answers handed out, by English, so their references stay valid.
  std::unordered_map<std::u16string, std::u16string> utf16;
};

std::mutex g_mutex;
std::array<Table, kLanguageCount> g_tables;

std::string ToUtf8(std::u16string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    uint32_t c = text[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] < 0xE000) {
      c = 0x10000 + ((c - 0xD800) << 10) + (text[i + 1] - 0xDC00);
      ++i;
    }
    if (c < 0x80) {
      out += char(c);
    } else if (c < 0x800) {
      out += char(0xC0 | (c >> 6));
      out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += char(0xE0 | (c >> 12));
      out += char(0x80 | ((c >> 6) & 0x3F));
      out += char(0x80 | (c & 0x3F));
    } else {
      out += char(0xF0 | (c >> 18));
      out += char(0x80 | ((c >> 12) & 0x3F));
      out += char(0x80 | ((c >> 6) & 0x3F));
      out += char(0x80 | (c & 0x3F));
    }
  }
  return out;
}

std::u16string ToUtf16(std::string_view text) {
  std::u16string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size();) {
    const unsigned char lead = static_cast<unsigned char>(text[i]);
    uint32_t c = lead;
    size_t extra = lead >= 0xF0 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC0 ? 1 : 0;
    if (extra) c = lead & (0x3F >> extra);
    // A sequence cut short by the end of the text keeps what there is of it.
    extra = std::min(extra, text.size() - i - 1);
    for (size_t k = 1; k <= extra; ++k) c = (c << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
    i += 1 + extra;
    if (c >= 0x10000) {
      c -= 0x10000;
      out += char16_t(0xD800 + (c >> 10));
      out += char16_t(0xDC00 + (c & 0x3FF));
    } else {
      out += char16_t(c);
    }
  }
  return out;
}

// \n in a lang file line is a line break, and \\ a backslash.
std::string Unescape(const std::string& line) {
  std::string out;
  for (size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '\\' && i + 1 < line.size() && (line[i + 1] == 'n' || line[i + 1] == '\\')) {
      out += line[i + 1] == 'n' ? '\n' : '\\';
      ++i;
    } else {
      out += line[i];
    }
  }
  return out;
}

void Load(int language, Table& table) {
  table.loaded = true;
  const char* code = LanguageCode(language);
  if (!*code) return;
  std::ifstream in(ProgramFolder() / "lang" / (std::string(code) + ".txt"), std::ios::binary);
  std::string line, english;
  bool have_english = false;
  bool first = true;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    // A byte order mark some editors add.
    if (first && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
    first = false;
    if (line.empty()) {
      have_english = false;
      continue;
    }
    if (line[0] == '#' && !have_english) continue;
    if (!have_english) {
      english = Unescape(line);
      have_english = true;
    } else {
      // An entry not yet translated is left with its English, or empty.
      const std::string translation = Unescape(line);
      if (!translation.empty()) table.utf8[english] = translation;
      have_english = false;
    }
  }
}

Table& CurrentTable() {
  Table& table = g_tables[size_t(CurrentLanguage())];
  if (!table.loaded) Load(CurrentLanguage(), table);
  return table;
}

template <typename String, typename View>
String Format(String text, std::initializer_list<View> args) {
  size_t index = 0;
  for (const View& arg : args) {
    String marker;
    marker += typename String::value_type('{');
    marker += typename String::value_type('0' + index);
    marker += typename String::value_type('}');
    for (size_t at = text.find(marker); at != String::npos; at = text.find(marker, at + arg.size())) text.replace(at, marker.size(), arg);
    ++index;
  }
  return text;
}

}  // namespace

void SetLanguage(int language) { g_language.store(language > 0 && language < kLanguageCount ? language : kEnglish, std::memory_order_relaxed); }

int CurrentLanguage() { return g_language.load(std::memory_order_relaxed); }

const char* LanguageCode(int language) { return language > 0 && language < kLanguageCount ? kCodes[language] : ""; }

const std::u16string& Tr(std::u16string_view english) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Table& table = CurrentTable();
  const std::u16string key(english);
  if (const auto found = table.utf16.find(key); found != table.utf16.end()) return found->second;
  const auto translated = table.utf8.find(ToUtf8(english));
  return table.utf16.emplace(key, translated != table.utf8.end() ? ToUtf16(translated->second) : key).first->second;
}

const char* Tr(const char* english) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Table& table = CurrentTable();
  const auto found = table.utf8.find(english);
  return found != table.utf8.end() ? found->second.c_str() : english;
}

std::u16string TrFormat(std::u16string_view english, std::initializer_list<std::u16string_view> args) { return Format(std::u16string(Tr(english)), args); }

std::string TrFormat(const char* english, std::initializer_list<std::string_view> args) { return Format(std::string(Tr(english)), args); }

int LanguageFromSettings(const std::filesystem::path& settings_file) {
  std::ifstream in(settings_file);
  std::string line;
  while (std::getline(in, line)) {
    const size_t key = line.find("kameo_language");
    if (key == std::string::npos || line.find_first_not_of(" \t") != key) continue;
    const size_t equals = line.find('=', key);
    if (equals == std::string::npos) continue;
    const int language = std::atoi(line.c_str() + equals + 1);
    return language > 0 && language < kLanguageCount ? language : kEnglish;
  }
  return kEnglish;
}

std::filesystem::path ProgramFolder() {
#ifdef _WIN32
  wchar_t path[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, path, MAX_PATH);
  return std::filesystem::path(path).parent_path();
#else
  std::error_code error;
  return std::filesystem::read_symlink("/proc/self/exe", error).parent_path();
#endif
}

}  // namespace kameo::l10n
