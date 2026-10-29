/*
 * web_handlers.ino - Web 服务器处理函数实现
 */
#include "web_stream.h"

// 检查 HTTP Basic 认证
bool checkAuth() {
  if (!server.authenticate(config.webUser.c_str(), config.webPass.c_str())) {
    server.requestAuthentication(BASIC_AUTH, "SMS Forwarding", "请输入管理员账号密码");
    return false;
  }
  return true;
}

// 发送 AT 命令并获取响应
String sendATCommand(const char* cmd, unsigned long timeout) {
  Serial.printf("[耗时] START AT命令: %s, timeout=%lums\n", cmd, timeout);
  while (Serial1.available()) Serial1.read();
  Serial1.println(cmd);
  
  unsigned long start = millis();
  String resp;
  resp.reserve(128); // 预分配响应缓冲区，避免频繁拼接再分配 (M5)
  // 注释 (M1): 无符号整型差值 (millis() - start) 溢出回绕安全
  while (millis() - start < timeout) {
    esp_task_wdt_reset();
    yield();
    delay(1);
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (resp.indexOf("OK") >= 0 || resp.indexOf("ERROR") >= 0) {
        delay(50);  // 等待剩余数据
        while (Serial1.available()) resp += (char)Serial1.read();
        Serial.printf("[耗时] END AT命令: %s, elapsed=%lums, result=%s, respLen=%u\n",
                      cmd,
                      millis() - start,
                      resp.indexOf("OK") >= 0 ? "OK" : "ERROR",
                      resp.length());
        return resp;
      }
    }
  }
  Serial.printf("[耗时] TIMEOUT AT命令: %s, elapsed=%lums, respLen=%u\n", cmd, millis() - start, resp.length());
  return resp;
}

// AT 命令并等待 OK
bool sendATandWaitOK(const char* cmd, unsigned long timeout) {
  Serial.printf("[耗时] START AT等待OK: %s, timeout=%lums\n", cmd, timeout);
  while (Serial1.available()) Serial1.read();
  Serial1.println(cmd);
  unsigned long start = millis();
  String resp;
  resp.reserve(64);
  // 注释 (M1): 无符号整型差值 (millis() - start) 溢出回绕安全
  while (millis() - start < timeout) {
    esp_task_wdt_reset();
    yield();
    delay(1);
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (resp.indexOf("OK") >= 0) {
        Serial.printf("[耗时] END AT等待OK: %s, elapsed=%lums, result=OK, respLen=%u\n", cmd, millis() - start, resp.length());
        return true;
      }
      if (resp.indexOf("ERROR") >= 0) {
        Serial.printf("[耗时] END AT等待OK: %s, elapsed=%lums, result=ERROR, respLen=%u\n", cmd, millis() - start, resp.length());
        return false;
      }
    }
  }
  Serial.printf("[耗时] TIMEOUT AT等待OK: %s, elapsed=%lums, respLen=%u\n", cmd, millis() - start, resp.length());
  return false;
}

// 等待 CGATT 附着
bool waitCGATT1() {
  Serial1.println("AT+CGATT?");
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < 2000) {
    esp_task_wdt_reset();
    yield();
    delay(1);
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (resp.indexOf("+CGATT: 1") >= 0) return true;
      if (resp.indexOf("+CGATT: 0") >= 0) return false;
    }
  }
  return false;
}

// 重启模组
void resetModule() {
  Serial.println("正在重启模组...");
  Serial1.println("AT+CFUN=1,1");
  delay(3000);
}

// LED 闪烁
void blink_short(unsigned long gap_time) {
  digitalWrite(LED_BUILTIN, LOW);
  delay(50);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(gap_time);
}

// WebServer::sendContent 返回 void，无法检查短写。这里使用相同的 chunk
// 格式，但用栈上的长度头并检查每次写入；HTTP/1.0 使用连接关闭定界。
static bool sendWebChunk(const char* data, size_t length) {
  if (!length) return true;
  esp_task_wdt_reset();
  yield();
  auto& client = server.client();
  if (!client.connected()) return false;
  bool chunked = server.version() != "HTTP/1.0";
  if (chunked) {
    char header[16];
    int headerLength = snprintf(header, sizeof(header), "%x\r\n", (unsigned int)length);
    if (client.write((const uint8_t*)header, headerLength) != (size_t)headerLength) {
      client.stop();
      return false;
    }
  }
  if (client.write((const uint8_t*)data, length) != length ||
      (chunked && client.write((const uint8_t*)"\r\n", 2) != 2)) {
    client.stop();
    return false;
  }
  esp_task_wdt_reset();
  yield();
  return true;
}

// 处理配置页面请求 —— chunked streaming 版，峰值内存大幅降低
void handleRoot() {
  if (!checkAuth()) return;

  // 给网络栈保留余量；同时检查碎片化后最大的连续空闲块。
  if (ESP.getFreeHeap() < 16384 || ESP.getMaxAllocHeap() < 4096) {
    Serial.printf("首页响应内存不足，free=%u max=%u min=%u\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());
    server.send(503, "text/plain; charset=utf-8", "首页暂时不可用：设备内存不足，请稍后重试");
    return;
  }

  // 只保留短小的运行状态字符串；配置值直接引用，不复制长列表/模板。
  wifi_mode_t wifiMode = WiFi.getMode();
  bool apActive = (wifiMode & WIFI_AP) != 0;
  bool staConnected = WiFi.status() == WL_CONNECTED;
  String displayIp = apActive ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  String wifiModeBadge = apActive ? "<span class=\"badge b-warn\">AP模式</span>" : "<span class=\"badge b-ok\">STA模式</span>";
  String wifiInfoLine = "";
  if (apActive) {
    wifiInfoLine = "热点名: SMS-Forwarder-AP<br>热点IP: " + WiFi.softAPIP().toString();
    if (staConnected) { wifiInfoLine += "<br>上游WiFi: " + WiFi.SSID(); }
  } else if (staConnected) {
    wifiInfoLine = "已连接WiFi: " + WiFi.SSID() + "<br>设备IP: " + WiFi.localIP().toString();
  } else {
    wifiInfoLine = "WiFi未连接";
  }

  String mqttStatusText = config.mqttEnabled ? (mqttClient.connected() ? "已连接" : "未连接") : "未启用";

  unsigned long remainSec = 0;
  if (config.timerEnabled && timerIntervalSec > 0) {
    unsigned long elapsedSec = (millis() - lastTimerExec) / 1000;
    if (elapsedSec < timerIntervalSec) {
      remainSec = timerIntervalSec - elapsedSec;
    }
  }

  // ---- 定位 htmlPage 中的 3 个大占位符，将模板拆成 4 段 ----
  // 顺序: ... %COMMON_CSS% ... %WIFI_NETWORKS% ... %PUSH_CHANNELS% ...
  const char* pCSS = strstr(htmlPage, "%COMMON_CSS%");
  const char* pWIFI = strstr(htmlPage, "%WIFI_NETWORKS%");
  const char* pPUSH = strstr(htmlPage, "%PUSH_CHANNELS%");

  const char* pTOPICS = strstr(htmlPage, "%MQTT_TOPICS%");
  if (!pCSS || !pWIFI || !pPUSH || !pTOPICS ||
      !(pCSS < pTOPICS && pTOPICS < pWIFI && pWIFI < pPUSH) ||
      strstr(pCSS + 1, "%COMMON_CSS%") || strstr(pWIFI + 1, "%WIFI_NETWORKS%") ||
      strstr(pPUSH + 1, "%PUSH_CHANNELS%") || strstr(pTOPICS + 1, "%MQTT_TOPICS%")) {
    Serial.println("handleRoot: 模板占位符定位失败");
    server.send(503, "text/plain; charset=utf-8", "首页暂时不可用：模板错误");
    return;
  }

  // 开始 chunked transfer
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=utf-8", "");
  WebStream out(sendWebChunk);

  if (!out.write(htmlPage, pCSS - htmlPage) || !out.text(commonCss)) return;

  // ==== 段2: %COMMON_CSS% 之后 → %WIFI_NETWORKS% 之前 ====
  // 跳过 "%COMMON_CSS%" 占位符本身 (12 字符)
  {
    const char* seg2Start = pCSS + strlen("%COMMON_CSS%");
    // 本段包含的占位符: 概览页+控制页+配置页(WiFi之前)的所有占位符
    const WebTemplateValue pairs[] = {
      {"%IP%", displayIp.c_str()},
      {"%WIFI_MODE_BADGE%", wifiModeBadge.c_str()},
      {"%WIFI_INFO_LINE%", wifiInfoLine.c_str()},
      {"%MQTT_STATUS%", mqttStatusText.c_str()},
    };
    if (!out.render(seg2Start, pTOPICS - seg2Start, pairs, sizeof(pairs) / sizeof(pairs[0]))) return;
    if (config.mqttEnabled && config.mqttServer.length() > 0) {
      if (!out.text(mqttTopicStatus.c_str()) || !out.text("<br>") ||
          !out.text(mqttTopicSmsReceived.c_str()) || !out.text("<br>") ||
          !out.text(mqttTopicCallReceived.c_str())) return;
    } else if (!out.text("未配置")) return;
    const char* afterTopics = pTOPICS + strlen("%MQTT_TOPICS%");
    if (!out.render(afterTopics, pWIFI - afterTopics, pairs, sizeof(pairs) / sizeof(pairs[0]))) return;
  }

  // ==== 生成 WiFi 网络 HTML 并发送（替代 %WIFI_NETWORKS%） ====
  {
    for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
      char idx[12], number[12];
      snprintf(idx, sizeof(idx), "%d", i);
      snprintf(number, sizeof(number), "%d", i + 1);
      String currentSsid = WiFi.SSID();
      bool isCurrent = config.wifiNetworks[i].ssid.length() > 0 && config.wifiNetworks[i].ssid == currentSsid;
      static const char wifiTemplate[] = R"html(<div style="padding:10px;background:#f8fafc;border-radius:8px;margin-bottom:8px;border:1px solid %BORDER%"><div class="sw-row" onclick="wfTog(%IDX%)"><span style="font-weight:600">网络 %NUMBER%%CURRENT%</span><div id="wfs%IDX%" class="sw %EN_SW%"></div><input type="hidden" id="wfe%IDX%" name="wifi%IDX%en" value="%EN_VAL%"></div><div class="grid-2" style="margin-top:8px"><div class="fg" style="margin-bottom:0"><label>SSID</label><input name="wifi%IDX%ssid" value="%SSID%" placeholder="WiFi名称"></div><div class="fg" style="margin-bottom:0"><label>密码</label><input name="wifi%IDX%pass" type="password" placeholder="%PASS_HINT%"></div></div></div>)html";
      const WebTemplateValue pairs[] = {
        {"%IDX%", idx}, {"%NUMBER%", number},
        {"%BORDER%", isCurrent ? "var(--success)" : "#e2e8f0"},
        {"%CURRENT%", isCurrent ? " <span class=\"badge b-ok\">当前</span>" : ""},
        {"%EN_SW%", config.wifiNetworks[i].enabled ? "on" : ""},
        {"%EN_VAL%", config.wifiNetworks[i].enabled ? "true" : "false"},
        {"%SSID%", config.wifiNetworks[i].ssid.c_str()},
        {"%PASS_HINT%", config.wifiNetworks[i].password.length() > 0 ? "（已保存，留空则保留）" : "WiFi密码"},
      };
      if (!out.render(wifiTemplate, sizeof(wifiTemplate) - 1, pairs, sizeof(pairs) / sizeof(pairs[0]))) return;
    }
  }

  // ==== 段3: %WIFI_NETWORKS% 之后 → %PUSH_CHANNELS% 之前 ====
  {
    const char* seg3Start = pWIFI + strlen("%WIFI_NETWORKS%");
    size_t seg3Len = pPUSH - seg3Start;
    char smtpPort[12], mqttPort[12];
    snprintf(smtpPort, sizeof(smtpPort), "%d", config.smtpPort);
    snprintf(mqttPort, sizeof(mqttPort), "%d", config.mqttPort);

    // 本段包含的占位符: Web管理/邮箱/MQTT 配置
    const WebTemplateValue pairs[] = {
      {"%WEB_USER%", config.webUser.c_str()},
      {"%WEB_PASS%", config.webPass.c_str()},
      {"%SMTP_EN_VAL%", config.emailEnabled ? "true" : "false"},
      {"%SMTP_EN_SW%", config.emailEnabled ? "on" : ""},
      {"%SMTP_DISP%", config.emailEnabled ? "block" : "none"},
      {"%SMTP_SERVER%", config.smtpServer.c_str()},
      {"%SMTP_PORT%", smtpPort},
      {"%SMTP_USER%", config.smtpUser.c_str()},
      {"%SMTP_PASS%", config.smtpPass.c_str()},
      {"%SMTP_SEND_TO%", config.smtpSendTo.c_str()},
      {"%MQTT_EN_VAL%", config.mqttEnabled ? "true" : "false"},
      {"%MQTT_EN_SW%", config.mqttEnabled ? "on" : ""},
      {"%MQTT_DISP%", config.mqttEnabled ? "block" : "none"},
      {"%MQTT_SERVER%", config.mqttServer.c_str()},
      {"%MQTT_PORT%", mqttPort},
      {"%MQTT_USER%", config.mqttUser.c_str()},
      {"%MQTT_PASS%", config.mqttPass.c_str()},
      {"%MQTT_PREFIX%", config.mqttPrefix.c_str()},
      {"%MQTT_CO_VAL%", config.mqttControlOnly ? "true" : "false"},
      {"%MQTT_CO_SW%", config.mqttControlOnly ? "on" : ""},
      {"%MQTT_HA_VAL%", config.mqttHaDiscovery ? "true" : "false"},
      {"%MQTT_HA_SW%", config.mqttHaDiscovery ? "on" : ""},
      {"%MQTT_HA_DISP%", config.mqttHaDiscovery ? "block" : "none"},
      {"%MQTT_HA_PREFIX%", config.mqttHaPrefix.length() > 0 ? config.mqttHaPrefix.c_str() : "homeassistant"},
    };
    if (!out.render(seg3Start, seg3Len, pairs, sizeof(pairs) / sizeof(pairs[0]))) return;
  }

  // ==== 生成推送通道 HTML 并发送（替代 %PUSH_CHANNELS%） ====
  {
    for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
      char idx[12], number[12];
      snprintf(idx, sizeof(idx), "%d", i);
      snprintf(number, sizeof(number), "%d", i + 1);
      const PushChannel& channel = config.pushChannels[i];
      static const char pushTemplate[] = R"html(<div class="card" style="border:1px solid #e2e8f0;padding:12px;margin-bottom:8px;box-shadow:none"><div class="sw-row" onclick="chTog(%IDX%)"><span style="font-weight:600">通道 %NUMBER%</span><div id="chs%IDX%" class="sw %EN_SW%"></div><input type="hidden" id="che%IDX%" name="push%IDX%en" value="%EN_VAL%"></div><div style="font-size:0.85em;color:var(--primary);text-align:right;cursor:pointer" onclick="fd(%IDX%)">展开/收起 <span id="chi%IDX%" style="display:inline-block;transition:.2s">></span></div><div id="chb%IDX%" style="display:none;margin-top:12px;border-top:1px solid #f1f5f9;padding-top:12px"><div class="fg"><label>名称</label><input name="push%IDX%name" value="%NAME%"></div><div class="fg"><label>类型</label><select name="push%IDX%type" id="tp%IDX%" onchange="upd(%IDX%)"><option value="1"%SEL1%>POST JSON</option><option value="2"%SEL2%>Bark</option><option value="3"%SEL3%>GET请求</option><option value="4"%SEL4%>自定义模板</option><option value="5"%SEL5%>Telegram Bot</option><option value="6"%SEL6%>企业微信</option><option value="7"%SEL7%>钉钉</option></select></div><div class="fg"><label>URL</label><input name="push%IDX%url" value="%URL%" placeholder="https://..."></div><div id="k1%IDX%" style="display:%K1_DISP%"><div class="fg"><label id="k1l%IDX%">%K1_LABEL%</label><input name="push%IDX%k1" value="%KEY1%" placeholder="%K1_HINT%"></div></div><div id="k2%IDX%" style="display:%K2_DISP%"><div class="fg"><label>Bark 分组 group (可选)</label><input name="push%IDX%k2" value="%KEY2%" placeholder="如 家庭短信 / 验证码"></div></div><div id="cf%IDX%" style="display:%CUSTOM_DISP%"><div class="fg"><label>Body模板</label><textarea name="push%IDX%body" rows="3">%BODY%</textarea></div></div></div></div>)html";
      const WebTemplateValue pairs[] = {
        {"%IDX%", idx}, {"%NUMBER%", number},
        {"%EN_SW%", channel.enabled ? "on" : ""},
        {"%EN_VAL%", channel.enabled ? "true" : "false"},
        {"%NAME%", channel.name.c_str()}, {"%URL%", channel.url.c_str()},
        {"%SEL1%", channel.type == PUSH_TYPE_POST_JSON ? " selected" : ""},
        {"%SEL2%", channel.type == PUSH_TYPE_BARK ? " selected" : ""},
        {"%SEL3%", channel.type == PUSH_TYPE_GET ? " selected" : ""},
        {"%SEL4%", channel.type == PUSH_TYPE_CUSTOM ? " selected" : ""},
        {"%SEL5%", channel.type == PUSH_TYPE_TELEGRAM ? " selected" : ""},
        {"%SEL6%", channel.type == PUSH_TYPE_WECOM ? " selected" : ""},
        {"%SEL7%", channel.type == PUSH_TYPE_DINGTALK ? " selected" : ""},
        {"%K1_DISP%", channel.type == PUSH_TYPE_TELEGRAM || channel.type == PUSH_TYPE_DINGTALK ? "block" : "none"},
        {"%K1_LABEL%", channel.type == PUSH_TYPE_TELEGRAM ? "Chat ID" : "加签密钥 (可选)"},
        {"%K1_HINT%", channel.type == PUSH_TYPE_TELEGRAM ? "如 123456789" : "SEC开头的密钥"},
        {"%KEY1%", channel.key1.c_str()}, {"%KEY2%", channel.key2.c_str()},
        {"%K2_DISP%", channel.type == PUSH_TYPE_BARK ? "block" : "none"},
        {"%CUSTOM_DISP%", channel.type == PUSH_TYPE_CUSTOM ? "block" : "none"},
        {"%BODY%", channel.customBody.c_str()},
      };
      if (!out.render(pushTemplate, sizeof(pushTemplate) - 1, pairs, sizeof(pairs) / sizeof(pairs[0]))) return;
    }
  }

  // ==== 段4: %PUSH_CHANNELS% 之后 → 模板结尾 ====
  // 包含：号码名单过滤、内容过滤、定时任务、JS 初始化脚本
  {
    const char* seg4Start = pPUSH + strlen("%PUSH_CHANNELS%");
    size_t seg4Len = strlen(seg4Start);
    char timerType[12], timerInterval[12], timerRemain[12];
    snprintf(timerType, sizeof(timerType), "%d", config.timerType);
    snprintf(timerInterval, sizeof(timerInterval), "%d", config.timerInterval);
    snprintf(timerRemain, sizeof(timerRemain), "%lu", remainSec);

    const WebTemplateValue pairs[] = {
      {"%FILTER_EN_VAL%", config.filterEnabled ? "true" : "false"},
      {"%FILTER_EN_BOOL%", config.filterEnabled ? "true" : "false"},
      {"%FILTER_WL_BOOL%", config.filterIsWhitelist ? "true" : "false"},
      {"%FILTER_WL_VAL%", config.filterIsWhitelist ? "true" : "false"},
      {"%FILTER_LIST%", config.filterList.c_str(), true},
      {"%CF_EN_VAL%", config.contentFilterEnabled ? "true" : "false"},
      {"%CF_EN_BOOL%", config.contentFilterEnabled ? "true" : "false"},
      {"%CF_WL_BOOL%", config.contentFilterIsWhitelist ? "true" : "false"},
      {"%CF_WL_VAL%", config.contentFilterIsWhitelist ? "true" : "false"},
      {"%CF_LIST%", config.contentFilterList.c_str(), true},
      {"%TIMER_EN_VAL%", config.timerEnabled ? "true" : "false"},
      {"%TIMER_EN_BOOL%", config.timerEnabled ? "true" : "false"},
      {"%TIMER_TP%", timerType},
      {"%TIMER_INT%", timerInterval},
      {"%TIMER_PH%", config.timerPhone.c_str()},
      {"%TIMER_MS%", config.timerMessage.c_str()},
      {"%TIMER_RM%", timerRemain},
    };
    if (!out.render(seg4Start, seg4Len, pairs, sizeof(pairs) / sizeof(pairs[0]))) return;
  }

  // ==== 结束 chunked transfer ====
  if (!out.flush()) return;
  server.sendContent("", 0);
  Serial.printf("首页流式响应完成，free=%u max=%u min=%u\n",
                ESP.getFreeHeap(), ESP.getMaxAllocHeap(), ESP.getMinFreeHeap());
}

// 处理工具箱页面请求 (重定向到首页)
void handleToolsPage() {
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

// 处理保存配置请求 (全量保存)
void handleSave() {
  if (!checkAuth()) return;
  
  // 喂狗防止超时
  esp_task_wdt_reset();
  yield();

  bool wifiChanged = false;

  // WiFi 网络配置
  for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
    String prefix = "wifi" + String(i);
    String newSsid = server.arg(prefix + "ssid");
    String newPass = server.arg(prefix + "pass");
    String newEn = server.arg(prefix + "en");
    bool nextEnabled = (newEn == "true");

    if (newSsid.length() > 0 && newSsid != config.wifiNetworks[i].ssid) {
      wifiChanged = true;
    }
    if (newPass.length() > 0 && newPass != config.wifiNetworks[i].password) {
      wifiChanged = true;
    }
    if (nextEnabled != config.wifiNetworks[i].enabled) {
      wifiChanged = true;
    }

    // 只有当 SSID 非空时才更新（避免清空已保存的配置）
    if (newSsid.length() > 0) {
      config.wifiNetworks[i].ssid = newSsid;
    }
    // 密码为空时保留原密码
    if (newPass.length() > 0) {
      config.wifiNetworks[i].password = newPass;
    }
    // enabled 状态始终更新
    config.wifiNetworks[i].enabled = nextEnabled;

    // 调试输出
    Serial.printf("WiFi%d: SSID=%s, Pass=%s, En=%d\n", 
      i, config.wifiNetworks[i].ssid.c_str(), 
      config.wifiNetworks[i].password.length() > 0 ? "***" : "(empty)",
      config.wifiNetworks[i].enabled);
  }
  
  esp_task_wdt_reset();
  yield();
  
  // 获取新的 Web 账号密码
  String newWebUser = server.arg("webUser");
  String newWebPass = server.arg("webPass");
  if (newWebUser.length() > 0 && newWebPass.length() > 0) {
    config.webUser = newWebUser;
    config.webPass = newWebPass;
  }
  
  // SMTP 配置
  config.emailEnabled = server.arg("smtpEn") == "true";
  String nextSmtpServer = server.arg("smtpServer");
  nextSmtpServer.trim();
  config.smtpServer = nextSmtpServer;
  config.smtpPort = server.arg("smtpPort").toInt();
  if (config.smtpPort <= 0) config.smtpPort = 465;
  String nextSmtpUser = server.arg("smtpUser");
  nextSmtpUser.trim();
  config.smtpUser = nextSmtpUser;
  String nextSmtpPass = server.arg("smtpPass");
  nextSmtpPass.trim();
  if (nextSmtpPass.length() > 0) {
    config.smtpPass = nextSmtpPass;
  }
  String nextSmtpSendTo = server.arg("smtpSendTo");
  nextSmtpSendTo.trim();
  config.smtpSendTo = nextSmtpSendTo;

  esp_task_wdt_reset();
  yield();
  
  // 推送通道配置
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String prefix = "push" + String(i);
    config.pushChannels[i].enabled = server.arg(prefix + "en") == "true";
    config.pushChannels[i].type = (PushType)server.arg(prefix + "type").toInt();
    config.pushChannels[i].url = server.arg(prefix + "url");
    config.pushChannels[i].name = server.arg(prefix + "name");
    config.pushChannels[i].key1 = server.arg(prefix + "k1"); // Telegram Chat ID
    config.pushChannels[i].key2 = server.arg(prefix + "k2"); // Bark group
    config.pushChannels[i].customBody = server.arg(prefix + "body");
  }
  
  esp_task_wdt_reset();
  yield();
  
  // MQTT 配置
  config.mqttEnabled = server.arg("mqttEn") == "true";
  config.mqttServer = server.arg("mqttServer");
  config.mqttPort = server.arg("mqttPort").toInt();
  if (config.mqttPort == 0) config.mqttPort = 1883;
  config.mqttUser = server.arg("mqttUser");
  config.mqttPass = server.arg("mqttPass");
  config.mqttPrefix = server.arg("mqttPrefix");
  if (config.mqttPrefix.length() == 0) config.mqttPrefix = "sms";
  config.mqttControlOnly = server.arg("mqttCtrlOnly") == "on" || server.arg("mqttCtrlOnly") == "true"; 
  
  // HA 自动发现配置
  config.mqttHaDiscovery = server.arg("mqttHaDiscovery") == "true";
  config.mqttHaPrefix = server.arg("mqttHaPrefix");
  if (config.mqttHaPrefix.length() == 0) config.mqttHaPrefix = "homeassistant";
  
  esp_task_wdt_reset();
  yield();
  
  // 黑白名单配置 (全量保存时也更新)
  if (server.hasArg("filterEn")) { // 只有当前端提交了这些字段才更新
      config.filterEnabled = server.arg("filterEn") == "true";
      config.filterIsWhitelist = server.arg("filterIsWhitelist") == "true";
      config.filterList = server.arg("filterList");
      config.filterList.replace("\r", "");
      config.filterList.replace("\n", ","); // 换行转逗号
  }

  // 定时任务配置 (全量保存时)
  if (server.hasArg("timerEn")) {
      config.timerEnabled = server.arg("timerEn") == "true";
      config.timerType = server.arg("timerType").toInt();
      config.timerInterval = server.arg("timerInterval").toInt();
      if (config.timerInterval < 1) config.timerInterval = 1;
      config.timerPhone = server.arg("timerPhone");
      config.timerMessage = server.arg("timerMessage");
      timerIntervalSec = (unsigned long)config.timerInterval * 24UL * 60UL * 60UL;
  }
  
  // 喂狗并保存配置
  esp_task_wdt_reset();
  yield();
  saveConfig();
  esp_task_wdt_reset();
  yield();
  configValid = isConfigValid();

  bool needRestart = wifiChanged || (WiFi.getMode() == WIFI_AP);

  // 返回成功响应，让前端询问用户是否重启
  String json = "{\"success\":true,\"message\":\"配置已保存\",\"needRestart\":" + String(needRestart ? "true" : "false") + "}";
  server.send(200, "application/json", json);
}

// 处理发送短信请求
void handleSendSms() {
  if (!checkAuth()) return;
  
  // 解析 JSON body (前端 postJ 发送 JSON 格式)
  String body = server.arg("plain");
  String phone = "";
  String content = "";
  
  // 简易 JSON 解析
  int phoneIdx = body.indexOf("\"phone\"");
  if (phoneIdx >= 0) {
    int colonIdx = body.indexOf(":", phoneIdx);
    int startQuote = body.indexOf("\"", colonIdx);
    int endQuote = body.indexOf("\"", startQuote + 1);
    if (startQuote >= 0 && endQuote > startQuote) {
      phone = body.substring(startQuote + 1, endQuote);
    }
  }
  
  int contentIdx = body.indexOf("\"content\"");
  if (contentIdx >= 0) {
    int colonIdx = body.indexOf(":", contentIdx);
    int startQuote = body.indexOf("\"", colonIdx);
    int endQuote = body.indexOf("\"", startQuote + 1);
    if (startQuote >= 0 && endQuote > startQuote) {
      content = body.substring(startQuote + 1, endQuote);
    }
  }
  
  phone.trim();
  content.trim();
  
  bool success = false;
  String resultMsg = "";
  
  if (phone.length() == 0) {
    resultMsg = "错误：请输入目标号码";
  } else if (content.length() == 0) {
    resultMsg = "错误：请输入短信内容";
  } else {
    Serial.println("网页端发送短信请求");
    Serial.println("目标号码: " + phone);
    Serial.println("短信内容: " + content);
    
    success = sendSMS(phone.c_str(), content.c_str());
    if (success) {
      stats.smsSent++;
      saveStats();
    }
    resultMsg = success ? "短信发送成功！" : "短信发送失败，请检查模组状态";
  }
  
  String json = "{";
  json += "\"success\":" + String(success ? "true" : "false") + ",";
  json += "\"message\":\"" + resultMsg + "\"";
  json += "}";
  
  server.send(200, "application/json", json);
}

// 处理定时任务配置保存 (API: /timer)
void handleTimer() {
  if (!checkAuth()) return;
  
  String body = server.arg("plain");
  
  config.timerEnabled = body.indexOf("\"enabled\":true") >= 0;
  
  // 简易解析 JSON (假设标准格式)
  // type
  int typeIdx = body.indexOf("\"type\":");
  if (typeIdx > 0) config.timerType = body.substring(typeIdx+7).toInt();
  
  // interval
  int intIdx = body.indexOf("\"interval\":");
  if (intIdx > 0) config.timerInterval = body.substring(intIdx+11).toInt();
  if (config.timerInterval < 1) config.timerInterval = 1;
  
  // phone
  int phIdx = body.indexOf("\"phone\":\"");
  if (phIdx > 0) {
      int end = body.indexOf("\"", phIdx+9);
      config.timerPhone = body.substring(phIdx+9, end);
  }
  
  // message
  int msgIdx = body.indexOf("\"message\":\"");
  if (msgIdx > 0) {
      int end = body.indexOf("\"", msgIdx+11);
      config.timerMessage = body.substring(msgIdx+11, end);
  }
  
  timerIntervalSec = (unsigned long)config.timerInterval * 24UL * 60UL * 60UL;
  lastTimerExec = millis();
  saveConfig();
  
  String json = "{\"success\":true,\"remain\":" + String(timerIntervalSec) + "}";
  server.send(200, "application/json", json);
}

// 处理重启请求
void handleRestart() {
  if (!checkAuth()) return;
  server.send(200, "application/json", "{\"success\":true,\"message\":\"设备将在2秒后重启\"}");
  delay(2000);
  ESP.restart();
}

// 获取短信历史记录
void handleSmsHistory() {
  if (!checkAuth()) return;
  
  String historyJson = getSmsHistory();
  String callHistoryJson = getCallHistory();

  // 预估大小并 reserve，仅保留单份 smsHistory 避免重复存放导致 OOM (M5)
  size_t estLen = historyJson.length() + callHistoryJson.length() + 64;
  String json;
  json.reserve(estLen);
  json = "{";
  json += "\"smsHistory\":";
  json += historyJson;
  json += ",\"callHistory\":";
  json += callHistoryJson;
  json += "}";

  server.send(200, "application/json", json);
}

// 获取统计信息
void handleStats() {
  if (!checkAuth()) return;
  
  String json;
  json.reserve(1024); // 预分配 1KB 空间，杜绝几十次 += 造成的堆碎片 (M5)
  json = "{";
  json += "\"received\":" + String(stats.smsReceived.load()) + ",";
  json += "\"sent\":" + String(stats.smsSent.load()) + ",";
  json += "\"calls\":" + String(stats.callsReceived.load()) + ",";
  json += "\"pushOk\":" + String(stats.pushSuccess.load()) + ",";
  json += "\"pushFail\":" + String(stats.pushFailed.load()) + ",";
  json += "\"boots\":" + String(stats.bootCount.load()) + ",";
  json += "\"lastResetReason\":\"" + jsonEscape(lastResetReasonText) + "\",";
  json += "\"lastResetReasonCode\":" + String((int)lastResetReasonCode) + ",";
  json += "\"powerSuspected\":" + String(lastResetPowerSuspected ? "true" : "false") + ",";
  json += "\"brownoutResets\":" + String(stats.brownoutResets.load()) + ",";
  json += "\"powerOnResets\":" + String(stats.powerOnResets.load()) + ",";
  json += "\"watchdogResets\":" + String(stats.watchdogResets.load()) + ",";
  json += "\"softwareResets\":" + String(stats.softwareResets.load()) + ",";
  json += "\"panicResets\":" + String(stats.panicResets.load()) + ",";
  json += "\"interruptedOperation\":\"" + jsonEscape(lastInterruptedOperation.operation) + "\",";
  json += "\"interruptedStage\":\"" + jsonEscape(lastInterruptedOperation.stage) + "\",";
  json += "\"interruptedUptime\":" + String(lastInterruptedOperation.uptimeSeconds) + ",";
  json += "\"interruptedFreeHeap\":" + String(lastInterruptedOperation.freeHeap) + ",";
  json += "\"interruptedMinFreeHeap\":" + String(lastInterruptedOperation.minFreeHeap) + ",";
  json += "\"interruptedMaxAllocHeap\":" + String(lastInterruptedOperation.maxAllocHeap) + ",";
  json += "\"hasInterruptedOperation\":" + String(lastInterruptedOperation.valid ? "true" : "false") + ",";
  json += "\"uptime\":" + String(millis() / 1000) + ",";
  json += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
  json += "\"maxAllocHeap\":" + String(ESP.getMaxAllocHeap()) + ",";
  json += "\"minFreeHeap\":" + String(ESP.getMinFreeHeap()) + ",";
  json += "\"wifiRssi\":" + String(WiFi.RSSI());
  json += "}";
  
  server.send(200, "application/json", json);
}

// 重置统计信息
void handleResetStats() {
  if (!checkAuth()) return;

  resetStats(false);
  server.send(200, "application/json", "{\"success\":true,\"message\":\"统计数据已重置\"}");
}

// 清空短信历史记录
void handleClearSmsHistory() {
  if (!checkAuth()) return;

  clearSmsHistory();
  stats.smsReceived.store(0);
  saveStats();
  server.send(200, "application/json", "{\"success\":true,\"message\":\"短信记录已清空，对应统计已归零\"}");
}

// 清空来电历史记录
void handleClearCallHistory() {
  if (!checkAuth()) return;

  clearCallHistory();
  stats.callsReceived.store(0);
  saveStats();
  server.send(200, "application/json", "{\"success\":true,\"message\":\"来电记录已清空，对应统计已归零\"}");
}

// 清洗旧历史记录中的损坏行
void handleCleanupHistory() {
  if (!checkAuth()) return;

  String type = server.arg("type");
  HistoryCleanupResult result = {0, 0};
  String targetName = "";

  if (type == "sms") {
    result = cleanupSmsHistory();
    targetName = "短信历史";
  } else if (type == "call") {
    result = cleanupCallHistory();
    targetName = "来电历史";
  } else {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"参数错误：type 仅支持 sms 或 call\"}");
    return;
  }

  String json;
  json.reserve(128);
  json = "{";
  json += "\"success\":true,";
  json += "\"message\":\"";
  json += targetName;
  json += "清洗完成\",";
  json += "\"kept\":";
  json += String(result.kept);
  json += ",\"removed\":";
  json += String(result.removed);
  json += "}";
  server.send(200, "application/json", json);
}

// 保存黑白名单配置
void handleFilterSave() {
  if (!checkAuth()) return;
  
  String body = server.arg("plain");
  // 简单 JSON 解析
  config.filterEnabled = body.indexOf("\"enabled\":true") >= 0;
  config.filterIsWhitelist = body.indexOf("\"whitelist\":true") >= 0;
  
  // 解析 numbers 数组或字符串
  int numStart = body.indexOf("\"numbers\":");
  if (numStart > 0) {
      int arrStart = body.indexOf("[", numStart);
      int arrEnd = body.indexOf("]", numStart);
      if (arrStart > 0 && arrEnd > arrStart) {
          String arrStr = body.substring(arrStart + 1, arrEnd);
          // 将 JSON 数组 "a","b" 转换为 a,b 存入 filterList
          config.filterList = "";
          int lastQ = -1;
          while (true) {
              int startQ = arrStr.indexOf("\"", lastQ + 1);
              if (startQ < 0) break;
              int endQ = arrStr.indexOf("\"", startQ + 1);
              if (endQ < 0) break;
              if (config.filterList.length() > 0) config.filterList += ",";
              config.filterList += arrStr.substring(startQ + 1, endQ);
              lastQ = endQ;
          }
      }
  }
  
  saveConfig();
  server.send(200, "application/json", "{\"success\":true,\"message\":\"号码名单已更新\"}");
}

// 保存内容关键词过滤配置
void handleContentFilterSave() {
  if (!checkAuth()) return;
  
  String body = server.arg("plain");
  // 简单 JSON 解析
  config.contentFilterEnabled = body.indexOf("\"enabled\":true") >= 0;
  config.contentFilterIsWhitelist = body.indexOf("\"whitelist\":true") >= 0;
  
  // 解析 keywords 字符串
  int kwStart = body.indexOf("\"keywords\":\"");
  if (kwStart > 0) {
    int valStart = kwStart + 12;  // 跳过 "keywords":"
    int valEnd = body.indexOf("\"", valStart);
    if (valEnd > valStart) {
      config.contentFilterList = body.substring(valStart, valEnd);
    }
  }
  
  saveConfig();
  server.send(200, "application/json", "{\"success\":true,\"message\":\"关键词过滤已更新\"}");
}
