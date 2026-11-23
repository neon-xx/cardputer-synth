// CardputerAMY - the AMY synthesizer engine (shorepine/amy, the engine behind
// AMYboard and Tulip) on the M5Stack Cardputer-Adv, played from a MIDI keyboard
// through the M5Stack Unit MIDI, or from the Cardputer's own keys.
//
// AMY brings 128 Juno-6 patches, 128 DX7 patches and a modelled piano.
// AMY renders the audio; this sketch hands it to the Cardputer's ES8311 codec
// through M5Unified, parses MIDI from the Grove port, and provides the UI.
//
// Hardware: Unit MIDI on the Grove port with its DIP switch on BYPASS.
// Libraries: AMY (pinned, see platformio.ini), M5Cardputer >= 1.1.0,
//            M5Unified >= 0.2.8, M5GFX >= 0.2.10

#include <M5Cardputer.h>
#include <Preferences.h>
#include <AMY-Arduino.h>
#include "midi_parser.h"
#include "amy_patch_names.h"
#include "amy_cmd.h"

extern "C" {
void amy_event_midi_message_received(uint8_t* data, uint32_t len, uint8_t sysex);
}

// ---------------------------------------------------------------------------
// Pins
// ---------------------------------------------------------------------------
static constexpr int MIDI_RX_PIN = 1;  // Grove white  = Unit MIDI TX (MIDI IN)
static constexpr int MIDI_TX_PIN = 2;  // Grove yellow = Unit MIDI RX (unused)

// ---------------------------------------------------------------------------
// Settings (saved to flash)
// ---------------------------------------------------------------------------
enum Bank : uint8_t { BANK_JUNO = 0, BANK_DX7 = 1, BANK_PIANO = 2 };
static constexpr uint8_t OVERRIDE_OFF = 128;  // cutoff/res value meaning "use the patch's"

struct Settings {
  uint8_t bank = BANK_JUNO;
  uint8_t patch[2] = {0, 0};      // patch within the Juno / DX7 bank (0-127)
  uint8_t voices[3] = {6, 8, 4};  // polyphony per bank
  uint8_t cutoff = OVERRIDE_OFF;
  uint8_t res = OVERRIDE_OFF;
  uint8_t reverb = 0;             // 0-127
  uint8_t midiCh = 0;             // 0 = omni
  uint8_t volume = 170;
  uint8_t rotation = 0xFF;        // 0xFF = auto landscape
  uint8_t level[3] = {100, 100, 100};  // per-bank level trim, percent
  uint8_t version = 2;
};
static Settings g_set;
static Preferences g_prefs;

static int amyPatchNumber() {
  if (g_set.bank == BANK_JUNO) return g_set.patch[0];
  if (g_set.bank == BANK_DX7) return 128 + g_set.patch[1];
  return 256;
}

// ---------------------------------------------------------------------------
// Commands from the UI to the audio task (which owns AMY)
// ---------------------------------------------------------------------------
static QueueHandle_t g_cmdq;

static volatile uint32_t g_midiActivityMs = 0;
static volatile float    g_cpu = 0.f, g_cpuPeak = 0.f;
static volatile int      g_activeNotes = 0;  // notes held (MIDI + keys), for the UI freeze
static volatile int      g_pendingProgram = -1;
static volatile bool     g_amyReady = false;

static void sendCmd(uint8_t t, uint8_t a = 0, uint8_t b = 0) {
  Cmd c{t, a, b};
  xQueueSend(g_cmdq, &c, 0);
}

// ---------------------------------------------------------------------------
// AMY control (audio task only)
// ---------------------------------------------------------------------------
static bool g_filterTypeSet = false;

static void amyApplyFilter() {
  amy_event e = amy_default_event();
  e.synth = 1;
  bool any = false;
  if (g_set.cutoff != OVERRIDE_OFF) {
    float hz = 20.f * exp2f(g_set.cutoff / 127.f * 10.f);  // 20 Hz .. 20 kHz
    e.filter_freq_coefs[COEF_CONST] = hz;
    if (g_set.bank != BANK_JUNO) { e.filter_type = FILTER_LPF24; g_filterTypeSet = true; }
    any = true;
  } else if (g_set.bank != BANK_JUNO && g_filterTypeSet) {
    e.filter_type = FILTER_NONE;
    g_filterTypeSet = false;
    any = true;
  }
  if (g_set.res != OVERRIDE_OFF) {
    e.resonance = 0.7f * exp2f(g_set.res / 127.f * 4.5f);  // Q 0.7 .. ~16
    any = true;
  }
  if (any) amy_add_event(&e);
}

// AMY's banks come out at very different raw levels, so each bank gets a base
// gain on the output bus, times the user's per-bank Level trim.
static void amyApplyLevel() {
  static const float kBankGain[3] = {2.0f, 3.5f, 0.2f};
  amy_event v = amy_default_event();
  v.volume = kBankGain[g_set.bank] * g_set.level[g_set.bank] / 100.f;
  amy_add_event(&v);
}

static void amyLoadPatch() {
  amy_event e = amy_default_event();
  e.synth = 1;
  e.patch_number = amyPatchNumber();
  e.num_voices = g_set.voices[g_set.bank];
  amy_add_event(&e);
  amyApplyLevel();
  g_filterTypeSet = false;
  amyApplyFilter();  // re-apply the user's filter override, if any
}

static void amyApplyReverb() {
  config_reverb(0, g_set.reverb / 127.f * 0.6f, 0.85f, 0.5f, 3000.f);
}

static void amyMidi(uint8_t s, uint8_t d1, uint8_t d2, int len) {
  uint8_t m[3] = {s, d1, d2};
  amy_event_midi_message_received(m, len, 0);
}

static int8_t g_noteCount[128];  // per-note hold count (keyboard + MIDI)
static void noteOn(uint8_t n, uint8_t v) {
  if (g_noteCount[n] < 8) g_noteCount[n]++;
  amyMidi(0x90, n, v, 3);
}
static void noteOff(uint8_t n) {
  if (g_noteCount[n] > 0) g_noteCount[n]--;
  amyMidi(0x80, n, 0, 3);
}
static void panic() {
  memset(g_noteCount, 0, sizeof(g_noteCount));
  amyMidi(0xB0, 64, 0, 3);
  amyMidi(0xB0, 123, 0, 3);
}

// MIDI from the Grove port: channel-filtered, re-sent to AMY on channel 1
struct MidiToAmy : syn::MidiParser::Handler {
  bool ok(uint8_t ch) { return g_set.midiCh == 0 || ch == g_set.midiCh - 1; }
  void onNoteOn(uint8_t ch, uint8_t n, uint8_t v) override { if (ok(ch)) { noteOn(n, v); g_midiActivityMs = millis(); } }
  void onNoteOff(uint8_t ch, uint8_t n) override { if (ok(ch)) { noteOff(n); g_midiActivityMs = millis(); } }
  void onCC(uint8_t ch, uint8_t cc, uint8_t v) override {
    if (!ok(ch)) return;
    g_midiActivityMs = millis();
    switch (cc) {
      case 7:   g_set.volume = v * 2; M5.Speaker.setVolume(g_set.volume); break;
      case 74:  g_set.cutoff = v; amyApplyFilter(); break;
      case 71:  g_set.res = v; amyApplyFilter(); break;
      case 91:  g_set.reverb = v; amyApplyReverb(); break;
      case 120: case 123: panic(); break;
      default:  amyMidi(0xB0, cc, v, 3); break;  // sustain, mod wheel, etc.
    }
  }
  void onProgram(uint8_t ch, uint8_t p) override { if (ok(ch)) g_pendingProgram = p; }
  void onBend(uint8_t ch, int value) override {
    if (!ok(ch)) return;
    int u = value + 8192;
    amyMidi(0xE0, u & 0x7F, (u >> 7) & 0x7F, 3);
  }
  void onSysEx(const uint8_t*, int) override {}
};
static MidiToAmy g_midiHandler;
static syn::MidiParser g_midi(g_midiHandler);

static void handleCmd(const Cmd& c) {
  switch (c.type) {
    case CMD_NOTE_ON:    noteOn(c.a, c.b); break;
    case CMD_NOTE_OFF:   noteOff(c.a); break;
    case CMD_LOAD_PATCH: panic(); amyLoadPatch(); break;
    case CMD_FILTER:     amyApplyFilter(); break;
    case CMD_REVERB:     amyApplyReverb(); break;
    case CMD_PANIC:      panic(); break;
    case CMD_LEVEL:      amyApplyLevel(); break;
  }
}

static void pollInputs() {
  while (Serial1.available()) g_midi.feed((uint8_t)Serial1.read());
  Cmd c;
  while (xQueueReceive(g_cmdq, &c, 0) == pdTRUE) handleCmd(c);
}

// ---------------------------------------------------------------------------
// Audio task: owns AMY. amy_start() must run in the same task as amy_update().
// ---------------------------------------------------------------------------
static void audioTask(void*) {
  amy_config_t cfg = amy_default_config();
  cfg.audio = AMY_AUDIO_IS_NONE;      // we take the blocks and feed M5Unified's speaker
  cfg.midi = AMY_MIDI_IS_NONE;        // MIDI is parsed here and injected
  cfg.features.default_synths = 0;
  cfg.features.startup_bleep = 0;
  cfg.features.partials = 0;
  cfg.features.custom = 0;
  cfg.features.echo = 0;
  cfg.features.reverb = 1;
  cfg.features.chorus = 1;            // Juno patches use it
  cfg.platform.multithread = 0;       // render in this task...
  cfg.platform.multicore = 1;         // ...with half the oscillators on the other core
  cfg.max_oscs = 100;                 // no PSRAM on the Cardputer: keep pools small
  cfg.max_voices = 12;
  cfg.max_synths = 17;
  cfg.max_buses = 1;
  cfg.max_sequencer_tags = 8;
  cfg.max_memory_patches = 2;
  amy_start(cfg);
  amyLoadPatch();
  amyApplyReverb();
  g_amyReady = true;

  // Mono output chunks. Render one ahead while up to two are queued in the
  // speaker, so the codec always has ~12-17 ms in hand.
  static int16_t pcm[3][AMY_BLOCK_SIZE];
  int bi = 0;
  const float blockUs = 1e6f * AMY_BLOCK_SIZE / AMY_SAMPLE_RATE;
  for (;;) {
    pollInputs();
    uint32_t t0 = micros();
    int16_t* st = amy_update();  // AMY_BLOCK_SIZE stereo frames, reused next call
    for (int i = 0; i < AMY_BLOCK_SIZE; i++) pcm[bi][i] = (int16_t)(((int32_t)st[2 * i] + st[2 * i + 1]) >> 1);
    float load = (micros() - t0) / blockUs;
    g_cpu = g_cpu * 0.9f + 0.1f * load;
    if (load > g_cpuPeak) g_cpuPeak = load;
    int held = 0;
    for (int n = 0; n < 128; n++) held += g_noteCount[n] > 0;
    g_activeNotes = held;

    while (M5.Speaker.isPlaying(0) >= 2) {
      vTaskDelay(1);
      pollInputs();
    }
    M5.Speaker.playRaw(pcm[bi], AMY_BLOCK_SIZE, AMY_SAMPLE_RATE, false, 1, 0, false);
    bi = (bi + 1) % 3;
  }
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
static uint32_t g_saveAt = 0;
static void saveSoon() { g_saveAt = millis() + 2000; }
static void loadSettings() {
  g_prefs.begin("camy", true);
  Settings s;
  if (g_prefs.getBytesLength("set") == sizeof(Settings) && g_prefs.getBytes("set", &s, sizeof(s)) && s.version == 2)
    g_set = s;
  g_prefs.end();
  if (g_set.bank > BANK_PIANO) g_set.bank = BANK_JUNO;
}
static void saveNow() {
  g_prefs.begin("camy", false);
  g_prefs.putBytes("set", &g_set, sizeof(Settings));
  g_prefs.end();
  g_saveAt = 0;
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------
static M5Canvas g_canvas(&M5Cardputer.Display);
static bool   g_dirty = true;
static int    g_sel = 0, g_scroll = 0, g_octave = 0;
static String g_toast;
static uint32_t g_toastUntil = 0;
static void toast(const String& s, uint32_t ms = 1500) { g_toast = s; g_toastUntil = millis() + ms; g_dirty = true; }

enum ParamId { P_BANK, P_PATCH, P_LEVEL, P_VOICES, P_CUTOFF, P_RES, P_REVERB, P_MIDICH, P_VOLUME, P_COUNT };
static const char* const kParamNames[P_COUNT] = {"Bank", "Patch", "Bank Level", "Voices", "Filter Cutoff", "Filter Res",
                                                 "Reverb", "MIDI Channel", "Volume"};
static const char* const kBankNames[3] = {"JUNO-6", "DX7", "PIANO"};

static void selectPatch(int bank, int idx) {
  g_set.bank = (uint8_t)((bank % 3 + 3) % 3);
  if (g_set.bank < 2) g_set.patch[g_set.bank] = (uint8_t)((idx % 128 + 128) % 128);
  sendCmd(CMD_LOAD_PATCH);
  saveSoon();
  g_dirty = true;
}

static String paramValue(int id) {
  switch (id) {
    case P_BANK:   return kBankNames[g_set.bank];
    case P_PATCH:  return g_set.bank == BANK_PIANO ? String("-") : String(g_set.patch[g_set.bank] + 1);
    case P_LEVEL:  return String(g_set.level[g_set.bank]) + "%";
    case P_VOICES: return String(g_set.voices[g_set.bank]);
    case P_CUTOFF: return g_set.cutoff == OVERRIDE_OFF ? String("PATCH") : String(g_set.cutoff);
    case P_RES:    return g_set.res == OVERRIDE_OFF ? String("PATCH") : String(g_set.res);
    case P_REVERB: return g_set.reverb ? String(g_set.reverb) : String("OFF");
    case P_MIDICH: return g_set.midiCh ? String(g_set.midiCh) : String("OMNI");
    case P_VOLUME: return String(g_set.volume * 100 / 255) + "%";
  }
  return "";
}

static void changeParam(int id, int d) {
  auto clampi = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
  switch (id) {
    case P_BANK: {
      int nb = ((int)g_set.bank + (d > 0 ? 1 : 2)) % 3;
      selectPatch(nb, nb < 2 ? g_set.patch[nb] : 0);
      return;
    }
    case P_PATCH:  if (g_set.bank < 2) selectPatch(g_set.bank, g_set.patch[g_set.bank] + d); return;
    case P_LEVEL:  g_set.level[g_set.bank] = clampi(g_set.level[g_set.bank] + d * 5, 0, 250);
                   sendCmd(CMD_LEVEL); break;
    case P_VOICES: g_set.voices[g_set.bank] = clampi(g_set.voices[g_set.bank] + (d > 0 ? 1 : -1), 1, 10);
                   sendCmd(CMD_LOAD_PATCH); break;
    case P_CUTOFF: {  // PATCH sits above 127; stepping down from it starts at 127
      int v = g_set.cutoff == OVERRIDE_OFF ? 128 : g_set.cutoff;
      g_set.cutoff = clampi(v + d, 0, 128);
      sendCmd(CMD_FILTER);
      break;
    }
    case P_RES: {
      int v = g_set.res == OVERRIDE_OFF ? -1 : g_set.res;  // PATCH sits below 0
      v = clampi(v + d, -1, 127);
      g_set.res = v < 0 ? OVERRIDE_OFF : v;
      if (v < 0) sendCmd(CMD_LOAD_PATCH); else sendCmd(CMD_FILTER);
      break;
    }
    case P_REVERB: g_set.reverb = clampi(g_set.reverb + d, 0, 127); sendCmd(CMD_REVERB); break;
    case P_MIDICH: g_set.midiCh = clampi(g_set.midiCh + (d > 0 ? 1 : -1), 0, 16); break;
    case P_VOLUME: g_set.volume = clampi(g_set.volume + d * 2, 0, 255); M5.Speaker.setVolume(g_set.volume); break;
  }
  if (id == P_CUTOFF && g_set.cutoff == OVERRIDE_OFF) sendCmd(CMD_LOAD_PATCH);  // back to the patch's own filter
  saveSoon();
  g_dirty = true;
}

static uint16_t bankColor() {
  return g_set.bank == BANK_JUNO ? 0xFC60 : (g_set.bank == BANK_DX7 ? 0x2D7F : 0x9E7F);
}

static void draw() {
  M5Canvas& c = g_canvas;
  const int W = 240;
  c.fillScreen(TFT_BLACK);
  c.fillRect(0, 0, W, 14, bankColor());
  c.setFont(&fonts::Font0);
  c.setTextColor(TFT_BLACK, bankColor());
  char hdr[48];
  int pn = amyPatchNumber();
  snprintf(hdr, sizeof(hdr), "AMY %s", kBankNames[g_set.bank]);
  c.drawString(hdr, 3, 3);
  char st[24];
  snprintf(st, sizeof(st), "%2d%% pk%d%%", (int)(g_cpu * 100), (int)(g_cpuPeak * 100));
  c.drawRightString(st, W - 14, 3);
  c.fillCircle(W - 6, 7, 3, millis() - g_midiActivityMs < 150 ? TFT_GREEN : 0x2104);

  // big patch name
  c.setTextColor(TFT_WHITE, TFT_BLACK);
  c.setFont(&fonts::Font2);
  char pl[40];
  if (g_set.bank == BANK_PIANO) snprintf(pl, sizeof(pl), "%s", kAmyPatchNames[256]);
  else snprintf(pl, sizeof(pl), "%03d  %s", g_set.patch[g_set.bank] + 1, kAmyPatchNames[pn]);
  c.drawString(pl, 4, 18);
  c.setFont(&fonts::Font0);
  c.setTextColor(0x8410, TFT_BLACK);
  char oct[12];
  snprintf(oct, sizeof(oct), "OCT %+d", g_octave);
  c.drawRightString(oct, W - 3, 22);

  const int rows = 4, rowH = 18, y0 = 40;
  if (g_sel < g_scroll) g_scroll = g_sel;
  if (g_sel >= g_scroll + rows) g_scroll = g_sel - rows + 1;
  c.setFont(&fonts::Font2);
  for (int r = 0; r < rows; r++) {
    int i = g_scroll + r;
    if (i >= P_COUNT) break;
    int y = y0 + r * rowH;
    bool sel = i == g_sel;
    if (sel) c.fillRoundRect(0, y - 1, W - 6, rowH, 3, 0x2945);
    c.setTextColor(sel ? TFT_WHITE : 0xAD55);
    c.drawString(kParamNames[i], 6, y);
    c.setTextColor(sel ? bankColor() : TFT_WHITE);
    c.drawRightString(paramValue(i), W - 12, y);
  }
  int h = rows * rowH, bh = h * rows / P_COUNT;
  c.fillRect(W - 3, y0 + (h - bh) * g_scroll / (P_COUNT - rows), 2, bh, 0x7BEF);

  c.setFont(&fonts::Font0);
  if (millis() < g_toastUntil) {
    c.fillRect(0, 122, W, 13, TFT_WHITE);
    c.setTextColor(TFT_BLACK, TFT_WHITE);
    c.drawCentreString(g_toast, W / 2, 125);
  } else {
    c.setTextColor(0x8410, TFT_BLACK);
    c.drawString("[] patch tab bank ;. sel ,/ val -= oct", 3, 125);
  }
  c.pushSprite(0, 0);
}

static void applyRotation() {
  auto& d = M5Cardputer.Display;
  if (g_set.rotation <= 3) d.setRotation(g_set.rotation);
  else {
    d.setRotation(1);
    if (d.width() < d.height()) d.setRotation(0);
    if (d.width() < d.height()) d.setRotation(3);
  }
  d.fillScreen(TFT_BLACK);
}

// --- keyboard ---------------------------------------------------------------
static int pianoOffset(char c) {
  static const char* lower = "zsxdcvgbhnjm";
  static const char* upper = "q2w3er5t6y7ui9o0p";
  if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
  if (!c) return -1;
  const char* p = strchr(lower, c);
  if (p) return (int)(p - lower);
  p = strchr(upper, c);
  if (p) return 12 + (int)(p - upper);
  return -1;
}

static int8_t   g_pianoNote[128];
static bool     g_prevKeys[128];
static bool     g_prevEnter = false, g_prevTab = false, g_prevDel = false;
static char     g_repeatKey = 0;
static uint32_t g_repeatAt = 0;

static void uiKey(char k) {
  switch (k) {
    case ';': g_sel = (g_sel - 1 + P_COUNT) % P_COUNT; break;
    case '.': g_sel = (g_sel + 1) % P_COUNT; break;
    case ',': changeParam(g_sel, -1); break;
    case '/': changeParam(g_sel, +1); break;
    case '<': changeParam(g_sel, -10); break;
    case '?': changeParam(g_sel, +10); break;
    case '[': if (g_set.bank < 2) selectPatch(g_set.bank, g_set.patch[g_set.bank] - 1); break;
    case ']': if (g_set.bank < 2) selectPatch(g_set.bank, g_set.patch[g_set.bank] + 1); break;
    case '{': if (g_set.bank < 2) selectPatch(g_set.bank, g_set.patch[g_set.bank] - 8); break;
    case '}': if (g_set.bank < 2) selectPatch(g_set.bank, g_set.patch[g_set.bank] + 8); break;
    case '-': g_octave = max(-3, g_octave - 1); break;
    case '=': g_octave = min(3, g_octave + 1); break;
    case '`': {
      static const uint8_t order[4] = {1, 3, 0, 2};
      uint8_t cur = M5Cardputer.Display.getRotation() & 3;
      int i = 0;
      while (order[i] != cur) i++;
      g_set.rotation = order[(i + 1) % 4];
      applyRotation();
      saveSoon();
      break;
    }
    default: break;
  }
  g_dirty = true;
}

static void handleKeyboard() {
  Keyboard_Class::KeysState st = M5Cardputer.Keyboard.keysState();
  bool now[128] = {};
  for (char ch : st.word) now[(uint8_t)ch & 127] = true;
  for (int ch = 0; ch < 128; ch++) {
    int off = pianoOffset((char)ch);
    if (off >= 0) {
      if (now[ch] && !g_prevKeys[ch]) {
        int note = 48 + g_octave * 12 + off;
        if (note >= 0 && note < 128) { g_pianoNote[ch] = note; sendCmd(CMD_NOTE_ON, note, 100); }
      } else if (!now[ch] && g_prevKeys[ch] && g_pianoNote[ch] >= 0) {
        sendCmd(CMD_NOTE_OFF, g_pianoNote[ch]);
        g_pianoNote[ch] = -1;
      }
    } else if (now[ch] && !g_prevKeys[ch]) {
      uiKey((char)ch);
      if (strchr(";.,/<?[]", ch)) { g_repeatKey = (char)ch; g_repeatAt = millis() + 350; }
    }
  }
  if (st.tab && !g_prevTab) changeParam(P_BANK, +1);
  if (st.enter && !g_prevEnter) { g_sel = (g_sel + 1) % P_COUNT; g_dirty = true; }
  if (st.del && !g_prevDel) { sendCmd(CMD_PANIC); toast("All notes off"); }
  g_prevTab = st.tab; g_prevEnter = st.enter; g_prevDel = st.del;
  memcpy(g_prevKeys, now, sizeof(now));
  if (g_repeatKey) {
    if (!now[(uint8_t)g_repeatKey]) g_repeatKey = 0;
    else if ((int32_t)(millis() - g_repeatAt) >= 0) { uiKey(g_repeatKey); g_repeatAt = millis() + 70; }
  }
}

// ---------------------------------------------------------------------------
void setup() {
  auto cfg = M5.config();
  cfg.internal_mic = false;
  M5Cardputer.begin(cfg, true);
  loadSettings();
  applyRotation();
  memset(g_pianoNote, -1, sizeof(g_pianoNote));

  M5.Speaker.end();
  auto sc = M5.Speaker.config();
  sc.sample_rate = 48000;  // AMY renders 44.1 kHz; M5Unified resamples
  sc.dma_buf_len = 128;
  sc.dma_buf_count = 8;
  sc.task_priority = 6;
  sc.task_pinned_core = 0;
  M5.Speaker.config(sc);
  M5.Speaker.begin();
  M5.Speaker.setVolume(g_set.volume);

  Serial1.setRxBufferSize(1024);
  Serial1.begin(31250, SERIAL_8N1, MIDI_RX_PIN, MIDI_TX_PIN);

  g_canvas.setColorDepth(8);  // 32 KB instead of 64: leave RAM for AMY
  g_canvas.createSprite(240, 135);

  g_cmdq = xQueueCreate(64, sizeof(Cmd));
  // AMY's helper render task runs on core 0, so the AMY "main" task goes on core 1.
  xTaskCreatePinnedToCore(audioTask, "amy", 16384, nullptr, 5, nullptr, 1);
  uint32_t t = millis();
  while (!g_amyReady && millis() - t < 3000) delay(10);
  toast(String("AMY ready, ") + (ESP.getFreeHeap() / 1024) + " KB free", 2500);
}

void loop() {
  M5Cardputer.update();
  handleKeyboard();

  if (g_pendingProgram >= 0) {
    int p = g_pendingProgram;
    g_pendingProgram = -1;
    if (g_set.bank < 2) selectPatch(g_set.bank, p);
  }
  if (g_saveAt && millis() > g_saveAt) saveNow();

  // Freeze the screen while notes are held (keeps the cache/CPU for audio),
  // except right after an edit.
  static uint32_t lastDraw = 0;
  static bool wasPlaying = false;
  bool playing = g_activeNotes > 0;
  if (playing && !wasPlaying) g_cpuPeak = 0.f;
  uint32_t since = millis() - lastDraw;
  bool want = playing ? g_dirty : (wasPlaying || g_dirty || since > 250 || millis() < g_toastUntil + 300);
  if (want && since > 30) {
    lastDraw = millis();
    draw();
    g_dirty = false;
  }
  wasPlaying = playing;
  delay(5);
}
