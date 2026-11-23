#include "synth_common.h"

namespace syn {

float g_sine[SINE_SIZE + 1];

void initTables() {
  for (int i = 0; i <= SINE_SIZE; i++) g_sine[i] = sinf(6.28318530718f * i / SINE_SIZE);
}

}  // namespace syn
