/*
 * config.ino - 配置相关函数实现
 */

#include "push_service.h"

// 短信历史和统计的全局变量定义
SmsRecord smsHistory[MAX_SMS_HISTORY];
int smsHistoryIndex = 0;
Statistics stats = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
esp_reset_reason_t lastResetReasonCode = ESP_RST_UNKNOWN;
String lastResetReasonText = "未知";
bool lastResetPowerSuspected = false;

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

// 保存配置到 NVS
void saveConfig() {
  preferences.begin("sms_config", false);
  
  // 喂狗防止超时
  esp_task_wdt_reset();
  yield();
  
  // 保存 WiFi 配置
  for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
    String prefix = "wifi" + String(i);
    preferences.putString((prefix + "ssid").c_str(), config.wifiNetworks[i].ssid);
    preferences.putString((prefix + "pass").c_str(), config.wifiNetworks[i].password);
    preferences.putBool((prefix + "en").c_str(), config.wifiNetworks[i].enabled);
    yield();  // 让出 CPU
  }
  
  esp_task_wdt_reset();
  
  preferences.putString("smtpServer", config.smtpServer);
  preferences.putInt("smtpPort", config.smtpPort);
  preferences.putString("smtpUser", config.smtpUser);
  preferences.putString("smtpPass", config.smtpPass);
  preferences.putString("smtpSendTo", config.smtpSendTo);
  preferences.putBool("smtpEn", config.emailEnabled);
  preferences.putString("webUser", config.webUser);
  preferences.putString("webPass", config.webPass);
  
  esp_task_wdt_reset();
  yield();
  
  // 保存推送通道配置
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String prefix = "push" + String(i);
    preferences.putBool((prefix + "en").c_str(), config.pushChannels[i].enabled);
    preferences.putUChar((prefix + "type").c_str(), (uint8_t)config.pushChannels[i].type);
    preferences.putString((prefix + "url").c_str(), config.pushChannels[i].url);
    preferences.putString((prefix + "name").c_str(), config.pushChannels[i].name);
    preferences.putString((prefix + "k1").c_str(), config.pushChannels[i].key1);
    preferences.putString((prefix + "k2").c_str(), config.pushChannels[i].key2);
    preferences.putString((prefix + "body").c_str(), config.pushChannels[i].customBody);
    esp_task_wdt_reset();
    yield();  // 让出 CPU
  }
  
  // 保存定时任务配置
  preferences.putBool("timerEn", config.timerEnabled);
  preferences.putInt("timerType", config.timerType);
  preferences.putInt("timerInt", config.timerInterval);
  preferences.putString("timerPhone", config.timerPhone);
  preferences.putString("timerMsg", config.timerMessage);
  
  esp_task_wdt_reset();
  yield();
  
  // 保存 MQTT 配置
  preferences.putBool("mqttEn", config.mqttEnabled);
  preferences.putBool("mqttCtrlOnly", config.mqttControlOnly);
  preferences.putString("mqttServer", config.mqttServer);
  preferences.putInt("mqttPort", config.mqttPort);
  preferences.putString("mqttUser", config.mqttUser);
  preferences.putString("mqttPass", config.mqttPass);
  preferences.putString("mqttPrefix", config.mqttPrefix);
  
  // 保存 HA 自动发现配置
  preferences.putBool("mqttHaDisc", config.mqttHaDiscovery);
  preferences.putString("mqttHaPre", config.mqttHaPrefix);
  
  esp_task_wdt_reset();
  
  // 保存黑白名单配置（号码过滤）
  preferences.putBool("filterEn", config.filterEnabled);
  preferences.putBool("filterWL", config.filterIsWhitelist);
  preferences.putString("filterList", config.filterList);
  
  // 保存内容关键词过滤配置
  preferences.putBool("cfEn", config.contentFilterEnabled);
  preferences.putBool("cfWL", config.contentFilterIsWhitelist);
  preferences.putString("cfList", config.contentFilterList);
  
  preferences.end();
  esp_task_wdt_reset();
  Serial.println("配置已保存");
}

// 从 NVS 加载配置
void loadConfig() {
  preferences.begin("sms_config", true);
  
  // 加载 WiFi 配置（首次使用时从 wifi_config.h 读取默认值）
  for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
    String prefix = "wifi" + String(i);
    if (i == 0) {
      // 第一个网络默认使用 wifi_config.h 中的配置
      config.wifiNetworks[i].ssid = preferences.getString((prefix + "ssid").c_str(), WIFI_SSID);
      config.wifiNetworks[i].password = preferences.getString((prefix + "pass").c_str(), WIFI_PASS);
      config.wifiNetworks[i].enabled = preferences.getBool((prefix + "en").c_str(), true);
    } else {
      config.wifiNetworks[i].ssid = preferences.getString((prefix + "ssid").c_str(), "");
      config.wifiNetworks[i].password = preferences.getString((prefix + "pass").c_str(), "");
      config.wifiNetworks[i].enabled = preferences.getBool((prefix + "en").c_str(), false);
    }
  }
  
  config.smtpServer = preferences.getString("smtpServer", "");
  config.smtpPort = preferences.getInt("smtpPort", 465);
  config.smtpUser = preferences.getString("smtpUser", "");
  config.smtpPass = preferences.getString("smtpPass", "");
  config.smtpSendTo = preferences.getString("smtpSendTo", "");
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
  timerIntervalSec = (unsigned long)config.timerInterval * 24UL * 60UL * 60UL;
  
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

// 初始化 SPIFFS
void initSmsStorage() {
  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS 初始化失败");
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
    Serial.println("历史清洗失败：无法替换原文件");
    SPIFFS.remove(tempPath);
    result.kept = 0;
    result.removed = 0;
  }

  return result;
}

// 添加短信到历史记录（SPIFFS 存储）
void addSmsToHistory(const char* sender, const char* message, const char* timestamp) {
  stats.smsReceived++;
  
  // 构建 JSON 行
  String safeTimestamp = jsonEscape(String(timestamp));
  String safeSender = jsonEscape(String(sender));
  String safeMessage = jsonEscape(String(message).substring(0, 200));
  String line = "{\"t\":\"" + safeTimestamp + "\",\"s\":\"" + safeSender + "\",\"m\":\"" + safeMessage + "\"}\n";

  // 检查文件大小，超过 50KB 则清理旧数据
  File f = SPIFFS.open("/sms.txt", "r");
  size_t fileSize = f ? f.size() : 0;
  f.close();
  
  if (fileSize > 50000) {
    // 读取后半部分保留
    f = SPIFFS.open("/sms.txt", "r");
    f.seek(fileSize / 2);
    String remaining = f.readString();
    f.close();
    // 从第一个换行开始保留
    int nl = remaining.indexOf('\n');
    if (nl > 0) remaining = remaining.substring(nl + 1);
    f = SPIFFS.open("/sms.txt", "w");
    f.print(remaining);
    f.close();
  }
  
  // 追加新短信
  f = SPIFFS.open("/sms.txt", "a");
  if (f) {
    f.print(line);
    f.close();
  }
  
  // 保存统计数据
  saveStats();
}

// 获取短信历史（返回 JSON 数组字符串）
String getSmsHistory() {
  File f = SPIFFS.open("/sms.txt", "r");
  if (!f) return "[]";
  
  String result = "[";
  bool first = true;
  
  // 读取所有行并倒序排列
  std::vector<String> lines;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.length() > 10 && isLikelyValidHistoryJsonLine(line)) {
      lines.push_back(line);
    } else if (line.length() > 0) {
      Serial.println("跳过损坏的短信历史行");
    }
  }
  f.close();
  
  // 倒序输出最近100条
  int count = 0;
  for (int i = lines.size() - 1; i >= 0 && count < 100; i--, count++) {
    if (!first) result += ",";
    first = false;
    result += lines[i];
  }
  result += "]";
  return result;
}

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
  stats.callsReceived++;

  String safeTimestamp = jsonEscape(String(timestamp));
  String safeCaller = jsonEscape(String(caller));

  String line = "{\"t\":\"" + safeTimestamp + "\",\"n\":\"" + safeCaller + "\"}\n";

  File f = SPIFFS.open("/calls.txt", "r");
  size_t fileSize = f ? f.size() : 0;
  f.close();

  if (fileSize > 30000) {
    f = SPIFFS.open("/calls.txt", "r");
    f.seek(fileSize / 2);
    String remaining = f.readString();
    f.close();
    int nl = remaining.indexOf('\n');
    if (nl > 0) remaining = remaining.substring(nl + 1);
    f = SPIFFS.open("/calls.txt", "w");
    f.print(remaining);
    f.close();
  }

  f = SPIFFS.open("/calls.txt", "a");
  if (f) {
    f.print(line);
    f.close();
  }

  saveStats();
}

// 获取来电历史（返回 JSON 数组字符串）
String getCallHistory() {
  File f = SPIFFS.open("/calls.txt", "r");
  if (!f) return "[]";

  String result = "[";
  bool first = true;
  std::vector<String> lines;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.length() > 8 && isLikelyValidHistoryJsonLine(line)) {
      lines.push_back(line);
    } else if (line.length() > 0) {
      Serial.println("跳过损坏的来电历史行");
    }
  }
  f.close();

  int count = 0;
  for (int i = lines.size() - 1; i >= 0 && count < 100; i--, count++) {
    if (!first) result += ",";
    first = false;
    result += lines[i];
  }
  result += "]";
  return result;
}

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

// 检查号码是否被过滤
bool isNumberFiltered(const char* number) {
  if (!config.filterEnabled) return false;
  if (config.filterList.length() == 0) return false;
  
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
  if (config.contentFilterList.length() == 0) return false;
  
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
  preferences.begin("sms_stats", false);
  preferences.putULong("received", stats.smsReceived);
  preferences.putULong("sent", stats.smsSent);
  preferences.putULong("calls", stats.callsReceived);
  preferences.putULong("pushOk", stats.pushSuccess);
  preferences.putULong("pushFail", stats.pushFailed);
  preferences.putULong("boots", stats.bootCount);
  preferences.putULong("brownout", stats.brownoutResets);
  preferences.putULong("poweron", stats.powerOnResets);
  preferences.putULong("wdt", stats.watchdogResets);
  preferences.putULong("swreset", stats.softwareResets);
  preferences.putULong("panic", stats.panicResets);
  preferences.end();
}

// 加载统计数据
void loadStats() {
  preferences.begin("sms_stats", true);
  stats.smsReceived = preferences.getULong("received", 0);
  stats.smsSent = preferences.getULong("sent", 0);
  stats.callsReceived = preferences.getULong("calls", 0);
  stats.pushSuccess = preferences.getULong("pushOk", 0);
  stats.pushFailed = preferences.getULong("pushFail", 0);
  stats.bootCount = preferences.getULong("boots", 0) + 1;
  stats.brownoutResets = preferences.getULong("brownout", 0);
  stats.powerOnResets = preferences.getULong("poweron", 0);
  stats.watchdogResets = preferences.getULong("wdt", 0);
  stats.softwareResets = preferences.getULong("swreset", 0);
  stats.panicResets = preferences.getULong("panic", 0);
  preferences.end();

  lastResetReasonCode = esp_reset_reason();
  lastResetReasonText = resetReasonToString(lastResetReasonCode);
  lastResetPowerSuspected = isPowerRelatedReset(lastResetReasonCode);

  if (lastResetReasonCode == ESP_RST_POWERON) stats.powerOnResets++;
  else if (lastResetReasonCode == ESP_RST_BROWNOUT) stats.brownoutResets++;
  else if (lastResetReasonCode == ESP_RST_TASK_WDT || lastResetReasonCode == ESP_RST_INT_WDT) stats.watchdogResets++;
  else if (lastResetReasonCode == ESP_RST_SW) stats.softwareResets++;
  else if (lastResetReasonCode == ESP_RST_PANIC) stats.panicResets++;

  // 保存更新后的启动次数和重启分类统计
  preferences.begin("sms_stats", false);
  preferences.putULong("boots", stats.bootCount);
  preferences.putULong("brownout", stats.brownoutResets);
  preferences.putULong("poweron", stats.powerOnResets);
  preferences.putULong("wdt", stats.watchdogResets);
  preferences.putULong("swreset", stats.softwareResets);
  preferences.putULong("panic", stats.panicResets);
  preferences.end();
}

// 重置统计数据，可选择保留启动次数
void resetStats(bool preserveBootCount) {
  unsigned long bootCount = preserveBootCount ? stats.bootCount : 0;
  unsigned long brownoutResets = preserveBootCount ? stats.brownoutResets : 0;
  unsigned long powerOnResets = preserveBootCount ? stats.powerOnResets : 0;
  unsigned long watchdogResets = preserveBootCount ? stats.watchdogResets : 0;
  unsigned long softwareResets = preserveBootCount ? stats.softwareResets : 0;
  unsigned long panicResets = preserveBootCount ? stats.panicResets : 0;

  stats.smsReceived = 0;
  stats.smsSent = 0;
  stats.callsReceived = 0;
  stats.pushSuccess = 0;
  stats.pushFailed = 0;
  stats.bootCount = bootCount;
  stats.brownoutResets = brownoutResets;
  stats.powerOnResets = powerOnResets;
  stats.watchdogResets = watchdogResets;
  stats.softwareResets = softwareResets;
  stats.panicResets = panicResets;

  saveStats();
}