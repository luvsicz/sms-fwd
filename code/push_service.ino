/*
 * push_service.ino - 推送服务函数实现
 * 
 * 支持 HTTPS 请求（跳过证书验证）
 * 支持钉钉加签验证
 */

#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// HTTP 推送必须短超时，避免单个通道阻塞 loopTask 触发看门狗
static const uint16_t HTTP_PUSH_TIMEOUT_MS = 1000;
static const uint8_t HTTP_PUSH_QUEUE_LENGTH = MAX_PUSH_CHANNELS * 2;
static const uint32_t HTTP_PUSH_TASK_STACK_SIZE = 6144;
static const uint8_t HTTP_PUSH_WORKER_COUNT = MAX_PUSH_CHANNELS;
static const uint8_t EMAIL_QUEUE_LENGTH = 4;
static const uint32_t EMAIL_TASK_STACK_SIZE = 8192;

struct HttpPushJob {
  PushChannel channel;
  String sender;
  String message;
  String timestamp;
};

struct EmailJob {
  String subject;
  String body;
};

static QueueHandle_t httpPushQueue = nullptr;
static TaskHandle_t httpPushTaskHandles[HTTP_PUSH_WORKER_COUNT] = {nullptr};
static QueueHandle_t emailQueue = nullptr;
static TaskHandle_t emailTaskHandle = nullptr;

void sendToChannel(const PushChannel& channel, const char* sender, const char* message, const char* timestamp);
static void sendEmailNotificationNow(const char* subject, const char* body);

static void feedPushWatchdog() {
  if (esp_task_wdt_status(NULL) == ESP_OK) {
    esp_task_wdt_reset();
  }
  yield();
}

static void httpPushWorkerTask(void* parameter) {
  HttpPushJob* job = nullptr;

  for (;;) {
    if (xQueueReceive(httpPushQueue, &job, portMAX_DELAY) == pdTRUE && job != nullptr) {
      sendToChannel(job->channel, job->sender.c_str(), job->message.c_str(), job->timestamp.c_str());
      saveStats();
      delete job;
      job = nullptr;
      delay(10);
    }
  }
}

static bool ensureHttpPushWorker() {
  if (httpPushQueue == nullptr) {
    httpPushQueue = xQueueCreate(HTTP_PUSH_QUEUE_LENGTH, sizeof(HttpPushJob*));
    if (httpPushQueue == nullptr) {
      Serial.println("HTTP推送队列创建失败");
      return false;
    }
  }

  for (uint8_t i = 0; i < HTTP_PUSH_WORKER_COUNT; i++) {
    if (httpPushTaskHandles[i] == nullptr) {
      char taskName[16];
      snprintf(taskName, sizeof(taskName), "httpPush%u", i + 1);
      BaseType_t taskCreated = xTaskCreate(
        httpPushWorkerTask,
        taskName,
        HTTP_PUSH_TASK_STACK_SIZE,
        nullptr,
        1,
        &httpPushTaskHandles[i]
      );

      if (taskCreated != pdPASS) {
        Serial.println("HTTP推送任务创建失败");
        httpPushTaskHandles[i] = nullptr;
        return i > 0;
      }
    }
  }

  return true;
}

static bool enqueueHttpPush(const PushChannel& channel, const char* sender, const char* message, const char* timestamp) {
  if (!ensureHttpPushWorker()) {
    stats.pushFailed++;
    return false;
  }

  HttpPushJob* job = new HttpPushJob();
  if (job == nullptr) {
    Serial.println("HTTP推送任务分配内存失败");
    stats.pushFailed++;
    return false;
  }

  job->channel = channel;
  job->sender = String(sender);
  job->message = String(message);
  job->timestamp = String(timestamp);

  if (xQueueSend(httpPushQueue, &job, 0) != pdTRUE) {
    Serial.println("HTTP推送队列已满，丢弃本次通道推送");
    delete job;
    stats.pushFailed++;
    return false;
  }

  return true;
}

static void emailWorkerTask(void* parameter) {
  EmailJob* job = nullptr;

  for (;;) {
    if (xQueueReceive(emailQueue, &job, portMAX_DELAY) == pdTRUE && job != nullptr) {
      sendEmailNotificationNow(job->subject.c_str(), job->body.c_str());
      delete job;
      job = nullptr;
      delay(10);
    }
  }
}

static bool ensureEmailWorker() {
  if (emailQueue == nullptr) {
    emailQueue = xQueueCreate(EMAIL_QUEUE_LENGTH, sizeof(EmailJob*));
    if (emailQueue == nullptr) {
      Serial.println("邮件队列创建失败");
      return false;
    }
  }

  if (emailTaskHandle == nullptr) {
    BaseType_t taskCreated = xTaskCreate(
      emailWorkerTask,
      "emailPush",
      EMAIL_TASK_STACK_SIZE,
      nullptr,
      1,
      &emailTaskHandle
    );

    if (taskCreated != pdPASS) {
      Serial.println("邮件任务创建失败");
      emailTaskHandle = nullptr;
      return false;
    }
  }

  return true;
}

static bool enqueueEmail(const char* subject, const char* body) {
  if (!ensureEmailWorker()) {
    stats.pushFailed++;
    return false;
  }

  EmailJob* job = new EmailJob();
  if (job == nullptr) {
    Serial.println("邮件任务分配内存失败");
    stats.pushFailed++;
    return false;
  }

  job->subject = String(subject);
  job->body = String(body);

  if (xQueueSend(emailQueue, &job, 0) != pdTRUE) {
    Serial.println("邮件队列已满，丢弃本次邮件通知");
    delete job;
    stats.pushFailed++;
    return false;
  }

  Serial.println("邮件通知已入队");
  return true;
}

// URL 编码辅助函数
String urlEncode(const String& str) {
  String encoded = "";
  for (unsigned int i = 0; i < str.length(); i++) {
    uint8_t c = (uint8_t)str.charAt(i);
    if (c == ' ') {
      encoded += '+';
    } else if ((c >= 'A' && c <= 'Z') ||
               (c >= 'a' && c <= 'z') ||
               (c >= '0' && c <= '9') ||
               c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += (char)c;
    } else {
      encoded += '%';
      encoded += "0123456789ABCDEF"[(c >> 4) & 0x0F];
      encoded += "0123456789ABCDEF"[c & 0x0F];
    }
  }
  return encoded;
}

// JSON 转义函数
String jsonEscape(const String& str) {
  String result = "";
  for (unsigned int i = 0; i < str.length(); i++) {
    uint8_t c = (uint8_t)str.charAt(i);
    if (c == '"') result += "\\\"";
    else if (c == '\\') result += "\\\\";
    else if (c == '\n') result += "\\n";
    else if (c == '\r') result += "\\r";
    else if (c == '\t') result += "\\t";
    else if (c == '\b') result += "\\b";
    else if (c == '\f') result += "\\f";
    else if (c < 0x20) {
      char buf[7];
      snprintf(buf, sizeof(buf), "\\u%04X", c);
      result += buf;
    } else {
      result += (char)c;
    }
  }
  return result;
}

// Telegram Markdown 转义函数
String telegramEscape(const String& str) {
  String result = "";
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    // 转义 Markdown 特殊字符: _ * [ ] ( ) ~ ` > # + - = | { } . !
    if (c == '_' || c == '*' || c == '[' || c == ']' || c == '(' || c == ')' ||
        c == '~' || c == '`' || c == '>' || c == '#' || c == '+' || c == '-' ||
        c == '=' || c == '|' || c == '{' || c == '}' || c == '.' || c == '!') {
      result += '\\';
    }
    result += c;
  }
  return result;
}

// HMAC-SHA256 签名（用于钉钉加签）
String hmacSha256Base64(const String& secret, const String& data) {
  unsigned char hmacResult[32];
  
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
  mbedtls_md_hmac_starts(&ctx, (unsigned char*)secret.c_str(), secret.length());
  mbedtls_md_hmac_update(&ctx, (unsigned char*)data.c_str(), data.length());
  mbedtls_md_hmac_finish(&ctx, hmacResult);
  mbedtls_md_free(&ctx);
  
  // Base64 编码
  unsigned char base64Result[64];
  size_t outLen;
  mbedtls_base64_encode(base64Result, sizeof(base64Result), &outLen, hmacResult, 32);
  base64Result[outLen] = 0;
  
  return String((char*)base64Result);
}

// 发送 HTTP/HTTPS 请求的通用函数
int sendHttpRequest(const String& url, const String& method, const String& contentType, const String& body) {
  HTTPClient http;
  WiFiClientSecure localSslClient;
  bool beginOk = false;

  feedPushWatchdog();

  // 判断是否为 HTTPS
  if (url.startsWith("https://")) {
    localSslClient.setInsecure();
    localSslClient.setTimeout(1);
    localSslClient.setHandshakeTimeout(1);
    beginOk = http.begin(localSslClient, url);
  } else {
    beginOk = http.begin(url);
  }

  if (!beginOk) {
    Serial.println("HTTP请求初始化失败");
    stats.pushFailed++;
    feedPushWatchdog();
    return -1;
  }

  http.setConnectTimeout(HTTP_PUSH_TIMEOUT_MS);
  http.setTimeout(HTTP_PUSH_TIMEOUT_MS);

  if (contentType.length() > 0) {
    http.addHeader("Content-Type", contentType);
  }
  
  feedPushWatchdog();

  int httpCode;
  if (method == "GET") {
    httpCode = http.GET();
  } else {
    httpCode = http.POST(body);
  }

  feedPushWatchdog();

  if (httpCode > 0) {
    Serial.printf("HTTP响应码: %d\n", httpCode);
    // 所有 2xx 状态码都视为成功 (200 OK, 201 Created, 202 Accepted, 204 No Content 等)
    if (httpCode >= 200 && httpCode < 300) {
      String response = http.getString();
      Serial.println("响应: " + response.substring(0, 200));  // 限制输出长度
      stats.pushSuccess++;
    } else {
      Serial.println("HTTP错误响应");
      stats.pushFailed++;
    }
  } else {
    Serial.printf("HTTP请求失败: %s\n", http.errorToString(httpCode).c_str());
    stats.pushFailed++;
  }
  
  http.end();
  feedPushWatchdog();
  return httpCode;
}

// 发送单个推送通道
void sendToChannel(const PushChannel& channel, const char* sender, const char* message, const char* timestamp) {
  if (!channel.enabled) return;
  if (channel.url.length() == 0) return;
  
  String channelName = channel.name.length() > 0 ? channel.name : ("通道" + String(channel.type));
  Serial.println("发送到推送通道: " + channelName);
  feedPushWatchdog();

  String senderEscaped = jsonEscape(String(sender));
  String messageEscaped = jsonEscape(String(message));
  String timestampEscaped = jsonEscape(String(timestamp));
  
  switch (channel.type) {
    case PUSH_TYPE_POST_JSON: {
      // 标准 POST JSON 格式
      String jsonData = "{";
      jsonData += "\"sender\":\"" + senderEscaped + "\",";
      jsonData += "\"message\":\"" + messageEscaped + "\",";
      jsonData += "\"timestamp\":\"" + timestampEscaped + "\"";
      jsonData += "}";
      Serial.println("POST JSON: " + jsonData);
      sendHttpRequest(channel.url, "POST", "application/json", jsonData);
      break;
    }
    
    case PUSH_TYPE_BARK: {
      // Bark 推送格式
      String jsonData = "{";
      jsonData += "\"title\":\"" + senderEscaped + "\",";
      jsonData += "\"body\":\"" + messageEscaped + "\"";
      if (channel.key2.length() > 0) {
        jsonData += ",\"group\":\"" + jsonEscape(channel.key2) + "\"";
      }
      jsonData += "}";
      Serial.println("BARK: " + jsonData);
      sendHttpRequest(channel.url, "POST", "application/json", jsonData);
      break;
    }
    
    case PUSH_TYPE_GET: {
      // GET 请求，参数放 URL 里
      String getUrl = channel.url;
      if (getUrl.indexOf('?') == -1) {
        getUrl += "?";
      } else {
        getUrl += "&";
      }
      getUrl += "sender=" + urlEncode(String(sender));
      getUrl += "&message=" + urlEncode(String(message));
      getUrl += "&timestamp=" + urlEncode(String(timestamp));
      Serial.println("GET: " + getUrl);
      sendHttpRequest(getUrl, "GET", "", "");
      break;
    }
    
    case PUSH_TYPE_CUSTOM: {
      // 自定义模板
      if (channel.customBody.length() == 0) {
        Serial.println("自定义模板为空，跳过");
        return;
      }
      String body = channel.customBody;
      body.replace("{sender}", senderEscaped);
      body.replace("{message}", messageEscaped);
      body.replace("{timestamp}", timestampEscaped);
      Serial.println("自定义: " + body);
      sendHttpRequest(channel.url, "POST", "application/json", body);
      break;
    }
    
    case PUSH_TYPE_TELEGRAM: {
      // Telegram Bot 推送
      // URL格式: https://api.telegram.org/bot<TOKEN>/sendMessage
      // 使用 MarkdownV2 模式，需要转义特殊字符
      String senderTg = telegramEscape(String(sender));
      String messageTg = telegramEscape(String(message));
      String timestampTg = telegramEscape(String(timestamp));
      String text = "📱 *来自: " + senderTg + "*\n" + messageTg + "\n\n_" + timestampTg + "_";
      String jsonData = "{";
      jsonData += "\"chat_id\":\"" + channel.key1 + "\",";
      jsonData += "\"text\":\"" + jsonEscape(text) + "\",";
      jsonData += "\"parse_mode\":\"MarkdownV2\"";
      jsonData += "}";
      Serial.println("Telegram: " + jsonData);
      sendHttpRequest(channel.url, "POST", "application/json", jsonData);
      break;
    }
    
    case PUSH_TYPE_WECOM: {
      // 企业微信机器人 (Webhook)
      // URL格式: https://qyapi.weixin.qq.com/cgi-bin/webhook/send?key=xxx
      String content = "📱 来自: " + String(sender) + "\n" + String(message) + "\n\n" + String(timestamp);
      String jsonData = "{";
      jsonData += "\"msgtype\":\"text\",";
      jsonData += "\"text\":{\"content\":\"" + jsonEscape(content) + "\"}";
      jsonData += "}";
      Serial.println("企业微信: " + jsonData);
      sendHttpRequest(channel.url, "POST", "application/json", jsonData);
      break;
    }
    
    case PUSH_TYPE_DINGTALK: {
      // 钉钉机器人 (Webhook)
      // URL格式: https://oapi.dingtalk.com/robot/send?access_token=xxx
      // 如果配置了加签密钥（key1），则需要添加签名
      
      String requestUrl = channel.url;
      
      // 检查是否需要加签
      if (channel.key1.length() > 0) {
        // 获取当前时间戳（毫秒）- 使用 millis() 补充真实毫秒精度
        unsigned long long timestampMs = (unsigned long long)time(nullptr) * 1000ULL + (millis() % 1000);
        // 格式化为 13 位时间戳字符串
        char timestampBuf[16];
        snprintf(timestampBuf, sizeof(timestampBuf), "%llu", timestampMs);
        String timestampStr = String(timestampBuf);
        
        // 构造签名字符串
        String stringToSign = timestampStr + "\n" + channel.key1;
        
        // 计算 HMAC-SHA256 签名
        String sign = hmacSha256Base64(channel.key1, stringToSign);
        sign = urlEncode(sign);
        
        // 添加签名参数到 URL
        if (requestUrl.indexOf('?') == -1) {
          requestUrl += "?";
        } else {
          requestUrl += "&";
        }
        requestUrl += "timestamp=" + timestampStr;
        requestUrl += "&sign=" + sign;
        
        Serial.println("钉钉加签URL: " + requestUrl);
      }
      
      String content = "📱 来自: " + String(sender) + "\n" + String(message) + "\n\n" + String(timestamp);
      String jsonData = "{";
      jsonData += "\"msgtype\":\"text\",";
      jsonData += "\"text\":{\"content\":\"" + jsonEscape(content) + "\"}";
      jsonData += "}";
      Serial.println("钉钉: " + jsonData);
      sendHttpRequest(requestUrl, "POST", "application/json", jsonData);
      break;
    }
    
    default:
      Serial.println("未知推送类型");
      return;
  }
}

// 发送短信到所有启用的推送通道
void sendSMSToServer(const char* sender, const char* message, const char* timestamp) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi未连接，跳过推送");
    return;
  }
  
  bool hasEnabledChannel = false;
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (isPushChannelValid(config.pushChannels[i])) {
      hasEnabledChannel = true;
      break;
    }
  }
  
  if (!hasEnabledChannel) {
    Serial.println("无HTTP推送通道");
    return;
  }
  
  Serial.println("\n=== HTTP推送入队 ===");
  int queuedCount = 0;
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (isPushChannelValid(config.pushChannels[i])) {
      feedPushWatchdog();
      String channelName = config.pushChannels[i].name.length() > 0 ? config.pushChannels[i].name : ("通道" + String(config.pushChannels[i].type));
      if (enqueueHttpPush(config.pushChannels[i], sender, message, timestamp)) {
        queuedCount++;
        Serial.println("HTTP推送已入队: " + channelName);
      }
      feedPushWatchdog();
      delay(10); // 短暂让出 CPU，避免请求过快且不拖慢后续通道
    }
  }
  Serial.printf("=== HTTP推送入队完成: %d 个通道 ===\n\n", queuedCount);

  // 保存队列满/内存不足等入队失败统计；HTTP请求结果由后台任务保存
  saveStats();
}

// 获取当前本地时间字符串（中国时区）
String getCurrentTimeString() {
  time_t now = time(nullptr);
  if (now < 100000) return "设备时间未知";

  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}

// 发送邮件通知函数
void sendEmailNotification(const char* subject, const char* body) {
  if (!config.emailEnabled) return;

  if (!enqueueEmail(subject, body)) {
    saveStats();
  }
}

static void sendEmailNotificationNow(const char* subject, const char* body) {
  if (!config.emailEnabled) return;

  if (config.smtpServer.length() == 0 || config.smtpUser.length() == 0 || 
      config.smtpPass.length() == 0 || config.smtpSendTo.length() == 0) {
    Serial.println("邮件配置不完整，跳过发送");
    return;
  }
  
  auto statusCallback = [](SMTPStatus status) {
    Serial.println(status.text);
  };
  smtp.connect(config.smtpServer.c_str(), config.smtpPort, statusCallback);
  if (smtp.isConnected()) {
    smtp.authenticate(config.smtpUser.c_str(), config.smtpPass.c_str(), readymail_auth_password);

    SMTPMessage msg;
    String from = "sms notify <"; from += config.smtpUser; from += ">";
    msg.headers.add(rfc822_from, from.c_str());
    String to = "your_email <"; to += config.smtpSendTo; to += ">";
    msg.headers.add(rfc822_to, to.c_str());
    msg.headers.add(rfc822_subject, subject);
    msg.text.body(body);
    if (time(nullptr) < 100000) {
      configTzTime("CST-8", "ntp.ntsc.ac.cn", "ntp.aliyun.com", "pool.ntp.org");
      while (time(nullptr) < 100000) delay(100);
    }
    msg.timestamp = time(nullptr);
    smtp.send(msg);
    Serial.println("邮件发送完成");
  } else {
    Serial.println("邮件服务器连接失败");
  }
}
