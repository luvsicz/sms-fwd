#ifndef MQTT_HANDLER_H
#define MQTT_HANDLER_H

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "config.h"

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

// Home Assistant 自动发现
void publishHaDiscoveryConfig();     // 发布 HA 自动发现配置

#endif // MQTT_HANDLER_H
