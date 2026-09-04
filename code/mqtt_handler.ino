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

static bool publishMqttPayload(const char* topic, const char* payload, bool retained = false) {
  bool published = mqttClient.publish(topic, payload, retained);
  if (esp_task_wdt_status(NULL) == ESP_OK) {
    esp_task_wdt_reset();
  }
  yield();
  return published;
}

static void logMqttElapsed(const char* label, unsigned long start) {
  unsigned long elapsed = millis() - start;
  Serial.printf("[耗时] END %s: elapsed=%lums\n", label, elapsed);
  publishTimingLog("mqtt", label, elapsed, true);
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
  String json = "{";
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
  json += "\"brownout_resets\":" + String(stats.brownoutResets) + ",";
  json += "\"poweron_resets\":" + String(stats.powerOnResets) + ",";
  json += "\"watchdog_resets\":" + String(stats.watchdogResets) + ",";
  json += "\"software_resets\":" + String(stats.softwareResets) + ",";
  json += "\"panic_resets\":" + String(stats.panicResets);
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
  payload += "\"name\":\"" + name + "\",";
  payload += "\"unique_id\":\"" + nodeId + suffix + "\",";
  payload += "\"state_topic\":\"" + mqttHaStatusTopic + "\",";
  payload += "\"value_template\":\"" + valueTemplate + "\",";
  if (unit.length() > 0) payload += "\"unit_of_measurement\":\"" + unit + "\",";
  if (stateClass.length() > 0) payload += "\"state_class\":\"" + stateClass + "\",";
  payload += "\"icon\":\"" + icon + "\",";
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
  deviceInfo += "\"identifiers\":[\"" + nodeId + "\"],";
  deviceInfo += "\"name\":\"短信转发器 " + mqttDeviceId + "\",";
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
  powerSuspectConfig += "\"unique_id\":\"" + nodeId + "_power_suspect\",";
  powerSuspectConfig += "\"state_topic\":\"" + mqttHaStatusTopic + "\",";
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
  onlineConfig += "\"unique_id\":\"" + nodeId + "_online\",";
  onlineConfig += "\"state_topic\":\"" + mqttHaStatusTopic + "\",";
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
  restartConfig += "\"unique_id\":\"" + nodeId + "_restart\",";
  restartConfig += "\"command_topic\":\"" + mqttTopicCmd + "\",";
  restartConfig += "\"payload_press\":\"{\\\"action\\\":\\\"restart\\\"}\",";
  restartConfig += "\"icon\":\"mdi:restart\",";
  restartConfig += deviceInfo;
  restartConfig += "}";
  publishMqttPayload(restartConfigTopic.c_str(), restartConfig.c_str(), true);

  String senderConfigTopic = haPrefix + "/sensor/" + nodeId + "_last_sender/config";
  String senderConfig = "{";
  senderConfig += "\"name\":\"最近短信发送者\",";
  senderConfig += "\"unique_id\":\"" + nodeId + "_last_sender\",";
  senderConfig += "\"state_topic\":\"" + mqttTopicSmsReceived + "\",";
  senderConfig += "\"value_template\":\"{{ value_json.sender }}\",";
  senderConfig += "\"icon\":\"mdi:account\",";
  senderConfig += deviceInfo;
  senderConfig += "}";
  publishMqttPayload(senderConfigTopic.c_str(), senderConfig.c_str(), true);

  String messageConfigTopic = haPrefix + "/sensor/" + nodeId + "_last_message/config";
  String messageConfig = "{";
  messageConfig += "\"name\":\"最近短信内容\",";
  messageConfig += "\"unique_id\":\"" + nodeId + "_last_message\",";
  messageConfig += "\"state_topic\":\"" + mqttTopicSmsReceived + "\",";
  messageConfig += "\"value_template\":\"{{ value_json.message[:50] }}{% if value_json.message | length > 50 %}...{% endif %}\",";
  messageConfig += "\"json_attributes_topic\":\"" + mqttTopicSmsReceived + "\",";
  messageConfig += "\"icon\":\"mdi:message-text\",";
  messageConfig += deviceInfo;
  messageConfig += "}";
  publishMqttPayload(messageConfigTopic.c_str(), messageConfig.c_str(), true);

  String smsEventConfigTopic = haPrefix + "/event/" + nodeId + "_sms/config";
  String smsEventConfig = "{";
  smsEventConfig += "\"name\":\"短信通知\",";
  smsEventConfig += "\"unique_id\":\"" + nodeId + "_sms_event\",";
  smsEventConfig += "\"state_topic\":\"" + mqttHaSmsReceivedTopic + "\",";
  smsEventConfig += "\"event_types\":[\"sms_received\"],";
  smsEventConfig += "\"icon\":\"mdi:message-badge\",";
  smsEventConfig += deviceInfo;
  smsEventConfig += "}";
  publishMqttPayload(smsEventConfigTopic.c_str(), smsEventConfig.c_str(), true);

  String lastCallerConfigTopic = haPrefix + "/sensor/" + nodeId + "_last_caller/config";
  String lastCallerConfig = "{";
  lastCallerConfig += "\"name\":\"最近来电号码\",";
  lastCallerConfig += "\"unique_id\":\"" + nodeId + "_last_caller\",";
  lastCallerConfig += "\"state_topic\":\"" + mqttTopicCallReceived + "\",";
  lastCallerConfig += "\"value_template\":\"{{ value_json.caller }}\",";
  lastCallerConfig += "\"json_attributes_topic\":\"" + mqttTopicCallReceived + "\",";
  lastCallerConfig += "\"icon\":\"mdi:phone-in-talk\",";
  lastCallerConfig += deviceInfo;
  lastCallerConfig += "}";
  publishMqttPayload(lastCallerConfigTopic.c_str(), lastCallerConfig.c_str(), true);

  String callEventConfigTopic = haPrefix + "/event/" + nodeId + "_call/config";
  String callEventConfig = "{";
  callEventConfig += "\"name\":\"来电通知\",";
  callEventConfig += "\"unique_id\":\"" + nodeId + "_call_event\",";
  callEventConfig += "\"state_topic\":\"" + mqttHaCallReceivedTopic + "\",";
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
  logEventConfig += "\"unique_id\":\"" + nodeId + "_log_event\",";
  logEventConfig += "\"state_topic\":\"" + mqttHaLogEventTopic + "\",";
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
  mqttClient.setSocketTimeout(5);

  String clientId = "sms_" + mqttDeviceId;
  Serial.println("连接MQTT服务器: " + config.mqttServer);
  Serial.println("客户端ID: " + clientId);
  
  bool connected = false;
  String willMessage = "{\"status\":\"offline\",\"device\":\"" + mqttDeviceId + "\"}";
  
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
  logMqttElapsed("MQTT重连", reconnectStart);
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];

  Serial.println("=== MQTT消息接收 ===");
  Serial.println("主题: " + String(topic));
  Serial.println("内容: " + message);
  Serial.println("====================");
  
  if (String(topic) == mqttTopicSmsSend) {
    int phoneStart = message.indexOf("\"phone\"");
    int msgStart = message.indexOf("\"message\"");
    
    if (phoneStart >= 0 && msgStart >= 0) {
      int phoneValStart = message.indexOf(":", phoneStart) + 1;
      int phoneValEnd = message.indexOf(",", phoneValStart);
      if (phoneValEnd < 0) phoneValEnd = message.indexOf("}", phoneValStart);
      String phoneRaw = message.substring(phoneValStart, phoneValEnd);
      phoneRaw.trim();
      if (phoneRaw.startsWith("\"")) phoneRaw = phoneRaw.substring(1);
      if (phoneRaw.endsWith("\"")) phoneRaw = phoneRaw.substring(0, phoneRaw.length() - 1);
      
      int msgValStart = message.indexOf(":", msgStart) + 1;
      int msgValEnd = message.lastIndexOf("\"");
      String msgRaw = message.substring(msgValStart, msgValEnd + 1);
      msgRaw.trim();
      if (msgRaw.startsWith("\"")) msgRaw = msgRaw.substring(1);
      if (msgRaw.endsWith("\"")) msgRaw = msgRaw.substring(0, msgRaw.length() - 1);
      
      Serial.println("MQTT发送短信命令:");
      Serial.println("  目标: " + phoneRaw);
      Serial.println("  内容: " + msgRaw);
      
      bool success = sendSMS(phoneRaw.c_str(), msgRaw.c_str());
      if (success) {
        stats.smsSent++;
        saveStats();
      }
      publishMqttSmsSent(phoneRaw.c_str(), msgRaw.c_str(), success);
    } else {
      Serial.println("短信命令格式错误");
      publishMqttSmsSent("", "", false);
    }
  } else if (String(topic) == mqttTopicPing) {
    String host = "8.8.8.8";

    int hostStart = message.indexOf("\"host\"");
    if (hostStart >= 0) {
      int hostValStart = message.indexOf(":", hostStart) + 1;
      int hostValEnd = message.indexOf("\"", hostValStart + 2);
      if (hostValEnd > hostValStart) {
        String hostRaw = message.substring(hostValStart, hostValEnd + 1);
        hostRaw.trim();
        if (hostRaw.startsWith("\"")) hostRaw = hostRaw.substring(1);
        if (hostRaw.endsWith("\"")) hostRaw = hostRaw.substring(0, hostRaw.length() - 1);
        if (hostRaw.length() > 0) host = hostRaw;
      }
    }
    
    Serial.println("MQTT Ping命令: " + host);
    sendATCommand("AT+CGACT=1,1", 10000);
    delay(500);
    
    String pingCmd = "AT+MPING=\"" + host + "\",30,1";
    while (Serial1.available()) Serial1.read();
    Serial1.println(pingCmd);
    
    unsigned long start = millis();
    String resp = "";
    bool gotResult = false;
    String resultMsg = "";
    bool pingSuccess = false;
    
    while (millis() - start < 35000) {
      while (Serial1.available()) {
        char c = Serial1.read();
        resp += c;
        
        int mpingIdx = resp.indexOf("+MPING:");
        if (mpingIdx >= 0) {
          int lineEnd = resp.indexOf('\n', mpingIdx);
          if (lineEnd >= 0) {
            String mpingLine = resp.substring(mpingIdx, lineEnd);
            mpingLine.trim();
            
            int colonIdx = mpingLine.indexOf(':');
            if (colonIdx >= 0) {
              String params = mpingLine.substring(colonIdx + 1);
              params.trim();
              
              int commaIdx = params.indexOf(',');
              int result = params.substring(0, commaIdx > 0 ? commaIdx : params.length()).toInt();
              
              gotResult = true;
              pingSuccess = (result == 0 || result == 1) || (params.indexOf(',') >= 0 && params.length() > 5);
              resultMsg = pingSuccess && commaIdx > 0 ? params : "错误码: " + String(result);
            }
            break;
          }
        }
        
        if (resp.indexOf("ERROR") >= 0) {
          gotResult = true;
          pingSuccess = false;
          resultMsg = "模组错误";
          break;
        }
      }
      if (gotResult) break;
      esp_task_wdt_reset();
      yield();
      delay(10);
    }
    
    sendATCommand("AT+CGACT=0,1", 5000);
    if (!gotResult) resultMsg = "超时";
    publishMqttPingResult(host.c_str(), pingSuccess, resultMsg.c_str());
  } else if (String(topic) == mqttTopicCmd) {
    int actionStart = message.indexOf("\"action\"");
    if (actionStart >= 0) {
      int actionValStart = message.indexOf(":", actionStart) + 1;
      int actionValEnd = message.indexOf("\"", actionValStart + 2);
      String actionRaw = message.substring(actionValStart, actionValEnd + 1);
      actionRaw.trim();
      if (actionRaw.startsWith("\"")) actionRaw = actionRaw.substring(1);
      if (actionRaw.endsWith("\"")) actionRaw = actionRaw.substring(0, actionRaw.length() - 1);
      
      Serial.println("MQTT控制命令: " + actionRaw);
      
      if (actionRaw == "restart" || actionRaw == "reset") {
        Serial.println("执行重启命令...");
        publishMqttStatus("restarting");
        delay(500);
        ESP.restart();
      } else if (actionRaw == "status") {
        String statusJson = buildMqttStatusJson("online");
        publishMqttPayload(mqttTopicStatus.c_str(), statusJson.c_str(), true);
        if (config.mqttHaDiscovery) {
          publishMqttPayload(mqttHaStatusTopic.c_str(), statusJson.c_str(), true);
        }
        Serial.println("已发送状态信息");
      } else {
        Serial.println("未知命令: " + actionRaw);
      }
    }
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

  String json = "{";
  json += "\"event_type\":\"sms_received\",";
  json += "\"sender\":\"" + jsonEscape(String(sender)) + "\",";
  json += "\"message\":\"" + jsonEscape(String(message)) + "\",";
  json += "\"time\":\"" + jsonEscape(String(timestamp)) + "\",";
  json += "\"timestamp\":\"" + jsonEscape(String(timestamp)) + "\",";
  json += "\"device\":\"" + mqttDeviceId + "\"";
  json += "}";
  
  Serial.println(" 主题1: " + mqttTopicSmsReceived);
  unsigned long publishStart = millis();
  Serial.println("[耗时] START MQTT发布短信主题1");
  bool success1 = publishMqttPayload(mqttTopicSmsReceived.c_str(), json.c_str());
  Serial.printf("[耗时] END MQTT发布短信主题1: elapsed=%lums, success=%s\n", millis() - publishStart, success1 ? "true" : "false");

  if (config.mqttHaDiscovery) {
    Serial.println(" 主题2 (HA): " + mqttHaSmsReceivedTopic);
    publishStart = millis();
    Serial.println("[耗时] START MQTT发布短信HA主题");
    bool successHa = publishMqttPayload(mqttHaSmsReceivedTopic.c_str(), json.c_str());
    Serial.printf("[耗时] END MQTT发布短信HA主题: elapsed=%lums, success=%s\n", millis() - publishStart, successHa ? "true" : "false");
  }
  
  if (success1) {
    Serial.println("MQTT短信推送完成");
    publishDeviceLog("info", "mqtt", "MQTT短信推送完成");
  } else {
    Serial.println("MQTT短信推送失败");
    publishErrorLog("mqtt", "publishMqttSmsReceived", "MQTT短信推送失败", "publish_failed");
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

  String json = "{";
  json += "\"event_type\":\"incoming_call\",";
  json += "\"caller\":\"" + jsonEscape(String(caller)) + "\",";
  json += "\"timestamp\":\"" + jsonEscape(String(timestamp)) + "\",";
  json += "\"device\":\"" + mqttDeviceId + "\"";
  json += "}";

  Serial.println(" 主题1: " + mqttTopicCallReceived);
  unsigned long publishStart = millis();
  Serial.println("[耗时] START MQTT发布来电主题1");
  bool success1 = publishMqttPayload(mqttTopicCallReceived.c_str(), json.c_str());
  Serial.printf("[耗时] END MQTT发布来电主题1: elapsed=%lums, success=%s\n", millis() - publishStart, success1 ? "true" : "false");

  if (config.mqttHaDiscovery) {
    Serial.println(" 主题2 (HA): " + mqttHaCallReceivedTopic);
    publishStart = millis();
    Serial.println("[耗时] START MQTT发布来电HA主题");
    bool successHa = publishMqttPayload(mqttHaCallReceivedTopic.c_str(), json.c_str());
    Serial.printf("[耗时] END MQTT发布来电HA主题: elapsed=%lums, success=%s\n", millis() - publishStart, successHa ? "true" : "false");
  }

  if (success1) {
    Serial.println("MQTT来电推送完成");
    publishDeviceLog("info", "mqtt", "MQTT来电推送完成");
  } else {
    Serial.println("MQTT来电推送失败");
    publishErrorLog("mqtt", "publishMqttCallReceived", "MQTT来电推送失败", "publish_failed");
  }
}

void publishMqttSmsSent(const char* phone, const char* message, bool success) {
  if (!config.mqttEnabled || !mqttClient.connected()) return;
  
  String json = "{";
  json += "\"success\":" + String(success ? "true" : "false") + ",";
  json += "\"phone\":\"" + jsonEscape(String(phone)) + "\",";
  json += "\"message\":\"" + jsonEscape(String(message)) + "\",";
  json += "\"device\":\"" + mqttDeviceId + "\"";
  json += "}";
  
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

  if (config.mqttHaDiscovery) {
    publishStart = millis();
    Serial.println("[耗时] START MQTT发布状态HA主题");
    bool successHa = publishMqttPayload(mqttHaStatusTopic.c_str(), json.c_str(), true);
    Serial.printf("[耗时] END MQTT发布状态HA主题: elapsed=%lums, success=%s\n", millis() - publishStart, successHa ? "true" : "false");
  }
  
  Serial.println("MQTT发布状态: " + String(status));
  logMqttElapsed("MQTT发布状态", statusStart);
  publishDeviceLog("info", "mqtt", (String("MQTT发布状态: ") + status).c_str());
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
  
  String json = "{";
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

  if (config.mqttHaDiscovery) {
    stepStart = millis();
    Serial.println("[耗时] START MQTT设备状态发布HA主题");
    bool successHa = publishMqttPayload(mqttHaStatusTopic.c_str(), json.c_str(), true);
    Serial.printf("[耗时] END MQTT设备状态发布HA主题: elapsed=%lums, success=%s\n", millis() - stepStart, successHa ? "true" : "false");
  }
  
  Serial.println("MQTT上报设备状态");
  logMqttElapsed("MQTT设备状态上报", statusStart);
  publishDeviceLog("info", "mqtt", "MQTT上报设备状态");
}
