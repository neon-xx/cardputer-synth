// small_reverb.h - compact mono reverb (Schroeder/Freeverb style) for the
// AMY output. AMY's own reverb needs ~110 KB of RAM, which the Cardputer
// (no PSRAM) can't spare; this one uses ~18 KB (int16 delay lines).
#pragma once
#include <stdint.h>
#include <string.h>

class SmallReverb {
 public:
  // Freeverb tunings (44.1 kHz)
  static constexpr int NC = 6, NA = 3;
  static constexpr int kComb[NC] = {1116, 1188, 1277, 1356, 1422, 1491};
  static constexpr int kAll[NA]  = {556, 441, 341};

  void clear() {
    memset(cbuf_, 0, sizeof(cbuf_));
    memset(abuf_, 0, sizeof(abuf_));
    for (int i = 0; i < NC; i++) { cpos_[i] = 0; cfilt_[i] = 0.f; }
    for (int i = 0; i < NA; i++) apos_[i] = 0;
  }
  SmallReverb() { clear(); }

  // level 0..1 (wet amount). Processes int16 mono in place.
  void process(int16_t* io, int n, float level) {
    if (level <= 0.f) {
      if (wasOn_) { clear(); wasOn_ = false; }
      return;
    }
    wasOn_ = true;
    const float fb = 0.84f, damp = 0.25f, inGain = 0.015f;
    for (int i = 0; i < n; i++) {
      float x = io[i] * inGain;
      float acc = 0.f;
      int16_t* base = cbuf_;
      for (int c = 0; c < NC; c++) {
        int16_t* b = base;
        float y = b[cpos_[c]];
        cfilt_[c] = y * (1.f - damp) + cfilt_[c] * damp;
        float w = x + cfilt_[c] * fb;
        b[cpos_[c]] = sat(w);
        if (++cpos_[c] >= kComb[c]) cpos_[c] = 0;
        acc += y;
        base += kComb[c];
      }
      int16_t* ab = abuf_;
      for (int a = 0; a < NA; a++) {
        float bo = ab[apos_[a]];
        float y = bo - acc;
        ab[apos_[a]] = sat(acc + bo * 0.5f);
        if (++apos_[a] >= kAll[a]) apos_[a] = 0;
        acc = y;
        ab += kAll[a];
      }
      float out = io[i] + acc * level * 6.f;
      io[i] = sat(out);
    }
  }

 private:
  static inline int16_t sat(float v) {
    return v > 32767.f ? 32767 : (v < -32768.f ? -32768 : (int16_t)v);
  }
  int16_t cbuf_[1116 + 1188 + 1277 + 1356 + 1422 + 1491];
  int16_t abuf_[556 + 441 + 341];
  int     cpos_[NC], apos_[NA];
  float   cfilt_[NC];
  bool    wasOn_ = false;
};
