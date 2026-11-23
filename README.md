# CardputerSynth

Firmware for the **M5Stack Cardputer-Adv** that turns it into either a
**Juno-106-style analog polysynth** or a **DX7-style 6-operator FM synth**
(one at a time, switch with `Tab`). You play it from a MIDI keyboard through the
**M5Stack Unit MIDI (U187)**, or from the Cardputer's own keys.

Sound comes out of the Cardputer's speaker and its 3.5 mm jack (ES8311 codec).
The Unit MIDI's built-in SAM2695 GM synth is not used.

## Hardware setup

1. Connect the Unit MIDI to the Cardputer's Grove port with the supplied cable.
2. **Set the Unit MIDI DIP switch to BYPASS.** In SEPARATE mode the unit does
   not pass MIDI IN to the Grove port, so the Cardputer would hear nothing.
3. Connect your keyboard's MIDI OUT to the Unit MIDI's **IN** socket (5-pin DIN
   or 3.5 mm TRS). In Bypass mode the IN signal is also echoed to its OUT socket.
4. Plug headphones or an amp into the Cardputer's 3.5 mm jack, or use the speaker.

| Grove wire | Cardputer-Adv pin | Unit MIDI signal |
|---|---|---|
| White  | G1 (UART RX, 31250 baud) | TX (MIDI IN) |
| Yellow | G2 (UART TX) | RX (to the SAM2695, unused) |

## Building

**Arduino IDE**
- Board package: M5Stack ≥ 3.2.2, board **M5Cardputer**
- Libraries: M5Cardputer ≥ 1.1.0 (adds the Adv keyboard), M5Unified ≥ 0.2.8, M5GFX ≥ 0.2.10
- Open `CardputerSynth/CardputerSynth.ino` and upload.

**PlatformIO**: `pio run -t upload` (see `platformio.ini`).

To flash, hold G0 while plugging in USB if the board doesn't enter download mode by itself.

## Playing from the Cardputer keyboard

The letter and number keys are a two-octave piano (tracker layout):

```
 2 3   5 6 7   9 0         <- black keys, upper octave
q w e r t y u i o p        <- C D E F G A B C D E
 s d   g h j               <- black keys, lower octave
z x c v b n m              <- C D E F G A B
```

| Key | Action |
|---|---|
| `Tab` | Switch Juno ↔ DX7 |
| `[` `]` | Previous / next patch (Juno) or voice (DX7) |
| `{` `}` (Shift+`[` `]`) | Previous / next DX7 bank from the SD card |
| `;` `.` | Move up / down the parameter list (Shift = jump 5) |
| `,` `/` | Decrease / increase the selected value (Shift = big steps); hold to repeat |
| `Enter` | Next page |
| `-` `=` | Octave down / up for the Cardputer keys |
| `\` | Save (Juno → user slot U1–U8 in flash; DX7 → `/dx7/USER.syx`) |
| `Backspace` | Panic (all notes off) |
| `` ` `` (top-left key) | Rotate the screen (cycles through all four, remembered) |

The header shows the patch, active voices, audio CPU load, the peak load (`pk`) of the last thing you played, and a MIDI activity LED. To keep the audio clean, the screen doesn't update while notes are sounding, except when you press an editing key.

## The engines

**Juno (6 voices).** Per voice: DCO with band-limited saw, pulse with PWM
(LFO, manual or envelope), sub-oscillator and noise, a 24 dB resonant ladder
filter that self-oscillates, and an ADSR or gate VCA. Global: triangle LFO with
delay, the 106's four-position HPF, and a BBD-style chorus (I, II, I+II).
There are 8 presets (P1–P8) and 8 user slots (U1–U8). Saving a preset with `\`
writes it to the matching U slot. A "Velocity" control is added. Set it to 0 for
an authentic, velocity-free 106.

**DX7 (8 voices).** Six operators with all 32 algorithms, operator feedback,
4-rate/4-level EGs, the DX7 output-level, velocity, key-level and rate-scaling
model, ratio/fixed frequencies with detune, pitch EG, LFO (6 waves, delay, PMD,
AMD, sensitivities) and osc key sync. Every DX7 voice parameter is editable on
the VOICE and OPERATOR 1–6 pages. The VOICE page also has a Juno-style
**resonant 24 dB low-pass filter** on the DX output (Filter Cutoff / Filter Res,
CC 74 / 71; fully open = bypassed), and **Brightness / Attack / Release** macros
that offset modulator levels and EG rates. None of these change the stored patch.

The internal bank has 8 original voices. For the classic sounds, load real DX7
cartridges:

- Copy `.syx` files to **`/dx7/`** on a FAT32 microSD card. It accepts standard
  32-voice bulk dumps (4104 bytes), raw 4096-byte VMEM files and single-voice dumps.
- Use `{` `}` to change bank and `[` `]` to pick a voice.
- You can also send a DX7 bulk dump or single voice over MIDI SysEx. It's loaded
  automatically.

## MIDI implementation

The MIDI channel (Omni or 1–16), bend range and volume are on the SETUP page and
are saved to flash.

| Message | Juno | DX7 |
|---|---|---|
| Note on/off, sustain (CC64), pitch bend | ✓ | ✓ |
| CC1 mod wheel | adds LFO vibrato | adds LFO vibrato |
| CC7 | master volume | master volume |
| CC71 / 74 | resonance / cutoff | output filter resonance / cutoff |
| CC70 | – | brightness macro |
| CC73 / 75 / 79 / 72 | attack / decay / sustain / release | attack macro (73) / release macro (72) |
| CC76 / 77 / 78 | LFO rate / LFO→DCO / LFO delay | LFO speed (76) |
| CC80–85 | env amt, PWM, VCF LFO, sub, noise, key follow | – |
| CC91 / 93 | chorus mode | – |
| Program change | patch 0–15 | voice in the current bank |
| CC120 / 123 | all notes off | all notes off |
| SysEx | – | DX7 32-voice and single-voice dumps |

## Notes and limits

- The code was compiled and tested on a PC: every preset was rendered to audio,
  pitch was checked, random patches were fuzzed (no NaNs or blow-ups) and the
  SysEx parsing was verified with a round trip. It **hasn't been run on a real
  Cardputer-Adv yet**. CPU use, audio latency and the M5Unified speaker setup
  are the things to check first.
- Long releases: a peak limiter stops piled-up notes from distorting, stolen
  voices fade instead of cutting off, and if the CPU falls behind the quietest
  released voice is faded out (~5 ms) rather than letting the audio drop out.
- If the CPU readout is still regularly above ~85% or you hear crackles, lower
  `Dx7Engine::NV` (dx7.h) or `JunoEngine::NV` (juno.h), or change `SAMPLE_RATE`
  in `synth_common.h` to 32000.
- The DX7 engine follows the original's level, envelope and scaling maths but is
  not bit-exact. LFO and pitch-EG curves, and the two feedback-loop algorithms
  (4 and 6), are approximations.
- The output is mono, because the ES8311 is a mono codec. The Juno chorus is
  mixed to mono the way summing a real Juno's L/R outputs would be.

## CardputerAMY: the AMY-engine version

`amy/` is a second firmware built on [AMY](https://github.com/shorepine/amy),
the synth engine behind AMYboard and Tulip. Instead of my engines it uses AMY's
own models and patch banks:

- **128 Juno-6 patches** (A11–B88), **128 DX7 patches** and AMY's modelled **piano**
- AMY handles voice allocation, the Juno chorus, envelopes, velocity, sustain, bend and mod wheel
- It renders on both CPU cores. The sketch hands AMY's 44.1 kHz blocks to the
  ES8311 through M5Unified, the same audio path as CardputerSynth.

Hardware setup is the same: Unit MIDI on the Grove port, set to **BYPASS**.

| Key | Action |
|---|---|
| `Tab` | Next bank: Juno-6 → DX7 → Piano (each bank remembers its patch) |
| `[` `]` | Previous / next patch; `{` `}` jump 8 |
| `;` `.` / `Enter` | Move through the settings |
| `,` `/` | Change the selected setting (Shift = ×10) |
| letters/numbers | Piano keys (same layout as above); `-` `=` octave |
| `` ` `` | Rotate the screen |
| `Backspace` | All notes off |

Settings: Bank, Patch, Bank Level (per-bank loudness trim, 0–250%), Voices (1–10, per bank), Filter Cutoff and Filter Res
(**PATCH** means the patch's own setting; on DX7/piano moving the cutoff adds a
24 dB low-pass), Reverb, MIDI Channel and Volume. All of them are saved to flash.

MIDI: notes, sustain, bend and mod wheel go to AMY. CC 74 / 71 set filter cutoff /
resonance, CC 91 sets reverb, CC 7 sets volume, and program change selects a patch
in the current bank.

Build: `pio run -d amy` (CI publishes `CardputerAMY-app.bin` / `-full.bin`).
For the Arduino IDE, open `amy/CardputerAMY/CardputerAMY.ino`. Install the AMY
library from GitHub at commit `9cdf615`; the patch-name table matches that version.

The Cardputer has no PSRAM, so AMY runs with small pools: 100 oscillators,
echo and partials off, and reverb and chorus on. Free memory is shown at startup.

## Files

```
CardputerSynth/CardputerSynth.ino  hardware, audio task, MIDI routing, UI
CardputerSynth/juno.*              Juno engine + presets
CardputerSynth/dx7.*               DX7 engine, SysEx (un)packing
CardputerSynth/dx7_presets.cpp     internal FM voices
CardputerSynth/midi_parser.h       MIDI byte parser (running status, SysEx)
CardputerSynth/synth_common.*      shared DSP helpers, engine base class
tools/                             host-side tests (g++), not part of the firmware
amy/                               CardputerAMY (AMY engine version) + its platformio.ini
```
