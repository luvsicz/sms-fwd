/*
 * call_handler.ino - 来电/通话处理函数实现
 */

#include "call_handler.h"
#include "config.h"
#include "push_service.h"

// 从 +CLIP URC 中提取来电号码
String parseClipNumber(const String& line) {
  // 典型格式:
  // +CLIP: "13800138000",145,,,,0
  int firstQuote = line.indexOf('"');
  if (firstQuote < 0) return "";
  int secondQuote = line.indexOf('"', firstQuote + 1);
  if (secondQuote < 0) return "";

  String number = line.substring(firstQuote + 1, secondQuote);
  number.trim();
  return number;
}

// 解析 +CLCC URC
bool parseClccInfo(const String& line, String& number, int& dir, int& stat) {
  number = "";
  dir = -1;
  stat = -1;

  int colon = line.indexOf(':');
  if (colon < 0) return false;

  String rest = line.substring(colon + 1);
  rest.trim();

  String fields[12];
  int fieldCount = 0;
  bool inQuote = false;
  int start = 0;

  for (int i = 0; i <= rest.length(); i++) {
    bool atEnd = (i == rest.length());
    char ch = atEnd ? '\0' : rest.charAt(i);

    if (!atEnd && ch == '"') inQuote = !inQuote;

    if (atEnd || (ch == ',' && !inQuote)) {
      if (fieldCount < 12) {
        fields[fieldCount] = rest.substring(start, i);
        fields[fieldCount].trim();
        fieldCount++;
      }
      start = i + 1;
    }
  }

  // +CLCC: <id>,<dir>,<stat>,<mode>,<mpty>,"<number>",<type>,...
  if (fieldCount < 7) return false;

  dir = fields[1].toInt();
  stat = fields[2].toInt();
  number = fields[5];
  number.trim();
  if (number.startsWith("\"") && number.endsWith("\"") && number.length() >= 2) {
    number = number.substring(1, number.length() - 1);
  }
  number.trim();
  return true;
}

// 处理来电通知
void processIncomingCall(const char* caller) {
  String callerStr = String(caller);
  callerStr.trim();
  if (callerStr.length() == 0) callerStr = "未知号码";

  // 短时间重复来电去重
  if (callerStr == lastCallNumber && millis() - lastCallNotifyTime < CALL_NOTIFY_DEDUP_MS) {
    Serial.println("短时间内重复来电通知，已忽略");
    return;
  }
  lastCallNumber = callerStr;
  lastCallNotifyTime = millis();

  Serial.println("=== 处理来电通知 ===");
  Serial.println("来电号码: " + callerStr);
  Serial.println("===================");

  if (callerStr != "未知号码" && isNumberFiltered(callerStr.c_str())) {
    Serial.println("来电号码被过滤，忽略该来电通知");
    return;
  }

  String timestamp = getCurrentTimeString();
  addCallToHistory(callerStr.c_str(), timestamp.c_str());

  String pushMessage = "[来电通知] 来电号码: " + callerStr;
  sendSMSToServer(callerStr.c_str(), pushMessage.c_str(), timestamp.c_str());

  String subject = "来电通知：" + callerStr;
  String body = "检测到新的来电\n来电号码：" + callerStr + "\n时间：" + timestamp + "\n";
  sendEmailNotification(subject.c_str(), body.c_str());
}

// 处理与来电相关的 URC，已处理返回 true
bool handleCallUrc(const String& line) {
  // 来电振铃
  if (line == "RING") {
    Serial.println("检测到来电振铃 RING");
    return true;
  }

  // CLIP 号码上报
  if (line.startsWith("+CLIP:")) {
    String caller = parseClipNumber(line);
    if (caller.length() == 0) caller = "未知号码";
    Serial.println("检测到来电号码(+CLIP): " + caller);
    processIncomingCall(caller.c_str());
    return true;
  }

  // CLCC 来电状态上报
  if (line.startsWith("+CLCC:")) {
    String caller;
    int dir = -1;
    int stat = -1;
    if (parseClccInfo(line, caller, dir, stat)) {
      Serial.printf("检测到来电状态(+CLCC): dir=%d stat=%d number=%s\n", dir, stat, caller.c_str());
      if (dir == 1 && (stat == 4 || stat == 5)) {
        if (caller.length() == 0) caller = "未知号码";
        processIncomingCall(caller.c_str());
      }
    }
    return true;
  }

  return false;
}

