// Flash adapter: lifeline::Flash on one append-only LittleFS file, plus a MODELED power loss.
// The torn-write fault is injected in software (Wokwi cannot cut power mid-write); everything else
// (bytes, CRC, recovery) is the real core running on the real LittleFS inside the simulated chip.
#pragma once
#include <Arduino.h>
#include <LittleFS.h>
#include "ll_protocol.h"

class LittleFsFlash : public lifeline::Flash {
 public:
  LittleFsFlash(const char* path, size_t cap) : path_(path), cap_(cap) {}

  size_t size() const override {
    File f = LittleFS.open(path_, "r");
    if (!f) return 0;
    size_t n = f.size();
    f.close();
    return n;
  }
  size_t capacity() const override { return cap_; }

  size_t read(size_t off, void* buf, size_t n) const override {
    File f = LittleFS.open(path_, "r");
    if (!f) return 0;
    size_t sz = f.size();
    if (off >= sz) { f.close(); return 0; }
    if (n > sz - off) n = sz - off;
    f.seek(off);
    size_t got = f.read(static_cast<uint8_t*>(buf), n);
    f.close();
    return got;
  }

  bool append(const void* buf, size_t n) override {
    if (dead_) return false;
    if (size() + n > cap_) return false;
    size_t w = n;
    if (armed_) {  // modeled power loss: only the first torn_after_ bytes reach the file
      w = torn_after_ < n ? torn_after_ : n;
      armed_ = false;
      dead_ = true;
    }
    File f = LittleFS.open(path_, "a");
    if (!f) { dead_ = true; return false; }
    size_t k = (w > 0) ? f.write(static_cast<const uint8_t*>(buf), w) : 0;
    f.close();  // close() commits the data to LittleFS
    return !dead_ && k == n;
  }

  // Arduino's File has no truncate(): rewrite the kept prefix to a temp file, then rename over the
  // original (LittleFS rename is atomic). Only used to cut a torn tail during recovery.
  void truncate(size_t n) override {
    if (dead_) return;
    size_t sz = size();
    if (n >= sz) return;
    cut_ += sz - n;
    uint8_t* tmp = static_cast<uint8_t*>(malloc(n ? n : 1));
    if (!tmp) return;
    size_t got = read(0, tmp, n);
    char t[48];
    snprintf(t, sizeof t, "%s.tmp", path_);
    File f = LittleFS.open(t, "w");
    if (f) {
      f.write(tmp, got);
      f.close();
      LittleFS.rename(t, path_);
    }
    free(tmp);
  }

  bool sync() override { return !dead_; }  // every append is closed (flushed) before it returns

  // --- demo fault injection ---
  void arm_torn(size_t bytes) { armed_ = true; torn_after_ = bytes; }
  bool dead() const { return dead_; }
  void power_on() { dead_ = false; armed_ = false; cut_ = 0; }
  size_t bytes_cut() const { return cut_; }  // torn bytes removed by recovery since power-on  // a reboot clears the modeled power loss

 private:
  const char* path_;
  size_t cap_;
  bool armed_ = false;
  bool dead_ = false;  // "power is off" until the next boot
  size_t torn_after_ = 0;
  size_t cut_ = 0;
};
