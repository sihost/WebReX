/**
 * ReXGlue web port (Workstream E): AudioSystem + AudioDriver for the browser.
 */

#include <rex/audio/web/web_audio_system.h>

#include <rex/assert.h>
#include <rex/audio/audio_driver.h>
#include <rex/audio/flags.h>
#include <rex/audio/web/web_audio_output.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/thread.h>

// Defined by the SDL driver natively, which is not built for the web.
REXCVAR_DEFINE_BOOL(audio_mute, false, "Audio", "Mute audio output");

namespace rex::audio::web {

namespace {

class WebAudioDriver final : public AudioDriver {
 public:
  WebAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
      : AudioDriver(memory),
        semaphore_(semaphore),
        output_([this](uint32_t frames) {
          auto ret = semaphore_->Release(static_cast<int>(frames), nullptr);
          assert_true(ret);
          (void)ret;
        }) {}

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
  }

 private:
  rex::thread::Semaphore* semaphore_;
  WebAudioOutput output_;
  uint32_t submit_count_ = 0;
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
  REXAPU_INFO("WebAudioSystem: driver {} created (AudioWorklet, 48 kHz stereo downmix)", index);
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
