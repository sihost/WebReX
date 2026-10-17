/**
 * ReXGlue: audio path diagnostics (any platform).
 *
 * With --audio_stats_interval_ms=N (default 0 = off) AudioSystem::SubmitFrame logs, every N ms
 * and per client, the frames the guest submitted, their RMS/peak (all 6 channels, from guest
 * memory) and how many were non-silent, plus the XMA decoder counters below. The web driver
 * also logs its AudioWorklet ring counters (played / free-run / underruns / context state).
 * This shows the audio path is live without anyone listening.
 */

#pragma once

#include <atomic>
#include <cstdint>

#include <rex/cvar.h>

REXCVAR_DECLARE(int32_t, audio_stats_interval_ms);

namespace rex::audio::stats {

struct XmaCounters {
  std::atomic<uint64_t> work{0};            // XmaContext::Work calls that ran Decode
  std::atomic<uint64_t> frames_decoded{0};  // XMA frames written to guest output buffers
  // contexts that wrote >= 1 frame since the last report (320 contexts -> 5 x 64 bits)
  std::atomic<uint64_t> context_mask[5] = {};
};

XmaCounters& xma();

inline void NoteXmaFrame(uint32_t context_id) {
  auto& c = xma();
  c.frames_decoded.fetch_add(1, std::memory_order_relaxed);
  if (context_id < 320) {
    c.context_mask[context_id / 64].fetch_or(uint64_t(1) << (context_id % 64),
                                             std::memory_order_relaxed);
  }
}

}  // namespace rex::audio::stats
