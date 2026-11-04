/*
 * mqtt_handler.ino - MQTT 功能实现
 * 
 * 支持两类主题:
 * 1. 用户自定义前缀 (如 sms/device_id/...)
 * 2. Home Assistant MQTT 自动发现 (homeassistant/...)
 */

#include <PubSubClient.h>
#include <esp_task_wdt.h>
#include "push_service.h"
#include "web_handlers.h"
#include "modem_handler.h"
#include "json_input.h"
#include <utility>

static bool mqttBufferReady = false;
static bool mqttLoopBusy = false;
static bool mqttPublishing = false;
static_assert(MQTT_MAX_COMMAND_BYTES == JsonInput::MAX_INPUT_BYTES, "MQTT receive/parser budgets must agree");

void serviceMqtt() {
  if (mqttLoopBusy || mqttPublishing || !config.mqttEnabled || !mqttClient.connected()) return;
  mqttLoopBusy = true;
  mqttClient.loop();  // callback 只排队 AT/重启；不要在这里处理待办或重连。
  mqttLoopBusy = false;
}

bool ensureMqttBufferSize(size_t minimumBytes) {
  const size_t currentBytes = mqttClient.getBufferSize();
  const size_t targetBytes = minimumBytes > currentBytes ? minimumBytes : currentBytes;
  if (minimumBytes == 0 || targetBytes > MQTT_MAX_BUFFER_BYTES) {
    Serial.printf("[MQTT] 缓冲请求超限: %u (max=%u)\n", (unsigned int)targetBytes, (unsigned int)MQTT_MAX_BUFFER_BYTES);
    return false;
  }
  if (mqttBufferReady && currentBytes >= minimumBytes) return true;
  // 2.8 首次 malloc 失败也可能留下非零 bufferSize，不能仅凭 getBufferSize 判定可用。
  mqttBufferReady = mqttClient.setBufferSize((uint16_t)targetBytes);
  if (!mqttBufferReady) {
    Serial.printf("[MQTT] 缓冲分配失败: %u bytes\n", (unsigned int)targetBytes);
  }
  return mqttBufferReady;
}

static void yieldMqttPublish() {
  if (esp_task_wdt_status(NULL) == ESP_OK) {
    esp_task_wdt_reset();
  }
  yield();
}

static bool publishMqttPayload(const char* topic, const char* payload, bool retained = false) {
  const auto fail = [topic](const char* reason, bool abortPacket) {
    // 不发送 MQTT DISCONNECT：它会被 broker 当作未完成 PUBLISH 的正文。
    if (abortPacket) mqttWifiClient.stop();
    // 日志主题也走此入口；这里只写串口，禁止 publishErrorLog 递归入队。
    Serial.printf("[MQTT] 发布失败: %s, topic=%.*s\n", reason,
                  (int)MQTT_MAX_TOPIC_BYTES, topic ? topic : "<null>");
    yieldMqttPublish();
    return false;
  };
  if (!topic || !payload) return fail("null_topic_or_payload", false);
  const size_t topicLen = strnlen(topic, MQTT_MAX_TOPIC_BYTES + 1U);
  const size_t payloadLen = strnlen(payload, MQTT_MAX_PAYLOAD_BYTES + 1U);
  if (topicLen == 0 || topicLen > MQTT_MAX_TOPIC_BYTES) return fail("topic_length_max1024", false);
  if (payloadLen > MQTT_MAX_PAYLOAD_BYTES) return fail("payload_length_max16384", false);
  if (!mqttClient.connected()) return fail("not_connected", false);
  if (mqttPublishing) return fail("nested_publish_rejected", false);
  struct PublishGuard {
    PublishGuard() { mqttPublishing = true; }
    ~PublishGuard() { mqttPublishing = false; }
  } publishing;

  // beginPublish 内部 writeString 不检查容量；仅为 5 + 2 + topicLen 扩容，不复制正文。
  if (!ensureMqttBufferSize(MQTT_MAX_HEADER_SIZE + 2U + topicLen)) {
    return fail("topic_header_buffer_unavailable", false);
  }
  if (!mqttClient.beginPublish(topic, (unsigned int)payloadLen, retained)) {
    return fail("begin_publish_failed_or_short_header", true);
  }
  size_t sent = 0;
  while (sent < payloadLen) {
    const size_t remaining = payloadLen - sent;
    const size_t chunk = remaining < MQTT_PUBLISH_CHUNK_BYTES ? remaining : MQTT_PUBLISH_CHUNK_BYTES;
    const size_t written = mqttClient.write(reinterpret_cast<const uint8_t*>(payload) + sent, chunk);
    if (written != chunk) {
      Serial.printf("[MQTT] 短写: offset=%u expected=%u actual=%u\n",
                    (unsigned int)sent, (unsigned int)chunk, (unsigned int)written);
      return fail("payload_short_write", true);
    }
    sent += written;
    // 包未结束前只喂狗/yield，不能调用 mqttClient.loop 或处理其他发布回调。
    yieldMqttPublish();
  }
  // PubSubClient 2.8 endPublish 恒返回 1；成功必须同时满足逐块/总字节数和连接检查。
  const int ended = mqttClient.endPublish();
  if (ended != 1 || sent != payloadLen || !mqttClient.connected()) {
    return fail("end_publish_failed_or_disconnected", true);
  }
  yieldMqttPublish();
  return true;
}

static bool appendMqttJsonBytes(String* output, size_t& size, size_t limit, const char* bytes, size_t count) {
  if (size > limit || count > limit - size) return false;
  if (output && !output->concat(bytes, (unsigned int)count)) return false;
  size += count;
  return true;
}

static bool appendMqttJsonQuoted(String* output, size_t& size, size_t limit, const char* text) {
  if (!text || !appendMqttJsonBytes(output, size, limit, "\"", 1)) return false;
  for (size_t i = 0; i <= MQTT_MAX_PAYLOAD_BYTES; ++i) {
    const uint8_t c = (uint8_t)text[i];
    if (c == 0) return appendMqttJsonBytes(output, size, limit, "\"", 1);
    const char* escape = nullptr;
    switch (c) {
      case '"': escape = "\\\""; break;
      case '\\': escape = "\\\\"; break;
      case '\n': escape = "\\n"; break;
      case '\r': escape = "\\r"; break;
      case '\t': escape = "\\t"; break;
      case '\b': escape = "\\b"; break;
      case '\f': escape = "\\f"; break;
    }
    if (escape) {
      if (!appendMqttJsonBytes(output, size, limit, escape, 2)) return false;
    } else if (c < 0x20) {
      const char hex[] = "0123456789ABCDEF";
      const char encoded[] = {'\\', 'u', '0', '0', hex[c >> 4], hex[c & 0x0F]};
      if (!appendMqttJsonBytes(output, size, limit, encoded, sizeof(encoded))) return false;
    } else {
      // UTF-8 按原始字节保留，不按字符截断。
      if (!appendMqttJsonBytes(output, size, limit, text + i, 1)) return false;
    }
  }
  return false;
}

static bool writeMqttJsonFields(String* output, size_t& size, size_t limit, const MqttJsonField* fields, size_t count) {
  if (!fields || !appendMqttJsonBytes(output, size, limit, "{", 1)) return false;
  for (size_t i = 0; i < count; ++i) {
    if (i && !appendMqttJsonBytes(output, size, limit, ",", 1)) return false;
    if (!appendMqttJsonQuoted(output, size, limit, fields[i].name) ||
        !appendMqttJsonBytes(output, size, limit, ":", 1)) return false;
    if (fields[i].quoted) {
      if (!appendMqttJsonQuoted(output, size, limit, fields[i].value)) return false;
    } else {
      // 不接受任意未转义 JSON，只允许内部布尔常量。
      if (!fields[i].value) return false;
      const size_t length = strcmp(fields[i].value, "true") == 0 ? 4U :
                            strcmp(fields[i].value, "false") == 0 ? 5U : 0U;
      if (length == 0 || !appendMqttJsonBytes(output, size, limit, fields[i].value, length)) return false;
    }
  }
  return appendMqttJsonBytes(output, size, limit, "}", 1);
}

static bool buildMqttJson(const MqttJsonField* fields, size_t count, String& json) {
  json.remove(0);
  size_t expected = 0;
  // 第一遍只计量（含转义、键和标点），不复制输入、不申请中间 String。
  if (!writeMqttJsonFields(nullptr, expected, MQTT_MAX_PAYLOAD_BYTES, fields, count)) {
    Serial.println("[MQTT] JSON拒绝: invalid_field_or_payload_max16384");
    return false;
  }
  if (!json.reserve((unsigned int)expected)) {
    Serial.printf("[MQTT] JSON拒绝: allocation_failed, bytes=%u\n", (unsigned int)expected);
    return false;
  }
  size_t actual = 0;
  // 第二遍使用同一编码逻辑和精确容量；任一追加失败都不能作为合法 JSON 发布。
  if (!writeMqttJsonFields(&json, actual, expected, fields, count) ||
      actual != expected || json.length() != expected) {
    json.remove(0);
    Serial.println("[MQTT] JSON拒绝: write_failed_or_size_mismatch");
    return false;
  }
  return true;
}

static void logMqttElapsed(const char* label, unsigned long start, bool success = true) {
  unsigned long elapsed = millis() - start;
  Serial.printf("[耗时] END %s: elapsed=%lums\n", label, elapsed);
  publishTimingLog("mqtt", label, elapsed, success);
}

static const uint8_t MQTT_LOG_QUEUE_SIZE = 16;
static const uint8_t MQTT_LOG_PROCESS_BURST = 2;
static const unsigned long MQTT_SLOW_LOG_THRESHOLD_MS = 1000;
static MqttLogEntry mqttLogQueue[MQTT_LOG_QUEUE_SIZE];
static uint8_t mqttLogQueueHead = 0;
static uint8_t mqttLogQueueTail = 0;
static uint8_t mqttLogQueueCount = 0;
static bool mqttLogQueueBusy = false;

static String truncateLogText(const String& text, size_t maxLen) {
  if (text.length() <= maxLen) return text;
  if (maxLen <= 3) return text.substring(0, maxLen);
  return text.substring(0, maxLen - 3) + "...";
}

static String mqttLogTimeString() {
  String ts = getCurrentTimeString();
  if (ts.length() == 0) ts = "";
  return ts;
}

static String mqttLogTopicByKind(MqttLogKind kind) {
  switch (kind) {
    case MQTT_LOG_KIND_TIMING: return mqttTopicMetricTiming;
    case MQTT_LOG_KIND_ERROR: return mqttTopicLogError;
    case MQTT_LOG_KIND_SLOW: return mqttTopicLogSlow;
    default: return mqttTopicLog;
  }
}

static String buildMqttLogPayload(const MqttLogEntry& entry) {
  String json;
  json.reserve(384);
  json = "{";
  switch (entry.kind) {
    case MQTT_LOG_KIND_TIMING:
      json += "\"event_type\":\"timing\",";
      json += "\"level\":\"" + entry.level + "\",";
      json += "\"module\":\"" + jsonEscape(entry.module) + "\",";
      json += "\"step\":\"" + jsonEscape(entry.step) + "\",";
      json += "\"elapsed_ms\":" + String(entry.elapsedMs) + ",";
      json += "\"success\":" + String(entry.success ? "true" : "false") + ",";
      if (entry.thresholdMs > 0) json += "\"threshold_ms\":" + String(entry.thresholdMs) + ",";
      break;
    case MQTT_LOG_KIND_ERROR:
      json += "\"event_type\":\"error_log\",";
      json += "\"level\":\"" + entry.level + "\",";
      json += "\"module\":\"" + jsonEscape(entry.module) + "\",";
      json += "\"step\":\"" + jsonEscape(entry.step) + "\",";
      json += "\"message\":\"" + jsonEscape(entry.message) + "\",";
      json += "\"error\":\"" + jsonEscape(entry.error) + "\",";
      json += "\"elapsed_ms\":" + String(entry.elapsedMs) + ",";
      json += "\"success\":" + String(entry.success ? "true" : "false") + ",";
      break;
    case MQTT_LOG_KIND_SLOW:
      json += "\"event_type\":\"slow_operation\",";
      json += "\"level\":\"" + entry.level + "\",";
      json += "\"module\":\"" + jsonEscape(entry.module) + "\",";
      json += "\"step\":\"" + jsonEscape(entry.step) + "\",";
      json += "\"message\":\"" + jsonEscape(entry.message) + "\",";
      json += "\"elapsed_ms\":" + String(entry.elapsedMs) + ",";
      json += "\"threshold_ms\":" + String(entry.thresholdMs) + ",";
      json += "\"success\":" + String(entry.success ? "true" : "false") + ",";
      break;
    case MQTT_LOG_KIND_INFO:
    default:
      json += "\"event_type\":\"device_log\",";
      json += "\"level\":\"" + entry.level + "\",";
      json += "\"module\":\"" + jsonEscape(entry.module) + "\",";
      json += "\"message\":\"" + jsonEscape(entry.message) + "\",";
      break;
  }
  json += "\"time\":\"" + jsonEscape(mqttLogTimeString()) + "\",";
  json += "\"uptime\":" + String(millis() / 1000) + ",";
  json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"device\":\"" + mqttDeviceId + "\"";
  json += "}";
  return json;
}

static void enqueueMqttLogEntry(const MqttLogEntry& entry) {
  MqttLogEntry stored = entry;
  stored.timestampMs = millis();
  if (mqttLogQueueCount >= MQTT_LOG_QUEUE_SIZE) {
    mqttLogQueue[mqttLogQueueTail] = stored;
    mqttLogQueueTail = (mqttLogQueueTail + 1) % MQTT_LOG_QUEUE_SIZE;
    mqttLogQueueHead = mqttLogQueueTail;
  } else {
    mqttLogQueue[mqttLogQueueHead] = stored;
    mqttLogQueueHead = (mqttLogQueueHead + 1) % MQTT_LOG_QUEUE_SIZE;
    mqttLogQueueCount++;
  }
}

static void publishMqttLogEntryToTopic(const String& topic, const String& payload) {
  if (topic.length() == 0) return;
  publishMqttPayload(topic.c_str(), payload.c_str(), false);
}

void publishDeviceLog(const char* level, const char* module, const char* message) {
  if (!config.mqttEnabled) return;
  MqttLogEntry entry;
  entry.kind = MQTT_LOG_KIND_INFO;
  entry.level = level && strlen(level) > 0 ? String(level) : "info";
  entry.module = module ? truncateLogText(String(module), 32) : "system";
  entry.message = message ? truncateLogText(String(message), 180) : "";
  entry.step = "";
  entry.error = "";
  entry.elapsedMs = 0;
  entry.thresholdMs = 0;
  entry.success = true;
  enqueueMqttLogEntry(entry);
}

void publishTimingLog(const char* module, const char* step, unsigned long elapsedMs, bool success) {
  if (!config.mqttEnabled) return;
  MqttLogEntry entry;
  entry.kind = MQTT_LOG_KIND_TIMING;
  entry.level = elapsedMs >= MQTT_SLOW_LOG_THRESHOLD_MS ? "warning" : "info";
  entry.module = module ? truncateLogText(String(module), 32) : "timing";
  entry.message = "";
  entry.step = step ? truncateLogText(String(step), 80) : "";
  entry.error = "";
  entry.elapsedMs = elapsedMs;
  entry.thresholdMs = elapsedMs >= MQTT_SLOW_LOG_THRESHOLD_MS ? MQTT_SLOW_LOG_THRESHOLD_MS : 0;
  entry.success = success;
  enqueueMqttLogEntry(entry);

  if (elapsedMs >= MQTT_SLOW_LOG_THRESHOLD_MS) {
    MqttLogEntry slow = entry;
    slow.kind = MQTT_LOG_KIND_SLOW;
    slow.level = "warning";
    slow.message = "操作耗时超过阈值";
    slow.thresholdMs = MQTT_SLOW_LOG_THRESHOLD_MS;
    enqueueMqttLogEntry(slow);
  }
}

void publishErrorLog(const char* module, const char* step, const char* message, const char* error, unsigned long elapsedMs) {
  if (!config.mqttEnabled) return;
  MqttLogEntry entry;
  entry.kind = MQTT_LOG_KIND_ERROR;
  entry.level = "error";
  entry.module = module ? truncateLogText(String(module), 32) : "system";
  entry.message = message ? truncateLogText(String(message), 160) : "";
  entry.step = step ? truncateLogText(String(step), 80) : "";
  entry.error = error ? truncateLogText(String(error), 80) : "";
  entry.elapsedMs = elapsedMs;
  entry.thresholdMs = 0;
  entry.success = false;
  enqueueMqttLogEntry(entry);
}

void processMqttLogQueue() {
  if (mqttLogQueueBusy) return;
  if (!config.mqttEnabled || !mqttClient.connected()) return;
  if (mqttLogQueueCount == 0) return;

  mqttLogQueueBusy = true;
  uint8_t processed = 0;
  while (mqttLogQueueCount > 0 && processed < MQTT_LOG_PROCESS_BURST) {
    MqttLogEntry entry = mqttLogQueue[mqttLogQueueTail];
    mqttLogQueueTail = (mqttLogQueueTail + 1) % MQTT_LOG_QUEUE_SIZE;
    mqttLogQueueCount--;

    String payload = buildMqttLogPayload(entry);
    publishMqttLogEntryToTopic(mqttLogTopicByKind(entry.kind), payload);
    if (config.mqttHaDiscovery && mqttHaLogEventTopic.length() > 0) {
      publishMqttLogEntryToTopic(mqttHaLogEventTopic, payload);
    }
    processed++;
    yield();
  }
  mqttLogQueueBusy = false;
}

String getMacSuffix() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  mac.toLowerCase();
  return mac.substring(6);
}

static String mqttResetStatusFields() {
  String json = "";
  json += "\"last_reset_reason\":\"" + jsonEscape(lastResetReasonText) + "\",";
  json += "\"last_reset_reason_code\":" + String((int)lastResetReasonCode) + ",";
  json += "\"power_suspected\":" + String(lastResetPowerSuspected ? "true" : "false") + ",";
  json += "\"brownout_resets\":" + String((unsigned long)stats.brownoutResets.load()) + ",";
  json += "\"poweron_resets\":" + String((unsigned long)stats.powerOnResets.load()) + ",";
  json += "\"watchdog_resets\":" + String((unsigned long)stats.watchdogResets.load()) + ",";
  json += "\"software_resets\":" + String((unsigned long)stats.softwareResets.load()) + ",";
  json += "\"panic_resets\":" + String((unsigned long)stats.panicResets.load()) + ",";
  json += "\"has_interrupted_operation\":" + String(lastInterruptedOperation.valid ? "true" : "false") + ",";
  json += "\"interrupted_operation\":\"" + jsonEscape(lastInterruptedOperation.operation) + "\",";
  json += "\"interrupted_stage\":\"" + jsonEscape(lastInterruptedOperation.stage) + "\",";
  json += "\"interrupted_uptime\":" + String(lastInterruptedOperation.uptimeSeconds) + ",";
  json += "\"interrupted_free_heap\":" + String(lastInterruptedOperation.freeHeap) + ",";
  json += "\"interrupted_min_free_heap\":" + String(lastInterruptedOperation.minFreeHeap) + ",";
  json += "\"interrupted_max_alloc_heap\":" + String(lastInterruptedOperation.maxAllocHeap);
  return json;
}

String buildMqttStatusJson(const char* status = "online") {
  int wifiRssi = WiFi.RSSI();
  String wifiStatus = "未知";
  if (wifiRssi >= -50) wifiStatus = "极好";
  else if (wifiRssi >= -60) wifiStatus = "很好";
  else if (wifiRssi >= -70) wifiStatus = "良好";
  else if (wifiRssi >= -80) wifiStatus = "一般";
  else if (wifiRssi >= -90) wifiStatus = "较弱";
  else if (wifiRssi != 0) wifiStatus = "很差";

  String json = "{";
  json += "\"status\":\"" + jsonEscape(String(status)) + "\",";
  json += "\"device\":\"" + mqttDeviceId + "\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"wifi_rssi\":" + String(wifiRssi) + ",";
  json += "\"wifi_status\":\"" + jsonEscape(wifiStatus) + "\",";
  json += "\"uptime\":" + String(millis() / 1000) + ",";
  json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  json += mqttResetStatusFields();
  json += "}";
  return json;
}

void initMqttTopics() {
  mqttDeviceId = getMacSuffix();
  String prefix = config.mqttPrefix + "/" + mqttDeviceId;
  
  mqttTopicStatus = prefix + "/status";
  mqttTopicSmsReceived = prefix + "/sms/received";
  mqttTopicCallReceived = prefix + "/call/received";
  mqttTopicSmsSent = prefix + "/sms/sent";
  mqttTopicPingResult = prefix + "/ping/result";
  mqttTopicLog = prefix + "/log";
  mqttTopicLogSlow = prefix + "/log/slow";
  mqttTopicLogError = prefix + "/log/error";
  mqttTopicMetricTiming = prefix + "/metric/timing";

  mqttTopicSmsSend = prefix + "/sms/send";
  mqttTopicPing = prefix + "/ping";
  mqttTopicCmd = prefix + "/cmd";
  
  String haPrefix = config.mqttHaPrefix;
  if (haPrefix.length() == 0) haPrefix = "homeassistant";
  mqttHaStatusTopic = haPrefix + "/sensor/sms_forwarder_" + mqttDeviceId + "/state";
  mqttHaSmsReceivedTopic = haPrefix + "/event/sms_forwarder_" + mqttDeviceId + "_sms/event";
  mqttHaCallReceivedTopic = haPrefix + "/event/sms_forwarder_" + mqttDeviceId + "_call/event";
  mqttHaLogEventTopic = haPrefix + "/event/sms_forwarder_" + mqttDeviceId + "_log/event";

  Serial.println("MQTT设备ID: " + mqttDeviceId);
  Serial.println("用户主题前缀: " + prefix);
  if (config.mqttHaDiscovery) {
    Serial.println("HA自动发现: 已启用 (前缀: " + haPrefix + ")");
  }
}

static void publishHaStatusSensor(const String& haPrefix, const String& nodeId, const String& suffix, const String& name, const String& valueTemplate, const String& icon, const String& deviceInfo, const String& unit = "", const String& stateClass = "") {
  String topic = haPrefix + "/sensor/" + nodeId + suffix + "/config";
  String payload = "{";
  payload += "\"name\":\"" + jsonEscape(name) + "\",";
  payload += "\"unique_id\":\"" + jsonEscape(nodeId + suffix) + "\",";
  payload += "\"state_topic\":\"" + jsonEscape(mqttHaStatusTopic) + "\",";
  payload += "\"value_template\":\"" + jsonEscape(valueTemplate) + "\",";
  if (unit.length() > 0) payload += "\"unit_of_measurement\":\"" + jsonEscape(unit) + "\",";
  if (stateClass.length() > 0) payload += "\"state_class\":\"" + jsonEscape(stateClass) + "\",";
  payload += "\"icon\":\"" + jsonEscape(icon) + "\",";
  payload += deviceInfo;
  payload += "}";
  publishMqttPayload(topic.c_str(), payload.c_str(), true);
}

void publishHaDiscoveryConfig() {
  if (!config.mqttHaDiscovery || !mqttClient.connected()) return;
  
  String haPrefix = config.mqttHaPrefix;
  if (haPrefix.length() == 0) haPrefix = "homeassistant";
  String nodeId = "sms_forwarder_" + mqttDeviceId;
  
  String deviceInfo = "\"device\":{";
  deviceInfo += "\"identifiers\":[\"" + jsonEscape(nodeId) + "\"],";
  deviceInfo += "\"name\":\"短信转发器 " + jsonEscape(mqttDeviceId) + "\",";
  deviceInfo += "\"manufacturer\":\"DIY\",";
  deviceInfo += "\"model\":\"ESP32-C3 SMS Forwarder\",";
  deviceInfo += "\"sw_version\":\"1.0\"";
  deviceInfo += "}";

  publishHaStatusSensor(haPrefix, nodeId, "_status", "状态", "{{ value_json.status }}", "mdi:message-text", deviceInfo);
  publishHaStatusSensor(haPrefix, nodeId, "_reset_reason", "上次复位原因", "{{ value_json.last_reset_reason | default('未知') }}", "mdi:restart-alert", deviceInfo);
  publishHaStatusSensor(haPrefix, nodeId, "_reset_reason_code", "上次复位原因代码", "{{ value_json.last_reset_reason_code | default(0) }}", "mdi:numeric", deviceInfo);

  String powerSuspectConfigTopic = haPrefix + "/binary_sensor/" + nodeId + "_power_suspect/config";
  String powerSuspectConfig = "{";
  powerSuspectConfig += "\"name\":\"疑似供电问题\",";
  powerSuspectConfig += "\"unique_id\":\"" + jsonEscape(nodeId) + "_power_suspect\",";
  powerSuspectConfig += "\"state_topic\":\"" + jsonEscape(mqttHaStatusTopic) + "\",";
  powerSuspectConfig += "\"value_template\":\"{{ value_json.power_suspected | default(false) }}\",";
  powerSuspectConfig += "\"payload_on\":\"true\",";
  powerSuspectConfig += "\"payload_off\":\"false\",";
  powerSuspectConfig += "\"device_class\":\"problem\",";
  powerSuspectConfig += deviceInfo;
  powerSuspectConfig += "}";
  publishMqttPayload(powerSuspectConfigTopic.c_str(), powerSuspectConfig.c_str(), true);

  publishHaStatusSensor(haPrefix, nodeId, "_brownout_resets", "欠压复位次数", "{{ value_json.brownout_resets | default(0) }}", "mdi:flash-alert", deviceInfo, "次", "measurement");
  publishHaStatusSensor(haPrefix, nodeId, "_poweron_resets", "上电复位次数", "{{ value_json.poweron_resets | default(0) }}", "mdi:power-plug", deviceInfo, "次", "measurement");
  publishHaStatusSensor(haPrefix, nodeId, "_watchdog_resets", "看门狗复位次数", "{{ value_json.watchdog_resets | default(0) }}", "mdi:dog-side", deviceInfo, "次", "measurement");
  publishHaStatusSensor(haPrefix, nodeId, "_software_resets", "软件复位次数", "{{ value_json.software_resets | default(0) }}", "mdi:restart", deviceInfo, "次", "measurement");
  publishHaStatusSensor(haPrefix, nodeId, "_panic_resets", "异常复位次数", "{{ value_json.panic_resets | default(0) }}", "mdi:alert-octagon", deviceInfo, "次", "measurement");
  publishHaStatusSensor(haPrefix, nodeId, "_wifi", "WiFi信号", "{{ value_json.wifi_rssi }}", "mdi:wifi", deviceInfo, "dBm");
  publishHaStatusSensor(haPrefix, nodeId, "_lte", "4G信号", "{{ value_json.lte_rsrp }}", "mdi:signal-4g", deviceInfo, "dBm");
  publishHaStatusSensor(haPrefix, nodeId, "_ip", "IP地址", "{{ value_json.ip }}", "mdi:ip-network", deviceInfo);
  publishHaStatusSensor(haPrefix, nodeId, "_uptime", "运行时间", "{{ (value_json.uptime | int / 3600) | round(1) }}", "mdi:clock-outline", deviceInfo, "小时");

  String onlineConfigTopic = haPrefix + "/binary_sensor/" + nodeId + "_online/config";
  String onlineConfig = "{";
  onlineConfig += "\"name\":\"在线\",";
  onlineConfig += "\"unique_id\":\"" + jsonEscape(nodeId) + "_online\",";
  // 与 LWT 同源；不加 availability，保留 offline_alert 所需的 off（而非 unavailable）。
  // 每次重连仍向同一 retained config 主题发布，覆盖已上线设备的旧 discovery。
  onlineConfig += "\"state_topic\":\"" + jsonEscape(mqttTopicStatus) + "\",";
  onlineConfig += "\"value_template\":\"{{ value_json.status }}\",";
  onlineConfig += "\"payload_on\":\"online\",";
  onlineConfig += "\"payload_off\":\"offline\",";
  onlineConfig += "\"device_class\":\"connectivity\",";
  onlineConfig += deviceInfo;
  onlineConfig += "}";
  publishMqttPayload(onlineConfigTopic.c_str(), onlineConfig.c_str(), true);

  String restartConfigTopic = haPrefix + "/button/" + nodeId + "_restart/config";
  String restartConfig = "{";
  restartConfig += "\"name\":\"重启\",";
  restartConfig += "\"unique_id\":\"" + jsonEscape(nodeId) + "_restart\",";
  restartConfig += "\"command_topic\":\"" + jsonEscape(mqttTopicCmd) + "\",";
  restartConfig += "\"payload_press\":\"{\\\"action\\\":\\\"restart\\\"}\",";
  restartConfig += "\"icon\":\"mdi:restart\",";
  restartConfig += deviceInfo;
  restartConfig += "}";
  publishMqttPayload(restartConfigTopic.c_str(), restartConfig.c_str(), true);

  String senderConfigTopic = haPrefix + "/sensor/" + nodeId + "_last_sender/config";
  String senderConfig = "{";
  senderConfig += "\"name\":\"最近短信发送者\",";
  senderConfig += "\"unique_id\":\"" + jsonEscape(nodeId) + "_last_sender\",";
  senderConfig += "\"state_topic\":\"" + jsonEscape(mqttTopicSmsReceived) + "\",";
  senderConfig += "\"value_template\":\"{{ value_json.sender }}\",";
  senderConfig += "\"icon\":\"mdi:account\",";
  senderConfig += deviceInfo;
  senderConfig += "}";
  publishMqttPayload(senderConfigTopic.c_str(), senderConfig.c_str(), true);

  String messageConfigTopic = haPrefix + "/sensor/" + nodeId + "_last_message/config";
  String messageConfig = "{";
  messageConfig += "\"name\":\"最近短信内容\",";
  messageConfig += "\"unique_id\":\"" + jsonEscape(nodeId) + "_last_message\",";
  messageConfig += "\"state_topic\":\"" + jsonEscape(mqttTopicSmsReceived) + "\",";
  messageConfig += "\"value_template\":\"{{ value_json.message[:50] }}{% if value_json.message | length > 50 %}...{% endif %}\",";
  messageConfig += "\"json_attributes_topic\":\"" + jsonEscape(mqttTopicSmsReceived) + "\",";
  messageConfig += "\"icon\":\"mdi:message-text\",";
  messageConfig += deviceInfo;
  messageConfig += "}";
  publishMqttPayload(messageConfigTopic.c_str(), messageConfig.c_str(), true);

  String smsEventConfigTopic = haPrefix + "/event/" + nodeId + "_sms/config";
  String smsEventConfig = "{";
  smsEventConfig += "\"name\":\"短信通知\",";
  smsEventConfig += "\"unique_id\":\"" + jsonEscape(nodeId) + "_sms_event\",";
  smsEventConfig += "\"state_topic\":\"" + jsonEscape(mqttHaSmsReceivedTopic) + "\",";
  smsEventConfig += "\"event_types\":[\"sms_received\"],";
  smsEventConfig += "\"icon\":\"mdi:message-badge\",";
  smsEventConfig += deviceInfo;
  smsEventConfig += "}";
  publishMqttPayload(smsEventConfigTopic.c_str(), smsEventConfig.c_str(), true);

  String lastCallerConfigTopic = haPrefix + "/sensor/" + nodeId + "_last_caller/config";
  String lastCallerConfig = "{";
  lastCallerConfig += "\"name\":\"最近来电号码\",";
  lastCallerConfig += "\"unique_id\":\"" + jsonEscape(nodeId) + "_last_caller\",";
  lastCallerConfig += "\"state_topic\":\"" + jsonEscape(mqttTopicCallReceived) + "\",";
  lastCallerConfig += "\"value_template\":\"{{ value_json.caller }}\",";
  lastCallerConfig += "\"json_attributes_topic\":\"" + jsonEscape(mqttTopicCallReceived) + "\",";
  lastCallerConfig += "\"icon\":\"mdi:phone-in-talk\",";
  lastCallerConfig += deviceInfo;
  lastCallerConfig += "}";
  publishMqttPayload(lastCallerConfigTopic.c_str(), lastCallerConfig.c_str(), true);

  String callEventConfigTopic = haPrefix + "/event/" + nodeId + "_call/config";
  String callEventConfig = "{";
  callEventConfig += "\"name\":\"来电通知\",";
  callEventConfig += "\"unique_id\":\"" + jsonEscape(nodeId) + "_call_event\",";
  callEventConfig += "\"state_topic\":\"" + jsonEscape(mqttHaCallReceivedTopic) + "\",";
  callEventConfig += "\"event_types\":[\"incoming_call\"],";
  callEventConfig += "\"device_class\":\"button\",";
  callEventConfig += "\"icon\":\"mdi:phone-ring\",";
  callEventConfig += deviceInfo;
  callEventConfig += "}";
  publishMqttPayload(callEventConfigTopic.c_str(), callEventConfig.c_str(), true);

  publishHaStatusSensor(haPrefix, nodeId, "_last_log", "最新日志", "{{ value_json.message[:80] }}{% if value_json.message | length > 80 %}...{% endif %}", "mdi:text-box-search", deviceInfo);
  publishHaStatusSensor(haPrefix, nodeId, "_last_slow_log", "最近慢操作", "{{ value_json.step | default('无') }}", "mdi:alert-clock", deviceInfo);
  publishHaStatusSensor(haPrefix, nodeId, "_last_error_log", "最近错误日志", "{{ value_json.message[:80] }}{% if value_json.message | length > 80 %}...{% endif %}", "mdi:alert-circle-outline", deviceInfo);
  publishHaStatusSensor(haPrefix, nodeId, "_last_timing", "最新耗时", "{{ value_json.elapsed_ms | default(0) }}", "mdi:speedometer", deviceInfo, "ms", "measurement");

  String logEventConfigTopic = haPrefix + "/event/" + nodeId + "_log/config";
  String logEventConfig = "{";
  logEventConfig += "\"name\":\"设备日志事件\",";
  logEventConfig += "\"unique_id\":\"" + jsonEscape(nodeId) + "_log_event\",";
  logEventConfig += "\"state_topic\":\"" + jsonEscape(mqttHaLogEventTopic) + "\",";
  logEventConfig += "\"event_types\":[\"device_log\",\"timing\",\"slow_operation\",\"error_log\"],";
  logEventConfig += "\"icon\":\"mdi:text-box-search\",";
  logEventConfig += deviceInfo;
  logEventConfig += "}";
  publishMqttPayload(logEventConfigTopic.c_str(), logEventConfig.c_str(), true);

  Serial.println("HA自动发现配置已发布");
  publishDeviceLog("info", "mqtt", "HA自动发现配置已发布");
}

void mqttReconnect() {
  if (!config.mqttEnabled) return;
  if (config.mqttServer.length() == 0) return;
  if (mqttClient.connected()) return;
  unsigned long reconnectStart = millis();
  Serial.println("[耗时] START MQTT重连");

  mqttClient.setServer(config.mqttServer.c_str(), config.mqttPort);
  // 同步库只能限制单次收包等待；慢/分片网络仍需硬件压测，不能无限饿死 UART。
  mqttClient.setSocketTimeout(1);

  String clientId = "sms_" + mqttDeviceId;
  Serial.println("连接MQTT服务器: " + config.mqttServer);
  Serial.println("客户端ID: " + clientId);
  
  bool connected = false;
  const MqttJsonField willFields[] = {{"status", "offline"}, {"device", mqttDeviceId.c_str()}};
  String willMessage;
  if (clientId.length() != mqttDeviceId.length() + 4U ||
      !buildMqttJson(willFields, sizeof(willFields) / sizeof(willFields[0]), willMessage)) {
    Serial.println("[MQTT] 重连拒绝: client_id_or_will_build_failed");
    return;
  }
  if (mqttTopicStatus.length() == 0 || mqttTopicStatus.length() > MQTT_MAX_TOPIC_BYTES) {
    Serial.println("[MQTT] 重连拒绝: will_topic_length_max1024");
    return;
  }
  // CONNECT 不能流式发送：预留协议名/版本/flags/keepalive（兼容 3.1 的 12 字节）。
  size_t connectBytes = MQTT_MAX_HEADER_SIZE + 12U;
  const auto addConnectString = [&connectBytes](size_t length) {
    if (connectBytes > MQTT_MAX_BUFFER_BYTES - 2U || length > MQTT_MAX_BUFFER_BYTES - connectBytes - 2U) return false;
    connectBytes += 2U + length;
    return true;
  };
  if (!addConnectString(clientId.length()) || !addConnectString(mqttTopicStatus.length()) ||
      !addConnectString(willMessage.length()) ||
      (config.mqttUser.length() > 0 && (!addConnectString(config.mqttUser.length()) || !addConnectString(config.mqttPass.length())))) {
    Serial.println("[MQTT] 重连拒绝: connect_packet_exceeds_buffer_budget");
    return;
  }
  if (!ensureMqttBufferSize(MQTT_RECEIVE_BUFFER_BYTES)) return;
  
  if (config.mqttUser.length() > 0) {
    connected = mqttClient.connect(clientId.c_str(), config.mqttUser.c_str(), config.mqttPass.c_str(), mqttTopicStatus.c_str(), 1, true, willMessage.c_str());
  } else {
    connected = mqttClient.connect(clientId.c_str(), mqttTopicStatus.c_str(), 1, true, willMessage.c_str());
  }
  
  if (connected) {
    Serial.println("MQTT连接成功");
    publishDeviceLog("info", "mqtt", "MQTT连接成功");

    mqttClient.subscribe(mqttTopicSmsSend.c_str());
    mqttClient.subscribe(mqttTopicPing.c_str());
    mqttClient.subscribe(mqttTopicCmd.c_str());
    Serial.println("已订阅主题:");
    Serial.println("  - " + mqttTopicSmsSend);
    Serial.println("  - " + mqttTopicPing);
    Serial.println("  - " + mqttTopicCmd);
    
    publishHaDiscoveryConfig();
    publishMqttStatus("online");
  } else {
    Serial.print("MQTT连接失败, 错误码: ");
    Serial.println(mqttClient.state());
    publishErrorLog("mqtt", "mqttReconnect", "MQTT连接失败", String(mqttClient.state()).c_str());
  }
  logMqttElapsed("MQTT重连", reconnectStart, connected && mqttClient.connected());
}

// P1: MQTT 命令异步执行 —— callback 仅入队/设标志，loop 中执行阻塞操作
struct MqttPendingSms {
  String phone;
  String message;
};
static const uint8_t MAX_PENDING_SMS = 4;
static MqttPendingSms pendingSmsQueue[MAX_PENDING_SMS];
static uint8_t pendingSmsCount = 0;

static bool mqttPendingPing = false;
static String mqttPendingPingHost;
static bool mqttPendingRestart = false;
static bool mqttCommandsBusy = false;

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  const bool smsCommand = topic && strcmp(topic, mqttTopicSmsSend.c_str()) == 0;
  const bool pingCommand = topic && strcmp(topic, mqttTopicPing.c_str()) == 0;
  const bool controlCommand = topic && strcmp(topic, mqttTopicCmd.c_str()) == 0;
  if (!smsCommand && !pingCommand && !controlCommand) return;

  const auto rejectCommand = [smsCommand, pingCommand](const char* reason) {
    Serial.print("MQTT命令拒绝: "); Serial.println(reason);
    publishErrorLog("mqtt", "mqttCallback", "MQTT命令拒绝", reason);
    if (smsCommand) publishMqttSmsSent("", "", false);
    if (pingCommand) publishMqttPingResult("", false, reason);
  };
  // payload 不保证以 NUL 结尾；先限长再一次性构建，禁止截断后当作合法命令。
  if (!payload || length < 2 || length > JsonInput::MAX_INPUT_BYTES) {
    rejectCommand("invalid_payload_length_max8192");
    return;
  }
  String message;
  if (!message.reserve(length) || !message.concat(reinterpret_cast<const char*>(payload), length) ||
      message.length() != length) {
    rejectCommand("payload_allocation_failed");
    return;
  }
  JsonInput input;
  if (!input.parse(message)) {
    rejectCommand("invalid_json");
    return;
  }

  Serial.println("=== MQTT消息接收 ===");
  Serial.print("主题: "); Serial.println(topic);
  Serial.print("内容: "); Serial.println(message);
  Serial.println("====================");

  if (smsCommand) {
    String phone, smsMessage;
    if (!input.getString("phone", phone) || !input.getString("message", smsMessage)) {
      rejectCommand("phone_message_must_be_strings");
      return;
    }
    phone.trim();
    if (phone.length() == 0 || smsMessage.length() == 0) {
      rejectCommand("empty_phone_or_message");
      return;
    }

    Serial.println("MQTT发送短信命令(异步):");
    Serial.print("  目标: "); Serial.println(phone);
    Serial.print("  内容: "); Serial.println(smsMessage);

    // P1: 校验完再入队，不在 callback 中执行阻塞 AT 命令；正文不 trim。
    if (pendingSmsCount < MAX_PENDING_SMS) {
      pendingSmsQueue[pendingSmsCount].phone = std::move(phone);
      pendingSmsQueue[pendingSmsCount].message = std::move(smsMessage);
      pendingSmsCount++;
    } else {
      Serial.println("MQTT待发送短信队列已满，丢弃命令");
      publishErrorLog("mqtt", "mqttCallback", "MQTT待发送短信队列已满", "sms_queue_full");
      publishMqttSmsSent(phone.c_str(), smsMessage.c_str(), false);
    }
  } else if (pingCommand) {
    String host = "8.8.8.8";
    if (input.has("host") && !input.getString("host", host)) {
      rejectCommand("host_must_be_string");
      return;
    }
    // DNS/IPv4/IPv6 文本白名单；不允许引号、反斜线、控制字符或 AT 分隔符。
    // 给 pending 路径的 AT+MPING 包装预留空间，满足 modem 的 256 字节限制。
    bool validHost = host.length() > 0 && host.length() <= 256 - (sizeof("AT+MPING=\"\",30,1") - 1);
    for (unsigned int i = 0; validHost && i < host.length(); ++i) {
      char c = host.charAt(i);
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':')) {
        validHost = false;
      }
    }
    if (!validHost) {
      rejectCommand("invalid_ping_host");
      return;
    }

    Serial.print("MQTT Ping命令(异步): "); Serial.println(host);
    // P1: 仅保存参数，不在 callback 中阻塞 50 秒
    if (!mqttPendingPing) {
      mqttPendingPingHost = std::move(host);
      mqttPendingPing = true;
    } else {
      Serial.print("已有待执行Ping，忽略新请求: "); Serial.println(host);
    }
  } else if (controlCommand) {
    String action;
    if (!input.getString("action", action)) {
      rejectCommand("action_must_be_string");
      return;
    }
    Serial.print("MQTT控制命令: "); Serial.println(action);

    if (action == "restart" || action == "reset") {
      // loop 可能由 AT 等待期调用；不能中断正在发送的 PDU/持久化或重启设备。
      mqttPendingRestart = true;
    } else if (action == "status") {
      publishMqttStatus("online");
    } else {
      Serial.print("未知命令: "); Serial.println(action);
      publishErrorLog("mqtt", "mqttCallback", "未知控制命令", "unknown_action");
    }
  }
}

// P1: 在 loop 中调用，执行 MQTT callback 中暂存的阻塞命令
void processMqttPendingCommands() {
  if (mqttCommandsBusy || modemBusy()) return;
  struct CommandGuard {
    CommandGuard() { mqttCommandsBusy = true; }
    ~CommandGuard() { mqttCommandsBusy = false; }
  } executing;
  if (mqttPendingRestart) {
    mqttPendingRestart = false;
    publishMqttStatus("restarting");
    delay(500);
    ESP.restart();
    return;
  }
  // 处理待执行的短信发送（单次循环消费一条，避免连续阻塞）
  if (pendingSmsCount > 0) {
    String phone = std::move(pendingSmsQueue[0].phone);
    String msg = std::move(pendingSmsQueue[0].message);
    for (uint8_t i = 1; i < pendingSmsCount; i++) {
      pendingSmsQueue[i - 1] = std::move(pendingSmsQueue[i]);
    }
    pendingSmsCount--;
    pendingSmsQueue[pendingSmsCount] = MqttPendingSms{};

    bool success = sendSMS(phone.c_str(), msg.c_str());
    if (success) {
      stats.smsSent++;
      saveStats();
    }
    publishMqttSmsSent(phone.c_str(), msg.c_str(), success);
    return;  // 同一轮不再连续执行 Ping；保活回调新入队的工作留给后续主循环。
  }

  // 处理待执行的 Ping
  if (mqttPendingPing) {
    mqttPendingPing = false;
    String host = std::move(mqttPendingPingHost);
    mqttPendingPingHost = "";

    Serial.println("执行MQTT Ping: " + host);
    sendATCommand("AT+CGACT=1,1", 10000);
    modemDelay(500);
    
    String pingCmd = "AT+MPING=\"" + host + "\",30,1";
    String resultLine;
    // 统一事务收齐 OK 和 +MPING:；等待期保活回调只排队，不嵌套执行 AT/Web。
    ModemResult pingResult = runModemCommand(pingCmd.c_str(), 35000, nullptr, &resultLine);
    bool gotResult = false;
    String resultMsg = "";
    bool pingSuccess = false;
    
    if (pingResult == ModemResult::Ok) {
      int colonIdx = resultLine.indexOf(':');
      if (colonIdx >= 0) {
        String params = resultLine.substring(colonIdx + 1);
        params.trim();
        
        int commaIdx = params.indexOf(',');
        int result = params.substring(0, commaIdx > 0 ? commaIdx : params.length()).toInt();
        
        gotResult = true;
        pingSuccess = (result == 0 || result == 1) || (params.indexOf(',') >= 0 && params.length() > 5);
        resultMsg = pingSuccess && commaIdx > 0 ? params : "错误码: " + String(result);
      }
    } else if (pingResult != ModemResult::Timeout) {
      gotResult = true;
      resultMsg = "模组错误";
    }
    
    sendATCommand("AT+CGACT=0,1", 5000);
    if (!gotResult) resultMsg = "超时";
    publishMqttPingResult(host.c_str(), pingSuccess, resultMsg.c_str());
  }
}

void publishMqttSmsReceived(const char* sender, const char* message, const char* timestamp) {
  if (!config.mqttEnabled) return;
  if (!mqttClient.connected()) {
    Serial.println("MQTT未连接，跳过短信推送");
    return;
  }
  if (config.mqttControlOnly) {
    Serial.println("MQTT仅控制模式，跳过短信推送");
    return;
  }
  
  Serial.println("MQTT推送短信...");
  publishDeviceLog("info", "mqtt", "MQTT推送短信");

  const MqttJsonField fields[] = {
    {"event_type", "sms_received"},
    {"sender", sender},
    {"message", message},
    {"time", timestamp},
    {"timestamp", timestamp},
    {"device", mqttDeviceId.c_str()}
  };
  String json;
  if (!buildMqttJson(fields, sizeof(fields) / sizeof(fields[0]), json)) {
    publishErrorLog("mqtt", "publishMqttSmsReceived", "MQTT短信JSON构造失败，未发布", "json_build_failed");
    return;
  }
  
  Serial.println(" 主题1: " + mqttTopicSmsReceived);
  unsigned long publishStart = millis();
  Serial.println("[耗时] START MQTT发布短信主题1");
  bool success1 = publishMqttPayload(mqttTopicSmsReceived.c_str(), json.c_str());
  Serial.printf("[耗时] END MQTT发布短信主题1: elapsed=%lums, success=%s\n", millis() - publishStart, success1 ? "true" : "false");
  if (!success1) publishErrorLog("mqtt", "publishMqttSmsReceived", "MQTT短信主题1推送失败", "user_topic_publish_failed");

  bool successHa = true; // HA 未启用时不影响总结果。
  if (config.mqttHaDiscovery) {
    Serial.println(" 主题2 (HA): " + mqttHaSmsReceivedTopic);
    publishStart = millis();
    Serial.println("[耗时] START MQTT发布短信HA主题");
    successHa = publishMqttPayload(mqttHaSmsReceivedTopic.c_str(), json.c_str());
    Serial.printf("[耗时] END MQTT发布短信HA主题: elapsed=%lums, success=%s\n", millis() - publishStart, successHa ? "true" : "false");
    if (!successHa) publishErrorLog("mqtt", "publishMqttSmsReceived", "MQTT短信HA推送失败", "ha_topic_publish_failed");
  }
  
  if (success1 && successHa) {
    Serial.println("MQTT短信推送完成");
    publishDeviceLog("info", "mqtt", "MQTT短信推送完成");
  } else {
    Serial.println("MQTT短信推送失败");
  }
}

void publishMqttCallReceived(const char* caller, const char* timestamp) {
  if (!config.mqttEnabled) return;
  if (!mqttClient.connected()) {
    Serial.println("MQTT未连接，跳过来电推送");
    return;
  }
  if (config.mqttControlOnly) {
    Serial.println("MQTT仅控制模式，跳过来电推送");
    return;
  }

  Serial.println("MQTT推送来电通知...");
  publishDeviceLog("info", "mqtt", "MQTT推送来电通知");

  String safeCaller = jsonEscape(String(caller));
  String safeTime = jsonEscape(String(timestamp));

  String json;
  json.reserve(safeCaller.length() + safeTime.length() + mqttDeviceId.length() + 96);
  json = "{";
  json += "\"event_type\":\"incoming_call\",";
  json += "\"caller\":\"" + safeCaller + "\",";
  json += "\"timestamp\":\"" + safeTime + "\",";
  json += "\"device\":\"" + mqttDeviceId + "\"";
  json += "}";

  Serial.println(" 主题1: " + mqttTopicCallReceived);
  unsigned long publishStart = millis();
  Serial.println("[耗时] START MQTT发布来电主题1");
  bool success1 = publishMqttPayload(mqttTopicCallReceived.c_str(), json.c_str());
  Serial.printf("[耗时] END MQTT发布来电主题1: elapsed=%lums, success=%s\n", millis() - publishStart, success1 ? "true" : "false");
  if (!success1) publishErrorLog("mqtt", "publishMqttCallReceived", "MQTT来电主题1推送失败", "user_topic_publish_failed");

  bool successHa = true;
  if (config.mqttHaDiscovery) {
    Serial.println(" 主题2 (HA): " + mqttHaCallReceivedTopic);
    publishStart = millis();
    Serial.println("[耗时] START MQTT发布来电HA主题");
    successHa = publishMqttPayload(mqttHaCallReceivedTopic.c_str(), json.c_str());
    Serial.printf("[耗时] END MQTT发布来电HA主题: elapsed=%lums, success=%s\n", millis() - publishStart, successHa ? "true" : "false");
    if (!successHa) publishErrorLog("mqtt", "publishMqttCallReceived", "MQTT来电HA推送失败", "ha_topic_publish_failed");
  }

  if (success1 && successHa) {
    Serial.println("MQTT来电推送完成");
    publishDeviceLog("info", "mqtt", "MQTT来电推送完成");
  } else {
    Serial.println("MQTT来电推送失败");
  }
}

void publishMqttSmsSent(const char* phone, const char* message, bool success) {
  if (!config.mqttEnabled || !mqttClient.connected()) return;
  
  const MqttJsonField fields[] = {
    {"success", success ? "true" : "false", false},
    {"phone", phone},
    {"message", message},
    {"device", mqttDeviceId.c_str()}
  };
  String json;
  if (!buildMqttJson(fields, sizeof(fields) / sizeof(fields[0]), json)) {
    publishErrorLog("mqtt", "publishMqttSmsSent", "MQTT发送结果JSON构造失败，未发布", "json_build_failed");
    return;
  }
  
  unsigned long publishStart = millis();
  Serial.println("[耗时] START MQTT发布发送短信结果");
  bool successPublish = publishMqttPayload(mqttTopicSmsSent.c_str(), json.c_str());
  Serial.printf("[耗时] END MQTT发布发送短信结果: elapsed=%lums, success=%s\n", millis() - publishStart, successPublish ? "true" : "false");
  Serial.println("MQTT发布发送短信结果: " + String(success ? "成功" : "失败"));
  if (successPublish) publishDeviceLog("info", "mqtt", "MQTT发布发送短信结果完成");
  else publishErrorLog("mqtt", "publishMqttSmsSent", "MQTT发布发送短信结果失败", "publish_failed");
}

void publishMqttPingResult(const char* host, bool success, const char* result) {
  if (!config.mqttEnabled || !mqttClient.connected()) return;
  
  String json = "{";
  json += "\"success\":" + String(success ? "true" : "false") + ",";
  json += "\"host\":\"" + jsonEscape(String(host)) + "\",";
  json += "\"result\":\"" + jsonEscape(String(result)) + "\",";
  json += "\"device\":\"" + mqttDeviceId + "\"";
  json += "}";
  
  unsigned long publishStart = millis();
  Serial.println("[耗时] START MQTT发布Ping结果");
  bool successPublish = publishMqttPayload(mqttTopicPingResult.c_str(), json.c_str());
  Serial.printf("[耗时] END MQTT发布Ping结果: elapsed=%lums, success=%s\n", millis() - publishStart, successPublish ? "true" : "false");
  Serial.println("MQTT发布Ping结果: " + String(success ? "成功" : "失败"));
  if (successPublish) publishDeviceLog("info", "mqtt", "MQTT发布Ping结果完成");
  else publishErrorLog("mqtt", "publishMqttPingResult", "MQTT发布Ping结果失败", "publish_failed");
}

void publishMqttStatus(const char* status) {
  if (!config.mqttEnabled) return;
  if (!mqttClient.connected() && String(status) != "online") return;
  unsigned long statusStart = millis();
  Serial.printf("[耗时] START MQTT发布状态: %s\n", status);

  String json = buildMqttStatusJson(status);

  unsigned long publishStart = millis();
  Serial.println("[耗时] START MQTT发布状态主题1");
  bool success1 = publishMqttPayload(mqttTopicStatus.c_str(), json.c_str(), true);
  Serial.printf("[耗时] END MQTT发布状态主题1: elapsed=%lums, success=%s\n", millis() - publishStart, success1 ? "true" : "false");
  if (!success1) publishErrorLog("mqtt", "publishMqttStatus", "MQTT状态主题1发布失败", "user_topic_publish_failed");

  bool successHa = true;
  if (config.mqttHaDiscovery) {
    publishStart = millis();
    Serial.println("[耗时] START MQTT发布状态HA主题");
    successHa = publishMqttPayload(mqttHaStatusTopic.c_str(), json.c_str(), true);
    Serial.printf("[耗时] END MQTT发布状态HA主题: elapsed=%lums, success=%s\n", millis() - publishStart, successHa ? "true" : "false");
    if (!successHa) publishErrorLog("mqtt", "publishMqttStatus", "MQTT状态HA发布失败", "ha_topic_publish_failed");
  }
  
  const bool success = success1 && successHa;
  Serial.printf("MQTT发布状态: %s, success=%s\n", status, success ? "true" : "false");
  logMqttElapsed("MQTT发布状态", statusStart, success);
  if (success) publishDeviceLog("info", "mqtt", (String("MQTT发布状态: ") + status).c_str());
}

void publishMqttDeviceStatus() {
  if (!config.mqttEnabled || !mqttClient.connected()) return;
  unsigned long statusStart = millis();
  Serial.println("[耗时] START MQTT设备状态上报");

  unsigned long stepStart = millis();
  String cesqResp = sendATCommand("AT+CESQ", 2000);
  logMqttElapsed("MQTT状态-AT+CESQ", stepStart);
  int rxlev = -1, rsrp = -1, rsrq = -1;
  int cesqIdx = cesqResp.indexOf("+CESQ:");
  if (cesqIdx >= 0) {
    String params = cesqResp.substring(cesqIdx + 6);
    params.trim();
    int vals[6] = {0};
    int vi = 0;
    int start = 0;
    for (int i = 0; i <= params.length() && vi < 6; i++) {
      if (i == params.length() || params[i] == ',') {
        vals[vi++] = params.substring(start, i).toInt();
        start = i + 1;
      }
    }
    rxlev = vals[0];
    rsrq = vals[4];
    rsrp = vals[5];
  }
  
  int rsrpDbm = (rsrp != 255 && rsrp <= 97) ? (rsrp - 141) : -999;
  int rsrqDb = (rsrq != 255 && rsrq <= 34) ? ((rsrq / 2) - 20) : -999;
  
  int wifiRssi = WiFi.RSSI();
  String wifiStatus = "未知";
  if (wifiRssi >= -50) wifiStatus = "极好";
  else if (wifiRssi >= -60) wifiStatus = "很好";
  else if (wifiRssi >= -70) wifiStatus = "良好";
  else if (wifiRssi >= -80) wifiStatus = "一般";
  else if (wifiRssi >= -90) wifiStatus = "较弱";
  else wifiStatus = "很差";
  
  String lteStatus = "未知";
  if (rsrpDbm != -999) {
    if (rsrpDbm >= -80) lteStatus = "极好";
    else if (rsrpDbm >= -90) lteStatus = "良好";
    else if (rsrpDbm >= -100) lteStatus = "一般";
    else if (rsrpDbm >= -110) lteStatus = "较弱";
    else lteStatus = "很差";
  }
  
  String apn = "";
  stepStart = millis();
  String cgdcontResp = sendATCommand("AT+CGDCONT?", 2000);
  logMqttElapsed("MQTT状态-AT+CGDCONT", stepStart);
  int cgdIdx = cgdcontResp.indexOf("+CGDCONT:");
  if (cgdIdx >= 0) {
    int idx0 = cgdcontResp.indexOf(",\"", cgdIdx);
    if (idx0 >= 0) {
      idx0 = cgdcontResp.indexOf(",\"", idx0 + 2);
      if (idx0 >= 0) {
        int endIdx0 = cgdcontResp.indexOf("\"", idx0 + 2);
        if (endIdx0 > idx0) apn = cgdcontResp.substring(idx0 + 2, endIdx0);
      }
    }
  }
  
  String json;
  json.reserve(512);
  json = "{";
  json += "\"status\":\"online\",";
  json += "\"device\":\"" + mqttDeviceId + "\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"wifi_rssi\":" + String(wifiRssi) + ",";
  json += "\"wifi_status\":\"" + jsonEscape(wifiStatus) + "\",";
  json += "\"wifi_ssid\":\"" + jsonEscape(WiFi.SSID()) + "\",";
  json += "\"uptime\":" + String(millis() / 1000) + ",";
  json += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"lte_rsrp\":" + String(rsrpDbm) + ",";
  json += "\"lte_rsrq\":" + String(rsrqDb) + ",";
  json += "\"lte_status\":\"" + jsonEscape(lteStatus) + "\",";
  json += "\"apn\":\"" + jsonEscape(apn) + "\",";
  json += mqttResetStatusFields();
  json += "}";
  
  stepStart = millis();
  Serial.println("[耗时] START MQTT设备状态发布主题1");
  bool success1 = publishMqttPayload(mqttTopicStatus.c_str(), json.c_str(), true);
  Serial.printf("[耗时] END MQTT设备状态发布主题1: elapsed=%lums, success=%s\n", millis() - stepStart, success1 ? "true" : "false");
  if (!success1) publishErrorLog("mqtt", "publishMqttDeviceStatus", "MQTT设备状态主题1发布失败", "user_topic_publish_failed");

  bool successHa = true;
  if (config.mqttHaDiscovery) {
    stepStart = millis();
    Serial.println("[耗时] START MQTT设备状态发布HA主题");
    successHa = publishMqttPayload(mqttHaStatusTopic.c_str(), json.c_str(), true);
    Serial.printf("[耗时] END MQTT设备状态发布HA主题: elapsed=%lums, success=%s\n", millis() - stepStart, successHa ? "true" : "false");
    if (!successHa) publishErrorLog("mqtt", "publishMqttDeviceStatus", "MQTT设备状态HA发布失败", "ha_topic_publish_failed");
  }
  
  const bool success = success1 && successHa;
  Serial.println(success ? "MQTT上报设备状态完成" : "MQTT上报设备状态失败");
  logMqttElapsed("MQTT设备状态上报", statusStart, success);
  if (success) publishDeviceLog("info", "mqtt", "MQTT上报设备状态");
}
