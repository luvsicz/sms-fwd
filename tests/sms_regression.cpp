// Executes the actual SMS + modem sources and headers on a host compiler.
// Actual installed pdulib is compiled as SmsRealPdu; only failure injection is a wrapper.
// See test_sms_regression.ps1. Synthetic messages only; no serial device/network/storage.
#include "sms_host_stubs.h"
#define PDU SmsRealPdu
#include <pdulib.h>
#undef PDU
#include <array>
#include <climits>
#include <functional>
#include <iostream>
#include <vector>

class PDU {
  SmsRealPdu real{4096};
public:
  bool forceFailure = false;
  bool overrideConcat = false;
  bool overrideText = false;
  std::array<int, 3> concat{{0, 0, 0}};
  std::string text;
  int decodeCalls = 0;
  bool decodePDU(const char* hex) {
    decodeCalls++;
    return !forceFailure && real.decodePDU(hex);
  }
  const char* getSender() { return real.getSender(); }
  const char* getTimeStamp() { return real.getTimeStamp(); }
  const char* getText() { return overrideText ? text.c_str() : real.getText(); }
  int* getConcatInfo() { return overrideConcat ? concat.data() : real.getConcatInfo(); }
  void setSCAnumber() { real.setSCAnumber(); }
  int encodePDU(const char* phone, const char* message) { return real.encodePDU(phone, message); }
  const char* getSMS() { return real.getSMS(); }
};

#include "sms_handler.h"
void sendSMSToServer(const char*, const char*, const char*);
void publishMqttSmsReceived(const char*, const char*, const char*);
void sendEmailNotification(const char*, const char*);
bool handleCallUrc(const String&);
#include "sms_handler.ino"
#include "modem_handler.ino"

HardwareSerial Serial, Serial1;
unsigned long smsTestMillis = 1000;
PDU pdu;
ConcatSms concatBuffer[MAX_CONCAT_MESSAGES];
struct Message { String sender, text, timestamp; };
std::vector<Message> history, httpMessages, mqttMessages;
std::vector<String> emailBodies;
std::vector<std::string> effects;
bool filterNumber = false, filterContent = false;
std::function<void()> beforeHistory;
void addSmsToHistory(const char* sender, const char* text, const char* timestamp) {
  if (beforeHistory) beforeHistory(); // End the entry-copy probe before the host sink copies data.
  history.push_back({sender, text, timestamp}); effects.push_back("history");
}
bool isNumberFiltered(const char*) { effects.push_back("number-filter"); return filterNumber; }
bool isContentFiltered(const char*) { effects.push_back("content-filter"); return filterContent; }
void sendSMSToServer(const char* sender, const char* text, const char* timestamp) {
  httpMessages.push_back({sender, text, timestamp}); effects.push_back("http");
}
void publishMqttSmsReceived(const char* sender, const char* text, const char* timestamp) {
  mqttMessages.push_back({sender, text, timestamp}); effects.push_back("mqtt");
}
void sendEmailNotification(const char*, const char* body) { emailBodies.emplace_back(body); effects.push_back("email"); }
bool handleCallUrc(const String&) { return false; }

static int checks = 0;
static void require(bool condition, const std::string& label) {
  checks++;
  if (!condition) throw std::runtime_error(label);
}
static void equal(const String& actual, const String& expected, const std::string& label) {
  require(actual == expected, label + " [actual=" + actual.c_str() + ", expected=" + expected.c_str() + "]");
}
static std::string octet(unsigned int value) {
  const char* digits = "0123456789ABCDEF";
  return std::string{digits[(value >> 4) & 15], digits[value & 15]};
}
static std::string hex(const std::vector<uint8_t>& data) {
  std::string result;
  for (uint8_t value : data) result += octet(value);
  return result;
}
static std::string scts(int year = 26, int month = 4, int day = 18,
                        int hour = 12, int minute = 34, int second = 56, int quarters = 32) {
  std::string result;
  for (int value : {year, month, day, hour, minute, second}) result += octet((value % 10) * 16 + value / 10);
  int magnitude = quarters < 0 ? -quarters : quarters;
  return result + octet((magnitude % 10) * 16 + magnitude / 10 + (quarters < 0 ? 8 : 0));
}
static std::vector<uint8_t> concat8(int ref, int total, int part) {
  return {0, 3, static_cast<uint8_t>(ref), static_cast<uint8_t>(total), static_cast<uint8_t>(part)};
}
static std::vector<uint8_t> concat16(int ref, int total, int part) {
  return {8, 4, static_cast<uint8_t>(ref >> 8), static_cast<uint8_t>(ref),
          static_cast<uint8_t>(total), static_cast<uint8_t>(part)};
}
static std::vector<uint8_t> bytes(const std::string& text) { return {text.begin(), text.end()}; }
static std::string deliver(int dcs, int udl, const std::vector<uint8_t>& data,
                           bool udhi = false, const std::string& timestamp = scts()) {
  // SCA + numeric originating address (synthetic test fixture, not a live recipient).
  return "0791683108200505" + octet(udhi ? 0x44 : 0x04) + "0B813108108300F0" +
         "00" + octet(dcs) + timestamp + octet(udl) + hex(data);
}
static std::string withByte(std::string input, size_t index, uint8_t value) {
  input.replace(index * 2, 2, octet(value));
  return input;
}
static std::string gsm(const std::vector<uint8_t>& text, const std::vector<uint8_t>& ies = {},
                        const std::string& timestamp = scts()) {
  int headerBytes = ies.empty() ? 0 : static_cast<int>(ies.size()) + 1;
  int headerSeptets = (headerBytes * 8 + 6) / 7;
  int udl = headerSeptets + static_cast<int>(text.size());
  std::vector<uint8_t> packed((udl * 7 + 7) / 8, 0);
  if (headerBytes) {
    packed.at(0) = static_cast<uint8_t>(ies.size());
    std::copy(ies.begin(), ies.end(), packed.begin() + 1);
  }
  // Independent bit-by-bit fixture encoder; not the production unpacking algorithm.
  for (size_t i = 0; i < text.size(); i++) {
    for (int bit = 0; bit < 7; bit++) {
      if (text[i] & (1 << bit)) {
        size_t offset = headerSeptets * 7 + i * 7 + bit;
        packed.at(offset / 8) |= static_cast<uint8_t>(1 << (offset % 8));
      }
    }
  }
  return deliver(0, udl, packed, headerBytes != 0, timestamp);
}
static std::string gsm(const std::string& text, const std::vector<uint8_t>& ies = {},
                        const std::string& timestamp = scts()) { return gsm(bytes(text), ies, timestamp); }
static std::string ucs2(const std::u16string& text, const std::vector<uint8_t>& ies = {},
                         const std::string& timestamp = scts()) {
  std::vector<uint8_t> data;
  if (!ies.empty()) {
    data.push_back(static_cast<uint8_t>(ies.size()));
    data.insert(data.end(), ies.begin(), ies.end());
  }
  for (char16_t value : text) {
    data.push_back(static_cast<uint8_t>(value >> 8)); data.push_back(static_cast<uint8_t>(value));
  }
  return deliver(8, static_cast<int>(data.size()), data, !ies.empty(), timestamp);
}
static void resetState() {
  beforeHistory = nullptr;
  resetSmsStringFault();
  resetModemUrc();
  initConcatBuffer();
  for (int slot = 0; slot < MAX_CONCAT_MESSAGES; slot++) clearConcatSlot(slot);
  history.clear(); httpMessages.clear(); mqttMessages.clear(); emailBodies.clear(); effects.clear();
  filterNumber = filterContent = false;
  pdu.forceFailure = pdu.overrideConcat = pdu.overrideText = false;
  pdu.decodeCalls = 0;
  Serial.reset(); Serial1.reset();
  resetSmsHostClock();
}
static void feed(const std::string& input) {
  int iterations = 0;
  // Preserve the original per-line driver checks: pollModem now drains a whole RX burst.
  for (size_t offset = 0; offset < input.size();) {
    size_t lf = input.find('\n', offset);
    size_t end = lf == std::string::npos ? input.size() : lf + 1;
    Serial1.feed(input.substr(offset, end - offset));
    while (Serial1.available()) {
      require(iterations++ < 20, "URC fixture should drain without a loop");
      checkSerial1URC();
    }
    offset = end;
  }
}
// Contract budgets, deliberately independent of implementation constants for red/green runs.
static constexpr unsigned long cmtWaitMs = 5000, recentConcatMs = 120000;
static constexpr int recentConcatCapacity = 5;
static std::string cmtHeader(const std::string& pduHex) {
  // Malformed fixtures still need a plausible header; do not parse beyond a truncated SCA.
  size_t scaBytes = pduHex.size() >= 2 && std::isxdigit(static_cast<unsigned char>(pduHex[0])) &&
      std::isxdigit(static_cast<unsigned char>(pduHex[1])) ? std::stoul(pduHex.substr(0, 2), nullptr, 16) : 0;
  size_t tpduBytes = pduHex.size() / 2 > scaBytes + 1 ? pduHex.size() / 2 - scaBytes - 1 : 23;
  return "+CMT: ," + std::to_string(tpduBytes) + "\r\n";
}
static void receive(const std::string& pduHex) { feed(cmtHeader(pduHex) + pduHex + "\r\n"); }
static void delivered(const String& text, const String& time = "2026-04-18 12:34:56") {
  require(history.size() == 1 && httpMessages.size() == 1 && mqttMessages.size() == 1 && emailBodies.size() == 1,
          "one history/HTTP/MQTT/email event");
  equal(history.back().text, text, "history body is exact");
  equal(httpMessages.back().text, text, "HTTP body is exact");
  equal(mqttMessages.back().text, text, "MQTT body is exact");
  equal(history.back().timestamp, time, "normalized history timestamp");
  equal(httpMessages.back().timestamp, time, "normalized HTTP timestamp");
  equal(mqttMessages.back().timestamp, time, "normalized MQTT timestamp");
  equal(emailBodies.back(), "发送者: " + history.back().sender + "\n时间: " + time + "\n内容:\n" + text,
        "email preserves complete body");
}
static int activeSlots() {
  int result = 0;
  for (auto& slot : concatBuffer) if (slot.inUse) result++;
  return result;
}
static void fallbackEquals(const std::string& input, const String& text, int ref = 0, int part = 1, int total = 1) {
  DecodedPDU decoded;
  require(decodeDeliverPDUFallback(input, decoded), "fallback accepts valid fixture");
  equal(decoded.text, text, "fallback retains every character");
  require(decoded.refNumber == ref && decoded.partNumber == part && decoded.totalParts == total,
          "fallback concat metadata");
  equal(decoded.timestamp, "2026-04-18 12:34:56", "fallback UTC+8 timestamp");
}
static void fallbackRejects(const std::string& input, const std::string& label) {
  DecodedPDU decoded;
  require(!decodeDeliverPDUFallback(input, decoded), label);
  resetState();
  // A call counter + failure sentinel detects a missing preflight without actually
  // passing malformed data to pdulib's unchecked char* reads (also without ASan).
  pdu.forceFailure = true;
  receive(input);
  require(pdu.decodeCalls == 0, label + ": rejected before pdulib is called");
  require(effects.empty() && history.empty() && httpMessages.empty() && mqttMessages.empty() &&
          emailBodies.empty() && activeSlots() == 0, label + ": no receive side effects");
  require(Serial1.tx.empty(), "malformed receive never transmits modem commands");
  if (!input.empty() && std::all_of(input.begin(), input.end(), [](unsigned char c) { return std::isxdigit(c); })) {
    // Invalid hex may be an AT numeric response, not proof that the CMT body arrived.
    // It must not reach pdulib, and the pending header must expire rather than live forever.
    smsTestMillis += cmtWaitMs;
    feed(gsm("orphan") + "\r\n");
    require(!smsWaitingPdu, "rejected/missing PDU cannot keep CMT alive beyond its deadline");
    require(pdu.decodeCalls == 0 && history.empty(), "orphan PDU cannot reuse an expired CMT");
  }
  pdu.forceFailure = false;
  receive(gsm("recovery"));
  delivered("recovery");
}

static void timestampTests() {
  equal(formatTimestamp("26041812345632"), "2026-04-18 12:34:56", "pdulib 14 chars +08");
  equal(formatTimestamp("26041812345600"), "2026-04-18 20:34:56", "UTC to UTC+8");
  equal(formatTimestamp("26041812345622"), "2026-04-18 15:04:56", "quarter-hour +05:30");
  equal(formatTimestamp("260418123456:0"), "2026-04-19 01:34:56", "pdulib negative nibble -05");
  equal(formatTimestamp("26041812345694"), "2026-04-19 00:04:56", "pdulib digit-only negative -03:30");
  equal(formatTimestamp("26041812345680"), "2026-04-18 20:34:56", "negative zero timezone");
  equal(formatTimestamp("2026-04-19 00:04:56"), "2026-04-19 00:04:56", "idempotent normalized timestamp");
  equal(decodeScts(scts(24, 2, 28, 20, 0, 0, 0)), "2024-02-29 04:00:00", "leap-day forward");
  equal(decodeScts(scts(24, 3, 1, 0, 30, 0, 56)), "2024-02-29 18:30:00", "leap-day backward");
  equal(decodeScts(scts(23, 3, 1, 0, 30, 0, 56)), "2023-02-28 18:30:00", "non-leap backward");
  equal(decodeScts(scts(26, 4, 30, 22, 0, 0, 0)), "2026-05-01 06:00:00", "month rollover");
  equal(decodeScts(scts(26, 12, 31, 23, 45, 0, -14)), "2027-01-01 11:15:00", "negative timezone year rollover");
  equal(decodeScts(scts(25, 1, 1, 1, 0, 0, 56)), "2024-12-31 19:00:00", "year rollback");
  equal(decodeScts(scts(26, 4, 30, 23, 30, 0, -79)), "2026-05-02 03:15:00", "multi-day BCD timezone carry");
  require(isTimestampLikelyValidSms("2024-02-29 23:59:59"), "valid leap date");
  require(smsDaysInMonth(2000, 2) == 29 && smsDaysInMonth(2100, 2) == 28, "Gregorian century rules");
  for (const char* invalid : {"", "260418123456", "260418123456320", "260418123456-20", "260418123456A0",
       "2604181234560:", "260418123456/0", "26x41812345632", "26131812345632", "26001812345632",
       "26040012345632", "26043112345632", "23022912345632", "26041824345632", "26041812605632",
       "26041812346032", "2026-02-29 00:00:00", "2026-04-31 00:00:00", "2026-04-18 1x:34:56",
       "2026/04/18 12:34:56", "2019-12-31 23:59:59", "2100-01-01 00:00:00"}) {
    equal(formatTimestamp(invalid), "", std::string("reject malformed timestamp: ") + invalid);
  }
  require(!isTimestampLikelyValidSms("26041812345632"), "storage gate stays strict; normalize first");
  equal(decodeScts("624081214365F3"), "", "invalid timezone BCD digit");
  equal(decodeScts("6F408121436523"), "", "invalid date BCD digit");
  equal(decodeScts("62408121436523FF"), "", "SCTS length exact");
  equal(decodeScts("6240812143652Z"), "", "non-hex SCTS rejected");
}

static void standaloneTests() {
  for (bool forceFallback : {false, true}) {
    for (const auto& fixture : std::vector<std::pair<std::string, std::string>>{
         {gsm("12345678"), "12345678"}, {ucs2(u"验证码123456"), "验证码123456"}}) {
      resetState(); pdu.forceFailure = forceFallback;
      receive(fixture.first);
      delivered(fixture.second);
      require(pdu.decodeCalls == 1, "primary decoder attempted exactly once");
      require(activeSlots() == 0, "standalone takes no concat slot");
      require(effects == std::vector<std::string>{"history", "number-filter", "content-filter", "http", "mqtt", "email"},
              "history still precedes filters and notifications");
    }
    resetState(); pdu.forceFailure = forceFallback;
    receive(gsm("negative time", {}, scts(26, 4, 18, 12, 34, 56, -20)));
    delivered("negative time", "2026-04-19 01:34:56");
    resetState(); pdu.forceFailure = forceFallback;
    receive(gsm("invalid date", {}, scts(26, 2, 30)));
    require(history.empty(), "invalid date rejected before storage");
  }
  resetState(); pdu.overrideText = true; pdu.text = "\xEF\xBF\xBD";
  receive(gsm("fallback replacement"));
  delivered("fallback replacement");
  resetState(); pdu.overrideConcat = true; pdu.concat = {{0, 0, 0}};
  receive(ucs2(u"plain")); delivered("plain");
  resetState(); filterContent = true;
  receive(gsm("still stored"));
  require(history.size() == 1 && httpMessages.empty() && mqttMessages.empty() && emailBodies.empty(),
          "content filtering remains after history");
  for (const char* timestamp : {"260418123456:0", "2026-04-19 01:34:56"}) {
    resetState();
    processSmsContent("10086", "OTP\r\n123456\r\n", timestamp);
    delivered("OTP\r\n123456\r\n", "2026-04-19 01:34:56");
  }
}

static void bodyTests() {
  const std::vector<std::pair<std::u16string, std::string>> cases = {
    {u"123456", "123456"}, {u"验证码：\n123456", "验证码：\n123456"},
    {u"验证码：\n123456\n", "验证码：\n123456\n"}, {u"ABC12345", "ABC12345"},
    {u"https://example.com/ABC12345", "https://example.com/ABC12345"},
    {u"查看详情\nhttps://example.com/ABC12345", "查看详情\nhttps://example.com/ABC12345"},
    {u"查看详情\nhttps://example.com/ABC12345\n", "查看详情\nhttps://example.com/ABC12345\n"},
    {u"<#>  验证码\n123456\n/AbCd1234567", "<#>  验证码\n123456\n/AbCd1234567"},
    {u"  第一行\r\n第二行\r\n123456\r\n\t ", "  第一行\r\n第二行\r\n123456\r\n\t "},
    {u"正文\nOK\nERROR\n+CMT: this is text\n", "正文\nOK\nERROR\n+CMT: this is text\n"}
  };
  for (const auto& entry : cases) {
    equal(cleanSmsContent(entry.second), entry.second, "cleaner is lossless");
    for (bool forceFallback : {false, true}) {
      resetState(); pdu.forceFailure = forceFallback;
      receive(ucs2(entry.first)); delivered(entry.second);
    }
  }
  for (bool forceFallback : {false, true}) {
    resetState(); pdu.forceFailure = forceFallback;
    const auto input = gsm("OTP\r\n123456\r\n");
    feed(cmtHeader(input) + input + "\r\nOK\r\nERROR\r\n");
    delivered("OTP\r\n123456\r\n");
    require(pdu.decodeCalls == 1, "protocol OK/ERROR lines not parsed as body or another PDU");
  }
}

static void gsmFallbackTests() {
  for (const std::string text : {"A", "1234567", "12345678", "123456789", "123456789012345", "1234567890123456"}) {
    fallbackEquals(gsm(text), text);
    fallbackEquals(gsm(text, concat8(0, 2, 1)), text, 0, 1, 2);
    fallbackEquals(gsm(text, concat16(0xBEEF, 3, 2)), text, 0xBEEF, 2, 3);
  }
  for (int headerBytes = 3; headerBytes <= 9; headerBytes++) {
    std::vector<uint8_t> ies(headerBytes - 1, 0x55);
    ies.at(0) = 0x70;
    ies.at(1) = static_cast<uint8_t>(headerBytes - 3);
    fallbackEquals(gsm("UDH align 12345678", ies), "UDH align 12345678");
  }
  auto ies = concat8(7, 2, 1);
  ies.insert(ies.begin(), {0x70, 0});
  ies.insert(ies.end(), {0x71, 1, 0xAA});
  fallbackEquals(gsm("multiple IEs", ies), "multiple IEs", 7, 1, 2);
  const std::vector<uint8_t> extended{0x41, 0x1B, 0x14, 0x1B, 0x28, 0x1B, 0x29, 0x1B, 0x2F,
                                    0x1B, 0x3C, 0x1B, 0x3E, 0x1B, 0x3D, 0x1B, 0x40, 0x1B, 0x65};
  fallbackEquals(gsm(extended), "A^{}\\[]~|€");
  fallbackEquals(gsm(extended, concat8(3, 2, 1)), "A^{}\\[]~|€", 3, 1, 2);
  fallbackEquals(gsm(std::vector<uint8_t>{0x00, 0x41, 0x00, 0x42}, concat8(3, 2, 1)), "@A@B", 3, 1, 2);
  fallbackEquals(gsm(std::string(160, 'X')), std::string(160, 'X'));
  fallbackEquals(gsm(std::string(153, 'Y'), concat8(0xFF, 10, 10)), std::string(153, 'Y'), 0xFF, 10, 10);
  fallbackEquals(gsm(std::string(152, 'Z'), concat16(0xFFFF, 10, 10)), std::string(152, 'Z'), 0xFFFF, 10, 10);
  fallbackEquals(ucs2(u"短信正文", concat8(4, 2, 1)), "短信正文", 4, 1, 2);
  fallbackEquals(ucs2(u"短信正文", concat16(0xFFFF, 2, 2)), "短信正文", 0xFFFF, 2, 2);
  fallbackEquals(gsm("12345678") + "DEADBEEF", "12345678"); // TP-UDL bounds decoded data.
  equal(decodeGsm7Bit("FF", 2, 0), "", "cannot read partial septet");
  equal(decodeGsm7Bit("FF", 1, 1), "", "cannot skip past payload bits");
  equal(decodeGsm7Bit("F", 1, 0), "", "odd hex length is invalid");
  equal(decodeGsm7Bit("FF", INT_MAX, INT_MAX), "", "septet arithmetic cannot overflow");
  DecodedPDU empty;
  require(decodeDeliverPDUFallback(gsm(""), empty) && !empty.hasUserData, "empty UD not marked as body");
  require(decodeDeliverPDUFallback(gsm("", concat8(1, 2, 1)), empty) && !empty.hasUserData,
          "header-only UD not marked as body");
}

static void malformedTests() {
  fallbackRejects(deliver(0, 160, {0x41}), "GSM7 TP-UDL exceeds actual PDU");
  fallbackRejects(deliver(8, 140, {0, 0x41}), "UCS2 TP-UDL exceeds actual PDU");
  for (const auto& valid : {gsm("12345678"), gsm("UDH payload", concat8(7, 2, 1)),
                            gsm("UDH payload", concat16(0x1234, 2, 1)), ucs2(u"中文123456"),
                            ucs2(u"中文123456", concat8(7, 2, 1)),
                            ucs2(u"中文123456", concat16(0x1234, 2, 1)),
                            "00" + gsm("no SCA").substr(16)}) {
    for (size_t length = 0; length < valid.size(); length++) {
      fallbackRejects(valid.substr(0, length), "every truncated prefix is rejected without reading past input");
    }
  }
  fallbackRejects(gsm("12345678").substr(1), "odd PDU length");
  fallbackRejects(gsm("12345678") + "GG", "non-hex input");
  fallbackRejects("FF" + gsm("hello").substr(2), "SCA extends past PDU");
  fallbackRejects(deliver(0, 0, {}, true), "UDHI without UDHL");
  fallbackRejects(deliver(0, 1, {5}, true), "UDHL exceeds packed bytes");
  fallbackRejects(deliver(0, 1, {0}, true), "UDH septets exceed UDL despite enough bytes");
  fallbackRejects(deliver(0, 7, {6, 8, 4, 0, 1, 2, 1}, true), "UDH fill septets exceed UDL");
  fallbackRejects(deliver(0, 8, {1, 0x70, 0, 0, 0, 0, 0}, true), "truncated IE header at UDHL boundary");
  fallbackRejects(deliver(0, 8, {2, 0x70, 4, 1, 2, 3, 4}, true), "IE cannot consume text beyond UDHL");
  fallbackRejects(gsm("bad IE", {0, 2, 1, 2}), "8-bit concat IE length mismatch");
  fallbackRejects(gsm("bad IE", {8, 3, 1, 2, 1}), "16-bit concat IE length mismatch");
  auto duplicate = concat8(1, 2, 1);
  auto other = concat16(1, 2, 1);
  duplicate.insert(duplicate.end(), other.begin(), other.end());
  fallbackRejects(gsm("duplicate header", duplicate), "ambiguous concat IE duplication");
  for (auto info : std::vector<std::array<int, 3>>{{{0, 0, 0}}, {{1, 0, 2}}, {{1, 3, 2}},
       {{1, 1, 0}}, {{1, 2, 1}}, {{1, 1, MAX_CONCAT_PARTS + 1}}}) {
    fallbackRejects(gsm("bad concat", concat8(info[0], info[2], info[1])), "malformed concat remains rejected");
    fallbackRejects(ucs2(u"bad concat", concat16(info[0], info[2], info[1])), "malformed UCS2 concat rejected");
  }
  fallbackRejects(deliver(8, 1, {0x41}), "odd UCS2 bytes");
  fallbackRejects(deliver(8, 7, {5, 0, 3, 1, 2, 1, 0x41}, true), "odd UCS2 text after UDH");
  fallbackRejects(gsm(std::string(161, 'A')), "GSM7 > 140 packed octets");
  fallbackRejects(ucs2(std::u16string(71, u'中')), "UCS2 > 140 octets");
  fallbackRejects(deliver(4, 1, {0x41}), "unsupported DCS remains rejected");
  std::string notDeliver = gsm("submit"); notDeliver.replace(16, 2, "01");
  fallbackRejects(notDeliver, "non-DELIVER PDU");
  for (const auto& bad : {deliver(0, 8, {1, 0x70, 0, 0, 0, 0, 0}, true),
                          gsm("broken", concat8(0, 0, 0)), ucs2(u"broken").substr(0, 46)}) {
    resetState(); pdu.forceFailure = true;
    receive(bad);
    require(history.empty() && activeSlots() == 0, "forced fallback malformed PDU has no side effects");
  }
}

static void receivePreflightTests() {
  resetState();
  receive(ucs2(u"head", concat16(0x2345, 2, 1)));
  require(pdu.decodeCalls == 1 && activeSlots() == 1, "compatible UCS2 still uses the primary decoder");
  equal(pdu.getText(), "head", "primary decoder holds the first segment");
  const auto tail = ucs2(u"tail", concat16(0x2345, 2, 2));
  for (const auto& truncated : {deliver(0, 160, {0x41}), deliver(8, 140, {0, 0x41}),
                                tail.substr(0, tail.size() - 2), gsm("cut header").substr(0, 28)}) {
    pdu.forceFailure = true; // Fail safely if the preflight is removed.
    receive(truncated);
    require(pdu.decodeCalls == 1, "truncated receive never touches the live primary decoder");
    equal(pdu.getText(), "head", "rejected PDU preserves primary decode state");
    smsTestMillis += cmtWaitMs;
    feed("OK\r\nERROR\r\n" + tail + "\r\n");
    require(!smsWaitingPdu, "rejected PDU wait expires without touching concat state");
    require(pdu.decodeCalls == 1 && effects.empty(), "orphan tail cannot complete an in-flight message");
    require(activeSlots() == 1 && concatBuffer[0].refNumber == 0x2345 &&
            concatBuffer[0].totalParts == 2 && concatBuffer[0].receivedParts == 1 &&
            !concatBuffer[0].parts[1].valid, "malformed receive leaves concat metadata untouched");
    equal(concatBuffer[0].parts[0].text, "head", "cached first segment is unchanged");
    equal(concatBuffer[0].timestamp, "2026-04-18 12:34:56", "cached timestamp is unchanged");
  }
  pdu.forceFailure = false;
  receive(tail);
  delivered("headtail");
  require(pdu.decodeCalls == 2 && activeSlots() == 0, "valid continuation recovers normally");
  require(Serial1.tx.empty(), "receive preflight never sends AT commands");
}

static void gsmReceiveBoundaryTests() {
  // This is the real Serial1 -> pollModem -> handleModemUrc entry, not just a
  // fallback unit test. Installed pdulib produces valid UTF-8 "1234567@" here.
  resetState();
  receive(gsm("1234567", concat16(0xBEEF, 1, 1)));
  delivered("1234567");

  const std::vector<std::pair<std::vector<uint8_t>, std::string>> bodies = {
    {bytes("A"), "A"}, {bytes("1234567"), "1234567"}, {bytes("12345678"), "12345678"},
    {bytes("123456789"), "123456789"}, {bytes("OTP\r\n123456\r\n"), "OTP\r\n123456\r\n"},
    {{0, 0x41, 0, 0x42, 0}, "@A@B@"},
    {{0x1B, 0x14, 0x1B, 0x28, 0x1B, 0x29, 0x1B, 0x65}, "^{}€"}
  };
  for (bool forceFallback : {false, true}) {
    for (bool wide : {true, false}) {
      auto header = [&](int total, int part) { return wide ? concat16(0xBEEF, total, part) : concat8(0, total, part); };
      for (const auto& body : bodies) {
        resetState(); pdu.forceFailure = forceFallback;
        receive(gsm(body.first, header(1, 1)));
        delivered(body.second);
        require(pdu.decodeCalls == 0, "GSM7 UDH never enters incompatible pdulib decoder");
      }
      for (bool reverse : {false, true}) {
        resetState(); pdu.forceFailure = forceFallback;
        int first = reverse ? 2 : 1;
        receive(gsm(first == 1 ? "1234567" : "tail\r\n", header(2, first)));
        require(history.empty() && activeSlots() == 1, "real GSM7 segment cached");
        equal(concatBuffer[0].parts[first - 1].text, first == 1 ? "1234567" : "tail\r\n", "cached body has no padding character");
        receive(gsm(first == 1 ? "1234567" : "tail\r\n", header(2, first)));
        require(concatBuffer[0].receivedParts == 1, "real GSM7 duplicate ignored");
        receive(gsm(first == 1 ? "tail\r\n" : "1234567", header(2, 3 - first)));
        delivered("1234567tail\r\n");
        require(activeSlots() == 0, "real GSM7 completed assembly cleared");
      }
      resetState(); pdu.forceFailure = forceFallback;
      std::string maximum(wide ? 152 : 153, 'X');
      receive(gsm(maximum, header(1, 1))); delivered(maximum);
    }
    for (int headerBytes = 3; headerBytes <= 9; headerBytes++) {
      std::vector<uint8_t> ies(headerBytes - 1, 0x55);
      ies[0] = 0x70; ies[1] = static_cast<uint8_t>(headerBytes - 3);
      resetState(); pdu.forceFailure = forceFallback;
      receive(gsm("align 12345678", ies)); delivered("align 12345678");
      resetState(); pdu.forceFailure = forceFallback;
      receive(ucs2(u"UDH正文", ies)); delivered("UDH正文");
    }
    auto multiple = concat16(0x1234, 1, 1);
    multiple.insert(multiple.begin(), {0x70, 0});
    multiple.insert(multiple.end(), {0x71, 1, 0xAA});
    resetState(); pdu.forceFailure = forceFallback;
    receive(gsm("multiple IEs", multiple)); delivered("multiple IEs");
    resetState(); pdu.forceFailure = forceFallback;
    receive(ucs2(u"multiple IEs", multiple)); delivered("multiple IEs");
  }
  for (const auto& empty : {gsm(""), gsm("", concat8(1, 2, 1)), gsm("", concat16(1, 2, 1)), ucs2(u"")}) {
    resetState(); receive(empty);
    require(pdu.decodeCalls == 0 && effects.empty() && activeSlots() == 0, "empty/header-only UD never reaches pdulib or storage");
  }
  resetState();
  receive(gsm(std::vector<uint8_t>{0x41, 0x1B}));
  require(pdu.decodeCalls == 0, "dangling GSM7 ESC cannot read past pdulib's unpacked array");
  resetState();
  receive(gsm("1234567", concat16(0xBEEF, 1, 1)) + "DEADBEEF");
  require(pdu.decodeCalls == 0 && effects.empty(), "CMT body must end at TP-UDL, not accept appended octets");
  receive(gsm("1234567", concat16(0xBEEF, 1, 1)));
  delivered("1234567"); // Tightened receive framing does not delete real trailing text/padding bits.

  for (bool wide : {false, true}) {
    for (int length = 1; length <= 16; length++) {
      auto body = bytes(std::string(length - 1, '7'));
      body.push_back(0); // A real trailing '@' must survive; trimming is not a fix.
      auto input = gsm(body, wide ? concat16(0xBEEF, 1, 1) : concat8(0, 1, 1));
      int usedBits = ((wide ? 8 : 7) + length) * 7 % 8;
      if (usedBits) {
        auto last = std::stoul(input.substr(input.size() - 2), nullptr, 16);
        input = withByte(input, input.size() / 2 - 1, static_cast<uint8_t>(last | (0xFFu << usedBits)));
      }
      resetState();
      receive(input);
      delivered(std::string(length - 1, '7') + "@");
      require(pdu.decodeCalls == 0, "UDH text ignores nonzero spare bits at every septet boundary");
    }
  }
}

static void dcsAndAddressBoundaryTests() {
  for (int dcs : {0x00, 0x10, 0x11, 0x12, 0x13, 0x40, 0x50,
                  0xC0, 0xC3, 0xC8, 0xCB, 0xD0, 0xD3, 0xD8, 0xDB, 0xF0, 0xF3}) {
    for (const auto& header : {std::vector<uint8_t>{}, concat16(0x1234, 1, 1)}) {
      resetState();
      auto input = withByte(gsm("1234567", header), 18, dcs);
      receive(input); delivered("1234567");
      fallbackEquals(input, "1234567", header.empty() ? 0 : 0x1234);
      fallbackRejects(input.substr(0, input.size() - 2), "DCS-specific GSM7 length checked before library");
    }
  }
  for (int dcs : {0x08, 0x18, 0x19, 0x1A, 0x1B, 0x48, 0x58, 0xE0, 0xE3, 0xE8, 0xEB}) {
    for (const auto& header : {std::vector<uint8_t>{}, concat8(2, 1, 1)}) {
      resetState();
      auto input = withByte(ucs2(u"短信123456", header), 18, dcs);
      receive(input); delivered("短信123456");
      fallbackEquals(input, "短信123456", header.empty() ? 0 : 2);
      fallbackRejects(input.substr(0, input.size() - 2), "DCS-specific UCS2 length checked before library");
    }
  }
  for (int dcs : {0x04, 0x0C, 0x14, 0x1C, 0x20, 0x28, 0x60, 0x68, 0x80, 0xA0, 0xB0,
                  0xC4, 0xCC, 0xD4, 0xDC, 0xE4, 0xEC, 0xF4, 0xF8, 0xFF}) {
    fallbackRejects(withByte(gsm("not text"), 18, dcs), "binary/compressed/reserved DCS rejected");
  }
  auto valid = gsm("address bounds");
  for (const auto& mutation : std::vector<std::pair<size_t, uint8_t>>{
      {0, 12}, {0, 1}, {1, 0x11}, {1, 0xF1}, {2, 0x6A},
      {9, 0}, {9, 21}, {9, 255}, {10, 0x01}, {10, 0xF1}, {11, 0x3F}, {16, 0}}) {
    fallbackRejects(withByte(valid, mutation.first, mutation.second), "SCA/OA length, type or BCD invalid");
  }
  for (int mti : {1, 2, 3}) fallbackRejects(withByte(valid, 8, mti), "only SMS-DELIVER may reach pdulib");
  fallbackRejects(gsm("date", {}, scts(26, 2, 30)), "SCTS validity checked before pdulib");
  resetState(); receive("00" + valid.substr(16)); delivered("address bounds");
  require(pdu.decodeCalls == 0, "SCA length zero bypasses pdulib address misalignment");
  resetState();
  auto wideAddress = valid;
  wideAddress.replace(18, 16, "1491" + std::string(20, '1'));
  receive(wideAddress); delivered("address bounds");
  equal(history.back().sender, "+11111111111111111111", "20-digit international sender is not truncated");
  require(pdu.decodeCalls == 0, "international OA plus prefix cannot overflow pdulib's 21-byte buffer");
  resetState();
  receive("0B91" + std::string(20, '1') + valid.substr(16)); delivered("address bounds");
  require(pdu.decodeCalls == 0, "international SCA plus prefix cannot overflow pdulib's 21-byte buffer");

  // 真实中国电信 SP 长号短信（OA 类型 0xC1 = TON 4），+CMT: ,125。曾被误判为非数字地址而丢失。
  const std::string telecom =
      "0891683109520000F02414C1019653330700000010100008621150910454236630106DD85B9D7F5130119A8C8BC1"
      "78010036003300360032FF0C60A86B63572877ED4FE1767B5F55FF0C8BF75728003100355206949F518563099875"
      "976263D0793A63D04EA49A8C8BC17801FF0C520752FF5C069A8C8BC178016CC497324E8E4ED64EBA3002";
  const String telecomText = "【淘宝网】验证码6362，您正在短信登录，请在15分钟内按页面提示提交验证码，切勿将验证码泄露于他人。";
  equal(String(cmtHeader(telecom).c_str()), "+CMT: ,125\r\n", "telecom fixture matches logged CMT length");
  for (bool fallback : {false, true}) {
    resetState(); pdu.forceFailure = fallback;
    feed("+CMT: ,125\r\n" + telecom + "\r\n");
    delivered(telecomText, "2026-11-05 19:40:45");
    equal(history.back().sender, "10693533700000000101", "TON 4 sender digits preserved without '+'");
    equal(httpMessages.back().sender, history.back().sender, "HTTP entry receives the complete telecom sender");
    require(!smsWaitingPdu && smsExpectedTpduBytes == 0, "telecom PDU consumes the pending header immediately");
    require(pdu.decodeCalls == 0, "TON 4 bypasses pdulib, which misaligns non-0/1/2 address types");
    feed(telecom + "\r\n");
    smsTestMillis += cmtWaitMs;
    checkSerial1URC();
    delivered(telecomText, "2026-11-05 19:40:45");
    require(Serial.tx.find("CMT等待PDU超时") == std::string::npos,
            "consumed telecom PDU neither repeats delivery nor later times out");
  }
  // SCA 与 OA 的数字 TON 都需保护；0/1/2 可用库，3/4/6 必须由有界解析器处理。
  for (const auto& base : {valid, ucs2(u"address bounds")}) {
    for (int scaType : {0x81, 0x91, 0xA1, 0xB1, 0xC1, 0xE1}) {
      for (int oaType : {0x81, 0x91, 0xA1, 0xB1, 0xC1, 0xE1}) {
        resetState();
        receive(withByte(withByte(base, 1, scaType), 10, oaType));
        delivered("address bounds");
        equal(history.back().sender, oaType == 0x91 ? "+13800138000" : "13800138000",
              "BCD sender and international prefix survive every SCA/OA TON combination");
        const bool librarySafe = scaType <= 0xA1 && oaType <= 0xA1;
        require(pdu.decodeCalls == (librarySafe ? 1 : 0), "unsupported numeric TON never reaches pdulib");
      }
    }
  }
  for (size_t index : {1U, 10U}) {
    fallbackRejects(withByte(valid, index, 0xF1), "reserved TON 7 rejected");
    fallbackRejects(withByte(valid, index, 0xD0), "TON 5 is not interpreted as a numeric address");
  }
  for (size_t length = 0; length < telecom.size(); ++length) {
    fallbackRejects(telecom.substr(0, length), "truncated telecom PDU rejected before pdulib");
  }
  require(isLikelyValidSmsSender("10693533700000000101") && !isLikelyValidSmsSender("+Google") &&
          !isLikelyValidSmsSender("<script>"), "telecom fix preserves the numeric sender gate");
}

static void concatTests() {
  int part = 0, total = 0;
  require(normalizeSmsConcatInfo(0, part, total) && part == 1 && total == 1, "only all-zero ordinary marker normalizes");
  for (auto info : std::vector<std::array<int, 3>>{{{1, 0, 0}}, {{0, 0, 1}}, {{1, 2, 1}}, {{1, -1, 2}},
       {{1, 3, 2}}, {{1, 1, 11}}, {{-1, 1, 2}}, {{65536, 1, 2}}}) {
    int index = info[1], count = info[2];
    require(!normalizeSmsConcatInfo(info[0], index, count), "abnormal primary concat metadata rejected");
    resetState(); pdu.overrideConcat = true; pdu.concat = info;
    receive(gsm("invalid metadata"));
    require(history.empty() && activeSlots() == 0, "invalid primary metadata not delivered or cached");
  }
  resetState();
  receive(gsm("invalid zero header", concat8(0, 0, 0)));
  require(history.empty() && activeSlots() == 0, "actual all-zero UDH is not normalized as ordinary");
  for (bool fallback : {false, true}) {
    for (bool wideRef : {false, true}) {
      for (bool reverse : {false, true}) {
        resetState(); pdu.forceFailure = fallback;
        int ref = wideRef ? 0xBEEF : 0;
        auto header = [&](int index) { return wideRef ? concat16(ref, 2, index) : concat8(ref, 2, index); };
        int first = reverse ? 2 : 1;
        receive(ucs2(first == 1 ? u"first\n" : u"123456", header(first)));
        require(history.empty() && activeSlots() == 1, "first segment cached only");
        receive(ucs2(first == 1 ? u"first\n" : u"123456", header(first)));
        require(concatBuffer[0].receivedParts == 1 && history.empty(), "duplicate cached segment ignored");
        receive(ucs2(first == 1 ? u"123456" : u"first\n", header(3 - first)));
        delivered("first\n123456");
        require(activeSlots() == 0, "completed concat slot cleared");
      }
    }
    resetState(); pdu.forceFailure = fallback;
    receive(gsm("first", concat8(1, 3, 1)));
    receive(gsm("wrong total", concat8(1, 2, 2)));
    require(concatBuffer[0].totalParts == 3 && concatBuffer[0].receivedParts == 1 && history.empty(),
            "changed total rejected without altering existing assembly");
    receive(gsm("third", concat8(1, 3, 3)));
    receive(gsm("second", concat8(1, 3, 2)));
    delivered("firstsecondthird");
    resetState(); pdu.forceFailure = fallback;
    receive(ucs2(u"tail", concat8(2, 2, 2)));
    smsTestMillis += CONCAT_TIMEOUT_MS - 1;
    checkConcatTimeout(); require(history.empty(), "missing segment does not timeout early");
    smsTestMillis++; checkConcatTimeout();
    delivered("[缺失分段1]tail");
    checkConcatTimeout(); require(history.size() == 1 && activeSlots() == 0, "timeout only notifies once");
  }
  resetState();
  smsTestMillis = ULONG_MAX - 100;
  receive(ucs2(u"wrap", concat8(5, 2, 2)));
  smsTestMillis += CONCAT_TIMEOUT_MS;
  checkConcatTimeout(); delivered("[缺失分段1]wrap");
  resetState(); pdu.forceFailure = true;
  for (int index = MAX_CONCAT_PARTS; index >= 1; index--) {
    receive(gsm("x", concat16(0xFFFF, MAX_CONCAT_PARTS, index)));
  }
  delivered(std::string(MAX_CONCAT_PARTS, 'x'));
  resetState();
  require(findOrCreateConcatSlot(1, "10086", MAX_CONCAT_PARTS + 1) == -1, "slot rejects excessive total");
  require(findOrCreateConcatSlot(-1, "10086", 2) == -1, "slot rejects negative reference");
  for (int i = 0; i < MAX_CONCAT_MESSAGES + 1; i++) {
    int slot = findOrCreateConcatSlot(i, "10086", 2);
    require(slot >= 0 && slot < MAX_CONCAT_MESSAGES, "full buffer replacement remains in bounds");
    smsTestMillis++;
  }
  int before = activeSlots();
  clearConcatSlot(-1); clearConcatSlot(MAX_CONCAT_MESSAGES);
  require(activeSlots() == before, "invalid clear slot is harmless");
  equal(assembleConcatSms(-1), "", "invalid assemble slot is harmless");
}

static void cmtLengthTests() {
  for (const auto& input : {gsm("header"), "00" + gsm("header").substr(16),
                            "0B91" + std::string(20, '1') + gsm("header").substr(16)}) {
    const auto length = std::to_string(input.size() / 2 - 1 - std::stoul(input.substr(0, 2), nullptr, 16));
    for (const std::string& fields : {"," + length, "\"\"," + length, length,
                                     " \"alpha, label\" ,\t" + length + " "}) {
      resetState();
      feed("+CMT: " + fields + "\r\n" + input + "\r\n");
      delivered("header");
    }
  }
  const auto input = gsm("declared length");
  const int length = static_cast<int>(input.size() / 2) - 8;
  for (const auto& header : std::vector<std::string>{
       "+CMT:", "+CMT: ,", "+CMT: ,0", "+CMT: ,-1", "+CMT: ,+23", "+CMT: ,0x17",
       "+CMT: ,999999999999999999999999999999", "+CMT: ,23junk", "+CMT: ,23,24",
       "+CMT: \"unterminated,23", "+CMT: \"alpha\"", "+CMT: nonsense,23",
       "+CMT: ," + std::to_string(length - 1), "+CMT: ," + std::to_string(length + 1),
       "+CMT: ," + std::to_string(input.size() / 2)}) {
    resetState();
    feed(header + "\r\n" + input + "\r\n");
    require(effects.empty() && pdu.decodeCalls == 0, "malformed/mismatched CMT does not own a PDU: " + header);
    receive(input); delivered("declared length");
  }
  resetState();
  feed(cmtHeader(input) + "+CMT: ,bogus\r\n" + input + "\r\n");
  require(effects.empty() && !smsWaitingPdu, "malformed replacement CMT invalidates the previous header");
}

static void cmtNumericAndTimeoutTests() {
  require(SMS_CMT_PDU_TIMEOUT_MS == cmtWaitMs, "CMT deadline matches the independent receive contract");
  resetState();
  auto numericPdu = ucs2(u"AB");
  numericPdu.replace(18, 16, "088121436587"); // Even-length numeric OA: no A-F nibbles anywhere.
  require(std::all_of(numericPdu.begin(), numericPdu.end(), [](char c) { return c >= '0' && c <= '9'; }),
          "fixture is both a valid SMS PDU and entirely decimal digits");
  receive(numericPdu); delivered("AB");
  require(pdu.decodeCalls == 1, "PDU ownership is structural, not a blanket ban on numeric lines");
  const auto input = gsm("pending header");
  const std::string numericSameLength = "07" + std::string(input.size() - 2, '1');
  for (bool wrap : {false, true}) {
    resetState(); resetSmsHostClock(wrap ? ULONG_MAX - 100 : 1000);
    feed(cmtHeader(input));
    for (const auto& line : std::vector<std::string>{"460001234567890", "861234567890123", "89860123456789012345",
         "00", std::string(input.size(), '1'), numericSameLength, "AT+CIMI", "+CSQ: 15,99", "OK", "ERROR"}) {
      require(!handleModemUrc(line), "non-PDU response remains owned by AT: " + line);
      require(smsWaitingPdu && effects.empty() && pdu.decodeCalls == 0, "unrelated response preserves valid CMT wait");
    }
    smsTestMillis += cmtWaitMs - 1;
    feed(input + "\r\n"); delivered("pending header");

    resetState(); resetSmsHostClock(wrap ? ULONG_MAX - 100 : 1000);
    feed(cmtHeader(input));
    smsTestMillis += cmtWaitMs - 1;
    require(!handleModemUrc("123456789012345") && smsWaitingPdu, "numeric response does not refresh the wait deadline");
    smsTestMillis++;
    require(!handleModemUrc(input) && !smsWaitingPdu, "PDU at the exact timeout is now orphaned, including wrap");
    require(effects.empty() && pdu.decodeCalls == 0, "expired header never decodes orphan input");
    receive(input); delivered("pending header");

    resetState(); resetSmsHostClock(wrap ? ULONG_MAX - 100 : 1000);
    feed(cmtHeader(input));
    smsTestMillis += cmtWaitMs;
    checkConcatTimeout();
    require(!smsWaitingPdu && effects.empty(), "main-loop timeout housekeeping also expires an idle CMT");
  }
  resetState();
  feed(cmtHeader(input));
  require(!handleModemUrc(input.substr(0, input.size() - 2)) && smsWaitingPdu, "truncated candidate does not cancel a valid pending header");
  require(!handleModemUrc(withByte(input, 18, 0x04)) && smsWaitingPdu, "same-length unsupported DCS is not claimed as a PDU");
  require(pdu.decodeCalls == 0 && effects.empty(), "rejected candidates never call pdulib");
  feed(input + "\r\n"); delivered("pending header");
}

static void languageTests() {
  const std::vector<std::pair<std::vector<uint8_t>, std::string>> gsmBodies = {
    {std::vector<uint8_t>(12, 0x05), "éééééééééééé"},
    {{0x0B, 0x0C, 0x0E, 0x0F, 0x1C, 0x1D, 0x5B, 0x5C, 0x5E, 0x0B, 0x0C, 0x0E}, "ØøÅåÆæÄÖÜØøÅ"},
    {{0x10, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x10, 0x12}, "ΔΦΓΛΩΠΨΣΘΞΔΦ"},
    {{0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x5D, 0x5F, 0x60, 0x7F, 0x01, 0x03, 0x24}, "èéùìòÇÑ§¿à£¥¤"}
  };
  for (const auto& body : gsmBodies) {
    fallbackEquals(gsm(body.first), body.second);
    for (bool fallback : {false, true}) {
      resetState(); pdu.forceFailure = fallback;
      receive(gsm(body.first)); delivered(body.second);
      require(pdu.decodeCalls == 1, "language fixture really exercises pdulib or injected fallback");
      require(fallback || Serial.tx.find("✓ pdulib PDU解析成功") != std::string::npos,
              "real pdulib language output is not silently replaced by the fallback");
      resetState();
      receive(gsm(body.first, concat16(123, 1, 1))); delivered(body.second);
      require(pdu.decodeCalls == 0, "UDH language fixture exercises the bounded fallback");
    }
  }
  for (const auto& body : std::vector<std::pair<std::u16string, std::string>>{
       {u"éééééééééééé", "éééééééééééé"}, {u"ΔΦΓΛΩΠΨΣΘΞΔΦ", "ΔΦΓΛΩΠΨΣΘΞΔΦ"},
       {u"短信 日本語 Русский مرحبا שלום", "短信 日本語 Русский مرحبا שלום"}}) {
    for (bool fallback : {false, true}) {
      resetState(); pdu.forceFailure = fallback;
      receive(ucs2(body.first)); delivered(body.second);
    }
  }
  for (const auto& invalid : std::vector<std::string>{"", "\r\n\t", "\xC3", "\xFF", "\xEF\xBF\xBD"}) {
    require(!isPduLikelyStorableSms("10086", "2026-04-18 12:34:56", invalid, true, true, false),
            "removing frequency heuristic does not remove empty/encoding validation");
  }
  require(!isPduLikelyStorableSms("10086", "2026-04-18 12:34:56", "text", false, true, false) &&
          !isPduLikelyStorableSms("10086", "2026-04-18 12:34:56", "text", true, false, false) &&
          !isPduLikelyStorableSms("?", "2026-04-18 12:34:56", "text", true, true, false) &&
          !isPduLikelyStorableSms("10086", "2026-02-30 12:34:56", "text", true, true, false),
          "message type, user data, sender and calendar gates are retained");
}

static void concatCompletedReplayTests() {
  for (bool fallback : {false, true}) {
    for (bool wide : {false, true}) {
      for (bool reverse : {false, true}) {
        resetState(); pdu.forceFailure = fallback;
        auto header = [&](int part) { return wide ? concat16(0xBEEF, 2, part) : concat8(0, 2, part); };
        const auto head = ucs2(u"head", header(1));
        const auto tail = ucs2(u"tail", header(2), scts(26, 4, 18, 12, 34, 57));
        const String time = reverse ? "2026-04-18 12:34:57" : "2026-04-18 12:34:56";
        receive(reverse ? tail : head); receive(reverse ? head : tail); delivered("headtail", time);
        receive(reverse ? head : tail); // Last received segment is often redelivered alone.
        smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout(); delivered("headtail", time);
        require(activeSlots() == 0, "replay-only candidate times out silently");
        receive(tail); receive(head); delivered("headtail", time);
        smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout(); delivered("headtail", time);
        require(activeSlots() == 0, "whole-group replay leaves no notification timer behind");
      }
    }
  }
}

static void concatReuseTests() {
  for (bool reverse : {false, true}) {
    resetState();
    const auto head = gsm("shared", concat8(7, 2, 1));
    const auto tail = gsm("old", concat8(7, 2, 2));
    const auto changed = gsm("new", concat8(7, 2, 2));
    receive(head); receive(tail);
    receive(reverse ? changed : head); receive(reverse ? head : changed);
    require(history.size() == 2, "reference reuse with one unchanged segment is a new SMS in either order");
    equal(history.back().text, "sharednew", "matching old segment is retained until a changed segment disambiguates reuse");
    receive(tail); receive(head); // Both old and new fingerprints must remain distinguishable.
    receive(changed); receive(head);
    smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout();
    require(history.size() == 2 && httpMessages.size() == 2 && mqttMessages.size() == 2,
            "both generations can be replayed without another notification");
  }
  for (bool differentTime : {false, true}) {
    resetState();
    auto part = [&](const char* body, int index, bool fresh) {
      return gsm(body, concat16(0x4321, 2, index), fresh && differentTime ? scts(26, 4, 18, 12, 35) : scts());
    };
    receive(part("head", 1, false)); receive(part("tail", 2, false));
    receive(part("tail", 2, false)); // A replay candidate cannot block a fresh value for the same index.
    receive(part(differentTime ? "tail" : "new", 2, true)); receive(part("head", 1, true));
    require(history.size() == 2, "same index/new text or new SCTS replaces a replay-only candidate");
    equal(history.back().text, differentTime ? "headtail" : "headnew", "reused reference keeps the new body");
    equal(history.back().timestamp, differentTime ? "2026-04-18 12:35:56" : "2026-04-18 12:34:56", "new SCTS is preserved");
  }
  resetState();
  receive(gsm("A", concat8(1, 2, 1))); receive(gsm("B", concat8(1, 2, 2)));
  receive(withByte(gsm("A", concat8(1, 2, 1)), 11, 0x51));
  receive(withByte(gsm("B", concat8(1, 2, 2)), 11, 0x51));
  receive(gsm("A", concat8(1, 3, 1))); receive(gsm("B", concat8(1, 3, 2))); receive(gsm("C", concat8(1, 3, 3)));
  require(history.size() == 3 && history[0].sender != history[1].sender, "sender and total are part of the replay key");
  equal(history.back().text, "ABC", "new total is not mistaken for old completed group");

  resetState();
  receive(gsm("A", concat8(17, 2, 1))); receive(gsm("X", concat8(17, 2, 2)));
  receive(gsm("B", concat8(17, 2, 1))); receive(gsm("Y", concat8(17, 2, 2)));
  receive(gsm("A", concat8(17, 2, 1))); receive(gsm("Y", concat8(17, 2, 2)));
  require(history.size() == 3, "all received signatures must match one recent group, not separate generations");
  equal(history.back().text, "AY", "individually repeated pieces can form a genuinely new message");

  resetState();
  const auto a = gsm("A", concat8(18, 3, 1)), b = gsm("B", concat8(18, 3, 2)), c = gsm("C", concat8(18, 3, 3));
  receive(a); receive(b); receive(c);
  receive(a); receive(b); // Could be a replay, or the shared pieces of a new group.
  receive(gsm("new", concat8(18, 3, 1))); receive(c);
  require(history.size() == 2, "a conflicting index replaces only that replay candidate, not all shared pieces");
  equal(history.back().text, "newBC", "reference reuse does not lose a shared segment already received");

  resetState();
  const auto oldHead = gsm("head", concat8(19, 2, 1)), oldTail = gsm("tail", concat8(19, 2, 2));
  receive(oldHead); receive(oldTail); receive(oldTail);
  receive(gsm("head", concat8(19, 2, 1), scts(26, 4, 18, 12, 35)));
  require(history.size() == 1, "new SCTS for an as-yet-unseen index cannot combine with a known old replay tail");
  receive(gsm("tail", concat8(19, 2, 2), scts(26, 4, 18, 12, 35)));
  require(history.size() == 2, "new-time message completes only from its own group");
  equal(history.back().timestamp, "2026-04-18 12:35:56", "pending replay cannot supply an old group timestamp");
  smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout();
  require(history.size() == 2 && activeSlots() == 0, "new-time group leaves no extra tail timeout behind");
}

static void concatLateTests() {
  resetState();
  const auto head = gsm("head", concat8(12, 3, 1));
  const auto middle = gsm("middle", concat8(12, 3, 2));
  const auto tail = gsm("tail", concat8(12, 3, 3));
  receive(head); smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout();
  delivered("head[缺失分段2][缺失分段3]");
  receive(head); smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout();
  delivered("head[缺失分段2][缺失分段3]");
  receive(tail); receive(tail);
  require(history.size() == 1, "new late segment waits for the remaining not-yet-forwarded segment");
  receive(middle);
  require(history.size() == 1, "possible late pieces still allow time for a conflicting reused-reference head");
  smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout();
  require(history.size() == 2, "late-only pieces produce a single supplemental notification at the bounded deadline");
  equal(history.back().text, "[已转发分段1]middletail", "supplement never forwards previously emitted body again");
  receive(head); receive(tail); receive(middle);
  smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout();
  require(history.size() == 2 && activeSlots() == 0, "full resend after timeout/supplement remains suppressed");

  resetState();
  receive(head); smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout();
  receive(gsm("new", concat8(12, 3, 1))); receive(middle); receive(tail);
  require(history.size() == 2, "changed content after timeout is a new group, not a late duplicate");
  equal(history.back().text, "newmiddletail", "new group includes shared segments after a partial notification");
}

static void concatLateReuseTests() {
  for (bool differentTime : {false, true}) {
    resetState();
    const auto oldHead = gsm("old head", concat8(21, 2, 1));
    receive(oldHead); smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout();
    const auto time = differentTime ? scts(26, 4, 18, 12, 35) : scts();
    receive(gsm("new tail", concat8(21, 2, 2), time));
    require(history.size() == 1, "unseen tail alone cannot prove that a timed-out reference was not reused");
    receive(gsm("new head", concat8(21, 2, 1), time));
    require(history.size() == 2, "new content or time after a timeout still assembles in reverse order");
    equal(history.back().text, "new headnew tail", "do not replace the head of a new SMS with an old forwarded marker");
  }

  resetState();
  const auto head = gsm("head", concat8(22, 3, 1)), tail = gsm("tail", concat8(22, 3, 3));
  receive(head); smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout();
  receive(tail); smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout();
  require(history.size() == 2, "late partial times out once even with a still-missing middle");
  equal(history.back().text, "[已转发分段1][缺失分段2]tail", "partial supplement repeats no known body");
  receive(tail); receive(head); smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout();
  require(history.size() == 2, "a timeout supplement is remembered as well as the initial timeout");
  receive(gsm("middle", concat8(22, 3, 2)));
  receive(tail); receive(head);
  require(history.size() == 3, "full known replay plus last new piece completes one supplement");
  equal(history.back().text, "[已转发分段1]middle[已转发分段3]", "last unseen piece is the only newly forwarded content");
}

static void concatReplayBoundsTests() {
  static_assert(sizeof(unsigned long) == 4, "SMS expiry tests require a real 32-bit millis type");
  static_assert(sizeof(smsRecentConcat) + sizeof(smsConcatState) <= 2048, "dedup metadata stays within its fixed 2 KiB budget");
  require(SMS_CONCAT_RECENT_MS == recentConcatMs && SMS_CONCAT_RECENT_CAPACITY == recentConcatCapacity,
          "dedup TTL/capacity match the independent behavioral contract");
  for (bool wrap : {false, true}) {
    for (bool replayPending : {false, true}) {
      resetState(); resetSmsHostClock(wrap ? ULONG_MAX - 100 : 1000);
      const auto head = gsm("head", concat8(9, 2, 1)), tail = gsm("tail", concat8(9, 2, 2));
      receive(head); receive(tail);
      smsTestMillis += recentConcatMs - 1;
      receive(tail);
      require(history.size() == 1, "recent duplicate remains suppressed just before TTL");
      smsTestMillis++;
      if (replayPending) {
        smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout();
        require(history.size() == 1, "a duplicate seen before TTL cannot turn into a notification after TTL");
      }
      receive(head); receive(tail);
      require(history.size() == 2, "TTL is bounded, non-sliding and wrap-safe, even with a pending replay");
    }
    resetState(); resetSmsHostClock(wrap ? ULONG_MAX - 2 : 1000);
    for (int ref = 0; ref <= recentConcatCapacity; ref++) {
      receive(gsm("head", concat8(ref, 2, 1))); receive(gsm("tail", concat8(ref, 2, 2)));
      smsTestMillis++;
    }
    require(history.size() == recentConcatCapacity + 1, "all unique groups notify even when recent cache fills");
    receive(gsm("head", concat8(0, 2, 1))); receive(gsm("tail", concat8(0, 2, 2)));
    require(history.size() == recentConcatCapacity + 2, "oldest recent entry evicted at fixed capacity, including wrap");
    receive(gsm("head", concat8(recentConcatCapacity, 2, 1)));
    receive(gsm("tail", concat8(recentConcatCapacity, 2, 2)));
    require(history.size() == recentConcatCapacity + 2, "newest recent entry survives bounded eviction");

    resetState(); resetSmsHostClock(wrap ? ULONG_MAX - 2 : 1000);
    for (int ref = 0; ref <= MAX_CONCAT_MESSAGES; ref++) {
      receive(gsm("head", concat8(ref, 2, 1)));
      smsTestMillis++;
    }
    require(activeSlots() == MAX_CONCAT_MESSAGES, "active assembly count remains bounded");
    for (const auto& slot : concatBuffer) require(slot.refNumber != 0, "oldest active slot, not smallest raw millis, is evicted");

    resetState(); resetSmsHostClock(wrap ? ULONG_MAX - 100 : 1000);
    const auto head = gsm("head", concat8(31, 3, 1)), tail = gsm("tail", concat8(31, 3, 3));
    receive(head); smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout();
    const auto firstForwarded = smsTestMillis;
    receive(tail); smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout();
    require(history.size() == 2, "late partial can update a recent entry across millis wrap");
    smsTestMillis = firstForwarded + recentConcatMs;
    receive(head); receive(gsm("middle", concat8(31, 3, 2))); receive(tail);
    require(history.size() == 3, "late entry updates never slide the original expiry");
    equal(history.back().text, "headmiddletail", "expired partial tombstone no longer changes a fresh group");
  }

  resetState();
  smsRecentConcatId = UINT32_MAX - 1; // Bounded generation token used only for update-vs-eviction checks.
  const auto first = gsm("first", concat8(40, 2, 1)), second = gsm("second", concat8(40, 2, 2));
  receive(first); receive(second);
  receive(gsm("other", concat8(41, 2, 1))); receive(gsm("body", concat8(41, 2, 2)));
  require(smsRecentConcatId == 0, "recent-entry generation token wraps without reserving zero");
  receive(first); receive(second);
  require(history.size() == 2, "generation wrap does not corrupt older recent fingerprints");
}

static void concatMessagesEqual(const std::vector<std::string>& texts) {
  require(history.size() == texts.size() && httpMessages.size() == texts.size() &&
          mqttMessages.size() == texts.size() && emailBodies.size() == texts.size(),
          "OOM/recovery emits only the expected complete history/HTTP/MQTT/email events");
  for (size_t i = 0; i < texts.size(); i++) {
    equal(history[i].text, texts[i], "OOM/recovery history body");
    equal(httpMessages[i].text, texts[i], "OOM/recovery HTTP body");
    equal(mqttMessages[i].text, texts[i], "OOM/recovery MQTT body");
    equal(history[i].timestamp, "2026-04-18 12:34:56", "OOM/recovery timestamp");
    equal(httpMessages[i].timestamp, "2026-04-18 12:34:56", "OOM/recovery HTTP timestamp");
    equal(mqttMessages[i].timestamp, "2026-04-18 12:34:56", "OOM/recovery MQTT timestamp");
    equal(emailBodies[i], "发送者: " + history[i].sender + "\n时间: " + history[i].timestamp + "\n内容:\n" + texts[i],
          "OOM/recovery complete email body");
  }
  require(Serial1.tx.empty(), "OOM handling never sends AT commands or retries a submission");
}

static void stringFaultConsumed() {
  require(smsTestStringFailures == 1 && !smsTestStringFault, "exactly one targeted String operation failed");
}

static void failConcatAssemblyOnce(SmsStringOp operation, const std::string& prefix, const std::string& chunk) {
  smsTestStringFault = [=](SmsStringOp op, const String* target, const char* text, size_t length) {
    if (op != operation) return false;
    // SCTS decoding reserves 14 bytes; only the later assembly reserves this much.
    if (op == SmsStringOp::Reserve) return length > 14;
    return std::string(target->c_str()) == prefix && length >= chunk.size() &&
           std::memcmp(text, chunk.data(), chunk.size()) == 0;
  };
}

static void concatCacheOomTests() {
  for (bool fallback : {false, true}) {
    for (bool firstPart : {false, true}) {
      resetState(); pdu.forceFailure = fallback;
      const auto head = ucs2(u"cached head 正文", concat16(0x5151, 2, 1));
      const auto tail = ucs2(u"retried tail 正文", concat16(0x5151, 2, 2));
      const int index = firstPart ? 0 : 1;
      if (!firstPart) receive(head);
      smsTestStringFault = [=](SmsStringOp op, const String* target, const char*, size_t) {
        return op == SmsStringOp::Copy && target == &concatBuffer[0].parts[index].text;
      };
      receive(firstPart ? head : tail);
      stringFaultConsumed();
      require(!concatBuffer[0].parts[index].valid && concatBuffer[0].receivedParts == (firstPart ? 0 : 1),
              "failed part copy cannot become valid or increase receivedParts");
      require(smsConcatState[0].signatures[index] == 0 && smsConcatState[0].timestamps[index] == 0 &&
              smsRecentConcatId == 0, "failed part copy commits neither signature nor forwarded record");
      concatMessagesEqual({});
      require(Serial.tx.find("[sms] ERROR: concat_") != std::string::npos, "cache OOM is diagnosable");
      if (firstPart) {
        smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout();
        concatMessagesEqual({});
        require(activeSlots() == 0, "failed first part cannot leave a placeholder-only timeout notification");
        receive(tail); // Recovery may arrive out of order.
        receive(head);
      } else {
        equal(concatBuffer[0].parts[0].text, "cached head 正文", "another accepted part survives cache OOM");
        receive(tail);
      }
      delivered("cached head 正文retried tail 正文");
      receive(tail); receive(head);
      smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout();
      delivered("cached head 正文retried tail 正文");
      require(activeSlots() == 0, "successful retry is deduplicated normally");
    }
  }
}

static void concatLateCacheOomTests() {
  resetState();
  const auto head = gsm("head", concat8(52, 2, 1)), tail = gsm("late tail", concat8(52, 2, 2));
  receive(head); smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout();
  receive(head); // A cached known replay must not hide an unsuccessfully copied late piece.
  const auto id = smsRecentConcatId;
  const auto completedAt = smsRecentConcat[0].completedAt;
  smsTestStringFault = [](SmsStringOp op, const String* target, const char*, size_t) {
    return op == SmsStringOp::Copy && target == &concatBuffer[0].parts[1].text;
  };
  receive(tail); stringFaultConsumed();
  require(concatBuffer[0].receivedParts == 1 && !concatBuffer[0].parts[1].valid &&
          smsConcatState[0].signatures[1] == 0 && smsConcatState[0].timestamps[1] == 0,
          "late copy failure does not poison active signatures/counts");
  require(smsRecentConcatId == id && smsRecentConcat[0].forwardedMask == 1 &&
          smsRecentConcat[0].completedAt == completedAt, "late copy failure does not register or extend a recent entry");
  concatMessagesEqual({"head[缺失分段2]"});
  receive(tail);
  concatMessagesEqual({"head[缺失分段2]", "[已转发分段1]late tail"});
  receive(tail); receive(head);
  smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout();
  concatMessagesEqual({"head[缺失分段2]", "[已转发分段1]late tail"});
}

static void concatMetadataOomTests() {
  for (bool snapshot : {false, true}) {
    for (bool senderSnapshot : {false, true}) {
      if (!snapshot && senderSnapshot) continue;
      resetState();
      const auto head = gsm("head", concat8(53, 2, 1)), tail = gsm("tail", concat8(53, 2, 2));
      if (snapshot) receive(head);
      smsTestStringFault = [=](SmsStringOp op, const String* target, const char* text, size_t length) {
        if (op != SmsStringOp::Copy) return false;
        if (!snapshot) return target == &concatBuffer[0].timestamp;
        const String& field = senderSnapshot ? concatBuffer[0].sender : concatBuffer[0].timestamp;
        return concatBuffer[0].receivedParts == 2 && target != &field && length == field.length() &&
               std::memcmp(text, field.c_str(), length) == 0;
      };
      receive(snapshot ? tail : head); stringFaultConsumed();
      concatMessagesEqual({});
      require(smsRecentConcatId == 0, "failed metadata copy cannot mark the message forwarded");
      if (snapshot) {
        require(concatBuffer[0].inUse && concatBuffer[0].receivedParts == 2,
                "failed notification snapshot retains both parts for retry");
      } else {
        require(!concatBuffer[0].parts[0].valid && concatBuffer[0].receivedParts == 0 &&
                smsConcatState[0].signatures[0] == 0, "first timestamp and part are committed together");
        receive(head);
      }
      receive(tail); delivered("headtail");
      receive(head); receive(tail); delivered("headtail");
    }
  }
}

static void concatAssemblyOomTests(bool timeout, bool late) {
  for (SmsStringOp operation : {SmsStringOp::Reserve, SmsStringOp::Concat}) {
    for (bool retryWithPdu : {false, true}) {
      resetState();
      const std::string headText(32, 'H'), tailText(32, 'T');
      const auto head = gsm(headText, concat8(54, 2, 1)), tail = gsm(tailText, concat8(54, 2, 2));
      const std::string partial = headText + "[缺失分段2]", supplement = "[已转发分段1]" + tailText;
      std::vector<std::string> expected;
      receive(head);
      if (late) {
        smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout(); expected.push_back(partial);
        receive(tail);
      }
      const auto id = smsRecentConcatId;
      const auto completedAt = smsRecentConcat[0].completedAt;
      const auto logStart = Serial.tx.size();
      failConcatAssemblyOnce(operation, late ? "[已转发分段1]" : headText, timeout ? "[缺失分段" : tailText);
      if (timeout || late) { smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout(); }
      else receive(tail);
      stringFaultConsumed();
      concatMessagesEqual(expected);
      require(concatBuffer[0].inUse && concatBuffer[0].receivedParts == (timeout || late ? 1 : 2),
              "assembly OOM retains accepted parts for recovery");
      require(smsRecentConcatId == id && smsRecentConcat[0].forwardedMask == (late ? 1 : 0) &&
              smsRecentConcat[0].completedAt == completedAt, "failed assembly cannot add forwarded bits or slide recent TTL");
      require(Serial.tx.find("[sms] ERROR: concat_", logStart) != std::string::npos, "assembly OOM is diagnosable");
      if (retryWithPdu) receive(late ? head : tail); // Duplicate of a full group must also retry assembly.
      else {
        checkConcatTimeout(); concatMessagesEqual(expected); // Avoid an OOM busy loop.
        smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout();
      }
      expected.push_back(late ? supplement : timeout && !retryWithPdu ? partial : headText + tailText);
      concatMessagesEqual(expected);
      require(activeSlots() == 0, "successful assembly retry releases its slot");
      if (timeout && !retryWithPdu) {
        receive(head); smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout(); concatMessagesEqual(expected);
        receive(tail); receive(head); expected.push_back(supplement); concatMessagesEqual(expected);
      }
      receive(tail); receive(head);
      smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout();
      concatMessagesEqual(expected);
      require(activeSlots() == 0, "post-recovery full/tail replay remains suppressed");
    }
  }
}

static void concatAssemblyResultOomTests() {
  for (SmsStringOp operation : {SmsStringOp::Reserve, SmsStringOp::Concat}) {
    resetState();
    receive(gsm("head", concat8(55, 10, 1)));
    std::string prefix = "head";
    for (int part = 2; part < 10; part++) prefix += "[缺失分段" + std::to_string(part) + "]";
    failConcatAssemblyOnce(operation, prefix, "[缺失分段");
    const String failed = assembleConcatSms(0); stringFaultConsumed();
    equal(failed, "", "assembly API never returns an incomplete body, including its final two-digit placeholder");
    concatMessagesEqual({});
    require(concatBuffer[0].receivedParts == 1 && smsRecentConcatId == 0, "assembly API has no commit side effects");
    equal(assembleConcatSms(0), prefix + "[缺失分段10]", "assembly API recovers with exact UTF-8 byte lengths");
  }
}

static void concatCommittedSnapshotTests(int failCopy, bool timestampSnapshot = false) {
  for (int mode : {0, 1, 2}) { // Complete, missing-part timeout, late supplement.
    resetState();
    const std::string headText(32, 'H'), tailText(32, 'T');
    const auto head = gsm(headText, concat8(56, 2, 1)), tail = gsm(tailText, concat8(56, 2, 2));
    const std::string partial = headText + "[缺失分段2]", supplement = "[已转发分段1]" + tailText;
    std::vector<std::string> expected;
    receive(head);
    if (mode == 2) {
      smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout(); expected.push_back(partial);
      receive(tail);
    }
    const std::string body = mode == 2 ? supplement : mode == 1 ? partial : headText + tailText;
    const std::string snapshot = timestampSnapshot ? "2026-04-18 12:34:56" : body;
    const uint16_t committedMask = mode == 1 ? 1 : 3;
    int copiesAfterCommit = 0;
    bool enteredHistory = false;
    // Target String(text)/cleanSmsContent or String(timestamp)/formatTimestamp copies in the old entry.
    // Decode, assembly, metadata snapshots and sink allocations are deliberately not faulted.
    smsTestStringFault = [&](SmsStringOp op, const String*, const char* data, size_t length) {
      if (op != SmsStringOp::Copy || concatBuffer[0].inUse || !smsRecentConcat[0].inUse ||
          smsRecentConcat[0].forwardedMask != committedMask || length != snapshot.size() ||
          std::memcmp(data, snapshot.data(), length) != 0) return false;
      return ++copiesAfterCommit == failCopy;
    };
    beforeHistory = [&] {
      enteredHistory = true;
      smsTestStringFault = nullptr;
      require(!concatBuffer[0].inUse && smsRecentConcat[0].forwardedMask == committedMask,
              "dedup is committed and slot freed before a reentrant history callback");
      checkConcatTimeout(); // Cannot emit the same slot a second time.
    };
    if (mode == 0) receive(tail);
    else { smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout(); }
    beforeHistory = nullptr;
    require(enteredHistory, "prepared snapshot reaches history");
    require(copiesAfterCommit == 0 && smsTestStringFailures == 0,
            std::string("borrow the prepared ") + (timestampSnapshot ? "time" : "body") +
            " after commit; legacy copy probe " + std::to_string(failCopy) +
            " observed " + std::to_string(copiesAfterCommit) + " copies / " + std::to_string(smsTestStringFailures) + " failures");
    resetSmsStringFault();
    expected.push_back(body); concatMessagesEqual(expected);
    require(activeSlots() == 0, "borrowed snapshot lives independently of the freed slot");
    receive(head);
    if (mode != 1) receive(tail);
    smsTestMillis += CONCAT_TIMEOUT_MS; checkConcatTimeout(); concatMessagesEqual(expected);
    if (mode == 1) {
      receive(tail); receive(head); expected.push_back(supplement); concatMessagesEqual(expected);
    }
    receive(tail); receive(head); concatMessagesEqual(expected);
  }
}

int main() {
  const std::vector<std::pair<const char*, std::function<void()>>> suites = {
    {"timestamp normalization, negative zones and calendar validation", timestampTests},
    {"real pdulib and forced fallback receive paths", standaloneTests},
    {"lossless OTP/URL/CRLF bodies and protocol tails", bodyTests},
    {"GSM7 packed length, extensions, all UDH alignments and limits", gsmFallbackTests},
    {"truncated PDU, UDH/IE boundaries and malformed metadata", malformedTests},
    {"receive preflight preserves decoder and in-flight concat state", receivePreflightTests},
    {"real receive GSM7 UDH, padding, extensions and multipart bodies", gsmReceiveBoundaryTests},
    {"pre-pdulib DCS, SCA/OA and SMS-DELIVER boundaries", dcsAndAddressBoundaryTests},
    {"ordered/out-of-order/duplicate/timeout/capacity concat", concatTests},
    {"CMT optional alpha, exact TPDU/SMSC lengths and invalid headers", cmtLengthTests},
    {"CMT numeric AT ownership, absolute deadline and wrap recovery", cmtNumericAndTimeoutTests},
    {"legitimate GSM7/UCS2 languages through real pdulib and fallback", languageTests},
    {"completed concat tail and whole-group replay", concatCompletedReplayTests},
    {"concat reference reuse with shared bodies, sender, total and SCTS", concatReuseTests},
    {"timeout duplicates and one-time late-segment supplements", concatLateTests},
    {"timed-out reference reuse and successive partial supplements", concatLateReuseTests},
    {"recent concat fixed capacity, non-sliding TTL and 32-bit wrap", concatReplayBoundsTests},
    {"first/last segment copy OOM, atomic cache state and retransmission", concatCacheOomTests},
    {"late segment copy OOM cannot poison the recent forwarded mask", concatLateCacheOomTests},
    {"concat timestamp/cache and notification snapshot OOM recovery", concatMetadataOomTests},
    {"complete concat reserve/append OOM, duplicate retry and timed recovery", [] { concatAssemblyOomTests(false, false); }},
    {"timeout concat reserve/placeholder OOM and late retransmission", [] { concatAssemblyOomTests(true, false); }},
    {"late supplement reserve/append OOM preserves prior forwarding", [] { concatAssemblyOomTests(false, true); }},
    {"assembly API failure is empty, not a truncated UTF-8/part-10 marker", concatAssemblyResultOomTests},
    {"post-commit String(text) copy fault cannot lose a completed snapshot", [] { concatCommittedSnapshotTests(1); }},
    {"post-commit cleaner copy fault cannot lose a completed snapshot", [] { concatCommittedSnapshotTests(2); }},
    {"post-commit String(timestamp) copy fault cannot lose prepared time", [] { concatCommittedSnapshotTests(1, true); }},
    {"post-commit formatter copy fault cannot lose prepared time", [] { concatCommittedSnapshotTests(2, true); }}
  };
  int failures = 0;
  for (const auto& suite : suites) {
    try { resetState(); suite.second(); std::cout << "PASS: " << suite.first << '\n'; }
    catch (const std::exception& error) { failures++; std::cerr << "FAIL: " << suite.first << ": " << error.what() << '\n'; }
  }
  std::cout << checks << " assertions, " << suites.size() << " suites, " << failures << " failures.\n";
  return failures ? 1 : 0;
}
