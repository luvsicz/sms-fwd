#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "config.h"

// 出站预算按字节计算，不等同于 PubSubClient 的常驻收包缓冲。
// 每段保守取 160 个字符、每字符 8 字节（UTF-8 最多 4，JSON 控制转义最多 6），
// 再留 1 KiB 字段开销；10 段不会因固定 1024 字节缓冲而被拒绝。
static const size_t MQTT_MAX_TOPIC_BYTES = 1024;
static const size_t MQTT_MAX_PAYLOAD_BYTES = 16 * 1024;
static const size_t MQTT_MAX_COMMAND_BYTES = 8192;
// PubSubClient 接收整包：固定头 + 主题长度/主题 + QoS1 packet ID + JSON。
static const size_t MQTT_RECEIVE_BUFFER_BYTES = MQTT_MAX_HEADER_SIZE + 2U + MQTT_MAX_TOPIC_BYTES + 2U + MQTT_MAX_COMMAND_BYTES;
static const size_t MQTT_MAX_BUFFER_BYTES = MQTT_RECEIVE_BUFFER_BYTES;
static const size_t MQTT_PUBLISH_CHUNK_BYTES = 512;
static_assert(MQTT_MAX_PAYLOAD_BYTES >= MAX_CONCAT_PARTS * 160U * 8U + 1024U,
              "MQTT payload budget must cover the maximum concatenated SMS");
static_assert(MQTT_MAX_PAYLOAD_BYTES + MQTT_MAX_TOPIC_BYTES + 2U < 65536U,
              "PubSubClient 2.8 buildHeader uses uint16_t remaining length");
static_assert(MQTT_MAX_BUFFER_BYTES >= MQTT_MAX_HEADER_SIZE + 2U + MQTT_MAX_TOPIC_BYTES &&
              MQTT_MAX_BUFFER_BYTES < 65536U,
              "MQTT buffer must hold the topic header and fit setBufferSize");

// 出站 JSON 构造器只引用调用期间有效的字段，不复制/截断输入。
struct MqttJsonField {
  const char* name;
  const char* value;
  bool quoted = true;  // false 仅用于内部 true/false 常量
};

// 外部 MQTT 变量声明
extern WiFiClient mqttWifiClient;
extern PubSubClient mqttClient;

extern String mqttDeviceId;

// 用户自定义前缀主题
extern String mqttTopicStatus;
extern String mqttTopicSmsReceived;
extern String mqttTopicCallReceived;
extern String mqttTopicSmsSent;
extern String mqttTopicPingResult;
extern String mqttTopicLog;
extern String mqttTopicLogSlow;
extern String mqttTopicLogError;
extern String mqttTopicMetricTiming;
extern String mqttTopicSmsSend;
extern String mqttTopicPing;
extern String mqttTopicCmd;

// Home Assistant 自动发现主题
extern String mqttHaStatusTopic;      // HA 状态发布主题
extern String mqttHaSmsReceivedTopic; // HA 短信接收事件主题
extern String mqttHaCallReceivedTopic; // HA 来电事件主题
extern String mqttHaLogEventTopic;    // HA 日志事件主题

extern unsigned long lastMqttReconnectAttempt;
extern unsigned long lastMqttStatusReport;
extern const unsigned long MQTT_RECONNECT_INTERVAL;
extern const unsigned long MQTT_STATUS_INTERVAL;

enum MqttLogKind {
  MQTT_LOG_KIND_INFO = 0,
  MQTT_LOG_KIND_TIMING = 1,
  MQTT_LOG_KIND_ERROR = 2,
  MQTT_LOG_KIND_SLOW = 3
};

struct MqttLogEntry {
  MqttLogKind kind;
  String level;
  String module;
  String message;
  String step;
  String error;
  unsigned long elapsedMs;
  unsigned long thresholdMs;
  bool success;
  unsigned long timestampMs;
};

// MQTT 函数声明
bool ensureMqttBufferSize(size_t minimumBytes); // 有界分配，失败不使用未经确认的缓冲
void serviceMqtt();  // 主任务/AT idle：仅 loop 保活和回调入队，不重连/执行待办
void mqttCallback(char* topic, byte* payload, unsigned int length);
void mqttReconnect();
void initMqttTopics();
String getMacSuffix();
void publishMqttSmsReceived(const char* sender, const char* message, const char* timestamp);
void publishMqttCallReceived(const char* caller, const char* timestamp);
void publishMqttSmsSent(const char* phone, const char* message, bool success);
void publishMqttPingResult(const char* host, bool success, const char* result);
void publishMqttStatus(const char* status);
void publishMqttDeviceStatus();
void publishDeviceLog(const char* level, const char* module, const char* message);
void publishTimingLog(const char* module, const char* step, unsigned long elapsedMs, bool success);
void publishErrorLog(const char* module, const char* step, const char* message, const char* error, unsigned long elapsedMs = 0);
void processMqttLogQueue();
void processMqttPendingCommands();  // P1: 在 loop 中调用，异步处理 MQTT 命令

// Home Assistant 自动发现
void publishHaDiscoveryConfig();     // 发布 HA 自动发现配置

#endif // MQTT_HANDLER_H
