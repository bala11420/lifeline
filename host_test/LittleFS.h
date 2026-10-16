// Host stand-in for LittleFS: files live in ./host_fs (tests only).
#pragma once
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <string>
class File {
 public:
  File() {}
  explicit File(FILE* f) : fp_(f) {}
  File(File&& o) : fp_(o.fp_) { o.fp_ = nullptr; }
  File(const File&) = delete;
  ~File() { close(); }
  explicit operator bool() const { return fp_ != nullptr; }
  size_t size() { long c = ftell(fp_); fseek(fp_, 0, SEEK_END); long n = ftell(fp_); fseek(fp_, c, SEEK_SET); return (size_t)n; }
  bool seek(size_t o) { return fseek(fp_, (long)o, SEEK_SET) == 0; }
  size_t read(uint8_t* b, size_t n) { return fread(b, 1, n, fp_); }
  size_t write(const uint8_t* b, size_t n) { return fwrite(b, 1, n, fp_); }
  void close() { if (fp_) { fclose(fp_); fp_ = nullptr; } }
 private:
  FILE* fp_ = nullptr;
};
struct FsStub {
  static std::string p(const char* path) { return std::string("host_fs") + path; }
  bool begin(bool) { mkdir("host_fs", 0755); return true; }
  File open(const char* path, const char* mode) { return File(fopen(p(path).c_str(), mode)); }
  bool rename(const char* a, const char* b) { return ::rename(p(a).c_str(), p(b).c_str()) == 0; }
  bool format() {
    DIR* d = opendir("host_fs");
    if (!d) return true;
    while (dirent* e = readdir(d)) { if (e->d_name[0] != '.') remove((std::string("host_fs/") + e->d_name).c_str()); }
    closedir(d);
    return true;
  }
};
extern FsStub LittleFS;
