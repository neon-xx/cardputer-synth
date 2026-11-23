// midi_parser.h - byte-stream MIDI 1.0 parser with running status and SysEx.
#pragma once
#include <stdint.h>

namespace syn {

class MidiParser {
 public:
  static constexpr int SYSEX_MAX = 4200;  // fits a DX7 32-voice dump (4104 bytes)

  struct Handler {
    virtual void onNoteOn(uint8_t ch, uint8_t note, uint8_t vel) = 0;
    virtual void onNoteOff(uint8_t ch, uint8_t note) = 0;
    virtual void onCC(uint8_t ch, uint8_t cc, uint8_t val) = 0;
    virtual void onProgram(uint8_t ch, uint8_t prog) = 0;
    virtual void onBend(uint8_t ch, int value) = 0;  // -8192..8191
    virtual void onSysEx(const uint8_t* data, int len) = 0;  // includes F0..F7
  };

  explicit MidiParser(Handler& h) : h_(h) {}

  void feed(uint8_t b) {
    if (b >= 0xF8) return;  // realtime bytes may appear anywhere: ignore
    if (b == 0xF0) { inSysex_ = true; sxLen_ = 0; sx_[sxLen_++] = b; return; }
    if (inSysex_) {
      if (b == 0xF7) {
        if (sxLen_ < SYSEX_MAX) sx_[sxLen_++] = b;
        inSysex_ = false;
        h_.onSysEx(sx_, sxLen_);
        return;
      }
      if (b & 0x80) { inSysex_ = false; }  // aborted by a status byte, fall through
      else { if (sxLen_ < SYSEX_MAX) sx_[sxLen_++] = b; return; }
    }
    if (b & 0x80) {
      if (b >= 0xF0) { status_ = 0; return; }  // system common: drop, clears running status
      status_ = b; nData_ = 0;
      return;
    }
    if (!status_) return;
    data_[nData_++] = b;
    uint8_t type = status_ & 0xF0, ch = status_ & 0x0F;
    int need = (type == 0xC0 || type == 0xD0) ? 1 : 2;
    if (nData_ < need) return;
    nData_ = 0;
    switch (type) {
      case 0x80: h_.onNoteOff(ch, data_[0]); break;
      case 0x90: if (data_[1]) h_.onNoteOn(ch, data_[0], data_[1]); else h_.onNoteOff(ch, data_[0]); break;
      case 0xB0: h_.onCC(ch, data_[0], data_[1]); break;
      case 0xC0: h_.onProgram(ch, data_[0]); break;
      case 0xE0: h_.onBend(ch, (int)((data_[1] << 7) | data_[0]) - 8192); break;
      default: break;  // poly/channel aftertouch ignored
    }
  }

 private:
  Handler& h_;
  uint8_t status_ = 0, data_[2] = {0, 0}, nData_ = 0;
  bool    inSysex_ = false;
  uint8_t sx_[SYSEX_MAX];
  int     sxLen_ = 0;
};

}  // namespace syn
