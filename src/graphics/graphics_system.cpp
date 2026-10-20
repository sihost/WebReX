/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <rex/graphics/graphics_system.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/flags.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/stream.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>
#include <rex/web_page.h>

REXCVAR_DEFINE_STRING(trace_gpu_prefix, "", "GPU", "GPU trace file prefix");

REXCVAR_DEFINE_BOOL(web_pause_when_hidden, false, "Web",
                    "Web: no guest vblanks while the browser tab is hidden or frozen, so a game "
                    "paced by vblank (Halo 3) stops advancing and its CPU/GPU use drops; it "
                    "continues where it was when the tab is shown again. Off (default): a hidden "
                    "tab keeps running at full speed (needed for System Link hosts)");

REXCVAR_DEFINE_BOOL(trace_gpu_stream, false, "GPU", "Enable GPU trace streaming");

REXCVAR_DEFINE_STRING(vblank_source, "animation_frame", "GPU",
                      "Web: what wakes the guest vsync thread. animation_frame = the browser's "
                      "animation frames (requestAnimationFrame of the WebGPU presenter), with a "
                      "timed wait until the deadline when there are none (hidden tab); timer = "
                      "1 ms timed waits only, which take up to 15.6 ms in browser Workers on "
                      "Windows and made vblanks irregular. Native: always timer")
    .allowed({"animation_frame", "timer"});

REXCVAR_DEFINE_INT32(vblank_stats_interval_ms, 0, "GPU",
                     "Every N ms, log the guest vblank rate, the host time between vblanks "
                     "(p50/p95/max) and how long the vsync thread's sleeps really took. 0 = off");

REXCVAR_DEFINE_STRING(swap_post_effect, "none", "GPU", "Swap post effect: none, fxaa, fxaa_extreme")
    .allowed({"none", "fxaa", "fxaa_extreme"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {
constexpr bool kStoreShaders = true;

rex::graphics::CommandProcessor::SwapPostEffect ParseSwapPostEffect(
    const std::string& effect_name) {
  std::string lowered = effect_name;
  std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) {
    c = static_cast<unsigned char>(std::tolower(c));
    return c == '-' ? '_' : char(c);
  });
  if (lowered == "fxaa") {
    return rex::graphics::CommandProcessor::SwapPostEffect::kFxaa;
  }
  if (lowered == "fxaa_extreme" || lowered == "extreme") {
    return rex::graphics::CommandProcessor::SwapPostEffect::kFxaaExtreme;
  }
  return rex::graphics::CommandProcessor::SwapPostEffect::kNone;
}
}  // namespace

namespace rex::graphics {

// Nvidia Optimus/AMD PowerXpress support.
// These exports force the process to trigger the discrete GPU in multi-GPU
// systems.
// https://developer.download.nvidia.com/devzone/devcenter/gamegraphics/files/OptimusRenderingPolicies.pdf
// https://stackoverflow.com/questions/17458803/amd-equivalent-to-nvoptimusenablement
#if REX_PLATFORM_WIN32
extern "C" {
__declspec(dllexport) uint32_t NvOptimusEnablement = 0x00000001;
__declspec(dllexport) uint32_t AmdPowerXpressRequestHighPerformance = 1;
}  // extern "C"
#endif  // REX_PLATFORM_WIN32

GraphicsSystem::GraphicsSystem() : vsync_worker_running_(false) {}

GraphicsSystem::~GraphicsSystem() = default;

X_STATUS GraphicsSystem::Setup(runtime::FunctionDispatcher* function_dispatcher,
                               system::KernelState* kernel_state,
                               ui::WindowedAppContext* app_context, bool with_presentation) {
  memory_ = function_dispatcher->memory();
  function_dispatcher_ = function_dispatcher;
  kernel_state_ = kernel_state;
  app_context_ = app_context;

  // Create presenter if presentation is requested and provider is available
  if (with_presentation && provider_) {
    // Safe if either the UI thread call or the presenter creation fails.
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous([this]() {
        presenter_ =
            provider_->CreatePresenter([this](bool is_responsible, bool statically_from_ui_thread) {
              OnHostGpuLossFromAnyThread(is_responsible);
            });
      });
    } else {
      // May be needed for offscreen use, such as capturing the guest output
      // image.
      presenter_ =
          provider_->CreatePresenter([this](bool is_responsible, bool statically_from_ui_thread) {
            OnHostGpuLossFromAnyThread(is_responsible);
          });
    }
  }

  // Create command processor. This will spin up a thread to process all
  // incoming ringbuffer packets.
  command_processor_ = CreateCommandProcessor();
  if (!command_processor_->Initialize()) {
    REXGPU_ERROR("Unable to initialize command processor");
    return X_STATUS_UNSUCCESSFUL;
  }
  command_processor_->SetDesiredSwapPostEffect(ParseSwapPostEffect(REXCVAR_GET(swap_post_effect)));

  // Register GPU MMIO handlers
  // GPU registers are at 0x7FC80000-0x7FCFFFFF
  memory_->AddVirtualMappedRange(0x7FC80000,  // base address
                                 0xFFFF0000,  // mask
                                 0x0000FFFF,  // size (64KB)
                                 this,        // context (GraphicsSystem*)
                                 reinterpret_cast<runtime::MMIOReadCallback>(ReadRegisterThunk),
                                 reinterpret_cast<runtime::MMIOWriteCallback>(WriteRegisterThunk));

  // Guest vblank timer based on the configured guest video mode.
  vsync_worker_running_ = true;
  vsync_worker_thread_ = system::object_ref<system::XHostThread>(
      new system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() {
        system::X_VIDEO_MODE video_mode;
        kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
        double refresh_rate_hz = std::max(1.0, double(float(video_mode.refresh_rate)));
        uint64_t guest_tick_frequency = chrono::Clock::guest_tick_frequency();
        uint64_t vsync_interval_ticks =
            std::max(uint64_t(1), uint64_t(double(guest_tick_frequency) / refresh_rate_hz));
        uint64_t no_vsync_interval_ticks = std::max(uint64_t(1), guest_tick_frequency / 1000);
        uint64_t next_frame_time = chrono::Clock::QueryGuestTickCount() + vsync_interval_ticks;
        bool paused_hidden = false;
        // vblank_stats_interval_ms (host steady clock, ms).
        using StatsClock = std::chrono::steady_clock;
        auto stats_ms = [] {
          return std::chrono::duration<double, std::milli>(
                     StatsClock::now().time_since_epoch())
              .count();
        };
        double stats_start_ms = 0.0, last_vblank_ms = 0.0;
        std::vector<float> vblank_intervals_ms, sleep_ms;
        auto pct = [](std::vector<float>& v, double q) -> float {
          if (v.empty()) {
            return 0.0f;
          }
          std::sort(v.begin(), v.end());
          return v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))];
        };
        // vblank_source=animation_frame (web): browser animation frames seen and when the
          // count last changed (steady clock, ms).
          const bool use_animation_frames =
            REX_PLATFORM_WEB && REXCVAR_GET(vblank_source) == "animation_frame";
          uint32_t animation_frames_seen = rex::web::GetAnimationFrameCount();
          double animation_frame_change_ms = -1e9;
          while (vsync_worker_running_) {
          const uint32_t animation_frames = rex::web::GetAnimationFrameCount();
          bool animation_frames_running = false;
          if (use_animation_frames) {
            double now_ms = stats_ms();
            if (animation_frames != animation_frames_seen) {
              animation_frames_seen = animation_frames;
              animation_frame_change_ms = now_ms;
            }
            animation_frames_running = now_ms - animation_frame_change_ms < 100.0;
          }
          uint64_t current_time = chrono::Clock::QueryGuestTickCount();
          bool pause = REXCVAR_GET(web_pause_when_hidden) && rex::web::IsPageHidden();
          if (pause != paused_hidden) {
            paused_hidden = pause;
            REXGPU_INFO("web_pause_when_hidden: guest vblanks {} (page {})",
                        pause ? "paused" : "resumed", pause ? "hidden" : "visible");
          }
          if (pause) {
            next_frame_time = current_time + vsync_interval_ticks;
            rex::thread::Sleep(std::chrono::milliseconds(20));
            continue;
          }
          uint64_t interval_ticks =
              REXCVAR_GET(vsync) ? vsync_interval_ticks : no_vsync_interval_ticks;
          // Woken by animation frames, fire on the one nearest to the deadline (up to half
          // an interval early): with a 60 Hz display every animation frame is a vblank, with
          // 120/144 Hz the nearest of them, with the average rate kept by the phase below.
          const uint64_t early_ticks =
              animation_frames_running && REXCVAR_GET(vsync) ? interval_ticks / 2 : 0;
          if (current_time + early_ticks >= next_frame_time) {
            MarkVblank();
            if (int32_t stats_interval = REXCVAR_GET(vblank_stats_interval_ms); stats_interval > 0) {
              double now_ms = stats_ms();
              if (stats_start_ms == 0.0) {
                stats_start_ms = now_ms;
              } else {
                vblank_intervals_ms.push_back(float(now_ms - last_vblank_ms));
              }
              last_vblank_ms = now_ms;
              if (now_ms - stats_start_ms >= double(stats_interval) && !vblank_intervals_ms.empty()) {
                size_t n = vblank_intervals_ms.size();
                double elapsed = now_ms - stats_start_ms;
                float max_interval = *std::max_element(vblank_intervals_ms.begin(),
                                                       vblank_intervals_ms.end());
                float max_sleep =
                    sleep_ms.empty() ? 0.0f : *std::max_element(sleep_ms.begin(), sleep_ms.end());
                REXGPU_INFO(
                    "vblank stats: {} vblanks in {:.0f} ms = {:.1f}/s | interval ms p50 {:.2f} "
                    "p95 {:.2f} max {:.2f} | vsync thread waits took p50 {:.2f} p95 {:.2f} max {:.2f} "
                    "({} waits)",
                    n, elapsed, double(n) * 1000.0 / elapsed, pct(vblank_intervals_ms, 0.5),
                    pct(vblank_intervals_ms, 0.95), max_interval, pct(sleep_ms, 0.5),
                    pct(sleep_ms, 0.95), max_sleep, sleep_ms.size());
                vblank_intervals_ms.clear();
                sleep_ms.clear();
                stats_start_ms = now_ms;
              }
            }
            // Keep the phase (next = previous deadline + interval, not now + interval):
              // when the 1 ms sleep below is coarser than that, the guest still gets its
              // refresh rate on average. A hidden browser tab's threads run with the 15.6 ms
              // Windows timer resolution: Sleep(1 ms) returns every 15.6 ms, and "now +
            // interval" fired only every second wake, 32 Hz instead of 60 (the game ran at
            // half speed while its tab was in the background). After a stall, resync
            // instead of firing a burst.
            next_frame_time += interval_ticks;
            if (current_time >= next_frame_time + 2 * interval_ticks) {
              next_frame_time = current_time + interval_ticks;
            }
          } else if (next_frame_time - current_time > vsync_interval_ticks + early_ticks) {
            // The interval got shorter (vsync turned off) or the clock moved: resync.
            next_frame_time = current_time + interval_ticks;
          }
          const bool stats = REXCVAR_GET(vblank_stats_interval_ms) > 0;
          const double sleep_start_ms = stats ? stats_ms() : 0.0;
          if (use_animation_frames && REXCVAR_GET(vsync)) {
            // Until the next animation frame (the precise wake-up). The timeout is only the
            // fallback for hidden tabs and pages before the first paint (no animation frames;
            // then this is the old, coarse, timed wait): while animation frames run it is half
            // an interval past the deadline, so the animation frame there wins.
            current_time = chrono::Clock::QueryGuestTickCount();
            const double ticks_per_ms = double(guest_tick_frequency) / 1000.0;
            double timeout_ms =
                next_frame_time > current_time
                    ? double(next_frame_time - current_time) / ticks_per_ms
                    : 0.0;
            if (animation_frames_running) {
              timeout_ms += double(interval_ticks / 2) / ticks_per_ms;
            }
            rex::web::WaitForAnimationFrame(animation_frames, std::clamp(timeout_ms, 1.0, 50.0));
          } else {
            rex::thread::Sleep(std::chrono::milliseconds(1));
          }
          if (stats) {
            sleep_ms.push_back(float(stats_ms() - sleep_start_ms));
          }
        }
        return 0;
      }));
  // TODO: set_can_debugger_suspend not yet ported
  // vsync_worker_thread_->set_can_debugger_suspend(true);
  vsync_worker_thread_->set_name("GPU VSync");
  vsync_worker_thread_->Create();

  if (REXCVAR_GET(trace_gpu_stream)) {
    BeginTracing();
  }

  return X_STATUS_SUCCESS;
}

void GraphicsSystem::Shutdown() {
  if (command_processor_) {
    EndTracing();
    command_processor_->Shutdown();
    command_processor_.reset();
  }

  if (vsync_worker_thread_) {
    vsync_worker_running_ = false;
    vsync_worker_thread_->Wait(0, 0, 0, nullptr);
    vsync_worker_thread_.reset();
  }

  if (presenter_) {
    if (app_context_) {
      app_context_->CallInUIThreadSynchronous([this]() { presenter_.reset(); });
    }
    // If there's no app context (thus the presenter is owned by the thread that
    // initialized the GraphicsSystem) or can't be queueing UI thread calls
    // anymore, shutdown anyway.
    presenter_.reset();
  }

  provider_.reset();
}

void GraphicsSystem::OnHostGpuLossFromAnyThread([[maybe_unused]] bool is_responsible) {
  // TODO(Triang3l): Somehow gain exclusive ownership of the Provider (may be
  // used by the command processor, the presenter, and possibly anything else,
  // it's considered free-threaded, except for lifetime management which will be
  // involved in this case) and reset it so a new host GPU API device is
  // created. Then ask the command processor to reset itself in its thread, and
  // ask the UI thread to reset the Presenter (the UI thread manages its
  // lifetime - but if there's no WindowedAppContext, either don't reset it as
  // in this case there's no user who needs uninterrupted gameplay, or somehow
  // protect it with a mutex so any thread can be considered a UI thread and
  // reset).
  if (host_gpu_loss_reported_.test_and_set(std::memory_order_relaxed)) {
    return;
  }
  if (HandleHostGpuLoss(is_responsible)) {
    return;
  }
  rex::FatalError("Graphics device lost (probably due to an internal error)");
}

uint32_t GraphicsSystem::ReadRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr) {
  return gs->ReadRegister(addr);
}

void GraphicsSystem::WriteRegisterThunk(void* ppc_context, GraphicsSystem* gs, uint32_t addr,
                                        uint32_t value) {
  gs->WriteRegister(addr, value);
}

uint32_t GraphicsSystem::ReadRegister(uint32_t addr) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x0F00:  // RB_EDRAM_TIMING
      return 0x08100748;
    case 0x0F01:  // RB_BC_CONTROL
      return 0x0000200E;
    case 0x194C: {  // R500_D1MODE_V_COUNTER
      system::X_VIDEO_MODE video_mode;
      kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      return std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
    }
    case 0x1951:    // interrupt status
      return 1;     // vblank
    case 0x1961: {  // AVIVO_D1MODE_VIEWPORT_SIZE
      // Maximum [width(0x0FFF), height(0x0FFF)].
      system::X_VIDEO_MODE video_mode;
      kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
      uint32_t viewport_width = std::min(uint32_t(video_mode.display_width), uint32_t(0x0FFF));
      uint32_t viewport_height = std::min(uint32_t(video_mode.display_height), uint32_t(0x0FFF));
      return (viewport_width << 16) | viewport_height;
    }
    default:
      if (!register_file_.GetRegisterInfo(r)) {
        REXGPU_ERROR("GPU: Read from unknown register ({:04X})", r);
      }
  }

  assert_true(r < RegisterFile::kRegisterCount);
  return register_file_.values[r];
}

void GraphicsSystem::WriteRegister(uint32_t addr, uint32_t value) {
  uint32_t r = (addr & 0xFFFF) / 4;

  switch (r) {
    case 0x01C5:  // CP_RB_WPTR
      command_processor_->UpdateWritePointer(value);
      break;
    case 0x1844:  // AVIVO_D1GRPH_PRIMARY_SURFACE_ADDRESS
      break;
    default:
      REXGPU_WARN("Unknown GPU register {:04X} write: {:08X}", r, value);
      break;
  }

  assert_true(r < RegisterFile::kRegisterCount);
  register_file_.values[r] = value;
}

void GraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
  command_processor_->InitializeRingBuffer(ptr, size_log2);
}

void GraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
  command_processor_->EnableReadPointerWriteBack(ptr, block_size_log2);
}

void GraphicsSystem::SetInterruptCallback(uint32_t callback, uint32_t user_data) {
  interrupt_callback_ = callback;
  interrupt_callback_data_ = user_data;
  REXGPU_INFO("SetInterruptCallback({:08X}, {:08X})", callback, user_data);
}

void GraphicsSystem::DispatchInterruptCallback(uint32_t source, uint32_t cpu) {
  if (!interrupt_callback_) {
    return;
  }

  auto thread = system::XThread::GetCurrentThread();
  assert_not_null(thread);

  // Pick a CPU, if needed. We're going to guess 2. Because.
  if (cpu == 0xFFFFFFFF) {
    cpu = 2;
  }
  thread->SetActiveCpu(cpu);

  // REXGPU_INFO("Dispatching GPU interrupt at {:08X} w/ mode {} on cpu {}",
  //          interrupt_callback_, source, cpu);

  uint64_t args[] = {source, interrupt_callback_data_};
  function_dispatcher_->ExecuteInterrupt(thread->thread_state(), interrupt_callback_, args,
                                         rex::countof(args));
}

void GraphicsSystem::MarkVblank() {
  // TODO: Enable profiling once ported
  // SCOPE_profile_cpu_f("gpu");

  // Increment vblank counter (so the game sees us making progress).
  if (command_processor_) {
    command_processor_->increment_counter();
  }

  // TODO(benvanik): we shouldn't need to do the dispatch here, but there's
  //     something wrong and the CP will block waiting for code that
  //     needs to be run in the interrupt.
  DispatchInterruptCallback(0, 2);
}

void GraphicsSystem::ClearCaches() {
  command_processor_->CallInThread([&]() { command_processor_->ClearCaches(); });
}

void GraphicsSystem::InvalidateGpuMemory() {
  command_processor_->CallInThread([&]() { command_processor_->InvalidateGpuMemory(); });
}

void GraphicsSystem::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                             uint32_t title_id, bool blocking) {
  if (!kStoreShaders) {
    return;
  }
  if (blocking) {
    if (command_processor_->is_paused()) {
      // Safe to run on any thread while the command processor is paused, no
      // race condition.
      command_processor_->InitializeShaderStorage(cache_root, title_id, true);
    } else {
      rex::thread::Fence fence;
      command_processor_->CallInThread([this, cache_root, title_id, &fence]() {
        command_processor_->InitializeShaderStorage(cache_root, title_id, true);
        fence.Signal();
      });
      fence.Wait();
    }
  } else {
    command_processor_->CallInThread([this, cache_root, title_id]() {
      command_processor_->InitializeShaderStorage(cache_root, title_id, false);
    });
  }
}

void GraphicsSystem::RequestFrameTrace() {
  command_processor_->RequestFrameTrace(REXCVAR_GET(trace_gpu_prefix));
}

void GraphicsSystem::BeginTracing() {
  command_processor_->BeginTracing(REXCVAR_GET(trace_gpu_prefix));
}

void GraphicsSystem::EndTracing() {
  command_processor_->EndTracing();
}

void GraphicsSystem::Pause() {
  paused_ = true;
  command_processor_->Pause();
}

void GraphicsSystem::Resume() {
  paused_ = false;
  command_processor_->Resume();
}

bool GraphicsSystem::Save(::rex::stream::ByteStream* stream) {
  stream->Write<uint32_t>(interrupt_callback_);
  stream->Write<uint32_t>(interrupt_callback_data_);
  return command_processor_->Save(stream);
}

bool GraphicsSystem::Restore(::rex::stream::ByteStream* stream) {
  interrupt_callback_ = stream->Read<uint32_t>();
  interrupt_callback_data_ = stream->Read<uint32_t>();
  return command_processor_->Restore(stream);
}

}  // namespace rex::graphics
