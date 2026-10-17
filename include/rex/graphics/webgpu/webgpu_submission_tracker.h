/**
 * @file        rex/graphics/webgpu/webgpu_submission_tracker.h
 * @brief       Submission counters shared by the command processor and the WebGPU device thread.
 *
 * WebGPU has no fences the CP thread could wait on (handles live on the device
 * thread, and wgpuInstanceWaitAny needs Asyncify). Every submission instead
 * gets a monotonic id on the CP thread, and the device thread advances two
 * counters:
 *  - replayed:  the stream has been encoded and wgpuQueueSubmit'ed. The CPU-side
 *               stream memory (upload and constant arenas) is free again.
 *  - completed: queue.onSubmittedWorkDone() resolved for that submission, i.e.
 *               the GPU has finished it. Resolves in the device worker's event
 *               loop (AllowSpontaneous callback), in submission order.
 * The analog of ui/vulkan/vulkan_submission_tracker.cpp, with the fence replaced
 * by the promise.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include <webgpu/webgpu.h>

namespace rex::graphics::webgpu {

class WebGPUSubmissionTracker {
 public:
  WebGPUSubmissionTracker() = default;
  WebGPUSubmissionTracker(const WebGPUSubmissionTracker&) = delete;
  WebGPUSubmissionTracker& operator=(const WebGPUSubmissionTracker&) = delete;

  // ---- Submitting thread (the CP). ----

  // Allocates the id of a new submission (1, 2, 3, ...).
  uint64_t AllocateSubmission() {
    return submitted_.fetch_add(1, std::memory_order_acq_rel) + 1;
  }

  // ---- Any thread. ----

  // Last id handed out by AllocateSubmission.
  uint64_t submitted() const { return submitted_.load(std::memory_order_acquire); }
  // Last id whose stream was encoded and queue-submitted on the device thread.
  uint64_t replayed() const { return replayed_.load(std::memory_order_acquire); }
  // Last id the GPU has finished executing (monotonic).
  uint64_t completed() const { return completed_.load(std::memory_order_acquire); }
  // After MarkLost, every wait returns at once and completed() reports all
  // submissions as done (there is nothing left to wait for).
  bool is_lost() const { return lost_.load(std::memory_order_acquire); }

  // Block until replayed() / completed() >= id, or the timeout passes, or the
  // tracker is marked lost. Must not be called on the device thread (the
  // counters only advance when it runs). Return whether the id was reached.
  bool AwaitReplayed(uint64_t id,
                     std::chrono::milliseconds timeout = std::chrono::milliseconds(10000));
  bool AwaitCompleted(uint64_t id,
                      std::chrono::milliseconds timeout = std::chrono::milliseconds(10000));

  // ---- Device thread. ----

  void MarkReplayed(uint64_t id);
  // Registers queue.onSubmittedWorkDone for the work submitted so far, which
  // is submission `id`; MarkCompleted(id) runs when it resolves.
  void TrackCompletion(WGPUQueue queue, uint64_t id);
  void MarkCompleted(uint64_t id);
  // Device lost or destroyed: release all waiters.
  void MarkLost();

 private:
  static void OnWorkDone(WGPUQueueWorkDoneStatus status, WGPUStringView message, void* userdata1,
                         void* userdata2);
  bool Await(const std::atomic<uint64_t>& counter, uint64_t id,
             std::chrono::milliseconds timeout);
  void Advance(std::atomic<uint64_t>& counter, uint64_t id);

  std::atomic<uint64_t> submitted_{0};
  std::atomic<uint64_t> replayed_{0};
  std::atomic<uint64_t> completed_{0};
  std::atomic<bool> lost_{false};

  std::mutex mutex_;
  std::condition_variable cond_;
};

}  // namespace rex::graphics::webgpu
