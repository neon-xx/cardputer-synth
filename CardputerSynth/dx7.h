// dx7.h - 6-operator FM engine modelled on the Yamaha DX7.
// Loads standard DX7 SysEx (32-voice VMEM bulk dumps and single VCED voices).
#pragma once
#include "synth_common.h"

namespace syn {

// Operator parameters, same order and ranges as the DX7 VCED format.
struct Dx7Op {
  uint8_t r[4];    // EG rates 0-99
  uint8_t l[4];    // EG levels 0-99
  uint8_t bp;      // level scaling break point 0-99 (39 = C3)
  uint8_t ld, rd;  // left/right depth 0-99
  uint8_t lc, rc;  // curves 0-3 (-LIN,-EXP,+EXP,+LIN)
  uint8_t rs;      // rate scaling 0-7
  uint8_t ams;     // amp mod sens 0-3
  uint8_t kvs;     // key velocity sens 0-7
  uint8_t ol;      // output level 0-99
  uint8_t mode;    // 0 ratio, 1 fixed
  uint8_t coarse;  // 0-31
  uint8_t fine;    // 0-99
  uint8_t det;     // 0-14, 7 = centre
};

struct Dx7Patch {
  Dx7Op   op[6];   // op[0] = OP1 ... op[5] = OP6
  uint8_t pr[4], pl[4];  // pitch EG
  uint8_t alg;     // 0-31 (displayed 1-32)
  uint8_t fb;      // 0-7
  uint8_t oks;     // osc key sync 0/1
  uint8_t lfs, lfd, lpmd, lamd;  // LFO speed, delay, pitch depth, amp depth
  uint8_t lfks;    // LFO key sync
  uint8_t lfw;     // 0 tri 1 saw down 2 saw up 3 square 4 sine 5 S&H
  uint8_t lpms;    // pitch mod sens 0-7
  uint8_t trnsp;   // 0-48, 24 = no transpose
  char    name[11];
};

void dx7UnpackVmem(const uint8_t* src128, Dx7Patch& out);
void dx7PackVmem(const Dx7Patch& in, uint8_t* dst128);
void dx7UnpackVced(const uint8_t* src155, Dx7Patch& out);
void dx7Sanitize(Dx7Patch& p);

constexpr int DX7_PRESETS = 8;
extern const Dx7Patch kDx7Presets[DX7_PRESETS];

class Dx7Engine : public Engine {
 public:
  static constexpr int NV = 8;
  Dx7Engine();
  const char* engineName() const override { return "DX7"; }
  const char* patchName() const override { return patch.name; }
  void render(float* out, int n) override;
  void voiceOn(uint8_t note, uint8_t vel) override;
  void voiceOff(uint8_t note) override;
  void allSoundOff() override;
  void controlChange(uint8_t cc, uint8_t val) override;
  int  activeVoices() const override;
  bool shedVoice() override;
  void loadPatch(const Dx7Patch& p) { patch = p; dx7Sanitize(patch); }

  Dx7Patch patch;
  // Performance macros (not stored in the patch). 50 = neutral.
  uint8_t macroBright = 50;   // modulator output level offset
  uint8_t macroAttack = 50;   // EG rate 1 offset
  uint8_t macroRelease = 50;  // EG rate 4 offset
  uint8_t opEnable = 0x3F;    // bit n = OP(n+1) on

 private:
  struct Eg {
    float level = 0.f;   // DX7 internal log units (256 per 6 dB)
    float target[4];
    float inc[4];        // per-sample rate in level units
    int   stage = 4;     // 0..2 = attack/decay, 3 = release, 4 = idle/hold done
    bool  down = false;
    void  advance(int n);
    void  keyUp() { down = false; stage = 3; }
  };
  struct Op {
    uint32_t phase = 0;
    float    baseHz = 0.f;  // frequency before pitch mods
    bool     fixed = false;
    Eg       eg;
    float    amp = 0.f;     // amplitude at end of last sub-block (cycles of phase mod)
    float    y1 = 0.f, y2 = 0.f;  // feedback history
    uint8_t  ams = 0;
  };
  struct Voice {
    bool     active = false, gate = false, shed = false;
    uint8_t  note = 0;
    uint32_t age = 0;
    Op       op[6];
    // pitch EG
    float    peg = 0.f;  // semitones
    int      pegStage = 4;
    float    pegTarget[4], pegRate[4];
  };
  Voice    v_[NV];
  uint32_t ageCounter_ = 0;
  // global LFO
  float    lfoPh_ = 0.f, lfoSH_ = 0.f, lfoDelayT_ = 0.f;
  Rng      rng_;

  void startVoice(Voice& v, uint8_t note, uint8_t vel, bool retrig);
  void renderVoice(Voice& v, float* acc, int n, float lfoPitchSemis, float lfoAmpUni);
};

}  // namespace syn
