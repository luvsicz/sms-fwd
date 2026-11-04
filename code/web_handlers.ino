/*
 * web_handlers.ino - Web 服务器处理函数实现
 */
#include "web_output.h"
#include "modem_handler.h"
#include "json_input.h"
#include <utility>

// 检查 HTTP Basic 认证
bool checkAuth() {
  if (!server.authenticate(config.webUser.c_str(), config.webPass.c_str())) {
    server.requestAuthentication(BASIC_AUTH, "SMS Forwarding", "请输入管理员账号密码");
    return false;
  }
  return true;
}

static const char* atResultText(ModemResult result) {
  switch (result) {
    case ModemResult::Pending: return "PENDING";
    case ModemResult::Ok: return "OK";
    case ModemResult::Error: return "ERROR";
    case ModemResult::Timeout: return "TIMEOUT";
    case ModemResult::Busy: return "BUSY";
    case ModemResult::Overflow: return "OVERFLOW";
    case ModemResult::Invalid: return "INVALID";
  }
  return "INVALID";
}

// 发送 AT 命令并获取响应
String sendATCommand(const char* cmd, unsigned long timeout) {
  Serial.printf("[耗时] START AT命令: %s, timeout=%lums\n", cmd, timeout);
  unsigned long start = millis();
  String resp;
  ModemResult result = runModemCommand(cmd, timeout, &resp);
  Serial.printf("[耗时] END AT命令: %s, elapsed=%lums, result=%s, respLen=%u\n",
                cmd, millis() - start, atResultText(result), resp.length());
  return resp;
}

// AT 命令并等待 OK
bool sendATandWaitOK(const char* cmd, unsigned long timeout) {
  Serial.printf("[耗时] START AT等待OK: %s, timeout=%lums\n", cmd, timeout);
  unsigned long start = millis();
  String resp;
  ModemResult result = runModemCommand(cmd, timeout, &resp);
  Serial.printf("[耗时] END AT等待OK: %s, elapsed=%lums, result=%s, respLen=%u\n",
                cmd, millis() - start, atResultText(result), resp.length());
  return result == ModemResult::Ok;
}

// 等待 CGATT 附着
bool waitCGATT1() {
  String resultLine;
  if (runModemCommand("AT+CGATT?", 2000, nullptr, &resultLine, "+CGATT:") != ModemResult::Ok) return false;
  String state = resultLine.substring(7);
  state.trim();
  return state == "1";
}

// 重启模组
void resetModule() {
  Serial.println("正在重启模组...");
  sendATandWaitOK("AT+CFUN=1,1", 3000);
  modemDelay(3000);
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
  String currentSsid = WiFi.SSID();

  String mqttStatusText = config.mqttEnabled ? (mqttClient.connected() ? "已连接" : "未连接") : "未启用";

  const uint64_t remainSec = timerRemainingSeconds();

  // ---- 定位 htmlPage 中的 3 个大占位符，将模板拆成 4 段 ----
  // 顺序: ... %COMMON_CSS% ... %WIFI_NETWORKS% ... %PUSH_CHANNELS% ...
  const char* pCSS = strstr(htmlPage, "%COMMON_CSS%");
  const char* pWIFI = strstr(htmlPage, "%WIFI_NETWORKS%");
  const char* pPUSH = strstr(htmlPage, "%PUSH_CHANNELS%");

  const char* pINFO = strstr(htmlPage, "%WIFI_INFO_LINE%");
  const char* pTOPICS = strstr(htmlPage, "%MQTT_TOPICS%");
  if (!pCSS || !pWIFI || !pPUSH || !pINFO || !pTOPICS ||
      !(pCSS < pINFO && pINFO < pTOPICS && pTOPICS < pWIFI && pWIFI < pPUSH) ||
      strstr(pCSS + 1, "%COMMON_CSS%") || strstr(pWIFI + 1, "%WIFI_NETWORKS%") ||
      strstr(pINFO + 1, "%WIFI_INFO_LINE%") ||
      strstr(pPUSH + 1, "%PUSH_CHANNELS%") || strstr(pTOPICS + 1, "%MQTT_TOPICS%")) {
    Serial.println("handleRoot: 模板占位符定位失败");
    server.send(503, "text/plain; charset=utf-8", "首页暂时不可用：模板错误");
    return;
  }

  // 开始 chunked transfer
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=utf-8", "");
  WebOutput out(sendWebChunk);

  if (!out.write(htmlPage, pCSS - htmlPage) || !out.text(commonCss)) return;

  // ==== 段2: %COMMON_CSS% 之后 → %WIFI_NETWORKS% 之前 ====
  // 跳过 "%COMMON_CSS%" 占位符本身 (12 字符)
  {
    const char* seg2Start = pCSS + strlen("%COMMON_CSS%");
    // 本段包含的占位符: 概览页+控制页+配置页(WiFi之前)的所有占位符
    const WebOutputValue pairs[] = {
      {"%IP%", displayIp.c_str()},
      {"%WIFI_MODE_BADGE%", wifiModeBadge.c_str(), WebEncoding::Raw},
      {"%MQTT_STATUS%", mqttStatusText.c_str()},
    };
    if (!out.render(seg2Start, pINFO - seg2Start, pairs, sizeof(pairs) / sizeof(pairs[0]))) return;
    // 静态换行与动态文本分开输出；SSID/主题不能作为 HTML。
    if (apActive) {
      if (!out.text("热点名: SMS-Forwarder-AP<br>热点IP: ") || !out.encoded(displayIp.c_str())) return;
      if (staConnected && (!out.text("<br>上游WiFi: ") || !out.encoded(currentSsid.c_str()))) return;
    } else if (staConnected) {
      if (!out.text("已连接WiFi: ") || !out.encoded(currentSsid.c_str()) ||
          !out.text("<br>设备IP: ") || !out.encoded(displayIp.c_str())) return;
    } else if (!out.text("WiFi未连接")) return;
    const char* afterInfo = pINFO + strlen("%WIFI_INFO_LINE%");
    if (!out.render(afterInfo, pTOPICS - afterInfo, pairs, sizeof(pairs) / sizeof(pairs[0]))) return;
    if (config.mqttEnabled && config.mqttServer.length() > 0) {
      if (!out.encoded(mqttTopicStatus.c_str()) || !out.text("<br>") ||
          !out.encoded(mqttTopicSmsReceived.c_str()) || !out.text("<br>") ||
          !out.encoded(mqttTopicCallReceived.c_str())) return;
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
      bool isCurrent = config.wifiNetworks[i].ssid.length() > 0 && config.wifiNetworks[i].ssid == currentSsid;
      static const char wifiTemplate[] = R"html(<div style="padding:10px;background:#f8fafc;border-radius:8px;margin-bottom:8px;border:1px solid %BORDER%"><div class="sw-row" onclick="wfTog(%IDX%)"><span style="font-weight:600">网络 %NUMBER%%CURRENT%</span><div id="wfs%IDX%" class="sw %EN_SW%"></div><input type="hidden" id="wfe%IDX%" name="wifi%IDX%en" value="%EN_VAL%"></div><div class="grid-2" style="margin-top:8px"><div class="fg" style="margin-bottom:0"><label>SSID</label><input name="wifi%IDX%ssid" value="%SSID%" placeholder="WiFi名称"></div><div class="fg" style="margin-bottom:0"><label>密码</label><input name="wifi%IDX%pass" type="password" placeholder="%PASS_HINT%"></div></div></div>)html";
      const WebOutputValue pairs[] = {
        {"%IDX%", idx}, {"%NUMBER%", number},
        {"%BORDER%", isCurrent ? "var(--success)" : "#e2e8f0"},
        {"%CURRENT%", isCurrent ? " <span class=\"badge b-ok\">当前</span>" : "", WebEncoding::Raw},
        {"%EN_SW%", config.wifiNetworks[i].enabled ? "on" : ""},
        {"%EN_VAL%", config.wifiNetworks[i].enabled ? "true" : "false"},
        {"%SSID%", config.wifiNetworks[i].ssid.c_str(), WebEncoding::HtmlAttribute},
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
    const WebOutputValue pairs[] = {
      {"%WEB_USER%", config.webUser.c_str(), WebEncoding::HtmlAttribute},
      {"%WEB_PASS%", config.webPass.c_str(), WebEncoding::HtmlAttribute},
      {"%SMTP_EN_VAL%", config.emailEnabled ? "true" : "false"},
      {"%SMTP_EN_SW%", config.emailEnabled ? "on" : ""},
      {"%SMTP_DISP%", config.emailEnabled ? "block" : "none"},
      {"%SMTP_SERVER%", config.smtpServer.c_str(), WebEncoding::HtmlAttribute},
      {"%SMTP_PORT%", smtpPort},
      {"%SMTP_USER%", config.smtpUser.c_str(), WebEncoding::HtmlAttribute},
      {"%SMTP_PASS%", config.smtpPass.c_str(), WebEncoding::HtmlAttribute},
      {"%SMTP_SEND_TO%", config.smtpSendTo.c_str(), WebEncoding::HtmlAttribute},
      {"%MQTT_EN_VAL%", config.mqttEnabled ? "true" : "false"},
      {"%MQTT_EN_SW%", config.mqttEnabled ? "on" : ""},
      {"%MQTT_DISP%", config.mqttEnabled ? "block" : "none"},
      {"%MQTT_SERVER%", config.mqttServer.c_str(), WebEncoding::HtmlAttribute},
      {"%MQTT_PORT%", mqttPort},
      {"%MQTT_USER%", config.mqttUser.c_str(), WebEncoding::HtmlAttribute},
      {"%MQTT_PASS%", config.mqttPass.c_str(), WebEncoding::HtmlAttribute},
      {"%MQTT_PREFIX%", config.mqttPrefix.c_str(), WebEncoding::HtmlAttribute},
      {"%MQTT_CO_VAL%", config.mqttControlOnly ? "true" : "false"},
      {"%MQTT_CO_SW%", config.mqttControlOnly ? "on" : ""},
      {"%MQTT_HA_VAL%", config.mqttHaDiscovery ? "true" : "false"},
      {"%MQTT_HA_SW%", config.mqttHaDiscovery ? "on" : ""},
      {"%MQTT_HA_DISP%", config.mqttHaDiscovery ? "block" : "none"},
      {"%MQTT_HA_PREFIX%", config.mqttHaPrefix.length() > 0 ? config.mqttHaPrefix.c_str() : "homeassistant", WebEncoding::HtmlAttribute},
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
      // HTML parser 吞掉 textarea 后的首个 LF；固定前导 LF 保留正文自己的首行。
      static const char pushTemplate[] = R"html(<div class="card" style="border:1px solid #e2e8f0;padding:12px;margin-bottom:8px;box-shadow:none"><div class="sw-row" onclick="chTog(%IDX%)"><span style="font-weight:600">通道 %NUMBER%</span><div id="chs%IDX%" class="sw %EN_SW%"></div><input type="hidden" id="che%IDX%" name="push%IDX%en" value="%EN_VAL%"></div><div style="font-size:0.85em;color:var(--primary);text-align:right;cursor:pointer" onclick="fd(%IDX%)">展开/收起 <span id="chi%IDX%" style="display:inline-block;transition:.2s">></span></div><div id="chb%IDX%" style="display:none;margin-top:12px;border-top:1px solid #f1f5f9;padding-top:12px"><div class="fg"><label>名称</label><input name="push%IDX%name" value="%NAME%"></div><div class="fg"><label>类型</label><select name="push%IDX%type" id="tp%IDX%" onchange="upd(%IDX%)"><option value="1"%SEL1%>POST JSON</option><option value="2"%SEL2%>Bark</option><option value="3"%SEL3%>GET请求</option><option value="4"%SEL4%>自定义模板</option><option value="5"%SEL5%>Telegram Bot</option><option value="6"%SEL6%>企业微信</option><option value="7"%SEL7%>钉钉</option></select></div><div class="fg"><label>URL</label><input name="push%IDX%url" value="%URL%" placeholder="https://..."></div><div id="k1%IDX%" style="display:%K1_DISP%"><div class="fg"><label id="k1l%IDX%">%K1_LABEL%</label><input name="push%IDX%k1" value="%KEY1%" placeholder="%K1_HINT%"></div></div><div id="k2%IDX%" style="display:%K2_DISP%"><div class="fg"><label>Bark 分组 group (可选)</label><input name="push%IDX%k2" value="%KEY2%" placeholder="如 家庭短信 / 验证码"></div></div><div id="cf%IDX%" style="display:%CUSTOM_DISP%"><div class="fg"><label>Body模板</label><textarea name="push%IDX%body" rows="3">
%BODY%</textarea></div></div></div></div>)html";
      const WebOutputValue pairs[] = {
        {"%IDX%", idx}, {"%NUMBER%", number},
        {"%EN_SW%", channel.enabled ? "on" : ""},
        {"%EN_VAL%", channel.enabled ? "true" : "false"},
        {"%NAME%", channel.name.c_str(), WebEncoding::HtmlAttribute},
        {"%URL%", channel.url.c_str(), WebEncoding::HtmlAttribute},
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
        {"%KEY1%", channel.key1.c_str(), WebEncoding::HtmlAttribute},
        {"%KEY2%", channel.key2.c_str(), WebEncoding::HtmlAttribute},
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
    char timerType[12], timerInterval[12], timerRemain[21];
    snprintf(timerType, sizeof(timerType), "%d", config.timerType);
    snprintf(timerInterval, sizeof(timerInterval), "%d", config.timerInterval);
    snprintf(timerRemain, sizeof(timerRemain), "%llu", (unsigned long long)remainSec);

    const WebOutputValue pairs[] = {
      {"%FILTER_EN_VAL%", config.filterEnabled ? "true" : "false"},
      {"%FILTER_EN_BOOL%", config.filterEnabled ? "true" : "false"},
      {"%FILTER_WL_BOOL%", config.filterIsWhitelist ? "true" : "false"},
      {"%FILTER_WL_VAL%", config.filterIsWhitelist ? "true" : "false"},
      {"%FILTER_LIST%", config.filterList.c_str(), WebEncoding::JsString},
      {"%CF_EN_VAL%", config.contentFilterEnabled ? "true" : "false"},
      {"%CF_EN_BOOL%", config.contentFilterEnabled ? "true" : "false"},
      {"%CF_WL_BOOL%", config.contentFilterIsWhitelist ? "true" : "false"},
      {"%CF_WL_VAL%", config.contentFilterIsWhitelist ? "true" : "false"},
      {"%CF_LIST%", config.contentFilterList.c_str(), WebEncoding::JsString},
      {"%TIMER_EN_VAL%", config.timerEnabled ? "true" : "false"},
      {"%TIMER_EN_BOOL%", config.timerEnabled ? "true" : "false"},
      {"%TIMER_TP%", timerType},
      {"%TIMER_INT%", timerInterval},
      {"%TIMER_PH%", config.timerPhone.c_str(), WebEncoding::JsString},
      {"%TIMER_MS%", config.timerMessage.c_str(), WebEncoding::JsString},
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

static void sendConfigSaveError(const char* reason) {
  // saveConfig 返回的原因均为不含引号/配置值的静态文本。
  server.send(503, "application/json", String("{\"success\":false,\"message\":\"") + reason + "\"}");
}

static void normalizeConfigList(String& value) {
  // 仅缩短，不创建临时 String；低内存时不能静默漏做规范化。
  for (int i = 0; i + 1 < static_cast<int>(value.length()); ++i) {
    if (value.charAt(i) == '\r' && value.charAt(i + 1) == '\n') value.remove(i + 1, 1);
  }
  value.replace('\r', ',');
  value.replace('\n', ',');
  for (unsigned int i = 0; i + 2 < value.length(); ++i) {
    if (memcmp(value.c_str() + i, "\xef\xbc\x8c", 3) == 0) {
      value.setCharAt(i, ',');
      value.remove(i + 1, 2);
    }
  }
}

static bool configCopyComplete(const Config& copy, const Config& original) {
  const String* a[] = {&copy.smtpServer, &copy.smtpUser, &copy.smtpPass, &copy.smtpSendTo,
    &copy.webUser, &copy.webPass, &copy.timerPhone, &copy.timerMessage, &copy.mqttServer,
    &copy.mqttUser, &copy.mqttPass, &copy.mqttPrefix, &copy.mqttHaPrefix, &copy.filterList, &copy.contentFilterList};
  const String* b[] = {&original.smtpServer, &original.smtpUser, &original.smtpPass, &original.smtpSendTo,
    &original.webUser, &original.webPass, &original.timerPhone, &original.timerMessage, &original.mqttServer,
    &original.mqttUser, &original.mqttPass, &original.mqttPrefix, &original.mqttHaPrefix, &original.filterList, &original.contentFilterList};
  auto complete = [](const String& x, const String& y) { return x && x.length() == y.length() && x == y; };
  for (size_t i = 0; i < sizeof(a) / sizeof(a[0]); ++i) if (!complete(*a[i], *b[i])) return false;
  for (int i = 0; i < MAX_WIFI_NETWORKS; ++i) {
    if (!complete(copy.wifiNetworks[i].ssid, original.wifiNetworks[i].ssid) ||
        !complete(copy.wifiNetworks[i].password, original.wifiNetworks[i].password)) return false;
  }
  for (int i = 0; i < MAX_PUSH_CHANNELS; ++i) {
    const PushChannel& x = copy.pushChannels[i]; const PushChannel& y = original.pushChannels[i];
    if (!complete(x.name, y.name) || !complete(x.url, y.url) || !complete(x.key1, y.key1) ||
        !complete(x.key2, y.key2) || !complete(x.customBody, y.customBody)) return false;
  }
  return true;
}

// 处理保存配置请求 (全量保存)
void handleSave() {
  if (!checkAuth()) return;

  // 仅主任务更新配置。先在堆上构建候选，不能用未经检查的复制直接覆盖全局。
  std::unique_ptr<Config> next(new (std::nothrow) Config(config));
  if (!next) {
    sendConfigSaveError("内存不足，未保存配置");
    return;
  }
  if (!configCopyComplete(*next, config)) {
    sendConfigSaveError("配置快照内存不足，未保存配置");
    return;
  }
  bool formOk = true;
  auto arg = [&formOk](const char* name) {
    String key(name);
    if (!key || key.length() != strlen(name)) formOk = false;
    String value = server.arg(key);
    // 表单原始文本也有界；规范化后的持久化字节预算在应用前统一检查。
    if (!value || value.length() > JsonInput::MAX_INPUT_BYTES ||
        memchr(value.c_str(), '\0', value.length())) formOk = false;
    return value;
  };
  auto flag = [&arg, &formOk](const char* name, bool allowOn = false) {
    String value = arg(name);
    if (value.length() && value != "true" && value != "false" && !(allowOn && value == "on")) formOk = false;
    return value == "true" || (allowOn && value == "on");
  };
  esp_task_wdt_reset();
  yield();
  bool wifiChanged = false;

  for (int i = 0; i < MAX_WIFI_NETWORKS; i++) {
    char key[24];
    snprintf(key, sizeof(key), "wifi%dssid", i); String ssid = arg(key);
    snprintf(key, sizeof(key), "wifi%dpass", i); String pass = arg(key);
    snprintf(key, sizeof(key), "wifi%den", i); bool enabled = flag(key);
    WifiNetwork& wifi = next->wifiNetworks[i];
    if (ssid.length()) wifi.ssid = std::move(ssid);
    if (pass.length()) wifi.password = std::move(pass);  // 留空保留
    wifi.enabled = enabled;
    wifiChanged |= wifi.ssid != config.wifiNetworks[i].ssid ||
                   wifi.password != config.wifiNetworks[i].password || enabled != config.wifiNetworks[i].enabled;
  }

  esp_task_wdt_reset();
  yield();
  String newWebUser = arg("webUser");
  String newWebPass = arg("webPass");
  // 保留已有策略：两个字段均非空才更换 Web 凭据。
  if (newWebUser.length() > 0 && newWebPass.length() > 0) {
    next->webUser = std::move(newWebUser);
    next->webPass = std::move(newWebPass);
  }
  next->emailEnabled = flag("smtpEn");
  next->smtpServer = arg("smtpServer"); next->smtpServer.trim();
  next->smtpPort = arg("smtpPort").toInt();
  if (next->smtpPort <= 0) next->smtpPort = 465;
  next->smtpUser = arg("smtpUser"); next->smtpUser.trim();
  String smtpPass = arg("smtpPass"); smtpPass.trim();
  if (smtpPass.length()) next->smtpPass = std::move(smtpPass);
  next->smtpSendTo = arg("smtpSendTo"); next->smtpSendTo.trim();

  esp_task_wdt_reset();
  yield();
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    char key[24];
    PushChannel& channel = next->pushChannels[i];
    snprintf(key, sizeof(key), "push%den", i); channel.enabled = flag(key);
    snprintf(key, sizeof(key), "push%dtype", i); channel.type = (PushType)arg(key).toInt();
    snprintf(key, sizeof(key), "push%durl", i); channel.url = arg(key);
    snprintf(key, sizeof(key), "push%dname", i); channel.name = arg(key);
    snprintf(key, sizeof(key), "push%dk1", i); channel.key1 = arg(key);
    snprintf(key, sizeof(key), "push%dk2", i); channel.key2 = arg(key);
    snprintf(key, sizeof(key), "push%dbody", i); channel.customBody = arg(key);
  }

  esp_task_wdt_reset();
  yield();
  next->mqttEnabled = flag("mqttEn");
  next->mqttServer = arg("mqttServer");
  next->mqttPort = arg("mqttPort").toInt();
  if (next->mqttPort == 0) next->mqttPort = 1883;
  next->mqttUser = arg("mqttUser");
  next->mqttPass = arg("mqttPass");  // MQTT 留空仍按原接口清空，不改变协议。
  next->mqttPrefix = arg("mqttPrefix");
  if (next->mqttPrefix.length() == 0) next->mqttPrefix = "sms";
  next->mqttControlOnly = flag("mqttCtrlOnly", true);
  next->mqttHaDiscovery = flag("mqttHaDiscovery");
  next->mqttHaPrefix = arg("mqttHaPrefix");
  if (next->mqttHaPrefix.length() == 0) next->mqttHaPrefix = "homeassistant";

  if (server.hasArg("filterEn")) {
    next->filterEnabled = flag("filterEn");
    next->filterIsWhitelist = flag("filterIsWhitelist");
    next->filterList = arg("filterList");
    normalizeConfigList(next->filterList);
  }
  // 内容过滤保留三个独立可选字段：省略保留，显式空名单清空。
  if (server.hasArg("contentFilterEn")) {
    String value = arg("contentFilterEn");
    if (value != "true" && value != "false") formOk = false;
    next->contentFilterEnabled = value == "true";
  }
  if (server.hasArg("contentFilterIsWhitelist")) {
    String value = arg("contentFilterIsWhitelist");
    if (value != "true" && value != "false") formOk = false;
    next->contentFilterIsWhitelist = value == "true";
  }
  if (server.hasArg("contentFilterList")) {
    next->contentFilterList = arg("contentFilterList");
    normalizeConfigList(next->contentFilterList);
  }

  if (server.hasArg("timerEn")) {
    next->timerEnabled = flag("timerEn");
    next->timerType = arg("timerType").toInt();
    next->timerInterval = arg("timerInterval").toInt();
    if (next->timerInterval < 1) next->timerInterval = 1;
    next->timerPhone = arg("timerPhone");
    next->timerMessage = arg("timerMessage");
    if ((next->timerType != 0 && next->timerType != 1) ||
        (next->timerEnabled && next->timerType == 1 &&
         (!next->timerPhone.length() || strspn(next->timerPhone.c_str(), " \t\r\n\f\v") == next->timerPhone.length() ||
          !next->timerMessage.length()))) {
      server.send(400, "application/json", "{\"success\":false,\"message\":\"type仅支持0或1，启用短信任务时号码和正文不能为空\"}");
      return;
    }
  }
  if (!formOk || !configFitsNvs(*next)) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"配置字段无效、内存不足或过长；每个持久化字符串最多3999个UTF-8字节\"}");
    return;
  }

  // 全量表单总含定时字段；比较规范化后的设置，未变更时保留原起点。
  const bool timerChanged = next->timerEnabled != config.timerEnabled ||
                            next->timerType != config.timerType ||
                            next->timerInterval != config.timerInterval ||
                            next->timerPhone != config.timerPhone ||
                            next->timerMessage != config.timerMessage;

  // 移动交换不分配 String。保存失败恢复所有字段和起点，不留下部分 RAM 应用。
  const uint64_t priorExec = lastTimerExec, priorInterval = timerIntervalSec;
  std::swap(config, *next);
  if (timerChanged) resetTimerSchedule();
  esp_task_wdt_reset();
  yield();
  const char* error = nullptr;
  if (!saveConfig(&error)) {
    std::swap(config, *next);
    lastTimerExec = priorExec; timerIntervalSec = priorInterval;
    sendConfigSaveError(error);
    return;
  }
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

  const int length = server.clientContentLength();
  if (length < 2 || static_cast<size_t>(length) > JsonInput::MAX_INPUT_BYTES) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"JSON请求长度无效，最多8192字节\"}");
    return;
  }
  const String& body = server.arg("plain");
  JsonInput input;
  String phone, content;
  if (body.length() != static_cast<unsigned int>(length) || !input.parse(body) ||
      !input.getString("phone", phone) || !input.getString("content", content)) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"无效JSON：phone/content须为字符串，最多8192字节\"}");
    return;
  }

  phone.trim();
  // 正文不 trim：解码后的空白、换行及引号都是短信内容。
  if (phone.length() == 0 || content.length() == 0) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"目标号码和短信内容不能为空\"}");
    return;
  }

  Serial.println("网页端发送短信请求");
  Serial.print("目标号码: "); Serial.println(phone);
  Serial.print("短信内容: "); Serial.println(content);
  bool success = sendSMS(phone.c_str(), content.c_str());
  if (success) {
    stats.smsSent++;
    saveStats();
  }
  String resultMsg = success ? "短信发送成功！" : "短信发送失败，请检查模组状态";

  String json = "{";
  json += "\"success\":" + String(success ? "true" : "false") + ",";
  json += "\"message\":\"" + resultMsg + "\"";
  json += "}";
  
  server.send(200, "application/json", json);
}

// 处理定时任务配置保存 (API: /timer)
void handleTimer() {
  if (!checkAuth()) return;

  const int length = server.clientContentLength();
  if (length < 2 || static_cast<size_t>(length) > JsonInput::MAX_INPUT_BYTES) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"JSON请求长度无效，最多8192字节\"}");
    return;
  }
  const String& body = server.arg("plain");
  JsonInput input;
  bool enabled = false;
  int type = config.timerType, interval = config.timerInterval;
  String phone, message;
  if (body.length() != static_cast<unsigned int>(length) || !input.parse(body) || !input.getBool("enabled", enabled) ||
      (input.has("type") && !input.getInt("type", type)) ||
      (input.has("interval") && !input.getInt("interval", interval)) ||
      (input.has("phone") && !input.getString("phone", phone)) ||
      (input.has("message") && !input.getString("message", message))) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"无效JSON或字段类型：enabled须为布尔值，可选type/interval须为整数、phone/message须为字符串，最多8192字节\"}");
    return;
  }
  const bool hasPhone = input.has("phone"), hasMessage = input.has("message");
  if (hasPhone) phone.trim();
  // 省略字符串只读引用，保留原值，避免隐式复制分配失败；正文始终不 trim。
  const String& nextPhone = hasPhone ? phone : config.timerPhone;
  const String& nextMessage = hasMessage ? message : config.timerMessage;
  if ((type != 0 && type != 1) ||
      (enabled && type == 1 && (nextPhone.length() == 0 ||
       strspn(nextPhone.c_str(), " \t\r\n\f\v") == nextPhone.length() || nextMessage.length() == 0))) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"type仅支持0或1，启用短信任务时号码和正文不能为空\"}");
    return;
  }
  if (interval < 1) interval = 1;  // 合法非正整数沿用旧接口的 1 天最小值。
  if (!configStringFitsNvs(nextPhone) || !configStringFitsNvs(nextMessage)) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"定时号码和正文各最多3999个UTF-8字节，未保存\"}");
    return;
  }

  // 只备份提供的字符串，不复制省略字段；失败回滚，成功维持原有重置时点。
  const bool priorEnabled = config.timerEnabled;
  const int priorType = config.timerType, priorDays = config.timerInterval;
  const uint64_t priorExec = lastTimerExec, priorInterval = timerIntervalSec;
  if (hasPhone) std::swap(config.timerPhone, phone);
  if (hasMessage) std::swap(config.timerMessage, message);
  config.timerEnabled = enabled;
  config.timerType = type;
  config.timerInterval = interval;
  resetTimerSchedule();
  const char* error = nullptr;
  if (!saveConfig(&error)) {
    if (hasPhone) std::swap(config.timerPhone, phone);
    if (hasMessage) std::swap(config.timerMessage, message);
    config.timerEnabled = priorEnabled; config.timerType = priorType; config.timerInterval = priorDays;
    lastTimerExec = priorExec; timerIntervalSec = priorInterval;
    sendConfigSaveError(error);
    return;
  }

  // /timer 的 remain 沿用完整周期；首页倒计时仍由 timerRemainingSeconds() 计算。
  String json = "{\"success\":true,\"message\":\"定时任务已保存\",\"remain\":" + String(timerIntervalSec) + "}";
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
  if (!historyJson.length() || !callHistoryJson.length()) {
    server.send(503, "application/json", "{\"success\":false,\"message\":\"历史读取失败，请稍后重试\"}");
    return;
  }
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  WebStream output(sendWebChunk);
  if (!output.text("{\"smsHistory\":") || !output.text(historyJson.c_str()) ||
      !output.text(",\"callHistory\":") || !output.text(callHistoryJson.c_str()) ||
      !output.text("}") || !output.flush()) {
    server.client().stop();
    return;
  }
  server.sendContent("", 0);
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

  const int length = server.clientContentLength();
  if (length < 2 || static_cast<size_t>(length) > JsonInput::MAX_INPUT_BYTES) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"JSON请求长度无效，最多8192字节\"}");
    return;
  }
  const String& body = server.arg("plain");
  JsonInput input;
  bool enabled = false, whitelist = false;
  String numbers;
  if (body.length() != static_cast<unsigned int>(length) || !input.parse(body) || !input.getBool("enabled", enabled) ||
      !input.getBool("whitelist", whitelist) || !input.getStringList("numbers", numbers)) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"无效JSON：enabled/whitelist须为布尔值，numbers须为字符串或字符串数组，最多8192字节\"}");
    return;
  }

  // 仅规范化已解码文本的分隔符，不再把引号或方括号解释为 JSON。
  normalizeConfigList(numbers);
  if (!configStringFitsNvs(numbers)) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"号码名单最多3999个UTF-8字节，未保存\"}");
    return;
  }
  const bool priorEnabled = config.filterEnabled, priorWhitelist = config.filterIsWhitelist;
  std::swap(config.filterList, numbers);
  config.filterEnabled = enabled;
  config.filterIsWhitelist = whitelist;
  const char* error = nullptr;
  if (!saveConfig(&error)) {
    std::swap(config.filterList, numbers);
    config.filterEnabled = priorEnabled; config.filterIsWhitelist = priorWhitelist;
    sendConfigSaveError(error);
    return;
  }
  server.send(200, "application/json", "{\"success\":true,\"message\":\"号码名单已更新\"}");
}

// 保存内容关键词过滤配置
void handleContentFilterSave() {
  if (!checkAuth()) return;

  const int length = server.clientContentLength();
  if (length < 2 || static_cast<size_t>(length) > JsonInput::MAX_INPUT_BYTES) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"JSON请求长度无效，最多8192字节\"}");
    return;
  }
  const String& body = server.arg("plain");
  JsonInput input;
  bool enabled = false, whitelist = false;
  String keywords;
  if (body.length() != static_cast<unsigned int>(length) || !input.parse(body) || !input.getBool("enabled", enabled) ||
      !input.getBool("whitelist", whitelist) || !input.getString("keywords", keywords)) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"无效JSON：enabled/whitelist须为布尔值，keywords须为字符串（可为空），最多8192字节\"}");
    return;
  }

  normalizeConfigList(keywords);
  if (!configStringFitsNvs(keywords)) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"关键词名单最多3999个UTF-8字节，未保存\"}");
    return;
  }
  const bool priorEnabled = config.contentFilterEnabled, priorWhitelist = config.contentFilterIsWhitelist;
  std::swap(config.contentFilterList, keywords);
  config.contentFilterEnabled = enabled;
  config.contentFilterIsWhitelist = whitelist;
  const char* error = nullptr;
  if (!saveConfig(&error)) {
    std::swap(config.contentFilterList, keywords);
    config.contentFilterEnabled = priorEnabled; config.contentFilterIsWhitelist = priorWhitelist;
    sendConfigSaveError(error);
    return;
  }
  server.send(200, "application/json", "{\"success\":true,\"message\":\"关键词过滤已更新\"}");
}
