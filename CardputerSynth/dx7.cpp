// dx7.cpp - 6-operator FM engine.
// Envelope, level and rate scaling follow the DX7's integer log-domain model
// (256 units per 6 dB), so patches loaded from real DX7 SysEx banks behave
// close to the original. Not bit-exact; LFO/pitch-EG curves are approximations.
#pragma GCC optimize("O2")  // DSP hot path: faster than the Arduino default -Os
#include "dx7.h"

namespace syn {

// ---------------------------------------------------------------------------
// Algorithms. Edges "a>b" = OP a modulates OP b. All edges go from a higher to
// a lower operator, so evaluating OP6..OP1 in order is always valid.
// fbSrc/fbDst: feedback loop (usually one op feeding itself).
// ---------------------------------------------------------------------------
struct AlgDef { const char* edges; const char* carriers; uint8_t fbSrc, fbDst; };
static const AlgDef kAlgDefs[32] = {
  {"2>1 4>3 5>4 6>5", "13", 6, 6},        // 1
  {"2>1 4>3 5>4 6>5", "13", 2, 2},        // 2
  {"2>1 3>2 5>4 6>5", "14", 6, 6},        // 3
  {"2>1 3>2 5>4 6>5", "14", 4, 6},        // 4  (loop 4->6)
  {"2>1 4>3 6>5", "135", 6, 6},           // 5
  {"2>1 4>3 6>5", "135", 5, 6},           // 6  (loop 5->6)
  {"2>1 4>3 5>3 6>5", "13", 6, 6},        // 7
  {"2>1 4>3 5>3 6>5", "13", 4, 4},        // 8
  {"2>1 4>3 5>3 6>5", "13", 2, 2},        // 9
  {"2>1 3>2 5>4 6>4", "14", 3, 3},        // 10
  {"2>1 3>2 5>4 6>4", "14", 6, 6},        // 11
  {"2>1 4>3 5>3 6>3", "13", 2, 2},        // 12
  {"2>1 4>3 5>3 6>3", "13", 6, 6},        // 13
  {"2>1 4>3 5>4 6>4", "13", 6, 6},        // 14
  {"2>1 4>3 5>4 6>4", "13", 2, 2},        // 15
  {"2>1 3>1 4>3 5>1 6>5", "1", 6, 6},     // 16
  {"2>1 3>1 4>3 5>1 6>5", "1", 2, 2},     // 17
  {"2>1 3>1 4>1 5>4 6>5", "1", 3, 3},     // 18
  {"2>1 3>2 6>4 6>5", "145", 6, 6},       // 19
  {"3>1 3>2 5>4 6>4", "124", 3, 3},       // 20
  {"3>1 3>2 6>4 6>5", "1245", 3, 3},      // 21
  {"2>1 6>3 6>4 6>5", "1345", 6, 6},      // 22
  {"3>2 6>4 6>5", "1245", 6, 6},          // 23
  {"6>3 6>4 6>5", "12345", 6, 6},         // 24
  {"6>4 6>5", "12345", 6, 6},             // 25
  {"3>2 5>4 6>4", "124", 6, 6},           // 26
  {"3>2 5>4 6>4", "124", 3, 3},           // 27
  {"2>1 4>3 5>4", "136", 5, 5},           // 28
  {"4>3 6>5", "1235", 6, 6},              // 29
  {"4>3 5>4", "1236", 5, 5},              // 30
  {"6>5", "12345", 6, 6},                 // 31
  {"", "123456", 6, 6},                   // 32
};

struct Alg {
  uint8_t mods[6][3];
  uint8_t nmods[6];
  uint8_t carriers[6];
  uint8_t ncar;
  uint8_t fbSrc, fbDst;  // 0-based
};
static Alg s_alg[32];
static bool s_algInit = false;

static void initAlgs() {
  for (int a = 0; a < 32; a++) {
    Alg& g = s_alg[a];
    memset(&g, 0, sizeof(g));
    for (const char* e = kAlgDefs[a].edges; *e; ) {
      if (e[0] >= '1' && e[0] <= '6' && e[1] == '>') {
        int from = e[0] - '1', to = e[2] - '1';
        if (g.nmods[to] < 3) g.mods[to][g.nmods[to]++] = from;
        e += 3;
      } else e++;
    }
    for (const char* c = kAlgDefs[a].carriers; *c; c++) g.carriers[g.ncar++] = *c - '1';
    g.fbSrc = kAlgDefs[a].fbSrc - 1;
    g.fbDst = kAlgDefs[a].fbDst - 1;
  }
  s_algInit = true;
}

// ---------------------------------------------------------------------------
// Scaling helpers (DX7 integer model)
// ---------------------------------------------------------------------------
static int scaleOutLevel(int l) {
  static const uint8_t lo[20] = {0, 5, 9, 13, 17, 20, 23, 25, 27, 29, 31, 33, 35, 37, 39, 41, 42, 43, 45, 46};
  if (l < 0) l = 0;
  if (l > 99) l = 99;
  return l >= 20 ? 28 + l : lo[l];
}

static int scaleCurve(int group, int depth, int curve) {
  int scale;
  if (curve == 0 || curve == 3) {
    scale = (group * depth * 329) >> 12;
  } else {
    if (group > 32) group = 32;
    float raw = 250.f * (powf(2.f, group / 5.f) - 1.f) / (powf(2.f, 6.4f) - 1.f);
    scale = ((int)raw * depth * 329) >> 15;
  }
  return curve < 2 ? -scale : scale;
}

static int scaleLevel(int note, const Dx7Op& o) {
  int offset = note - o.bp - 21;
  if (offset >= 0) return scaleCurve((offset + 1) / 3, o.rd, o.rc);
  return scaleCurve((-(offset - 1)) / 3, o.ld, o.lc);
}

static int scaleVelocity(int vel, int sens) {
  float v = vel / 127.f;
  int curve = (int)(254.f * powf(v, 0.4f));
  int val = curve - 239;
  return ((sens * val + 7) >> 3) << 4;
}

static int scaleRate(int note, int sens) {
  int x = note / 3 - 7;
  if (x < 0) x = 0;
  if (x > 31) x = 31;
  return (sens * x) >> 3;
}

static float egIncFor(int rate, int rateScale) {
  int q = (rate * 41) >> 6;
  q += rateScale;
  if (q > 63) q = 63;
  return (4 + (q & 3)) * (float)(1u << (2 + (q >> 2))) / 65536.f * (44100.f / SRF);
}

void DSP_HOT Dx7Engine::Eg::advance(int n) {
  for (int guard = 0; guard < 4; guard++) {
    if (stage == 4 || stage == 5) return;  // done / sustaining
    float tgt = target[stage];
    if (level < tgt) {
      float jump = tgt < 1716.f ? tgt : 1716.f;
      if (level < jump) level = jump;
      float mul = (4352.f - level) * (1.f / 256.f);
      if (mul < 1.f) mul = 1.f;
      level += mul * inc[stage] * n;
      if (level < tgt) return;
      level = tgt;
    } else if (level > tgt) {
      level -= inc[stage] * n;
      if (level > tgt) return;
      level = tgt;
    }
    // reached target -> next stage
    if (stage < 2) stage++;
    else if (stage == 2) { stage = down ? 5 : 3; }
    else stage = 4;
    n = 0;  // continue only for zero-length transitions
  }
}

// ---------------------------------------------------------------------------
// SysEx packing
// ---------------------------------------------------------------------------
void dx7Sanitize(Dx7Patch& p) {
  auto c = [](uint8_t& x, int mx) { if (x > mx) x = mx; };
  for (auto& o : p.op) {
    for (int i = 0; i < 4; i++) { c(o.r[i], 99); c(o.l[i], 99); }
    c(o.bp, 99); c(o.ld, 99); c(o.rd, 99); c(o.lc, 3); c(o.rc, 3); c(o.rs, 7);
    c(o.ams, 3); c(o.kvs, 7); c(o.ol, 99); c(o.mode, 1); c(o.coarse, 31); c(o.fine, 99); c(o.det, 14);
  }
  for (int i = 0; i < 4; i++) { c(p.pr[i], 99); c(p.pl[i], 99); }
  c(p.alg, 31); c(p.fb, 7); c(p.oks, 1); c(p.lfs, 99); c(p.lfd, 99); c(p.lpmd, 99); c(p.lamd, 99);
  c(p.lfks, 1); c(p.lfw, 5); c(p.lpms, 7); c(p.trnsp, 48);
  for (int i = 0; i < 10; i++) if (p.name[i] < 32 || p.name[i] > 126) p.name[i] = ' ';
  p.name[10] = 0;
}

void dx7UnpackVmem(const uint8_t* d, Dx7Patch& p) {
  for (int i = 0; i < 6; i++) {
    const uint8_t* s = d + i * 17;
    Dx7Op& o = p.op[5 - i];  // VMEM stores OP6 first
    for (int k = 0; k < 4; k++) { o.r[k] = s[k] & 0x7F; o.l[k] = s[4 + k] & 0x7F; }
    o.bp = s[8] & 0x7F; o.ld = s[9] & 0x7F; o.rd = s[10] & 0x7F;
    o.lc = s[11] & 3; o.rc = (s[11] >> 2) & 3;
    o.rs = s[12] & 7; o.det = (s[12] >> 3) & 15;
    o.ams = s[13] & 3; o.kvs = (s[13] >> 2) & 7;
    o.ol = s[14] & 0x7F;
    o.mode = s[15] & 1; o.coarse = (s[15] >> 1) & 31;
    o.fine = s[16] & 0x7F;
  }
  for (int k = 0; k < 4; k++) { p.pr[k] = d[102 + k] & 0x7F; p.pl[k] = d[106 + k] & 0x7F; }
  p.alg = d[110] & 31;
  p.fb = d[111] & 7; p.oks = (d[111] >> 3) & 1;
  p.lfs = d[112] & 0x7F; p.lfd = d[113] & 0x7F; p.lpmd = d[114] & 0x7F; p.lamd = d[115] & 0x7F;
  p.lfks = d[116] & 1; p.lfw = (d[116] >> 1) & 7; p.lpms = (d[116] >> 4) & 7;
  p.trnsp = d[117] & 0x7F;
  for (int k = 0; k < 10; k++) p.name[k] = (char)(d[118 + k] & 0x7F);
  p.name[10] = 0;
  dx7Sanitize(p);
}

void dx7PackVmem(const Dx7Patch& p, uint8_t* d) {
  memset(d, 0, 128);
  for (int i = 0; i < 6; i++) {
    uint8_t* s = d + i * 17;
    const Dx7Op& o = p.op[5 - i];
    for (int k = 0; k < 4; k++) { s[k] = o.r[k]; s[4 + k] = o.l[k]; }
    s[8] = o.bp; s[9] = o.ld; s[10] = o.rd;
    s[11] = (o.lc & 3) | ((o.rc & 3) << 2);
    s[12] = (o.rs & 7) | ((o.det & 15) << 3);
    s[13] = (o.ams & 3) | ((o.kvs & 7) << 2);
    s[14] = o.ol;
    s[15] = (o.mode & 1) | ((o.coarse & 31) << 1);
    s[16] = o.fine;
  }
  for (int k = 0; k < 4; k++) { d[102 + k] = p.pr[k]; d[106 + k] = p.pl[k]; }
  d[110] = p.alg & 31;
  d[111] = (p.fb & 7) | ((p.oks & 1) << 3);
  d[112] = p.lfs; d[113] = p.lfd; d[114] = p.lpmd; d[115] = p.lamd;
  d[116] = (p.lfks & 1) | ((p.lfw & 7) << 1) | ((p.lpms & 7) << 4);
  d[117] = p.trnsp;
  for (int k = 0; k < 10; k++) d[118 + k] = (uint8_t)(p.name[k] ? p.name[k] : ' ') & 0x7F;
}

void dx7UnpackVced(const uint8_t* d, Dx7Patch& p) {
  for (int i = 0; i < 6; i++) {
    const uint8_t* s = d + i * 21;
    Dx7Op& o = p.op[5 - i];
    for (int k = 0; k < 4; k++) { o.r[k] = s[k]; o.l[k] = s[4 + k]; }
    o.bp = s[8]; o.ld = s[9]; o.rd = s[10]; o.lc = s[11]; o.rc = s[12]; o.rs = s[13];
    o.ams = s[14]; o.kvs = s[15]; o.ol = s[16]; o.mode = s[17]; o.coarse = s[18];
    o.fine = s[19]; o.det = s[20];
  }
  const uint8_t* g = d + 126;
  for (int k = 0; k < 4; k++) { p.pr[k] = g[k]; p.pl[k] = g[4 + k]; }
  p.alg = g[8]; p.fb = g[9]; p.oks = g[10]; p.lfs = g[11]; p.lfd = g[12];
  p.lpmd = g[13]; p.lamd = g[14]; p.lfks = g[15]; p.lfw = g[16]; p.lpms = g[17];
  p.trnsp = g[18];
  for (int k = 0; k < 10; k++) p.name[k] = (char)g[19 + k];
  p.name[10] = 0;
  dx7Sanitize(p);
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------
Dx7Engine::Dx7Engine() {
  if (!s_algInit) initAlgs();
  patch = kDx7Presets[0];
}

int Dx7Engine::activeVoices() const {
  int c = 0;
  for (auto& v : v_) c += v.active;
  return c;
}

static float pegLevelToSemis(int l) {
  float v = (l - 50) / 50.f;
  return 48.f * v * fabsf(v);
}
static float pegRateToSemisPerSample(int r) {
  return 12.f * 0.05f * fastExp2(r / 10.f) / SRF;
}

void Dx7Engine::startVoice(Voice& v, uint8_t note, uint8_t vel, bool retrig) {
  const Dx7Patch& p = patch;
  int n = note + (int)p.trnsp - 24;
  if (n < 0) n = 0;
  if (n > 127) n = 127;
  float noteHz = midiToHz((float)n);
  float lf = log2f(noteHz);
  const Alg& alg = s_alg[p.alg];
  bool isCarrier[6] = {};
  for (int c = 0; c < alg.ncar; c++) isCarrier[alg.carriers[c]] = true;

  for (int i = 0; i < 6; i++) {
    const Dx7Op& o = p.op[i];
    Op& op = v.op[i];
    // frequency
    if (o.mode == 0) {
      float ratio = (o.coarse == 0 ? 0.5f : (float)o.coarse) * (1.f + o.fine * 0.01f);
      float dr = 0.0209f * expf(-0.396f * lf) / 7.f;
      float detune = fastExp2(dr * lf * ((int)o.det - 7));
      op.baseHz = noteHz * ratio * detune;
      op.fixed = false;
    } else {
      op.baseHz = powf(10.f, (float)(o.coarse & 3) + o.fine * 0.01f);
      op.fixed = true;
    }
    // output level incl. key scaling, velocity, brightness macro
    int ol = o.ol;
    if (!isCarrier[i]) ol += (int)((macroBright - 50) * 0.6f);
    if (ol < 0) ol = 0;
    if (ol > 99) ol = 99;
    int outlevel = scaleOutLevel(ol) + scaleLevel(n, o);
    if (outlevel > 127) outlevel = 127;
    if (outlevel < 0) outlevel = 0;
    outlevel = (outlevel << 5) + scaleVelocity(vel, o.kvs);
    if (outlevel < 0) outlevel = 0;
    int rs = scaleRate(n, o.rs);
    Eg& eg = op.eg;
    for (int s = 0; s < 4; s++) {
      int t = (scaleOutLevel(o.l[s]) >> 1) * 64 + outlevel - 4256;
      eg.target[s] = (float)(t < 16 ? 16 : t);
      int r = o.r[s];
      if (s == 0) r -= (int)((macroAttack - 50) * 1.2f);
      if (s == 3) r -= (int)((macroRelease - 50) * 1.2f);
      if (r < 0) r = 0;
      if (r > 99) r = 99;
      eg.inc[s] = egIncFor(r, rs);
    }
    // a voice that's still sounding (retrigger or steal) restarts its EGs from the
    // current level, so there's no click
    if (!v.active) { eg.level = 0.f; op.amp = 0.f; op.y1 = op.y2 = 0.f; }
    (void)retrig;
    eg.stage = 0;
    eg.down = true;
    op.ams = o.ams;
    if (p.oks) op.phase = 0;
  }
  // pitch EG
  for (int s = 0; s < 4; s++) {
    v.pegTarget[s] = pegLevelToSemis(p.pl[s]);
    v.pegRate[s] = pegRateToSemisPerSample(p.pr[s]);
  }
  if (!v.active) v.peg = v.pegTarget[3];
  v.shed = false;
  v.pegStage = 0;
}

void Dx7Engine::voiceOn(uint8_t note, uint8_t vel) {
  bool anyGate = false;
  for (auto& v : v_) anyGate |= v.gate;
  if (patch.lfks) lfoPh_ = 0.f;
  if (!anyGate) lfoDelayT_ = 0.f;

  int idx = -1;
  for (int i = 0; i < NV; i++) if (v_[i].active && v_[i].note == note) { idx = i; break; }
  if (idx < 0) for (int i = 0; i < NV; i++) if (!v_[i].active) { idx = i; break; }
  if (idx < 0) {
    // steal the quietest released voice, else the oldest
    float best = 1e9f;
    for (int i = 0; i < NV; i++) {
      if (v_[i].gate) continue;
      float lvl = 0;
      for (int k = 0; k < 6; k++) lvl = fmaxf(lvl, v_[i].op[k].eg.level);
      if (lvl < best) { best = lvl; idx = i; }
    }
    if (idx < 0) {
      uint32_t oldest = 0xFFFFFFFF;
      for (int i = 0; i < NV; i++) if (v_[i].age < oldest) { oldest = v_[i].age; idx = i; }
    }
  }
  Voice& v = v_[idx];
  bool retrig = v.active && v.note == note;
  startVoice(v, note, vel, retrig);
  v.note = note;
  v.gate = true;
  v.active = true;
  v.age = ++ageCounter_;
}

bool Dx7Engine::shedVoice() {
  int idx = -1;
  float best = 1e9f;
  for (int i = 0; i < NV; i++) {
    Voice& v = v_[i];
    if (!v.active || v.gate || v.shed) continue;
    float lvl = 0;
    for (int k = 0; k < 6; k++) lvl = fmaxf(lvl, v.op[k].eg.level);
    if (lvl < best) { best = lvl; idx = i; }
  }
  if (idx < 0) return false;
  Voice& v = v_[idx];
  v.shed = true;
  for (auto& op : v.op) {  // fastest DX7 release (~6 ms) down to silence
    op.eg.inc[3] = egIncFor(99, 0) * 2.f;
    op.eg.target[3] = 16.f;
    op.eg.stage = 3;
  }
  return true;
}

void Dx7Engine::voiceOff(uint8_t note) {
  for (auto& v : v_) {
    if (v.active && v.gate && v.note == note) {
      v.gate = false;
      for (auto& op : v.op) op.eg.keyUp();
      v.pegStage = 3;
    }
  }
}

void Dx7Engine::allSoundOff() {
  for (auto& v : v_) { v.active = false; v.gate = false; }
}

void Dx7Engine::controlChange(uint8_t cc, uint8_t val) {
  switch (cc) {
    case 74: macroBright = (uint8_t)(val * 100 / 127); break;
    case 73: macroAttack = (uint8_t)(val * 100 / 127); break;
    case 72: macroRelease = (uint8_t)(val * 100 / 127); break;
    case 76: patch.lfs = (uint8_t)(val * 99 / 127); break;
    default: break;
  }
}

static const float kPmsSemis[8] = {0.f, 0.15f, 0.3f, 0.5f, 0.8f, 1.4f, 2.8f, 12.f};
static const float kAmsUnits[4] = {0.f, 170.f, 450.f, 1200.f};

void DSP_HOT Dx7Engine::renderVoice(Voice& v, float* acc, int n, float lfoPitch, float lfoAmp) {
  const Alg& alg = s_alg[patch.alg];
  const float amd = patch.lamd / 99.f * lfoAmp;

  // pitch EG (per sub-block)
  if (v.pegStage < 4) {
    float tgt = v.pegTarget[v.pegStage];
    float step = v.pegRate[v.pegStage] * n;
    if (v.peg < tgt) { v.peg += step; if (v.peg >= tgt) v.peg = tgt; }
    else if (v.peg > tgt) { v.peg -= step; if (v.peg <= tgt) v.peg = tgt; }
    if (v.peg == tgt) {
      if (v.pegStage < 2) v.pegStage++;
      else if (v.pegStage == 2) v.pegStage = v.gate ? 5 : 3;
      else v.pegStage = 4;
    }
  }
  float pitchMul = fastExp2((bend_ + lfoPitch + v.peg) * (1.f / 12.f));

  uint32_t inc[6];
  float amp[6], dAmp[6];
  const float invN = 1.f / n;
  for (int i = 0; i < 6; i++) {
    Op& op = v.op[i];
    float hz = op.fixed ? op.baseHz : op.baseHz * pitchMul;
    float f = hz / SRF;
    if (f > 0.49f) f = 0.49f;
    inc[i] = (uint32_t)(f * 4294967296.f);
    op.eg.advance(n);
    float lvl = op.eg.level - amd * kAmsUnits[op.ams];
    float a = (opEnable >> i) & 1 ? fastExp2(lvl * (1.f / 256.f) - 14.f) : 0.f;
    if (lvl < 20.f) a = 0.f;
    amp[i] = op.amp;
    dAmp[i] = (a - op.amp) * invN;
    op.amp = a;
  }

  const int fbSrc = alg.fbSrc, fbDst = alg.fbDst;
  const float fbScale = patch.fb ? fastExp2((float)patch.fb - 9.f) : 0.f;
  float y1 = v.op[fbSrc].y1, y2 = v.op[fbSrc].y2;
  uint32_t ph[6];
  for (int i = 0; i < 6; i++) ph[i] = v.op[i].phase;

  for (int s = 0; s < n; s++) {
    float o[6];
    for (int i = 5; i >= 0; i--) {
      float mod = 0.f;
      for (int j = 0; j < alg.nmods[i]; j++) mod += o[alg.mods[i][j]];
      if (i == fbDst) mod += (y1 + y2) * fbScale;
      ph[i] += inc[i];
      uint32_t pm = ((uint32_t)(int32_t)(mod * 1048576.f)) << 12;
      o[i] = sinLookup(ph[i] + pm) * amp[i];
      amp[i] += dAmp[i];
    }
    y2 = y1;
    y1 = o[fbSrc];
    float sum = 0.f;
    for (int c = 0; c < alg.ncar; c++) sum += o[alg.carriers[c]];
    acc[s] += sum * 0.5f;
  }
  for (int i = 0; i < 6; i++) v.op[i].phase = ph[i];
  v.op[fbSrc].y1 = y1;
  v.op[fbSrc].y2 = y2;

  // free the voice once every carrier has died away after key-up
  if (!v.gate) {
    bool silent = true;
    for (int c = 0; c < alg.ncar; c++) {
      const Op& op = v.op[alg.carriers[c]];
      if (op.eg.level > 300.f && ((opEnable >> alg.carriers[c]) & 1)) { silent = false; break; }
    }
    if (silent) v.active = false;
  }
}

void DSP_HOT Dx7Engine::render(float* out, int n) {
  for (int i = 0; i < n; i++) out[i] = 0.f;
  const Dx7Patch& p = patch;
  float lfoHz = 0.062f * fastExp2(p.lfs * 0.0975f);
  float delay = 5.8f * (p.lfd / 99.f) * (p.lfd / 99.f);

  for (int off = 0; off < n; off += SUB) {
    int m = (n - off) < SUB ? (n - off) : SUB;
    float prev = lfoPh_;
    lfoPh_ += lfoHz * m / SRF;
    if (lfoPh_ >= 1.f) lfoPh_ -= 1.f;
    if (lfoPh_ < prev) lfoSH_ = rng_.bipolar();
    float ph = lfoPh_, lfo;
    switch (p.lfw) {
      case 0: lfo = ph < 0.5f ? 4.f * ph - 1.f : 3.f - 4.f * ph; break;
      case 1: lfo = 1.f - 2.f * ph; break;
      case 2: lfo = 2.f * ph - 1.f; break;
      case 3: lfo = ph < 0.5f ? 1.f : -1.f; break;
      case 4: lfo = sinLookup((uint32_t)(ph * 4294967295.f)); break;
      default: lfo = lfoSH_; break;
    }
    lfoDelayT_ += m / SRF;
    float fade = delay < 0.01f ? 1.f : clampf((lfoDelayT_ - delay) / (0.5f * delay + 0.01f), 0.f, 1.f);
    int pms = p.lpms;
    float pitchSemis = lfo * (fade * p.lpmd / 99.f * kPmsSemis[pms] + wheel_ * kPmsSemis[pms < 3 ? 3 : pms]);
    float ampUni = (lfo + 1.f) * 0.5f * fade;
    for (auto& v : v_) if (v.active) renderVoice(v, out + off, m, pitchSemis, ampUni);
  }
  for (int i = 0; i < n; i++) out[i] *= 0.14f;
}

}  // namespace syn
