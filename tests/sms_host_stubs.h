#pragma once
// Host-only Arduino/I/O substitutes. No firmware logic is duplicated here.
// charAt() deliberately throws on invalid access instead of Arduino's silent NUL.
#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// Monotonic scheduler time is separate from the naturally wrapping Arduino clock.
inline uint64_t smsTestTicks = 0;

class String;
enum class SmsStringOp { Copy, Reserve, Concat };
// Logical String operations, independent of host std::string capacity/SSO. One
// matching operation fails; unrelated parsing, logging and subsequent retries work.
inline std::function<bool(SmsStringOp, const String*, const char*, size_t)> smsTestStringFault;
inline size_t smsTestStringFailures = 0;
inline void resetSmsStringFault() { smsTestStringFault = nullptr; smsTestStringFailures = 0; }
inline bool failSmsStringOp(SmsStringOp op, const String* target, const char* text, size_t length) {
  if (!length || !smsTestStringFault || !smsTestStringFault(op, target, text, length)) return false;
  smsTestStringFault = nullptr;
  smsTestStringFailures++;
  return true;
}

class String {
  std::string value;
  void copy(const char* text, size_t length) {
    if (failSmsStringOp(SmsStringOp::Copy, this, text, length)) value.clear();
    else value.assign(text ? text : "", length);
  }
public:
  String() = default;
  String(const char* text) { copy(text, text ? std::strlen(text) : 0); }
  String(const std::string& text) { copy(text.data(), text.size()); }
  String(char c) { copy(&c, 1); }
  String(const String& other) { copy(other.c_str(), other.length()); }
  String(String&&) noexcept = default;
  String& operator=(const String& other) {
    if (this != &other) copy(other.c_str(), other.length());
    return *this;
  }
  String& operator=(String&&) noexcept = default;
  String& operator=(const char* text) { copy(text, text ? std::strlen(text) : 0); return *this; }
  template<class T, typename std::enable_if<std::is_integral<T>::value, int>::type = 0>
  String(T number) : String(std::to_string(number)) {}
  unsigned int length() const { return static_cast<unsigned int>(value.size()); }
  const char* c_str() const { return value.c_str(); }
  char charAt(unsigned int index) const {
    if (index >= value.size()) throw std::out_of_range("String::charAt beyond input");
    return value[index];
  }
  bool reserve(unsigned int size) {
    if (failSmsStringOp(SmsStringOp::Reserve, this, nullptr, size)) return false;
    value.reserve(size);
    return true;
  }
  bool concat(const char* text, unsigned int length) {
    if (!text || failSmsStringOp(SmsStringOp::Concat, this, text, length)) return false;
    value.append(text, length);
    return true;
  }
  bool concat(const char* text) { return text && concat(text, static_cast<unsigned int>(std::strlen(text))); }
  bool concat(const String& text) { return concat(text.c_str(), text.length()); }
  bool concat(char c) { return concat(&c, 1); }
  String substring(unsigned int start, unsigned int end) const {
    if (start > end) std::swap(start, end);
    if (start >= length()) return "";
    return value.substr(start, std::min(end, length()) - start);
  }
  String substring(unsigned int start) const { return substring(start, length()); }
  bool startsWith(const String& prefix) const { return value.rfind(prefix.value, 0) == 0; }
  int indexOf(const String& needle, unsigned int start = 0) const {
    auto index = value.find(needle.value, start);
    return index == std::string::npos ? -1 : static_cast<int>(index);
  }
  int indexOf(char needle, unsigned int start = 0) const {
    auto index = value.find(needle, start);
    return index == std::string::npos ? -1 : static_cast<int>(index);
  }
  int lastIndexOf(char needle) const {
    auto index = value.rfind(needle);
    return index == std::string::npos ? -1 : static_cast<int>(index);
  }
  bool equals(const String& other) const { return value == other.value; }
  long toInt() const { return std::strtol(c_str(), nullptr, 10); }
  void trim() {
    auto first = value.find_first_not_of(" \t\r\n\f\v");
    if (first == std::string::npos) { value.clear(); return; }
    value = value.substr(first, value.find_last_not_of(" \t\r\n\f\v") - first + 1);
  }
  String& operator+=(const String& other) { concat(other); return *this; }
  String& operator+=(const char* other) { concat(other); return *this; }
  String& operator+=(char other) { concat(other); return *this; }
  template<class T, typename std::enable_if<std::is_integral<T>::value && !std::is_same<T, char>::value, int>::type = 0>
  String& operator+=(T number) { concat(std::to_string(number).c_str()); return *this; }
  friend String operator+(String left, const String& right) { left += right; return left; }
  friend bool operator==(const String& left, const String& right) { return left.equals(right); }
  friend bool operator!=(const String& left, const String& right) { return !left.equals(right); }
};

class HardwareSerial {
  struct Input { uint64_t at; std::string bytes; };
  std::deque<Input> scheduled;
  void releaseDue() {
    while (!scheduled.empty() && scheduled.front().at <= smsTestTicks) {
      feed(scheduled.front().bytes);
      scheduled.pop_front();
    }
  }
public:
  struct Write { uint64_t at; std::string requested; size_t accepted; };
  std::deque<char> rx;
  std::string tx;
  std::vector<Write> writes;
  std::deque<size_t> writeResults; // Optional per-write short-write injection.
  std::function<void(const std::string&)> onWrite;
  std::function<void()> afterRead;
  size_t readCalls = 0;
  void feed(const std::string& data) { rx.insert(rx.end(), data.begin(), data.end()); }
  void schedule(uint64_t afterMs, const std::string& data) {
    Input input{smsTestTicks + afterMs, data};
    auto position = std::upper_bound(scheduled.begin(), scheduled.end(), input.at,
        [](uint64_t at, const Input& other) { return at < other.at; });
    scheduled.insert(position, std::move(input)); // Stable order for equal timestamps.
  }
  size_t pendingEvents() const { return scheduled.size(); }
  void reset() { *this = HardwareSerial{}; }
  int available() { releaseDue(); return static_cast<int>(rx.size()); }
  int read() {
    if (rx.empty()) return -1;
    unsigned char c = rx.front();
    rx.pop_front();
    readCalls++;
    if (afterRead) afterRead();
    return c;
  }
  size_t write(const uint8_t* data, size_t size) {
    size_t accepted = size;
    if (!writeResults.empty()) {
      accepted = std::min(size, writeResults.front());
      writeResults.pop_front();
    }
    std::string requested(reinterpret_cast<const char*>(data), size);
    writes.push_back({smsTestTicks, requested, accepted});
    std::string sent = requested.substr(0, accepted);
    tx += sent;
    if (onWrite && accepted) onWrite(sent);
    return accepted;
  }
  size_t write(uint8_t value) { return write(&value, 1); }
  template<class T> void print(const T& value) {
    String text(value);
    write(reinterpret_cast<const uint8_t*>(text.c_str()), text.length());
  }
  template<class T> void println(const T& value) { print(String(value) + "\r\n"); }
  void println() { print("\r\n"); }
  void printf(const char* format, ...) {
    char text[2048];
    va_list args; va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    print(text);
  }
};
extern HardwareSerial Serial, Serial1;
extern unsigned long smsTestMillis;
inline size_t smsTestDelays = 0, smsTestYields = 0, smsTestWdtChecks = 0, smsTestWdtResets = 0;
constexpr int ESP_OK = 0;
inline int smsTestWdtStatus = ESP_OK;
inline unsigned long millis() { return smsTestMillis; }
inline void delay(unsigned long amount) {
  smsTestMillis += amount;
  smsTestTicks += amount;
  smsTestDelays++;
}
inline void yield() { smsTestYields++; }
inline int esp_task_wdt_status(void*) { smsTestWdtChecks++; return smsTestWdtStatus; }
inline void esp_task_wdt_reset() { smsTestWdtResets++; }
inline void resetSmsHostClock(unsigned long start = 1000) {
  smsTestMillis = start;
  smsTestTicks = 0;
  smsTestDelays = smsTestYields = smsTestWdtChecks = smsTestWdtResets = 0;
  smsTestWdtStatus = ESP_OK;
}

class Preferences {};
using esp_reset_reason_t = int;
using SemaphoreHandle_t = void*;
constexpr int pdTRUE = 1;
inline unsigned long pdMS_TO_TICKS(unsigned long value) { return value; }
inline SemaphoreHandle_t xSemaphoreCreateMutex() { static int token; return &token; }
inline int xSemaphoreTake(SemaphoreHandle_t, unsigned long) { return pdTRUE; }
inline void xSemaphoreGive(SemaphoreHandle_t) {}
