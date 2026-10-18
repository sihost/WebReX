/**
 * ReXGlue web port (Workstream E): browser audio output.
 *
 * WebAudioOutput is the transport between the SDK audio driver thread and an AudioWorklet:
 *
 *   SubmitGuestFrame (driver thread)            AudioWorklet (audio rendering thread)
 *     6ch x 256 BE float, channel-sequential      reads slots, writes 128-frame quanta,
 *     -> stereo downmix or 5.1, interleaved  ->   advances READ (CAS) + Atomics.notify
 *        LE float -> ring slot, WRITE++           (in the wasm SharedArrayBuffer)
 *
 *   pacing thread: waits on READ (futex), hands consumed frames back through on_consumed
 *   (the AudioDriver releases the guest's frame semaphore there, as the SDL callback does).
 *
 * Latency: the guest keeps audio_maxqframes frames (default 8 = 42.7 ms) in flight, so
 * handing every played frame straight back kept the ring full: 42.7 ms of queue on top of
 * the AudioContext's own latency (~50 ms measured on Chrome/Windows). The pacing thread
 * instead withholds frames it was handed back so that ring + frames the guest holds stay at
 * an adaptive target: Options::min_frames at first (3 = 16 ms), one more after each real
 * underrun of the worklet (closer ones and those of the first second count once), one less
 * after 10 s without one; an underrun soon after such a step doubles that wait (up to 320 s).
 * Never below the worklet's post-underrun prime level, never above the guest's budget
 * (where it behaves as before: the ring stays full). Free-run hands every frame back.
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
  uint32_t target_frames = 0;      // current latency target (ring + frames held by the guest)
  uint32_t withheld = 0;           // played frames not handed back yet (budget - target)
  uint32_t underruns = 0;          // worklet underruns while playing (each raises the target)
  uint32_t output_channels = 0;    // 2 (stereo downmix) or 6 (5.1); 0 = not decided yet
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
  // Ring slots are sized for 5.1; each slot records the layout it was written with.
  static constexpr uint32_t kMaxOutputChannels = 6;

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
    kCtlTarget = 19,          // pacing thread: current latency target, frames
    kCtlUnderrunEvents = 20,  // worklet: ring ran empty while playing
    kCtlOutChannels = 21,     // JS: channels of the AudioWorkletNode (2 or 6), 0 = none yet
    kCtlWithheld = 22,        // pacing thread: frames played but not handed back
    kCtlCount = 32,
  };

  using ConsumedFn = std::function<void(uint32_t frames)>;

  struct Options {
    // Latency target floor in frames (ring + frames the guest holds); 0 = no latency control
    // (every played frame is handed back at once, the ring stays at the guest's budget).
    uint32_t min_frames = 3;
    // 0 = 5.1 when the AudioContext destination has >= 6 channels, else the stereo downmix;
    // 2 = always the stereo downmix (conversion::sequential_6_BE_to_interleaved_2_LE, the SDL
    // driver's 2-channel path); 6 = always 5.1 (the browser downmixes it for a stereo device).
    uint32_t channels = 0;
  };

  explicit WebAudioOutput(ConsumedFn on_consumed);
  WebAudioOutput(ConsumedFn on_consumed, const Options& options);
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
  Options options_;
  std::atomic<int32_t>* ctl_ = nullptr;            // kCtlCount entries
  std::atomic<int32_t>* slot_channels_ = nullptr;  // kSlots entries: 2 or 6
  float* ring_ = nullptr;  // kSlots * kChannelSamples * kMaxOutputChannels
  std::atomic<bool> running_{false};
  std::thread pacing_thread_;
  std::mutex submit_mutex_;
};

}  // namespace rex::audio::web
