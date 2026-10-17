#pragma once
/**
 * @file        rex/graphics/null/graphics_system.h
 * @brief       Null GPU backend: real PM4 command processing, no host rendering.
 *
 * Equivalent of Xenia's "null" GPU (gpu=null). The guest ring buffer is consumed
 * by the shared CommandProcessor (register writes, fences, WAIT_REG_MEM,
 * MEM_WRITE, EVENT_WRITE*, interrupts, XE_SWAP, read-pointer write-back) and
 * the vblank thread runs, so guest code that synchronizes with the GPU behaves
 * as on hardware. Draws, resolves (copies) and swaps produce no pixels, and no
 * presenter exists.
 *
 * Used on the web until the WebGPU backend renders (docs/port/STUBS.md in the
 * project, removal at gate 3).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <memory>
#include <string>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>

namespace rex::graphics::null {

class NullGraphicsSystem : public GraphicsSystem {
 public:
  NullGraphicsSystem();
  ~NullGraphicsSystem() override;

  static bool IsAvailable() { return true; }

  std::string name() const override;

  X_STATUS Setup(runtime::FunctionDispatcher* function_dispatcher,
                 system::KernelState* kernel_state, ui::WindowedAppContext* app_context,
                 bool with_presentation) override;

 private:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;
};

}  // namespace rex::graphics::null
