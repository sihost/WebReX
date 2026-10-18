/**
 * ReXGlue web port (Workstream E): browser audio output.
 *
 * WebAudioOutput is the transport between the SDK audio driver thread and an AudioWorklet:
 *
 *   SubmitGuestFrame (driver thread)            AudioWorklet (audio rendering thread)
 *     6ch x 256 BE float, channel-sequential      reads slots, writes 128-frame quanta,
 *     -> downmix to interleaved LE float     ->   advances READ (CAS) + Atomics.notify
 *     -> ring slot, WRITE++                       (in the wasm SharedArrayBuffer)
 *
 *   pacing thread: waits on READ (futex), hands consumed frames back through on_consumed
 *   (the AudioDriver releases the guest's frame semaphore there, as the SDL callback does).
 *
 * While the worklet is not rendering (no AudioContext yet, context suspended by the
 * autoplay gate, or Web Audio unavailable) the pacing thread consumes the ring itself at
 * 48 kHz ("free-run"), so the guest audio thread is paced exactly like real playback and
 * never stalls. A running context's worklet (also in hidden tabs, where Chrome gives it less
 * CPU) is waited for up to 1.5 s before free-run: discarding its queued frames made
 * background-tab audio choppy and "fast". Frames are 256 samples (5.33 ms); the guest keeps at most
 * audio_maxqframes (default 8 = 42.7 ms) in flight, which bounds the ring latency.
 *
 * Self-contained (std + Emscripten only) so tools/audio_tests can drive it without a guest.
 * Only built for Emscripten.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace rex::audio::web {

struct WebAudioStats {
  uint32_t submitted = 0;        // frames given to SubmitGuestFrame
  uint32_t played = 0;           // frames consumed by the worklet
  uint32_t free_run = 0;         // frames consumed by the pacing thread (worklet not running)
  uint32_t overflows = 0;        // frames dropped because the ring was full
  uint32_t quanta = 0;           // worklet process() calls (128 frames each)
  uint32_t underrun_quanta = 0;  // quanta rendered (partly) silent because the ring was empty
  int32_t state = 0;             // see WebAudioOutput::State
  int32_t context_sample_rate = 0;
  uint32_t output_latency_us = 0;  // AudioContext baseLatency + outputLatency
  uint32_t ring_frames = 0;        // frames currently queued (WRITE - READ)
  float peak = 0.0f;               // peak |sample| of the last frame the worklet played
  uint32_t max_batch = 0;          // most frames handed back to the guest in one wake
};

class WebAudioOutput {
 public:
  enum State : int32_t {
    kStateNone = 0,       // JS side not created yet
    kStateSuspended = 1,  // AudioContext exists but is not running (autoplay gate)
    kStateRunning = 2,
    kStateFailed = 3,  // Web Audio unavailable; free-run only
  };

  static constexpr uint32_t kSampleRate = 48000;
  static constexpr uint32_t kChannelSamples = 256;  // samples per channel per guest frame
  static constexpr uint32_t kSourceChannels = 6;    // XAudio render format (5.1)
  static constexpr uint32_t kSlots = 64;            // >= the audio_maxqframes maximum

  // Control block layout shared with rex_audio_worklet.js (Int32 indices).
  enum Control : uint32_t {
    kCtlWrite = 0,
    kCtlRead = 1,
    kCtlSlots = 2,
    kCtlSlotSamples = 3,
    kCtlChannels = 4,
    kCtlQuanta = 5,
    kCtlUnderrunQuanta = 6,
    kCtlPlayed = 7,
    kCtlFreeRun = 8,
    kCtlState = 9,
    kCtlPeak = 10,  // float32 bits
    kCtlOverflows = 11,
    kCtlMuted = 12,
    kCtlSampleRate = 13,
    kCtlLatencyUs = 14,
    kCtlSubmitted = 15,
    kCtlStop = 16,
    kCtlMaxBatch = 17,
    kCtlPrime = 18,  // worklet: frames it waits for after an underrun (adaptive, 2..6)
    kCtlCount = 32,
  };

  using ConsumedFn = std::function<void(uint32_t frames)>;

  // Output is always the stereo downmix (conversion::sequential_6_BE_to_interleaved_2_LE,
  // the SDL driver's 2-channel path): the default Web Audio destination is stereo.
  static constexpr uint32_t kOutputChannels = 2;

  explicit WebAudioOutput(ConsumedFn on_consumed);
  ~WebAudioOutput();

  WebAudioOutput(const WebAudioOutput&) = delete;
  WebAudioOutput& operator=(const WebAudioOutput&) = delete;

  // Allocates the ring, starts the pacing thread and asks the browser main thread to create
  // (or reuse) the AudioContext and an AudioWorkletNode for this output. Never blocks on JS.
  bool Start();
  void Stop();

  // frame: kSourceChannels x kChannelSamples big-endian floats, channel-sequential (the
  // layout XAudio hands to AudioDriver::SubmitFrame).
  void SubmitGuestFrame(const float* frame);

  void set_muted(bool muted);
  WebAudioStats stats() const;

 private:
  void PacingThread();
  void Consumed(uint32_t frames);

  ConsumedFn on_consumed_;
  std::atomic<int32_t>* ctl_ = nullptr;  // kCtlCount entries
  float* ring_ = nullptr;                // kSlots * kChannelSamples * kOutputChannels
  std::atomic<bool> running_{false};
  std::thread pacing_thread_;
  std::mutex submit_mutex_;
};

}  // namespace rex::audio::web
