// synth_common.h - shared DSP helpers and the Engine base class.
// Pure C++ (no Arduino dependencies) so engines can be tested on a PC.
#pragma once
#include <stdint.h>
#include <math.h>
#include <string.h>

namespace syn {

constexpr int   SAMPLE_RATE = 48000;
constexpr float SRF         = 48000.0f;
constexpr int   BLOCK       = 128;   // samples per audio block handed to the codec
constexpr int   SUB         = 16;    // control-rate sub-block (envelopes, filter coefs)

constexpr int SINE_BITS = 11;
constexpr int SINE_SIZE = 1 << SINE_BITS;
extern float g_sine[SINE_SIZE + 1];
void initTables();

inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }

// 2^x, ~1e-5 relative accuracy, much faster than exp2f on the ESP32-S3.
inline float fastExp2(float x) {
  if (x < -60.f) return 0.f;
  if (x > 60.f) x = 60.f;
  float fl = floorf(x);
  float f  = x - fl;
  float p  = 1.0f + f * (0.6931472f + f * (0.2402265f + f * (0.0555041f + f * 0.0096181f)));
  union { float f; int32_t i; } u;
  u.i = ((int32_t)fl + 127) << 23;
  return p * u.f;
}

inline float midiToHz(float note) { return 440.0f * fastExp2((note - 69.0f) * (1.0f / 12.0f)); }

// Sine lookup, phase is a full 32-bit cycle, linear interpolation.
inline float sinLookup(uint32_t ph) {
  constexpr int SH = 32 - SINE_BITS;
  uint32_t idx = ph >> SH;
  float frac   = (float)(ph & ((1u << SH) - 1)) * (1.0f / (float)(1u << SH));
  float a      = g_sine[idx];
  return a + (g_sine[idx + 1] - a) * frac;
}

// Cheap saturator (Pade tanh approximation, exact clip at +-3).
inline float softClip(float x) {
  if (x > 3.f) return 1.f;
  if (x < -3.f) return -1.f;
  float x2 = x * x;
  return x * (27.f + x2) / (27.f + 9.f * x2);
}

struct Rng {
  uint32_t s = 0x12345678u;
  inline uint32_t next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
  inline float bipolar() { return (float)(int32_t)next() * (1.0f / 2147483648.0f); }
  inline float unipolar() { return (float)(next() >> 8) * (1.0f / 16777216.0f); }
};

// ---------------------------------------------------------------------------
// Engine base: handles sustain pedal bookkeeping; subclasses implement voices.
// ---------------------------------------------------------------------------
class Engine {
 public:
  virtual ~Engine() {}
  virtual const char* engineName() const = 0;
  virtual const char* patchName() const = 0;
  virtual void render(float* out, int n) = 0;  // writes n mono samples
  virtual void voiceOn(uint8_t note, uint8_t vel) = 0;
  virtual void voiceOff(uint8_t note) = 0;
  virtual void allSoundOff() = 0;
  virtual void controlChange(uint8_t cc, uint8_t val) { (void)cc; (void)val; }
  virtual int activeVoices() const = 0;
  // Quickly fade out the quietest released voice (used when the CPU falls behind).
  virtual bool shedVoice() { return false; }

  void noteOn(uint8_t note, uint8_t vel) {
    if (vel == 0) { noteOff(note); return; }
    held_[note] = true;
    sustained_[note] = false;
    voiceOn(note, vel);
  }
  void noteOff(uint8_t note) {
    held_[note] = false;
    if (sustain_) sustained_[note] = true;
    else voiceOff(note);
  }
  void setSustain(bool on) {
    sustain_ = on;
    if (!on) {
      for (int n = 0; n < 128; n++)
        if (sustained_[n]) { sustained_[n] = false; if (!held_[n]) voiceOff(n); }
    }
  }
  void allNotesOff() {
    for (int n = 0; n < 128; n++) { held_[n] = false; sustained_[n] = false; }
    sustain_ = false;
    allSoundOff();
  }
  void setPitchBend(float semis) { bend_ = semis; }
  void setModWheel(float v) { wheel_ = v; }
  bool anyKeyHeld() const {
    for (int n = 0; n < 128; n++) if (held_[n] || sustained_[n]) return true;
    return false;
  }

 protected:
  bool  held_[128] = {};
  bool  sustained_[128] = {};
  bool  sustain_ = false;
  float bend_ = 0.f;   // semitones
  float wheel_ = 0.f;  // 0..1
};

}  // namespace syn
