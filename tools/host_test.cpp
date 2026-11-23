// Host-side test: renders every preset of both engines to WAV and prints stats.
// Build: g++ -O2 -std=c++17 -I../CardputerSynth host_test.cpp ../CardputerSynth/*.cpp -o host_test
#include <cstdio>
#include <vector>
#include <chrono>
#include <cmath>
#include <string>
#include "juno.h"
#include "dx7.h"

using namespace syn;

static void writeWav(const char* path, const std::vector<float>& s) {
  FILE* f = fopen(path, "wb");
  if (!f) return;
  uint32_t n = s.size(), bytes = n * 2;
  auto w32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
  auto w16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
  fwrite("RIFF", 1, 4, f); w32(36 + bytes); fwrite("WAVEfmt ", 1, 8, f);
  w32(16); w16(1); w16(1); w32(SAMPLE_RATE); w32(SAMPLE_RATE * 2); w16(2); w16(16);
  fwrite("data", 1, 4, f); w32(bytes);
  for (float x : s) { int16_t v = (int16_t)lrintf(softClip(x) * 30000.f); fwrite(&v, 2, 1, f); }
  fclose(f);
}

// Play a short chord-and-melody phrase through an engine.
static std::vector<float> phrase(Engine& e, double& renderSec) {
  struct Ev { float t; int note; bool on; };
  std::vector<Ev> evs = {
    {0.0f, 48, true}, {0.0f, 55, true}, {0.0f, 60, true}, {0.0f, 64, true},
    {1.2f, 48, false}, {1.2f, 55, false}, {1.2f, 60, false}, {1.2f, 64, false},
    {1.5f, 72, true}, {1.8f, 72, false}, {1.8f, 76, true}, {2.1f, 76, false},
    {2.1f, 79, true}, {2.6f, 79, false}, {2.6f, 36, true}, {3.2f, 36, false},
  };
  const float total = 4.5f;
  std::vector<float> out;
  float buf[BLOCK];
  size_t ei = 0;
  int blocks = (int)(total * SAMPLE_RATE / BLOCK);
  renderSec = 0;
  for (int b = 0; b < blocks; b++) {
    float t = (float)b * BLOCK / SAMPLE_RATE;
    while (ei < evs.size() && evs[ei].t <= t) {
      if (evs[ei].on) e.noteOn(evs[ei].note, 100); else e.noteOff(evs[ei].note);
      ei++;
    }
    auto t0 = std::chrono::high_resolution_clock::now();
    e.render(buf, BLOCK);
    auto t1 = std::chrono::high_resolution_clock::now();
    renderSec += std::chrono::duration<double>(t1 - t0).count();
    out.insert(out.end(), buf, buf + BLOCK);
  }
  return out;
}

static void stats(const char* label, const std::vector<float>& s, double sec) {
  double peak = 0, sum = 0; bool bad = false;
  for (float x : s) { if (!std::isfinite(x)) bad = true; peak = std::max(peak, (double)fabsf(x)); sum += x * x; }
  double rms = sqrt(sum / s.size());
  // tail energy (last 0.5 s) should be ~0 once everything has been released
  double tail = 0; size_t n0 = s.size() - SAMPLE_RATE / 2;
  for (size_t i = n0; i < s.size(); i++) tail = std::max(tail, (double)fabsf(s[i]));
  double audioSec = (double)s.size() / SAMPLE_RATE;
  printf("%-14s peak %.3f rms %.3f tail %.4f %s  render %.1f%% of realtime\n", label, peak, rms, tail,
         bad ? "NAN!" : "", 100.0 * sec / audioSec);
}

int main() {
  initTables();
  static JunoEngine juno;
  static Dx7Engine dx;
  for (int i = 0; i < JUNO_PRESETS; i++) {
    juno.allNotesOff();
    juno.loadPatch(kJunoPresets[i]);
    double sec;
    auto s = phrase(juno, sec);
    stats(kJunoPresets[i].name, s, sec);
    writeWav(("out/juno_" + std::to_string(i + 1) + ".wav").c_str(), s);
  }
  for (int i = 0; i < DX7_PRESETS; i++) {
    dx.allNotesOff();
    dx.loadPatch(kDx7Presets[i]);
    double sec;
    auto s = phrase(dx, sec);
    stats(kDx7Presets[i].name, s, sec);
    writeWav(("out/dx7_" + std::to_string(i + 1) + ".wav").c_str(), s);
  }
  // SysEx round trip
  uint8_t buf[128];
  Dx7Patch a = kDx7Presets[2], b;
  dx7PackVmem(a, buf);
  dx7UnpackVmem(buf, b);
  printf("VMEM round-trip: %s\n", memcmp(&a, &b, sizeof(a)) == 0 ? "OK" : "MISMATCH");
  return 0;
}
