#pragma once
// Offline substitutes for P2-04/P2-05 only. Existing SMS/config test substitutes are untouched.
// API failure injection models failed String reserve/concat/copy, not the ESP heap allocator.
#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

struct StringFaults {
  static inline int reserveAfter = -1, concatAfter = -1, copyAfter = -1;
  static inline size_t maxReserve = 0, reserveCalls = 0, concatCalls = 0, copyCalls = 0;
  static bool fail(int& after) {
    if (after < 0) return false;
    if (after-- == 0) { after = -1; return true; }
    return false;
  }
  static void reset() {
    reserveAfter = concatAfter = copyAfter = -1;
    maxReserve = reserveCalls = concatCalls = copyCalls = 0;
  }
};

class String {
  std::string value_;
  bool valid_ = true;
public:
  String() = default;
  String(const char* text) : value_(text ? text : ""), valid_(text != nullptr) {}
  String(const std::string& text) : value_(text) {}
  String(const char* text, unsigned int size) : value_(text, size) {}
  explicit String(char c) : value_(1, c) {}
  template<class T, typename std::enable_if<std::is_integral<T>::value, int>::type = 0>
  String(T number) : value_(std::to_string(number)) {}
  String(const String& other) { *this = other; }
  String(String&&) noexcept = default;
  String& operator=(String&&) noexcept = default;
  String& operator=(const String& other) {
    if (this == &other) return *this;
    StringFaults::copyCalls++;
    valid_ = other.valid_ && !StringFaults::fail(StringFaults::copyAfter);
    value_ = valid_ ? other.value_ : "";
    return *this;
  }
  explicit operator bool() const { return valid_; }
  unsigned int length() const { return static_cast<unsigned int>(value_.size()); }
  const char* c_str() const { return valid_ ? value_.c_str() : nullptr; }
  char charAt(unsigned int i) const { return value_.at(i); }
  char operator[](unsigned int i) const { return value_.at(i); }
  void setCharAt(unsigned int i, char value) { value_.at(i) = value; }
  bool reserve(unsigned int size) {
    StringFaults::reserveCalls++;
    StringFaults::maxReserve = std::max(StringFaults::maxReserve, static_cast<size_t>(size));
    if (StringFaults::fail(StringFaults::reserveAfter)) return false;
    value_.reserve(size); valid_ = true; return true;
  }
  bool concat(const char* text, unsigned int size) {
    StringFaults::concatCalls++;
    if (!text || StringFaults::fail(StringFaults::concatAfter)) return false;
    value_.append(text, size); return true;
  }
  bool concat(const String& text) { return text.valid_ && concat(text.c_str(), text.length()); }
  bool concat(char c) { return concat(&c, 1); }
  void remove(unsigned int start, unsigned int count = UINT_MAX) {
    if (start < length()) value_.erase(start, std::min(count, length() - start));
  }
  String substring(unsigned int start, unsigned int end) const {
    if (start > end) std::swap(start, end);
    if (start >= length()) return "";
    return value_.substr(start, std::min(end, length()) - start);
  }
  String substring(unsigned int start) const { return substring(start, length()); }
  int indexOf(const String& text, unsigned int start = 0) const {
    auto found = value_.find(text.value_, start);
    return found == std::string::npos ? -1 : static_cast<int>(found);
  }
  int indexOf(char c, unsigned int start = 0) const {
    auto found = value_.find(c, start);
    return found == std::string::npos ? -1 : static_cast<int>(found);
  }
  int lastIndexOf(char c) const {
    auto found = value_.rfind(c);
    return found == std::string::npos ? -1 : static_cast<int>(found);
  }
  bool startsWith(const String& text) const { return value_.rfind(text.value_, 0) == 0; }
  bool endsWith(const String& text) const {
    return length() >= text.length() && value_.compare(length() - text.length(), text.length(), text.value_) == 0;
  }
  long toInt() const { return std::strtol(c_str(), nullptr, 10); }
  void trim() {
    auto begin = value_.find_first_not_of(" \t\r\n\f\v");
    if (begin == std::string::npos) { value_.clear(); return; }
    value_ = value_.substr(begin, value_.find_last_not_of(" \t\r\n\f\v") - begin + 1);
  }
  void replace(char from, char to) { std::replace(value_.begin(), value_.end(), from, to); }
  void replace(const String& from, const String& to) {
    if (!from.length()) return;
    size_t start = 0;
    while ((start = value_.find(from.value_, start)) != std::string::npos) {
      value_.replace(start, from.length(), to.value_); start += to.length();
    }
  }
  void toLowerCase() {
    for (char& c : value_) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  String& operator+=(const String& text) { concat(text); return *this; }
  String& operator+=(const char* text) { concat(text, static_cast<unsigned int>(std::strlen(text))); return *this; }
  String& operator+=(char c) { concat(c); return *this; }
  friend String operator+(String left, const String& right) { left += right; return left; }
  friend bool operator==(const String& left, const String& right) { return left.value_ == right.value_; }
  friend bool operator!=(const String& left, const String& right) { return !(left == right); }
};

inline std::vector<std::string> testEffects;
inline uint64_t testClockMs = 0;
inline int64_t esp_timer_get_time() { return static_cast<int64_t>(testClockMs * 1000ULL); }
inline unsigned long millis() { return static_cast<uint32_t>(testClockMs); }
inline void delay(unsigned long amount) { testClockMs += amount; testEffects.push_back("delay:" + std::to_string(amount)); }
inline void yield() {}
inline void esp_task_wdt_reset() {}

struct HardwareSerial {
  std::string log;
  template<class T> void print(const T& text) { log += String(text).c_str(); }
  template<class T> void println(const T& text) { print(text); log += '\n'; }
  void println() { log += '\n'; }
  void printf(const char* format, ...) {
    char text[2048]; va_list args; va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args); va_end(args); log += text;
  }
};
inline HardwareSerial Serial;

using esp_reset_reason_t = int;
using SemaphoreHandle_t = void*;
constexpr int pdTRUE = 1;
inline unsigned long pdMS_TO_TICKS(unsigned long value) { return value; }
#include "web_persistence_host.h"

struct TestEsp {
  unsigned int restarts = 0;
  uint32_t getFreeHeap() const { return 100000; }
  uint32_t getMaxAllocHeap() const { return 80000; }
  uint32_t getMinFreeHeap() const { return 90000; }
  void restart() { restarts++; testEffects.push_back("restart"); }
};
inline TestEsp ESP;
using wifi_mode_t = int;
constexpr int WIFI_STA = 1, WIFI_AP = 2, WL_CONNECTED = 3;
struct TestIp { String toString() const { return "192.0.2.1"; } };
struct TestWifi {
  wifi_mode_t modeValue = WIFI_STA;
  String ssid = "synthetic-wifi";
  wifi_mode_t getMode() const { return modeValue; }
  int status() const { return WL_CONNECTED; }
  String SSID() const { return ssid; }
  TestIp localIP() const { return {}; }
  TestIp softAPIP() const { return {}; }
};
inline TestWifi WiFi;
struct TestMqtt { bool connected() const { return true; } };
inline TestMqtt mqttClient;
using byte = uint8_t;
constexpr int BASIC_AUTH = 1;
constexpr size_t CONTENT_LENGTH_UNKNOWN = static_cast<size_t>(-1);

class WebServer {
  struct Client {
    std::string bytes;
    bool connected() const { return true; }
    void stop() {}
    size_t write(const uint8_t* data, size_t count) { bytes.append(reinterpret_cast<const char*>(data), count); return count; }
  } connection_;
public:
  explicit WebServer(int) {}
  std::map<std::string, String> args;
  bool authorized = true;
  int statusCode = 0, declaredLength = INT_MIN;
  mutable unsigned int plainReads = 0;
  std::string body, contentType;
  bool authenticate(const char*, const char*) const { return authorized; }
  void requestAuthentication(int, const char*, const char*) { statusCode = 401; }
  String arg(const String& key) const {
    if (key == "plain") plainReads++;
    auto found = args.find(key.c_str()); return found == args.end() ? String("") : found->second;
  }
  bool hasArg(const String& key) const { return args.count(key.c_str()) != 0; }
  int clientContentLength() const {
    if (declaredLength != INT_MIN) return declaredLength;
    auto found = args.find("plain"); return found == args.end() ? 0 : static_cast<int>(found->second.length());
  }
  void send(int code, const char* mime, const String& text) {
    statusCode = code; contentType = mime; body.assign(text.c_str(), text.length());
  }
  void send(int code, const char* mime, const char* text) { statusCode = code; contentType = mime; body = text; }
  void setContentLength(size_t) {}
  void sendContent(const char*, size_t) {}
  Client& client() { return connection_; }
  String version() const { return "HTTP/1.0"; }
  const std::string& streamedBody() const { return connection_.bytes; }
  void reset() { *this = WebServer(80); }
};
