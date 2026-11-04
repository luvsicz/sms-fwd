/*
 * modem_handler.ino - 主任务唯一的 Serial1 读取器、行分流和 AT 仲裁
 */
#include "modem_handler.h"
#include "sms_handler.h"
#include <ctype.h>
#include <esp_task_wdt.h>

static ModemTransaction* modemOwner = nullptr;
static bool modemReading = false;
static void (*modemIdleHook)() = nullptr;
static bool modemServicing = false;
static char modemLineBuffer[SERIAL_BUFFER_SIZE];
static size_t modemLineLength = 0;
static bool modemDiscardLine = false;
static bool modemPromptSpace = false;
static uint32_t modemGeneration = 0;
static uint32_t modemLineGeneration = 0;

static void modemReportError(const char* reason) {
  // 不从这里调用网络/配置回调；错误在 USB 日志可见，且不输出短信正文。
  Serial.printf("[modem] ERROR: %s\n", reason);
}

void setModemIdleHook(void (*hook)()) {
  modemIdleHook = hook;
}

static void modemIdle() {
  if (esp_task_wdt_status(nullptr) == ESP_OK) esp_task_wdt_reset();
  if (modemIdleHook && !modemServicing && !modemReading) {
    modemServicing = true;
    modemIdleHook();
    modemServicing = false;
  }
  yield();
  delay(1);
}

bool modemBusy() {
  return modemOwner != nullptr || modemReading || modemServicing;
}

static bool modemIsCallListQuery(const String& command) {
  const char* p = command.c_str();
  while (*p == ' ' || *p == '\t') p++;
  for (const char* expected = "AT+CLCC"; *expected; expected++, p++) {
    if (toupper((unsigned char)*p) != *expected) return false;
  }
  while (*p == ' ' || *p == '\t') p++;
  return *p == '\0';
}

void pollModem() {
  if (modemReading) return;
  modemReading = true;
  // 只消费本次已有的字节，连续输入不能令单次 poll 永不返回、饿死事务超时检查。
  int available = Serial1.available();
  while (available-- > 0) {
    int value = Serial1.read();  // 全项目唯一 Serial1.read；禁止另建私有行缓冲读取它。
    if (value < 0) break;
    char c = (char)value;

    if (modemDiscardLine) {
      if (c == '\n') {
        modemDiscardLine = false;
        modemLineLength = 0;
      }
      continue;  // 丢弃到 LF，绝不把尾部十六进制重新当作 PDU。
    }
    if (c == '\r') continue;
    if (modemPromptSpace && (c == ' ' || c == '\t')) continue;
    modemPromptSpace = false;

    // CMGS 的提示符没有 LF；只在行首（允许空白）识别，不匹配行内的 '>'。
    if (c == '>') {
      bool atStart = true;
      for (size_t i = 0; i < modemLineLength; i++) {
        if (modemLineBuffer[i] != ' ' && modemLineBuffer[i] != '\t') atStart = false;
      }
      if (atStart) {
        bool current = modemLineLength == 0 || modemLineGeneration == modemGeneration;
        modemLineLength = 0;
        modemPromptSpace = true;
        if (current && modemOwner != nullptr && modemOwner->started_ &&
            modemOwner->result_ == ModemResult::Pending) {
          modemOwner->acceptPrompt();
        } else {
          // 超时后迟到的提示符也必须取消，不能让下一条 AT 被当成短信输入。
          Serial1.write((uint8_t)0x1B);
          modemReportError("orphan_prompt_cancelled");
        }
        continue;
      }
    }

    if (c == '\n') {
      if (modemLineLength == 0) continue;
      modemLineBuffer[modemLineLength] = '\0';
      String line(modemLineBuffer);
      bool copied = line.length() == modemLineLength;
      modemLineLength = 0;
      if (!copied) {
        resetModemUrc();
        modemReportError("rx_line_allocation_failed");
        if (modemOwner != nullptr) modemOwner->fail(ModemResult::Overflow, "rx_line_lost");
        continue;
      }
      line.trim();
      if (line.length() == 0) continue;
      Serial.println("Debug> " + line);
      // 接收路径先行：即使正在等 OK / prompt，也不吞 CMT/PDU/来电。
      // 接收链只做历史/转发，不发 AT；HTTP/SMTP 已有工作队列，MQTT 仅 publish。
      // NO CARRIER 是来电结束通知，不是 SMS/查询的失败终态，也不占响应容量。
      // CLCC query rows share the URC format: only the pending, matching generation owns them.
      bool callQueryReply = modemOwner != nullptr && modemOwner->started_ &&
                            modemOwner->result_ == ModemResult::Pending &&
                            modemLineGeneration == modemOwner->generation_ &&
                            line.startsWith("+CLCC:") && modemIsCallListQuery(modemOwner->command_);
      if (!callQueryReply && (handleModemUrc(line) || line == "NO CARRIER")) continue;
      if (modemOwner != nullptr && modemLineGeneration == modemOwner->generation_) {
        modemOwner->acceptLine(line);
      }
      continue;
    }

    if (modemLineLength == 0) modemLineGeneration = modemGeneration;
    if (c == '\0' || modemLineLength >= sizeof(modemLineBuffer) - 1) {
      modemDiscardLine = true;
      modemLineLength = 0;
      resetModemUrc();
      modemReportError("rx_line_overflow_or_nul: discarding whole line until LF");
      if (modemOwner != nullptr && modemLineGeneration == modemOwner->generation_) {
        modemOwner->fail(ModemResult::Overflow, "rx_line_lost");
      }
    } else {
      modemLineBuffer[modemLineLength++] = c;
    }
  }
  modemReading = false;
}

void modemDelay(unsigned long durationMs) {
  unsigned long start = millis();
  do {
    pollModem();
    modemIdle();
  } while (millis() - start < durationMs);
}

ModemTransaction::ModemTransaction() {
  if (modemBusy()) {
    result_ = ModemResult::Busy;
    modemReportError("busy: nested AT rejected");
    return;
  }
  modemOwner = this;
  owns_ = true;
}

ModemTransaction::~ModemTransaction() {
  end();
}

void ModemTransaction::fail(ModemResult result, const char* reason) {
  if (result_ != ModemResult::Pending) return;
  result_ = result;
  modemReportError(reason);
}

bool ModemTransaction::synchronizeRx() {
  unsigned long start = millis();
  // poll 只读一次快照；回调/串口续到的字节必须在旧代继续派发，不清空 partial 行/URC。
  for (;;) {
    pollModem();
    if (result_ != ModemResult::Pending) return false;
    if (Serial1.available() == 0) return true;
    if (millis() - start >= MODEM_RX_SYNC_MS) {
      fail(ModemResult::Busy, "rx_not_synchronized");
      return false;
    }
    modemIdle();
  }
}

bool ModemTransaction::begin(const char* command, const char* requiredPrefix, bool expectPrompt) {
  if (!owns_ || started_ || result_ != ModemResult::Pending) return false;
  if (command == nullptr || command[0] == '\0' || strlen(command) > 256 ||
      strchr(command, '\r') || strchr(command, '\n') || strchr(command, 0x1A) || strchr(command, 0x1B)) {
    fail(ModemResult::Invalid, "invalid_command");
    return false;
  }
  command_ = command;
  requiredPrefix_ = requiredPrefix ? requiredPrefix : "";
  if (command_.length() != strlen(command) ||
      requiredPrefix_.length() != (requiredPrefix ? strlen(requiredPrefix) : 0)) {
    fail(ModemResult::Overflow, "command_allocation_failed");
    return false;
  }

  // 先派发已有 RX 而非清空。未完成行保留旧 generation，不能充当本次命令的响应。
  if (!synchronizeRx()) return false;
  generation_ = ++modemGeneration;
  started_ = true;
  expectPrompt_ = expectPrompt;
  inputPending_ = expectPrompt;
  Serial1.println(command_);
  return true;
}

static bool modemErrorLine(const String& line) {
  return line == "ERROR" || line.startsWith("+CME ERROR:") || line.startsWith("+CMS ERROR:") ||
         line == "NO ANSWER" || line == "BUSY";
}

static bool modemCmgsReceipt(const String& line) {
  if (!line.startsWith("+CMGS:")) return false;
  const char* p = line.c_str() + 6;
  while (*p == ' ' || *p == '\t') p++;
  if (*p < '0' || *p > '9') return false;
  while (*p >= '0' && *p <= '9') p++;
  while (*p == ' ' || *p == '\t') p++;
  return *p == '\0';
}

void ModemTransaction::acceptLine(const String& line) {
  if (!started_ || result_ != ModemResult::Pending || line == command_) return;
  // 迟到的异步命令结果不能污染普通查询。
  if ((line.startsWith("+MPING:") && requiredPrefix_ != "+MPING:") ||
      (line.startsWith("+CMGS:") && requiredPrefix_ != "+CMGS:")) return;

  size_t length = response_.length() + line.length() + 2 + (response_.length() == 0 ? 2 : 0);
  if (length > MODEM_RESPONSE_LIMIT || !response_.reserve(length)) {
    fail(ModemResult::Overflow, "response_limit_exceeded");
    return;
  }
  if (response_.length() == 0) response_ = "\r\n";
  response_ += line;
  response_ += "\r\n";
  if (response_.length() != length) {
    fail(ModemResult::Overflow, "response_allocation_failed");
    return;
  }
  if (modemErrorLine(line)) {
    fail(ModemResult::Error, "command_error");
    return;
  }
  // 输入尚未提交时，任何旧的 OK / CMGS 都不能构成发信成功。
  if (inputPending_) return;
  if (line == "OK") sawOK_ = true;
  if (requiredPrefix_.length() > 0 && line.startsWith(requiredPrefix_)) {
    if (requiredPrefix_ == "+CMGS:" && !modemCmgsReceipt(line)) {
      fail(ModemResult::Invalid, "invalid_cmgs_receipt");
      return;
    }
    resultLine_ = line;
    if (resultLine_.length() != line.length()) {
      fail(ModemResult::Overflow, "result_allocation_failed");
      return;
    }
  }
  if (sawOK_ && (requiredPrefix_.length() == 0 || resultLine_.length() > 0)) result_ = ModemResult::Ok;
}

void ModemTransaction::acceptPrompt() {
  if (!expectPrompt_ || gotPrompt_) {
    inputPending_ = true;
    fail(ModemResult::Invalid, "unexpected_prompt");
    return;
  }
  gotPrompt_ = true;
}

bool ModemTransaction::waitForPrompt(unsigned long timeoutMs) {
  if (!owns_ || !started_ || !expectPrompt_) return false;
  unsigned long start = millis();
  while (result_ == ModemResult::Pending && !gotPrompt_) {
    pollModem();
    if (gotPrompt_ || result_ != ModemResult::Pending) break;
    if (millis() - start >= timeoutMs) {
      fail(ModemResult::Timeout, "prompt_timeout");
      break;
    }
    modemIdle();
  }
  return gotPrompt_ && result_ == ModemResult::Pending;
}

bool ModemTransaction::submitPdu(const char* hexPdu) {
  if (!owns_ || !started_ || result_ != ModemResult::Pending || !gotPrompt_ || !inputPending_) return false;
  size_t length = hexPdu ? strnlen(hexPdu, SERIAL_BUFFER_SIZE) : 0;
  if (length == 0 || length >= SERIAL_BUFFER_SIZE || length % 2 != 0) {
    fail(ModemResult::Invalid, "invalid_submit_pdu");
    return false;
  }
  for (size_t i = 0; i < length; i++) {
    char c = hexPdu[i];
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
      fail(ModemResult::Invalid, "invalid_submit_pdu");
      return false;
    }
  }
  // 仍在输入阶段有界派发 RX（含回调续到字节），同步失败或错误均禁止提交。
  if (!synchronizeRx()) return false;
  // 提交响应使用新 generation；未完成行保留旧值，URC 仍正常分流。
  generation_ = ++modemGeneration;
  sawOK_ = false;
  resultLine_ = "";
  // 不在写入之间派发回调；部分写入时仍保持 inputPending，析构负责 ESC。
  if (Serial1.write((const uint8_t*)hexPdu, length) != length || Serial1.write((uint8_t)0x1A) != 1) {
    fail(ModemResult::Error, "submit_write_failed");
    return false;
  }
  inputPending_ = false;
  return true;
}

ModemResult ModemTransaction::waitForFinal(unsigned long timeoutMs) {
  if (!owns_ || !started_) return result_;
  if (inputPending_) {
    fail(ModemResult::Invalid, "input_not_submitted");
    return result_;
  }
  unsigned long start = millis();
  while (result_ == ModemResult::Pending) {
    pollModem();
    if (result_ != ModemResult::Pending) break;
    if (millis() - start >= timeoutMs) {
      fail(ModemResult::Timeout, "command_timeout");
      break;
    }
    modemIdle();
  }
  return result_;
}

void ModemTransaction::end() {
  if (!owns_) return;
  bool recover = started_ && result_ != ModemResult::Ok;
  started_ = false;  // 收尾响应只记日志，不属于后续事务。
  if (inputPending_) {
    Serial1.write((uint8_t)0x1B);
    inputPending_ = false;
    recover = true;
  }
  // 短暂收尾仍使用同一读取器，保留 partial URC；不声称能识别无限迟到且无 ID 的 OK。
  if (recover) modemDelay(MODEM_RECOVERY_MS);
  modemOwner = nullptr;
  owns_ = false;
}

ModemResult runModemCommand(const char* command, unsigned long timeoutMs,
                            String* response, String* resultLine, const char* requiredPrefix) {
  if (response) *response = "";
  if (resultLine) *resultLine = "";
  ModemTransaction transaction;
  if (!transaction.acquired()) return transaction.result();
  if (requiredPrefix == nullptr && command != nullptr && strncmp(command, "AT+MPING=", 9) == 0) {
    requiredPrefix = "+MPING:";
  }
  if (transaction.begin(command, requiredPrefix)) transaction.waitForFinal(timeoutMs);
  if (response) *response = transaction.response();
  if (resultLine) *resultLine = transaction.resultLine();
  return transaction.result();
}
