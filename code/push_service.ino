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
#include <freertos/semphr.h>
#include <ctype.h>
#include <new>

// 全局 TLS/SMTP 互斥锁，确保后台邮件任务与外部并发调用时的独占安全 (C4)
static SemaphoreHandle_t smtpMutex = nullptr;

void initSmtpLock() {
  if (smtpMutex == nullptr) {
    smtpMutex = xSemaphoreCreateMutex();
    if (smtpMutex == nullptr) {
      Serial.println("SMTP互斥锁创建失败");
    }
  }
}

// RAII 锁守卫，确保在任何返回路径下自动释放 SMTP 锁
struct SmtpLockGuard {
  SemaphoreHandle_t mutex;
  bool locked;
  SmtpLockGuard(SemaphoreHandle_t m, TickType_t timeout) : mutex(m), locked(false) {
    if (mutex != nullptr && xSemaphoreTake(mutex, timeout) == pdTRUE) {
      locked = true;
    }
  }
  ~SmtpLockGuard() {
    if (locked && mutex != nullptr) {
      xSemaphoreGive(mutex);
    }
  }

  SmtpLockGuard(const SmtpLockGuard&) = delete;
  SmtpLockGuard& operator=(const SmtpLockGuard&) = delete;

  bool isLocked() const { return locked; }
};

// 请求仅在后台 worker 执行；1 秒不足以完成公网 TLS/APNs 往返，不能沿用同步 loop 的预算。
// TCP/响应为毫秒，TLS 握手 API 为秒；仍有界、单 worker，不重发结果不确定的 POST。
static const uint16_t HTTP_PUSH_CONNECT_TIMEOUT_MS = 5000;
static const uint16_t HTTP_PUSH_TIMEOUT_MS = 10000;
static const uint8_t HTTP_PUSH_TLS_HANDSHAKE_TIMEOUT_SEC = 15;
static const uint16_t HTTP_PUSH_PREVIEW_TIMEOUT_MS = 1000;
static const uint8_t HTTP_PUSH_QUEUE_LENGTH = MAX_PUSH_CHANNELS * 2;
static const uint32_t HTTP_PUSH_TASK_STACK_SIZE = 16384;  // HTTPS TLS 握手需 10-16KB 栈
static const uint8_t HTTP_PUSH_WORKER_COUNT = 1;          // 单 Worker 串行消费，节约栈空间并消除多通道并发 TLS 内存暴击 (OOM)
static const uint8_t EMAIL_QUEUE_LENGTH = 4;
static const uint32_t EMAIL_TASK_STACK_SIZE = 16384;     // SMTP TLS 握手需 10-16KB 栈
static const uint32_t SMTP_STAGE_TIMEOUT_MS = 20000;
static const TickType_t SMTP_PROGRESS_DELAY_TICKS = pdMS_TO_TICKS(10);

struct HttpPushJob {
  PushChannel channel;  // 通道快照，确保任务出队时配置一致并避免跨线程读写竞争
  String sender;
  String message;
  String timestamp;

  HttpPushJob() = default;
  HttpPushJob(const HttpPushJob&) = delete;
  HttpPushJob& operator=(const HttpPushJob&) = delete;
};

struct EmailJob {
  String subject;
  String body;
  // 独立持有 SMTP 配置快照，避免后台任务与 Web 更新共享 String。
  String smtpServer;
  String smtpUser;
  String smtpPass;
  String smtpSendTo;
  int smtpPort;

  // 队列仅转移指针所有权；禁止绕过 copyEmailString 的隐式 String 复制。
  EmailJob() = default;
  EmailJob(const EmailJob&) = delete;
  EmailJob& operator=(const EmailJob&) = delete;
};

static QueueHandle_t httpPushQueue = nullptr;
static TaskHandle_t httpPushTaskHandles[HTTP_PUSH_WORKER_COUNT] = {nullptr};
static QueueHandle_t emailQueue = nullptr;
static TaskHandle_t emailTaskHandle = nullptr;

void sendToChannel(const PushChannel& channel, const char* sender, const char* message, const char* timestamp);
static void sendEmailNotificationNow(const EmailJob& job);
// HTTP/邮件快照共用完整复制检查；定义保留在邮件 producer 附近。
static bool copyEmailString(String& target, const char* source, size_t length);

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

  HttpPushJob* job = new (std::nothrow) HttpPushJob();
  if (job == nullptr) {
    Serial.println("HTTP推送任务分配内存失败");
    stats.pushFailed++;
    return false;
  }

  job->channel.enabled = channel.enabled;
  job->channel.type = channel.type;
  const bool copied =
    copyEmailString(job->channel.name, channel.name.c_str(), channel.name.length()) &&
    copyEmailString(job->channel.url, channel.url.c_str(), channel.url.length()) &&
    copyEmailString(job->channel.key1, channel.key1.c_str(), channel.key1.length()) &&
    copyEmailString(job->channel.key2, channel.key2.c_str(), channel.key2.length()) &&
    copyEmailString(job->channel.customBody, channel.customBody.c_str(), channel.customBody.length()) &&
    copyEmailString(job->sender, sender, sender ? strlen(sender) : 0) &&
    copyEmailString(job->message, message, message ? strlen(message) : 0) &&
    copyEmailString(job->timestamp, timestamp, timestamp ? strlen(timestamp) : 0);
  if (!copied) {
    Serial.println("HTTP推送快照复制失败，丢弃本次通道推送");
    delete job;
    stats.pushFailed++;
    return false;
  }
  // 复制来的 URL 可能有粘贴空白；仅规范化任务副本，不修改持久化配置或短信正文。
  job->channel.url.trim();
  if (job->channel.url.length() == 0) {
    Serial.println("HTTP推送URL为空，丢弃本次通道推送");
    delete job;
    stats.pushFailed++;
    return false;
  }

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
      sendEmailNotificationNow(*job);
      saveStats();
      delete job;
      job = nullptr;
      delay(10);
    }
  }
}

static bool waitForSmtpProgress(const char* stage, unsigned long stageStart) {
  while (smtp.isProcessing()) {
    if (millis() - stageStart >= SMTP_STAGE_TIMEOUT_MS) {
      Serial.printf("SMTP事务超时，阶段: %s\n", stage);
      recordOperationDiagnostic("smtp", stage);
      smtp.stop();
      ssl_client.stop();
      return false;
    }

    smtp.loop();
    vTaskDelay(SMTP_PROGRESS_DELAY_TICKS);
  }

  SMTPStatus status = smtp.status();
  if (status.errorCode != 0 || status.statusCode >= 400) {
    Serial.printf(
      "SMTP阶段失败: %s, error=%d, status=%d\n",
      stage,
      status.errorCode,
      status.statusCode
    );
    return false;
  }

  return true;
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

static bool copyEmailString(String& target, const char* source, size_t length) {
  // String 的复制/拼接可能静默失败；不构造临时 String，并核对完整字节数。
  target.remove(0);
  return source != nullptr && target.reserve(length) &&
         target.concat(source, length) && target.length() == length;
}

static bool enqueueEmail(const char* subject, const char* body) {
  // 仅主任务调用，与 Web 更新串行；入队后配置变更或禁用不影响已排队邮件。
  if (!config.emailEnabled) return true;

  if (!ensureEmailWorker()) {
    stats.pushFailed++;
    return false;
  }

  EmailJob* job = new (std::nothrow) EmailJob();
  if (job == nullptr) {
    Serial.println("邮件任务分配内存失败");
    stats.pushFailed++;
    return false;
  }

  const bool copied =
    copyEmailString(job->subject, subject, subject ? strlen(subject) : 0) &&
    copyEmailString(job->body, body, body ? strlen(body) : 0) &&
    copyEmailString(job->smtpServer, config.smtpServer.c_str(), config.smtpServer.length()) &&
    copyEmailString(job->smtpUser, config.smtpUser.c_str(), config.smtpUser.length()) &&
    copyEmailString(job->smtpPass, config.smtpPass.c_str(), config.smtpPass.length()) &&
    copyEmailString(job->smtpSendTo, config.smtpSendTo.c_str(), config.smtpSendTo.length());
  if (!copied) {
    Serial.println("邮件快照复制失败，丢弃本次邮件通知");
    delete job;
    stats.pushFailed++;
    return false;
  }

  // 完整复制后就地 trim，不修改 config，也不让 worker 再复制整份快照。
  job->smtpServer.trim();
  job->smtpUser.trim();
  job->smtpPass.trim();
  job->smtpSendTo.trim();
  job->smtpPort = config.smtpPort;

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
  encoded.reserve(str.length() * 3);  // P2: 预分配最坏情况空间，避免逐字符扩容
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
  result.reserve(str.length() + str.length() / 4 + 8);  // P2: 预分配空间，减少堆碎片
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
  result.reserve(str.length() * 2);  // P2: 预分配空间
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
  // 局部对象按声明逆序析构，确保 http 先释放其引用的 TLS 客户端。
  WiFiClientSecure localSslClient;
  HTTPClient http;
  bool beginOk = false;
  const unsigned long requestStart = millis();

  feedPushWatchdog();

  // 判断是否为 HTTPS
  if (url.startsWith("https://")) {
    localSslClient.setInsecure();
    localSslClient.setTimeout(HTTP_PUSH_TIMEOUT_MS);
    localSslClient.setHandshakeTimeout(HTTP_PUSH_TLS_HANDSHAKE_TIMEOUT_SEC);
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

  http.setReuse(false);  // 每个任务独立连接；不为短响应空等 keep-alive。
  http.setConnectTimeout(HTTP_PUSH_CONNECT_TIMEOUT_MS);
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
    Serial.printf("HTTP响应码: %d，耗时: %lums\n", httpCode, millis() - requestStart);
    // 错误响应同样读取有界诊断；不调用无界 getString()，不原样打印可能回显密钥的远端正文。
    // chunked/未知长度仅作为原始预览，不把预览当作完整 JSON 或手机送达回执。
    NetworkClient* responseStream = http.getStreamPtr();
    if (responseStream != nullptr) {
      const size_t previewLimit = 256;
      const int responseLength = http.getSize();
      const size_t toPreview = responseLength >= 0 ? min((size_t)responseLength, previewLimit) : previewLimit;
      char preview[previewLimit + 1];
      size_t previewLength = 0;
      unsigned long previewStart = millis();
      while (previewLength < toPreview && millis() - previewStart < HTTP_PUSH_PREVIEW_TIMEOUT_MS) {
        int available = responseStream->available();
        if (available <= 0) {
          if (!responseStream->connected()) break;
          yield();
          delay(1);
          continue;
        }
        size_t toRead = min((size_t)available, toPreview - previewLength);
        int readLength = responseStream->readBytes(preview + previewLength, toRead);
        if (readLength <= 0) break;
        previewLength += (size_t)readLength;
      }
      preview[previewLength] = '\0';
      Serial.printf("HTTP响应预览: %u 字节（原文不写日志）\n", (unsigned int)previewLength);
      // 仅输出固件内置的错误标记，不输出未知远端文本、URL、设备 Key 或短信内容。
      const char* knownErrors[] = {"BadDeviceToken", "DeviceTokenNotForTopic", "Unregistered",
                                   "MissingDeviceToken", "TooManyRequests"};
      for (const char* error : knownErrors) {
        if (strstr(preview, error) != nullptr) {
          Serial.printf("推送服务错误标记: %s\n", error);
          break;
        }
      }
    }
    // 计数表示 HTTP 接受请求，不表示手机已经显示通知。
    if (httpCode >= 200 && httpCode < 300) {
      stats.pushSuccess++;
    } else {
      Serial.println(httpCode >= 300 && httpCode < 400
        ? "HTTP重定向未跟随，请配置最终推送URL" : "HTTP错误响应，请检查状态码、错误标记及通道配置");
      stats.pushFailed++;
    }
  } else {
    Serial.printf("HTTP请求失败: %s，耗时: %lums（连接/TLS或响应失败，未自动重发）\n",
                  http.errorToString(httpCode).c_str(), millis() - requestStart);
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
      Serial.println("Bark: 提交推送请求");
      int code = sendHttpRequest(channel.url, "POST", "application/json", jsonData);
      if (code >= 200 && code < 300) {
        Serial.println("Bark: HTTP请求已接受；手机是否显示通知仍需在客户端确认");
      }
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
      String text = "\xF0\x9F\x93\xB1 *来自: " + senderTg + "*\n" + messageTg + "\n\n_" + timestampTg + "_";
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
      String content = "\xF0\x9F\x93\xB1 来自: " + String(sender) + "\n" + String(message) + "\n\n" + String(timestamp);
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
      
      String content = "\xF0\x9F\x93\xB1 来自: " + String(sender) + "\n" + String(message) + "\n\n" + String(timestamp);
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
  if (!enqueueEmail(subject, body)) {
    saveStats();
  }
}

static bool buildEmailAddressHeader(String& header, const char* prefix, const char* address, size_t length) {
  const size_t expectedLength = strlen(prefix) + length + 1;  // 末尾的 '>'
  header.remove(0);
  return header.reserve(expectedLength) && header.concat(prefix) &&
         header.concat(address, length) && header.concat(">") && header.length() == expectedLength;
}

static void sendEmailNotificationNow(const EmailJob& job) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi未连接，跳过邮件发送");
    stats.pushFailed++;
    return;
  }

  const String& smtpServer = job.smtpServer;
  const String& smtpUser = job.smtpUser;
  const String& smtpPass = job.smtpPass;
  const String& smtpSendTo = job.smtpSendTo;
  const int smtpPort = job.smtpPort > 0 ? job.smtpPort : 465;

  if (smtpServer.length() == 0 || smtpUser.length() == 0 ||
      smtpPass.length() == 0 || smtpSendTo.length() == 0) {
    Serial.println("邮件配置不完整，跳过发送");
    stats.pushFailed++;
    return;
  }

  // 在建立 SMTP 连接之前检查 NTP 时间同步，避免连接建立后空等导致服务端断开，
  // 且即使 NTP 超时也继续使用 ReadyMail 默认时间戳发送，不丢弃邮件通知
  if (time(nullptr) < 100000) {
    configTzTime("CST-8", "ntp.ntsc.ac.cn", "ntp.aliyun.com", "pool.ntp.org");
    unsigned long syncStart = millis();
    while (time(nullptr) < 100000 && millis() - syncStart < 8000) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (time(nullptr) < 100000) {
      Serial.println("警告: NTP尚未完成同步，将使用默认时间戳继续发送邮件");
    }
  }

  // 获取 SMTP 独占锁，防止多任务/多线程并发调用导致 TLS/SMTP 崩溃 (C4)
  if (smtpMutex == nullptr) initSmtpLock();
  SmtpLockGuard lock(smtpMutex, pdMS_TO_TICKS(35000));
  if (!lock.isLocked()) {
    Serial.println("SMTP互斥锁获取超时，跳过本次邮件发送");
    stats.pushFailed++;
    return;
  }

  auto statusCallback = [](SMTPStatus status) {
    Serial.println(status.text);
  };

  if (ssl_client.stillInPlainStart() && smtpPort != 587 && smtpPort != 25) {
    ssl_client = WiFiClientSecure();
  }
  ssl_client.setInsecure();
  ssl_client.setConnectionTimeout(10000);
  ssl_client.setTimeout(10000);
  ssl_client.setHandshakeTimeout(15);

  bool useSsl = (smtpPort != 25);
  if (smtpPort == 587) {
    ssl_client.setPlainStart();
    smtp.setStartTLS([](bool &success) {
      success = ssl_client.startTLS() != 0;
    }, true);
  } else if (smtpPort == 25) {
    ssl_client.setPlainStart();
    smtp.setStartTLS(NULL, false);
  } else {
    smtp.setStartTLS(NULL, false);
  }

  recordOperationDiagnostic("smtp", "connect");
  unsigned long stageStart = millis();
  bool started = smtp.connect(smtpServer, smtpPort, statusCallback, useSsl, false);
  if (!started || !waitForSmtpProgress("connect", stageStart) || !smtp.isConnected()) {
    SMTPStatus st = smtp.status();
    Serial.printf("邮件服务器连接失败 (error=%d, status=%d)\n", st.errorCode, st.statusCode);
    smtp.stop();
    ssl_client.stop();
    clearOperationDiagnostic();
    stats.pushFailed++;
    return;
  }

  recordOperationDiagnostic("smtp", "authenticate");
  stageStart = millis();
  started = smtp.authenticate(smtpUser, smtpPass, readymail_auth_password, false);
  if (!started || !waitForSmtpProgress("authenticate", stageStart) || !smtp.isAuthenticated()) {
    SMTPStatus st = smtp.status();
    Serial.printf("SMTP认证失败 (error=%d, status=%d)\n", st.errorCode, st.statusCode);
    smtp.stop();
    ssl_client.stop();
    clearOperationDiagnostic();
    stats.pushFailed++;
    return;
  }

  // ReadyMail 异步发送模式 (await=false) 强制要求使用 smtp.getMessage() 内部消息对象，
  // 否则 sendImpl() 会因 local_msg.headers.size() == 0 直接报错 -109 (SMTP_ERROR_UNINITIALIZE_LOCAL_SMTP_MESSAGE)
  SMTPMessage &msg = smtp.getMessage();
  msg.clear();

  String addressHeader;
  bool headersOk = buildEmailAddressHeader(addressHeader, "sms notify <", smtpUser.c_str(), smtpUser.length());
  if (headersOk) msg.headers.add(rfc822_from, addressHeader);

  // 直接遍历 const 快照中的逗号/分号分隔项，复用地址头，不复制整串或 substring。
  const char* recipients = smtpSendTo.c_str();
  const size_t recipientsLength = smtpSendTo.length();
  size_t startIdx = 0;
  while (headersOk && startIdx < recipientsLength) {
    size_t endIdx = startIdx;
    while (endIdx < recipientsLength && recipients[endIdx] != ',' && recipients[endIdx] != ';') endIdx++;
    size_t first = startIdx;
    size_t last = endIdx;
    while (first < last && isspace((unsigned char)recipients[first])) first++;
    while (last > first && isspace((unsigned char)recipients[last - 1])) last--;
    if (first < last) {
      headersOk = buildEmailAddressHeader(addressHeader, "your_email <", recipients + first, last - first);
      if (headersOk) msg.headers.add(rfc822_to, addressHeader);
    }
    startIdx = endIdx + 1;
  }
  if (!headersOk) {
    Serial.println("邮件地址头分配内存失败");
    msg.clear();
    smtp.stop();
    ssl_client.stop();
    clearOperationDiagnostic();
    stats.pushFailed++;
    return;
  }

  // ReadyMail 的 String 形参直接引用已校验快照，不通过 c_str() 隐式分配临时 String。
  msg.headers.add(rfc822_subject, job.subject);
  // 静态正文重载不复制；worker 在发送结束并 clear/stop 后才释放 job。
  msg.text.body(job.body.c_str(), job.body.length());

  time_t now = time(nullptr);
  msg.timestamp = (now >= 100000) ? (uint32_t)now : 0;

  recordOperationDiagnostic("smtp", "send");
  stageStart = millis();
  started = smtp.send(msg, "", false);
  if (started && waitForSmtpProgress("send", stageStart) && smtp.status().isComplete) {
    Serial.println("邮件发送完成");
    stats.pushSuccess++;
  } else {
    SMTPStatus st = smtp.status();
    Serial.printf("邮件发送失败 (error=%d, status=%d)\n", st.errorCode, st.statusCode);
    stats.pushFailed++;
  }

  msg.clear();
  smtp.stop();
  ssl_client.stop();
  clearOperationDiagnostic();
}