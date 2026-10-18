/**
 * @file        core/crash_report_web.cpp
 * @brief       Web crash report: log ring, report assembly, delivery to the page.
 *
 * See rex/crash_report.h. The JS half (worker Module.onAbort / 'error' listeners, main-thread
 * Module.__rexFatal) is src/ui/rex_web_pre.js.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/crash_report.h>

static_assert(REX_PLATFORM_WEB, "This file is web-only (Emscripten)");

#include <emscripten.h>
#include <emscripten/em_js.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <spdlog/sinks/base_sink.h>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/detail/seh.h>
#include <rex/thread.h>
#include <rex/thread/thread_report.h>
#include <rex/version.h>

REXCVAR_DEFINE_STRING(web_crash_test, "", "Web",
                      "Web: crash a helper thread on purpose after web_crash_test_delay_s to try "
                      "the page's crash report UX (Module.rexOnFatal): fatal (rex::FatalError), "
                      "abort (std::abort), trap (wasm unreachable), guest (unhandled guest "
                      "exception, seh_rethrow). Empty = off");
REXCVAR_DEFINE_INT32(web_crash_test_delay_s, 20, "Web",
                     "Web: seconds after startup before --web_crash_test crashes");
REXCVAR_DEFINE_INT32(web_crash_report_log_lines, 120, "Web",
                     "Web: number of recent log lines included in a crash report (max 256)");

namespace {

// Lock-free reader, serialized writers (base_sink's mutex): the crash path must not take a lock
// the crashed thread could hold.
class CrashLogRing final : public spdlog::sinks::base_sink<std::mutex> {
 public:
  static constexpr size_t kLines = 256;
  static constexpr size_t kLineBytes = 384;

  std::string Tail(size_t count) const {
    uint64_t end = next_.load(std::memory_order_acquire);
    count = std::min<uint64_t>({count, kLines, end});
    std::string out;
    out.reserve(count * 96);
    for (uint64_t seq = end - count; seq < end; ++seq) {
      const char* line = lines_[seq % kLines];
      out.append(line, strnlen(line, kLineBytes));
      out.push_back('\n');
    }
    return out;
  }

 protected:
  void sink_it_(const spdlog::details::log_msg& msg) override {
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);
    size_t n = formatted.size();
    while (n && (formatted[n - 1] == '\n' || formatted[n - 1] == '\r')) {
      --n;
    }
    n = std::min(n, kLineBytes - 1);
    uint64_t seq = next_.load(std::memory_order_relaxed);
    char* slot = lines_[seq % kLines];
    std::memcpy(slot, formatted.data(), n);
    slot[n] = '\0';
    next_.store(seq + 1, std::memory_order_release);
  }
  void flush_() override {}

 private:
  char lines_[kLines][kLineBytes] = {};
  std::atomic<uint64_t> next_{0};
};

CrashLogRing* g_ring = nullptr;
std::once_flag g_install_once;
std::atomic<bool> g_collected{false};
std::atomic<bool> g_noted{false};
char g_note_kind[32];
char g_note_message[2048];
const auto g_start_time = std::chrono::steady_clock::now();

void AppendJsonString(std::string& out, std::string_view s) {
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
    }
  }
  out.push_back('"');
}

void CrashTestThread(std::string mode, int delay_s) {
  std::this_thread::sleep_for(std::chrono::seconds(std::max(delay_s, 0)));
  REXLOG_WARN("web_crash_test: crashing this thread on purpose ({})", mode);
  if (mode == "fatal") {
    rex::FatalError("web_crash_test: deliberate rex::FatalError");
  } else if (mode == "abort") {
    std::abort();
  } else if (mode == "trap") {
    __builtin_trap();
  } else if (mode == "guest") {
    rex::ppc::detail::seh_rethrow();
  }
  REXLOG_ERROR("web_crash_test: unknown mode '{}' (fatal, abort, trap, guest)", mode);
}

}  // namespace

EM_JS_DEPS(rex_crash_report, "$UTF8ToString,$lengthBytesUTF8,$stringToUTF8");

// The worker side of rex_web_pre.js leaves {message, stack} of the abort / trap in
// Module.__rexCrashJs as a JSON string; length first, then copy (no malloc from JS).
// clang-format off
EM_JS(double, rexweb_crash_js_length, (), {
  var s = Module["__rexCrashJsJson"] || "{}";
  return lengthBytesUTF8(s);
});
EM_JS(void, rexweb_crash_js_take, (double dst, double cap), {
  stringToUTF8(Module["__rexCrashJsJson"] || "{}", dst, cap);
});
// clang-format on

extern "C" {

void rex_web_note_fatal(const char* kind, const char* message) {
  if (g_noted.exchange(true)) {
    return;
  }
  std::snprintf(g_note_kind, sizeof(g_note_kind), "%s", kind ? kind : "fatal");
  std::snprintf(g_note_message, sizeof(g_note_message), "%s", message ? message : "");
}

// Called by rex_web_pre.js on the crashing thread (worker Module.onAbort before Emscripten's
// abort traps, or the worker 'error' event after a trap). code: 1 = abort, 2 = trap.
EMSCRIPTEN_KEEPALIVE void rex_web_crash_collect(int code) {
  if (g_collected.exchange(true)) {
    return;
  }
  std::string r;
  r.reserve(64 * 1024);
  r += "{\"kind\":";
  if (g_noted.load()) {
    AppendJsonString(r, g_note_kind);
    r += ",\"note\":";
    AppendJsonString(r, g_note_message);
  } else {
    AppendJsonString(r, code == 2 ? "trap" : "abort");
  }

  // {message, stack} from JS: valid JSON produced by JSON.stringify, embedded as is.
  size_t js_len = static_cast<size_t>(rexweb_crash_js_length());
  std::string js(js_len + 1, '\0');
  rexweb_crash_js_take(static_cast<double>(reinterpret_cast<uintptr_t>(js.data())),
                       static_cast<double>(js.size()));
  js.resize(std::strlen(js.c_str()));
  r += ",\"js\":";
  r += js.empty() ? "{}" : js;

  r += ",\"thread\":{\"tid\":" + std::to_string(gettid()) +
       ",\"id\":" + std::to_string(rex::thread::current_thread_id()) + "}";
  r += ",\"threads\":";
  AppendJsonString(r, rex::thread::DescribeThreadsForDiagnostics());

  r += ",\"uptime_s\":" +
       std::to_string(std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                    g_start_time)
                          .count());
  r += ",\"build\":{\"sdk\":";
  AppendJsonString(r, REXGLUE_VERSION_STRING);
  r += ",\"compiled\":";
  AppendJsonString(r, __DATE__ " " __TIME__);
  r += "}";

  r += ",\"cvars\":{";
  bool first = true;
  for (const auto& name : rex::cvar::ListModifiedFlags()) {
    if (!first) {
      r.push_back(',');
    }
    first = false;
    AppendJsonString(r, name);
    r.push_back(':');
    AppendJsonString(r, rex::cvar::GetFlagByName(name));
  }
  r += "}";

  r += ",\"log\":";
  int lines = std::clamp(REXCVAR_GET(web_crash_report_log_lines), 0, 256);
  AppendJsonString(r, g_ring ? g_ring->Tail(static_cast<size_t>(lines)) : std::string());
  r += "}";

  // Leaked on purpose: the main thread reads it after this thread is gone.
  char* copy = static_cast<char*>(std::malloc(r.size() + 1));
  if (!copy) {
    return;
  }
  std::memcpy(copy, r.c_str(), r.size() + 1);
  MAIN_THREAD_ASYNC_EM_ASM(
      {
        if (Module["__rexFatal"]) Module["__rexFatal"]($0, $1);
      },
      static_cast<double>(reinterpret_cast<uintptr_t>(copy)), static_cast<double>(r.size()));
}

}  // extern "C"

namespace rex::crash {

void InstallCrashReporting() {
  std::call_once(g_install_once, [] {
    auto ring = std::make_shared<CrashLogRing>();
    ring->set_pattern("%H:%M:%S.%e [%n] %L %v");
    g_ring = ring.get();
    // Kept alive by the logging registry (and leaked at exit on purpose: the report may be
    // collected while static destructors run).
    new std::shared_ptr<CrashLogRing>(ring);
    rex::AddSink(ring);
    std::string mode = REXCVAR_GET(web_crash_test);
    if (!mode.empty()) {
      REXLOG_WARN("web_crash_test={}: a helper thread crashes in {} s", mode,
                  REXCVAR_GET(web_crash_test_delay_s));
      std::thread(CrashTestThread, mode, REXCVAR_GET(web_crash_test_delay_s)).detach();
    }
  });
}

}  // namespace rex::crash
