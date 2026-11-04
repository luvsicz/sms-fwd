// Actual modem + SMS + call sources, real installed pdulib; only platform/I/O/sinks are stubs.
// Synthetic RX events, no device, SIM, network, filesystem or Arduino build.
#define String ModemHostString
#include "sms_host_stubs.h"
#undef String
class String : public ModemHostString {
public:
  using ModemHostString::ModemHostString;
  String(const ModemHostString& text) : ModemHostString(text) {}
  bool endsWith(const String& suffix) const {
    return length() >= suffix.length() && substring(length() - suffix.length()) == suffix;
  }
};
#define PDU SmsRealPdu
#include <pdulib.h>
#undef PDU
#include <climits>
#include <iostream>
#include <memory>

// Forwarding wrapper counts calls without replacing encode/decode/buffer behavior.
class PDU {
  SmsRealPdu real{4096};
public:
  int encodeCalls = 0, decodeCalls = 0;
  void setSCAnumber() { real.setSCAnumber(); }
  int encodePDU(const char* phone, const char* message) { encodeCalls++; return real.encodePDU(phone, message); }
  bool decodePDU(const char* text) { decodeCalls++; return real.decodePDU(text); }
  const char* getSMS() { return real.getSMS(); }
  const char* getSender() { return real.getSender(); }
  const char* getTimeStamp() { return real.getTimeStamp(); }
  const char* getText() { return real.getText(); }
  int* getConcatInfo() { return real.getConcatInfo(); }
};

#include "sms_handler.h"
#include "modem_handler.h"
void sendSMSToServer(const char*, const char*, const char*);
void publishMqttSmsReceived(const char*, const char*, const char*);
void publishMqttCallReceived(const char*, const char*);
void sendEmailNotification(const char*, const char*);
#include "call_handler.h"
#define MQTT_HANDLER_H // Network publishers are sink stubs; no WiFi/PubSubClient dependency.
#define handleCallUrc handleRealCallUrc
#include "call_handler.ino"
#undef handleCallUrc
#include "sms_handler.ino"
#include "modem_handler.ino"

HardwareSerial Serial, Serial1;
unsigned long smsTestMillis = 1000;
Config config{};
String lastCallNumber;
unsigned long lastCallNotifyTime = 0;
PDU pdu;
ConcatSms concatBuffer[MAX_CONCAT_MESSAGES];
struct Message { String sender, text, timestamp; };
std::vector<Message> history, httpMessages, mqttMessages;
std::vector<String> emailBodies, callLines;
std::vector<Message> callHttpMessages;
std::vector<String> callHistory, mqttCallers, callEmailBodies;
static bool handlingCall = false;
std::function<void()> onHistory, onCall;
void addSmsToHistory(const char* sender, const char* text, const char* timestamp) {
  history.push_back({sender, text, timestamp});
  if (onHistory) onHistory();
}
void addCallToHistory(const char* caller, const char*) { callHistory.emplace_back(caller); }
String getCurrentTimeString() { return "2026-04-18 12:34:56"; }
bool isNumberFiltered(const char*) { return false; }
bool isContentFiltered(const char*) { return false; }
void sendSMSToServer(const char* sender, const char* text, const char* timestamp) {
  (handlingCall ? callHttpMessages : httpMessages).push_back({sender, text, timestamp});
}
void publishMqttSmsReceived(const char* sender, const char* text, const char* timestamp) {
  mqttMessages.push_back({sender, text, timestamp});
}
void publishMqttCallReceived(const char* caller, const char*) { mqttCallers.emplace_back(caller); }
void sendEmailNotification(const char*, const char* body) {
  (handlingCall ? callEmailBodies : emailBodies).emplace_back(body);
}
bool handleCallUrc(const String& line) {
  // Observe dispatch while executing real parsing/dedup, with separate call/SMS sinks.
  handlingCall = true;
  const bool handled = handleRealCallUrc(line);
  handlingCall = false;
  if (handled) {
    callLines.push_back(line);
    if (onCall) onCall();
  }
  return handled;
}

static int checks = 0, failedChecks = 0, failures = 0;
static void require(bool condition, const std::string& label) {
  checks++;
  if (!condition) { failedChecks++; throw std::runtime_error(label); }
}
static void equal(const String& actual, const String& expected, const std::string& label) {
  require(actual == expected, label + " [actual=" + actual.c_str() + ", expected=" + expected.c_str() + "]");
}
static size_t sent(uint8_t byte) {
  return static_cast<size_t>(std::count(Serial1.tx.begin(), Serial1.tx.end(), static_cast<char>(byte)));
}
static void resetState() {
  // Reset only via public firmware APIs + a line boundary, never poke private modem state.
  if (modemBusy()) throw std::runtime_error("previous transaction leaked ownership");
  setModemIdleHook(nullptr);
  onHistory = onCall = nullptr;
  Serial1.reset();
  resetModemUrc();
  Serial1.feed("\n");
  pollModem();
  Serial1.reset(); Serial.reset();
  resetModemUrc();
  initConcatBuffer();
  for (int slot = 0; slot < MAX_CONCAT_MESSAGES; slot++) clearConcatSlot(slot);
  history.clear(); httpMessages.clear(); mqttMessages.clear(); emailBodies.clear(); callLines.clear();
  callHttpMessages.clear(); callHistory.clear(); mqttCallers.clear(); callEmailBodies.clear();
  handlingCall = false; config = Config{}; lastCallNumber = ""; lastCallNotifyTime = 0;
  pdu.encodeCalls = pdu.decodeCalls = 0;
  resetSmsHostClock();
}
static void pump(const std::string& data) { Serial1.feed(data); checkSerial1URC(); }
static std::string octet(unsigned int value) {
  const char* digits = "0123456789ABCDEF";
  return std::string{digits[(value >> 4) & 15], digits[value & 15]};
}
static std::string incoming(const std::string& ascii) {
  // Independent UCS2 SMS-DELIVER fixture; synthetic sender, fixed valid UTC+8 SCTS.
  std::string hex = "0791683108200505040B813108108300F0000862408121436523" + octet(ascii.size() * 2);
  for (unsigned char c : ascii) hex += "00" + octet(c);
  return hex;
}
static std::string cmtHeader(const std::string& pduHex) {
  return "+CMT: ," + std::to_string(pduHex.size() / 2 - 1 - std::stoul(pduHex.substr(0, 2), nullptr, 16)) + "\r\n";
}
static std::string cmt(const std::string& text) {
  const auto input = incoming(text);
  return cmtHeader(input) + input + "\r\n";
}
static const std::string clip = "+CLIP: \"15500000000\",129,,,,0";
static const std::string withheldClip = "+CLIP: \"\",129,,,,1";
static const std::string clccIncoming = "+CLCC: 1,1,4,0,0,\"15500000001\",129";
static const std::string clccWaiting = "+CLCC: 2,1,5,0,0,\"15500000002\",129";
static const std::string clccUnknown = "+CLCC: 3,1,4,0,0,\"\",129";
static const String unknownCaller = "\u672a\u77e5\u53f7\u7801";
static void notifiedCallers(const std::vector<String>& callers) {
  require(callHistory == callers && mqttCallers == callers, "only genuine calls reach history and MQTT");
  require(callHttpMessages.size() == callers.size() && callEmailBodies.size() == callers.size(),
          "each genuine call reaches HTTP and email exactly once");
  for (size_t i = 0; i < callers.size(); i++) {
    equal(callHttpMessages.at(i).sender, callers.at(i), "call HTTP caller");
    require(callEmailBodies.at(i).indexOf(callers.at(i)) >= 0, "call email contains the caller");
  }
}
static void delivered(const std::vector<std::string>& texts) {
  require(history.size() == texts.size(), "each RX SMS has exactly one history event");
  require(httpMessages.size() == texts.size() && mqttMessages.size() == texts.size() && emailBodies.size() == texts.size(),
          "each RX SMS has exactly one event at each notification sink");
  for (size_t i = 0; i < texts.size(); i++) {
    equal(history.at(i).text, texts[i], "history body");
    equal(httpMessages.at(i).text, texts[i], "HTTP body");
    equal(mqttMessages.at(i).text, texts[i], "MQTT body");
    equal(history.at(i).timestamp, "2026-04-18 12:34:56", "normalized receive timestamp");
  }
}

static void querySmoke() {
  resetState();
  pump("\r\n");
  Serial1.onWrite = [](const std::string& text) {
    if (text == "AT\r\n") Serial1.schedule(3, "AT\r\nOK\r\n");
  };
  String response;
  require(runModemCommand("AT", 20, &response) == ModemResult::Ok, "complete OK query succeeds");
  equal(response, "\r\nOK\r\n", "command echo excluded");
  require(Serial1.tx == "AT\r\n" && !modemBusy(), "one command and ownership released");
  require(smsTestTicks == 3, "scheduled RX advances only through firmware delay");
}

static void callQueryResponses() {
  const std::string rows = clccIncoming + "\r\n" + clccWaiting + "\r\n" + clccUnknown;
  for (bool explicitPrefix : {false, true}) {
    for (const auto& query : std::vector<std::pair<std::string, std::string>>{
         {"AT+CLIP?", "+CLIP: 1,1"}, {"at+clip?", "+CLIP:\t0,2"}, {"AT+CLIP=?", "+CLIP: (0,1)"},
         {"AT+CLCC", rows}, {"at+clcc", rows}, {" \tAt+ClCc \t", rows}}) {
      resetState();
      Serial1.onWrite = [&](const std::string& text) {
        if (text == query.first + "\r\n") Serial1.schedule(1, query.second + "\r\nOK\r\n");
      };
      const bool clipQuery = query.second.rfind("+CLIP:", 0) == 0;
      String response, resultLine;
      require(runModemCommand(query.first.c_str(), 5, &response, &resultLine,
                              explicitPrefix ? (clipQuery ? "+CLIP:" : "+CLCC:") : nullptr) == ModemResult::Ok,
              "call query succeeds with or without an explicit result prefix");
      equal(response, "\r\n" + query.second + "\r\nOK\r\n", "all call query rows remain in the AT response");
      equal(resultLine, explicitPrefix ? (clipQuery ? query.second : clccUnknown) : "", "requested call query result retained");
      require(callLines.empty() && lastCallNumber.length() == 0 && lastCallNotifyTime == 0,
              "query replies never enter call dispatch or change call dedup state");
      notifiedCallers({});
      require(Serial1.tx == query.first + "\r\n" && !modemBusy(), "query routing does not alter the command or ownership");
    }
  }
  resetState();
  Serial1.onWrite = [](const std::string&) { Serial1.schedule(1, "OK\r\n"); };
  String response;
  require(runModemCommand("AT+CLCC", 5, &response) == ModemResult::Ok, "empty CLCC list still succeeds on OK alone");
  equal(response, "\r\nOK\r\n", "empty CLCC query does not require a row");
  notifiedCallers({});
}

static void callQueryInterleaving() {
  for (bool clccQuery : {false, true}) {
    resetState();
    ModemTransaction tx;
    require(tx.begin(clccQuery ? "AT+CLCC" : "AT+CLIP?"), "begin call query with asynchronous traffic");
    const auto input = incoming("call query gap");
    const std::string rows = clccQuery ? clccIncoming + "\r\n" + clccUnknown : "+CLIP: 1,1";
    pump(cmtHeader(input) + "RING\r\n" + rows + "\r\n" + clip + "\r\n" + withheldClip + "\r\n" +
         (clccQuery ? "" : clccIncoming + "\r\n") + input + "\r\nNO CARRIER\r\n");
    require(tx.result() == ModemResult::Pending, "call query rows do not replace final OK");
    equal(tx.response(), "\r\n" + rows + "\r\n", "only solicited rows enter the pending query response");
    std::vector<String> expectedLines{"RING", clip, withheldClip};
    std::vector<String> callers{"15500000000", unknownCaller};
    if (!clccQuery) { expectedLines.push_back(clccIncoming); callers.emplace_back("15500000001"); }
    require(callLines == expectedLines, "quoted CLIP and unrelated CLCC remain asynchronous during queries");
    notifiedCallers(callers);
    delivered({"call query gap"});
    pump("OK\r\n" + clccWaiting + "\r\n");
    require(tx.waitForFinal(0) == ModemResult::Ok, "query completes despite interleaved calls and SMS");
    equal(tx.response(), "\r\n" + rows + "\r\nOK\r\n", "post-OK CLCC is not another query row");
    expectedLines.push_back(clccWaiting); callers.emplace_back("15500000002");
    require(callLines == expectedLines, "CLCC after OK in the same poll is unsolicited even before end");
    notifiedCallers(callers);
  }
}

static void callUrcRouting() {
  resetState();
  for (const char* reply : {"+CLIP: 1,1", "+CLIP:\t0,2", "+CLIP: (0,1)", "+CLIP: \"unterminated"}) {
    require(!handleCallUrc(reply), "status/test/malformed CLIP is not a caller-number URC");
    pump(std::string(reply) + "\r\n");
  }
  require(callLines.empty(), "orphan CLIP query replies cannot notify an unknown caller");
  notifiedCallers({});
  pump(clip + "\r\n" + clip + "\r\n" + clccIncoming + "\r\n" + withheldClip + "\r\n" +
       withheldClip + "\r\n" + clccUnknown + "\r\nRING\r\nNO CARRIER\r\n");
  notifiedCallers({"15500000000", "15500000001", unknownCaller});

  for (const char* command : {"AT+CSQ", "AT+CLIP=1", "AT+CLCC=1", "AT+CLCC=?", "AT+CLCCX"}) {
    resetState();
    ModemTransaction tx;
    require(tx.begin(command), "begin non-call-list command");
    pump(clccIncoming + "\r\nRING\r\n" + withheldClip + "\r\n" + clip + "\r\n" + clccWaiting + "\r\nOK\r\n");
    require(tx.waitForFinal(0) == ModemResult::Ok, "unrelated commands still complete alongside call URCs");
    equal(tx.response(), "\r\nOK\r\n", "a related command name alone cannot claim asynchronous CLCC");
    require(callLines == std::vector<String>{clccIncoming, "RING", withheldClip, clip, clccWaiting},
            "all genuine call lines reach the real handler");
    notifiedCallers({"15500000001", unknownCaller, "15500000000", "15500000002"});
  }
  resetState();
  {
    ModemTransaction tx;
    require(tx.begin("AT+CSQ", "+CLCC:"), "a required prefix does not make CSQ a CLCC query");
    pump(clccIncoming + "\r\nOK\r\n");
    require(tx.waitForFinal(1) == ModemResult::Timeout, "unsolicited CLCC cannot satisfy an unrelated required prefix");
    equal(tx.resultLine(), "", "unrelated prefix receives no call URC");
    notifiedCallers({"15500000001"});
  }
  resetState();
  ModemTransaction tx;
  require(tx.begin("AT+CMGS=1", "+CMGS:", true), "begin SMS with real call URCs");
  pump(clccIncoming + "\r\n" + withheldClip + "\r\n> ");
  require(tx.waitForPrompt(0) && tx.submitPdu("00"), "CLCC and withheld CLIP do not steal SMS prompt");
  pump(clccWaiting + "\r\n" + clip + "\r\n+CMGS: 42\r\nOK\r\n");
  require(tx.waitForFinal(0) == ModemResult::Ok, "real calls do not consume SMS receipt/final");
  equal(tx.response(), "\r\n+CMGS: 42\r\nOK\r\n", "call URCs stay out of SMS response");
  notifiedCallers({"15500000001", unknownCaller, "15500000002", "15500000000"});
}

static void clccQueryBoundaries() {
  const std::string wire = clccIncoming + "\r\n";
  for (bool readInLoop : {false, true}) {
    for (size_t cut = 1; cut <= wire.size(); cut++) {
      resetState();
      Serial1.feed(wire.substr(0, cut));
      if (readInLoop) checkSerial1URC();
      ModemTransaction tx;
      require(tx.begin("AT+CLCC"), "begin CLCC after queued or partial unsolicited row");
      pump(wire.substr(cut) + clccWaiting + "\r\nOK\r\n");
      require(tx.waitForFinal(0) == ModemResult::Ok, "CLCC succeeds across old-generation call line");
      equal(tx.response(), "\r\n" + clccWaiting + "\r\nOK\r\n", "only current-generation CLCC is solicited");
      require(callLines == std::vector<String>{clccIncoming}, "pre-command CLCC is delivered exactly once");
      notifiedCallers({"15500000001"});
    }
  }
  for (const std::string final : {"OK", "ERROR", ""}) {
    resetState();
    ModemTransaction tx;
    require(tx.begin("AT+CLCC"), "begin CLCC before completion/error/timeout");
    pump(clccIncoming + "\r\n");
    notifiedCallers({});
    if (final.empty()) require(tx.waitForFinal(0) == ModemResult::Timeout, "CLCC timeout ends reply ownership");
    pump((final.empty() ? "" : final + "\r\n") + clccWaiting + "\r\n");
    const auto expected = final.empty() ? ModemResult::Timeout : final == "OK" ? ModemResult::Ok : ModemResult::Error;
    require(tx.result() == expected, "post-terminal call does not change the command result");
    equal(tx.response(), "\r\n" + clccIncoming + "\r\n" + (final.empty() ? "" : final + "\r\n"),
          "CLCC after a terminal or timeout is excluded from the response");
    notifiedCallers({"15500000002"});
    Serial1.schedule(1, clccUnknown + "\r\n");
    tx.end();
    modemDelay(1);
    checkSerial1URC();
    require(callLines == std::vector<String>{clccWaiting, clccUnknown}, "recovery and idle preserve unsolicited CLCC");
    notifiedCallers({"15500000002", unknownCaller});
  }
}

static void snapshotSmoke() {
  resetState();
  SmsRealPdu expected{4096};
  expected.setSCAnumber();
  int length = expected.encodePDU("15500000000", "outgoing snapshot");
  std::string encoded = expected.getSMS();
  require(length > 0 && !encoded.empty() && encoded.back() == '\x1A', "installed pdulib appends Ctrl+Z");
  const std::string command = "AT+CMGS=" + std::to_string(length) + "\r\n";
  // begin() must dispatch this after sendSMS has already taken its encoded snapshot.
  Serial1.feed(cmt("queued before begin"));
  bool overwritten = false;
  Serial1.onWrite = [&](const std::string& text) {
    if (text == command) {
      overwritten = std::string(pdu.getSMS()) != encoded;
      const auto input = incoming("waiting prompt");
      Serial1.schedule(2, cmtHeader(input) + clip + "\r\nRING\r\nNO CARRIER\r\n" + input + "\r\n");
      Serial1.schedule(5, "> "); // No LF before submission.
    } else if (text == std::string(1, '\x1A')) {
      Serial1.schedule(2, cmt("after submit") + clip + "\r\nRING\r\nNO CARRIER\r\n+CMGS: 42\r\nOK\r\n");
    }
  };
  require(sendSMS("15500000000", "outgoing snapshot"), "real sendSMS succeeds with interleaved RX");
  require(overwritten, "real receive decode overwrote global pdulib work buffer");
  require(Serial1.tx == command + encoded, "private snapshot transmitted byte-for-byte, no extra terminator");
  require(sent(0x1A) == 1 && sent(0x1B) == 0, "exactly one Ctrl+Z, no ESC on success");
  require(pdu.encodeCalls == 1 && pdu.decodeCalls == 3, "actual encode/decode paths executed");
  require(callLines == std::vector<String>{clip, "RING", clip, "RING"}, "CLIP and RING dispatched during both waits");
  delivered({"queued before begin", "waiting prompt", "after submit"});
  require(!modemBusy() && Serial1.pendingEvents() == 0 && !Serial1.available(), "successful send fully drained and unlocked");
}

static void rxBoundaries() {
  const std::string wire = cmt("boundary");
  // Every byte split: complete/partial CMT header, header/PDU gap, partial PDU and CR/LF.
  for (bool readInLoop : {false, true}) {
    for (size_t cut = 0; cut <= wire.size(); cut++) {
      resetState();
      Serial1.feed(wire.substr(0, cut));
      if (readInLoop) checkSerial1URC();
      ModemTransaction tx;
      require(tx.begin("AT+CSQ"), "begin after existing complete/partial RX");
      Serial1.feed(wire.substr(cut) + "+CSQ: 10,99\r\nOK\r\n");
      require(tx.waitForFinal(10) == ModemResult::Ok, "RX split across begin does not steal query final");
      equal(tx.response(), "\r\n+CSQ: 10,99\r\nOK\r\n", "old-generation URC never pollutes response");
      delivered({"boundary"});
      require(pdu.decodeCalls == 1, "RX split decodes exactly once");
      tx.end();
      require(Serial1.tx == "AT+CSQ\r\n" && !modemBusy(), "begin/end neither drain destructively nor transmit extra AT");
    }
  }
  for (size_t cut = 1; cut < wire.size(); cut++) {
    resetState();
    ModemTransaction tx;
    require(tx.begin("AT"), "begin before partial RX");
    Serial1.feed("OK\r\n" + wire.substr(0, cut));
    require(tx.waitForFinal(0) == ModemResult::Ok, "URC tail in same poll as final OK");
    tx.end();
    require(history.empty(), "no PDU is delivered before its final LF");
    pump(wire.substr(cut));
    delivered({"boundary"});
    require(pdu.decodeCalls == 1, "main loop resumes the transaction's partial URC");
  }
  for (bool error : {false, true}) {
    for (size_t cut : {size_t(3), size_t(10), wire.size() - 3}) {
      resetState();
      {
        ModemTransaction first;
        require(first.begin("AT"), "first transaction begins");
        Serial1.feed(std::string(error ? "ERROR\r\n" : "OK\r\n") + wire.substr(0, cut));
        require(first.waitForFinal(0) == (error ? ModemResult::Error : ModemResult::Ok), "first transaction terminates");
      } // Also exercise error-recovery destructor without flushing partial state.
      Serial1.onWrite = [&](const std::string& text) {
        if (text == "AT+CSQ\r\n") Serial1.schedule(1, wire.substr(cut) + "OK\r\n");
      };
      String response;
      require(runModemCommand("AT+CSQ", 10, &response) == ModemResult::Ok, "URC bridges two transactions");
      equal(response, "\r\nOK\r\n", "partial URC remains URC across generations and recovery");
      delivered({"boundary"});
    }
  }
}

static void beginSubmit(ModemTransaction& tx) {
  require(tx.acquired() && tx.begin("AT+CMGS=1", "+CMGS:", true), "acquire CMGS transaction");
  pump("> ");
  require(tx.waitForPrompt(0), "prompt requires no LF");
  require(tx.submitPdu("0011"), "submit valid hex once");
}

static void cmgsReceipts() {
  for (bool okFirst : {false, true}) {
    for (const std::string receipt : {"+CMGS: 0", "+CMGS: 42", "+CMGS:\t000123"}) {
      resetState();
      ModemTransaction tx;
      beginSubmit(tx);
      const std::string first = okFirst ? "OK" : receipt;
      const std::string second = okFirst ? receipt : "OK";
      pump(first + "\r");
      require(tx.result() == ModemResult::Pending, "unterminated final/receipt is not a complete line");
      pump("\n");
      require(tx.result() == ModemResult::Pending, "both OK and numeric CMGS are required");
      pump(second);
      require(tx.result() == ModemResult::Pending, "second terminal requires LF too");
      pump("\r\n");
      require(tx.waitForFinal(0) == ModemResult::Ok, "OK + valid CMGS succeed in either order");
      equal(tx.resultLine(), receipt, "numeric CMGS receipt retained");
      require(!tx.submitPdu("0011") && sent(0x1A) == 1, "cannot resubmit after success");
      auto before = smsTestTicks;
      tx.end(); tx.end();
      require(smsTestTicks == before && sent(0x1B) == 0 && !modemBusy(), "successful end is immediate and idempotent");
    }
  }
  for (const std::string only : {"OK", "+CMGS: 42"}) {
    resetState();
    ModemTransaction tx;
    beginSubmit(tx);
    pump(only + "\r\n");
    require(tx.waitForFinal(3) == ModemResult::Timeout, "one half alone cannot report SMS success");
    tx.end();
    require(sent(0x1A) == 1 && sent(0x1B) == 0, "submitted SMS is not retried/cancelled on final timeout");
  }
  for (const std::string invalid : {"+CMGS:", "+CMGS: -1", "+CMGS: +1", "+CMGS: 1x",
                                   "+CMGS: 1 2", "+CMGS: 1,2", "+CMGS: 0x12", "+CMGS: OK"}) {
    for (bool okFirst : {false, true}) {
      resetState();
      ModemTransaction tx;
      beginSubmit(tx);
      pump((okFirst ? "OK\r\n" : "") + invalid + "\r\n" + (okFirst ? "" : "OK\r\n"));
      require(tx.waitForFinal(0) == ModemResult::Invalid, "malformed CMGS never confirms submission: " + invalid);
      equal(tx.resultLine(), "", "invalid CMGS not retained as receipt");
    }
  }
  for (const std::string embedded : {"NOK", "OKAY", "BOOK", "data OK data", "+DATA: OK", "OK OK"}) {
    resetState();
    ModemTransaction tx;
    beginSubmit(tx);
    pump("AT+CMGS=1\r\n+CMGS: 42\r\n" + embedded + "\r\n");
    require(tx.response().indexOf("AT+CMGS=1") < 0, "exact command echo ignored");
    require(tx.waitForFinal(2) == ModemResult::Timeout, "embedded OK is not a terminal: " + embedded);
  }
  for (const std::string fake : {"x+CMGS: 42", "+CMGSX: 42", "+CMGS"}) {
    resetState();
    ModemTransaction tx;
    beginSubmit(tx);
    pump(fake + "\r\nOK\r\n");
    require(tx.waitForFinal(2) == ModemResult::Timeout, "prefix-like text is not CMGS receipt");
  }
}

static void staleResponses() {
  for (bool readInLoop : {false, true}) {
    for (const std::string prefix : {"O", "OK", "OK\r"}) {
      resetState();
      Serial1.feed(prefix);
      if (readInLoop) checkSerial1URC();
      ModemTransaction tx;
      require(tx.begin("AT+ECHO=\"OK\""), "query begins after old partial OK");
      pump((prefix == "O" ? "K\r\n" : "\n") + std::string("AT+ECHO=\"OK\"\r\n"));
      require(tx.result() == ModemResult::Pending, "old partial and echoed OK must not satisfy new transaction");
      equal(tx.response(), "", "old generation and echo excluded completely");
      pump("OK\r\n");
      require(tx.waitForFinal(0) == ModemResult::Ok, "fresh complete OK still accepted");
    }
  }
  for (bool previousSucceeded : {false, true}) {
    resetState();
    {
      ModemTransaction previous;
      require(previous.begin("AT+PREVIOUS"), "old transaction begins");
      pump(previousSucceeded ? "OK\r\nO" : "O");
      require(previous.waitForFinal(1) == (previousSucceeded ? ModemResult::Ok : ModemResult::Timeout),
              "old transaction ends while a second OK remains partial");
    }
    ModemTransaction next;
    require(next.begin("AT+CMGS=1", "+CMGS:", true), "new SMS starts with old partial OK still buffered");
    pump("K\r\n> ");
    require(next.waitForPrompt(0) && next.submitPdu("00"), "old partial completes before new prompt");
    pump("+CMGS: 1\r\n");
    require(next.waitForFinal(2) == ModemResult::Timeout, "partial OK from previous command is never new SMS success");
  }
  resetState();
  Serial1.feed("OK\r\n+CMGS: 7\r\n");
  ModemTransaction tx;
  require(tx.begin("AT+CMGS=1", "+CMGS:", true), "CMGS begins after queued old finals");
  pump("OK\r\n+CMGS: 8\r\n> \r\nOK\r\n+CMGS: 9\r\n");
  require(tx.waitForPrompt(0), "old pre-submit terminals do not end prompt wait");
  require(tx.result() == ModemResult::Pending && tx.resultLine().length() == 0, "no receipt before input submission");
  require(tx.submitPdu("0011"), "submit after old terminals");
  require(!tx.submitPdu("0011") && sent(0x1A) == 1, "pending final cannot re-submit Ctrl+Z");
  pump("+CMGS: 42\r\n");
  require(tx.result() == ModemResult::Pending, "old pre-submit OK was forgotten");
  pump("OK\r\n");
  require(tx.waitForFinal(0) == ModemResult::Ok, "fresh post-submit receipt and OK succeed");
}

static void promptsAndRecovery() {
  for (const std::string prompt : {">", "> ", "\r\n>\t ", " \t> \t"}) {
    resetState();
    ModemTransaction tx;
    require(tx.begin("AT+CMGS=1", "+CMGS:", true), "begin prompt variants");
    pump(prompt);
    require(tx.waitForPrompt(0) && tx.submitPdu("00"), "whitespace/no-LF prompt accepted");
    pump("+CMGS: 0\r\nOK\r\n");
    require(tx.waitForFinal(0) == ModemResult::Ok && sent(0x1A) == 1, "prompt space never leaks into reply");
  }
  resetState();
  {
    ModemTransaction tx;
    require(tx.begin("AT+CMGS=1", "+CMGS:", true), "begin inline > case");
    pump("value>still a line\r\n");
    Serial1.schedule(4, "> ");
    require(tx.waitForPrompt(10) && smsTestTicks == 4, "inline > is not a prompt");
    equal(tx.response(), "\r\nvalue>still a line\r\n", "inline > retained as ordinary data");
    require(tx.submitPdu("00"), "submit after actual prompt");
    pump("+CMGS: 1\r\nOK\r\n");
    require(tx.waitForFinal(0) == ModemResult::Ok, "normal recovery from inline >");
  }
  for (bool smsCommand : {false, true}) {
    resetState();
    ModemTransaction tx;
    require(tx.begin(smsCommand ? "AT+CMGS=1" : "AT", smsCommand ? "+CMGS:" : nullptr, smsCommand), "begin unexpected prompt case");
    pump(smsCommand ? "> > " : "> ");
    require(tx.result() == ModemResult::Invalid, "unexpected/duplicate prompt rejected");
    require(!tx.submitPdu("00"), "invalid prompt cannot submit");
    tx.end();
    require(sent(0x1A) == 0 && sent(0x1B) == 1, "unexpected prompt cancelled with ESC");
  }
  resetState();
  {
    ModemTransaction tx;
    require(tx.begin("AT+CMGS=1", "+CMGS:", true), "begin prompt timeout");
    Serial1.schedule(7, cmt("during recovery") + clip + "\r\nRING\r\n> ");
    require(!tx.waitForPrompt(5) && tx.result() == ModemResult::Timeout, "prompt wait times out at its limit");
    require(smsTestTicks == 5, "prompt timeout is bounded");
    tx.end();
    require(smsTestTicks == 5 + MODEM_RECOVERY_MS, "recovery has fixed bounded duration");
    require(sent(0x1B) == 2 && sent(0x1A) == 0, "pending input and late orphan prompt each receive ESC");
    delivered({"during recovery"});
    require(callLines == std::vector<String>{clip, "RING"}, "recovery still dispatches call URCs");
    const auto before = smsTestTicks;
    tx.end();
    require(smsTestTicks == before && sent(0x1B) == 2, "end does not cancel twice");
  }
  Serial1.feed("> "); // Arrives after the old transaction ended, before the next begin.
  Serial1.onWrite = [](const std::string& text) {
    if (text == "AT\r\n") Serial1.schedule(1, "OK\r\n");
  };
  require(runModemCommand("AT", 5) == ModemResult::Ok, "fresh command succeeds after orphan cancellation");
  require(sent(0x1B) == 3 && Serial1.tx.substr(Serial1.tx.size() - 5) == std::string("\x1B") + "AT\r\n",
          "begin cancels already queued orphan before writing next AT");

  resetState();
  {
    ModemTransaction tx;
    beginSubmit(tx);
    pump("> ");
    require(tx.result() == ModemResult::Invalid, "second prompt after submit is unexpected too");
    tx.end();
    require(sent(0x1A) == 1 && sent(0x1B) == 1, "unexpected post-submit prompt cancelled without resubmit");
  }
  resetState();
  {
    ModemTransaction tx;
    require(tx.begin("AT+CMGS=1", "+CMGS:", true), "begin abandoned input");
    require(tx.waitForFinal(0) == ModemResult::Invalid, "final cannot be awaited before submitting input");
  }
  require(sent(0x1B) == 1 && !modemBusy(), "destructor cancels abandoned input and releases ownership");
}

static void preSubmitPartialFinal(bool partialOK) {
  resetState();
  ModemTransaction tx;
  require(tx.begin("AT+CMGS=1", "+CMGS:", true), "begin partial pre-submit final case");
  // Same poll sees a no-LF prompt then the start of an old final, before Ctrl+Z exists.
  pump(partialOK ? "> O" : "> +CMGS: 4");
  require(tx.waitForPrompt(0) && tx.result() == ModemResult::Pending, "old final is still partial before submit");
  require(tx.submitPdu("00"), "submit after reading old partial final");
  pump(partialOK ? "K\r\n+CMGS: 42\r\n" : "2\r\nOK\r\n");
  const ModemResult actual = tx.waitForFinal(3);
  require(sent(0x1A) == 1, "pre-submit partial-final rejection never retries input");
  require(actual == ModemResult::Timeout, std::string(
          partialOK ? "pre-submit partial OK must not combine with a fresh CMGS to report success"
                    : "pre-submit partial CMGS must not combine with a fresh OK to report success") +
          (actual == ModemResult::Ok ? " (actual: Ok; expected: Timeout)" : " (expected: Timeout)"));
}

static void preSubmitQueuedFinals() {
  const std::vector<std::pair<std::string, std::string>> replies = {
    {"OK\r\n", "+CMGS: 42\r\n"},
    {"+CMGS: 42\r\n", "OK\r\n"},
    {"OK\r\n+CMGS: 42\r\n", ""},
    {"+CMGS: 42\r\nOK\r\n", ""},
    {"O", "K\r\n+CMGS: 42\r\n"},
    {"+CMGS: 4", "2\r\nOK\r\n"}
  };
  for (const auto& reply : replies) {
    resetState();
    ModemTransaction tx;
    require(tx.begin("AT+CMGS=1", "+CMGS:", true), "begin queued pre-submit final case");
    pump("> ");
    require(tx.waitForPrompt(0), "prompt before queued old finals");
    Serial1.feed(reply.first); // Deliberately no poll/wait between feed and submit.
    require(tx.submitPdu("00"), "submit with old finals queued but unread");
    require(!Serial1.available() && tx.result() == ModemResult::Pending && tx.resultLine().length() == 0,
            "queued RX dispatched before submission without accepting a final");
    pump(reply.second);
    require(tx.waitForFinal(3) == ModemResult::Timeout, "unread pre-submit finals cannot complete fresh submission");
    require(sent(0x1A) == 1 && modemBusy(), "final timeout retains ownership without retry");
    tx.end();
    require(sent(0x1B) == 0 && !modemBusy(), "submitted timeout releases ownership without ESC");
  }
}

static void preSubmitCallbackFinals() {
  const std::vector<std::pair<std::string, std::string>> replies = {
    {"OK\r\n+CMGS: 42\r\n", ""},
    {"+CMGS: 42\r\nOK\r\n", ""},
    {"OK\r\n", "+CMGS: 42\r\n"},
    {"+CMGS: 42\r\n", "OK\r\n"},
    {"O", "K\r\n+CMGS: 42\r\n"},
    {"+CMGS: 4", "2\r\nOK\r\n"}
  };
  for (bool fromCall : {false, true}) {
    for (const auto& reply : replies) {
      resetState();
      ModemTransaction tx;
      require(tx.begin("AT+CMGS=1", "+CMGS:", true), "begin callback-injected old final case");
      pump("> ");
      require(tx.waitForPrompt(0), "prompt before callback-injected RX");
      int callbacks = 0;
      auto inject = [&] { callbacks++; Serial1.feed(reply.first); };
      if (fromCall) onCall = inject;
      else onHistory = inject;
      // The injected bytes do not exist in submitPdu's initial poll snapshot.
      Serial1.feed(fromCall ? clip + "\r\n" : cmt("submit callback"));
      const bool submitted = tx.submitPdu("00");
      onHistory = onCall = nullptr;
      const int unread = Serial1.available();
      require(submitted, "finite callback RX can be synchronized before submission");
      pump(reply.second);
      const ModemResult actual = tx.waitForFinal(3);
      require(actual == ModemResult::Timeout,
              std::string("old finals arriving inside pre-submit URC callback cannot report SMS success") +
              (actual == ModemResult::Ok ? " (actual: Ok; expected: Timeout)" : " (expected: Timeout)"));
      require(unread == 0 && callbacks == 1, "callback RX drained before generation changes, URC dispatched once");
      delivered(fromCall ? std::vector<std::string>{} : std::vector<std::string>{"submit callback"});
      require(callLines == (fromCall ? std::vector<String>{clip} : std::vector<String>{}), "call dispatch is not replayed");
      tx.end();
      require(Serial1.tx == "AT+CMGS=1\r\n00\x1A" && !modemBusy(), "one submission, no retry or ESC after final timeout");
    }
  }
}

static void beginCallbackFinals() {
  for (bool fromCall : {false, true}) {
    resetState();
    int callbacks = 0;
    auto inject = [&] { callbacks++; Serial1.feed("OK\r\n+CMGS: 42\r\n"); };
    if (fromCall) onCall = inject;
    else onHistory = inject;
    Serial1.feed(fromCall ? clip + "\r\n" : cmt("begin callback"));
    String response;
    const ModemResult actual = runModemCommand("AT+CSQ", 3, &response);
    onHistory = onCall = nullptr;
    require(actual == ModemResult::Timeout, "begin callback's old OK must not satisfy an unanswered new query");
    equal(response, "", "pre-begin callback finals excluded from the new response");
    require(callbacks == 1 && Serial1.tx == "AT+CSQ\r\n" && !modemBusy(), "begin drains without duplicate URCs or extra writes");
    delivered(fromCall ? std::vector<std::string>{} : std::vector<std::string>{"begin callback"});
    require(callLines == (fromCall ? std::vector<String>{clip} : std::vector<String>{}), "begin preserves call dispatch");
  }
}

static void rxSyncBudget() {
  for (bool submitting : {false, true}) {
    for (bool wrap : {false, true}) {
      resetState();
      ModemTransaction tx;
      if (submitting) {
        require(tx.begin("AT+CMGS=1", "+CMGS:", true), "begin before continuous pre-submit RX");
        pump("> ");
        require(tx.waitForPrompt(0), "prompt before synchronization budget test");
      }
      const unsigned long start = wrap ? ULONG_MAX - 2 : 1000;
      resetSmsHostClock(start);
      size_t callbacks = 0;
      onCall = [&] { callbacks++; Serial1.feed("RING\r\n"); };
      Serial1.feed("RING\r\n");
      const bool accepted = submitting ? tx.submitPdu("00") : tx.begin("AT+CSQ");
      onCall = nullptr; // Stop ingress before assertions/destructor, including on failure.
      require(!accepted && tx.result() == ModemResult::Busy, "unread RX at sync deadline rejects the generation change");
      require(smsTestTicks == MODEM_RX_SYNC_MS && millis() - start == MODEM_RX_SYNC_MS,
              "sync budget is bounded and rollover-safe");
      require(callbacks > 1 && callbacks <= MODEM_RX_SYNC_MS + 1, "synchronization rechecks multiple finite poll snapshots");
      require(smsTestWdtResets == MODEM_RX_SYNC_MS && smsTestYields == MODEM_RX_SYNC_MS,
              "continuous RX synchronization feeds watchdog and yields");
      require(Serial1.available() == 6 && Serial1.tx == (submitting ? "AT+CMGS=1\r\n" : ""),
              "remaining URC is preserved; no new command/PDU/CtrlZ is written");
      require(!tx.submitPdu("00") && !tx.begin("AT+RETRY"), "failed synchronization cannot retry within the transaction");
      tx.end();
      require(smsTestTicks == MODEM_RX_SYNC_MS + (submitting ? MODEM_RECOVERY_MS : 0) &&
              sent(0x1A) == 0 && sent(0x1B) == (submitting ? 1u : 0u) && !modemBusy(),
              "only pending SMS input needs ESC and bounded recovery");
      pollModem();
      require(!Serial1.available() && callLines.size() == callbacks + 1, "cleanup/loop dispatches the retained URC exactly once");
      Serial1.onWrite = [](const std::string& text) {
        if (text == "AT+CSQ\r\n") Serial1.schedule(1, "OK\r\n");
      };
      require(runModemCommand("AT+CSQ", 3) == ModemResult::Ok, "next transaction works once continuous ingress stops");
    }
  }
}

static void submitBoundaryUrc() {
  const std::string wire = cmt("submit boundary");
  for (bool readInLoop : {false, true}) {
    for (size_t cut : {size_t(3), size_t(10), wire.size() - 3}) {
      resetState();
      ModemTransaction tx;
      require(tx.begin("AT+CMGS=1", "+CMGS:", true), "begin URC submit boundary case");
      pump("> ");
      require(tx.waitForPrompt(0), "prompt before queued and partial URCs");
      Serial1.feed(cmt("queued at submit") + clip + "\r\nRING\r\n" + wire.substr(0, cut));
      if (readInLoop) checkSerial1URC();
      require(tx.submitPdu("00"), "submit while CMT header/PDU is incomplete");
      require(!Serial1.available() && history.size() == 1 && pdu.decodeCalls == 1,
              "submit dispatches complete queued SMS while preserving partial URC");
      require(callLines == std::vector<String>{clip, "RING"}, "submit dispatches queued call URCs");
      pump(wire.substr(cut) + "+CMGS: 42\r\nOK\r\n");
      require(tx.waitForFinal(0) == ModemResult::Ok, "fresh finals succeed after cross-submit URC");
      equal(tx.response(), "\r\n+CMGS: 42\r\nOK\r\n", "URCs stay independent of the submission response epoch");
      delivered({"queued at submit", "submit boundary"});
      require(pdu.decodeCalls == 2 && Serial1.tx == "AT+CMGS=1\r\n00\x1A" && modemBusy(),
              "both SMS decoded once; exact PDU/CtrlZ written without releasing ownership");
    }
  }
}

static void preSubmitQueuedErrors() {
  for (const std::string error : {"ERROR", "+CMS ERROR: 500", "+CME ERROR: 10", "NO ANSWER", "BUSY"}) {
    for (bool fromCallback : {false, true}) {
      resetState();
      ModemTransaction tx;
      require(tx.begin("AT+CMGS=1", "+CMGS:", true), "begin queued pre-submit error case");
      pump("> ");
      require(tx.waitForPrompt(0), "prompt before queued error");
      const std::string wire = error + "\r\n" + cmt("queued after error");
      if (fromCallback) onCall = [&] { Serial1.feed(wire); };
      Serial1.feed(fromCallback ? clip + "\r\n" : wire);
      const bool submitted = tx.submitPdu("00");
      onCall = nullptr;
      require(!submitted && tx.result() == ModemResult::Error, "queued/callback error aborts submission before PDU write");
      require(Serial1.tx == "AT+CMGS=1\r\n" && modemBusy(), "queued error writes no input and retains ownership");
      require(!Serial1.available(), "pre-submit poll dispatches all queued RX even after an error");
      delivered({"queued after error"});
      tx.end();
      require(sent(0x1A) == 0 && sent(0x1B) == 1 && !modemBusy(), "unsubmitted error keeps ESC cleanup and releases ownership");
    }
  }
}

static void callEndUrc() {
  for (bool sms : {false, true}) {
    for (const std::string final : {"OK", "ERROR", "+CMS ERROR: 500", "+CME ERROR: 10"}) {
      resetState();
      ModemTransaction tx;
      require(tx.begin(sms ? "AT+CMGS=1" : "AT+CSQ", sms ? "+CMGS:" : nullptr, sms), "begin call-end URC case");
      if (sms) {
        pump("> ");
        require(tx.waitForPrompt(0), "prompt before queued call-end URC");
      }
      // The call spy deliberately does NOT consume NO CARRIER: production modem must isolate it.
      const auto input = incoming("call ended");
      Serial1.feed(cmtHeader(input) + clip + "\r\nRING\r\nNO CARRIER\r\n" + input + "\r\n");
      if (sms) require(tx.submitPdu("00"), "queued NO CARRIER does not abort SMS input");
      else pollModem();
      require(tx.result() == ModemResult::Pending, "call-end URC is not an SMS/query final");
      equal(tx.response(), "", "NO CARRIER excluded from transaction response, like other call URCs");
      // Exercise post-submit/final-wait NO CARRIER as well as genuine terminal failures.
      Serial1.schedule(2, std::string("NO CARRIER\r\n") + (sms && final == "OK" ? "+CMGS: 42\r\n" : "") + final + "\r\n");
      require(tx.waitForFinal(5) == (final == "OK" ? ModemResult::Ok : ModemResult::Error),
              "call end neither fails valid finals nor hides ERROR/CMS/CME errors");
      require(tx.response().indexOf("NO CARRIER") < 0, "call-end URCs never consume response capacity");
      delivered({"call ended"});
      require(callLines == std::vector<String>{clip, "RING"}, "call dispatch and pending CMT/PDU survive call end");
      tx.end();
      require(sent(0x1A) == (sms ? 1u : 0u) && sent(0x1B) == 0 && !modemBusy(), "call end causes no cancellation or retry");
    }
  }
}

static void errorsAndWrites() {
  for (const std::string error : {"ERROR", "+CMS ERROR: 500", "+CME ERROR: 10", "NO ANSWER", "BUSY"}) {
    for (bool beforePrompt : {false, true}) {
      resetState();
      ModemTransaction tx;
      require(tx.begin("AT+CMGS=1", "+CMGS:", true), "begin error case");
      if (!beforePrompt) {
        pump("> ");
        require(tx.waitForPrompt(0) && tx.submitPdu("00"), "submit before final error");
      }
      Serial1.schedule(2, error + "\r\n");
      if (beforePrompt) require(!tx.waitForPrompt(10), "error terminates prompt wait");
      else require(tx.waitForFinal(10) == ModemResult::Error, "error terminates final wait");
      require(tx.result() == ModemResult::Error && smsTestTicks == 2, "error is immediate, not timeout");
      Serial1.schedule(1, cmt("error recovery"));
      tx.end();
      require(sent(0x1A) == (beforePrompt ? 0u : 1u) && sent(0x1B) == (beforePrompt ? 1u : 0u),
              "error cancels only unsubmitted input");
      delivered({"error recovery"});
    }
  }
  for (bool failCtrlZ : {false, true}) {
    resetState();
    ModemTransaction tx;
    require(tx.begin("AT+CMGS=1", "+CMGS:", true), "begin short write injection");
    pump("> ");
    require(tx.waitForPrompt(0), "prompt before short write");
    Serial1.writeResults = failCtrlZ ? std::deque<size_t>{4, 0} : std::deque<size_t>{2};
    require(!tx.submitPdu("0011") && tx.result() == ModemResult::Error, "short bulk/CtrlZ write is an error");
    require(!tx.submitPdu("0011"), "failed write cannot be retried by same transaction");
    tx.end();
    require(sent(0x1A) == 0 && sent(0x1B) == 1, "partial submission leaves ESC cleanup, never a second CtrlZ");
    size_t zAttempts = static_cast<size_t>(std::count_if(Serial1.writes.begin(), Serial1.writes.end(),
        [](const HardwareSerial::Write& write) { return write.requested == std::string(1, '\x1A'); }));
    require(zAttempts == (failCtrlZ ? 1u : 0u), "CtrlZ attempted only once and only after full bulk write");
  }
  for (const std::string& invalid : std::vector<std::string>{"", "0", "0G", "00\r\n", "00\x1A", "00\x1B", std::string(SERIAL_BUFFER_SIZE, 'A')}) {
    resetState();
    ModemTransaction tx;
    require(tx.begin("AT+CMGS=1", "+CMGS:", true), "begin invalid PDU input");
    pump("> ");
    require(tx.waitForPrompt(0), "prompt before invalid PDU");
    require(!tx.submitPdu(invalid.c_str()) && tx.result() == ModemResult::Invalid, "invalid hex rejected before write");
    tx.end();
    require(sent(0x1A) == 0 && sent(0x1B) == 1 && Serial1.writes.size() == 2, "invalid PDU sends only command and ESC");
  }
  resetState();
  Serial1.onWrite = [](const std::string& text) {
    if (text.rfind("AT+CMGS=", 0) == 0) {
      Serial1.schedule(1000, cmt("prompt timeout receive"));
      Serial1.schedule(4000, clip + "\r\nRING\r\n");
    }
  };
  require(!sendSMS("15500000000", "no prompt"), "real sendSMS propagates prompt timeout");
  require(smsTestTicks == 5000 + MODEM_RECOVERY_MS && sent(0x1A) == 0 && sent(0x1B) == 1,
          "real sendSMS uses its 5s prompt budget and ESC cleanup");
  delivered({"prompt timeout receive"});
  require(callLines == std::vector<String>{clip, "RING"}, "real sendSMS does not lose calls while timing out");
}

static void nestedBusy() {
  resetState();
  ModemTransaction outer;
  require(outer.acquired() && modemBusy(), "ownership starts at construction before encoding");
  {
    ModemTransaction nested;
    require(!nested.acquired() && nested.result() == ModemResult::Busy, "nested constructor rejected");
    require(!nested.begin("AT") && !nested.waitForPrompt(0) && !nested.submitPdu("00"), "busy owner cannot write");
    require(nested.waitForFinal(0) == ModemResult::Busy, "busy result preserved");
    nested.end();
    require(modemBusy(), "nested end cannot release outer owner");
  }
  String response = "old", receipt = "old";
  require(runModemCommand("AT+NESTED", 0, &response, &receipt) == ModemResult::Busy, "run helper respects ownership");
  require(response.length() == 0 && receipt.length() == 0, "busy helper clears caller output buffers");
  require(!sendSMS("15500000000", "nested") && pdu.encodeCalls == 0 && Serial1.tx.empty(), "busy send cannot clobber global PDU or UART");
  int callbacks = 0;
  onHistory = [&] {
    callbacks++;
    auto reads = Serial1.readCalls;
    pollModem();
    require(Serial1.readCalls == reads, "reentrant poll cannot consume outer reader's RX");
    require(runModemCommand("AT+CALLBACK", 0) == ModemResult::Busy, "URC callback cannot nest AT");
  };
  require(outer.begin("AT"), "outer owner remains usable");
  require(!outer.begin("AT+AGAIN"), "cannot begin twice");
  pump(cmt("outer") + "OK\r\n");
  require(outer.waitForFinal(0) == ModemResult::Ok && callbacks == 1, "outer completes after blocked callback");
  require(Serial1.tx == "AT\r\n", "no nested writes escaped guard");
  onHistory = nullptr;
  outer.end();
  require(!modemBusy(), "outer releases ownership");

  // There is no transaction here: modemReading alone must also reject a nested command.
  onCall = [&] {
    require(modemBusy() && runModemCommand("AT+CALLBACK", 0) == ModemResult::Busy, "main-loop URC dispatch is non-reentrant");
  };
  pump(clip + "\r\nRING\r\n");
  onCall = nullptr;
  require(!modemBusy() && Serial1.tx == "AT\r\n", "reader guard released without an owner");
}

static void lostLines() {
  for (bool nul : {false, true}) {
    for (bool active : {false, true}) {
      resetState();
      auto tx = active ? std::make_unique<ModemTransaction>() : nullptr;
      if (tx) require(tx->begin("AT+CSQ"), "start transaction before damaged line");
      pump(cmtHeader(incoming("must drop")));
      const std::string damage = nul ? std::string("bad\0", 4) : std::string(SERIAL_BUFFER_SIZE, 'A');
      pump(damage + incoming("must drop") + "> ");
      require(history.empty() && pdu.decodeCalls == 0, "overlong/NUL line and its hex suffix discarded entirely");
      require(sent(0x1B) == 0, "prompt-like bytes inside discarded line are not acted on");
      if (tx) require(tx->result() == ModemResult::Overflow, "current-generation RX corruption fails transaction");
      pump("\n" + incoming("also no header") + "\r\n");
      require(pdu.decodeCalls == 0, "lost line resets CMT wait so bare next PDU is not decoded");
      pump(cmt("next good line") + clip + "\r\nRING\r\n");
      delivered({"next good line"});
      require(pdu.decodeCalls == 1 && callLines == std::vector<String>{clip, "RING"}, "next complete URCs recover normally");
      if (tx) {
        require(tx->result() == ModemResult::Overflow, "valid later RX does not erase failure");
        require(tx->response().length() <= MODEM_RESPONSE_LIMIT, "failed response still bounded");
        tx->end();
      }
    }
    // Discard mode persists across a command boundary, but old corruption is not its response.
    resetState();
    Serial1.feed(nul ? std::string("old\0", 4) : std::string(SERIAL_BUFFER_SIZE + 20, 'A'));
    ModemTransaction tx;
    require(tx.begin("AT"), "begin while old RX enters discard mode");
    pump("OK>" + incoming("discarded tail"));
    require(tx.result() == ModemResult::Pending && history.empty() && sent(0x1B) == 0, "old damaged suffix cannot satisfy or cancel new command");
    pump("\nOK\r\n" + cmt("after old damage"));
    require(tx.waitForFinal(0) == ModemResult::Ok, "next clean OK is usable after LF resynchronization");
    equal(tx.response(), "\r\nOK\r\n", "no tail from corrupt line retained");
    delivered({"after old damage"});
  }
  resetState();
  ModemTransaction tx;
  require(tx.begin("AT"), "start exact line-capacity test");
  const std::string maxLine(SERIAL_BUFFER_SIZE - 1, 'L');
  pump(maxLine + "\r\n");
  require(tx.result() == ModemResult::Pending, "buffer-size-minus-one line allowed");
  equal(tx.response(), "\r\n" + maxLine + "\r\n", "maximum line preserved byte-for-byte");
  pump("OK\r\n");
  require(tx.waitForFinal(0) == ModemResult::Ok, "maximum line followed by final OK succeeds");
}

static std::string responseBody(size_t budget) {
  // Include framing cost; keep each independent line under the actual RX line cap.
  std::string body;
  size_t remaining = budget - 2; // response_ prepends CRLF once.
  while (remaining) {
    if (remaining < 3) throw std::runtime_error("invalid response-budget fixture");
    size_t chars = std::min(remaining - 2, static_cast<size_t>(SERIAL_BUFFER_SIZE - 1));
    size_t rest = remaining - chars - 2;
    if (rest > 0 && rest < 3) chars -= 3 - rest;
    body += std::string(chars, 'X') + "\r\n";
    remaining -= chars + 2;
  }
  return body;
}

static void responseLimits() {
  for (bool overflow : {false, true}) {
    resetState();
    ModemTransaction tx;
    require(tx.begin("AT+DATA"), "begin capacity query");
    const std::string body = responseBody(MODEM_RESPONSE_LIMIT - 4);
    pump(body);
    equal(tx.response(), "\r\n" + body, "all response framing counted exactly");
    require(tx.response().length() == MODEM_RESPONSE_LIMIT - 4 && tx.result() == ModemResult::Pending,
            "response leaves exactly one OK line of space");
    if (overflow) {
      pump("Y\r\n");
      require(tx.response().length() == MODEM_RESPONSE_LIMIT - 1, "one byte below cap still accepted");
    }
    pump("OK\r\n" + cmt("after capacity") + clip + "\r\nRING\r\n");
    require(tx.waitForFinal(0) == (overflow ? ModemResult::Overflow : ModemResult::Ok),
            "exact cap succeeds, crossing cap fails rather than truncating to success");
    require(tx.response().length() == MODEM_RESPONSE_LIMIT - (overflow ? 1 : 0), "never exceeds response cap");
    delivered({"after capacity"});
    require(callLines == std::vector<String>{clip, "RING"}, "full/error response still dispatches call URCs");
    tx.end();
  }
  resetState();
  {
    ModemTransaction tx;
    require(tx.begin("AT+DATA"), "begin exact-full pending response");
    const std::string body = responseBody(MODEM_RESPONSE_LIMIT);
    pump(body);
    require(tx.response().length() == MODEM_RESPONSE_LIMIT && tx.result() == ModemResult::Pending, "full response allowed absent terminator");
    pump("OK\r\n");
    require(tx.waitForFinal(0) == ModemResult::Overflow, "missing space for terminal cannot imply success");
    equal(tx.response(), "\r\n" + body, "overflow does not append partial OK");
  }
  resetState();
  {
    ModemTransaction tx;
    require(tx.begin("AT+SMALL"), "begin large URC burst");
    std::string burst;
    for (int i = 0; i < 32; i++) burst += cmt("burst");
    require(burst.size() > MODEM_RESPONSE_LIMIT, "fixture exceeds response capacity");
    pump(burst + "OK\r\n");
    require(tx.waitForFinal(0) == ModemResult::Ok, "dispatched URCs do not consume command response budget");
    equal(tx.response(), "\r\nOK\r\n", "only final OK in response despite URC burst");
    delivered(std::vector<std::string>(32, "burst"));
    require(pdu.decodeCalls == 32, "entire preexisting burst dispatched in one poll");
  }
}

static void mpingResponses() {
  const std::string command = "AT+MPING=\"192.0.2.1\"";
  const std::string receipt = "+MPING: 0,\"192.0.2.1\",32,12";
  for (bool okFirst : {false, true}) {
    resetState();
    Serial1.onWrite = [&](const std::string& text) {
      if (text == command + "\r\n") {
        Serial1.schedule(2, (okFirst ? "OK" : receipt) + "\r\n");
        Serial1.schedule(4, cmt("during ping") + clip + "\r\nRING\r\n");
        Serial1.schedule(7, (okFirst ? receipt : "OK") + "\r\n");
      }
    };
    String response, line;
    require(runModemCommand(command.c_str(), 20, &response, &line) == ModemResult::Ok, "MPING auto-requires result plus OK in either order");
    require(smsTestTicks == 7, "MPING helper waits for the second necessary line");
    equal(line, receipt, "MPING result exposed to caller");
    equal(response, "\r\n" + (okFirst ? "OK\r\n" + receipt : receipt + "\r\nOK") + "\r\n", "URCs excluded from MPING output");
    delivered({"during ping"});
    require(callLines == std::vector<String>{clip, "RING"}, "MPING waits dispatch calls");
  }
  for (const std::string& only : std::vector<std::string>{"OK", receipt, "BOOK\r\n" + receipt}) {
    resetState();
    Serial1.onWrite = [&](const std::string& text) {
      if (text == command + "\r\n") Serial1.schedule(1, only + "\r\n");
    };
    require(runModemCommand(command.c_str(), 5) == ModemResult::Timeout, "MPING never succeeds with one half or embedded OK");
    require(smsTestTicks == 5 + MODEM_RECOVERY_MS, "MPING failure timeout/recovery bounded");
  }
  resetState();
  {
    ModemTransaction tx;
    require(tx.begin("AT+CSQ"), "start ordinary query for late asynchronous results");
    pump(receipt + "\r\n+CMGS: invalid-old\r\n+CSQ: 14,99\r\nOK\r\n");
    require(tx.waitForFinal(0) == ModemResult::Ok, "late MPING/CMGS do not affect ordinary query");
    equal(tx.response(), "\r\n+CSQ: 14,99\r\nOK\r\n", "irrelevant asynchronous receipts excluded");
  }
  resetState();
  Serial1.onWrite = [](const std::string& text) {
    if (text == "AT+CUSTOM\r\n") { Serial1.schedule(1, "OK\r\n"); Serial1.schedule(3, "+CUSTOM: 9\r\n"); }
  };
  String result;
  require(runModemCommand("AT+CUSTOM", 10, nullptr, &result, "+CUSTOM:") == ModemResult::Ok, "explicit prefix supported too");
  equal(result, "+CUSTOM: 9", "explicit result line returned");
}

static void clockAndSharedReader() {
  // No misleading 64-bit-host subtraction masquerading as an ESP32 millis wrap test.
  static_assert(sizeof(unsigned long) == 4, "modem wrap regressions require 32-bit unsigned long (Windows host GCC or -m32)");
  resetState();
  resetSmsHostClock(ULONG_MAX - 3);
  Serial1.schedule(1, cmt("wrap delay"));
  Serial1.schedule(6, clip + "\r\nRING\r\n");
  modemDelay(9);
  require(smsTestTicks == 9 && millis() == 5, "modemDelay crosses actual 32-bit rollover");
  delivered({"wrap delay"});
  require(callLines == std::vector<String>{clip, "RING"}, "delay processes events on both sides of wrap");
  require(smsTestWdtChecks == 9 && smsTestWdtResets == 9 && smsTestYields == 9 && smsTestDelays == 9,
          "delay checks registered watchdog, resets and yields every virtual millisecond");

  resetState();
  smsTestWdtStatus = 1;
  modemDelay(4);
  require(smsTestWdtChecks == 4 && smsTestWdtResets == 0 && smsTestYields == 4, "unregistered watchdog is not reset");

  for (bool prompt : {false, true}) {
    resetState();
    resetSmsHostClock(ULONG_MAX - 3);
    ModemTransaction tx;
    require(tx.begin(prompt ? "AT+CMGS=1" : "AT", prompt ? "+CMGS:" : nullptr, prompt), "begin timeout near rollover");
    Serial1.schedule(2, cmt("wrap timeout"));
    if (prompt) require(!tx.waitForPrompt(9), "prompt times out across wrap");
    else require(tx.waitForFinal(9) == ModemResult::Timeout, "final times out across wrap");
    require(tx.result() == ModemResult::Timeout && smsTestTicks == 9 && millis() == 5, "timeout uses unsigned subtraction");
    delivered({"wrap timeout"});
    tx.end();
    require(smsTestTicks == 9 + MODEM_RECOVERY_MS && sent(0x1B) == (prompt ? 1u : 0u), "recovery runs after wrapping timeout");
  }
  resetState();
  resetSmsHostClock(ULONG_MAX - 2);
  Serial1.onWrite = [](const std::string& text) {
    if (text == "AT\r\n") Serial1.schedule(7, "OK\r\n");
  };
  require(runModemCommand("AT", 10) == ModemResult::Ok && smsTestTicks == 7 && millis() == 4, "delayed OK after wrap still succeeds");

  resetState();
  resetSmsHostClock(ULONG_MAX - 2);
  {
    ModemTransaction tx;
    require(tx.begin("AT+CMGS=1", "+CMGS:", true), "begin prompt near wrap");
    Serial1.schedule(5, "> ");
    require(tx.waitForPrompt(9) && millis() == 2 && smsTestTicks == 5, "prompt arrives after wrap before timeout");
    require(tx.submitPdu("00"), "submit after wrapping prompt wait");
    Serial1.schedule(2, "OK\r\n");
    Serial1.schedule(4, "+CMGS: 2\r\n");
    require(tx.waitForFinal(7) == ModemResult::Ok && smsTestTicks == 9, "final receipt succeeds after wrapping prompt");
  }

  resetState();
  const std::string wire = cmt("shared reader");
  Serial1.feed(wire.substr(0, 3));
  checkSerial1URC();
  require(history.empty(), "main loop holds partial CMT in shared buffer");
  char separate[16]{};
  int pos = 0;
  Serial1.feed(wire.substr(3));
  auto queued = Serial1.available();
  auto reads = Serial1.readCalls;
  equal(readSerialLine(Serial1), "", "legacy one-arg reader refuses modem UART");
  equal(readSerialLine(Serial1, separate, sizeof(separate), pos), "", "legacy caller-buffer reader refuses modem UART");
  require(Serial1.available() == queued && Serial1.readCalls == reads && pos == 0, "legacy overloads cannot steal even partial bytes");
  modemDelay(1);
  delivered({"shared reader"});
  Serial.feed("usb\r\n");
  equal(readSerialLine(Serial, separate, sizeof(separate), pos), "usb", "other UART caller-buffer overload still functional");
  Serial.feed("debug\r\n");
  equal(readSerialLine(Serial), "debug", "other UART single-buffer overload still functional");

  resetState();
  // The UART supplies more bytes on every read. One poll must use its initial byte budget.
  Serial1.feed("A");
  Serial1.afterRead = [] { Serial1.feed("A"); };
  pollModem();
  require(Serial1.readCalls == 1 && Serial1.rx.size() == 1, "continuous ingress cannot make a single poll unbounded");
  Serial1.afterRead = nullptr;
  {
    ModemTransaction tx;
    require(tx.begin("AT"), "begin after initial ingress stops");
    Serial1.feed("A");
    Serial1.afterRead = [] { Serial1.feed("A"); };
    require(tx.waitForFinal(4) == ModemResult::Timeout && smsTestTicks == 4, "continuous ingress cannot starve timeout checks");
    Serial1.afterRead = nullptr;
    pump("\n");
    tx.end();
  }
}

static void apiAndSendFailures() {
  for (const std::string& command : std::vector<std::string>{"", "AT\r", "AT\n", "AT\x1A", "AT\x1B", std::string(257, 'A')}) {
    resetState();
    String response = "old", line = "old";
    require(runModemCommand(command.c_str(), 0, &response, &line) == ModemResult::Invalid, "invalid AT rejected");
    require(Serial1.tx.empty() && smsTestTicks == 0 && !modemBusy(), "invalid command has no UART/clock effect");
    require(response.length() == 0 && line.length() == 0, "invalid command clears output parameters");
  }
  resetState();
  require(runModemCommand(nullptr, 0) == ModemResult::Invalid && Serial1.tx.empty(), "null AT rejected");
  {
    ModemTransaction tx;
    require(!tx.submitPdu("00") && !tx.waitForPrompt(0), "unstarted API cannot submit/wait prompt");
    require(tx.waitForFinal(0) == ModemResult::Pending && Serial1.tx.empty(), "unstarted final wait has no effect");
  }
  const std::string longest(256, 'Q');
  Serial1.onWrite = [&](const std::string& text) { if (text == longest + "\r\n") Serial1.schedule(1, "OK\r\n"); };
  require(runModemCommand(longest.c_str(), 3) == ModemResult::Ok, "maximum valid command length allowed");

  for (bool finalTimeout : {false, true}) {
    resetState();
    Serial1.onWrite = [&](const std::string& text) {
      if (text.rfind("AT+CMGS=", 0) == 0) Serial1.schedule(1, "> ");
      else if (text == std::string(1, '\x1A')) {
        Serial1.schedule(1, cmt("final wait receive") + clip + "\r\nRING\r\n");
        Serial1.schedule(2, finalTimeout ? "+CMGS: 42\r\n" : "+CMS ERROR: 500\r\n");
      }
    };
    require(!sendSMS("15500000000", "final failure"), "real sendSMS propagates final error/timeout");
    require(smsTestTicks == 1 + (finalTimeout ? 30000 : 2) + MODEM_RECOVERY_MS, "real send uses 30s final budget or stops on error");
    require(sent(0x1A) == 1 && sent(0x1B) == 0 && !modemBusy(), "submitted SMS never double-sends on failure");
    delivered({"final wait receive"});
    require(callLines == std::vector<String>{clip, "RING"}, "final wait failure still delivers call events");
  }
}

static void cmtNumericResponses() {
  const auto input = incoming("numeric response gap");
  for (const auto& query : std::vector<std::pair<std::string, std::string>>{
       {"AT+CIMI", "460001234567890"}, {"AT+CGSN", "861234567890123"},
       {"AT+CCID", "89860123456789012345"}, {"AT+NUMERIC", std::string(input.size(), '1')},
       {"AT+NUMERIC", "07" + std::string(input.size() - 2, '1')}}) {
    for (bool orphanHeader : {false, true}) {
      resetState();
      Serial1.onWrite = [&](const std::string& text) {
        if (text == query.first + "\r\n") {
          Serial1.schedule(1, cmtHeader(input) + query.second + "\r\n");
          Serial1.schedule(2, clip + "\r\nRING\r\n" + (orphanHeader ? "" : input + "\r\n") + "OK\r\n");
        }
      };
      String response;
      require(runModemCommand(query.first.c_str(), 10, &response) == ModemResult::Ok, "numeric query still completes with a pending CMT");
      equal(response, "\r\n" + query.second + "\r\nOK\r\n", "IMSI/IMEI/numeric reply is not swallowed as PDU");
      delivered(orphanHeader ? std::vector<std::string>{} : std::vector<std::string>{"numeric response gap"});
      require(callLines == std::vector<String>{clip, "RING"}, "calls remain independent of CMT/AT ownership");
      require(pdu.decodeCalls == (orphanHeader ? 0 : 1), "only a structurally valid, declared-length PDU enters pdulib");
    }
  }
}

static void cmtNumericSplitPdu() {
  const auto input = incoming("split after numeric");
  for (size_t cut = 0; cut <= input.size(); cut++) {
    resetState();
    ModemTransaction tx;
    require(tx.begin("AT+CIMI"), "query starts before split CMT");
    pump(cmtHeader(input) + "460001234567890\r\n" + input.substr(0, cut));
    equal(tx.response(), "\r\n460001234567890\r\n", "numeric reply retained while PDU line is still incomplete");
    require(history.empty() && pdu.decodeCalls == 0, "PDU is never decoded before its line terminator");
    pump(input.substr(cut) + "\r\nOK\r\n");
    require(tx.waitForFinal(0) == ModemResult::Ok, "split CMT does not steal final OK");
    equal(tx.response(), "\r\n460001234567890\r\nOK\r\n", "PDU does not leak into the numeric query response");
    delivered({"split after numeric"});
  }
}

static void cmtExpiryDuringCommand() {
  constexpr unsigned long waitMs = 5000; // Independent red/green contract, not copied implementation logic.
  const auto input = incoming("late orphan");
  for (bool wrap : {false, true}) {
    for (const std::string& header : {cmtHeader(input), std::string("+CMT: ,bad\r\n"), std::string("+CMT: ,0\r\n")}) {
      resetState(); resetSmsHostClock(wrap ? ULONG_MAX - 100 : 1000);
      Serial1.onWrite = [&](const std::string& text) {
        if (text == "AT+CIMI\r\n") {
          Serial1.schedule(0, header);
          Serial1.schedule(waitMs - 1, "460001234567890\r\n");
          Serial1.schedule(waitMs, input + "\r\nOK\r\n");
        }
      };
      String response;
      require(runModemCommand("AT+CIMI", waitMs + 1, &response) == ModemResult::Ok, "query runs across CMT expiry including millis wrap");
      equal(response, "\r\n460001234567890\r\n" + input + "\r\nOK\r\n", "expired/malformed CMT owns neither the numeric response nor later bare hex");
      delivered({});
      require(!smsWaitingPdu && pdu.decodeCalls == 0, "orphan header cannot claim a delayed PDU forever");
      pump(cmt("fresh after expiry")); delivered({"fresh after expiry"});
      require(pdu.decodeCalls == 1 && !modemBusy(), "next valid CMT recovers without modem reset or extra AT");
    }
    resetState(); resetSmsHostClock(wrap ? ULONG_MAX - 100 : 1000);
    ModemTransaction tx;
    require(tx.begin("AT+WAIT"), "begin a query before a silent CMT wait");
    pump(cmtHeader(input));
    Serial1.schedule(waitMs, input + "\r\nOK\r\n");
    require(tx.waitForFinal(waitMs + 1) == ModemResult::Ok, "query survives a CMT with no intervening response");
    equal(tx.response(), "\r\n" + input + "\r\nOK\r\n", "CMT expires by time alone, not just after a non-PDU line");
    require(history.empty() && pdu.decodeCalls == 0 && !smsWaitingPdu, "silently missing PDU cannot keep the old header alive");
  }
}

static size_t idleServiceCalls = 0;
static void idleServiceProbe() {
  ++idleServiceCalls;
  if (idleServiceCalls == 1) {
    require(modemBusy(), "idle hook reports busy even outside a transaction");
    ModemTransaction nested;
    require(!nested.acquired(), "idle service cannot run nested AT commands");
    modemDelay(1); // Recursive idle must not recursively call the service.
  }
}
static void idleServiceTests() {
  resetState(); idleServiceCalls = 0; setModemIdleHook(idleServiceProbe);
  Serial1.onWrite = [](const std::string& text) {
    if (text == "AT+WAIT\r\n") {
      Serial1.schedule(10000, cmt("during long wait"));
      Serial1.schedule(35000, "OK\r\n");
    }
  };
  require(runModemCommand("AT+WAIT", 35001) == ModemResult::Ok, "35-second AT wait succeeds");
  require(idleServiceCalls >= 34998 && !modemBusy(), "service runs throughout wait and releases guard");
  delivered({"during long wait"});
  idleServiceCalls = 0; modemDelay(5);
  require(idleServiceCalls > 0 && !modemBusy(), "plain modemDelay also services without allowing AT reentry");
  const auto before = idleServiceCalls; setModemIdleHook(nullptr); modemDelay(5);
  require(idleServiceCalls == before, "idle service can be disabled");
}

int main() {
  const std::vector<std::pair<const char*, std::function<void()>>> suites = {
    {"scheduled AT/echo smoke", querySmoke},
    {"real sendSMS snapshot + interleaved CMT/CLIP/RING", snapshotSmoke},
    {"CLIP status and CLCC query rows remain responses without call notifications", callQueryResponses},
    {"call queries preserve interleaved CLIP/CLCC/RING/CMT and post-OK calls", callQueryInterleaving},
    {"real call parsing/dedup stays active outside matching queries and during SMS", callUrcRouting},
    {"CLCC ownership respects partial generations, terminals, timeout and recovery", clccQueryBoundaries},
    {"every RX byte split across begin/end/main-loop/generations", rxBoundaries},
    {"CMGS numeric receipt AND complete OK, both orders", cmgsReceipts},
    {"old partial/complete responses, echo and pre-submit terminals", staleResponses},
    {"pre-submit partial OK spanning Ctrl+Z must stay stale", [] { preSubmitPartialFinal(true); }},
    {"pre-submit partial CMGS spanning Ctrl+Z must stay stale", [] { preSubmitPartialFinal(false); }},
    {"queued complete/partial finals before submit must stay stale", preSubmitQueuedFinals},
    {"URC callback injects old complete/partial finals during submit", preSubmitCallbackFinals},
    {"URC callback injects old finals during command begin", beginCallbackFinals},
    {"bounded RX synchronization rejects residual input, including millis wrap", rxSyncBudget},
    {"queued and partial CMT/PDU/call URCs across submit", submitBoundaryUrc},
    {"queued/callback pre-submit errors abort input while dispatching URCs", preSubmitQueuedErrors},
    {"NO CARRIER call-end URC preserves SMS/query and real terminal errors", callEndUrc},
    {"prompt detection, timeout, late prompt ESC and recovery", promptsAndRecovery},
    {"terminal errors, short writes, invalid PDU and real prompt timeout", errorsAndWrites},
    {"nested transaction and reentrant URC reader exclusion", nestedBusy},
    {"overlong/NUL whole-line discard and next-line recovery", lostLines},
    {"response framing at/beyond cap and independent URC burst budget", responseLimits},
    {"MPING result AND OK, order independence and late async isolation", mpingResponses},
    {"32-bit millis wrap, watchdog, shared loop reader and bounded polls", clockAndSharedReader},
    {"API validation and real sendSMS final error/30s timeout", apiAndSendFailures},
    {"CMT does not swallow IMSI/IMEI/other numeric AT responses", cmtNumericResponses},
    {"numeric response between CMT and every split of its PDU", cmtNumericSplitPdu},
    {"CMT declared wait expiry during commands and 32-bit wrap recovery", cmtExpiryDuringCommand},
    {"AT wait services idle hook without AT reentry or URC loss", idleServiceTests}
  };
  for (const auto& suite : suites) {
    try { suite.second(); std::cout << "PASS: " << suite.first << '\n'; }
    catch (const std::exception& error) {
      failures++;
      std::cerr << "FAIL: " << suite.first << ": " << error.what() << '\n' << Serial.tx;
    }
  }
  std::cout << checks << " assertions (" << checks - failedChecks << " passed, " << failedChecks << " failed), "
            << suites.size() << " suites, " << failures << " failures.\n";
  return failures ? 1 : 0;
}
