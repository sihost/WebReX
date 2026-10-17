/**
 * @file        rex/ui/windowed_app_context_web.h
 * @brief       WindowedAppContext for the Emscripten (wasm64) build.
 *
 * With -sPROXY_TO_PTHREAD, main() runs on a pthread, so the "UI thread" here is
 * that pthread and may block. The loop is a plain mutex/condvar pump of the
 * pending-function queue. Browser events reach it as posted functions: canvas
 * resize and paint requests from the web window (src/ui/window_web.cpp) and
 * input through rex::ui::web::PostInputEvent (rex/ui/window_web.h).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <condition_variable>
#include <mutex>

#include <rex/ui/windowed_app_context.h>

namespace rex {
namespace ui {

class WebWindowedAppContext final : public WindowedAppContext {
 public:
  WebWindowedAppContext() = default;
  ~WebWindowedAppContext();

  void NotifyUILoopOfPendingFunctions() override;

  void PlatformQuitFromUIThread() override;

  // Runs until QuitFromUIThread. Must be called on the thread that created the
  // context (main()'s pthread).
  void RunMainLoop();

 private:
  std::mutex loop_mutex_;
  std::condition_variable loop_cv_;
  bool pending_notified_ = false;
  bool quit_requested_ = false;
};

}  // namespace ui
}  // namespace rex
