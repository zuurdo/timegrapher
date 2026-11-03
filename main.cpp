// main.cpp — terminal timegrapher: PortAudio capture + tg_core analysis + FTXUI display.
//
//   ./timegrapher [-b bph] [-l lift_deg] [-d device_index] [-c calib_s_per_day] [-f highpass_hz] [-L]
//
// Keys:  q quit   r reset   + / -  threshold up / down   (watch the "det" figure)
#include <portaudio.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/canvas.hpp>
#include <ftxui/dom/elements.hpp>

#include "include/tg_core.hpp"

using namespace ftxui;

// ---- single-producer / single-consumer lock-free audio ring ---------------
class Ring {
 public:
  void push(const float* p, size_t n) {  // audio thread
    size_t w = w_.load(std::memory_order_relaxed);
    for (size_t i = 0; i < n; ++i) buf_[(w + i) & kMask] = p[i];
    w_.store(w + n, std::memory_order_release);
  }
  size_t pop(float* out, size_t max) {  // analysis thread
    size_t r = r_.load(std::memory_order_relaxed), w = w_.load(std::memory_order_acquire);
    if (w - r > kSize) { r = w - kSize; overruns_++; }
    size_t n = std::min(max, w - r);
    for (size_t i = 0; i < n; ++i) out[i] = buf_[(r + i) & kMask];
    r_.store(r + n, std::memory_order_release);
    return n;
  }
  std::atomic<int> overruns_{0};

 private:
  static constexpr size_t kSize = 1u << 18, kMask = kSize - 1;
  std::vector<float> buf_ = std::vector<float>(kSize);
  std::atomic<size_t> w_{0}, r_{0};
};

static Ring g_ring;

static int audioCallback(const void* in, void*, unsigned long frames, const PaStreamCallbackTimeInfo*,
                         PaStreamCallbackFlags, void*) {
  if (in) g_ring.push(static_cast<const float*>(in), frames);
  return paContinue;
}

// ---- formatting helpers ---------------------------------------------------
static std::string fmt(double v, const char* f, const char* na = "  --") {
  if (!std::isfinite(v)) return na;
  char b[64];
  snprintf(b, sizeof b, f, v);
  return b;
}

static Element bigValue(const std::string& label, const std::string& value, const std::string& unit, Color c) {
  return vbox({text(label) | dim, hbox({text(value) | bold | color(c), text(" " + unit) | dim})}) | border |
         flex;
}

// Timegrapher paper trace: x = time, y = deviation from the nominal grid (wrapped).
// Tick and tock are drawn in different colours; vertical separation = beat error,
// slope = rate (a flat line is a perfectly regulated watch).
static Element tracePlot(const tg::Stats& s) {
  const int W = 120, H = 56;  // braille canvas pixels (2x4 per character cell)
  const double range = 6.0;   // ms shown, wrapped
  auto c = Canvas(W, H);
  for (int y = 0; y < H; y += 8) c.DrawPointLine(0, y, W - 1, y, Color::GrayDark);
  for (const auto& p : s.trace) {
    double x = (p.secAgo + tg::kTraceWindowSec) / tg::kTraceWindowSec * (W - 1);
    double d = std::fmod(p.devMs + range / 2, range);
    if (d < 0) d += range;
    int y = static_cast<int>((1.0 - d / range) * (H - 1));
    Color col = p.odd ? Color::Cyan : Color::Yellow;
    int xi = static_cast<int>(x);
    for (int dy = 0; dy < 2; ++dy) c.DrawPoint(xi, std::clamp(y + dy, 0, H - 1), true, col);
  }
  return vbox({hbox({text(" Trace ") | bold, text(" tick") | color(Color::Yellow), text(" tock") | color(Color::Cyan),
                     filler(), text("±3 ms, wrapped · 30 s ")}),
               canvas(std::move(c)) | flex}) |
         border | flex;
}

// Filtered audio around the last beat, with the detected sounds marked.
static Element scope(const tg::Stats& s) {
  const int W = 120, H = 32;
  auto c = Canvas(W, H);
  c.DrawPointLine(0, H / 2, W - 1, H / 2, Color::GrayDark);
  if (s.wave.size() > 2) {
    float mx = 1e-6f;
    for (float v : s.wave) mx = std::max(mx, std::fabs(v));
    int px = -1, py = H / 2;
    for (int x = 0; x < W; ++x) {
      size_t i0 = x * s.wave.size() / W, i1 = std::max(i0 + 1, (x + 1) * s.wave.size() / W);
      float lo = 1e9f, hi = -1e9f;
      for (size_t i = i0; i < i1 && i < s.wave.size(); ++i) { lo = std::min(lo, s.wave[i]); hi = std::max(hi, s.wave[i]); }
      int ylo = H / 2 - static_cast<int>(hi / mx * (H / 2 - 1)), yhi = H / 2 - static_cast<int>(lo / mx * (H / 2 - 1));
      c.DrawPointLine(x, ylo, x, yhi, Color::Green);
      (void)px; (void)py;
    }
    for (double ms : s.wavePeaksMs) {
      int x = static_cast<int>(ms / s.waveMs * (W - 1));
      if (x >= 0 && x < W) c.DrawPointLine(x, 0, x, 3, Color::Red);
    }
  }
  return vbox({hbox({text(" Last beat (filtered) ") | bold, filler(), text(fmt(s.waveMs, "%.0f ms ", ""))}),
               canvas(std::move(c)) | flex}) |
         border | flex;
}

// Sparkline of a history series (one point per second), auto-scaled.
static Element history(const std::string& title, const std::vector<double>& h, const char* f, Color col) {
  std::vector<double> v;
  for (double x : h) if (std::isfinite(x)) v.push_back(x);
  double lo = 0, hi = 1;
  if (!v.empty()) {
    lo = *std::min_element(v.begin(), v.end());
    hi = *std::max_element(v.begin(), v.end());
    if (hi - lo < 1e-6) { lo -= 0.5; hi += 0.5; }
  }
  auto g = graph([v, lo, hi](int w, int hgt) {
    std::vector<int> out(w, 0);
    if (v.empty()) return out;
    for (int x = 0; x < w; ++x) {
      size_t i = v.size() > static_cast<size_t>(w) ? v.size() - w + x : x;
      if (i >= v.size()) { out[x] = 0; continue; }
      out[x] = static_cast<int>((v[i] - lo) / (hi - lo) * (hgt - 1));
    }
    return out;
  });
  return vbox({hbox({text(" " + title + " ") | bold, filler(), text(fmt(lo, f, "") + " … " + fmt(hi, f, "") + " ") | dim}),
               g | color(col) | size(HEIGHT, EQUAL, 5)}) |
         border | flex;
}

int main(int argc, char** argv) {
  tg::Config cfg;
  int device = -1;
  bool listDevices = false;
  for (int i = 1; i < argc; ++i) {
    auto arg = [&](const char* k) { return !strcmp(argv[i], k) && i + 1 < argc; };
    if (arg("-b")) cfg.fixedBph = atof(argv[++i]);
    else if (arg("-l")) cfg.liftAngleDeg = atof(argv[++i]);
    else if (arg("-d")) device = atoi(argv[++i]);
    else if (arg("-c")) cfg.calibrationSday = atof(argv[++i]);
    else if (arg("-f")) cfg.highpassHz = atof(argv[++i]);
    else if (!strcmp(argv[i], "-L")) listDevices = true;
    else { fprintf(stderr, "usage: %s [-b bph] [-l lift_deg] [-d device] [-c calib_s_per_day] [-f highpass_hz] [-L]\n", argv[0]); return 1; }
  }

  if (Pa_Initialize() != paNoError) { fprintf(stderr, "PortAudio init failed\n"); return 1; }
  if (listDevices) {
    for (int i = 0; i < Pa_GetDeviceCount(); ++i) {
      const PaDeviceInfo* d = Pa_GetDeviceInfo(i);
      if (d->maxInputChannels > 0) printf("%d: %s (%.0f Hz)\n", i, d->name, d->defaultSampleRate);
    }
    Pa_Terminate();
    return 0;
  }

  PaStreamParameters ip{};
  ip.device = device >= 0 ? device : Pa_GetDefaultInputDevice();
  if (ip.device == paNoDevice) { fprintf(stderr, "No input device\n"); Pa_Terminate(); return 1; }
  ip.channelCount = 1;
  ip.sampleFormat = paFloat32;
  ip.suggestedLatency = Pa_GetDeviceInfo(ip.device)->defaultLowInputLatency;

  // Use the device's native rate: avoids resampling, and timing uses the sample counter.
  cfg.sampleRate = Pa_GetDeviceInfo(ip.device)->defaultSampleRate;
  PaStream* stream = nullptr;
  PaError err = Pa_OpenStream(&stream, &ip, nullptr, cfg.sampleRate, 256, paClipOff, audioCallback, nullptr);
  if (err != paNoError) { fprintf(stderr, "Open failed: %s\n", Pa_GetErrorText(err)); Pa_Terminate(); return 1; }
  const std::string devName = Pa_GetDeviceInfo(ip.device)->name;

  tg::Analyzer analyzer(cfg);
  std::atomic<bool> running{true};

  std::thread worker([&] {
    std::vector<float> tmp(4096);
    while (running) {
      size_t n = g_ring.pop(tmp.data(), tmp.size());
      if (n) analyzer.process(tmp.data(), n); else std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });
  Pa_StartStream(stream);

  auto screen = ScreenInteractive::Fullscreen();

  auto ui = Renderer([&] {
    tg::Stats s = analyzer.snapshot();
    Color rc = !std::isfinite(s.rateSday) ? Color::GrayLight
               : std::fabs(s.rateSday) < 10 ? Color::Green
               : std::fabs(s.rateSday) < 30 ? Color::Yellow : Color::Red;
    Color bc = !std::isfinite(s.beatErrorMs) ? Color::GrayLight : s.beatErrorMs < 0.5 ? Color::Green
               : s.beatErrorMs < 1.0 ? Color::Yellow : Color::Red;
    Color ac = !std::isfinite(s.amplitudeDeg) ? Color::GrayLight : s.amplitudeDeg > 270 ? Color::Green
               : s.amplitudeDeg > 220 ? Color::Yellow : Color::Red;

    std::string status = s.locked ? "LOCKED" : (s.secondsSinceBeat < 3 ? "searching…" : "no signal");
    std::string head = " Timegrapher · " + devName + " · " + fmt(cfg.sampleRate / 1000, "%.1f", "?") + " kHz · " + status +
                       (s.locked ? " · " + fmt(s.bph, "%.0f") + " bph (" + fmt(s.bph / 7200, "%.2f") + " Hz)" : "");

    double lvl = std::min(1.0, s.envelope / std::max(s.threshold * 4, 1e-6));
    auto levelBar = hbox({text(" level ") | dim, gauge(lvl) | color(s.envelope > s.threshold ? Color::Green : Color::GrayLight) | flex,
                          text(" thr ×" + fmt(s.thresholdMult, "%.0f") + " ") | dim});

    auto numbers = hbox({
        bigValue("RATE (20 s)", fmt(s.rateSday, "%+7.1f"), "s/d", rc),
        bigValue("RATE (avg)", fmt(s.rateAvgSday, "%+7.1f"), "s/d", Color::White),
        bigValue("AMPLITUDE", fmt(s.amplitudeDeg, "%5.0f"), "°", ac),
        bigValue("BEAT ERROR", fmt(s.beatErrorMs, "%5.2f"), "ms", bc),
    });
    auto extra = hbox({
        text(" jitter " + fmt(s.jitterMs, "%.3f", "--") + " ms") | dim,
        text("  │  SNR " + fmt(s.snrDb, "%.0f", "--") + " dB") | dim,
        text("  │  det " + fmt(s.detectPct, "%.0f", "--") + " %") | dim,
        text("  │  beats " + std::to_string(s.beats)) | dim,
        text("  │  " + fmt(s.seconds, "%.0f") + " s") | dim,
        g_ring.overruns_ > 0 ? text("  │  AUDIO OVERRUN") | color(Color::Red) : text(""),
        filler(),
        text("q quit · r reset · +/- threshold · lift " + fmt(cfg.liftAngleDeg, "%.0f") + "° ") | dim,
    });

    return vbox({
        text(head) | bold | inverted,
        numbers,
        tracePlot(s) | size(HEIGHT, GREATER_THAN, 12),
        hbox({scope(s) | size(HEIGHT, EQUAL, 9),
              history("Rate s/d", s.rateHist, "%+.1f", Color::Green),
              history("Amplitude °", s.ampHist, "%.0f", Color::Magenta),
              history("Beat error ms", s.beHist, "%.2f", Color::Cyan)}),
        levelBar,
        extra,
    });
  });

  auto root = CatchEvent(ui, [&](Event e) {
    if (e == Event::Character('q') || e == Event::Escape) { screen.Exit(); return true; }
    if (e == Event::Character('r')) { analyzer.reset(); return true; }
    if (e == Event::Character('+') || e == Event::Character('=')) { analyzer.scaleThreshold(1.25); return true; }
    if (e == Event::Character('-')) { analyzer.scaleThreshold(0.8); return true; }
    return false;
  });

  std::thread refresher([&] {
    while (running) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      screen.PostEvent(Event::Custom);
    }
  });

  screen.Loop(root);

  running = false;
  refresher.join();
  worker.join();
  Pa_StopStream(stream);
  Pa_CloseStream(stream);
  Pa_Terminate();
  return 0;
}
