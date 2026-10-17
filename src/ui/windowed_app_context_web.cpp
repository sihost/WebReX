/**
 * @file        ui/windowed_app_context_web.cpp
 * @brief       Emscripten UI loop: pending-function pump on main()'s pthread.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/ui/windowed_app_context_web.h>

namespace rex {
namespace ui {

WebWindowedAppContext::~WebWindowedAppContext() = default;

void WebWindowedAppContext::NotifyUILoopOfPendingFunctions() {
  {
    std::lock_guard<std::mutex> lock(loop_mutex_);
    pending_notified_ = true;
  }
  loop_cv_.notify_one();
}

void WebWindowedAppContext::PlatformQuitFromUIThread() {
  {
    std::lock_guard<std::mutex> lock(loop_mutex_);
    quit_requested_ = true;
  }
  loop_cv_.notify_one();
}

void WebWindowedAppContext::RunMainLoop() {
  if (HasQuitFromUIThread()) {
    return;
  }
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(loop_mutex_);
      loop_cv_.wait(lock, [this] { return pending_notified_ || quit_requested_; });
      if (quit_requested_) {
        break;
      }
      pending_notified_ = false;
    }
    ExecutePendingFunctionsFromUIThread();
  }
  // In case something other than QuitFromUIThread ended the loop.
  QuitFromUIThread();
}

}  // namespace ui
}  // namespace rex
