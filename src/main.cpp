#include "app/app.h"

// The steps REX_TITLE_APP runs, with one of ours before Shutdown, which unloads the title before any hook of the app runs.
static int KameoMain(KameoApp& app, const rex::CLIArgList& args) {
  if (app.Init(args) && app.LaunchTitle()) app.Run();
  app.ReleaseTitleListeners();
  return app.Shutdown();
}

REX_TITLE_APP_MAIN(KameoApp, KameoMain)
