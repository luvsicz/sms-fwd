#include "mqtt_host_stubs.h"
#include "mqtt_handler.h"
#include "json_input.h"
#include <fstream>
#include <iostream>

HardwareSerial Serial, Serial1;
unsigned long smsTestMillis = 0;
Config config{};
Statistics stats;
esp_reset_reason_t lastResetReasonCode = 0;
String lastResetReasonText;
bool lastResetPowerSuspected = false;
InterruptedOperationDiagnostic lastInterruptedOperation{};
WiFiClient mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);
String mqttDeviceId;
String mqttTopicStatus, mqttTopicSmsReceived, mqttTopicCallReceived, mqttTopicSmsSent;
String mqttTopicPingResult, mqttTopicLog, mqttTopicLogSlow, mqttTopicLogError, mqttTopicMetricTiming;
String mqttTopicSmsSend, mqttTopicPing, mqttTopicCmd;
String mqttHaStatusTopic, mqttHaSmsReceivedTopic, mqttHaCallReceivedTopic, mqttHaLogEventTopic;
unsigned long lastMqttReconnectAttempt = 0, lastMqttStatusReport = 0;
const unsigned long MQTT_RECONNECT_INTERVAL = 5000, MQTT_STATUS_INTERVAL = 60000;

// The script extracts these definitions verbatim; no firmware implementation lives in this test.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include "mqtt_actual.inc"
#pragma GCC diagnostic pop

namespace {
size_t assertions = 0, failures = 0, cases = 0;
std::string currentCase;
std::ofstream observations;

void check(bool ok, const char* expression, int line) {
  ++assertions;
  if (!ok) {
    ++failures;
    std::cerr << "FAIL [" << currentCase << ":" << line << "] " << expression << '\n';
  }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

void observeJson(const std::string& label, const std::string& payload) {
  // BinaryReader.ReadUInt32 consumes little-endian framing, not an MQTT/JSON encoding.
  const auto length = [](size_t value) {
    if (value > UINT32_MAX) throw std::runtime_error("observation too large");
    const uint32_t n = static_cast<uint32_t>(value);
    const char bytes[] = {static_cast<char>(n), static_cast<char>(n >> 8),
                          static_cast<char>(n >> 16), static_cast<char>(n >> 24)};
    observations.write(bytes, sizeof(bytes));
  };
  length(label.size());
  observations.write(label.data(), static_cast<std::streamsize>(label.size()));
  length(payload.size());
  observations.write(payload.data(), static_cast<std::streamsize>(payload.size()));
}

void resetFixture(bool connected = true, bool failInitialAllocation = false) {
  CHECK(MqttHeap::guardsOk());
  mqttClient.~PubSubClient();
  CHECK(MqttHeap::blocks.empty());
  mqttWifiClient = WiFiClient{};
  MqttHeap::resetProbe();
  if (failInitialAllocation) MqttHeap::failMalloc = 1;
  new (&mqttClient) PubSubClient(mqttWifiClient);
  mqttBufferReady = false;
  mqttLoopBusy = mqttPublishing = mqttCommandsBusy = mqttPendingRestart = false;
  pendingSmsCount = 0;
  for (auto& item : pendingSmsQueue) item = MqttPendingSms{};
  mqttPendingPing = false; mqttPendingPingHost = ""; ESP.restarts = 0;
  mqttStrings = {};
  config = Config{};
  config.mqttEnabled = true;
  config.mqttServer = "offline.invalid";
  config.mqttPort = 1883;
  config.mqttPrefix = "sms";
  config.mqttHaPrefix = "homeassistant";
  stats = Statistics{};
  lastResetReasonCode = 1;
  lastResetReasonText = "power-on";
  lastResetPowerSuspected = false;
  lastInterruptedOperation = {};
  lastMqttReconnectAttempt = lastMqttStatusReport = 0;
  WiFi = MqttTestWifi{};
  mqttModemCommands.clear();
  mqttLogQueueHead = mqttLogQueueTail = mqttLogQueueCount = 0;
  mqttLogQueueBusy = false;
  for (auto& entry : mqttLogQueue) entry = MqttLogEntry{};
  resetSmsHostClock();
  initMqttTopics();
  if (connected) {
    if (!ensureMqttBufferSize(256) || !mqttClient.connect("host-regression")) {
      throw std::runtime_error("fixture could not connect to in-memory CONNACK");
    }
  }
  mqttClient.setCallback(mqttCallback);
  mqttClient.clearObservations();
  MqttHeap::resetProbe();
  mqttStrings = {};
  Serial.reset();
  Serial1.reset();
  resetSmsHostClock();
}

template<class F> void runCase(const std::string& name, F body) {
  currentCase = name;
  ++cases;
  const size_t before = failures;
  try { body(); }
  catch (const std::exception& error) { check(false, error.what(), __LINE__); }
  CHECK(MqttHeap::guardsOk());
  std::cout << (before == failures ? "PASS " : "FAIL ") << name << '\n';
}

struct WirePacket {
  uint8_t header;
  size_t end;
  std::string body;
};

// Read-only protocol assertions over bytes accepted by the transport, not a replacement encoder.
WirePacket decodePacket(const std::string& wire, size_t at) {
  const auto header = static_cast<uint8_t>(wire.at(at++));
  size_t remaining = 0;
  for (unsigned shift = 0; shift < 28; shift += 7) {
    const auto byte = static_cast<uint8_t>(wire.at(at++));
    remaining |= size_t(byte & 0x7f) << shift;
    if (!(byte & 0x80)) {
      if (remaining > wire.size() - at) throw std::runtime_error("truncated MQTT packet");
      return {header, at + remaining, wire.substr(at, remaining)};
    }
  }
  throw std::runtime_error("invalid MQTT remaining length");
}

std::string readMqttString(const std::string& bytes, size_t& at) {
  size_t length = static_cast<uint8_t>(bytes.at(at++)) * 256U;
  length += static_cast<uint8_t>(bytes.at(at++));
  if (length > bytes.size() - at) throw std::runtime_error("truncated MQTT string");
  const auto text = bytes.substr(at, length);
  at += length;
  return text;
}

std::string publishedPayload(size_t index, bool jsonObject = true) {
  const auto& packet = mqttClient.publishes.at(index);
  CHECK(packet.began && packet.ended && packet.realEnd == 1);
  CHECK(packet.headerEnd <= packet.end && packet.end <= mqttWifiClient.wire.size());
  const auto decoded = decodePacket(mqttWifiClient.wire, packet.start);
  CHECK(decoded.header == (packet.retained ? 0x31 : 0x30));
  CHECK(decoded.end == packet.end);
  size_t at = 0;
  CHECK(readMqttString(decoded.body, at) == packet.topic);
  const auto payload = decoded.body.substr(at);
  CHECK(packet.headerEnd + payload.size() == packet.end);
  CHECK(payload.size() == packet.declared);
  if (jsonObject) observeJson(currentCase + "/" + packet.topic, payload);
  return payload;
}

void checkChunks(size_t index, const std::string& payload, uintptr_t source = 0) {
  size_t offset = 0, chunks = 0;
  for (const auto& write : mqttWifiClient.writes) {
    if (write.phase != MqttPhase::Payload || write.publish != index + 1) continue;
    if (!source) source = write.source;
    CHECK(write.source == source + offset);
    CHECK(write.requested.size() == std::min(MQTT_PUBLISH_CHUNK_BYTES, payload.size() - offset));
    CHECK(write.accepted == write.requested.size());
    CHECK(write.requested == payload.substr(offset, write.requested.size()));
    offset += write.requested.size();
    ++chunks;
  }
  CHECK(offset == payload.size());
  CHECK(chunks == (payload.size() + MQTT_PUBLISH_CHUNK_BYTES - 1) / MQTT_PUBLISH_CHUNK_BYTES);
  CHECK(mqttClient.publishes.at(index).chunks == chunks);
}

void testPublishBudgets() {
  for (const size_t length : {size_t(1), size_t(249), size_t(250), MQTT_MAX_TOPIC_BYTES}) {
    runCase("topic-bytes-" + std::to_string(length), [=] {
      resetFixture();
      const std::string topic(length, 't');
      CHECK(publishMqttPayload(topic.c_str(), "{}", true));
      CHECK(publishedPayload(0) == "{}");
      CHECK(mqttClient.getBufferSize() == std::max(size_t(256), MQTT_MAX_HEADER_SIZE + 2U + length));
      CHECK(MqttHeap::reallocCalls == (length > 249 ? 1U : 0U));
      CHECK(mqttWifiClient.stops == 0 && mqttClient.disconnectCalls == 0);
    });
  }
  for (const size_t length : {size_t(0), size_t(1), size_t(511), size_t(512), size_t(513),
                              MQTT_MAX_PAYLOAD_BYTES - 1, MQTT_MAX_PAYLOAD_BYTES}) {
    runCase("payload-bytes-" + std::to_string(length), [=] {
      resetFixture();
      const std::string payload(length, 'p'); // Intentionally raw, not a JSON observation.
      CHECK(publishMqttPayload("t", payload.c_str()));
      CHECK(publishedPayload(0, false) == payload);
      checkChunks(0, payload, reinterpret_cast<uintptr_t>(payload.c_str()));
      CHECK(mqttClient.getBufferSize() == 256 && mqttClient.bufferRequests.empty());
      CHECK(MqttHeap::mallocCalls == 0 && MqttHeap::reallocCalls == 0);
      CHECK(mqttStrings.reserves == 0 && mqttStrings.concats == 0);
      CHECK(mqttClient.loopCalls == 0 && mqttClient.disconnectCalls == 0);
      CHECK(smsTestYields == mqttClient.publishes.at(0).chunks + 1);
      CHECK(smsTestWdtChecks == smsTestYields && smsTestWdtResets == smsTestYields);
    });
  }
  runCase("invalid-publish-inputs-have-no-wire-side-effects", [] {
    resetFixture();
    const std::string topic(MQTT_MAX_TOPIC_BYTES + 1, 't');
    const std::string payload(MQTT_MAX_PAYLOAD_BYTES + 1, 'p');
    CHECK(!publishMqttPayload(nullptr, "{}"));
    CHECK(!publishMqttPayload("t", nullptr));
    CHECK(!publishMqttPayload("", "{}"));
    CHECK(!publishMqttPayload(topic.c_str(), "{}"));
    CHECK(!publishMqttPayload("t", payload.c_str()));
    CHECK(mqttClient.beginCalls == 0 && mqttClient.endCalls == 0);
    CHECK(mqttWifiClient.writes.empty() && mqttWifiClient.stops == 0);
    CHECK(mqttClient.connected() && mqttClient.bufferRequests.empty());
    CHECK(mqttLogQueueCount == 0 && smsTestYields == 5);
    CHECK(!ensureMqttBufferSize(std::numeric_limits<size_t>::max()));
  });
  runCase("offline-publish-is-rejected-before-begin", [] {
    resetFixture(false);
    CHECK(!publishMqttPayload("t", "{}"));
    CHECK(mqttClient.beginCalls == 0 && mqttWifiClient.writes.empty());
    CHECK(mqttWifiClient.stops == 0 && mqttClient.disconnectCalls == 0);
  });
  runCase("topic-header-oom-does-not-start-packet", [] {
    resetFixture();
    MqttHeap::failRealloc = 1;
    const std::string topic(MQTT_MAX_TOPIC_BYTES, 't');
    CHECK(!publishMqttPayload(topic.c_str(), "{}"));
    CHECK(mqttClient.beginCalls == 0 && mqttWifiClient.writes.empty());
    CHECK(mqttClient.getBufferSize() == 256 && !mqttBufferReady);
    CHECK(MqttHeap::reallocCalls == 1);
    CHECK(mqttWifiClient.stops == 0 && mqttLogQueueCount == 0);
    CHECK(publishMqttPayload(topic.c_str(), "{}"));
    CHECK(MqttHeap::reallocCalls == 2);
    CHECK(publishedPayload(0) == "{}");
  });
}

void testJsonBuilder() {
  runCase("builder-escaping-control-bytes-utf8-and-booleans", [] {
    resetFixture();
    std::string input;
    for (int c = 1; c < 32; ++c) input += static_cast<char>(c);
    input += u8"\"\\/\x7f中文";
    const MqttJsonField fields[] = {{"k\"\\\n", input.c_str()}, {"t", "true", false},
                                    {"f", "false", false}, {"empty", ""}};
    const std::string expected = "{\"k\\\"\\\\\\n\":\""
        "\\u0001\\u0002\\u0003\\u0004\\u0005\\u0006\\u0007\\b\\t\\n\\u000B\\f\\r"
        "\\u000E\\u000F\\u0010\\u0011\\u0012\\u0013\\u0014\\u0015\\u0016\\u0017"
        "\\u0018\\u0019\\u001A\\u001B\\u001C\\u001D\\u001E\\u001F"
        u8"\\\"\\\\/\x7f中文\",\"t\":true,\"f\":false,\"empty\":\"\"}";
    String json = "stale";
    CHECK(buildMqttJson(fields, 4, json));
    CHECK(json.c_str() == expected);
    CHECK(mqttStrings.capacities == std::vector<size_t>{expected.size()});
    CHECK(publishMqttPayload("test/json", json.c_str()));
    CHECK(publishedPayload(0) == expected);
  });
  for (const bool expanded : {false, true}) {
    runCase(expanded ? "builder-escaped-budget-exact-and-over" : "builder-ascii-budget-exact-and-over", [=] {
      resetFixture();
      const size_t available = MQTT_MAX_PAYLOAD_BYTES - (sizeof("{\"m\":\"\"}") - 1);
      std::string input = expanded ? std::string(available / 6, '\x01') + std::string(available % 6, 'a')
                                   : std::string(available, 'a');
      MqttJsonField field{"m", input.c_str()};
      String json;
      CHECK(buildMqttJson(&field, 1, json));
      CHECK(json.length() == MQTT_MAX_PAYLOAD_BYTES);
      CHECK(mqttStrings.capacities == std::vector<size_t>{MQTT_MAX_PAYLOAD_BYTES});
      CHECK(publishMqttPayload("test/json", json.c_str()));
      CHECK(publishedPayload(0) == json.c_str());
      input += 'a';
      field.value = input.c_str();
      mqttStrings = {};
      CHECK(!buildMqttJson(&field, 1, json));
      CHECK(json.length() == 0);
      CHECK(mqttStrings.reserves == 0 && mqttStrings.concats == 0);
    });
  }
  runCase("builder-invalid-fields-rejected-before-allocation", [] {
    resetFixture();
    String json = "stale";
    CHECK(!buildMqttJson(nullptr, 0, json));
    for (const auto field : {MqttJsonField{nullptr, "v"}, MqttJsonField{"k", nullptr},
                             MqttJsonField{"k", nullptr, false}, MqttJsonField{"k", "0", false},
                             MqttJsonField{"k", "TRUE", false}, MqttJsonField{"k", "true ", false}}) {
      CHECK(!buildMqttJson(&field, 1, json));
      CHECK(json.length() == 0);
    }
    CHECK(mqttStrings.reserves == 0 && mqttStrings.concats == 0);
  });
  runCase("builder-reserve-oom-clears-stale-output-and-recovers", [] {
    resetFixture();
    const MqttJsonField field{"m", "hello"};
    String json = "stale";
    mqttStrings.failReserve = 1;
    CHECK(!buildMqttJson(&field, 1, json));
    CHECK(json.length() == 0);
    CHECK(mqttStrings.reserves == 1 && mqttStrings.concats == 0);
    CHECK(buildMqttJson(&field, 1, json));
    CHECK(json == "{\"m\":\"hello\"}");
  });
  for (const bool silent : {false, true}) {
    for (const size_t position : {size_t(1), size_t(8), size_t(0)}) {
      runCase(std::string(silent ? "builder-lost-concat-" : "builder-concat-oom-") +
              (position ? std::to_string(position) : "last"), [=] {
        resetFixture();
        const MqttJsonField field{"m", "hello"};
        String json;
        CHECK(buildMqttJson(&field, 1, json));
        const size_t total = mqttStrings.concats;
        const size_t faultAt = position ? position : total;
        CHECK(faultAt > 0 && faultAt <= total);
        mqttStrings = {};
        if (silent) mqttStrings.loseConcat = faultAt;
        else mqttStrings.failConcat = faultAt;
        CHECK(!buildMqttJson(&field, 1, json));
        CHECK(json.length() == 0);
        CHECK(mqttStrings.concats == (silent ? total : faultAt));
        mqttStrings = {};
        CHECK(buildMqttJson(&field, 1, json));
        CHECK(json == "{\"m\":\"hello\"}");
      });
    }
  }
}

std::string repeatText(const std::string& text, size_t count) {
  std::string result;
  result.reserve(text.size() * count);
  for (size_t i = 0; i < count; ++i) result += text;
  return result;
}

template<class F> bool queuedLog(F matches) {
  for (size_t i = 0; i < mqttLogQueueCount; ++i) {
    if (matches(mqttLogQueue[(mqttLogQueueTail + i) % MQTT_LOG_QUEUE_SIZE])) return true;
  }
  return false;
}

void testLongSms() {
  // Input is already assembled: reassembly belongs to the separate SMS regression suite.
  for (const size_t characters : {size_t(280), size_t(300), size_t(MAX_CONCAT_PARTS * 160)}) {
    runCase("sms-utf8-characters-" + std::to_string(characters), [=] {
      resetFixture();
      config.mqttHaDiscovery = true;
      // Two code points, 3 + 4 UTF-8 bytes; ASCII escapes prevent lost non-BMP fixtures.
      const std::string message = repeatText(u8"\u4E2D\U0001F600", characters / 2);
      CHECK(message.size() == characters / 2 * 7);
      CHECK(std::count_if(message.begin(), message.end(), [](unsigned char c) {
        return (c & 0xC0) != 0x80;
      }) == characters);
      const std::string expected = "{\"event_type\":\"sms_received\",\"sender\":\"+8613800000000\",\"message\":\"" +
          message + "\",\"time\":\"2026-05-22 12:34:56\",\"timestamp\":\"2026-05-22 12:34:56\",\"device\":\"123456\"}";
      publishMqttSmsReceived("+8613800000000", message.c_str(), "2026-05-22 12:34:56");
      CHECK(mqttClient.beginCalls == 2 && mqttClient.endCalls == 2);
      CHECK(mqttClient.publishes.at(0).topic == mqttTopicSmsReceived.c_str());
      CHECK(mqttClient.publishes.at(1).topic == mqttHaSmsReceivedTopic.c_str());
      for (size_t i = 0; i < 2; ++i) {
        CHECK(!mqttClient.publishes.at(i).retained);
        CHECK(publishedPayload(i) == expected);
        checkChunks(i, expected);
      }
      CHECK(expected.size() > 1024 && mqttClient.getBufferSize() == 256);
      CHECK(mqttStrings.capacities == std::vector<size_t>{expected.size()});
      CHECK(queuedLog([](const MqttLogEntry& log) { return log.message == "MQTT短信推送完成"; }));
      publishMqttSmsSent("+8613800000000", message.c_str(), false);
      const std::string sent = "{\"success\":false,\"phone\":\"+8613800000000\",\"message\":\"" +
                               message + "\",\"device\":\"123456\"}";
      CHECK(mqttClient.beginCalls == 3 && mqttClient.endCalls == 3);
      CHECK(mqttClient.publishes.at(2).topic == mqttTopicSmsSent.c_str());
      CHECK(publishedPayload(2) == sent);
      checkChunks(2, sent);
      CHECK(MqttHeap::reallocCalls == 0 && mqttClient.loopCalls == 0);
      CHECK(smsTestYields == mqttClient.publishes.at(0).chunks + mqttClient.publishes.at(1).chunks +
                             mqttClient.publishes.at(2).chunks + 3);
    });
  }
  runCase("sms-max10-parts-control-escape-expansion", [] {
    resetFixture();
    CHECK(MAX_CONCAT_PARTS == 10);
    const std::string message(MAX_CONCAT_PARTS * 160, '\x01');
    const std::string expected = "{\"success\":true,\"phone\":\"10086\",\"message\":\"" +
        repeatText("\\u0001", message.size()) + "\",\"device\":\"123456\"}";
    publishMqttSmsSent("10086", message.c_str(), true);
    CHECK(mqttClient.beginCalls == 1 && mqttClient.endCalls == 1);
    CHECK(publishedPayload(0) == expected);
    CHECK(expected.size() > 9600 && expected.size() < MQTT_MAX_PAYLOAD_BYTES);
    CHECK(mqttStrings.capacities == std::vector<size_t>{expected.size()});
    CHECK(mqttClient.getBufferSize() == 256 && MqttHeap::reallocCalls == 0);
    checkChunks(0, expected);
  });
  for (const bool received : {true, false}) {
    for (const std::string fault : {"reserve", "concat", "lost-concat"}) {
      runCase(std::string(received ? "sms-received-json-" : "sms-sent-json-") + fault, [=] {
        resetFixture();
        config.mqttHaDiscovery = true;
        if (fault == "reserve") mqttStrings.failReserve = 1;
        else if (fault == "concat") mqttStrings.failConcat = 8;
        else mqttStrings.loseConcat = 8;
        if (received) publishMqttSmsReceived("10086", "hello", "time");
        else publishMqttSmsSent("10086", "hello", true);
        CHECK(mqttStrings.reserves == 1);
        CHECK(mqttStrings.concats == 0 || mqttStrings.concats >= 8);
        CHECK(mqttClient.beginCalls == 0 && mqttClient.endCalls == 0);
        CHECK(mqttWifiClient.writes.empty() && mqttWifiClient.stops == 0);
        CHECK(mqttClient.connected());
        CHECK(queuedLog([](const MqttLogEntry& log) {
          return log.kind == MQTT_LOG_KIND_ERROR && log.error == "json_build_failed" && !log.success;
        }));
        CHECK(!queuedLog([](const MqttLogEntry& log) { return log.message.indexOf("完成") >= 0; }));
      });
    }
  }
}

void checkAbortedPublish() {
  CHECK(mqttWifiClient.stops >= 1 && !mqttWifiClient.linkUp);
  CHECK(mqttClient.disconnectCalls == 0 && mqttClient.loopCalls == 0);
  CHECK(mqttLogQueueCount == 0 && !mqttLogQueueBusy); // Failure logging must not recurse.
  CHECK(std::none_of(mqttWifiClient.writes.begin(), mqttWifiClient.writes.end(), [](const MqttWrite& write) {
    return write.phase == MqttPhase::Disconnect || write.phase == MqttPhase::Other;
  }));
  const auto stop = std::find(mqttWifiClient.events.begin(), mqttWifiClient.events.end(), "tcp.stop");
  CHECK(stop != mqttWifiClient.events.end());
  CHECK(std::none_of(stop, mqttWifiClient.events.end(), [](const std::string& event) {
    return event.rfind("tcp.write:", 0) == 0;
  }));
  CHECK(smsTestYields > 0 && smsTestWdtResets == smsTestYields);
}

void testTransportFailures() {
  for (const size_t accepted : {size_t(0), size_t(1), size_t(7)}) {
    runCase("header-short-write-" + std::to_string(accepted), [=] {
      resetFixture();
      const std::string payload(1300, 'x');
      mqttWifiClient.fault = {MqttPhase::Header, 1, 0, accepted, false};
      CHECK(!publishMqttPayload("fault/topic", payload.c_str()));
      CHECK(mqttWifiClient.faultHits == 1);
      CHECK(mqttClient.beginCalls == 1 && mqttClient.endCalls == 0);
      CHECK(!mqttClient.publishes.at(0).began && mqttClient.publishes.at(0).chunks == 0);
      CHECK(mqttWifiClient.writes.size() == 1);
      CHECK(mqttWifiClient.wire == mqttWifiClient.writes.at(0).requested.substr(0, accepted));
      checkAbortedPublish();
    });
  }
  for (const size_t chunk : {size_t(1), size_t(2), size_t(3)}) {
    for (const size_t accepted : {size_t(0), size_t(17)}) {
      runCase("payload-short-write-chunk-" + std::to_string(chunk) + "-accepted-" + std::to_string(accepted), [=] {
        resetFixture();
        const std::string payload(1300, 'x');
        mqttWifiClient.fault = {MqttPhase::Payload, 1, chunk, accepted, false};
        CHECK(!publishMqttPayload("fault/topic", payload.c_str()));
        CHECK(mqttWifiClient.faultHits == 1);
        CHECK(mqttClient.beginCalls == 1 && mqttClient.endCalls == 0);
        const auto& packet = mqttClient.publishes.at(0);
        CHECK(packet.began && !packet.ended && packet.chunks == chunk);
        CHECK(mqttWifiClient.writes.size() == chunk + 1);
        CHECK(mqttWifiClient.wire.substr(packet.headerEnd) == payload.substr(0, (chunk - 1) * 512 + accepted));
        CHECK(mqttWifiClient.writes.back().source == reinterpret_cast<uintptr_t>(payload.c_str()) + (chunk - 1) * 512);
        checkAbortedPublish();
      });
    }
  }
  for (const size_t chunk : {size_t(1), size_t(3)}) {
    runCase("disconnect-after-full-chunk-" + std::to_string(chunk), [=] {
      resetFixture();
      const std::string payload(1300, 'x');
      mqttWifiClient.fault = {MqttPhase::Payload, 1, chunk, std::numeric_limits<size_t>::max(), true};
      CHECK(!publishMqttPayload("fault/topic", payload.c_str()));
      CHECK(mqttWifiClient.faultHits == 1);
      const auto& packet = mqttClient.publishes.at(0);
      CHECK(mqttClient.endCalls == (chunk == 3 ? 1U : 0U));
      CHECK(mqttWifiClient.wire.substr(packet.headerEnd) == payload.substr(0, chunk == 3 ? payload.size() : 512));
      if (chunk == 3) CHECK(packet.ended && packet.realEnd == 1);
      else CHECK(mqttWifiClient.writes.back().accepted == 0);
      checkAbortedPublish();
    });
  }
  for (const std::string fault : {"before-begin", "at-end", "end-return-zero"}) {
    runCase("connection-seam-" + fault, [=] {
      resetFixture();
      if (fault == "before-begin") mqttClient.disconnectBeforeBegin = 1;
      else if (fault == "at-end") mqttClient.disconnectAtEnd = 1;
      else mqttClient.failEnd = 1; // Defensive branch only; real PubSubClient 2.8 always returns 1.
      CHECK(!publishMqttPayload("fault/topic", "{}"));
      CHECK(mqttClient.seamFaultHits == 1);
      CHECK(mqttClient.endCalls == (fault == "before-begin" ? 0U : 1U));
      if (fault == "before-begin") CHECK(mqttWifiClient.writes.empty());
      else CHECK(publishedPayload(0) == "{}");
      checkAbortedPublish();
    });
  }
  runCase("watchdog-not-registered-still-yields", [] {
    resetFixture();
    smsTestWdtStatus = -1;
    CHECK(publishMqttPayload("t", "{}"));
    CHECK(publishedPayload(0) == "{}");
    CHECK(smsTestYields == 2 && smsTestWdtChecks == 2 && smsTestWdtResets == 0);
  });
}

void testDualTopics() {
  struct Notification {
    const char* name;
    void (*send)();
    String* userTopic;
    String* haTopic;
    const char* completion;
    const char* timing;
    bool retained;
  };
  const Notification notifications[] = {
    {"sms", [] { publishMqttSmsReceived("10086", "hello", "time"); },
     &mqttTopicSmsReceived, &mqttHaSmsReceivedTopic, "MQTT短信推送完成", "", false},
    {"call", [] { publishMqttCallReceived("10086", "time"); },
     &mqttTopicCallReceived, &mqttHaCallReceivedTopic, "MQTT来电推送完成", "", false},
    {"status", [] { publishMqttStatus("online"); },
     &mqttTopicStatus, &mqttHaStatusTopic, "MQTT发布状态: online", "MQTT发布状态", true},
    {"device-status", [] { publishMqttDeviceStatus(); },
     &mqttTopicStatus, &mqttHaStatusTopic, "MQTT上报设备状态", "MQTT设备状态上报", true}
  };
  for (const auto& notification : notifications) {
    for (const std::string mode : {"success", "ha-disabled", "user-invalid", "ha-invalid", "user-short", "ha-short"}) {
      runCase(std::string("dual-") + notification.name + "-" + mode, [&] {
        resetFixture();
        config.mqttHaDiscovery = mode != "ha-disabled";
        const std::string user = notification.userTopic->c_str(), ha = notification.haTopic->c_str();
        if (mode == "user-invalid") *notification.userTopic = String(std::string(MQTT_MAX_TOPIC_BYTES + 1, 't'));
        if (mode == "ha-invalid") *notification.haTopic = String(std::string(MQTT_MAX_TOPIC_BYTES + 1, 't'));
        if (mode == "user-short" || mode == "ha-short") {
          mqttWifiClient.fault = {MqttPhase::Header, mode == "user-short" ? 1U : 2U, 0, 1, false};
        }
        notification.send();
        const bool success = mode == "success" || mode == "ha-disabled";
        const size_t expectedBegins = mode == "success" || mode == "ha-short" ? 2 : 1;
        const size_t expectedEnds = mode == "success" ? 2 : mode == "user-short" ? 0 : 1;
        CHECK(mqttClient.beginCalls == expectedBegins && mqttClient.endCalls == expectedEnds);
        CHECK(queuedLog([&](const MqttLogEntry& log) {
          return log.kind == MQTT_LOG_KIND_INFO && log.message == notification.completion;
        }) == success);
        if (*notification.timing) {
          CHECK(queuedLog([&](const MqttLogEntry& log) {
            return log.kind == MQTT_LOG_KIND_TIMING && log.step == notification.timing && log.success == success;
          }));
        }
        if (!success) {
          const char* error = mode.rfind("user-", 0) == 0 ? "user_topic_publish_failed" : "ha_topic_publish_failed";
          CHECK(queuedLog([&](const MqttLogEntry& log) {
            return log.kind == MQTT_LOG_KIND_ERROR && log.error == error && !log.success;
          }));
        }
        if (mode == "user-short") {
          CHECK(queuedLog([](const MqttLogEntry& log) { return log.error == "ha_topic_publish_failed"; }));
        }
        for (size_t i = 0; i < mqttClient.publishes.size(); ++i) {
          CHECK(mqttClient.publishes[i].retained == notification.retained);
          CHECK(mqttClient.publishes[i].topic == (mode == "user-invalid" || i == 1 ? ha : user));
          if (mqttClient.publishes[i].ended) {
            const auto payload = publishedPayload(i);
            if (i == 1) CHECK(payload == mqttWifiClient.wire.substr(mqttClient.publishes[0].headerEnd,
                                 mqttClient.publishes[0].declared));
          }
        }
        const bool shortWrite = mode == "user-short" || mode == "ha-short";
        CHECK(mqttWifiClient.faultHits == (shortWrite ? 1U : 0U));
        CHECK((mqttWifiClient.stops > 0) == shortWrite);
        CHECK(mqttWifiClient.linkUp != shortWrite);
        CHECK(mqttClient.disconnectCalls == 0 && mqttClient.loopCalls == 0);
        CHECK(std::none_of(mqttWifiClient.writes.begin(), mqttWifiClient.writes.end(), [](const MqttWrite& write) {
          return write.phase == MqttPhase::Disconnect;
        }));
      });
    }
  }
}

void testReconnectDiscovery() {
  for (const std::string& prefix : {std::string("sms"), std::string("custom/root"),
                                   std::string("quoted\"/slash\\root"), std::string(900, 'p')}) {
    runCase("real-reconnect-discovery-" + prefix.substr(0, 24), [&] {
      resetFixture(false);
      config.mqttHaDiscovery = true;
      config.mqttPrefix = String(prefix);
      config.mqttHaPrefix = prefix == "sms" ? String("") : String("ha/" + prefix);
      config.mqttUser = "synthetic-user";
      config.mqttPass = "synthetic-pass";
      initMqttTopics();
      const std::string statusTopic = prefix + "/123456/status";
      const std::string haPrefix = prefix == "sms" ? "homeassistant" : "ha/" + prefix;
      for (int connection = 0; connection < 2; ++connection) {
        mqttReconnect(); // Execute production CONNECT/subscriptions/discovery/online, not just extracted text.
        CHECK(mqttClient.connected() && mqttWifiClient.connects == 1);
        const auto connect = decodePacket(mqttWifiClient.wire, 0);
        CHECK(connect.header == 0x10);
        size_t at = 0;
        CHECK(readMqttString(connect.body, at) == "MQTT");
        CHECK(static_cast<uint8_t>(connect.body.at(at++)) == 4);
        CHECK(static_cast<uint8_t>(connect.body.at(at++)) == 0xEE); // user/pass, retained QoS1 will, clean session
        const unsigned int keepalive = static_cast<uint8_t>(connect.body.at(at++)) * 256U;
        CHECK(keepalive + static_cast<uint8_t>(connect.body.at(at++)) == 15);
        CHECK(readMqttString(connect.body, at) == "sms_123456");
        CHECK(readMqttString(connect.body, at) == statusTopic);
        const auto will = readMqttString(connect.body, at);
        CHECK(will == "{\"status\":\"offline\",\"device\":\"123456\"}");
        observeJson(currentCase + "/CONNECT-LWT", will);
        CHECK(readMqttString(connect.body, at) == "synthetic-user");
        CHECK(readMqttString(connect.body, at) == "synthetic-pass" && at == connect.body.size());
        CHECK(mqttClient.publishes.size() == 27); // 25 configs, retained user + HA status
        for (size_t i = 0; i < mqttClient.publishes.size(); ++i) {
          CHECK(mqttClient.publishes[i].retained);
          const auto payload = publishedPayload(i); // Independent strict JSON parser checks every actual packet.
          if (i < 25) CHECK(mqttClient.publishes[i].topic.rfind(haPrefix + "/", 0) == 0);
          if (mqttClient.publishes[i].topic == haPrefix + "/binary_sensor/sms_forwarder_123456_online/config") {
            CHECK(payload.find(std::string("\"state_topic\":\"") + jsonEscape(String(statusTopic)).c_str() + "\"") != std::string::npos);
            CHECK(payload.find("\"payload_off\":\"offline\"") != std::string::npos);
            CHECK(payload.find("availability") == std::string::npos); // offline_alert must see off, not unavailable.
          }
        }
        CHECK(mqttClient.publishes[25].topic == statusTopic);
        mqttWifiClient.stop();
        mqttClient.clearObservations();
      }
    });
  }
}

void feedCommand(const std::string& topic, const std::string& payload) {
  // Use a second real PubSubClient as the encoder of an incoming broker PUBLISH.
  WiFiClient senderTransport;
  MqttRealPubSubClient sender(senderTransport);
  sender.setServer("encoder.invalid", 1883);
  CHECK(sender.setBufferSize(static_cast<uint16_t>(payload.size() + topic.size() + 16)));
  CHECK(sender.connect("broker-fixture"));
  senderTransport.clearObservations();
  CHECK(sender.publish(topic.c_str(), payload.c_str()));
  mqttWifiClient.input.insert(mqttWifiClient.input.end(), senderTransport.wire.begin(), senderTransport.wire.end());
}

void testKeepaliveAndInbound() {
  runCase("protected-service-survives-60s-and-queues-control", [] {
    resetFixture();
    CHECK(ensureMqttBufferSize(MQTT_RECEIVE_BUFFER_BYTES));
    for (int i = 0; i < 60; ++i) {
      delay(1000);
      if (i == 20) feedCommand(mqttTopicSmsSend.c_str(), R"({"phone":"10086","message":"during wait"})");
      if (i == 25) feedCommand(mqttTopicCmd.c_str(), R"({"action":"restart"})");
      serviceMqtt();
      CHECK(mqttClient.connected() && ESP.restarts == 0);
    }
    CHECK(pendingSmsCount == 1 && pendingSmsQueue[0].message == "during wait" && mqttPendingRestart);
    CHECK(std::count_if(mqttWifiClient.writes.begin(), mqttWifiClient.writes.end(), [](const MqttWrite& write) {
      return write.requested == std::string("\xc0\x00", 2);
    }) >= 3);
    publishMqttSmsSent("10086", "final result", true);
    CHECK(mqttClient.publishes.size() == 1 && publishedPayload(0).find("\"success\":true") != std::string::npos);
    const auto loops = mqttClient.loopCalls;
    mqttPublishing = true; serviceMqtt(); mqttPublishing = false;
    mqttLoopBusy = true; serviceMqtt(); mqttLoopBusy = false;
    CHECK(mqttClient.loopCalls == loops);
  });
  runCase("keepalive-missing-response-disconnects", [] {
    resetFixture(); mqttWifiClient.answerPing = false;
    delay(16000); serviceMqtt(); CHECK(mqttClient.connected());
    delay(16000); serviceMqtt(); CHECK(!mqttClient.connected());
  });
  for (const size_t length : {size_t(1024), size_t(8192), size_t(8193)}) {
    runCase("real-inbound-json-bytes-" + std::to_string(length), [=] {
      resetFixture(false); mqttReconnect(); mqttClient.clearObservations();
      std::string payload = R"({"phone":"10086","message":"tail OTP 123456","padding":""})";
      payload.insert(payload.size() - 2, length - payload.size(), 'x');
      feedCommand(mqttTopicSmsSend.c_str(), payload);
      serviceMqtt();
      CHECK(mqttClient.getBufferSize() == MQTT_RECEIVE_BUFFER_BYTES);
      CHECK(pendingSmsCount == (length <= MQTT_MAX_COMMAND_BYTES ? 1U : 0U));
      if (pendingSmsCount) CHECK(pendingSmsQueue[0].message == "tail OTP 123456");
      else CHECK(queuedLog([](const MqttLogEntry& log) { return log.error == "invalid_payload_length_max8192"; }));
    });
  }
  runCase("receive-buffer-oom-prevents-connect-and-recovers", [] {
    resetFixture(false); MqttHeap::failRealloc = 1;
    mqttReconnect(); CHECK(!mqttClient.connected() && mqttWifiClient.connects == 0);
    MqttHeap::failRealloc = 0; mqttReconnect();
    CHECK(mqttClient.connected() && mqttClient.getBufferSize() == MQTT_RECEIVE_BUFFER_BYTES);
  });
}
} // namespace

int main() {
  observations.exceptions(std::ios::failbit | std::ios::badbit);
  observations.open("mqtt_json_observations.bin", std::ios::binary | std::ios::trunc);
  runCase("buffer-confirm-reuse-and-limit", [] {
    resetFixture(false);
    CHECK(ensureMqttBufferSize(1024));
    CHECK(mqttClient.getBufferSize() == 1024);
    CHECK(mqttClient.bufferRequests == std::vector<size_t>{1024});
    CHECK(MqttHeap::reallocCalls == 1);
    CHECK(ensureMqttBufferSize(512));
    CHECK(mqttClient.bufferRequests.size() == 1);
    CHECK(!ensureMqttBufferSize(0));
    CHECK(!ensureMqttBufferSize(MQTT_MAX_BUFFER_BYTES + 1));
    CHECK(mqttClient.getBufferSize() == 1024);
  });
  runCase("initial-malloc-failure-is-not-a-ready-buffer", [] {
    resetFixture(false, true);
    CHECK(mqttClient.getBufferSize() > 0 && MqttHeap::blocks.empty());
    MqttHeap::failRealloc = 1;
    CHECK(!ensureMqttBufferSize(128));
    CHECK(!mqttBufferReady && MqttHeap::blocks.empty());
    CHECK(MqttHeap::reallocCalls == 1);
    MqttHeap::failRealloc = 0;
    CHECK(ensureMqttBufferSize(128));
    CHECK(mqttBufferReady && MqttHeap::blocks.size() == 1);
    CHECK(MqttHeap::reallocCalls == 2);
    CHECK(ensureMqttBufferSize(128));
    CHECK(MqttHeap::reallocCalls == 2);
  });
  runCase("realloc-failure-preserves-storage-and-requires-reconfirmation", [] {
    resetFixture();
    const auto original = MqttHeap::blocks.begin()->first;
    MqttHeap::failRealloc = 1;
    CHECK(!ensureMqttBufferSize(MQTT_MAX_BUFFER_BYTES));
    CHECK(!mqttBufferReady && mqttClient.getBufferSize() == 256);
    CHECK(MqttHeap::blocks.size() == 1 && MqttHeap::blocks.count(original) == 1);
    CHECK(ensureMqttBufferSize(128));
    CHECK(MqttHeap::reallocCalls == 2 && mqttBufferReady);
    CHECK(ensureMqttBufferSize(MQTT_MAX_BUFFER_BYTES));
    CHECK(mqttClient.getBufferSize() == MQTT_MAX_BUFFER_BYTES);
    CHECK(!ensureMqttBufferSize(MQTT_MAX_BUFFER_BYTES + 1));
    CHECK(MqttHeap::reallocCalls == 3);
  });
  runCase("actual-builder-and-stream-publish", [] {
    resetFixture();
    const MqttJsonField fields[] = {{"message", "hello"}, {"success", "true", false}};
    String json;
    CHECK(buildMqttJson(fields, 2, json));
    CHECK(json == "{\"message\":\"hello\",\"success\":true}");
    CHECK(mqttStrings.reserves == 1);
    CHECK(mqttStrings.capacities == std::vector<size_t>{json.length()});
    CHECK(publishMqttPayload(mqttTopicSmsSent.c_str(), json.c_str()));
    CHECK(mqttClient.beginCalls == 1 && mqttClient.endCalls == 1);
    CHECK(publishedPayload(0) == json.c_str());
    CHECK(mqttClient.loopCalls == 0 && mqttClient.disconnectCalls == 0);
    CHECK(mqttWifiClient.stops == 0);
    CHECK(smsTestYields == 2 && smsTestWdtResets == 2);
  });
  testPublishBudgets();
  testJsonBuilder();
  testLongSms();
  testTransportFailures();
  testDualTopics();
  testReconnectDiscovery();
  testKeepaliveAndInbound();
  observations.close();
  std::ofstream summary("mqtt_summary.tsv", std::ios::trunc);
  summary << assertions << '\t' << failures << '\t' << cases << '\n';
  summary.close();
  if (!summary) return 2;
  std::cout << "Native MQTT: " << assertions << " assertions, " << failures
            << " failures, " << cases << " cases\n";
  return failures ? 1 : 0;
}
