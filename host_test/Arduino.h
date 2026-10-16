// Minimal host stand-in for the Arduino/ESP32 API used by wokwi/sketch.ino (tests only).
#pragma once
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
extern unsigned long host_now;
inline unsigned long millis() { return host_now; }
inline void delay(unsigned long ms) { host_now += ms; }
#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
extern int host_pin[64];
inline void pinMode(int, int) {}
inline void digitalWrite(int p, int v) { host_pin[p] = v; }
inline int analogRead(int) { return 0; }
struct SerialStub {
  std::deque<char> in;
  void begin(int) {}
  int available() { return (int)in.size(); }
  int read() { int c = in.front(); in.pop_front(); return c; }
  void println(const char* s = "") { std::printf("%s\n", s); }
  void print(const char* s) { std::printf("%s", s); }
  int printf(const char* f, ...) {
    va_list a; va_start(a, f); int n = std::vprintf(f, a); va_end(a); return n;
  }
};
extern SerialStub Serial;
struct RestartException {};
struct EspStub { void restart() { throw RestartException(); } };
extern EspStub ESP;
