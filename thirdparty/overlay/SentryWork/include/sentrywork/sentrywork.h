#pragma once

// SentryWork - Sentry crash reports for ReXGlue recomps.
//
// A crash (or a fatal assert, abort or std::terminate) is sent to the Sentry
// project whose DSN the game passed to sentrywork_attach() in CMake, with the
// game's log file, the last warnings and errors as breadcrumbs, and the title
// it was running. Players can turn reports off with crash_reports = false in
// the game's .toml (the setting crash.reports, or its old name crash_reports).
//
//   class MyApp : public rex::app::TitleApp {
//     rex::Result<void> OnConfigure(rex::SettingsRegistry& settings) override {
//       return sentrywork::RegisterSettings(settings);
//     }
//     rex::Result<void> OnInit() override { sentrywork::Init({.app_name = "my_app"}); return rex::Ok(); }
//     rex::Result<void> OnTitleLaunch(rex::kernel::TitleRuntime& title) override {
//       sentrywork::OnRuntimeReady(&title); return rex::Ok();
//     }
//     void OnShutdown() override { sentrywork::Shutdown(); }
//   };

#include <rex/result.h>
#include <rex/settings_registry.h>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace rex::kernel {
class TitleRuntime;
}

namespace sentrywork {

struct Options {
  // The name the app gives rex::app::App, used when the app.name setting is empty.
  std::string app_name;
  // Where reports wait until they are sent (a crash's is sent by the
  // sentry-crash helper, or on the next run). Default: "sentry" next to the
  // executable; give a writable folder when that one is not (an AppImage).
  std::filesystem::path database_dir;
  // The log attached to reports. Default: the newest <name>_NNN.log in app.log_root
  // (name: log.file_name, an absolute log.file_name as it is, or the app name).
  std::filesystem::path log_file;
  // Shown on every report, e.g. {"game", "Banjo-Kazooie: Nuts & Bolts"}.
  std::vector<std::pair<std::string, std::string>> tags;
};

// Registers crash.reports and crash.report_log_level (and their old names
// crash_reports and crash_report_log_level). Call from App::OnConfigure, before
// the config file is read; Shutdown removes the settings again.
rex::Result<void> RegisterSettings(rex::SettingsRegistry& settings);

// Starts crash reporting. Call from App::OnInit: every settings source and the
// log are loaded then, and the SDK's crash reporter is installed, so Sentry's
// exception filter ends up above it.
void Init(const Options& options);

// Adds the title to reports and puts Sentry's exception filter back on top
// once the title is loaded. Call from TitleApp::OnTitleLaunch.
void OnRuntimeReady(rex::kernel::TitleRuntime* runtime);

// Sends what is still queued and stops. Call from App::OnShutdown.
void Shutdown();

// Whether reports are being sent: a DSN was built in and the player allows it.
bool Enabled();

// Reports a problem the game survived, as a Sentry issue with a stack trace.
void CaptureMessage(const std::string& message);

} // namespace sentrywork
