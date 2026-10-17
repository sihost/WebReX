// WebGPU presenter demo + self-test for the browser (wasm64) build of the SDK.
//
// Runs the same objects an Xbox 360 title uses on the web: WebGPUDevice -> WebGPUProvider ->
// WebGPUPresenter on a ui::Window canvas, driven by the WebWindowedAppContext UI loop, with a
// "guest output" thread standing in for the GPU command processor's swap.
//
// Phases:
//   clear    no guest output yet: the whole canvas is the background color (pixel readback).
//   checker  a 320x180 rgb10a2 guest frame letterboxed 16:9 into the canvas (readback of the
//            bars and cell centers).
//   resize   index.html changes the canvas CSS size; the next presented frame must have the new
//            size and the same image.
//   pacing   5 s of requestAnimationFrame intervals with the guest output refreshed at ~60 Hz.
//   demo     after the checks, an animated guest frame is presented until the page is closed.
// Output: "[demo] ..." lines and one "RESULT {json}" line (index.html shows PASS/FAIL).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <emscripten/emscripten.h>

#include <rex/graphics/webgpu/webgpu_device.h>
#include <rex/graphics/webgpu/webgpu_guest_output.h>
#include <rex/logging.h>
#include <rex/ui/webgpu/webgpu_presenter.h>
#include <rex/ui/webgpu/webgpu_provider.h>
#include <rex/ui/window.h>
#include <rex/ui/window_web.h>
#include <rex/ui/windowed_app_context_web.h>

using rex::graphics::webgpu::WebGPUDevice;
using rex::ui::webgpu::WebGPUPresenter;
using rex::ui::webgpu::WebGPUProvider;

namespace {

constexpr uint32_t kGuestWidth = 320;
constexpr uint32_t kGuestHeight = 180;
constexpr uint32_t kCell = 20;
// Background 0.06/0.06/0.07 -> unorm8 15/15/18.
constexpr float kBackground[3] = {0.06f, 0.06f, 0.07f};
constexpr uint8_t kBackground8[3] = {15, 15, 18};
// Checker colors (10 bit): white and Xbox green (16, 124, 16 in 8 bit).
constexpr uint32_t kCheckerA10[3] = {1023, 1023, 1023};
constexpr uint32_t kCheckerB10[3] = {64, 497, 64};
constexpr uint8_t kCheckerA8[3] = {255, 255, 255};
constexpr uint8_t kCheckerB8[3] = {16, 124, 16};

void Log(const std::string& line) {
  std::printf("[demo] %s\n", line.c_str());
  std::fflush(stdout);
}

template <typename... Args>
std::string Format(const char* format, Args... args) {
  char buffer[1024];
  std::snprintf(buffer, sizeof(buffer), format, args...);
  return buffer;
}

struct Readback {
  uint32_t width = 0;
  uint32_t height = 0;
  WGPUTextureFormat format = WGPUTextureFormat_Undefined;
  std::vector<uint8_t> rgba;  // Tightly packed RGBA8.
};

bool ReadPresentedFrame(WebGPUPresenter& presenter, Readback& out) {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  presenter.RequestPresentedFrameReadback([&](uint32_t width, uint32_t height,
                                              WGPUTextureFormat format, const uint8_t* pixels,
                                              uint32_t row_pitch) {
    std::lock_guard<std::mutex> lock(mutex);
    out.width = width;
    out.height = height;
    out.format = format;
    out.rgba.resize(size_t(width) * height * 4);
    bool bgra = format == WGPUTextureFormat_BGRA8Unorm || format == WGPUTextureFormat_BGRA8UnormSrgb;
    for (uint32_t y = 0; pixels && y < height; ++y) {
      const uint8_t* row = pixels + size_t(y) * row_pitch;
      uint8_t* dest = out.rgba.data() + size_t(y) * width * 4;
      for (uint32_t x = 0; x < width; ++x) {
        dest[x * 4 + 0] = row[x * 4 + (bgra ? 2 : 0)];
        dest[x * 4 + 1] = row[x * 4 + 1];
        dest[x * 4 + 2] = row[x * 4 + (bgra ? 0 : 2)];
        dest[x * 4 + 3] = row[x * 4 + 3];
      }
    }
    done = true;
    cv.notify_all();
  });
  std::unique_lock<std::mutex> lock(mutex);
  return cv.wait_for(lock, std::chrono::seconds(5), [&] { return done; }) && out.width;
}

bool Near(const uint8_t* pixel, const uint8_t* expected, int tolerance = 1) {
  for (int i = 0; i < 3; ++i) {
    if (std::abs(int(pixel[i]) - int(expected[i])) > tolerance) {
      return false;
    }
  }
  return true;
}

// Background everywhere outside the guest rectangle, checkerboard at cell centers (away from
// bilinear edges) inside it. Returns the number of mismatching checked pixels.
uint64_t VerifyFrame(const Readback& frame, int32_t gx, int32_t gy, uint32_t gw, uint32_t gh,
                     uint64_t& checked, std::string& first_error) {
  uint64_t mismatches = 0;
  checked = 0;
  auto fail = [&](uint32_t x, uint32_t y, const uint8_t* pixel, const uint8_t* expected) {
    if (!mismatches) {
      first_error = Format("pixel (%u,%u) = %u,%u,%u expected %u,%u,%u", x, y, pixel[0], pixel[1],
                           pixel[2], expected[0], expected[1], expected[2]);
    }
    ++mismatches;
  };
  for (uint32_t y = 0; y < frame.height; ++y) {
    for (uint32_t x = 0; x < frame.width; ++x) {
      const uint8_t* pixel = frame.rgba.data() + (size_t(y) * frame.width + x) * 4;
      bool inside = gw && int64_t(x) >= gx && int64_t(x) < int64_t(gx) + gw && int64_t(y) >= gy &&
                    int64_t(y) < int64_t(gy) + gh;
      if (!inside) {
        ++checked;
        if (!Near(pixel, kBackground8)) {
          fail(x, y, pixel, kBackground8);
        }
        continue;
      }
      double u = (double(x) + 0.5 - gx) / gw * kGuestWidth;
      double v = (double(y) + 0.5 - gy) / gh * kGuestHeight;
      double cu = std::fmod(u, kCell), cv = std::fmod(v, kCell);
      if (cu < 2.5 || cu > kCell - 2.5 || cv < 2.5 || cv > kCell - 2.5) {
        continue;
      }
      bool a = ((uint32_t(u) / kCell) + (uint32_t(v) / kCell)) % 2 == 0;
      ++checked;
      if (!Near(pixel, a ? kCheckerA8 : kCheckerB8)) {
        fail(x, y, pixel, a ? kCheckerA8 : kCheckerB8);
      }
    }
  }
  return mismatches;
}

uint32_t Pack10(uint32_t r, uint32_t g, uint32_t b) { return r | (g << 10) | (b << 20) | (3u << 30); }

// Writes `texels` (rgb10a2, kGuestWidth x kGuestHeight) as the guest output, the way the
// command processor's swap refreshes it (webgpu_guest_output.h contract).
bool RefreshGuestOutput(WebGPUPresenter& presenter, WebGPUDevice& device,
                        const std::vector<uint32_t>& texels) {
  return presenter.RefreshGuestOutput(
      kGuestWidth, kGuestHeight, 16, 9, [&](rex::ui::Presenter::GuestOutputRefreshContext& ctx) {
        auto& context = static_cast<rex::graphics::webgpu::WebGPUGuestOutputRefreshContext&>(ctx);
        WGPUTexture texture = context.texture();
        uint32_t width = context.width(), height = context.height();
        return device.RunSync([&]() {
          WGPUTexelCopyTextureInfo dest = WGPU_TEXEL_COPY_TEXTURE_INFO_INIT;
          dest.texture = texture;
          WGPUTexelCopyBufferLayout layout = WGPU_TEXEL_COPY_BUFFER_LAYOUT_INIT;
          layout.bytesPerRow = width * 4;
          layout.rowsPerImage = height;
          WGPUExtent3D size = {width, height, 1};
          wgpuQueueWriteTexture(device.queue(), &dest, texels.data(), texels.size() * 4, &layout,
                                &size);
        });
      });
}

const std::vector<uint32_t>& CheckerTexels() {
  static std::vector<uint32_t> texels = [] {
    std::vector<uint32_t> data(size_t(kGuestWidth) * kGuestHeight);
    for (uint32_t y = 0; y < kGuestHeight; ++y) {
      for (uint32_t x = 0; x < kGuestWidth; ++x) {
        bool a = ((x / kCell) + (y / kCell)) % 2 == 0;
        const uint32_t* c = a ? kCheckerA10 : kCheckerB10;
        data[size_t(y) * kGuestWidth + x] = Pack10(c[0], c[1], c[2]);
      }
    }
    return data;
  }();
  return texels;
}

// Animated guest frame for the demo phase: a rotating green/white checkerboard over a plasma.
void DemoTexels(std::vector<uint32_t>& data, double t) {
  data.resize(size_t(kGuestWidth) * kGuestHeight);
  double angle = t * 0.4, ca = std::cos(angle), sa = std::sin(angle);
  double zoom = 1.0 + 0.35 * std::sin(t * 0.7);
  for (uint32_t y = 0; y < kGuestHeight; ++y) {
    for (uint32_t x = 0; x < kGuestWidth; ++x) {
      double px = (double(x) - kGuestWidth * 0.5) / kGuestHeight;
      double py = (double(y) - kGuestHeight * 0.5) / kGuestHeight;
      double rx = (px * ca - py * sa) * zoom * 6.0, ry = (px * sa + py * ca) * zoom * 6.0;
      bool a = (int(std::floor(rx)) + int(std::floor(ry))) & 1;
      double plasma = 0.5 + 0.5 * std::sin(px * 9.0 + t) * std::cos(py * 7.0 - t * 1.3);
      double r, g, b;
      if (a) {
        r = 0.85 + 0.15 * plasma;
        g = r;
        b = r;
      } else {
        r = 0.04 + 0.10 * plasma;
        g = 0.35 + 0.45 * plasma;
        b = 0.04 + 0.10 * plasma;
      }
      data[size_t(y) * kGuestWidth + x] =
          Pack10(uint32_t(r * 1023.0), uint32_t(g * 1023.0), uint32_t(b * 1023.0));
    }
  }
}

template <typename Predicate>
bool WaitFor(Predicate predicate, int timeout_ms) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (!predicate()) {
    if (std::chrono::steady_clock::now() > deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return true;
}

}  // namespace

int main() {
  rex::InitLogging(nullptr);
  rex::ui::WebWindowedAppContext app_context;

  std::unique_ptr<WebGPUDevice> device = WebGPUDevice::Create(WebGPUDevice::Options());
  if (!device || !device->has_surface()) {
    std::printf("RESULT {\"error\":\"WebGPU device creation failed (is WebGPU enabled?)\"}\n");
    return 1;
  }
  std::unique_ptr<WebGPUProvider> provider = WebGPUProvider::Create(*device);
  std::unique_ptr<rex::ui::Presenter> presenter_base =
      provider->CreatePresenter([](bool is_responsible, bool from_ui_thread) {
        Log(Format("host GPU lost (responsible %d, UI thread %d)", int(is_responsible),
                   int(from_ui_thread)));
      });
  if (!presenter_base) {
    std::printf("RESULT {\"error\":\"CreatePresenter failed\"}\n");
    return 1;
  }
  auto* presenter = static_cast<WebGPUPresenter*>(presenter_base.get());
  presenter->SetBackgroundColor(kBackground[0], kBackground[1], kBackground[2]);

  std::unique_ptr<rex::ui::Window> window =
      rex::ui::Window::Create(app_context, "WebReX WebGPU presenter demo", 640, 360);
  window->Open();
  window->SetPresenter(presenter);

  std::thread test_thread([&]() {
    std::string json = "{";
    bool all_ok = true;
    auto add = [&](const std::string& field) { json += (json.size() > 1 ? "," : "") + field; };
    auto stats = [&] { return presenter->GetPaintStats(); };

    Log(Format("adapter: %s", device->adapter_description().c_str()));

    // ---- clear ----
    WaitFor([&] { return stats().presented_frames >= 2; }, 5000);
    Readback frame;
    uint64_t checked = 0, mismatches = 0;
    std::string first_error;
    bool ok = ReadPresentedFrame(*presenter, frame);
    if (ok) {
      mismatches = VerifyFrame(frame, 0, 0, 0, 0, checked, first_error);
      ok = mismatches == 0 && checked == uint64_t(frame.width) * frame.height;
    }
    Log(Format("clear: %s - %ux%u, %" PRIu64 " pixels checked, %" PRIu64 " mismatches %s",
               ok ? "PASS" : "FAIL", frame.width, frame.height, checked, mismatches,
               first_error.c_str()));
    add(Format("\"clear\":%s", ok ? "true" : "false"));
    all_ok &= ok;

    // ---- checker ----
    bool refreshed = RefreshGuestOutput(*presenter, *device, CheckerTexels());
    WaitFor([&] { return stats().guest_output_width != 0; }, 5000);
    ok = refreshed && ReadPresentedFrame(*presenter, frame);
    auto s = stats();
    first_error.clear();
    if (ok) {
      mismatches = VerifyFrame(frame, s.guest_output_x, s.guest_output_y, s.guest_output_width,
                               s.guest_output_height, checked, first_error);
      double scale = std::min(double(frame.width) / 16.0, double(frame.height) / 9.0);
      uint32_t ew = uint32_t(std::lround(16.0 * scale)), eh = uint32_t(std::lround(9.0 * scale));
      bool rect_ok = std::abs(int(s.guest_output_width) - int(ew)) <= 1 &&
                     std::abs(int(s.guest_output_height) - int(eh)) <= 1;
      ok = mismatches == 0 && checked > 1000 && rect_ok;
    }
    Log(Format("checker: %s - guest rect (%d,%d) %ux%u, %" PRIu64 " pixels checked, %" PRIu64
               " mismatches %s",
               ok ? "PASS" : "FAIL", s.guest_output_x, s.guest_output_y, s.guest_output_width,
               s.guest_output_height, checked, mismatches, first_error.c_str()));
    add(Format("\"checker\":%s", ok ? "true" : "false"));
    all_ok &= ok;
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // ---- resize (index.html resizes the canvas when it sees READY_FOR_RESIZE) ----
    uint32_t old_w, old_h;
    rex::ui::web::GetCanvasPhysicalSize(old_w, old_h);
    Log(Format("READY_FOR_RESIZE %u %u", old_w, old_h));
    uint32_t new_w = 0, new_h = 0;
    bool size_changed = WaitFor(
        [&] {
          rex::ui::web::GetCanvasPhysicalSize(new_w, new_h);
          return new_w != old_w || new_h != old_h;
        },
        15000);
    bool presented_new =
        size_changed &&
        WaitFor([&] { auto st = stats(); return st.width == new_w && st.height == new_h; }, 5000);
    ok = presented_new && ReadPresentedFrame(*presenter, frame) && frame.width == new_w &&
         frame.height == new_h;
    first_error.clear();
    if (ok) {
      s = stats();
      mismatches = VerifyFrame(frame, s.guest_output_x, s.guest_output_y, s.guest_output_width,
                               s.guest_output_height, checked, first_error);
      ok = mismatches == 0 && checked > 1000;
    }
    Log(Format("resize: %s - %ux%u -> %ux%u %s", ok ? "PASS" : "FAIL", old_w, old_h, new_w,
               new_h, first_error.c_str()));
    add(Format("\"resize\":%s", ok ? "true" : "false"));
    all_ok &= ok;

    // ---- pacing ----
    presenter->TakeAnimationFrameIntervals();
    presenter->SetAnimationFrameIntervalRecording(true);
    auto pacing_start = std::chrono::steady_clock::now();
    uint32_t refreshes = 0;
    while (std::chrono::steady_clock::now() - pacing_start < std::chrono::seconds(5)) {
      RefreshGuestOutput(*presenter, *device, CheckerTexels());
      ++refreshes;
      std::this_thread::sleep_until(pacing_start + std::chrono::microseconds(16667) * refreshes);
    }
    std::vector<float> intervals = presenter->TakeAnimationFrameIntervals();
    presenter->SetAnimationFrameIntervalRecording(false);
    double sum = 0.0;
    for (float interval : intervals) {
      sum += interval;
    }
    double mean = intervals.empty() ? 0.0 : sum / intervals.size();
    double rate = mean > 0.0 ? 1000.0 / mean : 0.0;
    // Display-rate dependent (60/120/144 Hz all pass); background tabs are throttled by the browser.
    ok = !intervals.empty() && rate > 50.0;
    Log(Format("pacing: %s - %zu animation frames in 5 s, mean %.2f ms (%.1f Hz)",
               ok ? "PASS" : "FAIL", intervals.size(), mean, rate));
    add(Format("\"pacing\":%s,\"rate_hz\":%.1f", ok ? "true" : "false", rate));
    all_ok &= ok;

    uint32_t errors = device->uncaptured_error_count();
    Log(Format("uncaptured WebGPU errors: %u", errors));
    add(Format("\"uncaptured_errors\":%u", errors));
    all_ok &= errors == 0;
    add(Format("\"all_ok\":%s", all_ok ? "true" : "false"));
    std::printf("RESULT %s}\n", json.c_str());
    std::fflush(stdout);

    // ---- demo: animated guest output until the page closes ----
    Log("demo: presenting an animated guest frame");
    std::vector<uint32_t> texels;
    auto start = std::chrono::steady_clock::now();
    for (uint32_t n = 1;; ++n) {
      double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
      DemoTexels(texels, t);
      if (!RefreshGuestOutput(*presenter, *device, texels)) {
        Log("demo: guest output refresh failed, stopping");
        break;
      }
      std::this_thread::sleep_until(start + std::chrono::microseconds(16667) * n);
    }
  });

  app_context.RunMainLoop();
  test_thread.join();
  return 0;
}
