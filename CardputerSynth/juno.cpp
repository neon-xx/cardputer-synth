// juno.cpp - Juno-106 style engine.
// Signal path per voice: DCO (saw / pulse+PWM / sub / noise) -> 24 dB ladder VCF
// -> VCA (ADSR or gate). Global: triangle LFO with delay, HPF, BBD-style chorus.
#pragma GCC optimize("O2")  // DSP hot path: faster than the Arduino default -Os
#include "juno.h"

namespace syn {

// name, lfoRate, lfoDelay, range, dcoLfo, pwm, pwmMode, pulse, saw, sub, noise, hpf,
// cutoff, res, envPol, envAmt, vcfLfo, keyFlw, vcaMode, vcaLevel, A, D, S, R, chorus, velSens
const JunoPatch kJunoPresets[JUNO_PRESETS] = {
  {"Brass",     40,  0, 1,  0,  0, 1, 0, 1,   0, 0, 1, 38, 15, 0, 70,  0, 40, 0, 100,  25,  60,  75, 30, 1, 30},
  {"Strings",   45, 60, 1,  8, 70, 0, 1, 1,   0, 0, 2, 72, 10, 0, 15,  0, 50, 0, 100,  55,  70, 110, 65, 2, 20},
  {"Warm Pad",  30,  0, 1,  0, 80, 0, 1, 1,  40, 0, 1, 45, 20, 0, 35, 10, 40, 0,  85,  75,  90, 100, 85, 1, 20},
  {"Sub Bass",   0,  0, 0,  0,  0, 1, 0, 1, 110, 0, 1, 28, 45, 0, 75,  0, 60, 0, 110,   0,  48,  25, 18, 0, 50},
  {"Lead",      70, 70, 1, 10,  0, 1, 0, 1,  50, 0, 1, 70, 55, 0, 40,  0, 64, 0, 100,   5,  60,  90, 30, 1, 40},
  {"Pluck",     50,  0, 1,  0, 40, 1, 1, 1,   0, 0, 2, 34, 60, 0, 85,  0, 64, 0, 110,   0,  66,   0, 50, 2, 50},
  {"Organ",     60,  0, 1,  0,  0, 1, 1, 0,  90, 0, 1, 95,  0, 0,  0,  0, 40, 1,  55,   0,   0, 127, 10, 3,  0},
  {"Sweep Pad", 25,  0, 1,  0, 90, 0, 1, 1,  30, 8, 1, 22, 75, 0, 85, 25, 40, 0,  85, 100, 110,  40, 95, 2, 10},
};

static inline float polyBlep(float t, float dt) {
  if (t < dt) { float x = t / dt; return x + x - x * x - 1.f; }
  if (t > 1.f - dt) { float x = (t - 1.f) / dt; return x * x + x + x + 1.f; }
  return 0.f;
}

// Envelope coefficients shared by all voices for the current block.
static float s_attC, s_decC, s_relC, s_sus;

JunoEngine::JunoEngine() {
  patch = kJunoPresets[0];
  memset(chBuf_, 0, sizeof(chBuf_));
}

int JunoEngine::activeVoices() const {
  int c = 0;
  for (auto& v : v_) c += v.active;
  return c;
}

void JunoEngine::voiceOn(uint8_t note, uint8_t vel) {
  bool anyGate = false;
  for (auto& v : v_) anyGate |= v.gate;
  if (!anyGate) lfoDelayT_ = 0.f;  // LFO delay restarts after all keys were up

  int idx = -1;
  for (int i = 0; i < NV; i++) if (v_[i].active && v_[i].note == note) { idx = i; break; }
  if (idx < 0) {  // round-robin through free voices, like a real 106
    for (int k = 0; k < NV; k++) {
      int i = (rot_ + k) % NV;
      if (!v_[i].active) { idx = i; rot_ = (i + 1) % NV; break; }
    }
  }
  if (idx < 0) {  // steal: quietest released voice, else oldest
    float best = 1e9f;
    for (int i = 0; i < NV; i++) if (!v_[i].gate && v_[i].env < best) { best = v_[i].env; idx = i; }
    if (idx < 0) {
      uint32_t oldest = 0xFFFFFFFF;
      for (int i = 0; i < NV; i++) if (v_[i].age < oldest) { oldest = v_[i].age; idx = i; }
    }
  }
  Voice& v = v_[idx];
  if (!v.active) {
    v.env = 0.f;
    v.s1 = v.s2 = v.s3 = v.s4 = 0.f;
    v.ph = rng_.unipolar();  // free-running DCOs
    v.subPh = v.ph * 0.5f;
  }
  v.note = note;
  v.vel = vel / 127.f;
  v.gate = true;
  v.stage = 1;
  v.fastRel = false;
  v.active = true;
  v.age = ++ageCounter_;
}

bool JunoEngine::shedVoice() {
  int idx = -1;
  float best = 1e9f;
  for (int i = 0; i < NV; i++)
    if (v_[i].active && !v_[i].gate && !v_[i].fastRel && v_[i].env < best) { best = v_[i].env; idx = i; }
  if (idx < 0) return false;
  v_[idx].fastRel = true;
  v_[idx].stage = 3;
  return true;
}

void JunoEngine::voiceOff(uint8_t note) {
  for (auto& v : v_)
    if (v.active && v.gate && v.note == note) { v.gate = false; v.stage = 3; }
}

void JunoEngine::allSoundOff() {
  for (auto& v : v_) { v.active = false; v.gate = false; v.stage = 0; v.env = 0.f; }
}

void JunoEngine::controlChange(uint8_t cc, uint8_t val) {
  JunoPatch& p = patch;
  switch (cc) {
    case 71: p.res = val; break;
    case 72: p.R = val; break;
    case 73: p.A = val; break;
    case 74: p.cutoff = val; break;
    case 75: p.D = val; break;
    case 76: p.lfoRate = val; break;
    case 77: p.dcoLfo = val; break;
    case 78: p.lfoDelay = val; break;
    case 79: p.S = val; break;
    case 80: p.envAmt = val; break;
    case 81: p.pwm = val; break;
    case 82: p.vcfLfo = val; break;
    case 83: p.sub = val; break;
    case 84: p.noise = val; break;
    case 85: p.keyFlw = val; break;
    case 93: case 91: p.chorus = val >> 5; break;  // 0..3
    default: break;
  }
}

void JunoEngine::renderVoice(Voice& v, float* acc, int n, float lfo) {
  const JunoPatch& p = patch;

  // --- pitch / DCO ---
  float dcoDepth = 12.f * (p.dcoLfo / 127.f) * (p.dcoLfo / 127.f) + wheel_ * 0.6f;
  float pitch = v.note + (int)(p.dcoRange - 1) * 12 + bend_ + lfo * dcoDepth;
  float dt = clampf(midiToHz(pitch) / SRF, 1e-6f, 0.45f);
  float dtSub = dt * 0.5f;

  float pwAmt = p.pwm / 127.f;
  if (p.pwmMode == 0) pwAmt *= 0.5f + 0.5f * lfo;
  else if (p.pwmMode == 2) pwAmt *= v.env;
  float pw = 0.5f - 0.46f * pwAmt;

  const bool useSaw = p.saw, usePulse = p.pulse, useSub = p.sub > 0, useNoise = p.noise > 0;
  const float subG   = p.sub / 127.f;
  const float noiseG = p.noise / 127.f * 0.8f;

  // --- VCF coefficient for this sub-block ---
  float envSign = p.envPol ? -1.f : 1.f;
  float oct = p.cutoff / 127.f * 10.f
            + envSign * (p.envAmt / 127.f) * 8.f * v.env
            + (p.vcfLfo / 127.f) * 4.f * lfo
            + (p.keyFlw / 127.f) * (v.note - 60) / 12.f
            + (p.velSens / 127.f) * (v.vel - 0.7f) * 2.f;
  float fc = clampf(20.f * fastExp2(oct), 10.f, 0.42f * SRF);
  float g = tanf(3.14159265f * fc / SRF);
  float G = g / (1.f + g);
  float k = 4.1f * (p.res / 127.f);
  float G2 = G * G, G3 = G2 * G, G4 = G3 * G;
  float inv1g = 1.f / (1.f + g);
  float uNorm = 1.f / (1.f + k * G4);
  float comp = 1.f + 0.3f * k;  // partial pass-band loss compensation

  float vcaG = (p.vcaLevel / 127.f) * 0.85f * (1.f - (p.velSens / 127.f) * (1.f - v.vel));
  bool gateMode = p.vcaMode == 1;
  float gateTarget = v.gate ? 1.f : 0.f;

  float ph = v.ph, sph = v.subPh, env = v.env;
  float s1 = v.s1, s2 = v.s2, s3 = v.s3, s4 = v.s4;
  int stage = v.stage;
  float gEnv = v.gEnv;
  const float relC = v.fastRel ? 0.02f : s_relC;  // 0.02/sample = ~5 ms fade
  const float gateC = v.fastRel ? 0.02f : 0.01f;

  for (int i = 0; i < n; i++) {
    // envelope
    if (stage == 1) {
      env += (1.3f - env) * s_attC;
      if (env >= 1.f) { env = 1.f; stage = 2; }
    } else if (stage == 2) {
      env += (s_sus - env) * s_decC;
    } else if (stage == 3) {
      env -= env * relC;
    }
    gEnv += (gateTarget - gEnv) * gateC;
    float amp = gateMode ? gEnv : env;

    // oscillators
    float x = 0.f;
    float blep0 = polyBlep(ph, dt);
    if (useSaw) x += 2.f * ph - 1.f - blep0;
    if (usePulse) {
      float t2 = ph + 1.f - pw; if (t2 >= 1.f) t2 -= 1.f;
      x += (ph < pw ? 1.f : -1.f) + blep0 - polyBlep(t2, dt);
    }
    if (useSub) {
      float t3 = sph + 0.5f; if (t3 >= 1.f) t3 -= 1.f;
      x += ((sph < 0.5f ? 1.f : -1.f) + polyBlep(sph, dtSub) - polyBlep(t3, dtSub)) * subG;
    }
    if (useNoise) x += rng_.bipolar() * noiseG;
    ph += dt; if (ph >= 1.f) ph -= 1.f;
    sph += dtSub; if (sph >= 1.f) sph -= 1.f;

    // ladder (TPT, zero-delay feedback) with saturating input stage
    x *= 0.5f;
    float S = (G3 * s1 + G2 * s2 + G * s3 + s4) * inv1g;
    float u = (x - k * S) * uNorm;
    u = u > 1.5f ? 1.5f : (u < -1.5f ? -1.5f : u);
    u = u - 0.148148f * u * u * u;  // cubic soft clip, no division
    float vv, y;
    vv = (u - s1) * G;  y = vv + s1;  s1 = y + vv;
    vv = (y - s2) * G;  y = vv + s2;  s2 = y + vv;
    vv = (y - s3) * G;  y = vv + s3;  s3 = y + vv;
    vv = (y - s4) * G;  y = vv + s4;  s4 = y + vv;

    acc[i] += y * comp * amp * vcaG;
  }

  v.ph = ph; v.subPh = sph; v.env = env; v.stage = stage; v.gEnv = gEnv;
  v.s1 = s1; v.s2 = s2; v.s3 = s3; v.s4 = s4;
  if (stage == 3 && env < 3e-4f && (!gateMode || gEnv < 3e-4f)) {
    v.active = false; v.stage = 0; v.env = 0.f; v.fastRel = false;
  }
  if (!v.active) v.gEnv = 0.f;
}

void JunoEngine::render(float* out, int n) {
  for (int i = 0; i < n; i++) out[i] = 0.f;
  const JunoPatch& p = patch;

  // Envelope times (seconds): attack 1 ms..3 s, decay/release 2 ms..12 s
  float tA = 0.001f * powf(3000.f, p.A / 127.f);
  float tD = 0.002f * powf(6000.f, p.D / 127.f);
  float tR = 0.002f * powf(6000.f, p.R / 127.f);
  s_attC = 1.f - expf(-1.466f / (tA * SRF));
  s_decC = 1.f - expf(-4.6f / (tD * SRF));
  s_relC = 1.f - expf(-4.6f / (tR * SRF));
  s_sus  = p.S / 127.f;

  float lfoHz = 0.1f * fastExp2(p.lfoRate / 127.f * 8.2f);  // ~0.1..29 Hz
  float delaySec = 2.5f * p.lfoDelay / 127.f;

  for (int off = 0; off < n; off += SUB) {
    int m = (n - off) < SUB ? (n - off) : SUB;
    lfoPh_ += lfoHz * m / SRF;
    if (lfoPh_ >= 1.f) lfoPh_ -= 1.f;
    float tri = lfoPh_ < 0.5f ? 4.f * lfoPh_ - 1.f : 3.f - 4.f * lfoPh_;
    lfoDelayT_ += m / SRF;
    if (delaySec <= 0.001f) lfoAmp_ = 1.f;
    else lfoAmp_ = clampf((lfoDelayT_ - delaySec * 0.5f) / (delaySec * 0.5f), 0.f, 1.f);
    float lfo = tri * lfoAmp_;
    lfoVal_ = lfo;
    for (auto& v : v_) if (v.active) renderVoice(v, out + off, m, lfo);
  }

  // HPF (switch positions as on the 106)
  if (p.hpf == 0) {
    const float c = 1.f - expf(-6.2832f * 120.f / SRF);
    for (int i = 0; i < n; i++) { lsS_ += (out[i] - lsS_) * c; out[i] += 0.6f * lsS_; }
  } else if (p.hpf >= 2) {
    const float fc = p.hpf == 2 ? 240.f : 720.f;
    const float c = 1.f - expf(-6.2832f * fc / SRF);
    for (int i = 0; i < n; i++) { hpS_ += (out[i] - hpS_) * c; out[i] -= hpS_; }
  }

  // BBD chorus. Mono mix of the two (antiphase-modulated) delay taps, like
  // summing the Juno's L and R outputs.
  if (p.chorus) {
    float rate, center, depth;  // Hz, ms, ms
    if (p.chorus == 1)      { rate = 0.513f; center = 3.5f; depth = 1.85f; }
    else if (p.chorus == 2) { rate = 0.863f; center = 3.5f; depth = 1.85f; }
    else                    { rate = 9.75f;  center = 3.5f; depth = 0.2f;  }
    const float lpC = 1.f - expf(-6.2832f * 9000.f / SRF);
    for (int i = 0; i < n; i++) {
      chPh_ += rate / SRF;
      if (chPh_ >= 1.f) chPh_ -= 1.f;
      float tri = chPh_ < 0.5f ? 4.f * chPh_ - 1.f : 3.f - 4.f * chPh_;
      chLp_ += (out[i] - chLp_) * lpC;
      chBuf_[chW_] = chLp_;
      float dA = (center + depth * tri) * (SRF / 1000.f);
      float dB = (center - depth * tri) * (SRF / 1000.f);
      float rA = chW_ - dA; if (rA < 0) rA += CH_LEN;
      float rB = chW_ - dB; if (rB < 0) rB += CH_LEN;
      int iA = (int)rA, iB = (int)rB;
      float fA = rA - iA, fB = rB - iB;
      float a = chBuf_[iA] + (chBuf_[(iA + 1) & (CH_LEN - 1)] - chBuf_[iA]) * fA;
      float b = chBuf_[iB] + (chBuf_[(iB + 1) & (CH_LEN - 1)] - chBuf_[iB]) * fB;
      chW_ = (chW_ + 1) & (CH_LEN - 1);
      out[i] = 0.8f * out[i] + 0.45f * (a + b);
    }
  } else {
    // keep the delay line flowing so switching chorus on doesn't click
    for (int i = 0; i < n; i++) { chBuf_[chW_] = out[i]; chW_ = (chW_ + 1) & (CH_LEN - 1); }
  }
}

}  // namespace syn
