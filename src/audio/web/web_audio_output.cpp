/**
 * ReXGlue web port (Workstream E): browser audio output. See web_audio_output.h.
 */

#include <rex/audio/web/web_audio_output.h>

#include <rex/audio/conversion.h>

#include <emscripten/em_js.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <new>

namespace rex::audio::web {

namespace {

// The AudioWorkletProcessor source (src/audio/web/rex_audio_worklet.js), loaded on the main
// thread through a Blob URL: no extra file has to be deployed next to halo3.js.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wc23-extensions"
constexpr char kWorkletSource[] = {
#embed "rex_audio_worklet.js"
    , 0};
#pragma clang diagnostic pop

using Clock = std::chrono::steady_clock;
constexpr auto kFrameDuration = std::chrono::nanoseconds(
    1000000000ll * WebAudioOutput::kChannelSamples / WebAudioOutput::kSampleRate);
// No worklet quantum for this long = the worklet is not rendering; pace in free-run (which
// discards the queued frames at 48 kHz). While the AudioContext is not running (not created
// yet, suspended by the autoplay gate, Web Audio unavailable) the worklet can't render, so
// free-run starts quickly.
constexpr auto kStallTimeout = std::chrono::milliseconds(60);
// While the AudioContext is running, the worklet renders even in hidden tabs, but Chrome
// deprioritizes a hidden (or occluded) tab's renderer: its audio thread was measured at
// 68-214 quanta/s instead of 375 for seconds at a time. Taking over after 60 ms there
// threw the queued frames away (never played) and the worklet then played whatever came
// next: choppy, "fast" audio in background tabs. A running context's worklet is only
// waited for: playback is late, never skipped, and the guest is paced by real playback.
// Free-run only if it renders nothing for this long (audio device gone).
constexpr auto kRunningStallTimeout = std::chrono::milliseconds(1500);
// Latency target: lowered by a frame after this long without an underrun. An underrun soon
// after such a step (the lower target was too low) doubles the wait before the next one, up to
// kTargetDecayMax, so probing a lower latency costs a click at most every few minutes.
constexpr auto kTargetDecay = std::chrono::seconds(10);
constexpr auto kTargetDecayMax = std::chrono::seconds(320);
// Underruns closer together than this raise the target once.
constexpr auto kRaiseCooldown = std::chrono::milliseconds(250);
// Underruns in the first second of playback (375 quanta) do not raise it.
constexpr int32_t kWarmupQuanta = 375;

static_assert(sizeof(std::atomic<int32_t>) == 4 && std::atomic<int32_t>::is_always_lock_free,
              "the control block is shared with JS Int32Array Atomics");

}  // namespace

// Runs on the browser main thread (AudioContext and AudioWorklet are only exposed there).
// Pointers are passed as doubles: exact below 2^53 and free of the wasm64 BigInt question.
// One AudioContext is shared by all outputs; each output gets its own AudioWorkletNode.
// channels: 0 = 5.1 if the destination has >= 6 channels, else stereo; 2 or 6 = forced.
EM_JS(void, rex_audio_js_start,
      (double ctl_ptr, double ring_ptr, double slot_ch_ptr, int slots, int slot_samples,
       int max_channels, int channels, double src_ptr, int src_len),
      {
        const STATE = 9, RATE = 13, LAT = 14, OUT_CH = 21, COUNT = 32;
        const log = Module['printErr'] || console.error;
        const st = Module['rexAudio'] || (Module['rexAudio'] = {outputs : []});
        const buffer = HEAPU8.buffer;
        const out = {
          ctlPtr : ctl_ptr,
          ctl : new Int32Array(buffer, ctl_ptr, COUNT),
          ctlF : new Float32Array(buffer, ctl_ptr, COUNT),
          node : null
        };
        st.outputs.push(out);
        if (typeof AudioContext == 'undefined' || typeof AudioWorkletNode == 'undefined') {
          Atomics.store(out.ctl, STATE, 3);
          log('[audio] Web Audio unavailable; audio frames are paced silently');
          return;
        }
        if (!st.ready) {
          const src = new TextDecoder().decode(HEAPU8.slice(src_ptr, src_ptr + src_len));
          const ctx = new AudioContext({sampleRate : 48000, latencyHint : 'interactive'});
          st.ctx = ctx;
          st.update = () => {
            for (const o of st.outputs) {
              Atomics.store(o.ctl, STATE, ctx.state == 'running' ? 2 : 1);
              Atomics.store(o.ctl, RATE, ctx.sampleRate | 0);
              Atomics.store(o.ctl, LAT,
                            Math.round(((ctx.baseLatency || 0) + (ctx.outputLatency || 0)) * 1e6));
            }
          };
          // Autoplay gate: contexts start suspended until a user gesture. These listeners
          // (and the optional loader hook, which calls st.unlock) resume it.
          const gestures = [ 'pointerdown', 'mousedown', 'keydown', 'touchend' ];
          const unlock = () => {
            if (ctx.state == 'suspended') {
              ctx.resume().then(st.update, () => {});
            }
          };
          st.unlock = unlock;
          ctx.onstatechange = () => {
            st.update();
            if (ctx.state == 'running') {
              for (const g of gestures) window.removeEventListener(g, unlock, true);
            }
          };
          for (const g of gestures) window.addEventListener(g, unlock, true);
          st.stats = () => st.outputs.map((o) => ({
                                            write : Atomics.load(o.ctl, 0),
                                            read : Atomics.load(o.ctl, 1),
                                            quanta : Atomics.load(o.ctl, 5),
                                            underrunQuanta : Atomics.load(o.ctl, 6),
                                            played : Atomics.load(o.ctl, 7),
                                            freeRun : Atomics.load(o.ctl, 8),
                                            state : Atomics.load(o.ctl, 9),
                                            peak : o.ctlF[10],
                                            overflows : Atomics.load(o.ctl, 11),
                                            sampleRate : Atomics.load(o.ctl, 13),
                                            outputLatencyUs : Atomics.load(o.ctl, 14),
                                            submitted : Atomics.load(o.ctl, 15),
                                            maxBatch : Atomics.load(o.ctl, 17),
                                            prime : Atomics.load(o.ctl, 18),
                                            target : Atomics.load(o.ctl, 19),
                                            underruns : Atomics.load(o.ctl, 20),
                                            outputChannels : Atomics.load(o.ctl, 21),
                                            withheld : Atomics.load(o.ctl, 22),
                                            destinationMaxChannels :
                                                ctx.destination.maxChannelCount,
                                            contextState : ctx.state
                                          }));
          const url = URL.createObjectURL(new Blob([src], {type : 'text/javascript'}));
          st.ready = ctx.audioWorklet.addModule(url).finally(() => URL.revokeObjectURL(url));
          st.update();
          // Succeeds without a gesture if the page already has (sticky) user activation.
          ctx.resume().then(st.update, () => {});
        }
        st.ready
            .then(() => {
              const dest = st.ctx.destination;
              const destMax = dest.maxChannelCount | 0;
              const surround = channels == 6 || (channels == 0 && destMax >= 6);
              const outChannels = surround ? 6 : 2;
              if (surround && destMax >= 6 && dest.channelCount != 6) {
                // 5.1 to the device; the default destination is stereo.
                try {
                  dest.channelCount = 6;
                  dest.channelCountMode = 'explicit';
                  dest.channelInterpretation = 'speakers';
                } catch (e) {
                  log('[audio] could not set the destination to 6 channels: ' + e);
                }
              }
              const node = new AudioWorkletNode(st.ctx, 'rex-audio', {
                numberOfInputs : 0,
                numberOfOutputs : 1,
                outputChannelCount : [outChannels],
                channelInterpretation : 'speakers',
                processorOptions : {
                  buffer,
                  ctl : ctl_ptr,
                  ring : ring_ptr,
                  slotChannels : slot_ch_ptr,
                  slots,
                  slotSamples : slot_samples,
                  maxChannels : max_channels
                }
              });
              node.connect(dest);
              out.node = node;
              // From now on the driver writes frames in this layout.
              Atomics.store(out.ctl, OUT_CH, outChannels);
              log('[audio] output ' + (surround ? '5.1' : 'stereo downmix') + ' (destination ' +
                  destMax + ' channels max, ' + dest.channelCount + ' in use, requested ' +
                  (channels || 'auto') + ')');
              st.update();
            })
            .catch((e) => {
              Atomics.store(out.ctl, STATE, 3);
              log('[audio] AudioWorklet setup failed: ' + e);
            });
      });

EM_JS(void, rex_audio_js_stop, (double ctl_ptr), {
  const st = Module['rexAudio'];
  if (!st) return;
  const i = st.outputs.findIndex((o) => o.ctlPtr == ctl_ptr);
  if (i < 0) return;
  const o = st.outputs[i];
  st.outputs.splice(i, 1);
  if (o.node) o.node.disconnect();
});

namespace {

struct JsStartArgs {
  double ctl;
  double ring;
  double slot_channels;
  int channels;
};

void JsStartOnMainThread(void* p) {
  auto* args = static_cast<JsStartArgs*>(p);
  rex_audio_js_start(args->ctl, args->ring, args->slot_channels, WebAudioOutput::kSlots,
                     WebAudioOutput::kChannelSamples, WebAudioOutput::kMaxOutputChannels,
                     args->channels,
                     static_cast<double>(reinterpret_cast<uintptr_t>(kWorkletSource)),
                     static_cast<int>(sizeof(kWorkletSource) - 1));
  delete args;
}

void JsStopOnMainThread(void* p) {
  rex_audio_js_stop(static_cast<double>(reinterpret_cast<uintptr_t>(p)));
}

void RunOnMainThread(void (*fn)(void*), void* arg) {
  if (emscripten_is_main_runtime_thread()) {
    fn(arg);
    return;
  }
  // Fire and forget: the driver never waits for the browser (addModule is async anyway).
  emscripten_proxy_async(emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(),
                         fn, arg);
}

}  // namespace

WebAudioOutput::WebAudioOutput(ConsumedFn on_consumed)
    : WebAudioOutput(std::move(on_consumed), Options{}) {}

WebAudioOutput::WebAudioOutput(ConsumedFn on_consumed, const Options& options)
    : on_consumed_(std::move(on_consumed)), options_(options) {
  if (options_.channels != 2 && options_.channels != 6) {
    options_.channels = 0;
  }
}

WebAudioOutput::~WebAudioOutput() {
  Stop();
}

bool WebAudioOutput::Start() {
  if (running_) {
    return true;
  }
  // One 16-byte aligned block: control words, per-slot channel counts, then the ring.
  // Intentionally never freed: the AudioWorklet keeps typed-array views on it until its node
  // is torn down asynchronously on the main thread (about 390 KiB per driver; the guest
  // creates drivers once).
  constexpr size_t kCtlBytes = kCtlCount * sizeof(std::atomic<int32_t>);
  constexpr size_t kSlotChannelsBytes = kSlots * sizeof(std::atomic<int32_t>);
  constexpr size_t kRingBytes =
      size_t(kSlots) * kChannelSamples * kMaxOutputChannels * sizeof(float);
  void* block = std::aligned_alloc(16, kCtlBytes + kSlotChannelsBytes + kRingBytes);
  if (!block) {
    return false;
  }
  ctl_ = static_cast<std::atomic<int32_t>*>(block);
  for (uint32_t i = 0; i < kCtlCount; ++i) {
    new (&ctl_[i]) std::atomic<int32_t>(0);
  }
  slot_channels_ =
      reinterpret_cast<std::atomic<int32_t>*>(static_cast<uint8_t*>(block) + kCtlBytes);
  for (uint32_t i = 0; i < kSlots; ++i) {
    new (&slot_channels_[i]) std::atomic<int32_t>(2);
  }
  ring_ = reinterpret_cast<float*>(static_cast<uint8_t*>(block) + kCtlBytes + kSlotChannelsBytes);
  std::fill_n(ring_, kRingBytes / sizeof(float), 0.0f);
  ctl_[kCtlSlots].store(kSlots);
  ctl_[kCtlSlotSamples].store(kChannelSamples);
  ctl_[kCtlChannels].store(kMaxOutputChannels);
  ctl_[kCtlTarget].store(static_cast<int32_t>(options_.min_frames));

  running_ = true;
  pacing_thread_ = std::thread([this]() { PacingThread(); });

  RunOnMainThread(JsStartOnMainThread,
                  new JsStartArgs{static_cast<double>(reinterpret_cast<uintptr_t>(ctl_)),
                                  static_cast<double>(reinterpret_cast<uintptr_t>(ring_)),
                                  static_cast<double>(reinterpret_cast<uintptr_t>(slot_channels_)),
                                  static_cast<int>(options_.channels)});
  return true;
}

void WebAudioOutput::Stop() {
  if (!running_) {
    return;
  }
  running_ = false;
  emscripten_futex_wake(&ctl_[kCtlRead], INT32_MAX);
  if (pacing_thread_.joinable()) {
    pacing_thread_.join();
  }
  ctl_[kCtlStop].store(1);  // the worklet returns false from process() from now on
  RunOnMainThread(JsStopOnMainThread, ctl_);
}

void WebAudioOutput::set_muted(bool muted) {
  if (ctl_) {
    ctl_[kCtlMuted].store(muted ? 1 : 0, std::memory_order_relaxed);
  }
}

void WebAudioOutput::SubmitGuestFrame(const float* frame) {
  if (!ctl_) {
    Consumed(1);
    return;
  }
  std::lock_guard<std::mutex> lock(submit_mutex_);
  ctl_[kCtlSubmitted].fetch_add(1, std::memory_order_relaxed);
  const int32_t write = ctl_[kCtlWrite].load(std::memory_order_relaxed);
  const int32_t read = ctl_[kCtlRead].load(std::memory_order_acquire);
  if (write - read >= static_cast<int32_t>(kSlots)) {
    // Overflow (cannot happen while the guest respects its semaphore): drop the frame but
    // still hand it back so the guest's in-flight accounting stays balanced.
    ctl_[kCtlOverflows].fetch_add(1, std::memory_order_relaxed);
    Consumed(1);
    return;
  }
  const uint32_t index = static_cast<uint32_t>(write) % kSlots;
  float* slot = ring_ + index * kChannelSamples * kMaxOutputChannels;
  // The layout of the AudioWorkletNode once it exists (stereo until then); recorded per slot,
  // so frames queued before the node switched to 5.1 still play right.
  if (ctl_[kCtlOutChannels].load(std::memory_order_relaxed) == 6) {
    conversion::sequential_6_BE_to_interleaved_6_LE(slot, frame, kChannelSamples);
    slot_channels_[index].store(6, std::memory_order_relaxed);
  } else {
    conversion::sequential_6_BE_to_interleaved_2_LE(slot, frame, kChannelSamples);
    slot_channels_[index].store(2, std::memory_order_relaxed);
  }
  ctl_[kCtlWrite].store(write + 1, std::memory_order_release);
  // Wakes the pacing thread if it idles in free-run with an empty ring.
  emscripten_futex_wake(&ctl_[kCtlRead], 1);
}

WebAudioStats WebAudioOutput::stats() const {
  WebAudioStats s;
  if (!ctl_) {
    return s;
  }
  s.submitted = ctl_[kCtlSubmitted].load();
  s.played = ctl_[kCtlPlayed].load();
  s.free_run = ctl_[kCtlFreeRun].load();
  s.overflows = ctl_[kCtlOverflows].load();
  s.quanta = ctl_[kCtlQuanta].load();
  s.underrun_quanta = ctl_[kCtlUnderrunQuanta].load();
  s.state = ctl_[kCtlState].load();
  s.context_sample_rate = ctl_[kCtlSampleRate].load();
  s.output_latency_us = ctl_[kCtlLatencyUs].load();
  s.ring_frames = static_cast<uint32_t>(ctl_[kCtlWrite].load() - ctl_[kCtlRead].load());
  s.peak = std::bit_cast<float>(ctl_[kCtlPeak].load());
  s.max_batch = static_cast<uint32_t>(ctl_[kCtlMaxBatch].load());
  s.target_frames = static_cast<uint32_t>(ctl_[kCtlTarget].load());
  s.withheld = static_cast<uint32_t>(ctl_[kCtlWithheld].load());
  s.underruns = static_cast<uint32_t>(ctl_[kCtlUnderrunEvents].load());
  s.output_channels = static_cast<uint32_t>(ctl_[kCtlOutChannels].load());
  return s;
}

void WebAudioOutput::Consumed(uint32_t frames) {
  if (frames && on_consumed_) {
    on_consumed_(frames);
  }
}

void WebAudioOutput::PacingThread() {
  int32_t released = ctl_[kCtlRead].load();
  int32_t last_quanta = ctl_[kCtlQuanta].load();
  auto last_quanta_change = Clock::now();
  bool free_run = true;  // until the worklet renders its first quantum
  auto free_run_base = Clock::now();
  // Latency control (see the header). Frames are conserved: the guest's budget B (its
  // audio_maxqframes) = credits it holds + ring depth + withheld. budget is the largest
  // depth + withheld seen, which reaches B as soon as the guest has spent its credits (at
  // start and whenever it waits for us), so withholding budget - target leaves the guest and
  // the ring together `target` frames.
  const bool control = options_.min_frames > 0;
  const int32_t min_target = static_cast<int32_t>(std::max(options_.min_frames, 1u));
  int32_t target = min_target;
  int32_t withheld = 0;
  int32_t budget = 0;
  int32_t seen_underruns = ctl_[kCtlUnderrunEvents].load();
  auto target_changed = Clock::now();
  Clock::duration decay = kTargetDecay;
  auto last_decay = Clock::now() - 2 * kTargetDecayMax;  // "long ago" without overflow
  while (running_) {
    int32_t read = ctl_[kCtlRead].load(std::memory_order_acquire);
    uint32_t n = 0;
    if (read != released) {
      n = static_cast<uint32_t>(read - released);
      released = read;
    }
    const auto now = Clock::now();
    if (!control) {
      if (n) {
        Consumed(n);
      }
    } else {
      withheld += static_cast<int32_t>(n);
      const int32_t depth = ctl_[kCtlWrite].load(std::memory_order_acquire) - read;
      budget = std::max(budget, depth + withheld);
      const int32_t underruns = ctl_[kCtlUnderrunEvents].load(std::memory_order_relaxed);
      if (underruns != seen_underruns) {
        // The guest did not refill in time: give it (and the worklet) more room. Underruns
        // in the first second of playback (threads warming up) and within kRaiseCooldown of
        // the last raise (one scheduling hiccup) count once.
        seen_underruns = underruns;
        if (ctl_[kCtlQuanta].load(std::memory_order_relaxed) > kWarmupQuanta &&
            now - target_changed > kRaiseCooldown) {
          if (now - last_decay < decay) {
            decay = std::min<Clock::duration>(decay * 2, kTargetDecayMax);
          }
          target = std::min(target + 1, std::max(budget, min_target));
          target_changed = now;
        }
      } else if (target > min_target && now - target_changed > decay) {
        --target;
        target_changed = now;
        last_decay = now;
      }
      // After an underrun the worklet waits for `prime` queued frames before it plays again;
      // a smaller target would never let the ring get there.
      const int32_t effective =
          std::max(target, ctl_[kCtlPrime].load(std::memory_order_relaxed));
      // Free-run (nothing audible) paces like real playback only with a full ring: an empty
      // ring at a deadline restarts the clock (measured: 153 instead of 187.5 frames/s with
      // the guest refilling a 3-frame target), so hand everything back there.
      const int32_t keep = free_run ? 0 : std::max(budget - effective, 0);
      if (withheld > keep) {
        n = static_cast<uint32_t>(withheld - keep);
        withheld = keep;
        Consumed(n);
      } else {
        n = 0;
      }
      ctl_[kCtlTarget].store(effective, std::memory_order_relaxed);
      ctl_[kCtlWithheld].store(withheld, std::memory_order_relaxed);
    }
    // Diagnostic: largest batch handed back at once (worklet burst size + wake latency).
    if (static_cast<int32_t>(n) > ctl_[kCtlMaxBatch].load(std::memory_order_relaxed)) {
      ctl_[kCtlMaxBatch].store(static_cast<int32_t>(n), std::memory_order_relaxed);
    }
    const int32_t quanta = ctl_[kCtlQuanta].load(std::memory_order_relaxed);
    if (quanta != last_quanta) {
      last_quanta = quanta;
      last_quanta_change = now;
      free_run = false;
    } else if (!free_run &&
               now - last_quanta_change >
                   (ctl_[kCtlState].load(std::memory_order_relaxed) == kStateRunning
                        ? kRunningStallTimeout
                        : std::chrono::milliseconds(kStallTimeout))) {
      free_run = true;
      free_run_base = now;
    }
    double wait_ms = 20.0;
    if (free_run) {
      // Same model as the nop driver: a queued frame finishes "playing" one frame duration
      // after the previous one (or after it arrived, if the device was idle).
      const int32_t write = ctl_[kCtlWrite].load(std::memory_order_acquire);
      if (write - read <= 0) {
        free_run_base = std::max(free_run_base, now);
      } else if (now >= free_run_base + kFrameDuration) {
        if (ctl_[kCtlRead].compare_exchange_strong(read, read + 1)) {
          ctl_[kCtlFreeRun].fetch_add(1, std::memory_order_relaxed);
        }
        free_run_base += kFrameDuration;
        continue;
      } else {
        wait_ms = std::chrono::duration<double, std::milli>(free_run_base + kFrameDuration - now)
                      .count();
      }
    }
    // Woken by the worklet (Atomics.notify after each frame), by SubmitGuestFrame, or by
    // the timeout (heartbeat check / free-run deadline).
    emscripten_futex_wait(&ctl_[kCtlRead], static_cast<uint32_t>(read), std::max(wait_ms, 0.5));
  }
  // Keep the guest's accounting balanced (the driver is going away).
  if (withheld > 0) {
    Consumed(static_cast<uint32_t>(withheld));
  }
}

}  // namespace rex::audio::web
