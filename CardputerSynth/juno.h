// juno.h - Juno-106 style 6-voice analog polysynth emulation.
#pragma once
#include "synth_common.h"

namespace syn {

// All values 0..127 unless noted. Layout is stable (stored in NVS as a blob).
struct JunoPatch {
  char    name[12];
  uint8_t lfoRate;
  uint8_t lfoDelay;
  uint8_t dcoRange;   // 0=16' 1=8' 2=4'
  uint8_t dcoLfo;     // LFO -> pitch
  uint8_t pwm;        // pulse width / PWM depth
  uint8_t pwmMode;    // 0=LFO 1=manual 2=env
  uint8_t pulse;      // 0/1
  uint8_t saw;        // 0/1
  uint8_t sub;        // sub osc level
  uint8_t noise;      // noise level
  uint8_t hpf;        // 0..3 (0 = bass boost, 1 = flat, 2/3 = high pass)
  uint8_t cutoff;
  uint8_t res;
  uint8_t envPol;     // 0=+ 1=-
  uint8_t envAmt;
  uint8_t vcfLfo;
  uint8_t keyFlw;
  uint8_t vcaMode;    // 0=env 1=gate
  uint8_t vcaLevel;
  uint8_t A, D, S, R;
  uint8_t chorus;     // 0=off 1=I 2=II 3=I+II
  uint8_t velSens;    // velocity -> VCA/VCF (not on a real 106, 0 = authentic)
};

constexpr int JUNO_PRESETS = 8;
extern const JunoPatch kJunoPresets[JUNO_PRESETS];

class JunoEngine : public Engine {
 public:
  static constexpr int NV = 6;
  JunoEngine();
  const char* engineName() const override { return "JUNO"; }
  const char* patchName() const override { return patch.name; }
  void render(float* out, int n) override;
  void voiceOn(uint8_t note, uint8_t vel) override;
  void voiceOff(uint8_t note) override;
  void allSoundOff() override;
  void controlChange(uint8_t cc, uint8_t val) override;
  int  activeVoices() const override;
  bool shedVoice() override;
  void loadPatch(const JunoPatch& p) { patch = p; }

  JunoPatch patch;

 private:
  struct Voice {
    bool     active = false, gate = false;
    uint8_t  note = 0;
    float    vel = 1.f;
    uint32_t age = 0;
    float    ph = 0.f, subPh = 0.f;
    float    env = 0.f, gEnv = 0.f;
    int      stage = 0;  // 0 idle 1 attack 2 decay/sustain 3 release
    bool     fastRel = false;  // being shed: ~5 ms fade
    float    s1 = 0, s2 = 0, s3 = 0, s4 = 0;  // ladder states
  };
  Voice    v_[NV];
  uint32_t ageCounter_ = 0;
  int      rot_ = 0;
  Rng      rng_;
  // LFO
  float lfoPh_ = 0.f, lfoVal_ = 0.f, lfoAmp_ = 0.f, lfoDelayT_ = 0.f;
  // post-processing
  float hpS_ = 0.f, lsS_ = 0.f;
  static constexpr int CH_LEN = 1024;
  float chBuf_[CH_LEN];
  int   chW_ = 0;
  float chPh_ = 0.f, chLp_ = 0.f;

  void renderVoice(Voice& v, float* acc, int n, float lfo);
};

}  // namespace syn
