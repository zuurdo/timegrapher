// tg_core.hpp — timegrapher DSP core (no external dependencies).
//
// Feed raw mono audio into Analyzer::process(); call Analyzer::snapshot() from
// any other thread to get everything a timegrapher displays.
//
// Timing is derived from the *sample counter* (audio clock), never from wall
// clock, so scheduling jitter in the program cannot affect the measurements.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <deque>
#include <limits>
#include <mutex>
#include <vector>

namespace tg {

constexpr double kPi = 3.14159265358979323846;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kTraceWindowSec = 30.0;

inline double median(std::vector<double> v) {
  if (v.empty()) return kNaN;
  size_t m = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + m, v.end());
  double hi = v[m];
  if (v.size() % 2) return hi;
  double lo = *std::max_element(v.begin(), v.begin() + m);
  return 0.5 * (lo + hi);
}

// 2nd-order Butterworth high-pass (RBJ cookbook), transposed direct form II.
class Biquad {
 public:
  void designHighpass(double fs, double fc, double q = 0.70710678) {
    double w0 = 2.0 * kPi * fc / fs, cw = std::cos(w0), alpha = std::sin(w0) / (2.0 * q);
    double a0 = 1.0 + alpha;
    b0_ = (1.0 + cw) / 2.0 / a0;
    b1_ = -(1.0 + cw) / a0;
    b2_ = b0_;
    a1_ = -2.0 * cw / a0;
    a2_ = (1.0 - alpha) / a0;
  }
  float process(float x) {
    double y = b0_ * x + z1_;
    z1_ = b1_ * x - a1_ * y + z2_;
    z2_ = b2_ * x - a2_ * y;
    return static_cast<float>(y);
  }

 private:
  double b0_ = 1, b1_ = 0, b2_ = 0, a1_ = 0, a2_ = 0, z1_ = 0, z2_ = 0;
};

struct Config {
  double sampleRate = 44100.0;
  double highpassHz = 2000.0;    // removes handling noise / hum, keeps the "click"
  double thresholdMult = 4.5;    // detection threshold = mult * noise floor
  double minThreshold = 5e-4;    // absolute floor (full scale = 1.0)
  double liftAngleDeg = 52.0;    // escapement lift angle (movement dependent!)
  double fixedBph = 0.0;         // 0 = auto-detect from beat spacing
  double rateWindowSec = 20.0;   // window for the "instantaneous" rate
  double calibrationSday = 0.0;  // subtracted from rate (sound-card clock error)
  double warmupSec = 1.0;        // noise-floor learning time at start
};

struct TracePoint {
  double secAgo;  // <= 0
  double devMs;   // deviation from the nominal beat grid, mean removed
  bool odd;       // tick / tock
};

struct Stats {
  bool locked = false;
  double bph = 0;
  double rateSday = kNaN;     // over the last rateWindowSec
  double rateAvgSday = kNaN;  // over all retained beats
  double amplitudeDeg = kNaN;
  double beatErrorMs = kNaN;
  double jitterMs = kNaN;     // residual std-dev of beat times around the fit
  double snrDb = kNaN;
  double detectPct = 0;       // beats found / beats expected in the last 5 s
  double envelope = 0, noise = 0, threshold = 0, thresholdMult = 0;
  long beats = 0;
  double seconds = 0;
  double secondsSinceBeat = 1e9;
  std::vector<TracePoint> trace;
  std::vector<float> wave;         // filtered audio around the last beat
  double waveMs = 0;
  std::vector<double> wavePeaksMs; // detected sounds inside that window
  std::vector<double> rateHist, ampHist, beHist;  // one point per second
};

class Analyzer {
 public:
  explicit Analyzer(const Config& cfg) : cfg_(cfg), fs_(cfg.sampleRate) {
    hp1_.designHighpass(fs_, cfg.highpassHz);
    hp2_.designHighpass(fs_, cfg.highpassHz);
    envAlpha_ = 1.0 - std::exp(-1.0 / (0.00025 * fs_));  // 0.25 ms smoothing
    guard_ = 0.025 * fs_;
    gap_ = 0.020 * fs_;
    warm_ = cfg.warmupSec * fs_;
    thrMult_ = cfg.thresholdMult;
    wave_.assign(kWaveSize, 0.f);
    if (cfg.fixedBph > 0) lock(cfg.fixedBph);
  }

  void scaleThreshold(double f) {
    std::lock_guard<std::mutex> lk(m_);
    thrMult_ = std::clamp(thrMult_ * f, 2.0, 100.0);
  }

  void reset() {
    std::lock_guard<std::mutex> lk(m_);
    clearMeasurement();
    totalBeats_ = 0;
  }

  void process(const float* in, size_t n) {
    std::lock_guard<std::mutex> lk(m_);
    for (size_t i = 0; i < n; ++i) {
      float h = hp2_.process(hp1_.process(in[i]));
      wave_[sampleIdx_ & kWaveMask] = h;
      env_ += envAlpha_ * (std::fabs(h) - env_);
      detect(static_cast<double>(sampleIdx_), static_cast<float>(env_));
      ++sampleIdx_;
    }
    if (!beats_.empty() && static_cast<double>(sampleIdx_) - beats_.back().t > 10.0 * fs_)
      clearMeasurement();  // watch removed / silence: start a fresh measurement
  }

  Stats snapshot() const {
    std::lock_guard<std::mutex> lk(m_);
    Stats s;
    const double now = static_cast<double>(sampleIdx_);
    s.locked = locked_;
    s.bph = bph_;
    s.rateSday = rate_;
    s.rateAvgSday = rateAvg_;
    s.amplitudeDeg = amp_;
    s.beatErrorMs = be_;
    s.jitterMs = jitter_;
    s.envelope = env_;
    s.noise = noise_;
    s.threshold = threshold();
    s.thresholdMult = thrMult_;
    s.beats = totalBeats_;
    s.seconds = now / fs_;
    if (!beats_.empty()) s.secondsSinceBeat = (now - beats_.back().t) / fs_;
    if (peakLevel_ > 0) s.snrDb = 20.0 * std::log10(peakLevel_ / std::max(noise_, 1e-9));

    if (locked_ && s.seconds > 5.0) {
      size_t cnt = 0;
      for (size_t i = beats_.size(); i > 0 && now - beats_[i - 1].t <= 5.0 * fs_; --i) ++cnt;
      double expected = 5.0 * bph_ / 3600.0;
      s.detectPct = std::min(100.0, 100.0 * cnt / expected);
    }

    if (locked_ && !beats_.empty()) {
      size_t i0 = beats_.size();
      while (i0 > 0 && now - beats_[i0 - 1].t <= kTraceWindowSec * fs_) --i0;
      if (i0 < beats_.size()) {
        const Beat& r = beats_[i0];
        double sum = 0;
        for (size_t i = i0; i < beats_.size(); ++i) {
          const Beat& b = beats_[i];
          double dev = (b.t - r.t - (b.n - r.n) * tNom_) / fs_ * 1000.0;
          s.trace.push_back({(b.t - now) / fs_, dev, (b.n & 1) != 0});
          sum += dev;
        }
        double mean = sum / s.trace.size();
        for (auto& p : s.trace) p.devMs -= mean;
      }
    }
    s.wave = lastWave_;
    s.waveMs = lastWaveMs_;
    s.wavePeaksMs = lastWavePeaksMs_;
    s.rateHist.assign(rateHist_.begin(), rateHist_.end());
    s.ampHist.assign(ampHist_.begin(), ampHist_.end());
    s.beHist.assign(beHist_.begin(), beHist_.end());
    return s;
  }

 private:
  struct Peak { double t; float amp; };
  struct Beat { double t = 0; long n = 0; double lift = kNaN; float amp = 0; };
  struct Fit { bool ok = false; double slope = 0, rms = 0; };

  static constexpr size_t kWaveSize = 8192, kWaveMask = kWaveSize - 1;
  static constexpr size_t kMaxBeats = 7200;
  static constexpr float kMinRelPeak = 0.15f;  // ignore sounds < 15 % of the loudest in a beat

  double threshold() const { return std::max(cfg_.minThreshold, thrMult_ * noise_); }

  // ---- per-sample detection ------------------------------------------------
  void detect(double n, float e) {
    const double thr = threshold();

    // Noise floor: slow average of the envelope, frozen around ticks.
    if (e > thr) lastAbove_ = n;
    bool guard = (n - lastAbove_) < guard_;
    guardRun_ = guard ? guardRun_ + 1 : 0;
    if (n < warm_ || !guard || guardRun_ > fs_) {
      double alpha = (n < warm_) ? 1.0 / (0.05 * fs_) : 1.0 / (2.0 * fs_);
      noise_ += alpha * (e - noise_);
    }

    if (n >= warm_) {
      if (!inPeak_) {
        if (needRearm_) {
          if (e < thr) needRearm_ = false;
        } else if (e > thr) {
          inPeak_ = true;
          pkMax_ = e; pkN_ = n; pkPrev_ = prevE_; pkNext_ = e; nextPending_ = true;
        }
      } else if (e > pkMax_) {
        pkMax_ = e; pkN_ = n; pkPrev_ = prevE_; nextPending_ = true;
      } else {
        if (nextPending_) { pkNext_ = e; nextPending_ = false; }
        if (e < 0.5f * pkMax_) endPeak();
      }
    }
    prevE_ = e;

    if (!cluster_.empty() && !inPeak_ && n - cluster_.back().t > gap_) finalizeCluster();
  }

  void endPeak() {
    // Parabolic interpolation of the envelope maximum -> sub-sample timing.
    double denom = pkPrev_ - 2.0 * pkMax_ + pkNext_;
    double off = (denom < -1e-12) ? 0.5 * (pkPrev_ - pkNext_) / denom : 0.0;
    off = std::clamp(off, -0.5, 0.5);
    Peak p{pkN_ + off, pkMax_};
    if (!cluster_.empty() && p.t - cluster_.front().t > 0.04 * fs_) cluster_.clear();  // not a watch
    cluster_.push_back(p);
    inPeak_ = false;
    needRearm_ = true;
  }

  // A beat = a group of 1-3 sounds (unlock / impulse / drop) closer than gap_.
  void finalizeCluster() {
    float mx = 0;
    for (auto& p : cluster_) mx = std::max(mx, p.amp);
    std::vector<Peak> kept;
    for (auto& p : cluster_) if (p.amp >= kMinRelPeak * mx) kept.push_back(p);

    Beat b;
    b.t = kept.front().t;
    b.amp = mx;
    b.lift = kept.size() >= 2 ? kept.back().t - kept.front().t : kNaN;

    // Keep the filtered waveform around this beat for the scope view.
    long s0 = std::lround(b.t - 0.004 * fs_), len = static_cast<long>(0.020 * fs_);
    if (s0 >= 0) {
      lastWave_.resize(len);
      for (long i = 0; i < len; ++i) lastWave_[i] = wave_[(s0 + i) & kWaveMask];
      lastWaveMs_ = len / fs_ * 1000.0;
      lastWavePeaksMs_.clear();
      for (auto& p : kept) lastWavePeaksMs_.push_back((p.t - s0) / fs_ * 1000.0);
    }
    cluster_.clear();
    addBeat(b);
  }

  // ---- beat bookkeeping ----------------------------------------------------
  void addBeat(Beat b) {
    if (!beats_.empty()) {
      double dt = b.t - beats_.back().t;
      if (locked_) {
        if (dt < 0.5 * tNom_) return;  // extra sound, not a beat
        b.n = beats_.back().n + std::max(1L, std::lround(dt / tNom_));  // fills missed beats
      } else {
        b.n = beats_.back().n + 1;
        intervals_.push_back(dt);
        if (intervals_.size() > 16) intervals_.pop_front();
      }
    }
    beats_.push_back(b);
    ++totalBeats_;
    peakLevel_ = (peakLevel_ <= 0) ? b.amp : 0.8 * peakLevel_ + 0.2 * b.amp;
    if (!locked_) tryLock();
    if (beats_.size() > kMaxBeats) beats_.pop_front();
    if (locked_) updateStats();
  }

  void tryLock() {
    if (intervals_.size() < 12) return;
    double med = median(std::vector<double>(intervals_.begin(), intervals_.end())) / fs_;
    static const double kStd[] = {14400, 18000, 19800, 21600, 25200, 28800, 36000, 43200};
    double best = 0, bestErr = 0.04;
    for (double bph : kStd) {
      double tb = 3600.0 / bph, err = std::fabs(med - tb) / tb;
      if (err < bestErr) { bestErr = err; best = bph; }
    }
    if (best > 0) lock(best);
  }

  void lock(double bph) {
    locked_ = true;
    bph_ = bph;
    tNom_ = fs_ * 3600.0 / bph;
    std::deque<Beat> old;
    old.swap(beats_);
    for (auto& b : old) {
      if (beats_.empty()) { b.n = 0; beats_.push_back(b); continue; }
      double dt = b.t - beats_.back().t;
      if (dt < 0.5 * tNom_) continue;
      b.n = beats_.back().n + std::max(1L, std::lround(dt / tNom_));
      beats_.push_back(b);
    }
    intervals_.clear();
  }

  void clearMeasurement() {
    beats_.clear();
    intervals_.clear();
    cluster_.clear();
    rate_ = rateAvg_ = amp_ = be_ = jitter_ = kNaN;
    peakLevel_ = 0;
    lastHistT_ = lastAvgT_ = -1e18;
    rateHist_.clear(); ampHist_.clear(); beHist_.clear();
    lastWave_.clear(); lastWavePeaksMs_.clear();
    if (cfg_.fixedBph <= 0) { locked_ = false; bph_ = 0; tNom_ = 0; }
  }

  // ---- measurements --------------------------------------------------------
  // Robust linear fit  t = a + slope * n  over beats_[i0..end): one outlier-rejection pass.
  Fit robustFit(size_t i0) const {
    Fit f;
    const size_t N = beats_.size() - i0;
    if (N < 8) return f;
    const double n0 = static_cast<double>(beats_[i0].n), t0 = beats_[i0].t;
    std::vector<char> use(N, 1);
    std::vector<double> res(N);
    for (int pass = 0; pass < 2; ++pass) {
      double sx = 0, sy = 0, sxx = 0, sxy = 0;
      size_t m = 0;
      for (size_t i = 0; i < N; ++i) {
        if (!use[i]) continue;
        double x = beats_[i0 + i].n - n0, y = beats_[i0 + i].t - t0;
        sx += x; sy += y; sxx += x * x; sxy += x * y; ++m;
      }
      if (m < 8) return f;
      double den = m * sxx - sx * sx;
      if (den <= 0) return f;
      double b = (m * sxy - sx * sy) / den, a = (sy - b * sx) / m;
      double ss = 0;
      for (size_t i = 0; i < N; ++i) {
        res[i] = (beats_[i0 + i].t - t0) - (a + b * (beats_[i0 + i].n - n0));
        if (use[i]) ss += res[i] * res[i];
      }
      if (pass == 0) {
        std::vector<double> ar(N);
        for (size_t i = 0; i < N; ++i) ar[i] = std::fabs(res[i]);
        double lim = std::max(4.0 * 1.4826 * median(ar), 0.001 * fs_);
        for (size_t i = 0; i < N; ++i) use[i] = ar[i] <= lim;
      } else {
        f.ok = true; f.slope = b; f.rms = std::sqrt(ss / m);
      }
    }
    return f;
  }

  // Beat error = half the difference between the tick->tock and tock->tick intervals.
  double calcBeatError(size_t i0) const {
    std::vector<double> A, B;
    for (size_t i = i0 + 1; i < beats_.size(); ++i) {
      const Beat &p = beats_[i - 1], &q = beats_[i];
      if (q.n != p.n + 1) continue;
      double d = q.t - p.t;
      if (d < 0.7 * tNom_ || d > 1.3 * tNom_) continue;
      ((p.n & 1) ? B : A).push_back(d);
    }
    if (A.size() < 3 || B.size() < 3) return kNaN;
    return std::fabs(median(A) - median(B)) * 0.5 / fs_ * 1000.0;
  }

  // Amplitude = lift / (2 sin(pi * t_lift / T_osc)),  T_osc = 2 beats.
  double calcAmplitude() const {
    std::vector<double> v;
    size_t cnt = 0;
    for (size_t i = beats_.size(); i > 0 && cnt < 60; --i, ++cnt) {
      const Beat& b = beats_[i - 1];
      if (!std::isfinite(b.lift)) continue;
      double s = std::sin(kPi * b.lift / (2.0 * tNom_));
      if (s <= 1e-3) continue;
      double A = cfg_.liftAngleDeg / (2.0 * s);
      if (A >= 100.0 && A <= 360.0) v.push_back(A);
    }
    return v.size() < 5 ? kNaN : median(v);
  }

  void updateStats() {
    const double lastT = beats_.back().t;
    size_t iw = beats_.size();
    while (iw > 0 && lastT - beats_[iw - 1].t <= cfg_.rateWindowSec * fs_) --iw;

    Fit fw = robustFit(iw);
    if (fw.ok) {
      rate_ = (tNom_ / fw.slope - 1.0) * 86400.0 - cfg_.calibrationSday;
      jitter_ = fw.rms / fs_ * 1000.0;
    }
    be_ = calcBeatError(iw);
    amp_ = calcAmplitude();

    if (lastT - lastAvgT_ >= fs_ && lastT - beats_.front().t > 40.0 * fs_) {
      lastAvgT_ = lastT;
      Fit fa = robustFit(0);
      if (fa.ok) rateAvg_ = (tNom_ / fa.slope - 1.0) * 86400.0 - cfg_.calibrationSday;
    }
    if (lastT - lastHistT_ >= fs_) {
      lastHistT_ = lastT;
      auto push = [](std::deque<double>& d, double v) { d.push_back(v); if (d.size() > 240) d.pop_front(); };
      push(rateHist_, rate_); push(ampHist_, amp_); push(beHist_, be_);
    }
  }

  // ---- state ---------------------------------------------------------------
  mutable std::mutex m_;
  Config cfg_;
  double fs_;
  Biquad hp1_, hp2_;
  double envAlpha_ = 0, guard_ = 0, gap_ = 0, warm_ = 0, thrMult_ = 8;
  std::vector<float> wave_;
  size_t sampleIdx_ = 0;
  double env_ = 0, noise_ = 0, lastAbove_ = -1e18, guardRun_ = 0;
  float prevE_ = 0, pkMax_ = 0, pkPrev_ = 0, pkNext_ = 0;
  double pkN_ = 0;
  bool inPeak_ = false, needRearm_ = false, nextPending_ = false;
  std::vector<Peak> cluster_;

  std::deque<Beat> beats_;
  std::deque<double> intervals_;
  bool locked_ = false;
  double bph_ = 0, tNom_ = 0;
  long totalBeats_ = 0;
  double peakLevel_ = 0;
  double rate_ = kNaN, rateAvg_ = kNaN, amp_ = kNaN, be_ = kNaN, jitter_ = kNaN;
  double lastHistT_ = -1e18, lastAvgT_ = -1e18;
  std::deque<double> rateHist_, ampHist_, beHist_;
  std::vector<float> lastWave_;
  double lastWaveMs_ = 0;
  std::vector<double> lastWavePeaksMs_;
};

}  // namespace tg
