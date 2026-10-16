// Runs wokwi/sketch.ino on the host: reads commands from stdin, one per line.
//   !gas N     set the MQ2 reading        !wait MS   let time pass
//   anything else: typed into the serial monitor, then 400 ms pass
#include <iostream>
#include <string>
#define HOST_TEST 1
#include "Arduino.h"
#include "LittleFS.h"
unsigned long host_now = 0;
int host_pin[64];
SerialStub Serial;
EspStub ESP;
FsStub LittleFS;
#include "../wokwi/sketch.ino"

static void advance(unsigned ms) {
  for (unsigned t = 0; t < ms; t += 20) {
    host_now += 20;
    try { loop(); } catch (RestartException&) { Serial.in.clear(); setup(); }
  }
}
int main() {
  setup();
  std::string line;
  while (std::getline(std::cin, line)) {
    std::printf(">>> %s\n", line.c_str());
    if (line.rfind("!gas ", 0) == 0) host_gas = std::atoi(line.c_str() + 5);
    else if (line.rfind("!wait ", 0) == 0) advance((unsigned)std::atoi(line.c_str() + 6));
    else { for (char c : line) Serial.in.push_back(c); advance(400); }
    std::fflush(stdout);
  }
}
