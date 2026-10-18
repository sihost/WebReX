/**
 * @file        graphics/webgpu/webgpu_gpu_loss.cpp
 * @brief       WebGPU device loss as a page event instead of a FatalError (graceful part).
 *
 * Browsers lose WebGPU devices on GPU process crashes (chrome://gpucrash), driver updates
 * and TDRs. GraphicsSystem::OnHostGpuLossFromAnyThread (from the presenter's paint once
 * the device reports lost) calls HandleHostGpuLoss once. Here, on a helper thread (the
 * caller may be the UI or the device thread, which the steps below must not block):
 *   1. every open file descriptor is fsync'ed and stdio flushed, so saves the guest wrote
 *      to OPFS are durable before the user reloads;
 *   2. the page gets Module.rexOnGpuLost(reason, message, info) and a 'rexgpulost'
 *      CustomEvent on window (detail {reason, message, filesSynced});
 *   3. the command processor is paused (its thread parks), so nothing touches the dead
 *      device any more. The guest runs on until it waits for the GPU.
 * Test: Module._rex_debug_lose_gpu_device() (destroys the device), or chrome://gpucrash.
 * Recreating the device and resuming play is not implemented (gap audit item 11).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/webgpu_graphics_system.h>

#include <unistd.h>

#include <cstdio>
#include <string>
#include <thread>

#include <emscripten/em_js.h>
#include <emscripten/emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/webgpu/webgpu_device.h>
#include <rex/logging.h>

namespace {

// Browser main thread. Strings as double pointers (no wasm64 BigInt question).
EM_JS(void, rexwgpu_notify_page_gpu_lost,
      (double reason_ptr, double message_ptr, int files_synced, int is_responsible), {
  var reason = UTF8ToString(reason_ptr);
  var message = UTF8ToString(message_ptr);
  var info = {reason : reason, message : message, filesSynced : files_synced,
              responsible : !!is_responsible};
  console.error('[rex] WebGPU device lost (' + reason + '): ' + message +
                '. The game is stopped; reload the page to continue.');
  Module['rexGpuLost'] = info;
  var cb = Module['rexOnGpuLost'];
  if (typeof cb == 'function') {
    try {
      cb(reason, message, info);
    } catch (e) {
      console.error(e);
    }
  }
  try {
    if (typeof dispatchEvent == 'function' && typeof CustomEvent == 'function') {
      dispatchEvent(new CustomEvent('rexgpulost', {detail : info}));
    }
  } catch (e) {
  }
});
EM_JS_DEPS(rex_webgpu_gpu_loss, "$UTF8ToString");

const char* LostReasonName(uint32_t reason) {
  switch (reason) {
    case WGPUDeviceLostReason_Unknown:
      return "unknown";
    case WGPUDeviceLostReason_Destroyed:
      return "destroyed";
    case WGPUDeviceLostReason_CallbackCancelled:
      return "callback-cancelled";
    case WGPUDeviceLostReason_FailedCreation:
      return "failed-creation";
    default:
      return "unknown";
  }
}

struct PageNotice {
  std::string reason;
  std::string message;
  int files_synced;
  int is_responsible;
};

void NotifyPage(PageNotice* notice) {
  auto run = [](void* arg) {
    auto* n = static_cast<PageNotice*>(arg);
    rexwgpu_notify_page_gpu_lost(double(uintptr_t(n->reason.c_str())),
                                 double(uintptr_t(n->message.c_str())), n->files_synced,
                                 n->is_responsible);
    delete n;
  };
  if (emscripten_is_main_runtime_thread()) {
    run(notice);
    return;
  }
  if (!emscripten_proxy_async(emscripten_proxy_get_system_queue(),
                              emscripten_main_runtime_thread_id(), run, notice)) {
    delete notice;
  }
}

}  // namespace

namespace rex::graphics::webgpu {

bool WebGPUGraphicsSystem::HandleHostGpuLoss(bool is_responsible) {
  auto* notice = new PageNotice{"unknown", "", 0, is_responsible ? 1 : 0};
  if (device_) {
    notice->reason = LostReasonName(device_->lost_reason());
    notice->message = device_->lost_message();
  }
  REXGPU_ERROR(
      "WebGPU device lost ({}: {}): syncing files, telling the page (Module.rexOnGpuLost) and "
      "stopping the GPU command processor instead of a fatal error; reload the page to continue",
      notice->reason, notice->message);
  std::thread([this, notice]() {
    std::fflush(nullptr);
    int synced = 0;
    for (int fd = 3; fd < 1024; ++fd) {
      if (::fsync(fd) == 0) {
        ++synced;
      }
    }
    notice->files_synced = synced;
    REXGPU_INFO("WebGPU device loss: {} open files synced", synced);
    NotifyPage(notice);
    if (command_processor_ && !command_processor_->is_paused()) {
      command_processor_->Pause();
      REXGPU_INFO("WebGPU device loss: command processor stopped");
    }
  }).detach();
  return true;
}

}  // namespace rex::graphics::webgpu
