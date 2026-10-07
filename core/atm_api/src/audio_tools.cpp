#include "atm/api/audio_tools.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <random>

#include "atm/base/error.hpp"
#include "atm/base/profiler.hpp"
#include "atm/storage/file.hpp"

namespace atm::api::audio {

namespace {

constexpr int kRate = 48000;
constexpr double kPi = 3.14159265358979323846;
constexpr size_t kHop = 240; // 5 ms

std::vector<float> mono_of(const std::vector<float> &stereo) {
  std::vector<float> m(stereo.size() / 2);
  for (size_t i = 0; i < m.size(); ++i)
    m[i] = 0.5f * (stereo[2 * i] + stereo[2 * i + 1]);
  return m;
}

} // namespace

Tempo find_tempo(const std::vector<float> &stereo, double min_bpm, double max_bpm) {
  ATM_PROFILE_SCOPE("audio.find_tempo");
  Tempo out;
  const std::vector<float> mono = mono_of(stereo);
  const size_t n = mono.size() / kHop;
  if (n < 400 || min_bpm <= 0.0 || max_bpm <= min_bpm) // less than two seconds: no beat to find
    return out;
  // The onset curve: the loudness of every 5 ms, and how much it rises from the one before (a little smoothed, so a beat that falls
  // between two frames still counts).
  std::vector<double> env(n), flux(n, 0.0);
  for (size_t i = 0; i < n; ++i) {
    double sum = 0.0;
    for (size_t k = 0; k < kHop; ++k)
      sum += double(mono[i * kHop + k]) * double(mono[i * kHop + k]);
    env[i] = std::sqrt(sum / double(kHop));
  }
  for (size_t i = 1; i < n; ++i)
    flux[i] = std::max(0.0, env[i] - env[i - 1]);
  std::vector<double> soft(n, 0.0);
  for (size_t i = 0; i < n; ++i)
    soft[i] = flux[i] + 0.5 * ((i > 0 ? flux[i - 1] : 0.0) + (i + 1 < n ? flux[i + 1] : 0.0));
  double total = 0.0;
  for (double v : soft)
    total += v;
  if (total <= 1e-9)
    return out;

  const double frame_s = double(kHop) / double(kRate);
  double best = -1.0, best_bpm = 0.0, best_phase = 0.0, sum_scores = 0.0;
  size_t tried = 0;
  for (double bpm = min_bpm; bpm <= max_bpm + 1e-9; bpm += 0.05) {
    const double period = 60.0 / bpm / frame_s; // frames between beats
    const size_t beats = size_t(double(n) / period);
    if (beats < 3)
      continue;
    double tempo_best = -1.0, tempo_phase = 0.0;
    for (double ph = 0.0; ph < period; ph += 1.0) {
      double score = 0.0;
      for (size_t k = 0; k < beats; ++k) {
        const size_t idx = size_t(std::llround(ph + double(k) * period));
        if (idx >= n)
          break;
        score += soft[idx];
      }
      if (score > tempo_best) {
        tempo_best = score;
        tempo_phase = ph;
      }
    }
    sum_scores += tempo_best;
    ++tried;
    if (tempo_best > best) {
      best = tempo_best;
      best_bpm = bpm;
      best_phase = tempo_phase;
    }
  }
  if (tried == 0 || best <= 0.0)
    return out;
  out.bpm = std::round(best_bpm * 20.0) / 20.0;
  out.first_beat = best_phase * frame_s;
  out.confidence = best / std::max(1e-12, sum_scores / double(tried));
  return out;
}

Level measure(const std::vector<float> &stereo) {
  Level l;
  if (stereo.empty())
    return l;
  double sum = 0.0, peak = 0.0;
  for (float v : stereo) {
    sum += double(v) * double(v);
    peak = std::max(peak, double(std::fabs(v)));
  }
  l.peak = peak;
  l.rms = std::sqrt(sum / double(stereo.size()));
  l.peak_db = peak > 1e-6 ? 20.0 * std::log10(peak) : -120.0;
  l.rms_db = l.rms > 1e-6 ? 20.0 * std::log10(l.rms) : -120.0;
  return l;
}

namespace {

std::vector<float> interleave(const std::vector<double> &left, const std::vector<double> &right) {
  std::vector<float> out(left.size() * 2);
  for (size_t i = 0; i < left.size(); ++i) {
    out[2 * i] = float(left[i]);
    out[2 * i + 1] = float(right[i]);
  }
  return out;
}

void normalise(std::vector<double> &y, double peak) {
  double top = 1e-9;
  for (double v : y)
    top = std::max(top, std::fabs(v));
  for (double &v : y)
    v = v / top * peak;
}

// A state-variable band-pass whose centre follows `centre(t)`, fed with noise.
std::vector<double> swept_noise(size_t n, std::mt19937 &rng, const std::function<double(double)> &centre, double q) {
  std::normal_distribution<double> gauss(0.0, 1.0);
  std::vector<double> y(n);
  double low = 0.0, band = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const double t = double(i) / double(kRate);
    const double f = 2.0 * std::sin(kPi * std::min(centre(t), 9000.0) / double(kRate));
    low += f * band;
    const double high = gauss(rng) - low - band / q;
    band += f * high;
    y[i] = band;
  }
  return y;
}

} // namespace

const std::vector<std::string> &kinds() {
  static const std::vector<std::string> all = {"whoosh", "click", "pop", "riser", "impact"};
  return all;
}

std::vector<float> synth(const std::string &kind, unsigned seed, double seconds) {
  ATM_PROFILE_SCOPE("audio.synth");
  std::mt19937 rng(seed);
  std::normal_distribution<double> gauss(0.0, 1.0);
  if (kind == "whoosh") { // builds for half a second and lands on the cut, then tails off; it travels from left to right
    const double land = 0.5;
    const size_t n = size_t(0.7 * kRate);
    const double f0 = 250.0, f1 = 5200.0;
    std::vector<double> y = swept_noise(
        n, rng, [&](double t) { return t < land ? f0 * std::pow(f1 / f0, t / land) : f1 * std::exp(-(t - land) * 9.0); }, 1.6);
    std::vector<double> thump(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
      const double t = double(i) / kRate;
      y[i] *= t < land ? std::pow(t / land, 2.2) : std::exp(-(t - land) * 11.0);
      if (t >= land)
        thump[i] = std::sin(2.0 * kPi * (95.0 - 55.0 * std::clamp(t - land, 0.0, 1.0)) * t) * std::exp(-(t - land) * 16.0);
    }
    normalise(y, 1.0);
    for (size_t i = 0; i < n; ++i)
      y[i] = y[i] * 0.8 + thump[i] * 0.7;
    for (size_t i = 0; i < 2400 && i < n; ++i)
      y[n - 1 - i] *= double(i) / 2400.0;
    normalise(y, 0.85);
    std::vector<double> l(n), r(n);
    for (size_t i = 0; i < n; ++i) {
      const double pan = std::clamp(double(i) / kRate / land, 0.0, 1.0);
      l[i] = y[i] * (1.0 - 0.35 * pan);
      r[i] = y[i] * (0.65 + 0.35 * pan);
    }
    return interleave(l, r);
  }
  if (kind == "click") { // a crisp tick, a 2.4 kHz body that falls quickly, a low thump under it
    const size_t n = size_t(0.14 * kRate);
    std::vector<double> y(n);
    for (size_t i = 0; i < n; ++i) {
      const double t = double(i) / kRate;
      y[i] = gauss(rng) * std::exp(-t * 900.0) * 0.5 + std::sin(2.0 * kPi * (2400.0 - 6000.0 * t) * t) * std::exp(-t * 55.0) * 0.7 +
             std::sin(2.0 * kPi * 140.0 * t) * std::exp(-t * 60.0) * 0.5;
    }
    for (size_t i = 0; i < 48 && i < n; ++i)
      y[i] *= double(i) / 48.0; // no click from the edge itself
    normalise(y, 0.8);
    return interleave(y, y);
  }
  if (kind == "pop") { // a bubble: a sine that falls from 520 to 160 Hz, with a tick at the start
    const size_t n = size_t(0.2 * kRate);
    std::vector<double> y(n);
    double phase = 0.0;
    for (size_t i = 0; i < n; ++i) {
      const double t = double(i) / kRate;
      const double f = 160.0 + 360.0 * std::exp(-t * 30.0);
      phase += 2.0 * kPi * f / kRate;
      y[i] = std::sin(phase) * std::exp(-t * 22.0) + gauss(rng) * std::exp(-t * 700.0) * 0.25;
    }
    for (size_t i = 0; i < 24 && i < n; ++i)
      y[i] *= double(i) / 24.0;
    normalise(y, 0.8);
    return interleave(y, y);
  }
  if (kind == "riser") { // noise that climbs for `seconds` (1.5 by default) and stops at its peak, for a cut or a drop
    const double length = seconds > 0.0 ? std::clamp(seconds, 0.3, 8.0) : 1.5;
    const size_t n = size_t(length * kRate);
    std::vector<double> y = swept_noise(n, rng, [&](double t) { return 200.0 * std::pow(6000.0 / 200.0, std::min(1.0, t / length)); }, 2.2);
    for (size_t i = 0; i < n; ++i) {
      const double t = double(i) / kRate / length;
      y[i] *= t * t;
    }
    for (size_t i = 0; i < 240 && i < n; ++i)
      y[n - 1 - i] *= double(i) / 240.0;
    normalise(y, 0.85);
    std::vector<double> l(n), r(n);
    for (size_t i = 0; i < n; ++i) {
      const double width = 0.2 * double(i) / double(n);
      l[i] = y[i] * (1.0 - width);
      r[i] = y[i] * (1.0 - 0.2 + width);
    }
    return interleave(l, r);
  }
  if (kind == "impact") { // a hit: a deep thump that falls in pitch, and a burst of dark noise
    const size_t n = size_t(0.9 * kRate);
    std::vector<double> y(n);
    double phase = 0.0, low = 0.0;
    for (size_t i = 0; i < n; ++i) {
      const double t = double(i) / kRate;
      phase += 2.0 * kPi * (45.0 + 40.0 * std::exp(-t * 14.0)) / kRate;
      low += 0.06 * (gauss(rng) - low); // dark noise
      y[i] = std::sin(phase) * std::exp(-t * 6.0) + low * std::exp(-t * 20.0) * 2.0;
    }
    for (size_t i = 0; i < 24 && i < n; ++i)
      y[i] *= double(i) / 24.0;
    for (size_t i = 0; i < 2400 && i < n; ++i)
      y[n - 1 - i] *= double(i) / 2400.0;
    normalise(y, 0.9);
    return interleave(y, y);
  }
  return {};
}

Result<void> write_wav(const std::string &path, const std::vector<float> &stereo) {
  const size_t frames = stereo.size() / 2;
  std::string wav(44 + frames * 4, '\0');
  const auto put = [&](size_t at, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; ++i)
      wav[at + size_t(i)] = char((v >> (8 * i)) & 255);
  };
  wav.replace(0, 4, "RIFF");
  put(4, uint32_t(36 + frames * 4), 4);
  wav.replace(8, 8, "WAVEfmt ");
  put(16, 16, 4);
  put(20, 1, 2);
  put(22, 2, 2);
  put(24, uint32_t(kRate), 4);
  put(28, uint32_t(kRate * 4), 4);
  put(32, 4, 2);
  put(34, 16, 2);
  wav.replace(36, 4, "data");
  put(40, uint32_t(frames * 4), 4);
  for (size_t i = 0; i < frames * 2; ++i) {
    const float v = std::clamp(stereo[i], -1.0f, 1.0f);
    put(44 + i * 2, uint32_t(uint16_t(int16_t(std::lround(v * 32767.0f)))), 2);
  }
  std::error_code ec;
  const std::filesystem::path p(std::u8string(path.begin(), path.end()));
  if (p.has_parent_path())
    std::filesystem::create_directories(p.parent_path(), ec);
  return storage::atomic_write(p, wav);
}

} // namespace atm::api::audio
