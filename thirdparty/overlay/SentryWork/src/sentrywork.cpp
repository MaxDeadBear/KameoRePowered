#include "sentrywork/sentrywork.h"

#include "fault_fallback.h"

#include <rex/kernel/title_runtime.h>
#include <rex/logging/log_category.h>
#include <rex/logging/logger.h>
#include <rex/path_settings.h>
#include <rex/platform/file.h>
#include <rex/platform/thread.h>
#include <rex/setting.h>
#include <rex/version.h>

#include <sentry.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <system_error>

namespace sentrywork {

namespace {

// Registered under the scope "crash"; the flat names crash_reports and
// crash_report_log_level stay as aliases for existing config files.
class CrashSettings : public rex::SettingOwner {
public:
  CrashSettings() : SettingOwner("crash") {}

  REX_SETTING(bool, reports, true, "Send crash reports to the game's developers (through Sentry)");
  REX_SETTING(std::string, report_log_level, "warn",
              "The log lines sent with a crash report: error, warn, info or off",
              rex::Allowed("trace", "dev", "debug", "info", "warn", "error", "critical", "off"));
};

std::unique_ptr<CrashSettings> g_settings;

#ifdef _WIN32
constexpr const char *kHelperName = "sentry-crash.exe";
#else
constexpr const char *kHelperName = "sentry-crash";
#endif

bool g_enabled = false;

// Sentry's path options take wide strings on Windows, so folders with
// non-ASCII names (a player's user name) still work.
#ifdef _WIN32
void SetDatabasePath(sentry_options_t *o, const std::filesystem::path &p) {
  sentry_options_set_database_pathw(o, p.c_str());
}
void SetHandlerPath(sentry_options_t *o, const std::filesystem::path &p) {
  sentry_options_set_handler_pathw(o, p.c_str());
}
void AddAttachment(sentry_options_t *o, const std::filesystem::path &p) {
  sentry_options_add_attachmentw(o, p.c_str());
}
#else
void SetDatabasePath(sentry_options_t *o, const std::filesystem::path &p) {
  sentry_options_set_database_path(o, p.c_str());
}
void SetHandlerPath(sentry_options_t *o, const std::filesystem::path &p) {
  sentry_options_set_handler_path(o, p.c_str());
}
void AddAttachment(sentry_options_t *o, const std::filesystem::path &p) {
  sentry_options_add_attachment(o, p.c_str());
}
#endif

#ifndef _WIN32
// Reports go out over libcurl, and an AppImage carries the libcurl of the
// distribution it was built on, which looks for root certificates where that
// distribution keeps them. Point it at this system's (or SSL_CERT_FILE).
std::string FindCaCertificates() {
  if (const char *file = std::getenv("SSL_CERT_FILE"); file && *file)
    return file;
  static constexpr const char *kBundles[] = {
      "/etc/ssl/certs/ca-certificates.crt",                // Debian, Ubuntu, Arch, Gentoo
      "/etc/pki/tls/certs/ca-bundle.crt",                  // Fedora, RHEL
      "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // CentOS, RHEL 7
      "/etc/ssl/ca-bundle.pem",                            // openSUSE
      "/etc/ssl/cert.pem",                                 // Alpine, macOS
  };
  std::error_code ec;
  for (const char *bundle : kBundles) {
    if (std::filesystem::exists(bundle, ec))
      return bundle;
  }
  return {};
}
#endif

// The SDK names its log <app.log_root>/<log.file_name or app.name>_NNN.log,
// counting up each run, unless log.file_name is an absolute path; the newest
// is this run's.
std::filesystem::path FindLogFile(const Options &options) {
  if (!options.log_file.empty())
    return options.log_file;
  if (!REX_LOGGER().file.Get())
    return {};
  const std::string file_name = REX_LOGGER().file_name.Get();
  if (!file_name.empty() && std::filesystem::path(file_name).is_absolute())
    return file_name;
  std::string name = file_name;
  if (name.empty()) {
    if (auto app_name = REX_SETTINGS().Find("app.name"))
      name = (*app_name)->GetText();
  }
  if (name.empty())
    name = options.app_name;
  auto log_root = REX_SETTINGS().Find("app.log_root");
  if (name.empty() || !log_root)
    return {};
  const std::filesystem::path folder = rex::detail::ResolveSettingPath((*log_root)->GetText());
  const std::string prefix = name + "_";
  std::filesystem::path newest;
  std::filesystem::file_time_type newest_time;
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(folder, ec)) {
    const std::string entry_name = entry.path().filename().string();
    if (!entry_name.starts_with(prefix) || entry.path().extension() != ".log")
      continue;
    const auto time = entry.last_write_time(ec);
    if (!ec && (newest.empty() || time > newest_time)) {
      newest = entry.path();
      newest_time = time;
    }
  }
  return newest;
}

const char *SentryLevel(rex::log::Level level) {
  switch (level) {
  case rex::log::Level::kCritical:
    return "fatal";
  case rex::log::Level::kError:
    return "error";
  case rex::log::Level::kWarn:
    return "warning";
  case rex::log::Level::kInfo:
    return "info";
  default:
    return "debug";
  }
}

// Log lines become breadcrumbs: the last ones before a crash come with it.
// Each breadcrumb is written to disk for the crash daemon, so only the levels
// crash.report_log_level asks for are passed on.
class BreadcrumbSink final : public rex::log::ILogSink {
public:
  explicit BreadcrumbSink(rex::log::Level min_level) : min_level_(min_level) {}

  void Write(const rex::log::Message &msg) override {
    if (msg.level < min_level_)
      return;
    sentry_value_t crumb = sentry_value_new_breadcrumb("default", nullptr);
    sentry_value_set_by_key(crumb, "message",
                            sentry_value_new_string_n(msg.text.data(), msg.text.size()));
    sentry_value_set_by_key(crumb, "category",
                            sentry_value_new_string_n(msg.category.data(), msg.category.size()));
    sentry_value_set_by_key(crumb, "level", sentry_value_new_string(SentryLevel(msg.level)));
    sentry_add_breadcrumb(crumb);
  }
  void Flush() override {}

private:
  rex::log::Level min_level_;
};

std::unique_ptr<BreadcrumbSink> g_breadcrumbs;
// Shutdown releases the guard before the sink.
rex::ScopeGuard g_breadcrumbs_guard;

// SENTRYWORK_TEST_CRASH=1 crashes the game once it is loaded, the way a game
// bug does (a write through a null pointer), to check that reports arrive.
[[gnu::noinline]] void TestCrash() {
  REXLOG_WARN("SentryWork: crashing on purpose (SENTRYWORK_TEST_CRASH=1)");
  int *volatile null = nullptr;
  *null = 0x5E27;
}

} // namespace

rex::Result<void> RegisterSettings(rex::SettingsRegistry &settings) {
  if (g_settings)
    return rex::Ok();
  auto owner = std::make_unique<CrashSettings>();
  if (auto registered = settings.Register(*owner); !registered)
    return registered;
  settings.AddAlias("crash_reports", "crash.reports");
  settings.AddAlias("crash_report_log_level", "crash.report_log_level");
  g_settings = std::move(owner);
  return rex::Ok();
}

void Init(const Options &options) {
  if (g_enabled)
    return;
  if (SENTRYWORK_DSN[0] == '\0') {
    REXLOG_INFO("SentryWork: built without a DSN, so crash reports are off");
    return;
  }
  if (!(g_settings ? g_settings->reports.Get() : true)) {
    REXLOG_INFO("SentryWork: crash reports are off (crash.reports = false)");
    return;
  }

  const std::filesystem::path exe_dir = rex::platform::GetExecutableFolder();
  sentry_options_t *o = sentry_options_new();
  sentry_options_set_dsn(o, SENTRYWORK_DSN);
  sentry_options_set_release(o, SENTRYWORK_RELEASE);
  sentry_options_set_environment(o, SENTRYWORK_ENVIRONMENT);
  SetDatabasePath(o, options.database_dir.empty() ? exe_dir / "sentry" : options.database_dir);
  SetHandlerPath(o, exe_dir / kHelperName);
  // Sentry walks the stack from the minidump, with the debug files uploaded
  // for the build: the daemon's own walk loses the crashed thread's frames.
  sentry_options_set_crash_reporting_mode(o, SENTRY_CRASH_REPORTING_MODE_MINIDUMP);
#ifndef _WIN32
  if (const std::string certs = FindCaCertificates(); !certs.empty())
    sentry_options_set_ca_certs(o, certs.c_str());
#endif
  const std::filesystem::path log = FindLogFile(options);
  if (!log.empty())
    AddAttachment(o, log);
  if (sentry_init(o) != 0) {
    REXLOG_WARN("SentryWork: Sentry failed to start, so crash reports are off");
    return;
  }
  g_enabled = true;
#ifndef _WIN32
  detail::CaptureSentrySignalHandlers();
#endif

  sentry_set_tag("rexglue", REXGLUE_VERSION_STRING);
  for (const auto &[key, value] : options.tags)
    sentry_set_tag(key.c_str(), value.c_str());

  const auto level = rex::log::ParseLevel(g_settings ? g_settings->report_log_level.Get() : "warn");
  if (level && *level != rex::log::Level::kOff) {
    g_breadcrumbs = std::make_unique<BreadcrumbSink>(*level);
    g_breadcrumbs_guard = REX_LOGGER().AddSink(*g_breadcrumbs);
  }
  REXLOG_INFO("SentryWork: crash reports on ({}, {})", SENTRYWORK_RELEASE, SENTRYWORK_ENVIRONMENT);
}

void OnRuntimeReady(rex::kernel::TitleRuntime *runtime) {
  if (!g_enabled)
    return;
  if (runtime && runtime->GetTitleID()) {
    char title_id[16];
    std::snprintf(title_id, sizeof(title_id), "%08X", runtime->GetTitleID());
    sentry_set_tag("title_id", title_id);
  }
#ifdef _WIN32
  // Runtime setup can replace the unhandled exception filter; take it back.
  // Sentry's ends the process once the crash is written up, so filters it
  // replaced (a game's own minidump writer) no longer run.
  sentry_reinstall_backend();
#else
  detail::InstallFaultFallback();
#endif
  if (const char *test = std::getenv("SENTRYWORK_TEST_CRASH"); test && test[0] == '1')
    TestCrash();
}

void Shutdown() {
  if (g_enabled) {
    g_breadcrumbs_guard.Reset();
    g_breadcrumbs.reset();
    sentry_close();
    g_enabled = false;
  }
  g_settings.reset();
}

bool Enabled() {
  return g_enabled;
}

void CaptureMessage(const std::string &message) {
  if (!g_enabled)
    return;
  sentry_value_t event =
      sentry_value_new_message_event(SENTRY_LEVEL_WARNING, "sentrywork", message.c_str());
  sentry_value_t thread = sentry_value_new_thread(rex::platform::GetCallerSystemThreadID(), nullptr);
  sentry_value_set_by_key(thread, "stacktrace", sentry_value_new_stacktrace(nullptr, 0));
  sentry_event_add_thread(event, thread);
  sentry_capture_event(event);
}

} // namespace sentrywork
