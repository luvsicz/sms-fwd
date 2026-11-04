#pragma once
// Only Arduino APIs, allocation and transport are substitutes. The MQTT codec is the installed 2.8 source.
#define String MqttBaseString
#include "sms_host_stubs.h"
#undef String
#include <limits>
#include <map>
#include <new>

struct MqttStringProbe {
  size_t reserves = 0, concats = 0, failReserve = 0, failConcat = 0, loseConcat = 0;
  std::vector<size_t> capacities;
};
inline MqttStringProbe mqttStrings;

class String : public MqttBaseString {
public:
  String() = default;
  using MqttBaseString::MqttBaseString;
  String(const MqttBaseString& text) : MqttBaseString(text) {}
  bool reserve(unsigned int size) {
    mqttStrings.capacities.push_back(size);
    if (++mqttStrings.reserves == mqttStrings.failReserve) return false;
    return MqttBaseString::reserve(size);
  }
  bool concat(const char* bytes, unsigned int size) {
    const size_t call = ++mqttStrings.concats;
    if (call == mqttStrings.failConcat) return false;
    if (call != mqttStrings.loseConcat) *this += String(std::string(bytes, size));
    return true;
  }
  bool concat(char value) { return concat(&value, 1); }
  void remove(unsigned int start, unsigned int count = std::numeric_limits<unsigned int>::max()) {
    if (start >= length()) return;
    std::string text(c_str(), length());
    text.erase(start, count);
    *this = String(text);
  }
  void replace(const String& from, const String& to) {
    if (!from.length()) return;
    std::string text = c_str();
    size_t pos = 0;
    while ((pos = text.find(from.c_str(), pos)) != std::string::npos) {
      text.replace(pos, from.length(), to.c_str());
      pos += to.length();
    }
    *this = String(text);
  }
  void toLowerCase() {
    std::string text = c_str();
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    *this = String(text);
  }
  char operator[](unsigned int pos) const { return charAt(pos); }
};

using byte = uint8_t;
using boolean = bool;
inline uint8_t pgm_read_byte_near(const uint8_t* p) { return *p; }
class Print {
public:
  virtual ~Print() = default;
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t* bytes, size_t size) {
    size_t sent = 0;
    while (sent < size && write(bytes[sent]) == 1) ++sent;
    return sent;
  }
};
class Stream : public Print {
public:
  virtual int available() = 0;
  virtual int read() = 0;
  virtual void flush() = 0;
};
class IPAddress {
public:
  IPAddress() = default;
  IPAddress(uint8_t, uint8_t, uint8_t, uint8_t) {}
};
class Client : public Stream {
public:
  virtual int connect(const char*, uint16_t) = 0;
  virtual int connect(IPAddress, uint16_t) = 0;
  virtual uint8_t connected() = 0;
  virtual void stop() = 0;
};

// Red zones also catch beginPublish/writeString overruns without requiring a downloaded sanitizer.
namespace MqttHeap {
struct Block { uint8_t* raw; size_t size; };
inline std::map<void*, Block> blocks;
inline size_t mallocCalls = 0, reallocCalls = 0, failMalloc = 0, failRealloc = 0;
inline bool damage = false;
constexpr size_t guardSize = 32;
inline bool intact(const Block& block) {
  for (size_t i = 0; i < guardSize; ++i) {
    if (block.raw[i] != 0xA7 || block.raw[guardSize + block.size + i] != 0xA7) return false;
  }
  return true;
}
inline bool guardsOk() {
  for (const auto& item : blocks) if (!intact(item.second)) return false;
  return !damage;
}
inline void resetProbe() { mallocCalls = reallocCalls = failMalloc = failRealloc = 0; }
inline void* allocate(size_t size) {
  auto* raw = static_cast<uint8_t*>(std::malloc(size + 2 * guardSize));
  if (!raw) return nullptr;
  std::memset(raw, 0xA7, size + 2 * guardSize);
  void* result = raw + guardSize;
  blocks[result] = {raw, size};
  return result;
}
inline void release(void* ptr) {
  if (!ptr) return;
  const auto found = blocks.find(ptr);
  if (found == blocks.end()) throw std::runtime_error("unexpected PubSubClient allocation");
  damage = damage || !intact(found->second);
  std::free(found->second.raw);
  blocks.erase(found);
}
inline void* mallocProbe(size_t size) {
  if (++mallocCalls == failMalloc) return nullptr;
  return allocate(size);
}
inline void* reallocProbe(void* ptr, size_t size) {
  if (++reallocCalls == failRealloc) return nullptr;
  void* next = allocate(size);
  if (!next) return nullptr; // Preserve the old allocation on failure, just like realloc.
  if (ptr) {
    std::memcpy(next, ptr, std::min(size, blocks.at(ptr).size));
    release(ptr);
  }
  return next;
}
} // namespace MqttHeap

enum class MqttPhase { Other, Header, Payload, Disconnect };
struct MqttIoFault {
  MqttPhase phase = MqttPhase::Other;
  size_t publish = 0, chunk = 0;
  size_t accepted = std::numeric_limits<size_t>::max();
  bool dropAfterWrite = false;
};
struct MqttWrite {
  MqttPhase phase;
  size_t publish, chunk;
  std::string requested;
  size_t accepted;
  uintptr_t source;
};

class WiFiClient : public Client {
public:
  bool linkUp = false, connectSucceeds = true;
  bool answerPing = true;
  uint8_t connackCode = 0;
  std::deque<uint8_t> input;
  std::string wire, host;
  uint16_t port = 0;
  std::vector<MqttWrite> writes;
  std::vector<std::string> events;
  size_t connects = 0, stops = 0, flushes = 0, faultHits = 0;
  MqttPhase phase = MqttPhase::Other;
  size_t publishOrdinal = 0, chunkOrdinal = 0;
  MqttIoFault fault;

  int connect(const char* server, uint16_t targetPort) override {
    ++connects; host = server; port = targetPort;
    events.push_back("tcp.connect");
    linkUp = connectSucceeds;
    if (linkUp) input = {0x20, 0x02, 0x00, connackCode}; // In-memory CONNACK, never DNS or sockets.
    return linkUp ? 1 : 0;
  }
  int connect(IPAddress, uint16_t targetPort) override { return connect("numeric.test", targetPort); }
  uint8_t connected() override { return linkUp ? 1 : 0; }
  int available() override { return static_cast<int>(input.size()); }
  int read() override {
    if (input.empty()) return -1;
    const int result = input.front(); input.pop_front(); return result;
  }
  void flush() override { ++flushes; events.push_back("tcp.flush"); }
  void stop() override { ++stops; linkUp = false; events.push_back("tcp.stop"); }
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* bytes, size_t count) override {
    size_t accepted = linkUp ? count : 0;
    const bool inject = fault.publish != 0 && fault.publish == publishOrdinal &&
        fault.phase == phase && (phase != MqttPhase::Payload || fault.chunk == chunkOrdinal);
    if (inject) { ++faultHits; accepted = std::min(accepted, fault.accepted); }
    const std::string requested(reinterpret_cast<const char*>(bytes), count);
    writes.push_back({phase, publishOrdinal, chunkOrdinal, requested, accepted, reinterpret_cast<uintptr_t>(bytes)});
    wire.append(requested, 0, accepted);
    if (answerPing && accepted == 2 && requested == std::string("\xc0\x00", 2)) {
      input.push_back(0xd0); input.push_back(0x00);
    }
    events.push_back("tcp.write:" + std::to_string(accepted));
    if (inject && fault.dropAfterWrite) { linkUp = false; events.push_back("tcp.drop"); }
    return accepted;
  }
  void clearObservations() {
    wire.clear(); writes.clear(); events.clear();
    connects = stops = flushes = faultHits = 0;
    phase = MqttPhase::Other; publishOrdinal = chunkOrdinal = 0;
    fault = {};
  }
};

// Include the exact installed header under a private name. The public seam below does not encode MQTT.
#define PubSubClient MqttRealPubSubClient
#include "mqtt_real/PubSubClient.h"
#undef PubSubClient

struct MqttPublishAttempt {
  std::string topic;
  size_t declared, start, headerEnd = 0, end = 0, chunks = 0;
  bool retained, began = false, ended = false;
  int realEnd = 0;
};
class PubSubClient : public MqttRealPubSubClient {
  WiFiClient& transport;
public:
  std::vector<size_t> bufferRequests;
  std::vector<MqttPublishAttempt> publishes;
  size_t beginCalls = 0, endCalls = 0, loopCalls = 0, disconnectCalls = 0;
  size_t disconnectBeforeBegin = 0, disconnectAtEnd = 0, failEnd = 0, seamFaultHits = 0;
  explicit PubSubClient(WiFiClient& client) : MqttRealPubSubClient(client), transport(client) {
    MqttRealPubSubClient::setCallback(nullptr);
    MqttRealPubSubClient::setServer("offline.invalid", 1883);
  }
  bool setBufferSize(uint16_t size) {
    bufferRequests.push_back(size);
    return MqttRealPubSubClient::setBufferSize(size);
  }
  bool beginPublish(const char* topic, unsigned int length, bool retained) {
    ++beginCalls;
    transport.events.push_back("mqtt.begin");
    publishes.push_back({topic, length, transport.wire.size(), 0, 0, 0, retained});
    transport.publishOrdinal = beginCalls; transport.chunkOrdinal = 0;
    transport.phase = MqttPhase::Header;
    if (disconnectBeforeBegin == beginCalls) { transport.linkUp = false; ++seamFaultHits; }
    const bool result = MqttRealPubSubClient::beginPublish(topic, length, retained);
    auto& attempt = publishes.back();
    attempt.began = result;
    attempt.headerEnd = attempt.end = transport.wire.size();
    transport.phase = MqttPhase::Other;
    return result;
  }
  size_t write(const uint8_t* bytes, size_t size) override {
    if (publishes.empty()) throw std::runtime_error("payload write without beginPublish");
    auto& attempt = publishes.back();
    transport.phase = MqttPhase::Payload;
    transport.chunkOrdinal = ++attempt.chunks;
    const size_t result = MqttRealPubSubClient::write(bytes, size);
    attempt.end = transport.wire.size();
    transport.phase = MqttPhase::Other;
    return result;
  }
  size_t write(uint8_t value) override { return write(&value, 1); }
  int endPublish() {
    ++endCalls;
    transport.events.push_back("mqtt.end");
    auto& attempt = publishes.back();
    attempt.ended = true;
    attempt.realEnd = MqttRealPubSubClient::endPublish();
    if (disconnectAtEnd == beginCalls) { transport.linkUp = false; ++seamFaultHits; }
    // 2.8 cannot naturally return 0. This optional fault probes the firmware's defensive branch only.
    if (failEnd == beginCalls) { ++seamFaultHits; return 0; }
    return attempt.realEnd;
  }
  void disconnect() {
    ++disconnectCalls;
    transport.events.push_back("mqtt.disconnect");
    transport.phase = MqttPhase::Disconnect;
    MqttRealPubSubClient::disconnect(); // Sends the real E0 00 bytes if firmware uses the wrong abort API.
    transport.phase = MqttPhase::Other;
  }
  bool loop() {
    ++loopCalls;
    transport.events.push_back("mqtt.loop");
    return MqttRealPubSubClient::loop();
  }
  void clearObservations() {
    bufferRequests.clear(); publishes.clear();
    beginCalls = endCalls = loopCalls = disconnectCalls = seamFaultHits = 0;
    disconnectBeforeBegin = disconnectAtEnd = failEnd = 0;
    transport.clearObservations();
  }
};

struct MqttTestIp { String toString() const { return "192.0.2.15"; } };
struct MqttTestWifi {
  String mac = "02:00:00:12:34:56", ssid = "offline-wifi";
  int rssi = -61;
  String macAddress() const { return mac; }
  int RSSI() const { return rssi; }
  String SSID() const { return ssid; }
  MqttTestIp localIP() const { return {}; }
};
inline MqttTestWifi WiFi;
struct MqttTestEsp {
  size_t restarts = 0;
  uint32_t getFreeHeap() const { return 80000; }
  void restart() { ++restarts; }
};
inline MqttTestEsp ESP;
inline String getCurrentTimeString() { return "2026-05-22 12:34:56"; }
inline std::vector<std::string> mqttModemCommands;
inline String sendATCommand(const char* command, unsigned long) {
  mqttModemCommands.emplace_back(command);
  if (std::strcmp(command, "AT+CESQ") == 0) return "+CESQ: 99,99,255,255,20,70\r\nOK";
  if (std::strcmp(command, "AT+CGDCONT?") == 0) return "+CGDCONT: 1,\"IP\",\"offline-apn\"\r\nOK";
  throw std::runtime_error("unexpected modem command in MQTT host test");
}