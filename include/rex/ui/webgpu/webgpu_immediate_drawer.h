/**
 * @file        rex/ui/webgpu/webgpu_immediate_drawer.h
 * @brief       ui::ImmediateDrawer for WebGPU (the ImGui overlay on the web canvas).
 *
 * Threads (rex/ui/webgpu/webgpu_presenter.h): ImGui and the other UI drawers
 * run on the UI thread (main() pthread), but every WGPU* handle lives on the
 * WebGPU device thread, which paints the canvas from its requestAnimationFrame
 * loop. So this drawer does not encode anything while ImGui draws:
 * - Begin/BeginDrawBatch/Draw/EndDrawBatch/End (UI thread) record the vertices
 *   (already transformed to NDC), the indices and the draws (scissor, texture,
 *   topology) into a Recording. End hands it to the WebGPUPresenter, which
 *   keeps the recordings of the latest UI paint.
 * - The device thread encodes them into the canvas texture after the guest
 *   output blit, in a load/store render pass (WebGPUPresentPass::Frame::overlay),
 *   on every canvas paint until the next UI paint replaces them.
 * - CreateTexture creates the texture on the device thread (RunSync). Its
 *   resources are shared by the ImmediateTexture and every recording that draws
 *   with it, and are released on the device thread with the last reference.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <webgpu/webgpu.h>

#include <rex/graphics/webgpu/webgpu_device.h>
#include <rex/ui/immediate_drawer.h>

namespace rex {
namespace ui {
namespace webgpu {

// Device-thread state (defined in the .cpp), released on the device thread
// with the last reference.
struct WebGPUImmediateDeviceResources;
struct WebGPUImmediateTextureResources;

// A recorded UI paint (one Begin..End), encoded on the device thread.
class WebGPUImmediateRecording {
 public:
  // Device thread: draws into target (loaded and stored, not cleared). The
  // target may have another size than the one recorded (a resize in between);
  // the NDC vertices stretch, the scissors are scaled.
  void Encode(WGPUCommandEncoder encoder, WGPUTextureView target, WGPUTextureFormat format,
              uint32_t target_width, uint32_t target_height) const;
  bool empty() const { return draws_.empty(); }
  size_t draw_count() const { return draws_.size(); }

 private:
  friend class WebGPUImmediateDrawer;
  struct DrawCommand {
    std::shared_ptr<WebGPUImmediateTextureResources> texture;  // nullptr: white.
    bool lines = false;
    bool indexed = false;
    uint32_t count = 0;
    // First index (indexed) or vertex, absolute in the recording's buffers.
    uint32_t first = 0;
    int32_t base_vertex = 0;
    // In pixels of the recorded render target size.
    uint32_t scissor_x = 0, scissor_y = 0, scissor_width = 0, scissor_height = 0;
  };
  std::shared_ptr<WebGPUImmediateDeviceResources> resources_;
  // Positions already in NDC (x right, y up).
  std::vector<ImmediateVertex> vertices_;
  std::vector<uint16_t> indices_;
  std::vector<DrawCommand> draws_;
  uint32_t render_target_width_ = 0;
  uint32_t render_target_height_ = 0;
};

class WebGPUImmediateDrawer final : public ImmediateDrawer {
 public:
  // UI thread. Returns nullptr if the device is lost or initialization fails.
  static std::unique_ptr<WebGPUImmediateDrawer> Create(graphics::webgpu::WebGPUDevice& device);

  ~WebGPUImmediateDrawer() override;

  std::unique_ptr<ImmediateTexture> CreateTexture(uint32_t width, uint32_t height,
                                                  ImmediateTextureFilter filter, bool is_repeated,
                                                  const uint8_t* data) override;

  void Begin(UIDrawContext& ui_draw_context, float coordinate_space_width,
             float coordinate_space_height) override;
  void BeginDrawBatch(const ImmediateDrawBatch& batch) override;
  void Draw(const ImmediateDraw& draw) override;
  void EndDrawBatch() override;
  void End() override;

 private:
  class WebGPUImmediateTexture;

  explicit WebGPUImmediateDrawer(graphics::webgpu::WebGPUDevice& device) : device_(device) {}
  bool Initialize();

  graphics::webgpu::WebGPUDevice& device_;
  std::shared_ptr<WebGPUImmediateDeviceResources> resources_;

  // Between Begin and End.
  std::shared_ptr<WebGPUImmediateRecording> recording_;
  // Between BeginDrawBatch and EndDrawBatch: where the batch starts in the
  // recording's buffers.
  uint32_t batch_vertex_base_ = 0;
  uint32_t batch_index_base_ = 0;
  uint32_t batch_vertex_count_ = 0;
  uint32_t batch_index_count_ = 0;
  bool batch_open_ = false;
  bool batch_has_index_buffer_ = false;
};

}  // namespace webgpu
}  // namespace ui
}  // namespace rex
