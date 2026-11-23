#include <cstdio>
#include <vector>
#include "dx7.h"
#include "midi_parser.h"
using namespace syn;
struct H : MidiParser::Handler {
  int on=0, off=0, cc=0, bend=0, sx=0; Dx7Patch got[32];
  void onNoteOn(uint8_t, uint8_t, uint8_t) override { on++; }
  void onNoteOff(uint8_t, uint8_t) override { off++; }
  void onCC(uint8_t, uint8_t, uint8_t) override { cc++; }
  void onProgram(uint8_t, uint8_t) override {}
  void onBend(uint8_t, int v) override { bend = v; }
  void onSysEx(const uint8_t* d, int len) override {
    sx = len;
    if (len >= 4104 && d[1]==0x43 && d[3]==9) for (int i=0;i<32;i++) dx7UnpackVmem(d+6+i*128, got[i]);
  }
};
int main(){
  static H h; static MidiParser p(h);
  // running status: 90 3C 64 40 64 (two note-ons) 3C 00 (note-off via vel 0), F8 clock interleaved
  uint8_t s[] = {0x90,0x3C,0x64,0xF8,0x40,0x64,0x3C,0x00,0xB0,0x01,0x10,0xE0,0x00,0x60};
  for (auto b: s) p.feed(b);
  printf("on %d off %d cc %d bend %d\n", h.on, h.off, h.cc, h.bend);
  std::vector<uint8_t> dump = {0xF0,0x43,0x00,0x09,0x20,0x00};
  uint8_t v[128]; uint8_t sum=0;
  for (int i=0;i<32;i++){ dx7PackVmem(kDx7Presets[i%DX7_PRESETS], v); for(int k=0;k<128;k++){dump.push_back(v[k]); sum+=v[k];} }
  dump.push_back((-sum)&0x7F); dump.push_back(0xF7);
  for (auto b: dump) p.feed(b);
  bool ok = true;
  for (int i=0;i<32;i++) ok &= memcmp(&h.got[i], &kDx7Presets[i%DX7_PRESETS], sizeof(Dx7Patch))==0;
  printf("sysex len %d, bank decode %s, voice 3 name '%s'\n", h.sx, ok?"OK":"MISMATCH", h.got[2].name);
}
