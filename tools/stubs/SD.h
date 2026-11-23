#pragma once
#include "M5Cardputer.h"
#define FILE_READ "r"
#define FILE_WRITE "w"
struct File { operator bool() const; size_t size(); size_t read(uint8_t*, size_t); size_t write(const uint8_t*, size_t);
  void close(); File openNextFile(); const char* name(); bool isDirectory(); };
struct SPIClass { void begin(int,int,int,int); };
extern SPIClass SPI;
struct SDFS { bool begin(int, SPIClass&, int); bool exists(const char*); bool mkdir(const char*); File open(const String&); File open(const char*, const char* m = "r"); };
extern SDFS SD;
