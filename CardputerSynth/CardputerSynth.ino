// CardputerSynth - Juno-106 and DX7 style synthesizers for the M5Stack Cardputer-Adv,
// played from a MIDI keyboard through the M5Stack Unit MIDI (U187) on the Grove port.
//
// Hardware
//   Cardputer-Adv (ESP32-S3, ES8311 codec -> speaker + 3.5 mm jack)
//   Unit MIDI on the Grove port, DIP switch set to BYPASS (so the host can read MIDI IN).
//   MIDI keyboard -> Unit MIDI "IN" DIN/TRS socket.
//
// Libraries: M5Cardputer >= 1.1.0, M5Unified >= 0.2.8, M5GFX >= 0.2.10
// Board:     M5Cardputer (M5Stack board package >= 3.2.2)

#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <Preferences.h>
#include "juno.h"
#include "dx7.h"
#include "midi_parser.h"
#include "ui_param.h"

using namespace syn;

// ---------------------------------------------------------------------------
// Pins
// ---------------------------------------------------------------------------
static constexpr int MIDI_RX_PIN = 1;  // Grove white  = Unit MIDI TX (MIDI IN)
static constexpr int MIDI_TX_PIN = 2;  // Grove yellow = Unit MIDI RX (to its SAM2695, unused)
static constexpr int SD_SCK = 40, SD_MISO = 39, SD_MOSI = 14, SD_CS = 12;

// ---------------------------------------------------------------------------
// Engines and shared state
// ---------------------------------------------------------------------------
static JunoEngine juno;
static Dx7Engine  dx7;
static Engine* volatile g_engine = &juno;

struct Settings {
  uint8_t midiCh = 0;      // 0 = omni, 1..16
  uint8_t bendRange = 2;   // semitones
  uint8_t volume = 170;    // codec volume 0..255
  uint8_t engine = 0;      // 0 juno, 1 dx7
  uint8_t rotation = 0xFF; // display rotation 0-3, 0xFF = auto (landscape)
};
static Settings g_set;

enum EvType : uint8_t { EV_NOTE_ON, EV_NOTE_OFF, EV_ENGINE, EV_JUNO_PATCH, EV_DX7_PATCH, EV_PANIC };
struct Ev { uint8_t type, a, b; };
static QueueHandle_t g_evq;

static portMUX_TYPE g_patchMux = portMUX_INITIALIZER_UNLOCKED;
static JunoPatch g_pendingJuno;
static Dx7Patch  g_pendingDx7;

static volatile uint32_t g_midiActivityMs = 0;
static volatile float    g_cpu = 0.f;
static volatile int      g_pendingProgram = -1;
static volatile bool     g_dumpReady = false;       // 32-voice dump received over MIDI
static Dx7Patch          g_dumpBank[32];
static volatile bool     g_singleVoiceRx = false;   // single DX7 voice received

// ---------------------------------------------------------------------------
// MIDI handling (runs inside the audio task, so it can touch engines directly)
// ---------------------------------------------------------------------------
struct MidiRouter : MidiParser::Handler {
  bool chOk(uint8_t ch) { return g_set.midiCh == 0 || ch == g_set.midiCh - 1; }
  void onNoteOn(uint8_t ch, uint8_t n, uint8_t v) override {
    if (!chOk(ch)) return;
    g_engine->noteOn(n, v);
    g_midiActivityMs = millis();
  }
  void onNoteOff(uint8_t ch, uint8_t n) override {
    if (!chOk(ch)) return;
    g_engine->noteOff(n);
    g_midiActivityMs = millis();
  }
  void onCC(uint8_t ch, uint8_t cc, uint8_t v) override {
    if (!chOk(ch)) return;
    g_midiActivityMs = millis();
    switch (cc) {
      case 1:   g_engine->setModWheel(v / 127.f); break;
      case 64:  g_engine->setSustain(v >= 64); break;
      case 7:   g_set.volume = v * 2; M5.Speaker.setVolume(g_set.volume); break;
      case 120: case 123: g_engine->allNotesOff(); break;
      default:  g_engine->controlChange(cc, v); break;
    }
  }
  void onProgram(uint8_t ch, uint8_t p) override {
    if (!chOk(ch)) return;
    g_pendingProgram = p;  // handled by the UI task (it owns banks/files)
  }
  void onBend(uint8_t ch, int value) override {
    if (!chOk(ch)) return;
    g_engine->setPitchBend(value / 8192.f * g_set.bendRange);
  }
  void onSysEx(const uint8_t* d, int len) override {
    g_midiActivityMs = millis();
    if (len < 8 || d[1] != 0x43) return;  // Yamaha only
    if (len >= 4104 && d[3] == 0x09) {     // 32-voice bulk dump (VMEM)
      for (int i = 0; i < 32; i++) dx7UnpackVmem(d + 6 + i * 128, g_dumpBank[i]);
      g_dumpReady = true;
    } else if (len >= 163 && d[3] == 0x00) {  // single voice (VCED)
      Dx7Patch p;
      dx7UnpackVced(d + 6, p);
      dx7.allNotesOff();
      dx7.loadPatch(p);
      if (g_engine != &dx7) { g_engine->allNotesOff(); g_engine = &dx7; g_set.engine = 1; }
      g_singleVoiceRx = true;
    }
  }
};
static MidiRouter g_router;
static MidiParser g_midi(g_router);

// ---------------------------------------------------------------------------
// Audio task: MIDI in -> engine -> int16 blocks -> M5Unified speaker (ES8311)
// ---------------------------------------------------------------------------
static void handleEvent(const Ev& e) {
  switch (e.type) {
    case EV_NOTE_ON:  g_engine->noteOn(e.a, e.b); break;
    case EV_NOTE_OFF: g_engine->noteOff(e.a); break;
    case EV_PANIC:    juno.allNotesOff(); dx7.allNotesOff(); break;
    case EV_ENGINE: {
      Engine* next = e.a ? (Engine*)&dx7 : (Engine*)&juno;
      if (next != g_engine) { g_engine->allNotesOff(); g_engine = next; }
      break;
    }
    case EV_JUNO_PATCH:
      portENTER_CRITICAL(&g_patchMux);
      juno.loadPatch(g_pendingJuno);
      portEXIT_CRITICAL(&g_patchMux);
      break;
    case EV_DX7_PATCH:
      portENTER_CRITICAL(&g_patchMux);
      dx7.loadPatch(g_pendingDx7);
      portEXIT_CRITICAL(&g_patchMux);
      break;
  }
}

static void pollInputs() {
  while (Serial1.available()) g_midi.feed((uint8_t)Serial1.read());
  Ev e;
  while (xQueueReceive(g_evq, &e, 0) == pdTRUE) handleEvent(e);
}

// Output chunks are OUT_BLOCKS engine blocks long. The next chunk is rendered
// while the previous two are still queued/playing, so the codec always has
// 2-3 chunks (~11-16 ms) in hand and small scheduling hiccups don't cause gaps.
static constexpr int OUT_BLOCKS = 2;
static constexpr int OUT_LEN = BLOCK * OUT_BLOCKS;

static void DSP_HOT audioTask(void*) {
  static int16_t pcm[3][OUT_LEN];
  static float   mix[BLOCK];
  int   bi = 0;
  float dcX = 0.f, dcY = 0.f;
  float limEnv = 0.f;                              // peak limiter envelope
  const float limRel = expf(-1.f / (0.25f * SAMPLE_RATE));  // 250 ms release
  const float limThresh = 0.8f;
  const float chunkUs = 1e6f * OUT_LEN / SAMPLE_RATE;
  for (;;) {
    // 1) render the next chunk ahead of time
    uint32_t t0 = micros();
    for (int b = 0; b < OUT_BLOCKS; b++) {
      pollInputs();
      g_engine->render(mix, BLOCK);
      int16_t* out = pcm[bi] + b * BLOCK;
      for (int i = 0; i < BLOCK; i++) {
        // DC blocker -> peak limiter -> soft clip
        float x = mix[i];
        dcY = x - dcX + 0.9987f * dcY;
        dcX = x;
        float a = fabsf(dcY);
        limEnv = a > limEnv ? a : limEnv * limRel;
        float y = limEnv > limThresh ? dcY * (limThresh / limEnv) : dcY;
        out[i] = (int16_t)(softClip(y) * 30000.f);
      }
    }
    uint32_t dt = micros() - t0;
    float load = dt / chunkUs;
    g_cpu = g_cpu * 0.9f + 0.1f * load;
    // Falling behind: fade out the quietest released voice rather than let the
    // audio run dry.
    if (load > 0.8f || g_cpu > 0.75f) g_engine->shedVoice();

    // 2) hand it to the codec as soon as there's a free queue slot
    while (M5.Speaker.isPlaying(0) >= 2) {
      vTaskDelay(1);
      pollInputs();
    }
    M5.Speaker.playRaw(pcm[bi], OUT_LEN, SAMPLE_RATE, false, 1, 0, false);
    bi = (bi + 1) % 3;
  }
}

static void sendEv(uint8_t type, uint8_t a = 0, uint8_t b = 0) {
  Ev e{type, a, b};
  xQueueSend(g_evq, &e, 0);
}

// ---------------------------------------------------------------------------
// Patch storage
// ---------------------------------------------------------------------------
static Preferences g_prefs;
static JunoPatch   g_junoSlots[16];  // 0-7 presets, 8-15 user (NVS)
static int         g_junoIdx = 0;

static bool        g_sdOk = false;
static String      g_bankFiles[48];
static int         g_bankCount = 0;   // number of SD bank files
static int         g_bankIdx = 0;     // 0 = internal, 1.. = files, -1 = MIDI dump
static Dx7Patch    g_bank[32];
static int         g_bankSize = 0;
static int         g_voiceIdx = 0;
static char        g_bankName[24] = "INTERNAL";

static String g_toast;
static uint32_t g_toastUntil = 0;
static void toast(const String& s, uint32_t ms = 1500) { g_toast = s; g_toastUntil = millis() + ms; }

static void loadSettings() {
  g_prefs.begin("csynth", true);
  if (g_prefs.getBytesLength("set") == sizeof(Settings)) g_prefs.getBytes("set", &g_set, sizeof(Settings));
  for (int i = 0; i < 8; i++) {
    g_junoSlots[i] = kJunoPresets[i];
    char key[4] = {'u', (char)('0' + i), 0};
    if (g_prefs.getBytesLength(key) == sizeof(JunoPatch)) g_prefs.getBytes(key, &g_junoSlots[8 + i], sizeof(JunoPatch));
    else {
      g_junoSlots[8 + i] = kJunoPresets[i];
      snprintf(g_junoSlots[8 + i].name, sizeof(JunoPatch::name), "User %d", i + 1);
    }
  }
  g_prefs.end();
}

static uint32_t g_settingsDirtyAt = 0;
static void saveSettingsSoon() { g_settingsDirtyAt = millis() + 2000; }
static void saveSettingsNow() {
  g_prefs.begin("csynth", false);
  g_prefs.putBytes("set", &g_set, sizeof(Settings));
  g_prefs.end();
  g_settingsDirtyAt = 0;
}

static void selectJuno(int idx) {
  g_junoIdx = (idx + 16) % 16;
  portENTER_CRITICAL(&g_patchMux);
  g_pendingJuno = g_junoSlots[g_junoIdx];
  portEXIT_CRITICAL(&g_patchMux);
  sendEv(EV_JUNO_PATCH);
}

static void saveJuno() {
  int target = g_junoIdx < 8 ? g_junoIdx + 8 : g_junoIdx;
  JunoPatch p = juno.patch;
  if (g_junoIdx < 8) snprintf(p.name, sizeof(p.name), "%.6s U%d", juno.patch.name, target - 7);
  g_junoSlots[target] = p;
  char key[4] = {'u', (char)('0' + target - 8), 0};
  g_prefs.begin("csynth", false);
  g_prefs.putBytes(key, &p, sizeof(JunoPatch));
  g_prefs.end();
  g_junoIdx = target;
  selectJuno(target);
  toast("Saved to U" + String(target - 7));
}

static void selectDx7Voice(int idx) {
  if (g_bankSize <= 0) return;
  g_voiceIdx = (idx % g_bankSize + g_bankSize) % g_bankSize;
  portENTER_CRITICAL(&g_patchMux);
  g_pendingDx7 = g_bank[g_voiceIdx];
  portEXIT_CRITICAL(&g_patchMux);
  sendEv(EV_DX7_PATCH);
}

static void loadInternalBank() {
  for (int i = 0; i < DX7_PRESETS; i++) g_bank[i] = kDx7Presets[i];
  g_bankSize = DX7_PRESETS;
  strcpy(g_bankName, "INTERNAL");
}

// Parse a DX7 .syx file: 32-voice VMEM dump, raw 4096-byte VMEM, or single VCED voice.
static bool loadBankFile(const String& path) {
  File f = SD.open(path);
  if (!f) return false;
  size_t sz = f.size();
  if (sz > 8192) sz = 8192;
  static uint8_t buf[8192];
  size_t got = f.read(buf, sz);
  f.close();
  int count = 0;
  for (size_t i = 0; i + 6 < got; i++) {  // find a VMEM header anywhere in the file
    if (buf[i] == 0xF0 && buf[i + 1] == 0x43 && buf[i + 3] == 0x09 && i + 6 + 4096 <= got) {
      for (int v = 0; v < 32; v++) dx7UnpackVmem(buf + i + 6 + v * 128, g_bank[v]);
      count = 32;
      break;
    }
    if (buf[i] == 0xF0 && buf[i + 1] == 0x43 && buf[i + 3] == 0x00 && buf[i + 4] == 0x01 &&
        i + 6 + 155 <= got) {
      dx7UnpackVced(buf + i + 6, g_bank[0]);
      count = 1;
      break;
    }
  }
  if (!count && got == 4096) {
    for (int v = 0; v < 32; v++) dx7UnpackVmem(buf + v * 128, g_bank[v]);
    count = 32;
  }
  if (!count) return false;
  g_bankSize = count;
  String nm = path.substring(path.lastIndexOf('/') + 1);
  nm.replace(".syx", ""); nm.replace(".SYX", "");
  strncpy(g_bankName, nm.c_str(), sizeof(g_bankName) - 1);
  g_bankName[sizeof(g_bankName) - 1] = 0;
  return true;
}

static void selectBank(int idx) {
  int total = g_bankCount + 1;
  idx = ((idx % total) + total) % total;
  if (idx == 0) loadInternalBank();
  else if (!loadBankFile(g_bankFiles[idx - 1])) { toast("Bad bank file"); return; }
  g_bankIdx = idx;
  selectDx7Voice(0);
}

static void scanSd() {
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  g_sdOk = SD.begin(SD_CS, SPI, 25000000);
  g_bankCount = 0;
  if (!g_sdOk) return;
  if (!SD.exists("/dx7")) SD.mkdir("/dx7");
  File dir = SD.open("/dx7");
  if (!dir) return;
  for (File f = dir.openNextFile(); f && g_bankCount < 48; f = dir.openNextFile()) {
    String n = f.name();
    String lower = n; lower.toLowerCase();
    if (!f.isDirectory() && lower.endsWith(".syx")) {
      if (!n.startsWith("/")) n = "/dx7/" + n;
      g_bankFiles[g_bankCount++] = n;
    }
    f.close();
  }
  dir.close();
  // simple alphabetical sort
  for (int i = 0; i < g_bankCount; i++)
    for (int j = i + 1; j < g_bankCount; j++)
      if (g_bankFiles[j] < g_bankFiles[i]) { String t = g_bankFiles[i]; g_bankFiles[i] = g_bankFiles[j]; g_bankFiles[j] = t; }
}

// Save the edited DX7 voice into /dx7/USER.syx at the current voice number.
static void saveDx7() {
  if (!g_sdOk) { toast("No SD card"); return; }
  static uint8_t file[4104];
  const char* path = "/dx7/USER.syx";
  bool have = false;
  if (SD.exists(path)) {
    File f = SD.open(path, FILE_READ);
    if (f && f.size() >= 4104) have = f.read(file, 4104) == 4104 && file[0] == 0xF0 && file[3] == 0x09;
    if (f) f.close();
  }
  if (!have) {  // new bank, seeded with the current one
    file[0] = 0xF0; file[1] = 0x43; file[2] = 0x00; file[3] = 0x09; file[4] = 0x20; file[5] = 0x00;
    for (int v = 0; v < 32; v++) dx7PackVmem(g_bank[v % g_bankSize], file + 6 + v * 128);
  }
  int slot = g_voiceIdx % 32;
  dx7PackVmem(dx7.patch, file + 6 + slot * 128);
  uint8_t sum = 0;
  for (int i = 0; i < 4096; i++) sum += file[6 + i];
  file[4102] = (uint8_t)(-sum) & 0x7F;
  file[4103] = 0xF7;
  File f = SD.open(path, FILE_WRITE);
  if (!f) { toast("Write failed"); return; }
  f.write(file, 4104);
  f.close();
  scanSd();
  toast("Saved USER.syx #" + String(slot + 1));
}

// ---------------------------------------------------------------------------
// Parameter pages
// ---------------------------------------------------------------------------

static const char* const L_RANGE[] = {"16'", "8'", "4'"};
static const char* const L_PWM[]   = {"LFO", "MANUAL", "ENV"};
static const char* const L_ONOFF[] = {"OFF", "ON"};
static const char* const L_HPF[]   = {"0 BOOST", "1 FLAT", "2", "3"};
static const char* const L_POL[]   = {"+ NORMAL", "- INVERT"};
static const char* const L_VCA[]   = {"ENV", "GATE"};
static const char* const L_CHO[]   = {"OFF", "I", "II", "I+II"};
static const char* const L_WAVE[]  = {"TRIANGLE", "SAW DOWN", "SAW UP", "SQUARE", "SINE", "S&H"};
static const char* const L_MODE[]  = {"RATIO", "FIXED"};
static const char* const L_CURVE[] = {"-LIN", "-EXP", "+EXP", "+LIN"};

static Param g_params[40];
static int   g_nParams = 0;
static int   g_page = 0, g_sel = 0, g_scroll = 0;

static void addP(const char* n, uint8_t* p, int mn, int mx, int off = 0, const char* const* lb = nullptr,
                 uint8_t flags = PF_NONE, uint8_t bit = 0) {
  if (g_nParams < 40) g_params[g_nParams++] = Param{n, p, (int16_t)mn, (int16_t)mx, (int16_t)off, lb, flags, bit};
}

static int pageCount() { return g_set.engine == 0 ? 2 : 8; }

static const char* pageName() {
  static char buf[16];
  if (g_set.engine == 0) return g_page == 0 ? "TONE" : "SETUP";
  if (g_page == 0) return "VOICE";
  if (g_page == 7) return "SETUP";
  snprintf(buf, sizeof(buf), "OPERATOR %d", g_page);
  return buf;
}

static void addSetupParams() {
  addP("MIDI Channel", &g_set.midiCh, 0, 16, 0, nullptr, PF_SETTING);
  addP("Bend Range", &g_set.bendRange, 1, 12, 0, nullptr, PF_SETTING);
  addP("Volume", &g_set.volume, 0, 255, 0, nullptr, PF_SETTING);
}

static void buildPage() {
  g_nParams = 0;
  if (g_set.engine == 0) {
    JunoPatch& p = juno.patch;
    if (g_page == 0) {
      addP("LFO Rate", &p.lfoRate, 0, 127);
      addP("LFO Delay", &p.lfoDelay, 0, 127);
      addP("DCO Range", &p.dcoRange, 0, 2, 0, L_RANGE);
      addP("DCO LFO", &p.dcoLfo, 0, 127);
      addP("DCO PWM", &p.pwm, 0, 127);
      addP("PWM Source", &p.pwmMode, 0, 2, 0, L_PWM);
      addP("Pulse", &p.pulse, 0, 1, 0, L_ONOFF);
      addP("Saw", &p.saw, 0, 1, 0, L_ONOFF);
      addP("Sub Osc", &p.sub, 0, 127);
      addP("Noise", &p.noise, 0, 127);
      addP("HPF", &p.hpf, 0, 3, 0, L_HPF);
      addP("VCF Freq", &p.cutoff, 0, 127);
      addP("VCF Res", &p.res, 0, 127);
      addP("VCF Env Pol", &p.envPol, 0, 1, 0, L_POL);
      addP("VCF Env", &p.envAmt, 0, 127);
      addP("VCF LFO", &p.vcfLfo, 0, 127);
      addP("VCF Kybd", &p.keyFlw, 0, 127);
      addP("VCA Mode", &p.vcaMode, 0, 1, 0, L_VCA);
      addP("VCA Level", &p.vcaLevel, 0, 127);
      addP("Attack", &p.A, 0, 127);
      addP("Decay", &p.D, 0, 127);
      addP("Sustain", &p.S, 0, 127);
      addP("Release", &p.R, 0, 127);
      addP("Chorus", &p.chorus, 0, 3, 0, L_CHO);
      addP("Velocity", &p.velSens, 0, 127);
    } else addSetupParams();
  } else {
    Dx7Patch& p = dx7.patch;
    if (g_page == 0) {
      addP("Algorithm", &p.alg, 0, 31, 1);
      addP("Feedback", &p.fb, 0, 7);
      addP("Osc Key Sync", &p.oks, 0, 1, 0, L_ONOFF);
      addP("Transpose", &p.trnsp, 0, 48, -24);
      addP("* Brightness", &dx7.macroBright, 0, 100, -50);
      addP("* Attack", &dx7.macroAttack, 0, 100, -50);
      addP("* Release", &dx7.macroRelease, 0, 100, -50);
      addP("LFO Wave", &p.lfw, 0, 5, 0, L_WAVE);
      addP("LFO Speed", &p.lfs, 0, 99);
      addP("LFO Delay", &p.lfd, 0, 99);
      addP("LFO Pitch Dep", &p.lpmd, 0, 99);
      addP("LFO Amp Dep", &p.lamd, 0, 99);
      addP("LFO Pitch Sens", &p.lpms, 0, 7);
      addP("LFO Key Sync", &p.lfks, 0, 1, 0, L_ONOFF);
      addP("PEG Rate 1", &p.pr[0], 0, 99);
      addP("PEG Rate 2", &p.pr[1], 0, 99);
      addP("PEG Rate 3", &p.pr[2], 0, 99);
      addP("PEG Rate 4", &p.pr[3], 0, 99);
      addP("PEG Level 1", &p.pl[0], 0, 99, -50);
      addP("PEG Level 2", &p.pl[1], 0, 99, -50);
      addP("PEG Level 3", &p.pl[2], 0, 99, -50);
      addP("PEG Level 4", &p.pl[3], 0, 99, -50);
    } else if (g_page == 7) {
      addSetupParams();
    } else {
      int i = g_page - 1;
      Dx7Op& o = p.op[i];
      addP("Enabled", &dx7.opEnable, 0, 1, 0, L_ONOFF, PF_BIT, (uint8_t)i);
      addP("Output Level", &o.ol, 0, 99);
      addP("Freq Mode", &o.mode, 0, 1, 0, L_MODE);
      addP("Coarse", &o.coarse, 0, 31);
      addP("Fine", &o.fine, 0, 99);
      addP("Detune", &o.det, 0, 14, -7);
      addP("EG Rate 1", &o.r[0], 0, 99);
      addP("EG Rate 2", &o.r[1], 0, 99);
      addP("EG Rate 3", &o.r[2], 0, 99);
      addP("EG Rate 4", &o.r[3], 0, 99);
      addP("EG Level 1", &o.l[0], 0, 99);
      addP("EG Level 2", &o.l[1], 0, 99);
      addP("EG Level 3", &o.l[2], 0, 99);
      addP("EG Level 4", &o.l[3], 0, 99);
      addP("Velocity Sens", &o.kvs, 0, 7);
      addP("Amp Mod Sens", &o.ams, 0, 3);
      addP("Rate Scaling", &o.rs, 0, 7);
      addP("Break Point", &o.bp, 0, 99);
      addP("Left Depth", &o.ld, 0, 99);
      addP("Right Depth", &o.rd, 0, 99);
      addP("Left Curve", &o.lc, 0, 3, 0, L_CURVE);
      addP("Right Curve", &o.rc, 0, 3, 0, L_CURVE);
    }
  }
  if (g_sel >= g_nParams) g_sel = g_nParams - 1;
  if (g_sel < 0) g_sel = 0;
}

static int getRaw(const Param& p) {
  if (p.flags & PF_BIT) return (*p.ptr >> p.bit) & 1;
  return *p.ptr;
}

static void setRaw(const Param& p, int v) {
  if (v < p.mn) v = p.mn;
  if (v > p.mx) v = p.mx;
  if (p.flags & PF_BIT) {
    if (v) *p.ptr |= (1 << p.bit); else *p.ptr &= ~(1 << p.bit);
  } else {
    *p.ptr = (uint8_t)v;  // single-byte write: safe to do while the audio task reads it
  }
  if (p.flags & PF_SETTING) {
    if (p.ptr == &g_set.volume) M5.Speaker.setVolume(g_set.volume);
    saveSettingsSoon();
  }
}

static String valueText(const Param& p) {
  int raw = getRaw(p);
  if (p.labels) return String(p.labels[raw - p.mn]);
  if (p.ptr == &g_set.midiCh) return raw == 0 ? String("OMNI") : String(raw);
  if (p.ptr == &g_set.volume) return String(raw * 100 / 255) + "%";
  int v = raw + p.dispOffset;
  if (p.dispOffset < 0 && v > 0) return "+" + String(v);
  return String(v);
}

// ---------------------------------------------------------------------------
// Display
// ---------------------------------------------------------------------------
static M5Canvas g_canvas(&M5Cardputer.Display);
static int  g_octave = 0;
static bool g_dirty = true;

// Landscape is rotation 1 or 3. In auto mode pick whichever the panel reports
// as wider than tall; the ` key cycles through all four and remembers the choice.
static void applyRotation() {
  auto& d = M5Cardputer.Display;
  if (g_set.rotation <= 3) {
    d.setRotation(g_set.rotation);
  } else {
    d.setRotation(1);
    if (d.width() < d.height()) d.setRotation(0);
    if (d.width() < d.height()) d.setRotation(3);
  }
  d.fillScreen(TFT_BLACK);
}

static void cycleRotation() {
  static const uint8_t order[4] = {1, 3, 0, 2};
  uint8_t cur = M5Cardputer.Display.getRotation() & 3;
  int i = 0;
  while (order[i] != cur) i++;
  g_set.rotation = order[(i + 1) % 4];
  applyRotation();
  saveSettingsSoon();
}

static uint16_t engineColor() { return g_set.engine == 0 ? 0xFC60 /*orange*/ : 0x2D7F /*blue*/; }

static void draw() {
  M5Canvas& c = g_canvas;
  const int W = 240;
  c.fillScreen(TFT_BLACK);

  // header
  c.fillRect(0, 0, W, 14, engineColor());
  c.setFont(&fonts::Font0);
  c.setTextSize(1);
  c.setTextColor(TFT_BLACK, engineColor());
  char hdr[48];
  if (g_set.engine == 0)
    snprintf(hdr, sizeof(hdr), "JUNO %c%d %s", g_junoIdx < 8 ? 'P' : 'U', g_junoIdx % 8 + 1, juno.patch.name);
  else
    snprintf(hdr, sizeof(hdr), "DX7 %02d %s", g_voiceIdx + 1, dx7.patch.name);
  c.drawString(hdr, 3, 3);
  char st[24];
  snprintf(st, sizeof(st), "V%d %2d%%", g_engine->activeVoices(), (int)(g_cpu * 100));
  c.drawRightString(st, W - 14, 3);
  bool midiLit = millis() - g_midiActivityMs < 120;
  c.fillCircle(W - 6, 7, 3, midiLit ? TFT_GREEN : 0x2104);

  // sub header
  c.setTextColor(engineColor(), TFT_BLACK);
  char sub[64];
  if (g_set.engine == 0)
    snprintf(sub, sizeof(sub), "%s  %d/%d", pageName(), g_page + 1, pageCount());
  else
    snprintf(sub, sizeof(sub), "%s %d/%d  %.12s", pageName(), g_page + 1, pageCount(), g_bankName);
  c.drawString(sub, 3, 18);
  char oct[12];
  snprintf(oct, sizeof(oct), "OCT %+d", g_octave);
  c.drawRightString(oct, W - 3, 18);

  // parameter list
  const int rows = 5, rowH = 18, y0 = 30;
  if (g_sel < g_scroll) g_scroll = g_sel;
  if (g_sel >= g_scroll + rows) g_scroll = g_sel - rows + 1;
  c.setFont(&fonts::Font2);
  for (int r = 0; r < rows; r++) {
    int i = g_scroll + r;
    if (i >= g_nParams) break;
    const Param& p = g_params[i];
    int y = y0 + r * rowH;
    bool sel = i == g_sel;
    if (sel) c.fillRoundRect(0, y - 1, W - 6, rowH, 3, 0x2945);
    c.setTextColor(sel ? TFT_WHITE : 0xAD55);
    c.drawString(p.name, 6, y);
    String v = valueText(p);
    c.setTextColor(sel ? engineColor() : TFT_WHITE);
    c.drawRightString(v, W - 12, y);
    if (!p.labels && p.mx - p.mn > 7) {  // value bar
      int bw = 60, bx = 112;
      int fillw = (getRaw(p) - p.mn) * bw / (p.mx - p.mn);
      c.drawRect(bx, y + 6, bw, 4, 0x4208);
      c.fillRect(bx, y + 6, fillw, 4, sel ? engineColor() : 0x7BEF);
    }
  }
  // scrollbar
  if (g_nParams > rows) {
    int h = rows * rowH;
    int bh = h * rows / g_nParams;
    int by = y0 + (h - bh) * g_scroll / (g_nParams - rows);
    c.fillRect(W - 3, by, 2, bh, 0x7BEF);
  }

  // footer / toast
  c.setFont(&fonts::Font0);
  if (millis() < g_toastUntil) {
    c.fillRect(0, 122, W, 13, TFT_WHITE);
    c.setTextColor(TFT_BLACK, TFT_WHITE);
    c.drawCentreString(g_toast, W / 2, 125);
  } else {
    c.setTextColor(0x8410, TFT_BLACK);
    c.drawString(g_set.engine == 0 ? "[] patch ;. sel ,/ val ent page tab DX7"
                                   : "[] voice {} bank ,/ val ent page tab JUNO", 3, 125);
  }
  c.pushSprite(0, 0);
}

// ---------------------------------------------------------------------------
// Keyboard: QWERTY piano + UI keys
// ---------------------------------------------------------------------------
// Two rows like a tracker: Z-row = lower octave, Q-row = upper octave.
static int pianoOffset(char c) {
  static const char* lower = "zsxdcvgbhnjm";
  static const char* upper = "q2w3er5t6y7ui9o0p";
  if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
  const char* p = strchr(lower, c);
  if (p && c) return (int)(p - lower);
  p = strchr(upper, c);
  if (p && c) return 12 + (int)(p - upper);
  return -1;
}

static int8_t  g_pianoNote[128];  // note sent for each held key char, -1 = none
static bool    g_prevKeys[128];
static bool    g_prevEnter = false, g_prevTab = false, g_prevDel = false;
static char    g_repeatKey = 0;
static uint32_t g_repeatAt = 0;

static void switchEngine() {
  g_set.engine ^= 1;
  sendEv(EV_ENGINE, g_set.engine);
  g_page = 0; g_sel = 0; g_scroll = 0;
  buildPage();
  saveSettingsSoon();
}

static void prevNextPatch(int d) {
  if (g_set.engine == 0) selectJuno(g_junoIdx + d);
  else selectDx7Voice(g_voiceIdx + d);
  buildPage();
}

static void uiKey(char k) {
  switch (k) {
    case ';': g_sel = (g_sel - 1 + g_nParams) % g_nParams; break;
    case '.': g_sel = (g_sel + 1) % g_nParams; break;
    case ':': g_sel = max(0, g_sel - 5); break;
    case '>': g_sel = min(g_nParams - 1, g_sel + 5); break;
    case ',': case '/': case '<': case '?': {
      const Param& p = g_params[g_sel];
      int step = (k == '<' || k == '?') ? max(1, (p.mx - p.mn) / 10) : 1;
      setRaw(p, getRaw(p) + ((k == '/' || k == '?') ? step : -step));
      break;
    }
    case '[': prevNextPatch(-1); break;
    case ']': prevNextPatch(+1); break;
    case '{': if (g_set.engine == 1) { selectBank(g_bankIdx - 1); buildPage(); } break;
    case '}': if (g_set.engine == 1) { selectBank(g_bankIdx + 1); buildPage(); } break;
    case '-': g_octave = max(-3, g_octave - 1); break;
    case '=': g_octave = min(3, g_octave + 1); break;
    case '\\': if (g_set.engine == 0) saveJuno(); else saveDx7(); buildPage(); break;
    case '`': cycleRotation(); toast("Rotation " + String(g_set.rotation)); break;
    default: break;
  }
  g_dirty = true;
}

static bool isRepeatable(char k) { return strchr(";.,/<?:>", k) != nullptr; }

static void handleKeyboard() {
  auto& kb = M5Cardputer.Keyboard;
  Keyboard_Class::KeysState st = kb.keysState();
  bool now[128] = {};
  for (char ch : st.word) now[(uint8_t)ch & 127] = true;

  {  // diff against the previous scan (isChange() only tracks the key count)
    // piano keys
    for (int ch = 0; ch < 128; ch++) {
      int off = pianoOffset((char)ch);
      if (off < 0) continue;
      if (now[ch] && !g_prevKeys[ch]) {
        int note = 48 + g_octave * 12 + off;
        if (note >= 0 && note < 128) { g_pianoNote[ch] = note; sendEv(EV_NOTE_ON, note, 100); }
      } else if (!now[ch] && g_prevKeys[ch] && g_pianoNote[ch] >= 0) {
        sendEv(EV_NOTE_OFF, g_pianoNote[ch]);
        g_pianoNote[ch] = -1;
      }
    }
    // UI keys (edge triggered)
    for (int ch = 0; ch < 128; ch++) {
      if (now[ch] && !g_prevKeys[ch] && pianoOffset((char)ch) < 0) {
        uiKey((char)ch);
        if (isRepeatable((char)ch)) { g_repeatKey = (char)ch; g_repeatAt = millis() + 350; }
      }
    }
    if (st.enter && !g_prevEnter) { g_page = (g_page + 1) % pageCount(); g_sel = 0; g_scroll = 0; buildPage(); g_dirty = true; }
    if (st.tab && !g_prevTab) { switchEngine(); g_dirty = true; }
    if (st.del && !g_prevDel) { sendEv(EV_PANIC); toast("All notes off"); g_dirty = true; }
    g_prevEnter = st.enter; g_prevTab = st.tab; g_prevDel = st.del;
    memcpy(g_prevKeys, now, sizeof(now));
  }
  // auto-repeat for navigation/value keys
  if (g_repeatKey) {
    if (!now[(uint8_t)g_repeatKey]) g_repeatKey = 0;
    else if ((int32_t)(millis() - g_repeatAt) >= 0) { uiKey(g_repeatKey); g_repeatAt = millis() + 60; }
  }
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------
void setup() {
  auto cfg = M5.config();
  cfg.internal_mic = false;  // ES8311 is used for playback only
  M5Cardputer.begin(cfg, true);
  initTables();
  loadSettings();
  applyRotation();
  memset(g_pianoNote, -1, sizeof(g_pianoNote));

  // Speaker / codec at the synth's sample rate with short DMA buffers for low latency
  M5.Speaker.end();
  auto sc = M5.Speaker.config();
  sc.sample_rate = SAMPLE_RATE;
  sc.dma_buf_len = 128;
  sc.dma_buf_count = 6;
  sc.task_priority = 6;
  sc.task_pinned_core = 0;
  M5.Speaker.config(sc);
  M5.Speaker.begin();
  M5.Speaker.setVolume(g_set.volume);

  // MIDI in from Unit MIDI (31250 baud, 8N1)
  Serial1.setRxBufferSize(1024);
  Serial1.begin(31250, SERIAL_8N1, MIDI_RX_PIN, MIDI_TX_PIN);

  scanSd();
  loadInternalBank();

  g_canvas.setColorDepth(16);
  g_canvas.createSprite(240, 135);

  g_evq = xQueueCreate(64, sizeof(Ev));
  // initial patches
  juno.loadPatch(g_junoSlots[0]);
  dx7.loadPatch(g_bank[0]);
  g_engine = g_set.engine ? (Engine*)&dx7 : (Engine*)&juno;

  // Core 1: away from M5Unified's speaker task on core 0; priority above loop()
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 5, nullptr, 1);

  buildPage();
  toast(g_sdOk ? String("SD: ") + g_bankCount + " DX7 banks" : String("No SD card (DX7 internal bank)"), 2500);
}

void loop() {
  M5Cardputer.update();
  handleKeyboard();

  if (g_pendingProgram >= 0) {
    int p = g_pendingProgram;
    g_pendingProgram = -1;
    if (g_set.engine == 0) selectJuno(p % 16);
    else selectDx7Voice(p);
    buildPage();
    g_dirty = true;
  }
  if (g_dumpReady) {
    g_dumpReady = false;
    for (int i = 0; i < 32; i++) g_bank[i] = g_dumpBank[i];
    g_bankSize = 32;
    g_bankIdx = 0;
    strcpy(g_bankName, "MIDI DUMP");
    if (g_set.engine != 1) switchEngine();
    selectDx7Voice(0);
    buildPage();
    toast("Received 32-voice bank");
  }
  if (g_singleVoiceRx) {
    g_singleVoiceRx = false;
    if (g_set.engine != 1) { g_set.engine = 1; g_page = 0; }
    buildPage();
    toast("Received DX7 voice");
  }
  if (g_settingsDirtyAt && millis() > g_settingsDirtyAt) saveSettingsNow();

  // Redraw immediately after edits, otherwise ~6 fps for the meters. Screen
  // drawing competes with the audio code for the flash cache, so keep it light.
  static uint32_t lastDraw = 0;
  static bool lastLit = false;
  bool lit = millis() - g_midiActivityMs < 120;
  uint32_t since = millis() - lastDraw;
  if ((g_dirty && since > 30) || lit != lastLit || since > 160 || (millis() < g_toastUntil + 200 && since > 100)) {
    lastDraw = millis();
    lastLit = lit;
    draw();
    g_dirty = false;
  }
  delay(5);
}
