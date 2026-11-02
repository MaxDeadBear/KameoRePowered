// Our own text in the game's languages: the game, launcher, crash dialog and shader editor show what they add in the
// language the game is set to. Translations are in lang/<code>.txt beside the programs (res/lang in the source), one
// file per language, keyed by the English; text with no translation shows in English.
//
// A lang file is pairs of lines, the English then its translation, each pair followed by a blank line; lines starting
// with # are comments, and \n in either line is a line break. tools/lang/extract.py lists every string the code asks
// for, and adds any new ones to each file for translating.
#pragma once

#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>

// Marks text kept in a table to be translated where it's shown, with Tr(entry.label): it is the text as written, and
// tools/lang/extract.py lists it.
#define TR_NOOP(text) text

namespace kameo::l10n {

// The game's numbering, as kameo_language and XGetLanguage use it.
enum Language : int { kEnglish = 1, kJapanese, kGerman, kFrench, kSpanish, kItalian, kKorean, kChinese, kLanguageCount };

// Sets the language text is looked up in; anything outside the list is English. Readable from any thread.
void SetLanguage(int language);
int CurrentLanguage();
// The file name a language's translations live under (ja, de, ...), or empty for English.
const char* LanguageCode(int language);

// The text in the current language, or the English given. The results stay valid for the program's life.
const std::u16string& Tr(std::u16string_view english);
const char* Tr(const char* english);
// The same with {0}, {1}... in the text replaced by the arguments, so a translation can place them where its language
// needs them.
std::u16string TrFormat(std::u16string_view english, std::initializer_list<std::u16string_view> args);
std::string TrFormat(const char* english, std::initializer_list<std::string_view> args);

// The language a kameo_settings.toml sets, for the programs beside the game: kameo_language, with 0 or none meaning
// English, the game's own default.
int LanguageFromSettings(const std::filesystem::path& settings_file);
// The folder the running program is in, where lang/ and kameo_settings.toml are.
std::filesystem::path ProgramFolder();

}  // namespace kameo::l10n
