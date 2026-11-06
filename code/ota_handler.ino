/*
 * ota_handler.ino - 固件在线升级（OTA）
 *
 * 设计要点：
 * 1. 分区：依赖 code/partitions.csv 的双 app 槽（app0/app1 + otadata）。
 *    单槽固件（Huge App）没有第二个槽，上传接口会返回 400 并提示先用 USB 刷双槽分区表，
 *    不会误写当前运行分区。
 * 2. 上传：网页以 application/octet-stream 直接 POST 原始 bin；WebServer 的请求体回调
 *    按 1436 字节块交给 Update，不在 RAM 里缓存整包（设备只有 ~120KB 可用堆）。
 * 3. 拉取：/updateurl 与 MQTT {"action":"ota","url":...} 共用 otaRunUrlUpdate()，
 *    仅支持 http://（HTTPS 需要额外 TLS 内存与证书处理，收益不足）。
 * 4. 安全网：写入成功后置 NVS 试用标记；新固件若连续 3 次启动都未确认（15 秒内没有
 *    联网/开热点），自动把启动分区切回旧槽并重启。Arduino 预编译 bootloader 默认未开启
 *    CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE，因此这是固件层回滚，不是 bootloader 回滚。
 * 5. 升级期间继续分流模组 URC，避免漏收短信；每块都喂 30 秒看门狗。
 */

#include "ota_handler.h"

#include <Update.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "config.h"
#include "json_input.h"
#include "modem_handler.h"
#include "sms_handler.h"
#include "web_handlers.h"
#include "push_service.h"
#include "mqtt_handler.h"

namespace {

const char* OTA_NVS_NAMESPACE = "sms_ota";
const uint8_t OTA_TRIAL_MAX_BOOTS = 3;         // 新固件最多试启动次数
const unsigned long OTA_TRIAL_CONFIRM_MS = 15000;  // 启动后多久视为健康
const unsigned long OTA_IDLE_TIMEOUT_MS = 20000;   // 单次无数据等待上限
const unsigned long OTA_REBOOT_DELAY_MS = 1500;    // 先回 HTTP 响应再重启
const size_t OTA_CHUNK_BYTES = 2048;
const size_t OTA_URL_MAX_BYTES = 256;

bool otaRebootPending = false;
unsigned long otaRebootAt = 0;
bool otaTrialPending = false;
String otaPendingUrl;
char otaLastError[96] = {0};

// 网页上传状态：WebServer 在解析请求体时就会回调，早于最终处理函数。
bool otaUploadActive = false;
bool otaUploadFinished = false;
size_t otaUploadBytes = 0;

// 放 BSS 而不是 loopTask 栈：loopTask 栈默认 8KB，还要留给 JSON/HTML 处理。
uint8_t otaBuffer[OTA_CHUNK_BYTES];

void otaSetError(const char* text) {
  snprintf(otaLastError, sizeof(otaLastError), "%s", text ? text : "");
}

void otaSetUpdateError() {
  const char* reason = Update.errorString();
  snprintf(otaLastError, sizeof(otaLastError), "写入失败: %s", reason ? reason : "未知");
}

void otaClearError() {
  otaLastError[0] = 0;
}

// 另一槽首字节为 0xE9 说明存在 ESP 应用镜像；空槽或擦除态不能作为回滚目标。
bool otaOtherSlotHasImage(const esp_partition_t* other) {
  if (!other) return false;
  uint8_t magic = 0;
  return esp_partition_read(other, 0, &magic, 1) == ESP_OK && magic == 0xE9;
}

bool otaRevertToOtherSlot() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  if (!running || !other || other == running) return false;
  if (!otaOtherSlotHasImage(other)) return false;
  return esp_ota_set_boot_partition(other) == ESP_OK;
}

// 写入一块固件并维持系统服务：喂 loopTask 看门狗、让 idle 任务跑一次（idle 也订阅了
// 30 秒 TWDT，纯忙等会被判饥饿重启）、继续分流模组 URC。
bool otaWriteBytes(const uint8_t* data, size_t size) {
  if (Update.write(const_cast<uint8_t*>(data), size) != size) {
    otaSetUpdateError();
    return false;
  }
  esp_task_wdt_reset();
  delay(1);
  if (!modemBusy()) checkSerial1URC();
  return true;
}

// 把连接里的固件流写进 OTA 分区。
// expected == 0 表示长度未知（chunked），读到连接关闭为止。
bool otaPumpClient(Client& client, size_t expected) {
  const bool requireLength = expected > 0;
  size_t total = 0;
  unsigned long lastData = millis();

  while (!requireLength || total < expected) {
    int available = client.available();
    if (available > 0) {
      size_t want = (size_t)available > sizeof(otaBuffer) ? sizeof(otaBuffer) : (size_t)available;
      int readBytes = client.read(otaBuffer, want);
      if (readBytes <= 0) continue;
      if (!otaWriteBytes(otaBuffer, (size_t)readBytes)) return false;
      total += (size_t)readBytes;
      lastData = millis();
      continue;
    }

    if (!client.connected()) break;
    if (millis() - lastData > OTA_IDLE_TIMEOUT_MS) {
      otaSetError("接收超时，已中止升级");
      return false;
    }
    delay(1);
    esp_task_wdt_reset();
  }

  if (requireLength && total != expected) {
    snprintf(otaLastError, sizeof(otaLastError), "接收不完整: %u/%u 字节",
             (unsigned int)total, (unsigned int)expected);
    return false;
  }
  if (total == 0) {
    otaSetError("没有收到固件数据");
    return false;
  }
  return true;
}

// 结束写入、置试用标记并安排重启；返回 false 时调用方负责 Update.abort()。
bool otaFinishWrite() {
  if (!Update.end(true)) {
    otaSetUpdateError();
    return false;
  }

  Preferences prefs;
  if (prefs.begin(OTA_NVS_NAMESPACE, false)) {
    prefs.putUChar("trial", 1);
    prefs.putUChar("boots", 0);
    prefs.end();
  }
  otaClearError();
  otaRebootPending = true;
  otaRebootAt = millis() + OTA_REBOOT_DELAY_MS;
  Serial.println("OTA 固件写入完成，即将重启");
  return true;
}

bool otaUrlLooksValid(const String& url) {
  if (url.length() < 8 || url.length() > OTA_URL_MAX_BYTES) return false;
  if (!url.startsWith("http://")) return false;
  for (unsigned int i = 0; i < url.length(); ++i) {
    char c = url.charAt(i);
    bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') ||
                   c == ':' || c == '/' || c == '.' || c == '-' || c == '_' ||
                   c == '~' || c == '%' || c == '?' || c == '=' || c == '&' ||
                   c == '#' || c == '+' || c == '@' || c == ',';
    if (!allowed) return false;
  }
  return true;
}

// 阻塞式下载；只在 loop 中调用（HTTP 请求可能持续十几秒，期间靠喂狗与 URC 分流保活）。
bool otaRunUrlUpdate(const String& url) {
  if (!otaDualSlotSupported()) {
    otaSetError("当前分区表没有第二个 app 槽，需先用 USB 刷入双槽分区表");
    return false;
  }
  const esp_partition_t* slot = esp_ota_get_next_update_partition(nullptr);
  if (!slot) {
    otaSetError("找不到可写的 OTA 分区");
    return false;
  }

  Serial.print("OTA 从 URL 升级: ");
  Serial.println(url);
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(15000);
  http.setTimeout(20000);
  http.setReuse(false);

  if (!http.begin(client, url)) {
    otaSetError("升级 URL 无法解析");
    return false;
  }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    snprintf(otaLastError, sizeof(otaLastError), "下载失败: HTTP %d", code);
    http.end();
    return false;
  }

  int declared = http.getSize();
  if (declared > 0 && (size_t)declared > slot->size) {
    snprintf(otaLastError, sizeof(otaLastError), "固件过大: %d > %u 字节",
             declared, (unsigned int)slot->size);
    http.end();
    return false;
  }
  if (!Update.begin(declared > 0 ? (size_t)declared : UPDATE_SIZE_UNKNOWN, U_FLASH)) {
    otaSetUpdateError();
    http.end();
    return false;
  }

  bool ok = otaPumpClient(*http.getStreamPtr(), declared > 0 ? (size_t)declared : 0);
  if (ok) ok = otaFinishWrite();
  if (!ok) Update.abort();
  http.end();
  return ok;
}

}  // namespace

bool otaDualSlotSupported() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!running) return false;
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  return next != nullptr && next != running;
}

void otaInit() {
  otaTrialPending = false;
  if (!otaDualSlotSupported()) {
    Serial.println("OTA: 当前为单 app 槽分区，暂不支持在线升级");
    return;
  }

  Preferences prefs;
  if (!prefs.begin(OTA_NVS_NAMESPACE, false)) return;
  const uint8_t trial = prefs.getUChar("trial", 0);
  uint8_t boots = prefs.getUChar("boots", 0);
  if (trial != 1) {
    prefs.end();
    return;
  }

  if (boots < 255) boots++;
  prefs.putUChar("boots", boots);
  otaTrialPending = true;

  if (boots >= OTA_TRIAL_MAX_BOOTS) {
    prefs.putUChar("trial", 0);
    prefs.putUChar("boots", 0);
    prefs.end();
    if (otaRevertToOtherSlot()) {
      Serial.printf("OTA 新固件连续 %u 次未确认，回滚到另一分区并重启\n", (unsigned int)boots);
      delay(100);
      ESP.restart();
    }
    Serial.println("OTA 新固件未确认，但另一分区没有有效镜像，继续启动当前固件");
    return;
  }

  prefs.end();
  Serial.printf("OTA 试用固件：第 %u/%u 次启动，%lu 秒后自动确认\n",
                (unsigned int)boots, (unsigned int)OTA_TRIAL_MAX_BOOTS,
                OTA_TRIAL_CONFIRM_MS / 1000UL);
}

void otaLoop() {
  // 新固件只要跑到这里并且联网/开热点满 15 秒，就撤销试用标记，避免以后误回滚。
  if (otaTrialPending && millis() >= OTA_TRIAL_CONFIRM_MS &&
      (WiFi.status() == WL_CONNECTED || (WiFi.getMode() & WIFI_AP))) {
    Preferences prefs;
    if (prefs.begin(OTA_NVS_NAMESPACE, false)) {
      if (prefs.getUChar("trial", 0) == 1) {
        prefs.putUChar("trial", 0);
        prefs.putUChar("boots", 0);
        Serial.println("OTA 新固件已确认健康");
      }
      prefs.end();
    }
    otaTrialPending = false;
  }

  if (otaRebootPending && (long)(millis() - otaRebootAt) >= 0) {
    otaRebootPending = false;
    Serial.flush();
    ESP.restart();
  }

  if (otaPendingUrl.length() > 0 && !modemBusy()) {
    String url = otaPendingUrl;
    otaPendingUrl = "";
    bool ok = otaRunUrlUpdate(url);
    if (!ok) {
      Serial.print("OTA URL 升级失败: ");
      Serial.println(otaLastError);
      publishErrorLog("ota", "otaRunUrlUpdate", "URL 固件升级失败", otaLastError);
    }
  }
}

bool otaQueueUrl(const String& url) {
  if (!otaUrlLooksValid(url)) return false;
  if (!otaDualSlotSupported()) return false;
  otaPendingUrl = url;
  return true;
}

String otaInfoJson() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  const bool dual = otaDualSlotSupported();

  String json;
  json.reserve(384);
  json += "{\"success\":true,\"version\":\"" FW_VERSION "\",";
  json += "\"running\":\"" + String(running ? running->label : "unknown") + "\",";
  json += "\"slotBytes\":" + String(running ? (unsigned long)running->size : 0UL) + ",";
  json += "\"dualSlot\":" + String(dual ? "true" : "false") + ",";
  json += "\"otherSlotValid\":" + String(dual && otaOtherSlotHasImage(other) ? "true" : "false") + ",";
  json += "\"sketchBytes\":" + String((unsigned long)ESP.getSketchSize()) + ",";
  json += "\"freeSketchBytes\":" + String((unsigned long)ESP.getFreeSketchSpace()) + ",";
  json += "\"freeHeap\":" + String((unsigned long)ESP.getFreeHeap()) + ",";
  json += "\"lastError\":\"" + jsonEscape(String(otaLastError)) + "\"";
  json += "}";
  return json;
}

void handleOtaInfo() {
  if (!checkAuth()) return;
  server.send(200, "application/json", otaInfoJson());
}

// 请求体开始：认证、槽容量与 Update 初始化。失败只记录原因，最终处理函数负责回包。
void otaBeginUpload(bool unknownSize, size_t contentLength) {
  otaUploadActive = false;
  otaUploadFinished = false;
  otaUploadBytes = 0;
  otaClearError();

  // 这里不能调用 checkAuth()：它在失败时会直接回 401，而此刻请求体还在发送。
  if (!server.authenticate(config.webUser.c_str(), config.webPass.c_str())) {
    otaSetError("认证失败");
    return;
  }
  if (!otaDualSlotSupported()) {
    otaSetError("当前分区表只有一个 app 槽，请先用 USB 刷入双槽分区表");
    return;
  }
  const esp_partition_t* slot = esp_ota_get_next_update_partition(nullptr);
  if (!slot) {
    otaSetError("找不到可写的 OTA 分区");
    return;
  }
  // multipart 的 Content-Length 含边界与头尾开销，留 4KB 余量再判超限。
  if (contentLength == 0 || contentLength > slot->size + (unknownSize ? 4096 : 0)) {
    otaSetError("固件超过 OTA 槽容量");
    return;
  }

  const size_t expected = unknownSize ? UPDATE_SIZE_UNKNOWN : contentLength;
  if (!Update.begin(expected, U_FLASH)) {
    otaSetUpdateError();
    return;
  }
  otaUploadActive = true;
  Serial.printf("OTA 上传开始(%s): %u 字节 -> %s\n",
                unknownSize ? "multipart" : "raw",
                (unsigned int)contentLength, slot->label);
}

void otaWriteChunk(const uint8_t* data, size_t size) {
  if (!otaUploadActive || size == 0) return;
  if (!otaWriteBytes(data, size)) {
    Update.abort();
    otaUploadActive = false;
    return;
  }
  otaUploadBytes += size;
}

void otaFinishUpload() {
  if (!otaUploadActive) return;
  otaUploadActive = false;
  Serial.printf("OTA 上传结束: 已写入 %u 字节\n", (unsigned int)otaUploadBytes);
  otaUploadFinished = otaFinishWrite();
  if (!otaUploadFinished) Update.abort();
}

void otaAbortUpload(const char* reason) {
  if (otaUploadActive) Update.abort();
  otaUploadActive = false;
  otaSetError(reason);
}

void handleOtaUpload() {
  if (!checkAuth()) return;

  // 空 body 时 WebServer 不会回调请求体处理函数，先排除掉，避免误读上一次上传的残留状态。
  if (server.clientContentLength() == 0) {
    server.send(400, "application/json",
                "{\"success\":false,\"message\":\"未收到固件数据\"}");
    return;
  }

  if (otaUploadFinished) {
    server.send(200, "application/json",
                "{\"success\":true,\"message\":\"固件写入成功，设备即将重启\"}");
    return;
  }

  // 没有进入 upload 回调（空 body）或中途失败：给出具体原因，不假装成功。
  String reason = otaLastError[0] ? String(otaLastError) : String("未收到固件数据");
  String message = "{\"success\":false,\"message\":\"" + jsonEscape(reason) + "\"}";
  server.send(400, "application/json", message);
}

// WebServer 在解析请求体阶段回调：raw（application/octet-stream）与 multipart 共用同一个
// upload 函数指针，因此必须按 Content-Type 区分，否则会解引用另一个未初始化的结构。
void handleOtaUploadChunk() {
  const bool multipart = server.header("Content-Type").startsWith("multipart/");

  if (multipart) {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      otaBeginUpload(true, server.clientContentLength());
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      otaWriteChunk(upload.buf, upload.currentSize);
    } else if (upload.status == UPLOAD_FILE_END) {
      otaFinishUpload();
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
      otaAbortUpload("上传被中断");
    }
    return;
  }

  HTTPRaw& raw = server.raw();
  if (raw.status == RAW_START) {
    otaBeginUpload(false, server.clientContentLength());
  } else if (raw.status == RAW_WRITE) {
    otaWriteChunk(raw.buf, raw.currentSize);
  } else if (raw.status == RAW_END) {
    otaFinishUpload();
  } else if (raw.status == RAW_ABORTED) {
    otaAbortUpload("上传被中断");
  }
}

void handleOtaUrl() {
  if (!checkAuth()) return;

  const int length = server.clientContentLength();
  if (length < 2 || (size_t)length > JsonInput::MAX_INPUT_BYTES) {
    server.send(400, "application/json",
                "{\"success\":false,\"message\":\"JSON请求长度无效，最多8192字节\"}");
    return;
  }
  const String& body = server.arg("plain");
  JsonInput input;
  String url;
  if (body.length() != (unsigned int)length || !input.parse(body) ||
      !input.getString("url", url)) {
    server.send(400, "application/json",
                "{\"success\":false,\"message\":\"无效JSON：需要 {\\\"url\\\":\\\"http://...\\\"}\"}");
    return;
  }
  url.trim();
  if (!otaUrlLooksValid(url)) {
    server.send(400, "application/json",
                "{\"success\":false,\"message\":\"升级地址无效：仅支持 http:// 且长度不超过 256 字节\"}");
    return;
  }
  if (!otaDualSlotSupported()) {
    server.send(409, "application/json",
                "{\"success\":false,\"message\":\"当前分区表只有一个 app 槽，请先用 USB 刷入双槽分区表\"}");
    return;
  }

  otaPendingUrl = url;
  server.send(202, "application/json",
              "{\"success\":true,\"message\":\"已接受升级任务，正在下载固件，完成后自动重启\"}");
}
