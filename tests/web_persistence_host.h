#pragma once
// Include after the suite's String, SemaphoreHandle_t, pdTRUE and testEffects.
// No filesystem/NVS/network access. Limits and return lengths match ESP32 Preferences.
#include <cstring>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

struct ConfigLockFaults {
  static inline bool failCreate = false, failTake = false;
  static inline unsigned int creates = 0, takes = 0, gives = 0, held = 0;
  static void reset() { failCreate = failTake = false; creates = takes = gives = held = 0; }
};
inline SemaphoreHandle_t xSemaphoreCreateMutex() {
  ConfigLockFaults::creates++;
  static int token;
  return ConfigLockFaults::failCreate ? nullptr : &token;
}
inline int xSemaphoreTake(SemaphoreHandle_t lock, unsigned long) {
  ConfigLockFaults::takes++;
  if (!lock || ConfigLockFaults::failTake || ConfigLockFaults::held) return 0;
  ConfigLockFaults::held++;
  return pdTRUE;
}
inline void xSemaphoreGive(SemaphoreHandle_t lock) {
  if (!lock || !ConfigLockFaults::held) throw std::runtime_error("giving an unowned NVS lock");
  ConfigLockFaults::held--; ConfigLockFaults::gives++;
}

class Preferences {
  struct Value { std::string text; bool string; };
  std::map<std::string, std::map<std::string, Value>> data_;
  std::string current_;
  bool readOnly_ = true;
  const Value* find(const char* key) const {
    auto space = data_.find(current_);
    if (space == data_.end()) return nullptr;
    auto value = space->second.find(key);
    return value == space->second.end() ? nullptr : &value->second;
  }
  String read(const char* key, const String& fallback) const {
    const Value* value = find(key);
    return value ? String(value->text) : fallback;
  }
  size_t write(const char* key, const char* text, bool string, size_t result) {
    writeAttempts++;
    if (!key || !*key || std::strlen(key) > 15 || !text || current_.empty() || readOnly_) return 0;
    attemptedKeys.emplace_back(key);
    if (string && std::strlen(text) > 3999) return 0;
    if (failKey == key) return 0;
    if (failWriteAfter >= 0 && failWriteAfter-- == 0) return 0;
    if (shortWriteKey == key) return result > 1 ? result - 1 : 0;
    data_[current_][key] = {text, string}; writes++;
    return result;
  }
public:
  unsigned int saves = 0, loads = 0, statsSaves = 0, writeAttempts = 0, writes = 0;
  bool failBegin = false, failRead = false;
  int failWriteAfter = -1;
  std::string failKey, shortWriteKey;
  std::vector<std::string> attemptedKeys;
  std::function<void()> beforeSave;
  bool begin(const char* name, bool readOnly = false) {
    if (!current_.empty()) throw std::runtime_error("nested Preferences namespace");
    if (std::strcmp(name, "sms_config") == 0) {
      if (readOnly) loads++;
      else { saves++; testEffects.push_back("save"); if (beforeSave) beforeSave(); }
    }
    if (failBegin) return false;
    current_ = name; readOnly_ = readOnly;
    return true;
  }
  void end() { current_.clear(); }
  bool isOpen() const { return !current_.empty(); }
  bool isKey(const char* key) const { return find(key) != nullptr; }
  bool remove(const char* key) {
    return !current_.empty() && !readOnly_ && data_[current_].erase(key) != 0;
  }
  size_t putString(const char* key, const char* value) {
    return write(key, value, true, value ? std::strlen(value) : 0);
  }
  // This overload really copies, like ESP32. Production should prefer c_str().
  size_t putString(const char* key, const String value) { return putString(key, value ? value.c_str() : nullptr); }
  size_t putBool(const char* key, bool value) { return putUChar(key, value ? 1 : 0); }
  size_t putInt(const char* key, int value) { return write(key, std::to_string(value).c_str(), false, 4); }
  size_t putUChar(const char* key, uint8_t value) { return write(key, std::to_string(value).c_str(), false, 1); }
  size_t putULong(const char* key, uint32_t value) {
    const size_t result = write(key, std::to_string(value).c_str(), false, 4);
    if (result && current_ == "sms_stats" && std::strcmp(key, "sent") == 0) {
      statsSaves++; testEffects.push_back("stats");
    }
    return result;
  }
  String getString(const char* key, const String& fallback = "") const {
    const Value* value = find(key);
    return !failRead && value && value->string ? String(value->text) : fallback;
  }
  size_t getString(const char* key, char* buffer, size_t capacity) const {
    const Value* value = find(key);
    if (failRead || !value || !value->string || !buffer || value->text.size() + 1 > capacity) return 0;
    std::memcpy(buffer, value->text.c_str(), value->text.size() + 1);
    return value->text.size() + 1; // Includes NUL, unlike putString.
  }
  bool getBool(const char* key, bool fallback = false) const { return read(key, String(fallback ? 1 : 0)).toInt() != 0; }
  int getInt(const char* key, int fallback = 0) const { return static_cast<int>(read(key, String(fallback)).toInt()); }
  uint8_t getUChar(const char* key, uint8_t fallback = 0) const { return static_cast<uint8_t>(getInt(key, fallback)); }
  uint32_t getULong(const char* key, uint32_t fallback = 0) const {
    return static_cast<uint32_t>(std::strtoull(read(key, String(fallback)).c_str(), nullptr, 10));
  }
  void clearTestData() { *this = Preferences{}; }
};
