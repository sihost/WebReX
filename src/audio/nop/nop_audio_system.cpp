/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/audio/audio_driver.h>
#include <rex/audio/flags.h>
#include <rex/audio/nop/nop_audio_system.h>
#include <rex/thread.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace rex::audio::nop {

namespace {

// A silent output device. Guest XAudio needs a render driver client
// (XAudioRegisterRenderDriverClient fails without one, and XAudio then runs
// with a null effect manager), and its render callback is paced by the device
// handing frames back. This driver discards the samples but consumes the
// queued frames at the real playback rate (256 samples at 48 kHz per frame),
// like the SDL driver's callback does, so the guest audio thread neither
// stalls nor spins.
class NopAudioDriver final : public AudioDriver {
 public:
  NopAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
      : AudioDriver(memory), semaphore_(semaphore), thread_([this]() { PlaybackThread(); }) {}

  ~NopAudioDriver() override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      running_ = false;
    }
    cv_.notify_all();
    thread_.join();
  }

  void SubmitFrame(uint32_t /*frame_ptr*/) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++queued_frames_;
    }
    cv_.notify_all();
  }

 private:
  static constexpr std::chrono::nanoseconds kFrameDuration{256ll * 1000000000ll / 48000};

  void PlaybackThread() {
    auto next = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(mutex_);
    while (running_) {
      if (!queued_frames_) {
        // Underrun: the device idles; playback restarts on the next frame.
        cv_.wait(lock, [this]() { return !running_ || queued_frames_; });
        next = std::max(next, std::chrono::steady_clock::now());
        continue;
      }
      next += kFrameDuration;
      if (cv_.wait_until(lock, next, [this]() { return !running_; })) {
        break;
      }
      --queued_frames_;
      lock.unlock();
      semaphore_->Release(1, nullptr);
      lock.lock();
    }
  }

  rex::thread::Semaphore* semaphore_;
  std::mutex mutex_;
  std::condition_variable cv_;
  uint32_t queued_frames_ = 0;
  bool running_ = true;
  std::thread thread_;  // last: starts after the members it uses
};

}  // namespace

std::unique_ptr<AudioSystem> NopAudioSystem::Create(
    runtime::FunctionDispatcher* function_dispatcher) {
  return std::make_unique<NopAudioSystem>(function_dispatcher);
}

NopAudioSystem::NopAudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : AudioSystem(function_dispatcher) {}

NopAudioSystem::~NopAudioSystem() = default;

X_STATUS NopAudioSystem::CreateDriver(size_t /*index*/, rex::thread::Semaphore* semaphore,
                                      AudioDriver** out_driver) {
  *out_driver = new NopAudioDriver(memory(), semaphore);
  return X_STATUS_SUCCESS;
}

void NopAudioSystem::DestroyDriver(AudioDriver* driver) {
  delete driver;
}

}  // namespace rex::audio::nop
