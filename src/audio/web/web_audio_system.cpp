/**
 * ReXGlue web port (Workstream E): AudioSystem + AudioDriver for the browser.
 */

#include <rex/audio/web/web_audio_system.h>

#include <rex/assert.h>
#include <rex/audio/audio_driver.h>
#include <rex/audio/audio_stats.h>
#include <rex/audio/flags.h>
#include <rex/audio/web/web_audio_output.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/thread.h>

#include <algorithm>
#include <chrono>

// Defined by the SDL driver natively, which is not built for the web.
REXCVAR_DEFINE_BOOL(audio_mute, false, "Audio", "Mute audio output");
REXCVAR_DEFINE_INT32(audio_web_latency_frames, 3, "Audio",
                     "Web: starting/minimum latency target in 5.33 ms frames (queued + being "
                     "rendered by the guest); raised by one frame on each underrun, lowered "
                     "again after 10 s without one (longer after a lowering caused an "
                     "underrun). 0 = off (the ring stays at audio_maxqframes)");
REXCVAR_DEFINE_INT32(audio_web_channels, 0, "Audio",
                     "Web: 0 = 5.1 when the browser's audio destination has 6 or more "
                     "channels, else the stereo downmix; 2 = always stereo; 6 = always 5.1 "
                     "(the browser downmixes it for a stereo device)");

namespace rex::audio::web {

namespace {

class WebAudioDriver final : public AudioDriver {
 public:
  WebAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
      : AudioDriver(memory),
        semaphore_(semaphore),
        output_(
            [this](uint32_t frames) {
              auto ret = semaphore_->Release(static_cast<int>(frames), nullptr);
              assert_true(ret);
              (void)ret;
            },
            MakeOptions()) {}

  static WebAudioOutput::Options MakeOptions() {
    WebAudioOutput::Options options;
    options.min_frames = static_cast<uint32_t>(std::max(REXCVAR_GET(audio_web_latency_frames), 0));
    options.channels = static_cast<uint32_t>(std::max(REXCVAR_GET(audio_web_channels), 0));
    return options;
  }

  bool Initialize() { return output_.Start(); }

  void Shutdown() { output_.Stop(); }

  void SubmitFrame(uint32_t frame_ptr) override {
    // 6 channels x 256 big-endian floats, channel-sequential (same as the SDL driver).
    const auto input_frame = memory_->TranslateVirtual<const float*>(frame_ptr);
    output_.set_muted(REXCVAR_GET(audio_mute));
    output_.SubmitGuestFrame(input_frame);
    if (submit_count_ < 4) {
      ++submit_count_;
      const auto s = output_.stats();
      REXAPU_DEBUG("WebAudioDriver::SubmitFrame: frame_ptr={:08X} state={} ring_frames={}",
                   frame_ptr, s.state, s.ring_frames);
    }
    // --audio_stats_interval_ms: the AudioWorklet side of the ring (rex/audio/audio_stats.h).
    const int32_t interval = REXCVAR_GET(audio_stats_interval_ms);
    if (interval > 0) {
      const auto now = std::chrono::steady_clock::now();
      if (now - last_stats_ >= std::chrono::milliseconds(interval)) {
        last_stats_ = now;
        const auto s = output_.stats();
        REXAPU_INFO(
            "web audio stats: state {} (0 none, 1 suspended, 2 running, 3 failed) rate {} "
            "submitted {} played {} free_run {} quanta {} underrun_quanta {} overflows {} "
            "ring {} last_peak {:.4f} max_batch {} latency {} us target {} withheld {} "
            "underruns {} channels {}",
            s.state, s.context_sample_rate, s.submitted, s.played, s.free_run, s.quanta,
            s.underrun_quanta, s.overflows, s.ring_frames, s.peak, s.max_batch,
            s.output_latency_us, s.target_frames, s.withheld, s.underruns, s.output_channels);
      }
    }
  }

 private:
  rex::thread::Semaphore* semaphore_;
  WebAudioOutput output_;
  uint32_t submit_count_ = 0;
  std::chrono::steady_clock::time_point last_stats_{};
};

}  // namespace

std::unique_ptr<AudioSystem> WebAudioSystem::Create(
    runtime::FunctionDispatcher* function_dispatcher) {
  return std::make_unique<WebAudioSystem>(function_dispatcher);
}

WebAudioSystem::WebAudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : AudioSystem(function_dispatcher) {}

WebAudioSystem::~WebAudioSystem() = default;

X_STATUS WebAudioSystem::CreateDriver([[maybe_unused]] size_t index,
                                      rex::thread::Semaphore* semaphore,
                                      AudioDriver** out_driver) {
  assert_not_null(out_driver);
  auto driver = new WebAudioDriver(memory_, semaphore);
  if (!driver->Initialize()) {
    REXAPU_ERROR("WebAudioSystem: could not start the web audio output");
    delete driver;
    return X_STATUS_UNSUCCESSFUL;
  }
  REXAPU_INFO(
      "WebAudioSystem: driver {} created (AudioWorklet, 48 kHz, channels {} (0 = auto), latency "
      "target from {} frames)",
      index, REXCVAR_GET(audio_web_channels), REXCVAR_GET(audio_web_latency_frames));
  *out_driver = driver;
  return X_STATUS_SUCCESS;
}

void WebAudioSystem::DestroyDriver(AudioDriver* driver) {
  assert_not_null(driver);
  auto web_driver = static_cast<WebAudioDriver*>(driver);
  web_driver->Shutdown();
  delete web_driver;
}

}  // namespace rex::audio::web
