// The Sentry settings SentryWork has no option for. CMakeLists.txt renames its sentry_init call to this function, the only way to reach the options it builds.

#include <sentry.h>

#ifdef _WIN32
#include <rex/filesystem.h>
#endif

extern "C" int kameo_sentry_init(sentry_options_t* options) {
  // Sending a report is the player's choice, so nothing goes out before they make it: no record of each session.
  sentry_options_set_auto_session_tracking(options, 0);
  // Stacks are all Sentry needs to walk a crash, and the less memory a dump holds, the less of a player's sign-in it can catch.
  sentry_options_set_minidump_mode(options, SENTRY_MINIDUMP_MODE_STACK_ONLY);
#ifdef _WIN32
  // The helper SentryWork names sentry-crash.exe ships as KameoBugReporter.exe (CMakeLists.txt).
  sentry_options_set_handler_pathw(options, (rex::filesystem::GetExecutableFolder() / L"KameoBugReporter.exe").c_str());
#endif
  return sentry_init(options);
}
