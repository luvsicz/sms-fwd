/*
 * config.ino - 配置相关函数实现
 */

#include "push_service.h"
#include "history_json.h"
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// 短信历史和统计的全局变量定义
SmsRecord smsHistory[MAX_SMS_HISTORY];
int smsHistoryIndex = 0;
Statistics stats;
esp_reset_reason_t lastResetReasonCode = ESP_RST_UNKNOWN;
String lastResetReasonText = "未知";
bool lastResetPowerSuspected = false;
InterruptedOperationDiagnostic lastInterruptedOperation = {
  false, "", "", 0, 0, 0, 0
};

static SemaphoreHandle_t statsMutex = nullptr;

void initStatsLock() {
  if (statsMutex == nullptr) {
    statsMutex = xSemaphoreCreateMutex();
    if (statsMutex == nullptr) {
      Serial.println("统计锁创建失败，统计持久化将使用无锁降级路径");
    }
  }
}

// 统计原子安全自增辅助函数（支持多线程/任务安全并发递增）
void statsIncrementSmsReceived() { stats.smsReceived++; }
void statsIncrementSmsSent() { stats.smsSent++; }
void statsIncrementCallsReceived() { stats.callsReceived++; }
void statsIncrementPushSuccess() { stats.pushSuccess++; }
void statsIncrementPushFailed() { stats.pushFailed++; }

// 生成紧凑 NVS 键名，确保即便 MAX_WIFI_NETWORKS 增大也不会超过 15 字符 (M4)
static String makeNvsWifiKey(const char* prefix, int index) {
  String key = String(prefix) + String(index);
  if (key.length() > 15) {
    Serial.printf("警告: NVS键名超长(长度=%u)，截断至15字符: %s\n", key.length(), key.c_str());
    key = key.substring(0, 15);
  }
  return key;
}

uint64_t monotonicMillis() {
  return static_cast<uint64_t>(esp_timer_get_time()) / 1000ULL;
}

void resetTimerSchedule() {
  // 保持 UI 的 1 天最小值；先确保为正，再以 64 位换算，包含 INT_MAX 天也不溢出。
  if (config.timerInterval < 1) config.timerInterval = 1;
  timerIntervalSec = static_cast<uint64_t>(config.timerInterval) * 86400ULL;
  lastTimerExec = monotonicMillis();
}

uint64_t timerRemainingSeconds() {
  if (!config.timerEnabled || timerIntervalSec == 0) return 0;
  const uint64_t elapsedSec = (monotonicMillis() - lastTimerExec) / 1000ULL;
  return elapsedSec < timerIntervalSec ? timerIntervalSec - elapsedSec : 0;
}

String resetReasonToString(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "上电复位";
    case ESP_RST_EXT: return "外部复位";
    case ESP_RST_SW: return "软件重启";
    case ESP_RST_PANIC: return "异常崩溃复位";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT: return "看门狗复位";
    case ESP_RST_BROWNOUT: return "欠压复位";
    case ESP_RST_DEEPSLEEP: return "深睡唤醒";
    case ESP_RST_SDIO: return "SDIO 复位";
    case ESP_RST_USB: return "USB 复位";
    case ESP_RST_JTAG: return "JTAG 复位";
    case ESP_RST_EFUSE: return "eFuse 复位";
    case ESP_RST_UNKNOWN:
    default: return "未知";
  }
}

bool isPowerRelatedReset(esp_reset_reason_t reason) {
  return reason == ESP_RST_POWERON || reason == ESP_RST_BROWNOUT;
}

void recordOperationDiagnostic(const char* operation, const char* stage) {
  if (statsMutex == nullptr) initStatsLock();
  if (statsMutex != nullptr &&
      xSemaphoreTake(statsMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    Serial.println("诊断锁获取超时，跳过本次操作快照");
    return;
  }

  String operationText = operation ? String(operation) : "";
  String stageText = stage ? String(stage) : "";
  operationText = operationText.substring(0, 31);
  stageText = stageText.substring(0, 31);

  preferences.begin("sms_diag", false);
  preferences.putBool("active", true);
  preferences.putString("op", operationText);
  preferences.putString("stage", stageText);
  preferences.putULong("uptime", millis() / 1000);
  preferences.putULong("heap", ESP.getFreeHeap());
  preferences.putULong("minHeap", ESP.getMinFreeHeap());
  preferences.putULong("maxAlloc", ESP.getMaxAllocHeap());
  preferences.end();

  if (statsMutex != nullptr) xSemaphoreGive(statsMutex);
}

void clearOperationDiagnostic() {
  if (statsMutex == nullptr) initStatsLock();
  if (statsMutex != nullptr &&
      xSemaphoreTake(statsMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    Serial.println("诊断锁获取超时，未清除操作快照");
    return;
  }

  preferences.begin("sms_diag", false);
  preferences.putBool("active", false);
  preferences.end();

  if (statsMutex != nullptr) xSemaphoreGive(statsMutex);
}

static void loadInterruptedOperationDiagnostic() {
  preferences.begin("sms_diag", true);
  lastInterruptedOperation.valid = preferences.getBool("active", false);
  lastInterruptedOperation.operation = preferences.getString("op", "");
  lastInterruptedOperation.stage = preferences.getString("stage", "");
  lastInterruptedOperation.uptimeSeconds = preferences.getULong("uptime", 0);
  lastInterruptedOperation.freeHeap = preferences.getULong("heap", 0);
  lastInterruptedOperation.minFreeHeap = preferences.getULong("minHeap", 0);
  lastInterruptedOperation.maxAllocHeap = preferences.getULong("maxAlloc", 0);
  preferences.end();

  if (lastInterruptedOperation.valid) {
    preferences.begin("sms_diag", false);
    preferences.putBool("active", false);
    preferences.end();
  }
}

bool configStringFitsNvs(const String& value) {
  // 不按字符数计算，也不截断；嵌入 NUL 会被 Preferences::putString 静默截短。
  return value && value.length() <= CONFIG_NVS_MAX_STRING_BYTES &&
         memchr(value.c_str(), '\0', value.length()) == nullptr;
}

bool configFitsNvs(const Config& value) {
  const String* strings[] = {
    &value.smtpServer, &value.smtpUser, &value.smtpPass, &value.smtpSendTo,
    &value.webUser, &value.webPass, &value.timerPhone, &value.timerMessage,
    &value.mqttServer, &value.mqttUser, &value.mqttPass, &value.mqttPrefix,
    &value.mqttHaPrefix, &value.filterList, &value.contentFilterList
  };
  for (const String* text : strings) if (!configStringFitsNvs(*text)) return false;
  for (const WifiNetwork& wifi : value.wifiNetworks) {
    if (!configStringFitsNvs(wifi.ssid) || !configStringFitsNvs(wifi.password)) return false;
  }
  for (const PushChannel& channel : value.pushChannels) {
    if (!configStringFitsNvs(channel.name) || !configStringFitsNvs(channel.url) ||
        !configStringFitsNvs(channel.key1) || !configStringFitsNvs(channel.key2) ||
        !configStringFitsNvs(channel.customBody)) return false;
  }
  return true;
}

static bool putConfigString(const char* key, const String& value) {
  // 使用 const char* 重载，避免 Preferences 按值复制 String 时发生静默 OOM。
  if (value.length()) return preferences.putString(key, value.c_str()) == value.length();
  // 空字符串成功/失败都返回 0；用单字节缓冲确认该键实际存储了空串。
  // 不能以删除键代替空串，否则 loadConfig 的非空默认值会重新出现。
  char empty[1] = {'x'};
  if (preferences.getString(key, empty, sizeof(empty)) == 1 && empty[0] == '\0') return true;
  preferences.putString(key, "");
  empty[0] = 'x';
  return preferences.getString(key, empty, sizeof(empty)) == 1 && empty[0] == '\0';
}

static bool configSaveFailure(const char** error, const char* reason) {
  if (error) *error = reason;
  Serial.println(reason);  // 只记录原因，不输出配置值/密码。
  return false;
}

// 保存配置到 NVS；多键写入不是掉电原子事务，失败时明确返回并停止后续写入。
bool saveConfig(const char** error) {
  if (error) *error = nullptr;
  if (!configFitsNvs(config)) {
    return configSaveFailure(error, "配置字符串无效或超过3999个UTF-8字节，未保存");
  }
  if (statsMutex == nullptr) initStatsLock();
  if (statsMutex == nullptr || xSemaphoreTake(statsMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    return configSaveFailure(error, "配置存储锁不可用或获取超时，未保存，请重试");
  }
  if (!preferences.begin("sms_config", false)) {
    xSemaphoreGive(statsMutex);
    return configSaveFailure(error, "无法打开配置存储，未保存，请重试");
  }

  bool ok = true;
  auto putString = [&ok](const char* key, const String& value) {
    if (ok) ok = putConfigString(key, value);
  };
  auto putBool = [&ok](const char* key, bool value) {
    if (ok) ok = preferences.putBool(key, value) == sizeof(uint8_t);
  };
  auto putInt = [&ok](const char* key, int value) {
    if (ok) ok = preferences.putInt(key, value) == sizeof(int32_t);
  };
  auto putUChar = [&ok](const char* key, uint8_t value) {
    if (ok) ok = preferences.putUChar(key, value) == sizeof(uint8_t);
  };
  
  // 喂狗防止超时
  esp_task_wdt_reset();
  yield();
  
  // 保存 WiFi 配置（使用紧凑前缀确保不超过 15 字符，即使 MAX_WIFI_NETWORKS 增大也不会超限）
  for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
    char key[16];
    snprintf(key, sizeof(key), "w_s_%d", i); putString(key, config.wifiNetworks[i].ssid);
    snprintf(key, sizeof(key), "w_p_%d", i); putString(key, config.wifiNetworks[i].password);
    snprintf(key, sizeof(key), "w_e_%d", i); putBool(key, config.wifiNetworks[i].enabled);
    yield();  // 让出 CPU
  }
  
  esp_task_wdt_reset();
  
  putString("smtpServer", config.smtpServer);
  putInt("smtpPort", config.smtpPort);
  putString("smtpUser", config.smtpUser);
  putString("smtpPass", config.smtpPass);
  putString("smtpSendTo", config.smtpSendTo);
  putBool("smtpEn", config.emailEnabled);
  putString("webUser", config.webUser);
  putString("webPass", config.webPass);
  
  esp_task_wdt_reset();
  yield();
  
  // 保存推送通道配置
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    char key[16];
    const PushChannel& channel = config.pushChannels[i];
    snprintf(key, sizeof(key), "push%den", i); putBool(key, channel.enabled);
    snprintf(key, sizeof(key), "push%dtype", i); putUChar(key, (uint8_t)channel.type);
    snprintf(key, sizeof(key), "push%durl", i); putString(key, channel.url);
    snprintf(key, sizeof(key), "push%dname", i); putString(key, channel.name);
    snprintf(key, sizeof(key), "push%dk1", i); putString(key, channel.key1);
    snprintf(key, sizeof(key), "push%dk2", i); putString(key, channel.key2);
    snprintf(key, sizeof(key), "push%dbody", i); putString(key, channel.customBody);
    esp_task_wdt_reset();
    yield();  // 让出 CPU
  }
  
  // 保存定时任务配置
  putBool("timerEn", config.timerEnabled);
  putInt("timerType", config.timerType);
  putInt("timerInt", config.timerInterval);
  putString("timerPhone", config.timerPhone);
  putString("timerMsg", config.timerMessage);
  
  esp_task_wdt_reset();
  yield();
  
  // 保存 MQTT 配置
  putBool("mqttEn", config.mqttEnabled);
  putBool("mqttCtrlOnly", config.mqttControlOnly);
  putString("mqttServer", config.mqttServer);
  putInt("mqttPort", config.mqttPort);
  putString("mqttUser", config.mqttUser);
  putString("mqttPass", config.mqttPass);
  putString("mqttPrefix", config.mqttPrefix);
  
  // 保存 HA 自动发现配置
  putBool("mqttHaDisc", config.mqttHaDiscovery);
  putString("mqttHaPre", config.mqttHaPrefix);
  
  esp_task_wdt_reset();
  
  // 保存黑白名单配置（号码过滤）
  putBool("filterEn", config.filterEnabled);
  putBool("filterWL", config.filterIsWhitelist);
  putString("filterList", config.filterList);
  
  // 保存内容关键词过滤配置
  putBool("cfEn", config.contentFilterEnabled);
  putBool("cfWL", config.contentFilterIsWhitelist);
  putString("cfList", config.contentFilterList);

  // 全部写入成功后才清理旧 WiFi 键；清理失败不影响新键的读取。
  if (ok) {
    for (int i = 0; i < MAX_WIFI_NETWORKS; ++i) {
      char key[20];  // 旧键可能超 15 字节，此时 remove 自行拒绝，不能截成其他有效键。
      snprintf(key, sizeof(key), "wifi%dssid", i); preferences.remove(key);
      snprintf(key, sizeof(key), "wifi%dpass", i); preferences.remove(key);
      snprintf(key, sizeof(key), "wifi%den", i); preferences.remove(key);
    }
  }
  preferences.end();
  xSemaphoreGive(statsMutex);
  esp_task_wdt_reset();
  if (!ok) {
    return configSaveFailure(error, "NVS写入失败，配置未完整保存；请在重启前重试保存");
  }
  Serial.println("配置已保存");
  return true;
}

// 从 NVS 加载配置
void loadConfig() {
  preferences.begin("sms_config", true);
  
  // 加载 WiFi 配置（优先读取紧凑新键名，回退旧键名以保证向前兼容）
  for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
    String newKeySsid = makeNvsWifiKey("w_s_", i);
    String newKeyPass = makeNvsWifiKey("w_p_", i);
    String newKeyEn   = makeNvsWifiKey("w_e_", i);
    String oldPrefix  = "wifi" + String(i);

    if (i == 0) {
      // 第一个网络默认使用 wifi_config.h 中的配置
      String defaultSsid = WIFI_SSID;
      String defaultPass = WIFI_PASS;
      if (preferences.isKey(newKeySsid.c_str())) {
        config.wifiNetworks[i].ssid = preferences.getString(newKeySsid.c_str(), defaultSsid);
      } else {
        config.wifiNetworks[i].ssid = preferences.getString((oldPrefix + "ssid").c_str(), defaultSsid);
      }

      if (preferences.isKey(newKeyPass.c_str())) {
        config.wifiNetworks[i].password = preferences.getString(newKeyPass.c_str(), defaultPass);
      } else {
        config.wifiNetworks[i].password = preferences.getString((oldPrefix + "pass").c_str(), defaultPass);
      }

      if (preferences.isKey(newKeyEn.c_str())) {
        config.wifiNetworks[i].enabled = preferences.getBool(newKeyEn.c_str(), true);
      } else {
        config.wifiNetworks[i].enabled = preferences.getBool((oldPrefix + "en").c_str(), true);
      }
    } else {
      if (preferences.isKey(newKeySsid.c_str())) {
        config.wifiNetworks[i].ssid = preferences.getString(newKeySsid.c_str(), "");
      } else {
        config.wifiNetworks[i].ssid = preferences.getString((oldPrefix + "ssid").c_str(), "");
      }

      if (preferences.isKey(newKeyPass.c_str())) {
        config.wifiNetworks[i].password = preferences.getString(newKeyPass.c_str(), "");
      } else {
        config.wifiNetworks[i].password = preferences.getString((oldPrefix + "pass").c_str(), "");
      }

      if (preferences.isKey(newKeyEn.c_str())) {
        config.wifiNetworks[i].enabled = preferences.getBool(newKeyEn.c_str(), false);
      } else {
        config.wifiNetworks[i].enabled = preferences.getBool((oldPrefix + "en").c_str(), false);
      }
    }
  }
  
  config.smtpServer = preferences.getString("smtpServer", "");
  config.smtpServer.trim();
  config.smtpPort = preferences.getInt("smtpPort", 465);
  if (config.smtpPort <= 0) config.smtpPort = 465;
  config.smtpUser = preferences.getString("smtpUser", "");
  config.smtpUser.trim();
  config.smtpPass = preferences.getString("smtpPass", "");
  config.smtpPass.trim();
  config.smtpSendTo = preferences.getString("smtpSendTo", "");
  config.smtpSendTo.trim();
  config.emailEnabled = preferences.getBool("smtpEn", false);
  config.webUser = preferences.getString("webUser", DEFAULT_WEB_USER);
  config.webPass = preferences.getString("webPass", DEFAULT_WEB_PASS);
  
  // 加载推送通道配置
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String prefix = "push" + String(i);
    config.pushChannels[i].enabled = preferences.getBool((prefix + "en").c_str(), false);
    config.pushChannels[i].type = (PushType)preferences.getUChar((prefix + "type").c_str(), PUSH_TYPE_POST_JSON);
    config.pushChannels[i].url = preferences.getString((prefix + "url").c_str(), "");
    config.pushChannels[i].name = preferences.getString((prefix + "name").c_str(), "通道" + String(i + 1));
    config.pushChannels[i].key1 = preferences.getString((prefix + "k1").c_str(), "");
    config.pushChannels[i].key2 = preferences.getString((prefix + "k2").c_str(), "");
    config.pushChannels[i].customBody = preferences.getString((prefix + "body").c_str(), "");
  }
  
  // 加载定时任务配置
  config.timerEnabled = preferences.getBool("timerEn", false);
  config.timerType = preferences.getInt("timerType", 0);
  config.timerInterval = preferences.getInt("timerInt", 30);
  config.timerPhone = preferences.getString("timerPhone", "");
  config.timerMessage = preferences.getString("timerMsg", "保号短信");
  resetTimerSchedule();  // 启动后按完整周期重计，不恢复关机前的起点
  
  // 加载 MQTT 配置
  config.mqttEnabled = preferences.getBool("mqttEn", false);
  config.mqttControlOnly = preferences.getBool("mqttCtrlOnly", false);
  config.mqttServer = preferences.getString("mqttServer", "");
  config.mqttPort = preferences.getInt("mqttPort", 1883);
  config.mqttUser = preferences.getString("mqttUser", "");
  config.mqttPass = preferences.getString("mqttPass", "");
  config.mqttPrefix = preferences.getString("mqttPrefix", "sms");
  
  // 加载 HA 自动发现配置（默认启用）
  config.mqttHaDiscovery = preferences.getBool("mqttHaDisc", true);
  config.mqttHaPrefix = preferences.getString("mqttHaPre", "homeassistant");
  
  // 加载黑白名单配置（号码过滤）
  config.filterEnabled = preferences.getBool("filterEn", false);
  config.filterIsWhitelist = preferences.getBool("filterWL", false);
  config.filterList = preferences.getString("filterList", "");
  
  // 加载内容关键词过滤配置
  config.contentFilterEnabled = preferences.getBool("cfEn", false);
  config.contentFilterIsWhitelist = preferences.getBool("cfWL", false);
  config.contentFilterList = preferences.getString("cfList", "");
  
  preferences.end();
  
  // 加载统计数据
  loadStats();
  
  // 初始化短信历史
  for (int i = 0; i < MAX_SMS_HISTORY; i++) {
    smsHistory[i].valid = false;
  }
  
  Serial.println("配置已加载");
}

// 检查推送通道是否有效
bool isPushChannelValid(const PushChannel& ch) {
  if (!ch.enabled) return false;
  switch (ch.type) {
    case PUSH_TYPE_POST_JSON:
    case PUSH_TYPE_BARK:
    case PUSH_TYPE_GET:
    case PUSH_TYPE_CUSTOM:
    case PUSH_TYPE_WECOM:
    case PUSH_TYPE_DINGTALK:
      return ch.url.length() > 0;
    case PUSH_TYPE_TELEGRAM:
      // Telegram 需要 URL 和 chat_id (key1)
      return ch.url.length() > 0 && ch.key1.length() > 0;
    default:
      return false;
  }
}

// 检查配置是否有效
bool isConfigValid() {
  bool emailValid = config.emailEnabled &&
                    config.smtpServer.length() > 0 && 
                    config.smtpUser.length() > 0 && 
                    config.smtpPass.length() > 0 && 
                    config.smtpSendTo.length() > 0;
  
  bool pushValid = false;
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (isPushChannelValid(config.pushChannels[i])) {
      pushValid = true;
      break;
    }
  }
  
  return emailValid || pushValid;
}

// 获取设备 URL（同时显示 mDNS 和 IP）
String getDeviceUrl() {
  return "http://" MDNS_HOSTNAME ".local 或 http://" + WiFi.localIP().toString();
}

// 初始化 SPIFFS 并检查历史文件自愈恢复
void initSmsStorage() {
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS 初始化失败");
    return;
  }

  // 检查主历史文件与修剪临时文件自愈恢复 (H3)
  const char* historyFiles[] = {"/sms.txt", "/calls.txt"};
  for (const char* path : historyFiles) {
    String tempPath = String(path) + ".trim.tmp";
    if (!SPIFFS.exists(path) && SPIFFS.exists(tempPath)) {
      Serial.printf("检测到主历史文件 %s 缺失，但存在临时文件 %s，执行自愈恢复\n", path, tempPath.c_str());
      if (SPIFFS.rename(tempPath, path)) {
        Serial.printf("历史文件自愈恢复成功: %s\n", path);
      } else {
        Serial.printf("历史文件自愈恢复失败: %s\n", path);
      }
    } else if (SPIFFS.exists(path) && SPIFFS.exists(tempPath)) {
      // 主文件已完好但残留修剪临时文件，清理以释放闪存空间
      SPIFFS.remove(tempPath);
    }
  }
}

bool isLikelyValidHistoryJsonLine(const String& line) {
  String trimmed = line;
  trimmed.trim();
  if (trimmed.length() < 6) return false;
  if (!trimmed.startsWith("{") || !trimmed.endsWith("}")) return false;

  int quoteCount = 0;
  bool escaping = false;
  for (unsigned int i = 0; i < trimmed.length(); i++) {
    char c = trimmed.charAt(i);
    if (escaping) {
      escaping = false;
      continue;
    }
    if (c == '\\') {
      escaping = true;
      continue;
    }
    if (c == '"') quoteCount++;
    if ((uint8_t)c < 0x20 && c != '\t') return false;
  }

  return (quoteCount % 2) == 0;
}

HistoryCleanupResult cleanupHistoryFile(const char* path) {
  HistoryCleanupResult result = {0, 0};

  File src = SPIFFS.open(path, "r");
  if (!src) {
    return result;
  }

  String tempPath = String(path) + ".tmp";
  File dst = SPIFFS.open(tempPath, "w");
  if (!dst) {
    src.close();
    Serial.println("历史清洗失败：无法创建临时文件");
    return result;
  }

  while (src.available()) {
    String line = src.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) {
      continue;
    }

    if (isLikelyValidHistoryJsonLine(line)) {
      dst.println(line);
      result.kept++;
    } else {
      result.removed++;
    }
  }

  src.close();
  dst.close();

  SPIFFS.remove(path);
  if (!SPIFFS.rename(tempPath, path)) {
    Serial.println("历史清洗失败：无法替换原文件，保留临时文件以待恢复");
    result.kept = 0;
    result.removed = 0;
  }

  return result;
}

// 安全修剪历史文件：采用固定小缓冲区流式复制到临时文件，避免低内存下大字符串分配失败清空文件 (M2, M3, M5)
bool safeTrimHistoryFile(const char* path, size_t maxSize, size_t targetSize) {
  if (!path || path[0] == '\0') return false;

  File src = SPIFFS.open(path, "r");
  if (!src) {
    return false; // M3: 打开失败立即返回，不调用 close()
  }

  size_t fileSize = src.size();
  if (fileSize <= maxSize) {
    src.close();
    return true; // 文件未超过上限，无需修剪
  }

  // 低内存安全防护：避免在极端内存不足时修剪导致数据丢失 (M2)
  if (ESP.getMaxAllocHeap() < 4096) {
    Serial.printf("内存水位极低(maxAlloc=%u)，跳过历史文件修剪: %s\n", ESP.getMaxAllocHeap(), path);
    src.close();
    return false;
  }

  size_t startOffset = (fileSize > targetSize) ? (fileSize - targetSize) : (fileSize / 2);
  if (!src.seek(startOffset)) {
    Serial.println("历史修剪 seek 失败");
    src.close();
    return false;
  }

  // 跳过定位点可能截断的半行，找到下一个换行符作为完整行的起点
  bool foundNewline = false;
  while (src.available()) {
    char c = (char)src.read();
    ++startOffset;
    if (c == '\n') {
      foundNewline = true;
      break;
    }
  }

  if (!foundNewline || !src.available()) {
    Serial.println("历史修剪未找到有效的后续行，保留原文件");
    src.close();
    return false;
  }

  String tempPath = String(path) + ".trim.tmp";
  File dst = SPIFFS.open(tempPath, "w");
  if (!dst) {
    Serial.println("历史修剪无法创建临时文件");
    src.close();
    return false;
  }

  // 使用栈上固定 256 字节小缓冲区流式分块拷贝，杜绝 String 内存碎片与 OOM (M2, M5)
  uint8_t buffer[256];
  size_t copiedBytes = 0;
  while (src.available()) {
    int bytesRead = src.read(buffer, sizeof(buffer));
    if (bytesRead > 0) {
      size_t written = dst.write(buffer, bytesRead);
      if (written != (size_t)bytesRead) {
        Serial.println("历史修剪写入临时文件异常");
        src.close();
        dst.close();
        SPIFFS.remove(tempPath);
        return false;
      }
      copiedBytes += written;
    } else {
      break;
    }
  }

  src.close();
  dst.close();

  // 空内容防护：若复制字节为 0，说明未获取到有效内容，绝不覆盖原文件！ (M2)
  if (copiedBytes == 0 || copiedBytes != fileSize - startOffset) {
    Serial.println("历史修剪读取不完整，取消替换，保护原历史记录");
    SPIFFS.remove(tempPath);
    return false;
  }

  // 临时文件写入有效，执行原子替换
  SPIFFS.remove(path);
  if (!SPIFFS.rename(tempPath, path)) {
    Serial.println("历史修剪替换原文件失败，保留临时文件以待自愈");
    return false;
  }

  Serial.printf("历史文件修剪成功: %s, 原大小: %u, 修剪后: %u\n", path, fileSize, copiedBytes);
  return true;
}

static bool appendHistoryRecord(const char* path, const char* number, const char* message,
                                const char* timestamp, bool sms) {
  const auto fail = [path](const char* reason) {
    Serial.printf("历史写入失败: %s (%s)\n", path, reason);
    return false;
  };
  size_t timeBytes, numberBytes, messageBytes = 0;
  if (!historyQuotedSize(timestamp, HISTORY_MAX_TIMESTAMP_BYTES, timeBytes) ||
      !historyQuotedSize(number, HISTORY_MAX_NUMBER_BYTES, numberBytes) ||
      (sms && !historyQuotedSize(message, SMS_HISTORY_MAX_MESSAGE_BYTES, messageBytes))) {
    return fail("字段超限或UTF-8无效，未截断正文");
  }
  const size_t expected = timeBytes + numberBytes + messageBytes + (sms ? 17U : 12U);
  if (ESP.getFreeHeap() < expected + HISTORY_MIN_FREE_HEAP || ESP.getMaxAllocHeap() <= expected + 32) {
    return fail("内存不足");
  }
  String line;
  if (!line.reserve(expected) || !line.concat("{\"t\":", 5) || !historyAppendQuoted(line, timestamp) ||
      !line.concat(sms ? ",\"s\":" : ",\"n\":", 5) || !historyAppendQuoted(line, number) ||
      (sms && (!line.concat(",\"m\":", 5) || !historyAppendQuoted(line, message))) ||
      !line.concat("}\n", 2) || line.length() != expected) return fail("JSON构造失败");

  const size_t maxSize = sms ? SMS_HISTORY_MAX_FILE_BYTES : CALL_HISTORY_MAX_FILE_BYTES;
  const size_t target = sms ? SMS_HISTORY_TRIM_BYTES : CALL_HISTORY_TRIM_BYTES;
  // 为本条记录及分隔符留空间；修剪失败则拒绝追加，不能让文件无限增长。
  if (SPIFFS.exists(path) && !safeTrimHistoryFile(path, maxSize - line.length() - 1, target)) return fail("修剪失败");
  File file = SPIFFS.open(path, "a");
  if (!file) return fail("无法打开文件");
  // 独立 LF 隔开上次短写的残行，保证下一条完整记录仍可读取。
  const bool written = file.write(reinterpret_cast<const uint8_t*>("\n"), 1) == 1 &&
    file.write(reinterpret_cast<const uint8_t*>(line.c_str()), line.length()) == line.length();
  file.close();
  return written || fail("短写，保留旧记录");
}

// 接收统计仍记录实际收到的短信；持久化失败单独诊断，不伪造成功记录。
void addSmsToHistory(const char* sender, const char* message, const char* timestamp) {
  statsIncrementSmsReceived();
  appendHistoryRecord("/sms.txt", sender, message, timestamp, true);
  saveStats();
}

static String readHistoryJson(const char* path, bool sms) {
  const auto fail = [path]() -> String {
    Serial.printf("历史读取失败: %s，内存不足或文件读取异常\n", path);
    return String();  // 空串为错误；有效空历史为 []，Web 可区分后返回 503。
  };
  File file = SPIFFS.open(path, "r");
  if (!file) return SPIFFS.exists(path) ? fail() : String("[]");
  struct Record { size_t offset; size_t length; };
  std::unique_ptr<Record[]> records(new (std::nothrow) Record[MAX_SMS_HISTORY]);
  if (!records) return fail();
  const size_t maxLine = sms ? SMS_HISTORY_MAX_LINE_BYTES : CALL_HISTORY_MAX_LINE_BYTES;
  size_t head = 0, count = 0, position = 0, lineStart = 0;
  const size_t fileSize = file.size();
  uint8_t chunk[256];
  HistoryJsonLine validator(sms);
  while (position < fileSize) {
    const size_t remaining = fileSize - position;
    const size_t amount = remaining < sizeof(chunk) ? remaining : sizeof(chunk);
    if (file.read(chunk, amount) != static_cast<int>(amount)) return fail();
    for (size_t i = 0; i < amount; ++i) {
      if (chunk[i] == '\n') {
        const size_t length = position + i - lineStart;
        if (length <= maxLine && validator.complete()) {
          records[head] = {lineStart, length};
          head = (head + 1) % MAX_SMS_HISTORY;
          if (count < MAX_SMS_HISTORY) ++count;
        }
        lineStart = position + i + 1;
        validator = HistoryJsonLine(sms);
      } else {
        validator.consume(chunk[i]);
      }
    }
    position += amount;
    if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
    yield();
  }
  // 只认以 LF 提交的完整行，损坏/超长/短写尾部不当作合法记录。
  if (!count) return "[]";
  size_t budget = sms ? SMS_HISTORY_MAX_RESPONSE_BYTES : CALL_HISTORY_MAX_RESPONSE_BYTES;
  const size_t freeHeap = ESP.getFreeHeap(), maxAlloc = ESP.getMaxAllocHeap();
  if (freeHeap <= HISTORY_MIN_FREE_HEAP + 32 || maxAlloc <= 32) return fail();
  if (budget > freeHeap - HISTORY_MIN_FREE_HEAP - 32) budget = freeHeap - HISTORY_MIN_FREE_HEAP - 32;
  if (budget > maxAlloc - 32) budget = maxAlloc - 32;
  size_t selected = 0, total = 2;
  while (selected < count) {
    const size_t index = (head + MAX_SMS_HISTORY - 1 - selected) % MAX_SMS_HISTORY;
    const size_t extra = records[index].length + (selected ? 1U : 0U);
    if (total > budget || extra > budget - total) break;
    total += extra;
    ++selected;
  }
  if (!selected) return fail();
  if (selected < count) Serial.printf("历史响应仅包含容量内最新 %u 条完整记录\n", (unsigned int)selected);
  String result;
  if (!result.reserve(total) || !result.concat("[", 1)) return fail();
  for (size_t i = 0; i < selected; ++i) {
    const Record& record = records[(head + MAX_SMS_HISTORY - 1 - i) % MAX_SMS_HISTORY];
    if ((i && !result.concat(",", 1)) || !file.seek(record.offset)) return fail();
    size_t left = record.length;
    while (left) {
      const size_t amount = left < sizeof(chunk) ? left : sizeof(chunk);
      if (file.read(chunk, amount) != static_cast<int>(amount) ||
          !result.concat(reinterpret_cast<const char*>(chunk), amount)) return fail();
      left -= amount;
    }
  }
  if (!result.concat("]", 1) || result.length() != total) return fail();
  return result;
}

String getSmsHistory() { return readHistoryJson("/sms.txt", true); }

// 清空短信历史记录
void clearSmsHistory() {
  if (SPIFFS.exists("/sms.txt")) {
    SPIFFS.remove("/sms.txt");
  }

  File f = SPIFFS.open("/sms.txt", "w");
  if (f) {
    f.close();
  }

  for (int i = 0; i < MAX_SMS_HISTORY; i++) {
    smsHistory[i].valid = false;
    smsHistory[i].sender = "";
    smsHistory[i].message = "";
    smsHistory[i].timestamp = "";
  }
  smsHistoryIndex = 0;
}

// 添加来电到历史记录（SPIFFS 存储）
void addCallToHistory(const char* caller, const char* timestamp) {
  statsIncrementCallsReceived();
  appendHistoryRecord("/calls.txt", caller, nullptr, timestamp, false);
  saveStats();
}

// 获取来电历史（返回 JSON 数组字符串，使用固定容量环形缓冲区避免一次性把整个文件读入内存）(M6, M5)
String getCallHistory() { return readHistoryJson("/calls.txt", false); }

HistoryCleanupResult cleanupSmsHistory() {
  return cleanupHistoryFile("/sms.txt");
}

HistoryCleanupResult cleanupCallHistory() {
  return cleanupHistoryFile("/calls.txt");
}

// 清空来电历史记录
void clearCallHistory() {
  if (SPIFFS.exists("/calls.txt")) {
    SPIFFS.remove("/calls.txt");
  }

  File f = SPIFFS.open("/calls.txt", "w");
  if (f) {
    f.close();
  }
}

// 标准化号码（去除 +、空格、-，以及开头的国家区号 86）
String normalizePhoneNumber(const String& rawNumber) {
  String num = rawNumber;
  num.trim();
  
  // 移除常见的特殊字符
  num.replace("+", "");
  num.replace(" ", "");
  num.replace("-", "");
  num.replace("(", "");
  num.replace(")", "");
  
  // 如果以 86 开头且长度超过 11 位，去掉 86（中国区号）
  if (num.startsWith("86") && num.length() > 11) {
    num = num.substring(2);
  }
  
  return num;
}

// 检查两个号码是否匹配（支持模糊匹配）
bool phoneNumbersMatch(const String& senderNum, const String& filterNum) {
  // 标准化两个号码
  String sender = normalizePhoneNumber(senderNum);
  String filter = normalizePhoneNumber(filterNum);

  // 空输入或仅含 +、-、括号等的条目不是有效号码；endsWith("") 会误匹配全部。
  if (sender.length() == 0 || filter.length() == 0) return false;
  
  // 精确匹配
  if (sender == filter) return true;
  
  // 后缀匹配：如果过滤号码是发送者号码的后缀（处理不同格式的区号）
  // 例如：发送者 8613800138000，过滤项 13800138000 -> 匹配
  if (sender.length() > filter.length() && sender.endsWith(filter)) return true;
  
  // 反向后缀匹配：如果发送者号码是过滤号码的后缀
  // 例如：发送者 13800138000，过滤项 8613800138000 -> 匹配
  if (filter.length() > sender.length() && filter.endsWith(sender)) return true;
  
  return false;
}

// 检查号码是否被过滤；仅控制转发，短信仍由调用者先入历史再过滤。
bool isNumberFiltered(const char* number) {
  if (!config.filterEnabled) return false;
  
  String senderNum = String(number);
  senderNum.trim();
  
  // 将 filterList 按逗号分割，逐个比较
  String list = config.filterList;
  list.replace(" ", ""); // 移除空格
  
  bool found = false;
  int startIdx = 0;
  while (startIdx < (int)list.length()) {
    int commaIdx = list.indexOf(',', startIdx);
    if (commaIdx < 0) commaIdx = list.length();
    
    String filterItem = list.substring(startIdx, commaIdx);
    filterItem.trim();
    
    if (filterItem.length() > 0 && phoneNumbersMatch(senderNum, filterItem)) {
      found = true;
      Serial.printf("号码匹配: %s ~ %s\n", senderNum.c_str(), filterItem.c_str());
      break;
    }
    startIdx = commaIdx + 1;
  }
  
  if (config.filterIsWhitelist) {
    return !found; // 白名单：未找到则过滤
  } else {
    return found; // 黑名单：找到则过滤
  }
}

// 检查短信内容是否被过滤（关键词匹配）
bool isContentFiltered(const char* content) {
  if (!config.contentFilterEnabled) return false;
  
  String text = String(content);
  text.toLowerCase();  // 转为小写进行不区分大小写匹配
  
  // 将关键词列表按逗号分割，逐个搜索
  String list = config.contentFilterList;
  
  bool found = false;
  int startIdx = 0;
  while (startIdx < (int)list.length()) {
    int commaIdx = list.indexOf(',', startIdx);
    if (commaIdx < 0) commaIdx = list.length();
    
    String keyword = list.substring(startIdx, commaIdx);
    keyword.trim();
    keyword.toLowerCase();  // 关键词也转小写
    
    if (keyword.length() > 0 && text.indexOf(keyword) >= 0) {
      found = true;
      Serial.printf("关键词匹配: '%s' 在内容中找到\n", keyword.c_str());
      break;
    }
    startIdx = commaIdx + 1;
  }
  
  if (config.contentFilterIsWhitelist) {
    return !found; // 白名单：未找到关键词则过滤（只转发包含关键词的）
  } else {
    return found; // 黑名单：找到关键词则过滤（拦截包含关键词的）
  }
}

// 保存统计数据
void saveStats() {
  if (statsMutex == nullptr) initStatsLock();
  if (statsMutex != nullptr &&
      xSemaphoreTake(statsMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    Serial.println("统计锁获取超时，跳过本次统计保存");
    return;
  }

  preferences.begin("sms_stats", false);
  preferences.putULong("received", stats.smsReceived.load());
  preferences.putULong("sent", stats.smsSent.load());
  preferences.putULong("calls", stats.callsReceived.load());
  preferences.putULong("pushOk", stats.pushSuccess.load());
  preferences.putULong("pushFail", stats.pushFailed.load());
  preferences.putULong("boots", stats.bootCount.load());
  preferences.putULong("brownout", stats.brownoutResets.load());
  preferences.putULong("poweron", stats.powerOnResets.load());
  preferences.putULong("wdt", stats.watchdogResets.load());
  preferences.putULong("swreset", stats.softwareResets.load());
  preferences.putULong("panic", stats.panicResets.load());
  preferences.end();

  if (statsMutex != nullptr) xSemaphoreGive(statsMutex);
}

// 加载统计数据
void loadStats() {
  if (statsMutex == nullptr) initStatsLock();
  if (statsMutex != nullptr &&
      xSemaphoreTake(statsMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    Serial.println("统计锁获取超时，跳过本次统计加载");
    return;
  }

  preferences.begin("sms_stats", true);
  stats.smsReceived.store(preferences.getULong("received", 0));
  stats.smsSent.store(preferences.getULong("sent", 0));
  stats.callsReceived.store(preferences.getULong("calls", 0));
  stats.pushSuccess.store(preferences.getULong("pushOk", 0));
  stats.pushFailed.store(preferences.getULong("pushFail", 0));
  stats.bootCount.store(preferences.getULong("boots", 0) + 1);
  stats.brownoutResets.store(preferences.getULong("brownout", 0));
  stats.powerOnResets.store(preferences.getULong("poweron", 0));
  stats.watchdogResets.store(preferences.getULong("wdt", 0));
  stats.softwareResets.store(preferences.getULong("swreset", 0));
  stats.panicResets.store(preferences.getULong("panic", 0));
  preferences.end();

  lastResetReasonCode = esp_reset_reason();
  lastResetReasonText = resetReasonToString(lastResetReasonCode);
  lastResetPowerSuspected = isPowerRelatedReset(lastResetReasonCode);
  loadInterruptedOperationDiagnostic();

  if (lastResetReasonCode == ESP_RST_POWERON) stats.powerOnResets++;
  else if (lastResetReasonCode == ESP_RST_BROWNOUT) stats.brownoutResets++;
  else if (lastResetReasonCode == ESP_RST_TASK_WDT || lastResetReasonCode == ESP_RST_INT_WDT) stats.watchdogResets++;
  else if (lastResetReasonCode == ESP_RST_SW) stats.softwareResets++;
  else if (lastResetReasonCode == ESP_RST_PANIC) stats.panicResets++;

  // 保存更新后的启动次数和重启分类统计
  preferences.begin("sms_stats", false);
  preferences.putULong("boots", stats.bootCount.load());
  preferences.putULong("brownout", stats.brownoutResets.load());
  preferences.putULong("poweron", stats.powerOnResets.load());
  preferences.putULong("wdt", stats.watchdogResets.load());
  preferences.putULong("swreset", stats.softwareResets.load());
  preferences.putULong("panic", stats.panicResets.load());
  preferences.end();

  if (statsMutex != nullptr) xSemaphoreGive(statsMutex);

  if (lastInterruptedOperation.valid) {
    Serial.printf(
      "复位前未完成操作: %s/%s, uptime=%lus, heap=%u, minHeap=%u, maxAlloc=%u\n",
      lastInterruptedOperation.operation.c_str(),
      lastInterruptedOperation.stage.c_str(),
      lastInterruptedOperation.uptimeSeconds,
      lastInterruptedOperation.freeHeap,
      lastInterruptedOperation.minFreeHeap,
      lastInterruptedOperation.maxAllocHeap
    );
  }
}

// 重置统计数据，可选择保留启动次数
void resetStats(bool preserveBootCount) {
  if (statsMutex == nullptr) initStatsLock();
  if (statsMutex != nullptr &&
      xSemaphoreTake(statsMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    Serial.println("统计锁获取超时，跳过本次重置统计");
    return;
  }

  unsigned long bootCount = preserveBootCount ? stats.bootCount.load() : 0;
  unsigned long brownoutResets = preserveBootCount ? stats.brownoutResets.load() : 0;
  unsigned long powerOnResets = preserveBootCount ? stats.powerOnResets.load() : 0;
  unsigned long watchdogResets = preserveBootCount ? stats.watchdogResets.load() : 0;
  unsigned long softwareResets = preserveBootCount ? stats.softwareResets.load() : 0;
  unsigned long panicResets = preserveBootCount ? stats.panicResets.load() : 0;

  stats.smsReceived.store(0);
  stats.smsSent.store(0);
  stats.callsReceived.store(0);
  stats.pushSuccess.store(0);
  stats.pushFailed.store(0);
  stats.bootCount.store(bootCount);
  stats.brownoutResets.store(brownoutResets);
  stats.powerOnResets.store(powerOnResets);
  stats.watchdogResets.store(watchdogResets);
  stats.softwareResets.store(softwareResets);
  stats.panicResets.store(panicResets);

  if (statsMutex != nullptr) xSemaphoreGive(statsMutex);

  saveStats();
}