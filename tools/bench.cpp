#include <cstdio>
#include <chrono>
#include "juno.h"
#include "dx7.h"
using namespace syn;
template<class E> double bench(E& e, int nv){
  for(int i=0;i<nv;i++) e.noteOn(48+i*3,100);
  float b[BLOCK]; int blocks=SAMPLE_RATE*5/BLOCK;
  auto t0=std::chrono::high_resolution_clock::now();
  for(int i=0;i<blocks;i++) e.render(b,BLOCK);
  auto t1=std::chrono::high_resolution_clock::now();
  double s=std::chrono::duration<double>(t1-t0).count();
  printf("active %d  ns/sample %.1f  ns/voice-sample %.1f\n", e.activeVoices(), s*1e9/(blocks*BLOCK), s*1e9/(blocks*BLOCK)/nv);
  return s;
}
int main(){ initTables(); static JunoEngine j; j.loadPatch(kJunoPresets[7]); bench(j,6);
 static Dx7Engine d; d.loadPatch(kDx7Presets[4]); bench(d,8); }
