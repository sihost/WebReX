/**
 * @file        rex/ui/surface_web.h
 * @brief       Surface for an HTML canvas (Emscripten), presented through WebGPU.
 *
 * The surface does not own a WGPUSurface: the canvas is an OffscreenCanvas
 * owned by the WebGPU device thread (rex/graphics/webgpu/webgpu_device.h), and
 * only that thread may use the WGPUSurface made from it. This object only names
 * the canvas and publishes its size in physical (device) pixels, which the
 * WebWindow keeps up to date from the browser main thread (ResizeObserver, see
 * src/ui/window_web.cpp).
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

#include <rex/ui/surface.h>

namespace rex {
namespace ui {

class WebCanvasSurface final : public Surface {
 public:
  // size_source: packed physical size (width | height << 32), written by the
  // window from any thread; must outlive the surface (the window owns both).
  WebCanvasSurface(std::string canvas_selector, const std::atomic<uint64_t>& size_source)
      : canvas_selector_(std::move(canvas_selector)), size_source_(size_source) {}

  TypeIndex GetType() const override { return kTypeIndex_WebCanvas; }

  // CSS selector of the canvas ("#canvas").
  const std::string& canvas_selector() const { return canvas_selector_; }

  // Any thread. Asks the UI thread to call Window::OnPaint(force_paint) for the
  // open web window, which owns the canvas surface (no-op if no window is open).
  // Presenters use it to report state found on another thread (WebGPU device
  // loss). Static: it uses no surface state, so a racing surface destruction is
  // harmless.
  static void RequestPaintFromAnyThread(bool force_paint);

 protected:
  bool GetSizeImpl(uint32_t& width_out, uint32_t& height_out) const override {
    uint64_t packed = size_source_.load(std::memory_order_acquire);
    width_out = uint32_t(packed);
    height_out = uint32_t(packed >> 32);
    return width_out && height_out;
  }

 private:
  std::string canvas_selector_;
  const std::atomic<uint64_t>& size_source_;
};

}  // namespace ui
}  // namespace rex
