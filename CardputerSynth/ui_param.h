// ui_param.h - parameter descriptor for the on-screen editor.
// Lives in a header so the Arduino .ino preprocessor sees it before its auto-generated prototypes.
#pragma once
#include <stdint.h>

enum PFlag : uint8_t { PF_NONE = 0, PF_BIT = 1, PF_SETTING = 2 };
struct Param {
  const char*        name;
  uint8_t*           ptr;
  int16_t            mn, mx;
  int16_t            dispOffset;   // shown value = raw + dispOffset
  const char* const* labels;
  uint8_t            flags;
  uint8_t            bit;
};
