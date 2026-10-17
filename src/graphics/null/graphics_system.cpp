/**
 * @file        graphics/null/graphics_system.cpp
 * @brief       Null GPU backend (see rex/graphics/null/graphics_system.h).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/graphics/null/command_processor.h>
#include <rex/graphics/null/graphics_system.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

namespace rex::graphics::null {

NullGraphicsSystem::NullGraphicsSystem() = default;

NullGraphicsSystem::~NullGraphicsSystem() = default;

std::string NullGraphicsSystem::name() const {
  return "null";
}

X_STATUS NullGraphicsSystem::Setup(runtime::FunctionDispatcher* function_dispatcher,
                                   system::KernelState* kernel_state,
                                   ui::WindowedAppContext* app_context, bool with_presentation) {
  // No GraphicsProvider: GraphicsSystem::Setup then creates no presenter, and
  // the app skips ImGui/presentation.
  REXGPU_WARN(
      "[STUB] null GPU backend: guest command buffers are executed, nothing is rendered or "
      "presented");
  return GraphicsSystem::Setup(function_dispatcher, kernel_state, app_context, with_presentation);
}

std::unique_ptr<CommandProcessor> NullGraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<NullCommandProcessor>(this, kernel_state_);
}

}  // namespace rex::graphics::null
