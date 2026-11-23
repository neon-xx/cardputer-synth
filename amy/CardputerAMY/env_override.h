// env_override.h - ADSR overrides for AMY patches.
//
//  * Juno / custom banks: one main envelope on osc 0 -> replaced with an
//    absolute ADSR ("i1v0A<a>,1,<d>,<s>,<r>,0").
//  * DX7: every operator (osc 2..7) has its own envelope, which also shapes the
//    timbre, so A/D/R scale the operators' segment times and S scales their
//    sustain levels. The patch string is rewritten and stored as a user patch.
#pragma once
#include <string>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

namespace envov {

static constexpr uint8_t OFF = 128;  // "PATCH": no override

// Knob (0-127) -> absolute values
inline float attackMs(uint8_t v)  { return 1.f * powf(5000.f, v / 127.f); }   // 1 ms .. 5 s
inline float decayMs(uint8_t v)   { return 5.f * powf(2000.f, v / 127.f); }   // 5 ms .. 10 s
inline float sustainLv(uint8_t v) { return v / 127.f; }
// Knob (0-127) -> DX7 multiplier, 64 = x1, x0.1 .. x10
inline float dxMul(uint8_t v)     { return powf(10.f, (v - 64) / 64.f); }

// Absolute ADSR string for osc 0. Unset stages use neutral defaults.
inline std::string absoluteEnvMessage(const uint8_t env[4]) {
  float a = attackMs(env[0] == OFF ? 20 : env[0]);
  float d = decayMs(env[1] == OFF ? 70 : env[1]);
  float s = sustainLv(env[2] == OFF ? 90 : env[2]);
  float r = decayMs(env[3] == OFF ? 55 : env[3]);
  char buf[96];
  snprintf(buf, sizeof(buf), "i1v0A%d,1,%d,%.3f,%d,0Z", (int)a, (int)d, s, (int)r);
  return buf;
}

// Rewrite one breakpoint list "t0,v0,t1,v1,...,tr,vr" (last pair = release).
inline std::string scaleBreakpoints(const std::string& bp, float aMul, float dMul, float sMul, float rMul) {
  float vals[32];
  bool  set[32];
  int n = 0;
  size_t i = 0;
  while (n < 32) {
    size_t comma = bp.find(',', i);
    std::string tok = bp.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
    set[n] = !tok.empty();
    vals[n] = set[n] ? strtof(tok.c_str(), nullptr) : 0.f;
    n++;
    if (comma == std::string::npos) break;
    i = comma + 1;
  }
  int pairs = n / 2;
  // AMY's DX7 envelopes start with a zero-time "initial level" point; the
  // attack is the first segment after it.
  int attackPair = (pairs > 2 && set[0] && vals[0] == 0.f) ? 1 : 0;
  for (int p = 0; p < pairs; p++) {
    float& t = vals[2 * p];
    float& v = vals[2 * p + 1];
    if (!set[2 * p] || p < attackPair) continue;
    if (p == pairs - 1) t *= rMul;          // release
    else if (p == attackPair) t *= aMul;    // attack
    else t *= dMul;                         // decay segments
    if (p == pairs - 2 && set[2 * p + 1]) { // sustain level
      v *= sMul;
      if (v > 1.f) v = 1.f;
    }
  }
  std::string out;
  char tmp[24];
  for (int k = 0; k < n; k++) {
    if (k) out += ',';
    if (!set[k]) continue;
    if (k % 2 == 0) snprintf(tmp, sizeof(tmp), "%d", (int)(vals[k] + 0.5f));
    else snprintf(tmp, sizeof(tmp), "%g", vals[k]);
    out += tmp;
  }
  return out;
}

// Rewrite a DX7 patch string: scale the 'A' envelope of every operator osc
// (v2 and up; v0 is the ALGO osc whose envelope is the pitch EG, v1 the LFO).
// Calls emit(cmd) for each resulting osc command (without the trailing Z).
template <typename F>
void rewriteDx7(const char* patch, const uint8_t env[4], F emit) {
  float aMul = env[0] == OFF ? 1.f : dxMul(env[0]);
  float dMul = env[1] == OFF ? 1.f : dxMul(env[1]);
  float sMul = env[2] == OFF ? 1.f : dxMul(env[2]);
  float rMul = env[3] == OFF ? 1.f : dxMul(env[3]);
  std::string s(patch);
  size_t start = 0;
  while (start < s.size()) {
    size_t z = s.find('Z', start);
    std::string cmd = s.substr(start, z == std::string::npos ? std::string::npos : z - start);
    start = z == std::string::npos ? s.size() : z + 1;
    if (cmd.empty()) continue;
    int osc = (cmd[0] == 'v') ? atoi(cmd.c_str() + 1) : -1;
    size_t a = cmd.find('A');
    if (osc >= 2 && a != std::string::npos) {
      size_t e = a + 1;
      while (e < cmd.size() && (isdigit((unsigned char)cmd[e]) || cmd[e] == ',' || cmd[e] == '.' || cmd[e] == '-'))
        e++;
      cmd = cmd.substr(0, a + 1) + scaleBreakpoints(cmd.substr(a + 1, e - a - 1), aMul, dMul, sMul, rMul) + cmd.substr(e);
    }
    emit(cmd);
  }
}

}  // namespace envov
