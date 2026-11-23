#include <cstdio>
#include <cstdlib>
#include <cmath>
#include "juno.h"
#include "dx7.h"
using namespace syn;
int main(){
  initTables(); srand(1);
  static JunoEngine j; static Dx7Engine d; float b[BLOCK]; double peakJ=0, peakD=0; int bad=0;
  for (int t=0;t<300;t++){
    JunoPatch jp = kJunoPresets[t%8]; uint8_t* raw=(uint8_t*)&jp;
    for (size_t k=12;k<sizeof(jp);k++) raw[k]=rand()%128;
    jp.dcoRange%=3; jp.pwmMode%=3; jp.pulse&=1; jp.saw&=1; jp.hpf&=3; jp.envPol&=1; jp.vcaMode&=1; jp.chorus&=3;
    j.allNotesOff(); j.loadPatch(jp);
    Dx7Patch dp; uint8_t vm[128]; for(int k=0;k<128;k++) vm[k]=rand()%128; dx7UnpackVmem(vm, dp);
    d.allNotesOff(); d.loadPatch(dp);
    for (int n=0;n<8;n++){ j.noteOn(rand()%128, 1+rand()%127); d.noteOn(rand()%128, 1+rand()%127);}
    j.setPitchBend((rand()%25)-12); d.setPitchBend((rand()%25)-12); j.setModWheel(1); d.setModWheel(1);
    for (int k=0;k<200;k++){
      j.render(b,BLOCK); for(float x:b){ if(!std::isfinite(x)) bad++; peakJ=fmax(peakJ,fabs(x)); }
      d.render(b,BLOCK); for(float x:b){ if(!std::isfinite(x)) bad++; peakD=fmax(peakD,fabs(x)); }
      if (k==100) for(int n=0;n<128;n++){ j.noteOff(n); d.noteOff(n);} 
    }
  }
  printf("non-finite samples: %d  peak juno %.2f  peak dx7 %.2f\n", bad, peakJ, peakD);
}
