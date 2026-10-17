/**
 * @file        ui/window_web.cpp
 * @brief       STUB Window/MenuItem for the Emscripten (wasm64) build.
 *
 * Honest placeholder until the canvas window lands (Phase 2, WS-D/WS-F):
 * - The window is "open" at its desired size with focus, so the app and the
 *   input system can attach listeners.
 * - CreateSurfaceImpl returns null: there is no presentable surface yet (no
 *   WebGPU presenter), so nothing is drawn.
 * - No browser events (resize, keyboard, mouse) are delivered.
 * Listed in docs/port/STUBS.md (owner WS-D; removal gate: first frame on
 * canvas).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <cstdio>
#include <memory>
#include <string>

#include <rex/ui/menu_item.h>
#include <rex/ui/window.h>

namespace rex {
namespace ui {

namespace {

class WebWindow final : public Window {
 public:
  WebWindow(WindowedAppContext& app_context, const std::string_view title,
            uint32_t desired_logical_width, uint32_t desired_logical_height)
      : Window(app_context, title, desired_logical_width, desired_logical_height) {}

  ~WebWindow() override { EnterDestructor(); }

 protected:
  bool OpenImpl() override {
    std::fprintf(stderr,
                 "[STUB] ui::WebWindow::OpenImpl: headless placeholder window %ux%u, no canvas "
                 "surface (docs/port/STUBS.md)\n",
                 GetDesiredLogicalWidth(), GetDesiredLogicalHeight());
    WindowDestructionReceiver destruction_receiver(this);
    OnActualSizeUpdate(GetDesiredLogicalWidth(), GetDesiredLogicalHeight(), destruction_receiver);
    if (destruction_receiver.IsWindowDestroyedOrClosed()) {
      return true;
    }
    OnFocusUpdate(true, destruction_receiver);
    return true;
  }

  void RequestCloseImpl() override {
    WindowDestructionReceiver destruction_receiver(this);
    OnBeforeClose(destruction_receiver);
    if (destruction_receiver.IsWindowDestroyed()) {
      return;
    }
    OnAfterClose();
  }

  std::unique_ptr<Surface> CreateSurfaceImpl(Surface::TypeFlags allowed_types) override {
    (void)allowed_types;
    std::fprintf(stderr,
                 "[STUB] ui::WebWindow::CreateSurfaceImpl: no web surface type yet "
                 "(docs/port/STUBS.md)\n");
    return nullptr;
  }

  void RequestPaintImpl() override {}
};

class WebMenuItem final : public MenuItem {
 public:
  WebMenuItem(Type type, const std::string& text, const std::string& hotkey,
              std::function<void()> callback)
      : MenuItem(type, text, hotkey, std::move(callback)) {}
};

}  // namespace

std::unique_ptr<Window> Window::Create(WindowedAppContext& app_context,
                                       const std::string_view title, uint32_t desired_logical_width,
                                       uint32_t desired_logical_height) {
  return std::make_unique<WebWindow>(app_context, title, desired_logical_width,
                                     desired_logical_height);
}

std::unique_ptr<ui::MenuItem> MenuItem::Create(Type type, const std::string& text,
                                               const std::string& hotkey,
                                               std::function<void()> callback) {
  return std::make_unique<WebMenuItem>(type, text, hotkey, std::move(callback));
}

}  // namespace ui
}  // namespace rex
