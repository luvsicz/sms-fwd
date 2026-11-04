#pragma once
// API substitutes only. The runner extracts the current production functions verbatim.
// String models non-throwing OOM (without ESP SSO); ReadyMail's internal allocator/TLS are NOT modeled.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using std::min;
inline std::vector<std::string> pushEvents;

// Fixed bookkeeping avoids recursion in the replacement global new/delete in push_regression.cpp.
struct PushJobHeap {
  size_t calls = 0, failAt = 0, faultHits = 0, frees = 0, live = 0;
  void* pointers[32]{};
  bool active = false;
  void remember(void* p) noexcept {
    for (auto& slot : pointers) if (!slot) { slot = p; ++live; return; }
    std::abort();
  }
  void release(void* p) noexcept {
    for (auto& slot : pointers) if (slot && slot == p) { slot = nullptr; --live; ++frees; return; }
  }
};
inline PushJobHeap pushJobs;

namespace PushStringHeap {
struct Block { unsigned char* raw; size_t size; };
inline std::map<void*, Block> blocks;
inline size_t allocations = 0, failAt = 0, faultHits = 0;
inline size_t concats = 0, failConcat = 0, shortConcat = 0, copies = 0, instances = 0;
inline bool damaged = false;
constexpr size_t guardBytes = 16;
inline bool intact(const Block& block) {
  for (size_t i = 0; i < guardBytes; ++i) {
    if (block.raw[i] != 0xA7 || block.raw[guardBytes + block.size + i] != 0xA7) return false;
  }
  return true;
}
inline bool guardsOk() {
  for (const auto& item : blocks) if (!intact(item.second)) return false;
  return !damaged;
}
inline void resetProbe() { allocations = failAt = faultHits = concats = failConcat = shortConcat = copies = 0; }
inline char* allocate(size_t size) {
  if (++allocations == failAt) { ++faultHits; return nullptr; }
  auto* raw = static_cast<unsigned char*>(std::malloc(size + 2 * guardBytes));
  if (!raw) return nullptr;
  std::memset(raw, 0xA7, size + 2 * guardBytes);
  void* p = raw + guardBytes;
  blocks.emplace(p, Block{raw, size});
  return static_cast<char*>(p);
}
inline void release(void* p) {
  if (!p) return;
  const auto found = blocks.find(p);
  if (found == blocks.end()) std::abort();
  damaged = damaged || !intact(found->second);
  std::free(found->second.raw);
  blocks.erase(found);
}
} // namespace PushStringHeap

class String {
  char* bytes = nullptr;
  unsigned int size = 0, capacity = 0;
  bool valid = true;
  void invalidate() {
    PushStringHeap::release(bytes);
    bytes = nullptr; size = capacity = 0; valid = false;
  }
  void assign(const char* source, unsigned int count) {
    if (!source || !reserve(count)) { invalidate(); return; }
    if (count) std::memmove(bytes, source, count);
    size = count;
    if (bytes) bytes[size] = 0;
  }
public:
  String() { ++PushStringHeap::instances; }
  String(const char* text) : String() { assign(text, text ? std::strlen(text) : 0); }
  String(const String& text) : String() { ++PushStringHeap::copies; assign(text.c_str(), text.length()); }
  String(String&& text) noexcept : String() { std::swap(bytes, text.bytes); std::swap(size, text.size);
    std::swap(capacity, text.capacity); std::swap(valid, text.valid); }
  ~String() { PushStringHeap::release(bytes); --PushStringHeap::instances; }
  String& operator=(const char* text) { assign(text, text ? std::strlen(text) : 0); return *this; }
  String& operator=(const String& text) {
    if (this != &text) { ++PushStringHeap::copies; assign(text.c_str(), text.length()); }
    return *this;
  }
  String& operator=(String&& text) noexcept {
    if (this != &text) { std::swap(bytes, text.bytes); std::swap(size, text.size);
      std::swap(capacity, text.capacity); std::swap(valid, text.valid); }
    return *this;
  }
  unsigned int length() const { return size; }
  const char* c_str() const { return valid ? (bytes ? bytes : "") : nullptr; }
  explicit operator bool() const { return valid; }
  bool reserve(unsigned int count) {
    if (count <= capacity) { valid = true; return true; }
    char* next = PushStringHeap::allocate(size_t(count) + 1);
    if (!next) return false; // reserve preserves the old value on failure.
    if (size) std::memcpy(next, bytes, size);
    next[size] = 0;
    PushStringHeap::release(bytes);
    bytes = next; capacity = count; valid = true;
    return true;
  }
  bool concat(const char* text, unsigned int count) {
    const size_t call = ++PushStringHeap::concats;
    if (call == PushStringHeap::failConcat) { ++PushStringHeap::faultHits; return false; }
    if (!text || !reserve(size + count)) return false;
    if (call == PushStringHeap::shortConcat && count) { --count; ++PushStringHeap::faultHits; }
    if (count) std::memcpy(bytes + size, text, count);
    size += count;
    if (bytes) bytes[size] = 0;
    return true;
  }
  bool concat(const char* text) { return concat(text, text ? std::strlen(text) : 0); }
  bool concat(const String& text) { return concat(text.c_str(), text.length()); }
  void remove(unsigned int first) {
    if (first < size) { size = first; bytes[size] = 0; }
  }
  void trim() {
    unsigned int first = 0, last = size;
    while (first < last && std::isspace(static_cast<unsigned char>(bytes[first]))) ++first;
    while (last > first && std::isspace(static_cast<unsigned char>(bytes[last - 1]))) --last;
    if (last > first) std::memmove(bytes, bytes + first, last - first);
    size = last - first;
    if (bytes) bytes[size] = 0;
  }
  bool startsWith(const char* prefix) const {
    const size_t count = std::strlen(prefix);
    return count <= size && std::strncmp(c_str(), prefix, count) == 0;
  }
  bool operator==(const char* text) const { return c_str() && text && std::strcmp(c_str(), text) == 0; }
  friend String operator+(const char* left, const String& right) {
    String result(left); result.concat(right); return result;
  }
};

class Preferences {};
using esp_reset_reason_t = int;
struct PushSerial {
  void println(const char*) {}
  void println(const String&) {}
  template<class... Args> void printf(const char*, Args...) {}
};
inline PushSerial Serial;

inline unsigned long pushMillis = 0;
inline std::time_t pushEpoch = 1700000000;
inline size_t pushNtpCalls = 0, pushYields = 0, pushWdtChecks = 0, pushWdtResets = 0;
inline int pushWdtStatus = 0;
constexpr int ESP_OK = 0, WL_CONNECTED = 3;
inline unsigned long millis() { return pushMillis; }
inline void yield() { ++pushYields; }
inline void delay(unsigned long ms) { pushMillis += ms; }
inline std::time_t pushHostTime(std::time_t* result) { if (result) *result = pushEpoch; return pushEpoch; }
inline void configTzTime(const char*, const char*, const char*, const char*) { ++pushNtpCalls; }
inline int esp_task_wdt_status(void*) { ++pushWdtChecks; return pushWdtStatus; }
inline void esp_task_wdt_reset() { ++pushWdtResets; }
struct PushWiFi { int state = WL_CONNECTED; int status() const { return state; } };
inline PushWiFi WiFi;

using BaseType_t = int;
using TickType_t = uint32_t;
constexpr BaseType_t pdTRUE = 1, pdPASS = 1;
constexpr TickType_t portMAX_DELAY = UINT32_MAX;
constexpr TickType_t pdMS_TO_TICKS(unsigned long ms) { return static_cast<TickType_t>(ms); }
inline void vTaskDelay(TickType_t ticks) { pushMillis += ticks; }
struct PushQueue { size_t capacity, itemSize; std::deque<void*> items; };
struct PushTask { void (*entry)(void*) = nullptr; uint32_t stack = 0; };
struct PushSemaphore { bool held = false; };
using QueueHandle_t = PushQueue*;
using TaskHandle_t = PushTask*;
using SemaphoreHandle_t = PushSemaphore*;
struct PushRtosProbe {
  bool failQueue = false, failTask = false, failMutex = false, failTake = false, skipReceive = false;
  size_t queueCreates = 0, taskCreates = 0, sends = 0, receives = 0, waits = 0, takes = 0, gives = 0;
  PushTask task;
  PushSemaphore mutex;
};
inline PushRtosProbe pushRtos;
struct PushQueueDrained {}; // Only thrown at the NEXT blocking receive, after production send/save/delete.
inline QueueHandle_t xQueueCreate(size_t length, size_t itemSize) {
  ++pushRtos.queueCreates;
  if (itemSize != sizeof(void*)) throw std::runtime_error("queue copied object bytes instead of a pointer");
  return pushRtos.failQueue ? nullptr : new PushQueue{length, itemSize, {}};
}
inline BaseType_t xTaskCreate(void (*entry)(void*), const char*, uint32_t stack, void*, int, TaskHandle_t* handle) {
  ++pushRtos.taskCreates;
  if (pushRtos.failTask) return 0;
  pushRtos.task = {entry, stack}; *handle = &pushRtos.task;
  return pdPASS; // Deterministic scheduling: tests explicitly run this production task entry later.
}
inline BaseType_t xQueueSend(QueueHandle_t queue, const void* item, TickType_t wait) {
  ++pushRtos.sends;
  if (wait != 0) throw std::runtime_error("producer blocks on a full queue");
  if (queue->items.size() == queue->capacity) return 0;
  void* job = nullptr; std::memcpy(&job, item, sizeof(job)); queue->items.push_back(job);
  return pdTRUE;
}
inline BaseType_t xQueueReceive(QueueHandle_t queue, void* item, TickType_t wait) {
  ++pushRtos.receives;
  if (wait != portMAX_DELAY) throw std::runtime_error("unexpected worker receive timeout");
  if (pushRtos.skipReceive) { pushRtos.skipReceive = false; return 0; }
  if (queue->items.empty()) { ++pushRtos.waits; throw PushQueueDrained{}; }
  void* job = queue->items.front(); queue->items.pop_front(); std::memcpy(item, &job, sizeof(job));
  return pdTRUE;
}
inline SemaphoreHandle_t xSemaphoreCreateMutex() { return pushRtos.failMutex ? nullptr : &pushRtos.mutex; }
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t) {
  ++pushRtos.takes;
  if (pushRtos.failTake || mutex->held) return 0;
  mutex->held = true; pushEvents.emplace_back("lock.take"); return pdTRUE;
}
inline void xSemaphoreGive(SemaphoreHandle_t mutex) {
  ++pushRtos.gives; mutex->held = false; pushEvents.emplace_back("lock.give");
}

inline std::map<const void*, bool> pushTlsAlive;
class WiFiClientSecure {
public:
  bool plain = false;
  size_t stops = 0, upgrades = 0;
  int timeout = 0, handshakeTimeout = 0;
  WiFiClientSecure() { pushTlsAlive[this] = true; pushEvents.emplace_back("tls.construct"); }
  ~WiFiClientSecure() { pushEvents.emplace_back("tls.destroy"); pushTlsAlive.erase(this); }
  WiFiClientSecure& operator=(const WiFiClientSecure& other) { plain = other.plain; return *this; }
  bool stillInPlainStart() const { return plain; }
  void setPlainStart() { plain = true; }
  int startTLS() { ++upgrades; return 1; }
  void setInsecure() {}
  void setConnectionTimeout(int) {}
  void setTimeout(int value) { timeout = value; }
  void setHandshakeTimeout(int value) { handshakeTimeout = value; }
  void stop() { ++stops; pushEvents.emplace_back("tls.stop"); }
};

enum { rfc822_from, rfc822_to, rfc822_subject, readymail_auth_password };
struct SMTPStatus { int errorCode = 0, statusCode = 250; bool isComplete = false; const char* text = "host status"; };
struct PushHeaders {
  std::vector<std::pair<int, std::string>> values;
  uintptr_t subjectSource = 0;
  PushHeaders& add(int kind, const String& text) {
    if (!text) throw std::runtime_error("invalid String passed to ReadyMail headers.add");
    values.emplace_back(kind, text.c_str());
    if (kind == rfc822_subject) subjectSource = reinterpret_cast<uintptr_t>(text.c_str());
    return *this;
  }
};
struct PushText {
  std::string value;
  uintptr_t source = 0;
  bool borrowed = false;
  PushText& body(const String& text) {
    if (!text) throw std::runtime_error("invalid String passed to ReadyMail text.body");
    value = text.c_str(); source = reinterpret_cast<uintptr_t>(text.c_str()); borrowed = false;
    return *this;
  }
  PushText& body(const char* text, size_t size) {
    if (!text) throw std::runtime_error("null static body passed to ReadyMail");
    value.assign(text, size); source = reinterpret_cast<uintptr_t>(text); borrowed = true;
    return *this;
  }
  void validateBorrow() const {
    if (borrowed && !value.empty()) {
      const auto* pointer = reinterpret_cast<const char*>(source);
      if (!PushStringHeap::blocks.count(const_cast<char*>(pointer)) ||
          std::memcmp(pointer, value.data(), value.size()) != 0) {
        throw std::runtime_error("ReadyMail borrowed body expired before async completion");
      }
    }
  }
};
struct SMTPMessage {
  PushHeaders headers;
  PushText text;
  uint32_t timestamp = 0;
  size_t clears = 0;
  void clear() { ++clears; headers.values.clear(); text.value.clear(); pushEvents.emplace_back("message.clear"); }
};
enum class PushSmtpFault {
  None, ConnectStart, ConnectStatus, ConnectDisconnected, ConnectTimeout,
  AuthStart, AuthStatus, AuthUnconfirmed, AuthTimeout, SendStart, SendStatus, SendIncomplete, SendTimeout
};
struct PushDelivery {
  std::string server, user, pass, from, subject, body;
  std::vector<std::string> to;
  int port = 0;
  bool ssl = false, startTls = false;
  uint32_t timestamp = 0;
  uintptr_t serverSource = 0, userSource = 0, passSource = 0, subjectSource = 0, bodySource = 0;
};
struct PushSmtp {
  PushSmtpFault fault = PushSmtpFault::None;
  SMTPMessage message;
  PushDelivery current;
  std::vector<PushDelivery> attempts;
  size_t connects = 0, auths = 0, sends = 0, stops = 0, loops = 0, liveJobsAtSend = 0;
  unsigned long connectAt = 0;
  int stage = 0, pending = 0;
  bool connected = false, authenticated = false;
  void (*startTlsCallback)(bool&) = nullptr;
  void setStartTLS(void (*callback)(bool&), bool enabled) {
    startTlsCallback = callback; current.startTls = enabled;
  }
  bool connect(const String& server, uint16_t port, void (*)(SMTPStatus), bool ssl, bool await) {
    if (await || !pushRtos.mutex.held) throw std::runtime_error("SMTP must be async and locked");
    if (!server) throw std::runtime_error("invalid String passed to ReadyMail connect");
    ++connects; connectAt = millis(); stage = 1; pending = 1;
    current.server = server.c_str(); current.port = port; current.ssl = ssl;
    current.serverSource = reinterpret_cast<uintptr_t>(server.c_str());
    connected = fault != PushSmtpFault::ConnectDisconnected;
    if (startTlsCallback) { bool success = false; startTlsCallback(success); if (!success) connected = false; }
    pushEvents.emplace_back("smtp.connect");
    return fault != PushSmtpFault::ConnectStart;
  }
  bool authenticate(const String& user, const String& pass, int, bool await) {
    if (await) throw std::runtime_error("authenticate must be async");
    if (!user || !pass) throw std::runtime_error("invalid String passed to ReadyMail authenticate");
    ++auths; stage = 2; pending = 1;
    current.user = user.c_str(); current.pass = pass.c_str();
    current.userSource = reinterpret_cast<uintptr_t>(user.c_str()); current.passSource = reinterpret_cast<uintptr_t>(pass.c_str());
    authenticated = fault != PushSmtpFault::AuthUnconfirmed;
    pushEvents.emplace_back("smtp.auth");
    return fault != PushSmtpFault::AuthStart;
  }
  SMTPMessage& getMessage() { return message; }
  bool send(SMTPMessage& msg, const String&, bool await) {
    if (await || &msg != &message || !msg.clears || !pushRtos.mutex.held) {
      throw std::runtime_error("send must use locked async internal ReadyMail message");
    }
    msg.text.validateBorrow();
    ++sends; stage = 3; pending = 1; liveJobsAtSend = pushJobs.live;
    current.from.clear(); current.to.clear(); current.subject.clear();
    for (const auto& h : msg.headers.values) {
      if (h.first == rfc822_from) current.from = h.second;
      if (h.first == rfc822_to) current.to.push_back(h.second);
      if (h.first == rfc822_subject) current.subject = h.second;
    }
    current.body = msg.text.value; current.timestamp = msg.timestamp;
    current.subjectSource = msg.headers.subjectSource; current.bodySource = msg.text.source;
    attempts.push_back(current); pushEvents.emplace_back("smtp.send");
    return fault != PushSmtpFault::SendStart;
  }
  bool timedOut() const {
    return (stage == 1 && fault == PushSmtpFault::ConnectTimeout) ||
           (stage == 2 && fault == PushSmtpFault::AuthTimeout) ||
           (stage == 3 && fault == PushSmtpFault::SendTimeout);
  }
  bool isProcessing() const { return pending > 0 || timedOut(); }
  void loop() { ++loops; if (stage == 3) message.text.validateBorrow(); if (pending) --pending; }
  SMTPStatus status() const {
    const bool error = (stage == 1 && fault == PushSmtpFault::ConnectStatus) ||
                       (stage == 2 && fault == PushSmtpFault::AuthStatus) ||
                       (stage == 3 && fault == PushSmtpFault::SendStatus);
    return {error ? -1 : 0, error ? 550 : 250,
            stage == 3 && !error && fault != PushSmtpFault::SendIncomplete, "host status"};
  }
  bool isConnected() const { return connected; }
  bool isAuthenticated() const { return authenticated; }
  void stop() { ++stops; pending = stage = 0; connected = authenticated = false; pushEvents.emplace_back("smtp.stop"); }
};
extern WiFiClientSecure ssl_client;
extern PushSmtp smtp;

struct NetworkClient {
  std::string response;
  size_t cursor = 0;
  bool idleConnected = false, failRead = false;
  int available() const { return static_cast<int>(response.size() - cursor); }
  bool connected() const { return idleConnected || cursor < response.size(); }
  int readBytes(char* buffer, size_t count) {
    if (failRead) return 0;
    count = min(count, response.size() - cursor);
    std::memcpy(buffer, response.data() + cursor, count); cursor += count;
    return static_cast<int>(count);
  }
};
struct PushHttpProbe {
  bool beginOk = true, nullStream = false;
  int code = 200, connectTimeout = 0, timeout = 0, tlsTimeout = 0, tlsHandshake = 0;
  size_t begins = 0, requests = 0, ends = 0, destructors = 0, lifetimeErrors = 0;
  std::string url, method, contentType, body;
  NetworkClient stream;
};
inline PushHttpProbe pushHttp;
class HTTPClient {
  const WiFiClientSecure* client = nullptr;
public:
  HTTPClient() { pushEvents.emplace_back("http.construct"); }
  ~HTTPClient() {
    ++pushHttp.destructors;
    // Consult a lifetime registry instead of dereferencing freed storage (also safe on a regressed order).
    if (client && pushTlsAlive.count(client) == 0) ++pushHttp.lifetimeErrors;
    pushEvents.emplace_back("http.destroy");
  }
  bool begin(WiFiClientSecure& tls, const String& url) {
    client = &tls; pushHttp.tlsTimeout = tls.timeout; pushHttp.tlsHandshake = tls.handshakeTimeout;
    return begin(url); // Retain the reference even when begin fails: exercise early-return destruction.
  }
  bool begin(const String& url) { ++pushHttp.begins; pushHttp.url = url.c_str(); return pushHttp.beginOk; }
  void setConnectTimeout(int value) { pushHttp.connectTimeout = value; }
  void setTimeout(int value) { pushHttp.timeout = value; }
  void addHeader(const char*, const String& value) { pushHttp.contentType = value.c_str(); }
  int GET() { ++pushHttp.requests; pushHttp.method = "GET"; return pushHttp.code; }
  int POST(const String& body) { ++pushHttp.requests; pushHttp.method = "POST"; pushHttp.body = body.c_str(); return pushHttp.code; }
  NetworkClient* getStreamPtr() { return pushHttp.nullStream ? nullptr : &pushHttp.stream; }
  String errorToString(int) { return "host HTTP error"; }
  void end() { ++pushHttp.ends; pushEvents.emplace_back("http.end"); }
};
