// Long-release stress: fast repeated notes so voices pile up and get stolen/shed.
#include <cstdio>
#include <cmath>
#include "juno.h"
#include "dx7.h"
using namespace syn;
template<class E> void run(E& e, const char* name){
  float b[BLOCK]; double maxJump=0, peak=0; float prev=0; int note=0;
  for(int k=0;k<SAMPLE_RATE*6/BLOCK;k++){
    if(k%20==0){ e.noteOff(48+(note%14)*2); note++; e.noteOn(48+(note%14)*2,110); }
    if(k%7==0) e.shedVoice();
    e.render(b,BLOCK);
    for(float x:b){ maxJump=fmax(maxJump,fabs(x-prev)); prev=x; peak=fmax(peak,fabs(x)); }
  }
  printf("%-6s peak %.2f  largest sample-to-sample jump %.3f  active %d\n",name,peak,maxJump,e.activeVoices());
}
int main(){ initTables();
  static JunoEngine j; JunoPatch p=kJunoPresets[2]; p.R=127; j.loadPatch(p); run(j,"Juno");
  static Dx7Engine d; Dx7Patch q=kDx7Presets[0]; for(auto&o:q.op) o.r[3]=20; d.loadPatch(q); run(d,"DX7"); }
