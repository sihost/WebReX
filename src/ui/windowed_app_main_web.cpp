/**
 * @file        ui/windowed_app_main_web.cpp
 * @brief       Emscripten entry point (compiled into the consumer executable).
 *
 * Built with -sPROXY_TO_PTHREAD: main() runs on a pthread, so the UI loop below
 * may block. Mirrors windowed_app_main_posix.cpp without GTK.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <spdlog/common.h>

#include <rex/crash_report.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_web.h>
#include <rex/web_page.h>

extern "C" int main(int argc, char** argv) {
  auto remaining = rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();

  int result;

  {
    rex::ui::WebWindowedAppContext app_context;

    std::unique_ptr<rex::ui::WindowedApp> app = rex::ui::GetWindowedAppCreator()(app_context);

    // Match remaining positional args to app's expected options
    const auto& option_names = app->GetPositionalOptions();
    std::map<std::string, std::string> parsed;
    size_t count = std::min(remaining.size(), option_names.size());
    for (size_t i = 0; i < count; ++i) {
      parsed[option_names[i]] = remaining[i];
    }
    app->SetParsedArguments(std::move(parsed));

    // No writable executable folder on the web: log to the console here. A file
    // log is opt-in with --log_file=<path under OPFS>, e.g. /opfs/user/halo3.log:
    // RexApp::OnInitialize mounts OPFS (OnConfigurePaths) and then re-runs
    // InitLogging with the cvar, which adds the file sink (core/logging.cpp).
    rex::InitLogging(nullptr);
    // Crash reports to the page (Module.rexOnFatal, src/ui/rex_web_pre.js) and the page
    // lifecycle state (hidden / frozen) for any thread.
    rex::crash::InstallCrashReporting();
    rex::web::InstallPageLifecycle();

    if (app->OnInitialize()) {
      app_context.RunMainLoop();
      result = EXIT_SUCCESS;
    } else {
      result = EXIT_FAILURE;
    }

    app->InvokeOnDestroy();
  }

  rex::ShutdownLogging();

  return result;
}
