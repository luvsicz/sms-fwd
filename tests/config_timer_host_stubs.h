#pragma once
// Host-only extensions of the shared SMS substitutes, including the base watchdog API.
// All I/O is in memory; no real WiFi credentials, UART, filesystem, NVS or network is used.
#define String SmsHostString
#define HardwareSerial SmsHostSerial
#define Preferences SmsHostPreferences
#define Serial smsBaseSerial
#define Serial1 smsBaseSerial1
#define millis smsBaseMillis
#define delay smsBaseDelay
#define xSemaphoreCreateMutex smsBaseCreateMutex
#define xSemaphoreTake smsBaseTakeMutex
#define xSemaphoreGive smsBaseGiveMutex
#include "sms_host_stubs.h"
#undef String
#undef HardwareSerial
#undef Preferences
#undef Serial
#undef Serial1
#undef millis
#undef delay
#undef xSemaphoreCreateMutex
#undef xSemaphoreTake
#undef xSemaphoreGive
#include <cstdarg>
#include <limits>
#include <map>
#include <memory>
#include <vector>

inline int historyReserveFailure = -1;
class String : public SmsHostString {
public:
  using SmsHostString::SmsHostString;
  String(const SmsHostString& text) : SmsHostString(text) {}
  String(double value, unsigned char digits) {
    char buffer[64]; std::snprintf(buffer, sizeof(buffer), "%.*f", static_cast<int>(digits), value);
    *this = String(buffer);
  }
  bool reserve(unsigned int size) {
    if (historyReserveFailure == 0) { historyReserveFailure = -1; return false; }
    if (historyReserveFailure > 0) --historyReserveFailure;
    return SmsHostString::reserve(size);
  }
  // Host allocation failures throw; an allocated empty string is still valid.
  explicit operator bool() const { return c_str() != nullptr; }
  void setCharAt(unsigned int index, char value) {
    std::string text(c_str(), length()); text.at(index) = value; *this = String(text);
  }
  void remove(unsigned int start, unsigned int count = std::numeric_limits<unsigned int>::max()) {
    if (start >= length() || count == 0) return;
    std::string text(c_str(), length());
    text.erase(start, count);
    *this = String(text);
  }
  void replace(const String& from, const String& to) {
    if (!from.length()) return;
    std::string text = c_str();
    size_t start = 0;
    while ((start = text.find(from.c_str(), start)) != std::string::npos) {
      text.replace(start, from.length(), to.c_str());
      start += to.length();
    }
    *this = String(text);
  }
  void replace(char from, char to) { replace(String(from), String(to)); }
  bool endsWith(const String& suffix) const {
    return length() >= suffix.length() && substring(length() - suffix.length()) == suffix;
  }
  void toLowerCase() {
    std::string text = c_str();
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    *this = String(text);
  }
  void toUpperCase() {
    std::string text = c_str();
    for (char& c : text) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    *this = String(text);
  }
  bool concat(char c) { *this += c; return true; }
  bool concat(const String& text) { *this += text; return true; }
  bool concat(const char* text, unsigned int size) { *this += String(std::string(text, size)); return true; }
};

inline uint64_t testClockMs = 0;
inline int64_t esp_timer_get_time() { return static_cast<int64_t>(testClockMs * 1000ULL); }
inline uint32_t millis() { return static_cast<uint32_t>(testClockMs); }
inline void delay(unsigned long amount) { testClockMs += amount; }
inline std::vector<std::string> testEffects;

class HardwareSerial : public SmsHostSerial {
public:
  std::string log;
  std::vector<std::string> initOrder;
  size_t requestedRx = 0;
  size_t rxResult = std::numeric_limits<size_t>::max();
  bool allocationSucceeds = true;
  bool started = false;
  unsigned long baudRate = 0;
  int rxPin = -1, txPin = -1;
  size_t setRxBufferSize(size_t capacity) {
    initOrder.push_back("rx"); requestedRx = capacity;
    if (started) return 0;
    return rxResult == std::numeric_limits<size_t>::max() ? capacity : rxResult;
  }
  void begin(unsigned long baud, uint32_t = 0, int rx = -1, int tx = -1) {
    initOrder.push_back("begin"); baudRate = baud; rxPin = rx; txPin = tx;
    started = allocationSucceeds;
  }
  explicit operator bool() const { return started; }
  void printf(const char* format, ...) {
    char text[2048];
    va_list args; va_start(args, format);
    std::vsnprintf(text, sizeof(text), format, args);
    va_end(args); log += text;
  }
  template<class T> void print(const T& text) { log += String(text).c_str(); }
  template<class T> void println(const T& text) { print(text); log += '\n'; }
  void println() { log += '\n'; }
};
inline HardwareSerial Serial, Serial1;

#include "web_persistence_host.h"

struct HistoryIoFaults {
  size_t readLimit = std::numeric_limits<size_t>::max();
  size_t writeLimit = std::numeric_limits<size_t>::max();
  bool failOpen = false, failRename = false;
};
inline HistoryIoFaults historyIo;
class File {
  std::shared_ptr<std::string> data;
  size_t pos = 0;
public:
  File() = default;
  File(std::shared_ptr<std::string> value, bool append) : data(std::move(value)), pos(append ? data->size() : 0) {}
  explicit operator bool() const { return bool(data); }
  void close() { data.reset(); }
  size_t size() const { return data ? data->size() : 0; }
  int available() const { return data ? static_cast<int>(data->size() - pos) : 0; }
  bool seek(size_t offset) { if (offset > size()) return false; pos = offset; return true; }
  int read() { return available() ? static_cast<unsigned char>((*data)[pos++]) : -1; }
  int read(uint8_t* target, size_t count) {
    count = std::min({count, static_cast<size_t>(available()), historyIo.readLimit});
    if (count) { std::memcpy(target, data->data() + pos, count); pos += count; }
    return static_cast<int>(count);
  }
  String readStringUntil(char delimiter) {
    String result;
    while (available()) { char c = static_cast<char>(read()); if (c == delimiter) break; result += c; }
    return result;
  }
  size_t write(const uint8_t* source, size_t count) {
    if (!data) return 0;
    count = std::min(count, historyIo.writeLimit);
    data->replace(pos, count, reinterpret_cast<const char*>(source), count); pos += count;
    return count;
  }
  void print(const String& text) { write(reinterpret_cast<const uint8_t*>(text.c_str()), text.length()); }
  void println(const String& text) { print(text + "\n"); }
};
class TestSpiffs {
  std::map<std::string, std::shared_ptr<std::string>> files;
public:
  bool begin(bool) { return true; }
  bool exists(const String& path) const { return files.count(path.c_str()) != 0; }
  bool remove(const String& path) { return files.erase(path.c_str()) != 0; }
  bool rename(const String& from, const String& to) {
    if (historyIo.failRename) return false;
    auto file = files.find(from.c_str()); if (file == files.end()) return false;
    files[to.c_str()] = file->second; files.erase(file); return true;
  }
  File open(const String& path, const char* mode) {
    if (historyIo.failOpen) return {};
    auto key = std::string(path.c_str());
    if (*mode == 'r' && !files.count(key)) return {};
    if (*mode == 'w' || !files.count(key)) files[key] = std::make_shared<std::string>();
    return File(files[key], *mode == 'a');
  }
  std::string contents(const char* path) const { auto it = files.find(path); return it == files.end() ? "" : *it->second; }
  void clear() { files.clear(); }
};
inline TestSpiffs SPIFFS;

constexpr int ESP_RST_UNKNOWN = 0, ESP_RST_POWERON = 1, ESP_RST_EXT = 2, ESP_RST_SW = 3,
  ESP_RST_PANIC = 4, ESP_RST_INT_WDT = 5, ESP_RST_TASK_WDT = 6, ESP_RST_BROWNOUT = 7,
  ESP_RST_DEEPSLEEP = 8, ESP_RST_SDIO = 9, ESP_RST_USB = 10, ESP_RST_JTAG = 11, ESP_RST_EFUSE = 12;
inline esp_reset_reason_t esp_reset_reason() { return ESP_RST_POWERON; }
struct TestEsp {
  uint32_t freeHeap = 100000, maxHeap = 80000;
  uint32_t getFreeHeap() const { return freeHeap; }
  uint32_t getMinFreeHeap() const { return 90000; }
  uint32_t getMaxAllocHeap() const { return maxHeap; }
  void restart() {}
};
inline TestEsp ESP;
using esp_err_t = int;
constexpr int ESP_ERR_INVALID_STATE = 1, ESP_ERR_NOT_FOUND = 2;
struct esp_task_wdt_config_t { uint32_t timeout_ms; uint32_t idle_core_mask; bool trigger_panic; };
inline int esp_task_wdt_reconfigure(const esp_task_wdt_config_t*) { return ESP_OK; }
inline void esp_task_wdt_deinit() {}
inline int esp_task_wdt_init(const esp_task_wdt_config_t*) { return ESP_OK; }
inline int esp_task_wdt_add(void*) { return ESP_OK; }
constexpr int LED_BUILTIN = 8, OUTPUT = 1, HIGH = 1, LOW = 0;
constexpr uint32_t SERIAL_8N1 = 0;
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline void configTzTime(const char*, const char*, const char*, const char*) {}

using wifi_mode_t = int;
constexpr int WIFI_STA = 1, WIFI_AP = 2, WL_CONNECTED = 3;
struct TestIp { String toString() const { return "192.0.2.1"; } };
class TestWifi {
  wifi_mode_t modeValue = WIFI_STA;
public:
  String ssid = "synthetic-wifi";
  void mode(wifi_mode_t value) { modeValue = value; }
  wifi_mode_t getMode() const { return modeValue; }
  int status() const { return WL_CONNECTED; }
  bool isConnected() const { return true; }
  int RSSI() const { return -45; }
  String SSID() const { return ssid; }
  TestIp localIP() const { return {}; }
  TestIp softAPIP() const { return {}; }
  TestIp gatewayIP() const { return {}; }
  TestIp subnetMask() const { return {}; }
  TestIp dnsIP() const { return {}; }
  String macAddress() const { return "02:00:00:12:34:56"; }
  String BSSIDstr() const { return "02:00:00:65:43:21"; }
  int channel() const { return 6; }
  void disconnect(bool, bool) {}
  void softAP(const char*) {}
};
inline TestWifi WiFi;
class WiFiMulti {
public:
  void addAP(const char*, const char*) {}
  int run() const { return WL_CONNECTED; }
};
class WiFiClient {};
class WiFiClientSecure { public: void setInsecure() {} };
class SMTPClient { public: explicit SMTPClient(WiFiClientSecure&) {} };
class PDU { public: explicit PDU(int) {} };
struct TestMdns { bool begin(const char*) { return true; } void addService(const char*, const char*, int) {} };
inline TestMdns MDNS;
using byte = uint8_t;
#define MQTT_MAX_HEADER_SIZE 5
class PubSubClient {
public:
  explicit PubSubClient(WiFiClient&) {}
  void setCallback(void (*)(char*, byte*, unsigned int)) {}
  bool setBufferSize(uint16_t) { return true; }
  void setServer(const char*, int) {}
  bool connected() const { return true; }
  void loop() {}
};

constexpr int BASIC_AUTH = 1, HTTP_POST = 1;
constexpr size_t CONTENT_LENGTH_UNKNOWN = static_cast<size_t>(-1);
class WebServer {
  struct Client {
    std::string bytes;
    bool stopped = false;
    size_t maxWrite = std::numeric_limits<size_t>::max();
    unsigned int calls = 0;
    bool connected() const { return !stopped; }
    void stop() { stopped = true; }
    size_t write(const uint8_t* data, size_t length) {
      calls++; length = std::min(length, maxWrite);
      bytes.append(reinterpret_cast<const char*>(data), length); return length;
    }
  } connection;
public:
  explicit WebServer(int) {}
  std::map<std::string, String> args;
  int statusCode = 0;
  String body;
  String contentType;
  String httpVersion = "HTTP/1.0";
  unsigned int terminators = 0;
  bool authorized = true;
  bool authenticate(const char*, const char*) const { return authorized; }
  void requestAuthentication(int, const char*, const char*) { statusCode = 401; }
  String arg(const String& key) const { auto it = args.find(key.c_str()); return it == args.end() ? String("") : it->second; }
  bool hasArg(const String& key) const { return args.count(key.c_str()) != 0; }
  int clientContentLength() const {
    auto it = args.find("plain");
    return it == args.end() ? 0 : static_cast<int>(it->second.length());
  }
  void send(int code, const char* mime, const String& text) { statusCode = code; contentType = mime; body = text; }
  void sendHeader(const char*, const char*, bool) {}
  void setContentLength(size_t) {}
  void sendContent(const char*, size_t) { terminators++; }
  Client& client() { return connection; }
  String version() const { return httpVersion; }
  void on(const char*, void (*)()) {}
  void on(const char*, int, void (*)()) {}
  void begin() {}
  void handleClient() {}
  void reset() { *this = WebServer(80); }
  const std::string& streamedBody() const { return connection.bytes; }
};
