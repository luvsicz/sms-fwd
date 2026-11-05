/*
 * sms_handler.ino - 短信处理函数实现
 */

#include "sms_handler.h"
#include "modem_handler.h"
#include <limits.h>

static void expireSmsPduWait();
static void processPreparedSmsContent(const char* sender, const String& cleanedText, const String& formattedTime);

// 不保留已转发正文；固定数组只存数字地址、逐段 SCTS/正文的 64 位签名。
// 活跃槽仍保留疑似重放正文，直到收齐或超时，避免复用参考号且前缀相同的新短信丢段。
static_assert(MAX_CONCAT_PARTS <= 16, "concat masks must hold every segment");
struct SmsRecentConcat {
  bool inUse;
  char sender[22];  // 最多 20 位数字及 '+'、NUL
  int refNumber;
  int totalParts;
  unsigned long completedAt;
  uint32_t id;
  uint16_t forwardedMask;
  uint64_t signatures[MAX_CONCAT_PARTS];
  uint64_t timestamps[MAX_CONCAT_PARTS];
};
struct SmsConcatReceiveState {
  uint64_t signatures[MAX_CONCAT_PARTS];
  uint64_t timestamps[MAX_CONCAT_PARTS];
  uint16_t forwardedMask;
  bool replayOnly;
  int recentSlot;
  uint32_t recentId;
};
static SmsRecentConcat smsRecentConcat[SMS_CONCAT_RECENT_CAPACITY];
static SmsConcatReceiveState smsConcatState[MAX_CONCAT_MESSAGES];
static uint32_t smsRecentConcatId = 0;
static_assert(sizeof(smsRecentConcat) + sizeof(smsConcatState) <= 2048, "concat dedup metadata exceeds 2 KiB");

static uint64_t smsConcatFingerprint(const String& value, uint64_t hash = 0xCBF29CE484222325ULL) {
  for (unsigned int i = 0; i < value.length(); i++) {
    hash = (hash ^ (uint8_t)value.charAt(i)) * 0x100000001B3ULL;
  }
  return hash;
}

static void expireRecentConcatSms() {
  unsigned long now = millis();
  for (int i = 0; i < SMS_CONCAT_RECENT_CAPACITY; i++) {
    if (smsRecentConcat[i].inUse && now - smsRecentConcat[i].completedAt >= SMS_CONCAT_RECENT_MS) {
      smsRecentConcat[i].inUse = false;
    }
  }
}

static uint16_t receivedConcatMask(int slot) {
  uint16_t mask = 0;
  for (int i = 0; i < concatBuffer[slot].totalParts; i++) {
    if (concatBuffer[slot].parts[i].valid) mask |= (uint16_t)(1U << i);
  }
  return mask;
}

// 只在收到分段时重新分类；缓存过期不能把之前已确认的重放变成超时通知。
static void classifyConcatReplay(int slot) {
  expireRecentConcatSms();
  auto& state = smsConcatState[slot];
  const auto& message = concatBuffer[slot];
  uint16_t received = receivedConcatMask(slot);
  int best = -1, bestOverlap = -1;
  for (int i = 0; i < SMS_CONCAT_RECENT_CAPACITY; i++) {
    const auto& recent = smsRecentConcat[i];
    if (!recent.inUse || recent.refNumber != message.refNumber || recent.totalParts != message.totalParts ||
        !message.sender.equals(recent.sender)) continue;
    bool matches = true;
    int overlap = 0;
    for (int part = 0; part < message.totalParts && matches; part++) {
      uint16_t bit = (uint16_t)(1U << part);
      if (!(received & bit)) continue;
      if (recent.forwardedMask & bit) {
        matches = state.signatures[part] == recent.signatures[part];
        overlap++;
      } else {
        // 超时后未见过的段只能按同一 key 且与已发段相同的 SCTS 关联。
        // 不同 SCTS 或已发段正文冲突均视为新短信，不按参考号一概屏蔽。
        matches = false;
        for (int known = 0; known < message.totalParts; known++) {
          if ((recent.forwardedMask & (1U << known)) && state.timestamps[part] == recent.timestamps[known]) {
            matches = true;
            break;
          }
        }
      }
    }
    if (matches && overlap > bestOverlap) { best = i; bestOverlap = overlap; }
  }
  state.recentSlot = best;
  state.recentId = best < 0 ? 0 : smsRecentConcat[best].id;
  state.forwardedMask = best < 0 ? 0 : smsRecentConcat[best].forwardedMask;
  state.replayOnly = received != 0 && (received & ~state.forwardedMask) == 0;
  if (best >= 0) {
    // 保存已发但本轮未收到的签名，使迟到段等待期间的缓存过期/淘汰不重发旧正文。
    for (int part = 0; part < message.totalParts; part++) {
      if ((state.forwardedMask & (1U << part)) && !(received & (1U << part))) {
        state.signatures[part] = smsRecentConcat[best].signatures[part];
        state.timestamps[part] = smsRecentConcat[best].timestamps[part];
      }
    }
  }
}

static void rememberForwardedConcat(int slot) {
  expireRecentConcatSms();
  const auto& state = smsConcatState[slot];
  const auto& message = concatBuffer[slot];
  if (message.sender.length() >= sizeof(smsRecentConcat[0].sender)) return;
  int target = state.recentSlot;
  bool updating = target >= 0 && target < SMS_CONCAT_RECENT_CAPACITY &&
                  smsRecentConcat[target].inUse && smsRecentConcat[target].id == state.recentId;
  if (!updating) {
    target = 0;
    unsigned long now = millis();
    for (int i = 0; i < SMS_CONCAT_RECENT_CAPACITY; i++) {
      if (!smsRecentConcat[i].inUse) { target = i; break; }
      if (now - smsRecentConcat[i].completedAt > now - smsRecentConcat[target].completedAt) target = i;
    }
    smsRecentConcat[target] = SmsRecentConcat{};
    smsRecentConcat[target].completedAt = now;
    smsRecentConcat[target].id = ++smsRecentConcatId;
  }
  auto& recent = smsRecentConcat[target];
  recent.inUse = true;
  memcpy(recent.sender, message.sender.c_str(), message.sender.length() + 1);
  recent.refNumber = message.refNumber;
  recent.totalParts = message.totalParts;
  recent.forwardedMask = state.forwardedMask | receivedConcatMask(slot);
  for (int part = 0; part < message.totalParts; part++) {
    if (recent.forwardedMask & (1U << part)) {
      recent.signatures[part] = state.signatures[part];
      recent.timestamps[part] = state.timestamps[part];
    }
  }
  // 更新迟到段不延长原记录的期限；纯重复段从不调用此函数。
}

static void finishConcatSms(int slot) {
  if (slot < 0 || slot >= MAX_CONCAT_MESSAGES || !concatBuffer[slot].inUse) return;
  if (concatBuffer[slot].receivedParts == 0) {
    clearConcatSlot(slot);  // 缓存首段失败不能生成只有占位符的短信。
    return;
  }
  if (smsConcatState[slot].replayOnly) {
    Serial.println("已转发长短信的重复分段，跳过通知");
    clearConcatSlot(slot);
    return;
  }
  String fullText = assembleConcatSms(slot);
  if (fullText.length() == 0) {
    // 合并失败不登记已转发；保留正文供重投触发或下一个超时周期重试，避免忙循环。
    concatBuffer[slot].firstPartTime = millis();
    return;
  }
  String sender = concatBuffer[slot].sender;
  String timestamp = concatBuffer[slot].timestamp;
  if (sender.length() == 0 || sender.length() != concatBuffer[slot].sender.length() ||
      timestamp.length() == 0 || timestamp.length() != concatBuffer[slot].timestamp.length()) {
    Serial.printf("[sms] ERROR: concat_snapshot_allocation_failed ref=%d; retry pending\n", concatBuffer[slot].refNumber);
    concatBuffer[slot].firstPartTime = millis();
    return;
  }
  rememberForwardedConcat(slot);
  clearConcatSlot(slot);  // 先记录并释放，再进入历史/转发回调，避免重入重复发送。
  processPreparedSmsContent(sender.c_str(), fullText, timestamp);
}

// PDU 解码结果已由 TP-UDL 界定正文；AT 行由串口层分离，不能猜测并删除正文尾行。
String cleanSmsContent(const String& text) {
  return text;
}

static int smsDaysInMonth(int year, int month) {
  static const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0)) {
    return 29;
  }
  return days[month - 1];
}

static bool readSmsDecimal(const String& value, int start, int count, int& result) {
  if (start < 0 || count < 1 || start + count > (int)value.length()) return false;
  result = 0;
  for (int i = start; i < start + count; i++) {
    char c = value.charAt(i);
    if (c < '0' || c > '9') return false;
    result = result * 10 + (c - '0');
  }
  return true;
}

// pdulib 0.5.x 输出 14 字符：每个 SCTS 字节的低/高半字节分别加 '0'。
// 时区低半字节的 bit 3 是负号，因此负时区可能含 ':' 等字符，不能用 toInt()。
// 归一为 UTC+8 的 YYYY-MM-DD HH:MM:SS；19 字符输入视为已归一，非法输入返回空串。
String formatTimestamp(const String& pduTimestamp) {
  if (pduTimestamp.length() == 19) {
    return isTimestampLikelyValidSms(pduTimestamp) ? pduTimestamp : String("");
  }
  if (pduTimestamp.length() != 14) return "";

  int year, month, day, hour, minute, second;
  if (!readSmsDecimal(pduTimestamp, 0, 2, year) ||
      !readSmsDecimal(pduTimestamp, 2, 2, month) ||
      !readSmsDecimal(pduTimestamp, 4, 2, day) ||
      !readSmsDecimal(pduTimestamp, 6, 2, hour) ||
      !readSmsDecimal(pduTimestamp, 8, 2, minute) ||
      !readSmsDecimal(pduTimestamp, 10, 2, second)) return "";
  year += 2000;
  if (day < 1 || day > smsDaysInMonth(year, month) ||
      hour > 23 || minute > 59 || second > 59) return "";

  int tzTens = pduTimestamp.charAt(12) - '0';
  int tzUnits = pduTimestamp.charAt(13) - '0';
  if (tzTens < 0 || tzTens > 15 || tzUnits < 0 || tzUnits > 9) return "";
  int tzQuarters = (tzTens & 7) * 10 + tzUnits;
  if (tzTens & 8) tzQuarters = -tzQuarters;
  int totalMinutes = hour * 60 + minute + (32 - tzQuarters) * 15;

  while (totalMinutes < 0) {
    totalMinutes += 24 * 60;
    if (--day < 1) {
      if (--month < 1) { month = 12; year--; }
      day = smsDaysInMonth(year, month);
    }
  }
  while (totalMinutes >= 24 * 60) {
    totalMinutes -= 24 * 60;
    if (++day > smsDaysInMonth(year, month)) {
      day = 1;
      if (++month > 12) { month = 1; year++; }
    }
  }

  char buf[20];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           year, month, day, totalMinutes / 60, totalMinutes % 60, second);
  String result(buf);
  return isTimestampLikelyValidSms(result) ? result : String("");
}

// 提取验证码（用于邮件主题）
String extractVerifyCode(const String& text) {
  // 查找4-8位连续数字
  String code = "";
  for (unsigned int i = 0; i < text.length(); i++) {
    char c = text.charAt(i);
    if (c >= '0' && c <= '9') {
      code += c;
    } else if (code.length() >= 4 && code.length() <= 8) {
      break;  // 找到了有效验证码
    } else {
      code = "";  // 重新开始
    }
  }
  // 最后检查
  if (code.length() >= 4 && code.length() <= 8) {
    return code;
  }
  return "";
}

// 初始化长短信缓存
void initConcatBuffer() {
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    clearConcatSlot(i);
  }
  for (int i = 0; i < SMS_CONCAT_RECENT_CAPACITY; i++) smsRecentConcat[i] = SmsRecentConcat{};
  smsRecentConcatId = 0;
}

// 查找或创建长短信缓存槽位
int findOrCreateConcatSlot(int refNumber, const char* sender, int totalParts) {
  if (!sender || refNumber < 0 || refNumber > 0xFFFF || totalParts < 2 || totalParts > MAX_CONCAT_PARTS) return -1;
  // 先查找是否已存在
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].inUse && 
        concatBuffer[i].refNumber == refNumber &&
        concatBuffer[i].sender.equals(sender)) {
      // 同一参考号/发送者的总段数不能中途改变，避免提前合并或丢段。
      return concatBuffer[i].totalParts == totalParts ? i : -1;
    }
  }
  
  // 查找空闲槽位
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (!concatBuffer[i].inUse) {
      clearConcatSlot(i);
      concatBuffer[i].sender = sender;
      if (concatBuffer[i].sender.length() != strlen(sender)) {
        Serial.printf("[sms] ERROR: concat_sender_allocation_failed ref=%d; awaiting retransmission\n", refNumber);
        return -1;
      }
      concatBuffer[i].inUse = true;
      concatBuffer[i].refNumber = refNumber;
      concatBuffer[i].totalParts = totalParts;
      concatBuffer[i].receivedParts = 0;
      concatBuffer[i].firstPartTime = millis();
      for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
        concatBuffer[i].parts[j].valid = false;
        concatBuffer[i].parts[j].text = "";
      }
      return i;
    }
  }
  
  // 没有空闲槽位，查找最老的槽位覆盖
  int oldestSlot = 0;
  unsigned long now = millis();
  unsigned long oldestAge = now - concatBuffer[0].firstPartTime;
  for (int i = 1; i < MAX_CONCAT_MESSAGES; i++) {
    if (now - concatBuffer[i].firstPartTime > oldestAge) {
      oldestAge = now - concatBuffer[i].firstPartTime;
      oldestSlot = i;
    }
  }
  
  // 覆盖最老的槽位
  Serial.println("长短信缓存已满，覆盖最老的槽位");
  clearConcatSlot(oldestSlot);
  concatBuffer[oldestSlot].sender = sender;
  if (concatBuffer[oldestSlot].sender.length() != strlen(sender)) {
    Serial.printf("[sms] ERROR: concat_sender_allocation_failed ref=%d; awaiting retransmission\n", refNumber);
    return -1;
  }
  concatBuffer[oldestSlot].inUse = true;
  concatBuffer[oldestSlot].refNumber = refNumber;
  concatBuffer[oldestSlot].totalParts = totalParts;
  concatBuffer[oldestSlot].receivedParts = 0;
  concatBuffer[oldestSlot].firstPartTime = millis();
  for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
    concatBuffer[oldestSlot].parts[j].valid = false;
    concatBuffer[oldestSlot].parts[j].text = "";
  }
  return oldestSlot;
}

// 两遍遍历：先精确计算 UTF-8 字节数，再检查每次追加；占位符不构造临时 String。
static bool appendPendingConcatContent(int slot, String& result) {
  const auto& message = concatBuffer[slot];
  size_t totalLen = 0;
  for (int pass = 0; pass < 2; pass++) {
    if (pass == 1 && !result.reserve(totalLen)) return false;
    for (int i = 0; i < message.totalParts; i++) {
      const auto& part = message.parts[i];
      bool forwarded = smsConcatState[slot].forwardedMask & (1U << i);
      const char* content = part.text.c_str();
      size_t length = part.text.length();
      char placeholder[32];
      if (forwarded || !part.valid) {
        int written = snprintf(placeholder, sizeof(placeholder),
                               forwarded ? "[已转发分段%d]" : "[缺失分段%d]", i + 1);
        if (written < 0 || (size_t)written >= sizeof(placeholder)) return false;
        content = placeholder;
        length = (size_t)written;
      }
      if (length == 0) return false;  // valid 段的正文不得因复制失败变为空串。
      if (pass == 0) {
        if (length > UINT_MAX - totalLen) return false;
        totalLen += length;
      } else if (!result.concat(content, length)) {
        return false;
      }
    }
  }
  return result.length() == totalLen;
}

// 合并失败返回空串，绝不把部分结果交给转发/去重提交路径。
String assembleConcatSms(int slot) {
  if (slot < 0 || slot >= MAX_CONCAT_MESSAGES || !concatBuffer[slot].inUse ||
      concatBuffer[slot].totalParts < 2 || concatBuffer[slot].totalParts > MAX_CONCAT_PARTS ||
      concatBuffer[slot].receivedParts == 0) return "";
  String result;
  if (!appendPendingConcatContent(slot, result)) {
    Serial.printf("[sms] ERROR: concat_assembly_allocation_failed ref=%d; retry pending\n", concatBuffer[slot].refNumber);
    return "";
  }
  return result;
}

// 清空长短信槽位（增加边界安全检查）
void clearConcatSlot(int slot) {
  if (slot < 0 || slot >= MAX_CONCAT_MESSAGES) return;
  smsConcatState[slot] = SmsConcatReceiveState{};
  smsConcatState[slot].recentSlot = -1;
  concatBuffer[slot].inUse = false;
  concatBuffer[slot].receivedParts = 0;
  concatBuffer[slot].sender = "";
  concatBuffer[slot].timestamp = "";
  for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
    concatBuffer[slot].parts[j].valid = false;
    concatBuffer[slot].parts[j].text = "";
  }
}

// 检查长短信超时并转发
void checkConcatTimeout() {
  expireSmsPduWait();
  expireRecentConcatSms();
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].inUse) {
      // 每槽重新取时钟：前一个转发回调可能耗时/重入；无符号差值仍保证回绕安全。
      unsigned long now = millis();
      if (now - concatBuffer[i].firstPartTime >= CONCAT_TIMEOUT_MS) {
        Serial.println("长短信等待超时，检查待转发分段");
        Serial.printf("  参考号: %d, 已收到: %d/%d\n", 
                      concatBuffer[i].refNumber,
                      concatBuffer[i].receivedParts,
                      concatBuffer[i].totalParts);
        
        finishConcatSms(i);
      }
    }
  }
}

// 发送短信（PDU 模式）
bool sendSMS(const char* phoneNumber, const char* message) {
  Serial.println("准备发送短信...");
  Serial.print("目标号码: "); Serial.println(phoneNumber);
  Serial.print("短信内容: "); Serial.println(message);

  ModemTransaction transaction;
  if (!transaction.acquired()) {
    Serial.println("modem忙，无法发送短信");
    return false;
  }

  // 使用 pdulib 编码 PDU
  pdu.setSCAnumber();  // 使用默认短信中心
  int pduLen = pdu.encodePDU(phoneNumber, message);
  
  if (pduLen < 0) {
    Serial.print("PDU编码失败，错误码: ");
    Serial.println(pduLen);
    return false;
  }

  // begin/wait 会分流 URC 并覆写全局 pdu，必须在首次 poll 前保存独立快照。
  String smsPdu(pdu.getSMS());
  size_t hexLength = strlen(pdu.getSMS());
  if (hexLength == 0 || smsPdu.length() != hexLength) {
    Serial.println("PDU快照失败");
    return false;
  }
  // pdulib 自带尾部 Ctrl+Z；只移除该终止符，由 submitPdu 统一发送一次。
  if (smsPdu.charAt(hexLength - 1) == '\x1A') {
    hexLength--;
    smsPdu = smsPdu.substring(0, hexLength);
  }
  if (hexLength == 0 || hexLength >= SERIAL_BUFFER_SIZE || hexLength % 2 != 0 ||
      smsPdu.length() != hexLength || !isHexString(smsPdu)) {
    Serial.println("PDU快照无效");
    return false;
  }

  Serial.print("PDU数据: "); Serial.println(smsPdu);
  Serial.print("PDU长度: "); Serial.println(pduLen);
  
  // 发送 AT+CMGS 命令
  String cmgsCmd = "AT+CMGS=";
  cmgsCmd += pduLen;

  if (!transaction.begin(cmgsCmd.c_str(), "+CMGS:", true)) {
    Serial.println("无法开始短信发送事务");
    return false;
  }

  // 等待期间由 modem 持续分流 URC；失败时析构负责取消输入和收尾。
  if (!transaction.waitForPrompt(5000)) {
    Serial.println("未收到>提示符");
    return false;
  }
  
  if (!transaction.submitPdu(smsPdu.c_str())) {
    Serial.println("PDU提交失败");
    return false;
  }

  // 成功必须同时收到有效 +CMGS: 回执及完整 OK 行，不能按响应子串猜测。
  ModemResult result = transaction.waitForFinal(30000);
  if (result == ModemResult::Ok) {
    Serial.println("\n短信发送成功");
    return true;
  }
  Serial.println(result == ModemResult::Timeout ? "短信发送超时" : "\n短信发送失败");
  return false;
}

// 处理最终的短信内容（管理员命令检查和转发）
void processSmsContent(const char* sender, const char* text, const char* timestamp) {
  // 保留正文（包括 CR/LF、OTP、URL）；JSON 等转义由各输出边界负责。
  String cleanedText(text);
  String formattedTime = formatTimestamp(String(timestamp));
  processPreparedSmsContent(sender, cleanedText, formattedTime);
}

// 调用方持有完整正文和已归一时间，直到所有同步回调返回；去重提交后不能再复制这些快照。
static void processPreparedSmsContent(const char* sender, const String& cleanedText, const String& formattedTime) {
  String verifyCode = extractVerifyCode(cleanedText);
  
  Serial.println("=== 处理短信 ===");
  Serial.printf("发送者: %s\n", sender);
  Serial.printf("时间: %s\n", formattedTime.c_str());
  
  // 添加到短信历史
  addSmsToHistory(sender, cleanedText.c_str(), formattedTime.c_str());
  
  // 检查号码过滤（黑白名单）
  if (isNumberFiltered(sender)) {
    Serial.println("号码被过滤，跳过推送");
    return;
  }
  
  // 检查内容关键词过滤
  if (isContentFiltered(cleanedText.c_str())) {
    Serial.println("内容被关键词过滤，跳过推送");
    return;
  }
  
  if (verifyCode.length() > 0) {
    Serial.printf("验证码: %s\n", verifyCode.c_str());
  }

  // 发送通知 http
  sendSMSToServer(sender, cleanedText.c_str(), formattedTime.c_str());
  
  // 发送 MQTT 通知
  publishMqttSmsReceived(sender, cleanedText.c_str(), formattedTime.c_str());
  
  // 发送通知邮件
  String subject;
  if (verifyCode.length() > 0) {
    subject = "[" + verifyCode + "] " + String(sender);
  } else {
    String preview = cleanedText.substring(0, 30);
    if (cleanedText.length() > 30) preview += "...";
    subject = String(sender) + ": " + preview;
  }
  
  String body = "发送者: " + String(sender) + "\n";
  body += "时间: " + formattedTime + "\n";
  body += "内容:\n" + cleanedText;
  
  sendEmailNotification(subject.c_str(), body.c_str());
}


// 读取串口一行（使用调用方提供的独立缓冲区和位置指针，彻底消除 static 缓冲区并发竞争）(H4)
String readSerialLine(HardwareSerial& port, char* lineBuf, size_t bufSize, int& linePos) {
  if (&port == &Serial1) return "";  // Serial1 只能由 pollModem 读取
  if (lineBuf == nullptr || bufSize == 0) return "";

  while (port.available()) {
    char c = port.read();
    if (c == '\n') {
      lineBuf[linePos] = 0;
      String res = String(lineBuf);
      linePos = 0;
      return res;
    } else if (c != '\r') {  // 跳过 \r
      if (linePos < (int)bufSize - 1)
        lineBuf[linePos++] = c;
      else
        linePos = 0;  // 超长报错保护，重头计
    }
  }
  return "";
}

// 兼容单参版本：使用 FreeRTOS 互斥锁保护内部静态缓冲区，并在锁外构造 String，避免关中断与锁内 malloc (H4)
String readSerialLine(HardwareSerial& port) {
  if (&port == &Serial1) return "";  // 不创建另一份 Serial1 行缓冲或消费字节
  static char fallbackBuf[SERIAL_BUFFER_SIZE];
  static int fallbackPos = 0;
  static SemaphoreHandle_t lineMutex = nullptr;

  if (lineMutex == nullptr) {
    lineMutex = xSemaphoreCreateMutex();
    if (lineMutex == nullptr) {
      return "";
    }
  }

  char localBuf[SERIAL_BUFFER_SIZE];
  bool hasLine = false;

  if (xSemaphoreTake(lineMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    while (port.available()) {
      char c = port.read();
      if (c == '\n') {
        fallbackBuf[fallbackPos] = '\0';
        size_t copyLen = (fallbackPos < (int)sizeof(localBuf) - 1) ? fallbackPos : (sizeof(localBuf) - 1);
        memcpy(localBuf, fallbackBuf, copyLen);
        localBuf[copyLen] = '\0';
        fallbackPos = 0;
        hasLine = true;
        break;
      } else if (c != '\r') {
        if (fallbackPos < (int)sizeof(fallbackBuf) - 1) {
          fallbackBuf[fallbackPos++] = c;
        } else {
          fallbackPos = 0;  // 超长保护，重头计
        }
      }
    }
    xSemaphoreGive(lineMutex);
  }

  if (hasLine) {
    return String(localBuf);
  }
  return "";
}

// 检查字符串是否为有效的十六进制 PDU 数据
bool isHexString(const String& str) {
  if (str.length() == 0) return false;
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

// ===== PDU fallback 解析辅助 =====
bool isLikelyValidUtf8(const String& text) {
  int expectedContinuation = 0;

  for (unsigned int i = 0; i < text.length(); i++) {
    uint8_t c = (uint8_t)text.charAt(i);

    if (expectedContinuation == 0) {
      if (c <= 0x7F) {
        continue;
      }
      if (c >= 0xC2 && c <= 0xDF) {
        expectedContinuation = 1;
      } else if (c >= 0xE0 && c <= 0xEF) {
        expectedContinuation = 2;
      } else if (c >= 0xF0 && c <= 0xF4) {
        expectedContinuation = 3;
      } else {
        return false;
      }
    } else {
      if ((c & 0xC0) != 0x80) {
        return false;
      }
      expectedContinuation--;
    }
  }

  return expectedContinuation == 0;
}

bool containsReplacementCharUtf8(const String& text) {
  for (unsigned int i = 0; i + 2 < text.length(); i++) {
    if ((uint8_t)text.charAt(i) == 0xEF &&
        (uint8_t)text.charAt(i + 1) == 0xBF &&
        (uint8_t)text.charAt(i + 2) == 0xBD) {
      return true;
    }
  }
  return false;
}

bool isTimestampLikelyValidSms(const String& timestamp) {
  if (timestamp.length() != 19) return false;
  if (timestamp.charAt(4) != '-' || timestamp.charAt(7) != '-' ||
      timestamp.charAt(10) != ' ' || timestamp.charAt(13) != ':' ||
      timestamp.charAt(16) != ':') {
    return false;
  }

  int year, month, day, hour, minute, second;
  if (!readSmsDecimal(timestamp, 0, 4, year) ||
      !readSmsDecimal(timestamp, 5, 2, month) ||
      !readSmsDecimal(timestamp, 8, 2, day) ||
      !readSmsDecimal(timestamp, 11, 2, hour) ||
      !readSmsDecimal(timestamp, 14, 2, minute) ||
      !readSmsDecimal(timestamp, 17, 2, second)) return false;

  if (year < 2020 || year > 2099) return false;
  if (month < 1 || month > 12) return false;
  if (day < 1 || day > smsDaysInMonth(year, month)) return false;
  if (hour < 0 || hour > 23) return false;
  if (minute < 0 || minute > 59) return false;
  if (second < 0 || second > 59) return false;

  return true;
}

// 仅无分段标记的 [0,0,0] 归一为单段；真实 UDH 中的零段数/零序号必须先拒绝。
bool normalizeSmsConcatInfo(int refNumber, int& partNumber, int& totalParts) {
  if (refNumber == 0 && partNumber == 0 && totalParts == 0) {
    partNumber = 1;
    totalParts = 1;
  }
  return refNumber >= 0 && refNumber <= 0xFFFF &&
         totalParts >= 1 && totalParts <= MAX_CONCAT_PARTS &&
         partNumber >= 1 && partNumber <= totalParts;
}

bool isLikelyValidSmsSender(const String& sender) {
  if (sender.length() == 0) return false;

  int digitCount = 0;
  for (unsigned int i = 0; i < sender.length(); i++) {
    char c = sender.charAt(i);
    if (c >= '0' && c <= '9') {
      digitCount++;
      continue;
    }
    if (c == '+' && i == 0) continue;
    if (c == ' ' || c == '-' || c == '(' || c == ')') continue;
    return false;
  }

  return digitCount >= 3;
}

bool hasVisibleSmsText(const String& text) {
  if (text.length() == 0) return false;

  for (unsigned int i = 0; i < text.length(); i++) {
    uint8_t c = (uint8_t)text.charAt(i);
    if (c == '\n' || c == '\r' || c == '\t') continue;
    if (c >= 0x20) return true;
  }

  return false;
}

bool isPduLikelyStorableSms(const String& sender,
                            const String& timestamp,
                            const String& text,
                            bool isSmsDeliver,
                            bool hasUserData,
                            bool fallbackDecoded,
                            const DecodedPDU* fallbackSms) {
  if (!isSmsDeliver) return false;
  if (!hasUserData) return false;
  if (!hasVisibleSmsText(text)) return false;

  bool senderOk = isLikelyValidSmsSender(sender);
  bool timeOk = isTimestampLikelyValidSms(timestamp);
  bool textUtf8Ok = isLikelyValidUtf8(text) && !containsReplacementCharUtf8(text);

  // GSM7 的重音、北欧和希腊字符都是合法正文；不能再按字符频次猜测乱码。
  if (senderOk && timeOk && textUtf8Ok) {
    return true;
  }

  if (fallbackDecoded && fallbackSms != nullptr) {
    bool fbSenderOk = isLikelyValidSmsSender(fallbackSms->sender);
    bool fbTimeOk = isTimestampLikelyValidSms(fallbackSms->timestamp);
    bool fbTextOk = hasVisibleSmsText(fallbackSms->text) &&
                    isLikelyValidUtf8(fallbackSms->text) &&
                    !containsReplacementCharUtf8(fallbackSms->text);

    if (fallbackSms->isSmsDeliver && fallbackSms->hasUserData && fbSenderOk && fbTimeOk && fbTextOk) {
      return true;
    }
  }

  return false;
}

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return 0;
}

uint8_t hexByteAt(const String& s, int pos) {
  return (hexNibble(s.charAt(pos)) << 4) | hexNibble(s.charAt(pos + 1));
}

String swapSemiOctetsToDigits(const String& hex, int digitCount) {
  String out = "";
  for (int i = 0; i < hex.length(); i += 2) {
    char hi = hex.charAt(i);
    char lo = hex.charAt(i + 1);

    if (lo != 'F' && lo != 'f') out += lo;
    if ((int)out.length() >= digitCount) break;

    if (hi != 'F' && hi != 'f') out += hi;
    if ((int)out.length() >= digitCount) break;
  }
  return out;
}

void appendUtf8(String& out, uint16_t cp) {
  if (cp < 0x80) {
    out += (char)cp;
  } else if (cp < 0x800) {
    out += (char)(0xC0 | (cp >> 6));
    out += (char)(0x80 | (cp & 0x3F));
  } else {
    out += (char)(0xE0 | (cp >> 12));
    out += (char)(0x80 | ((cp >> 6) & 0x3F));
    out += (char)(0x80 | (cp & 0x3F));
  }
}

String decodeUcs2Hex(const String& hex) {
  String out = "";
  for (int i = 0; i + 3 < hex.length(); i += 4) {
    uint16_t cp = ((uint16_t)hexByteAt(hex, i) << 8) | hexByteAt(hex, i + 2);
    if (cp >= 0xD800 && cp <= 0xDFFF) {
      out += "?";
      continue;
    }
    appendUtf8(out, cp);
  }
  return out;
}

String gsm7ToUtf8(uint8_t septet) {
  switch (septet) {
    case 0x00: return "@";
    case 0x01: return "£";
    case 0x02: return "$";
    case 0x03: return "¥";
    case 0x04: return "è";
    case 0x05: return "é";
    case 0x06: return "ù";
    case 0x07: return "ì";
    case 0x08: return "ò";
    case 0x09: return "Ç";
    case 0x0A: return "\n";
    case 0x0B: return "Ø";
    case 0x0C: return "ø";
    case 0x0D: return "\r";
    case 0x0E: return "Å";
    case 0x0F: return "å";
    case 0x10: return "Δ";
    case 0x11: return "_";
    case 0x12: return "Φ";
    case 0x13: return "Γ";
    case 0x14: return "Λ";
    case 0x15: return "Ω";
    case 0x16: return "Π";
    case 0x17: return "Ψ";
    case 0x18: return "Σ";
    case 0x19: return "Θ";
    case 0x1A: return "Ξ";
    case 0x1B: return "";
    case 0x1C: return "Æ";
    case 0x1D: return "æ";
    case 0x1E: return "ß";
    case 0x1F: return "É";
    case 0x20: return " ";
    case 0x21: return "!";
    case 0x22: return "\"";
    case 0x23: return "#";
    case 0x24: return "¤";
    case 0x25: return "%";
    case 0x26: return "&";
    case 0x27: return "'";
    case 0x28: return "(";
    case 0x29: return ")";
    case 0x2A: return "*";
    case 0x2B: return "+";
    case 0x2C: return ",";
    case 0x2D: return "-";
    case 0x2E: return ".";
    case 0x2F: return "/";
    case 0x3A: return ":";
    case 0x3B: return ";";
    case 0x3C: return "<";
    case 0x3D: return "=";
    case 0x3E: return ">";
    case 0x3F: return "?";
    case 0x40: return "¡";
    case 0x5B: return "Ä";
    case 0x5C: return "Ö";
    case 0x5D: return "Ñ";
    case 0x5E: return "Ü";
    case 0x5F: return "§";
    case 0x60: return "¿";
    case 0x7B: return "ä";
    case 0x7C: return "ö";
    case 0x7D: return "ñ";
    case 0x7E: return "ü";
    case 0x7F: return "à";
    default:
      return String((char)septet);
  }
}

String gsm7ExtToUtf8(uint8_t septet) {
  switch (septet) {
    case 0x0A: return "\f";
    case 0x14: return "^";
    case 0x28: return "{";
    case 0x29: return "}";
    case 0x2F: return "\\";
    case 0x3C: return "[";
    case 0x3D: return "~";
    case 0x3E: return "]";
    case 0x40: return "|";
    case 0x65: return "€";
    default: return "?";
  }
}

String decodeGsm7Bit(const String& hex, int septetCount, int skipSeptets) {
  String out = "";
  int totalBytes = hex.length() / 2;
  if (hex.length() % 2 != 0 || septetCount < 0 || skipSeptets < 0 ||
      skipSeptets > totalBytes * 8 / 7 ||
      septetCount > totalBytes * 8 / 7 - skipSeptets) return "";
  int bitOffset = skipSeptets * 7;
  bool escapeNext = false;

  for (int i = 0; i < septetCount; i++) {
    int bitIndex = bitOffset + i * 7;
    int byteIndex = bitIndex / 8;
    int shift = bitIndex % 8;

    if (byteIndex >= totalBytes) break;

    uint16_t chunk = hexByteAt(hex, byteIndex * 2);
    if (byteIndex + 1 < totalBytes) {
      chunk |= ((uint16_t)hexByteAt(hex, (byteIndex + 1) * 2) << 8);
    }

    uint8_t septet = (chunk >> shift) & 0x7F;
    if (escapeNext) {
      out += gsm7ExtToUtf8(septet);
      escapeNext = false;
      continue;
    }

    if (septet == 0x1B) {
      escapeNext = true;
      continue;
    }
    out += gsm7ToUtf8(septet);
  }

  return out;
}

String decodeScts(const String& sctsHex) {
  if (sctsHex.length() != 14 || !isHexString(sctsHex)) return "";
  String timestamp;
  timestamp.reserve(14);
  for (int i = 0; i < 14; i += 2) {
    uint8_t value = hexByteAt(sctsHex, i);
    timestamp += (char)('0' + (value & 0x0F));
    timestamp += (char)('0' + (value >> 4));
  }
  return formatTimestamp(timestamp);
}

// 按 DCS 编码组确定 TP-UDL 的单位，不能只看 alphabet 位（MWI 组含义不同）。
// 返回 0x00 / GSM7、0x08 / UCS2；二进制、压缩和保留编码不作为文本短信接收。
static int smsTextAlphabet(uint8_t dcs) {
  if ((dcs & 0x80) == 0) {  // 通用编码及自动删除组
    if (dcs & 0x20) return -1;
    int alphabet = dcs & 0x0C;
    return alphabet == 0x00 || alphabet == 0x08 ? alphabet : -1;
  }
  int group = dcs & 0xF0;
  if (group == 0xC0 || group == 0xD0 || group == 0xE0) {
    if (dcs & 0x04) return -1;  // MWI 的 bit 2 保留
    return group == 0xE0 ? 0x08 : 0x00;
  }
  return (dcs & 0xFC) == 0xF0 ? 0x00 : -1;
}

// 地址类型 TON：0 未知、1 国际、2 国内、3 网络特定、4 用户号（运营商 SP 长号常用，
// 如电信 0xC1 的 1069…）、6 缩位号均为 BCD；5 为 GSM7 字母数字；7 保留。
static int smsAddressTon(uint8_t type) { return (type >> 4) & 7; }
static bool isSmsBcdAddressTon(int ton) { return ton <= 4 || ton == 6; }

// 仅接受已支持的数字地址；先检查字段范围，再读取 BCD 和末尾填充半字节。
static bool isSmsNumericAddress(const String& pduHex, int pos, int digits, uint8_t type) {
  int bytes = (digits + 1) / 2;
  if (!(type & 0x80) || !isSmsBcdAddressTon(smsAddressTon(type)) || digits < 1 || digits > 20 ||
      pos < 0 || pos + bytes * 2 > (int)pduHex.length()) return false;
  for (int i = 0; i < bytes; i++) {
    uint8_t value = hexByteAt(pduHex, pos + i * 2);
    if ((value & 0x0F) > 9) return false;
    int high = value >> 4;
    if (i == bytes - 1 && (digits & 1)) {
      if (high != 0x0F) return false;
    } else if (high > 9) {
      return false;
    }
  }
  return true;
}

// 有界解析 SMS-DELIVER：既用于 fallback，也在主入口调用 pdulib 前完成校验。
bool decodeDeliverPDUFallback(const String& pduHex, DecodedPDU& out) {
  out.sender = "";
  out.timestamp = "";
  out.text = "";
  out.refNumber = 0;
  out.partNumber = 1;
  out.totalParts = 1;
  out.isSmsDeliver = false;
  out.hasUserData = false;
  out.dcs = 0;
  out.fo = 0;
  out.tpduBytes = 0;

  if (pduHex.length() < 20 || (pduHex.length() % 2 != 0) || !isHexString(pduHex)) return false;

  int pos = 0;

  // SCA
  int scaLen = hexByteAt(pduHex, pos);
  pos += 2;
  if (scaLen != 0) {
    if (scaLen < 2 || scaLen > 11 || pos + scaLen * 2 > (int)pduHex.length()) return false;
    int scaDigits = (scaLen - 1) * 2;
    if ((hexByteAt(pduHex, pos + scaLen * 2 - 2) >> 4) == 0x0F) scaDigits--;
    if (!isSmsNumericAddress(pduHex, pos + 2, scaDigits, hexByteAt(pduHex, pos))) return false;
  }
  pos += scaLen * 2;
  if (pos + 2 > pduHex.length()) return false;

  // FO
  uint8_t fo = hexByteAt(pduHex, pos);
  out.fo = fo;
  pos += 2;

  if ((fo & 0x03) != 0x00) {
    Serial.printf("fallback parser: 非 SMS-DELIVER PDU, FO=0x%02X\n", fo);
    return false;
  }
  out.isSmsDeliver = true;

  // OA
  if (pos + 4 > pduHex.length()) return false;
  int oaLen = hexByteAt(pduHex, pos);
  pos += 2;
  uint8_t oaType = hexByteAt(pduHex, pos);
  pos += 2;

  int oaByteLen = (oaLen + 1) / 2;
  // 本次仅扩展运营商数字地址 TON，不把字母地址混入数字发送方校验。
  if (!isSmsNumericAddress(pduHex, pos, oaLen, oaType)) return false;
  out.sender = swapSemiOctetsToDigits(pduHex.substring(pos, pos + oaByteLen * 2), oaLen);
  if (smsAddressTon(oaType) == 1) out.sender = "+" + out.sender;
  pos += oaByteLen * 2;

  // PID / DCS
  if (pos + 4 > pduHex.length()) return false;
  uint8_t pid = hexByteAt(pduHex, pos);
  pos += 2;
  uint8_t dcs = hexByteAt(pduHex, pos);
  out.dcs = dcs;
  pos += 2;
  (void)pid;

  // SCTS
  if (pos + 14 > pduHex.length()) return false;
  String sctsHex = pduHex.substring(pos, pos + 14);
  pos += 14;
  out.timestamp = decodeScts(sctsHex);
  if (out.timestamp.length() == 0) return false;

  // UDL
  if (pos + 2 > pduHex.length()) return false;
  int udl = hexByteAt(pduHex, pos);
  pos += 2;

  int alphabet = smsTextAlphabet(dcs);
  if (alphabet < 0) {
    Serial.printf("fallback parser: 暂不支持 DCS=0x%02X\n", dcs);
    return false;
  }

  // GSM7 的 TP-UDL 是 septet（含 UDH 和填充位）；UCS2 的 TP-UDL 才是字节。
  int udBytes = alphabet == 0x00 ? (udl * 7 + 7) / 8 : udl;
  if (udBytes > 140) return false;
  int udHexLen = udBytes * 2;
  if (pos + udHexLen > pduHex.length()) return false;
  String udHex = pduHex.substring(pos, pos + udHexLen);

  bool hasUdh = (fo & 0x40) != 0;
  int textStart = 0;

  int udhTotalBytes = 0;
  int skipSeptets = 0;
  bool hasConcat = false;
  if (hasUdh) {
    if (udBytes < 1) return false;
    int udhl = hexByteAt(udHex, 0);  // 不含自身长度字节
    udhTotalBytes = 1 + udhl;
    if (udhTotalBytes > udBytes) return false;
    textStart = udhTotalBytes * 2;
    if (alphabet == 0x00) {
      skipSeptets = (udhTotalBytes * 8 + 6) / 7;
      if (skipSeptets > udl) return false;
    }

    int p = 2; // 跳过 UDHL
    while (p < textStart) {
      if (p + 4 > textStart) return false;
      uint8_t iei = hexByteAt(udHex, p);
      p += 2;
      uint8_t iedl = hexByteAt(udHex, p);
      p += 2;
      if (p + iedl * 2 > textStart) return false;

      if (iei == 0x00 || iei == 0x08) {
        if (hasConcat || iedl != (iei == 0x00 ? 3 : 4)) return false;
        hasConcat = true;
      }
      if (iei == 0x00) {
        out.refNumber = hexByteAt(udHex, p);
        out.totalParts = hexByteAt(udHex, p + 2);
        out.partNumber = hexByteAt(udHex, p + 4);
      } else if (iei == 0x08) {
        out.refNumber = ((int)hexByteAt(udHex, p) << 8) | hexByteAt(udHex, p + 2);
        out.totalParts = hexByteAt(udHex, p + 4);
        out.partNumber = hexByteAt(udHex, p + 6);
      }

      p += iedl * 2;
    }
  }

  if (hasConcat && (out.totalParts < 1 || out.partNumber < 1)) return false;
  if (!normalizeSmsConcatInfo(out.refNumber, out.partNumber, out.totalParts)) return false;

  if (alphabet == 0x08) {
    if ((udBytes - udhTotalBytes) % 2 != 0) return false;
    String textHex = udHex.substring(textStart);
    out.text = decodeUcs2Hex(textHex);
  } else {
    int textSeptets = udl - skipSeptets;
    out.text = decodeGsm7Bit(udHex, textSeptets, skipSeptets);
  }
  out.hasUserData = out.text.length() > 0;
  out.tpduBytes = pos / 2 + udBytes - scaLen - 1;

  return true;
}

// 仅在有界解析成功且正文非空后调用。合法 PDU 也可能超出 pdulib 0.5.x 的能力。
static bool canDecodeSmsWithPdulib(const String& pduHex, const DecodedPDU& checked) {
  int scaLen = hexByteAt(pduHex, 0);
  // 空 SCA 会令库偏移错误；最长 SCA / 带 '+' 的 OA 可能超出其 21 字节缓冲。
  if (scaLen == 0 || scaLen == 11 || checked.sender.length() > 20) return false;
  if (checked.dcs != 0x00 && checked.dcs != 0x08) return false;

  int oaPos = 2 + scaLen * 2 + 2;
  // pdulib 的数字地址分支只处理 TON 0/1/2；其余 BCD 类型会返回长度 0，导致错位。
  // SCA 与 OA 都使用同一库分支，必须同时检查；其 TON5 分支不属于本项目支持的数字地址。
  if (smsAddressTon(hexByteAt(pduHex, 2)) > 2 ||
      smsAddressTon(hexByteAt(pduHex, oaPos + 2)) > 2) return false;
  int oaBytes = (hexByteAt(pduHex, oaPos) + 1) / 2;
  int udlPos = oaPos + 4 + oaBytes * 2 + 4 + 14;  // OA、PID/DCS、SCTS
  int udPos = udlPos + 2;
  if (checked.fo & 0x40) {
    // 库对 16 位 UDH 固定少扣一个 septet；8 位 UDH 首字符 '@' 也会丢失。
    // 不先调用库再裁尾，否则既可能越界，也会误删真实正文中的 '@' / 扩展字符。
    if (checked.dcs == 0x00) return false;
    int udhl = hexByteAt(pduHex, udPos);
    // UCS2 仅单个标准 concat IE 可安全交给库；其它 IE 由有界解析器处理。
    return (udhl == 5 && hexByteAt(pduHex, udPos + 2) == 0x00 && hexByteAt(pduHex, udPos + 4) == 3) ||
           (udhl == 6 && hexByteAt(pduHex, udPos + 2) == 0x08 && hexByteAt(pduHex, udPos + 4) == 4);
  }
  if (checked.dcs == 0x00) {
    int bit = (hexByteAt(pduHex, udlPos) - 1) * 7;
    int pos = udPos + (bit / 8) * 2;
    int shift = bit % 8;
    uint16_t bits = hexByteAt(pduHex, pos);
    if (shift > 1) bits |= (uint16_t)hexByteAt(pduHex, pos + 2) << 8;
    // 库处理末尾 ESC 时不检查下一个 septet 是否存在。
    if (((bits >> shift) & 0x7F) == 0x1B) return false;
  }
  return true;
}

bool shouldUseFallbackDecodedText(const String& pdulibText, const String& fallbackText) {
  if (fallbackText.length() == 0) return false;
  if (pdulibText.length() == 0) return true;
  if (!isLikelyValidUtf8(pdulibText) && isLikelyValidUtf8(fallbackText)) return true;
  if (containsReplacementCharUtf8(pdulibText) && !containsReplacementCharUtf8(fallbackText)) return true;
  return false;
}

static bool smsWaitingPdu = false;
static unsigned long smsPduStartedAt = 0;
static int smsExpectedTpduBytes = 0;

void resetModemUrc() {
  smsWaitingPdu = false;
  smsExpectedTpduBytes = 0;
}

static void expireSmsPduWait() {
  if (smsWaitingPdu && millis() - smsPduStartedAt >= SMS_CMT_PDU_TIMEOUT_MS) {
    resetModemUrc();
    Serial.println("CMT等待PDU超时，已取消等待");
  }
}

// PDU 模式 +CMT: [<alpha>],<length>；兼容省略 alpha 逗号的单长度形式。
// 空/带逗号的引号 alpha 可用，但空长度、负数、溢出、额外字段均不是有效头。
static int smsCmtTpduLength(const String& line) {
  const char* p = line.c_str() + 5;
  while (*p == ' ' || *p == '\t') p++;
  if (*p == '"') {
    p++;
    while (*p && *p != '"') p++;
    if (*p != '"') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ',') return 0;
    p++;
  } else if (*p == ',') {
    p++;
  }
  while (*p == ' ' || *p == '\t') p++;
  if (*p < '0' || *p > '9') return 0;
  int length = 0;
  while (*p >= '0' && *p <= '9') {
    int digit = *p++ - '0';
    if (length > ((SERIAL_BUFFER_SIZE - 1) / 2 - digit) / 10) return 0;
    length = length * 10 + digit;
  }
  while (*p == ' ' || *p == '\t') p++;
  return *p == '\0' ? length : 0;
}

// 兼容原 loop 调用；所有 Serial1 字节和未完成行都归 modem 管理。
void checkSerial1URC() {
  expireSmsPduWait();
  pollModem();
}

// 仅解析 pollModem 提供的完整行，不自行读取串口，也不吞掉 AT 事务响应。
bool handleModemUrc(const String& line) {
  expireSmsPduWait();
  if (line.length() == 0) return false;
  if (line.startsWith("+CMT:")) {
    resetModemUrc();
    smsExpectedTpduBytes = smsCmtTpduLength(line);
    if (smsExpectedTpduBytes > 0) {
      Serial.println("检测到有效+CMT，等待PDU数据...");
      smsPduStartedAt = millis();
      smsWaitingPdu = true;
    } else {
      Serial.println("CMT头长度无效，已忽略");
    }
    return true;
  }
  // 来电可以夹在 CMT 头与 PDU 之间，不能取消等待。
  if (handleCallUrc(line)) return true;

  if (smsWaitingPdu) {
    if (line.length() >= 2 && line.length() % 2 == 0 && isHexString(line)) {
      // AT+CIMI/IMEI 等数字响应也是 hex；只有声明长度及完整 SMS 结构都匹配才认领。
      // 非 PDU 行留给事务且不续期/清除等待；坏/缺失 PDU 最迟在绝对期限失效。
      int scaBytes = hexByteAt(line, 0);
      if ((int)line.length() / 2 != 1 + scaBytes + smsExpectedTpduBytes) return false;
      DecodedPDU fallbackSms;
      // pdulib 不接收长度参数；必须先验证完整字段、TP-UDL 和 UDH，不能失败后才校验。
      if (!decodeDeliverPDUFallback(line, fallbackSms) || fallbackSms.tpduBytes != smsExpectedTpduBytes) {
        // 长度已与 +CMT 吻合却结构校验失败：打印线索，便于定位新的运营商格式。
        Serial.println("⚠️ PDU长度匹配但结构校验失败，未认领: " + line);
        return false;
      }
      resetModemUrc();  // 归属已确认；在进入解码器/转发回调前消费等待状态。
      Serial.println("收到PDU数据: " + line);
      if (!fallbackSms.hasUserData) {
        Serial.println("PDU正文为空，已跳过解码与转发");
        return true;
      }

      bool fallbackDecoded = true;
      String sender = fallbackSms.sender;
      String timestamp = fallbackSms.timestamp;
      String text = fallbackSms.text;
      int refNumber = fallbackSms.refNumber;
      int partNumber = fallbackSms.partNumber;
      int totalParts = fallbackSms.totalParts;
      bool isSmsDeliver = fallbackSms.isSmsDeliver;
      bool hasUserData = fallbackSms.hasUserData;

      if (canDecodeSmsWithPdulib(line, fallbackSms) && pdu.decodePDU(line.c_str())) {
        String pdulibText(pdu.getText());
        if (shouldUseFallbackDecodedText(pdulibText, fallbackSms.text)) {
          Serial.println("✓ 使用已校验的fallback文本替换pdulib结果");
        } else {
          fallbackDecoded = false;
          sender = String(pdu.getSender());
          timestamp = formatTimestamp(String(pdu.getTimeStamp()));
          text = pdulibText;
          int* concatInfo = pdu.getConcatInfo();
          refNumber = concatInfo[0];
          partNumber = concatInfo[1];
          totalParts = concatInfo[2];
          Serial.println("✓ pdulib PDU解析成功");
        }
      } else {
        Serial.println("✓ 使用有界fallback PDU解析结果");
      }

      if (!isPduLikelyStorableSms(sender,
                                  timestamp,
                                  text,
                                  isSmsDeliver,
                                  hasUserData,
                                  fallbackDecoded,
                                  fallbackDecoded ? &fallbackSms : nullptr)) {
        Serial.println("⚠️ 识别为异常/非短信PDU，已跳过存储与转发");
        Serial.println("  原因线索:");
        Serial.println("  - sender=" + sender);
        Serial.println("  - timestamp=" + timestamp);
        Serial.printf("  - deliver=%s, userData=%s, fallback=%s\n",
                      isSmsDeliver ? "true" : "false",
                      hasUserData ? "true" : "false",
                      fallbackDecoded ? "true" : "false");
        Serial.println("  - text=" + text);
        return true;
      }

      Serial.println("=== 短信内容 ===");
      Serial.println("发送者: " + sender);
      Serial.println("时间戳: " + timestamp);
      Serial.println("内容: " + text);
      Serial.printf("长短信信息: 参考号=%d, 当前=%d, 总计=%d\n", refNumber, partNumber, totalParts);
      Serial.println("===============");

      // [0,0,0] 仅表示无分段标记，不能把含非法 concat UDH 的 PDU 当普通短信。
      if (refNumber == 0 && partNumber == 0 && totalParts == 0 &&
          (fallbackSms.fo & 0x40)) {
        refNumber = fallbackSms.refNumber;
        partNumber = fallbackSms.partNumber;
        totalParts = fallbackSms.totalParts;
      }
      // 判断是否为长短信，拒绝越界分段，避免写入固定大小缓存。
      if (!normalizeSmsConcatInfo(refNumber, partNumber, totalParts)) {
        Serial.printf("异常长短信分段信息，已丢弃: part=%d total=%d\n", partNumber, totalParts);
        return true;
      }
      if (totalParts > 1 && partNumber > 0) {
        Serial.printf("收到长短信分段 %d/%d\n", partNumber, totalParts);

        uint64_t timeSignature = smsConcatFingerprint(timestamp);
        uint64_t signature = smsConcatFingerprint(text, (timeSignature ^ 0xFF) * 0x100000001B3ULL);
        // 尚未转发的疑似重放不能挡住同参考号的新总段数/同序号新正文或新 SCTS。
        for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
          if (concatBuffer[i].inUse && smsConcatState[i].replayOnly &&
              concatBuffer[i].refNumber == refNumber && concatBuffer[i].sender == sender) {
            int index = partNumber - 1;
            if (concatBuffer[i].totalParts != totalParts ||
                ((smsConcatState[i].forwardedMask & (1U << index)) && smsConcatState[i].timestamps[index] != timeSignature)) {
              clearConcatSlot(i);
            } else if (concatBuffer[i].parts[index].valid && smsConcatState[i].signatures[index] != signature) {
              // 同时间的新正文可能只改一段；其它已收到的共享段仍属于候选新短信。
              concatBuffer[i].parts[index].valid = false;
              concatBuffer[i].parts[index].text = "";
              concatBuffer[i].receivedParts--;
              concatBuffer[i].firstPartTime = millis();
            }
          }
        }
        // 查找或创建缓存槽位
        int slot = findOrCreateConcatSlot(refNumber, sender.c_str(), totalParts);
        if (slot < 0) {
          Serial.println("长短信缓存不可用或总段数不一致，已丢弃当前分段（可重投）");
          return true;
        }

        // 存储该分段（partNumber 从 1 开始，数组从 0 开始）
        int partIndex = partNumber - 1;
        if (partIndex >= 0 && partIndex < MAX_CONCAT_PARTS) {
          if (!concatBuffer[slot].parts[partIndex].valid) {
            auto& part = concatBuffer[slot].parts[partIndex];
            part.text = text;
            bool copied = text.length() > 0 && part.text.length() == text.length();
            // 首段时间戳和正文均复制完整后，才提交 valid、计数及签名。
            if (copied && concatBuffer[slot].receivedParts == 0) {
              concatBuffer[slot].timestamp = timestamp;
              copied = timestamp.length() > 0 && concatBuffer[slot].timestamp.length() == timestamp.length();
            }
            if (!copied) {
              part.text = "";
              Serial.printf("[sms] ERROR: concat_cache_allocation_failed ref=%d part=%d; awaiting retransmission\n",
                            refNumber, partNumber);
              if (concatBuffer[slot].receivedParts == 0) clearConcatSlot(slot);
              return true;  // 不重新分类/登记失败段，已缓存的其它段仍可用于重投恢复。
            }
            part.valid = true;
            smsConcatState[slot].signatures[partIndex] = signature;
            smsConcatState[slot].timestamps[partIndex] = timeSignature;
            concatBuffer[slot].receivedParts++;

            Serial.printf("  已缓存分段 %d，当前已收到 %d/%d\n",
                         partNumber,
                         concatBuffer[slot].receivedParts,
                         totalParts);
          } else {
            Serial.printf("  分段 %d 已存在，跳过\n", partNumber);
          }
        }

        bool wasReplayOnly = smsConcatState[slot].replayOnly;
        classifyConcatReplay(slot);
        if (wasReplayOnly && !smsConcatState[slot].replayOnly) concatBuffer[slot].firstPartTime = millis();
        // 即使剩余缺段已在旧组发过，也等本轮收齐或 30s 超时；新组可能逆序到达。
        // 超时补充只带未转发的段，纯重放不通知；相同 key/SCTS 的未见段无法绝对判别。
        if (concatBuffer[slot].receivedParts >= totalParts) {
          Serial.println("长短信已收齐，开始合并转发");
          finishConcatSms(slot);
        }
      } else {
        // 普通短信，直接处理
        processPreparedSmsContent(sender.c_str(), text, timestamp);
      }

      return true;
    }
  }
  // OK、ERROR、+CMGS 等交回事务；插入的响应不应丢失随后到来的 PDU。
  return false;
}
