// Synthetic-watch test for tg_core.hpp.
// usage: ./test_synth [bph] [rate s/d] [beat error ms] [amplitude deg] [noise sigma] [seconds]
#include <cstdio>
#include <cstdlib>
#include <random>

#include "tg_core.hpp"

int main(int argc, char** argv) {
  double bph = argc > 1 ? atof(argv[1]) : 28800;
  double rate = argc > 2 ? atof(argv[2]) : 12.0;
  double beMs = argc > 3 ? atof(argv[3]) : 0.35;
  double ampDeg = argc > 4 ? atof(argv[4]) : 270.0;
  double sigma = argc > 5 ? atof(argv[5]) : 0.003;
  double secs = argc > 6 ? atof(argv[6]) : 60.0;
  const double fs = 44100.0, lift = 52.0;

  tg::Config cfg;
  cfg.sampleRate = fs;
  cfg.liftAngleDeg = lift;
  tg::Analyzer an(cfg);

  const double tNom = 3600.0 / bph, tOsc = 2 * tNom;
  const double tp = tNom / (1.0 + rate / 86400.0);  // true beat period
  const double liftS = tOsc / tg::kPi * std::asin(lift / (2.0 * ampDeg));
  const size_t N = static_cast<size_t>(secs * fs);

  std::vector<float> x(N);
  std::mt19937 rng(1);
  std::normal_distribution<float> nd(0.f, static_cast<float>(sigma));
  for (auto& v : x) v = nd(rng);

  auto click = [&](double tc, double amp) {
    long s0 = static_cast<long>(tc * fs);
    for (long i = s0; i < s0 + 200 && i < (long)N; ++i) {
      double tau = i / fs - tc;
      if (tau < 0) continue;
      x[i] += static_cast<float>(amp * std::exp(-tau / 0.00035) * std::sin(2 * tg::kPi * 5000.0 * tau));
    }
  };
  for (long k = 0; 0.3 + k * tp < secs - 0.1; ++k) {
    double t = 0.3 + k * tp + ((k & 1) ? beMs * 1e-3 : 0.0);
    click(t, 0.25);
    click(t + 0.0012, 0.12);
    click(t + liftS, 0.40);
  }

  for (size_t i = 0; i < N; i += 256) an.process(&x[i], std::min<size_t>(256, N - i));

  tg::Stats s = an.snapshot();
  printf("truth : bph %.0f  rate %+.2f s/d  beat error %.2f ms  amplitude %.0f deg  (lift %.2f ms)\n", bph, rate, beMs,
         ampDeg, liftS * 1000);
  printf("found : bph %.0f  rate %+.2f s/d (avg %+.2f)  beat error %.2f ms  amplitude %.0f deg\n", s.bph, s.rateSday,
         s.rateAvgSday, s.beatErrorMs, s.amplitudeDeg);
  printf("extra : jitter %.3f ms  snr %.1f dB  detect %.0f%%  beats %ld  trace pts %zu  hist %zu\n", s.jitterMs, s.snrDb,
         s.detectPct, s.beats, s.trace.size(), s.rateHist.size());
  return 0;
}
