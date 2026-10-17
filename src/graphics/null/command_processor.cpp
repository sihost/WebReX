/**
 * @file        graphics/null/command_processor.cpp
 * @brief       Null GPU backend command processor: PM4 executes, no host rendering.
 *
 * Everything that affects guest-visible state (registers, memory writes,
 * fences, interrupts, the read-pointer write-back) is done by the shared
 * CommandProcessor. This class only declines the host-side work: shaders are
 * not translated (no active shader objects), draws and resolves are dropped,
 * and swaps are counted. Guest code that reads back resolve output therefore
 * sees stale memory, as with Xenia's null GPU.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/null/command_processor.h>
#include <rex/graphics/null/graphics_system.h>
#include <rex/logging.h>

namespace rex::graphics::null {

NullCommandProcessor::NullCommandProcessor(NullGraphicsSystem* graphics_system,
                                           system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {}

NullCommandProcessor::~NullCommandProcessor() = default;

void NullCommandProcessor::TracePlaybackWroteMemory(uint32_t /*base_ptr*/, uint32_t /*length*/) {}

void NullCommandProcessor::RestoreEdramSnapshot(const void* /*snapshot*/) {}

bool NullCommandProcessor::SetupContext() {
  return true;
}

void NullCommandProcessor::ShutdownContext() {}

void NullCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                     uint32_t frontbuffer_height) {
  uint64_t n = swap_count_.fetch_add(1, std::memory_order_relaxed) + 1;
  // Log the first swap and then every 600th (about 10 s at 60 Hz) so progress
  // is visible without a presenter.
  if (n == 1 || n % 600 == 0) {
    REXGPU_INFO("null GPU: swap #{} front buffer {:08X} {}x{} ({} draws, {} resolves so far)", n,
                frontbuffer_ptr, frontbuffer_width, frontbuffer_height, draw_count_, copy_count_);
  }
}

Shader* NullCommandProcessor::LoadShader(xenos::ShaderType /*shader_type*/,
                                         uint32_t /*guest_address*/,
                                         const uint32_t* /*host_address*/,
                                         uint32_t /*dword_count*/) {
  return nullptr;
}

bool NullCommandProcessor::IssueDraw(xenos::PrimitiveType /*prim_type*/, uint32_t /*index_count*/,
                                     IndexBufferInfo* /*index_buffer_info*/,
                                     bool /*major_mode_explicit*/) {
  ++draw_count_;
  return true;
}

bool NullCommandProcessor::IssueCopy() {
  ++copy_count_;
  return true;
}

}  // namespace rex::graphics::null
