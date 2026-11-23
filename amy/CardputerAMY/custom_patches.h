// custom_patches.h - original patches for AMY's other engines (Karplus-Strong,
// wavetable, raw oscillators). Each is a list of AMY osc commands ("|"-separated)
// stored as user patch 1024 when selected. gain = level match to the Juno bank.
// Tested on a host build of AMY 9cdf615 for pitch, envelope and level.
#pragma once

struct CustomPatch { const char* name; float gain; const char* cmds; };

static const CustomPatch kPluckPatches[] = {
  {"Nylon Guitar", 2.4f, "v0w6b0.996d0.42A0,1,20000,1,150,0"},
  {"Steel String", 2.2f, "v0w6b0.998d0.40A0,1,20000,1,200,0"},
  {"Harp", 2.3f, "v0w6b0.997d0.20A0,1,20000,1,400,0"},
  {"Koto", 3.0f, "v0w6b0.993d0.46A0,1,20000,1,120,0"},
  {"Muted Pluck", 3.0f, "v0w6b0.975d0.44A0,1,20000,1,60,0"},
  {"Long String", 3.4f, "v0w6b0.9992d0.48A0,1,30000,1,600,0"},
};
static const CustomPatch kWavePatches[] = {
  {"PPG Sweep", 1.1f, "v0w19p13d0.02,,,,0.95A5,1,900,0.7,400,0B0,0,2500,1,400,1"},
  {"PPG Pad", 1.0f, "v1w4a1,,0f0.25,0|v0w19p13d0.5,,,,,0.35L1C3,0.008,0,1A900,1,2000,0.8,1500,0"},
  {"Braids Keys", 2.0f, "v0w19p12d0.15,,0.6A2,1,1500,0.3,300,0"},
  {"Sine-Saw Bass", 0.6f, "v0w19p14d0.1,,,0.8A2,1,400,0.6,100,0"},
  {"111 Lead", 1.35f, "v1w4a1,,0f5,0A0,0,600,0,300,1,100,1|v0w19p11d0.6f440,1,,,,0.012,1L1m60A5,1,200,0.85,200,0"},
  {"Viral Bell", 1.3f, "v0w19p15d0.85,,,,-0.7A1,1,3000,0,800,0B0,1,1500,0,800,0"},
  {"Wave Scan", 1.3f, "v1w4a1,,0f0.12,0|v0w19p13d0.5,,,,,0.48L1A300,1,1000,0.9,800,0"},
  {"Braids Drone", 2.2f, "v1w4a1,,0f0.07,0|v0w19p12d0.5,,,,,0.45L1C4,0.006,0,1A1500,1,3000,1,2500,0"},
};
static const CustomPatch kAnalogPatches[] = {
  {"Supersaw", 2.1f, "v0w3C7,0.007,0,1A10,1,800,0.8,500,0"},
  {"Square Lead", 0.62f, "v0w1d0.5F900,0.8,,3G4R2A5,1,300,0.7,200,0"},
  {"Sub Bass", 0.7f, "v0w20F350,0.5,,2.5G4R1.5A2,1,300,0.5,120,0c1|v1w3a1,,0,0c2|v2w1d0.5f220,1a0.7,,0,0"},
  {"Sync-ish Lead", 0.93f, "v1w4a1,,0f5.5,0A0,0,500,0,400,1,100,1|v0w2C2,0.004,0,1F2500,0.6,,1.5G4R3f440,1,,,,0.01,1L1m40A3,1,200,0.8,250,0"},
  {"PWM Strings", 0.7f, "v1w4a1,,0f0.6,0|v0w1d0.5,,,,,0.35L1C3,0.005,0,1F3000,0.4,,1G4R0.8A600,1,1500,0.85,1200,0"},
  {"Resonant Bass", 0.6f, "v0w3F150,0.6,,4.5G4R6A1,1,250,0.4,80,0"},
  {"Noise Sweep", 0.45f, "v0w5F400,1,,,5G2R8A20,1,4000,0.6,1500,0B2000,1,3000,0.3,1500,0"},
  {"Soft Sine Pad", 1.3f, "v0w0C3,0.004,0,1A1200,1,2000,0.9,2000,0"},
};
