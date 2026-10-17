/**
 * ReXGlue web port (Workstream E): AudioSystem backed by the browser (AudioWorklet).
 *
 * Same contract as SDLAudioSystem: one driver per guest render client; the driver
 * releases the client's frame semaphore once per frame played (or paced, while audio is
 * locked by the autoplay gate). XMA decoding is the shared AudioSystem/XmaDecoder path.
 * Only built for Emscripten.
 */

#pragma once

#include <rex/audio/audio_system.h>

namespace rex::audio::web {

class WebAudioSystem : public AudioSystem {
 public:
  explicit WebAudioSystem(runtime::FunctionDispatcher* function_dispatcher);
  ~WebAudioSystem() override;

  static bool IsAvailable() { return true; }

  static std::unique_ptr<AudioSystem> Create(runtime::FunctionDispatcher* function_dispatcher);

  X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                        AudioDriver** out_driver) override;
  void DestroyDriver(AudioDriver* driver) override;
};

}  // namespace rex::audio::web
