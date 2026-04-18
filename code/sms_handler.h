#ifndef SMS_HANDLER_H
#define SMS_HANDLER_H

#include <Arduino.h>
#include "config.h"

// 前向声明 (PDU 类型在 code.ino 中通过 pdulib.h 定义)
class PDU;

// fallback PDU解析结构
struct DecodedPDU {
  String sender;
  String timestamp;
  String text;
  int refNumber;
  int partNumber;
  int totalParts;
};

// 外部依赖变量声明
extern PDU pdu;
extern ConcatSms concatBuffer[MAX_CONCAT_MESSAGES];

// 函数声明
void initConcatBuffer();
int findOrCreateConcatSlot(int refNumber, const char* sender, int totalParts);
String assembleConcatSms(int slot);
void clearConcatSlot(int slot);
void checkConcatTimeout();
bool sendSMS(const char* phoneNumber, const char* message);
void processSmsContent(const char* sender, const char* text, const char* timestamp);
void checkSerial1URC();
String readSerialLine(HardwareSerial& port);
bool isHexString(const String& str);
int hexNibble(char c);
uint8_t hexByteAt(const String& s, int pos);
String swapSemiOctetsToDigits(const String& hex, int digitCount);
void appendUtf8(String& out, uint16_t cp);
String decodeUcs2Hex(const String& hex);
String decodeScts(const String& sctsHex);
bool decodeDeliverPDUFallback(const String& pduHex, DecodedPDU& out);

#endif // SMS_HANDLER_H
