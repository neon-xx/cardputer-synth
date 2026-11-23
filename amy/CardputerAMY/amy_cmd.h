// amy_cmd.h - UI -> audio task commands.
// In a header so the Arduino .ino preprocessor sees it before its generated prototypes.
#pragma once
#include <stdint.h>

enum CmdType : uint8_t { CMD_NOTE_ON, CMD_NOTE_OFF, CMD_LOAD_PATCH, CMD_FILTER, CMD_REVERB, CMD_PANIC };
struct Cmd { uint8_t type, a, b; };
