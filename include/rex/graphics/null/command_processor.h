#pragma once
/**
 * @file        rex/graphics/null/command_processor.h
 * @brief       Null GPU backend command processor (see null/graphics_system.h).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <atomic>
#include <cstdint>

#include <rex/graphics/command_processor.h>

namespace rex::graphics::null {

class NullGraphicsSystem;

class NullCommandProcessor : public CommandProcessor {
 public:
  NullCommandProcessor(NullGraphicsSystem* graphics_system, system::KernelState* kernel_state);
  ~NullCommandProcessor() override;

  void TracePlaybackWroteMemory(uint32_t base_ptr, uint32_t length) override;
  void RestoreEdramSnapshot(const void* snapshot) override;

  uint64_t swap_count() const { return swap_count_.load(std::memory_order_relaxed); }

 private:
  bool SetupContext() override;
  void ShutdownContext() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;

  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;

  std::atomic<uint64_t> swap_count_{0};
  uint64_t draw_count_ = 0;
  uint64_t copy_count_ = 0;
};

}  // namespace rex::graphics::null
