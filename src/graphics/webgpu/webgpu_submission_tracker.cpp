/**
 * @file        graphics/webgpu/webgpu_submission_tracker.cpp
 * @brief       See rex/graphics/webgpu/webgpu_submission_tracker.h.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/webgpu/webgpu_submission_tracker.h>

#include <rex/graphics/webgpu/webgpu_log.h>

namespace rex::graphics::webgpu {

bool WebGPUSubmissionTracker::Await(const std::atomic<uint64_t>& counter, uint64_t id,
                                    std::chrono::milliseconds timeout) {
  if (counter.load(std::memory_order_acquire) >= id || is_lost()) {
    return counter.load(std::memory_order_acquire) >= id;
  }
  std::unique_lock<std::mutex> lock(mutex_);
  cond_.wait_for(lock, timeout, [&]() {
    return counter.load(std::memory_order_acquire) >= id || is_lost();
  });
  return counter.load(std::memory_order_acquire) >= id;
}

bool WebGPUSubmissionTracker::AwaitReplayed(uint64_t id, std::chrono::milliseconds timeout) {
  return Await(replayed_, id, timeout);
}

bool WebGPUSubmissionTracker::AwaitCompleted(uint64_t id, std::chrono::milliseconds timeout) {
  return Await(completed_, id, timeout);
}

void WebGPUSubmissionTracker::Advance(std::atomic<uint64_t>& counter, uint64_t id) {
  uint64_t current = counter.load(std::memory_order_relaxed);
  while (current < id &&
         !counter.compare_exchange_weak(current, id, std::memory_order_acq_rel,
                                        std::memory_order_relaxed)) {
  }
  // Waiters check the counter under the mutex, so taking it here (briefly, on
  // the device worker) closes the check-then-wait race.
  { std::lock_guard<std::mutex> lock(mutex_); }
  cond_.notify_all();
}

void WebGPUSubmissionTracker::MarkReplayed(uint64_t id) {
  Advance(replayed_, id);
}

void WebGPUSubmissionTracker::MarkCompleted(uint64_t id) {
  Advance(completed_, id);
}

void WebGPUSubmissionTracker::MarkLost() {
  lost_.store(true, std::memory_order_release);
  Advance(replayed_, UINT64_MAX);
  Advance(completed_, UINT64_MAX);
}

void WebGPUSubmissionTracker::TrackCompletion(WGPUQueue queue, uint64_t id) {
  if (!queue || is_lost()) {
    MarkCompleted(id);
    return;
  }
  WGPUQueueWorkDoneCallbackInfo callback_info = WGPU_QUEUE_WORK_DONE_CALLBACK_INFO_INIT;
  // Resolves from the device worker's event loop after the task that submitted
  // returns to it.
  callback_info.mode = WGPUCallbackMode_AllowSpontaneous;
  callback_info.callback = OnWorkDone;
  callback_info.userdata1 = this;
  callback_info.userdata2 = reinterpret_cast<void*>(uintptr_t(id));
  wgpuQueueOnSubmittedWorkDone(queue, callback_info);
}

void WebGPUSubmissionTracker::OnWorkDone(WGPUQueueWorkDoneStatus status, WGPUStringView message,
                                         void* userdata1, void* userdata2) {
  auto* self = static_cast<WebGPUSubmissionTracker*>(userdata1);
  uint64_t id = uint64_t(reinterpret_cast<uintptr_t>(userdata2));
  if (status != WGPUQueueWorkDoneStatus_Success) {
    // Error (device lost) or cancelled (device destroyed): nothing will run
    // anymore, so don't let the CP wait for it.
    REXWGPU_WARN("WebGPU: onSubmittedWorkDone for submission {} ended with status {}: {}", id,
                 uint32_t(status), ToStringView(message));
    self->MarkLost();
    return;
  }
  self->MarkCompleted(id);
}

}  // namespace rex::graphics::webgpu
