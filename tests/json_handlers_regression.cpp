// P2-04/P2-05: execute the shared JsonInput and exact extracted Web/MQTT handlers.
// Persistence, timer APIs and root loading/rendering are production code too; only I/O is fake.
#include "json_handlers_host_stubs.h"
#include "json_input.h"
#include "config.h"
#include "modem_handler.h"
#include "web_stream.h"
#include "web_output.h"
#include "web_pages.h"
#include <iostream>
#include <numeric>

#define WIFI_SSID "synthetic-wifi"
#define WIFI_PASS "synthetic-pass"
Config config;
Preferences preferences;
Statistics stats;
SmsRecord smsHistory[MAX_SMS_HISTORY];
bool configValid = true;
uint64_t lastTimerExec = 0, timerIntervalSec = 0;
static SemaphoreHandle_t statsMutex = nullptr;
WebServer server(80);
String mqttTopicSmsSend = "test/sms/send", mqttTopicPing = "test/ping", mqttTopicCmd = "test/cmd";
String mqttTopicStatus = "test/status", mqttHaStatusTopic = "test/ha/state";
String mqttTopicSmsReceived = "test/sms/received", mqttTopicCallReceived = "test/call/received";

struct SmsEvent { std::string phone, message; bool success; };
struct PingEvent { std::string host, result; bool success; };
struct Publication { std::string topic, payload; bool retained; };
static std::vector<SmsEvent> sends, sentResults;
static std::vector<PingEvent> pingResults;
static std::vector<Publication> publications;
static std::vector<std::string> errors, atCommands;
static bool sendResult = true;
static unsigned int statsSaves = 0;
static bool testModemBusy = false;
static std::function<void()> duringSend;
bool modemBusy() { return testModemBusy; }

void initStatsLock() { statsMutex = xSemaphoreCreateMutex(); }
void loadStats() {}
void saveStats() { statsSaves++; testEffects.push_back("stats"); }
bool sendSMS(const char* phone, const char* message) {
  sends.push_back({phone, message, sendResult}); testEffects.push_back("send");
  if (duringSend) duringSend();
  return sendResult;
}
void publishMqttSmsSent(const char* phone, const char* message, bool success) {
  sentResults.push_back({phone, message, success}); testEffects.push_back(success ? "sms-ok" : "sms-fail");
}
void publishMqttPingResult(const char* host, bool success, const char* result) { pingResults.push_back({host, result, success}); }
void publishErrorLog(const char*, const char*, const char*, const char* error, unsigned long = 0) { errors.emplace_back(error); }
void publishTimingLog(const char*, const char*, unsigned long, bool) {}
void publishDeviceLog(const char*, const char* module, const char* message) {
  // Observe completion of the real status helper without simulating its publications.
  const std::string prefix = "MQTT发布状态: ";
  const std::string text = message;
  if (std::strcmp(module, "mqtt") == 0 && text.rfind(prefix, 0) == 0) {
    testEffects.push_back("status:" + text.substr(prefix.size()));
  }
}
String buildMqttStatusJson(const char* status) { return String("{\"status\":\"") + status + "\"}"; }
static bool publishMqttPayload(const char* topic, const char* payload, bool retained = false) {
  publications.push_back({topic, payload, retained}); return true;
}
String sendATCommand(const char* command, unsigned long) { atCommands.emplace_back(command); return "OK"; }
void modemDelay(unsigned long amount) { delay(amount); }
ModemResult runModemCommand(const char* command, unsigned long, String*, String* result, const char*) {
  atCommands.emplace_back(command);
  if (result) *result = "+MPING: 0,192.0.2.1,32,1,64";
  return ModemResult::Ok;
}

#include "json_handlers_actual.inc"

static_assert(std::is_same<decltype(timerIntervalSec), uint64_t>::value, "period is 64-bit");
static_assert(std::is_same<decltype(lastTimerExec), uint64_t>::value, "origin is 64-bit");
static int checks = 0;
static void require(bool ok, const std::string& message) {
  checks++;
  if (!ok) throw std::runtime_error(message);
}
static std::string text(const String& value) { return std::string(value.c_str(), value.length()); }
static std::string object(const std::vector<std::string>& fields) {
  std::string result = "{";
  for (const auto& field : fields) { if (result.size() > 1) result += ','; result += field; }
  return result + "}";
}
static std::string extend(const std::string& json, const std::string& field) {
  return json.substr(0, json.size() - 1) + (json.size() > 2 ? "," : "") + field + "}";
}
static std::string padded(const std::string& json, size_t length) {
  std::string result = extend(json, "\"padding\":\"\"");
  if (result.size() > length) throw std::runtime_error("bad padding fixture");
  result.insert(result.size() - 2, length - result.size(), 'x');
  return result;
}
static std::string list(size_t count) {
  std::string result = "[";
  for (size_t i = 0; i < count; ++i) { if (i) result += ','; result += '"' + std::to_string(i) + '"'; }
  return result + "]";
}
static void permutations(const std::vector<std::string>& fields, const std::function<void(const std::string&)>& test) {
  std::vector<size_t> order(fields.size()); std::iota(order.begin(), order.end(), 0);
  do {
    std::vector<std::string> ordered;
    for (size_t i : order) ordered.push_back(fields[i]);
    test(object(ordered));
  } while (std::next_permutation(order.begin(), order.end()));
}
static bool sameConfig(const Config& a, const Config& b) {
  for (int i = 0; i < MAX_WIFI_NETWORKS; ++i) {
    if (a.wifiNetworks[i].ssid != b.wifiNetworks[i].ssid || a.wifiNetworks[i].password != b.wifiNetworks[i].password ||
        a.wifiNetworks[i].enabled != b.wifiNetworks[i].enabled) return false;
  }
  for (int i = 0; i < MAX_PUSH_CHANNELS; ++i) {
    const auto& x = a.pushChannels[i]; const auto& y = b.pushChannels[i];
    if (x.enabled != y.enabled || x.type != y.type || x.name != y.name || x.url != y.url ||
        x.key1 != y.key1 || x.key2 != y.key2 || x.customBody != y.customBody) return false;
  }
  return a.smtpServer == b.smtpServer && a.smtpPort == b.smtpPort && a.smtpUser == b.smtpUser &&
    a.smtpPass == b.smtpPass && a.smtpSendTo == b.smtpSendTo && a.emailEnabled == b.emailEnabled &&
    a.webUser == b.webUser && a.webPass == b.webPass && a.timerEnabled == b.timerEnabled &&
    a.timerType == b.timerType && a.timerInterval == b.timerInterval && a.timerPhone == b.timerPhone &&
    a.timerMessage == b.timerMessage && a.mqttEnabled == b.mqttEnabled && a.mqttControlOnly == b.mqttControlOnly &&
    a.mqttServer == b.mqttServer && a.mqttPort == b.mqttPort && a.mqttUser == b.mqttUser && a.mqttPass == b.mqttPass &&
    a.mqttPrefix == b.mqttPrefix && a.mqttHaDiscovery == b.mqttHaDiscovery && a.mqttHaPrefix == b.mqttHaPrefix &&
    a.filterEnabled == b.filterEnabled && a.filterIsWhitelist == b.filterIsWhitelist && a.filterList == b.filterList &&
    a.contentFilterEnabled == b.contentFilterEnabled && a.contentFilterIsWhitelist == b.contentFilterIsWhitelist &&
    a.contentFilterList == b.contentFilterList;
}
static void resetState() {
  ConfigLockFaults::reset();
  StringFaults::reset(); config = Config{}; stats = Statistics{}; stats.smsSent = 7;
  config.webUser = "saved-admin"; config.webPass = "synthetic-web-secret";
  config.smtpPass = "synthetic-smtp-secret"; config.mqttPass = "synthetic-mqtt-secret";
  config.wifiNetworks[0].ssid = "saved-wifi"; config.wifiNetworks[0].password = "synthetic-wifi-secret";
  config.timerEnabled = true; config.timerType = 1; config.timerInterval = 50;
  config.timerPhone = "old-phone"; config.timerMessage = " old message ";
  config.filterEnabled = true; config.filterIsWhitelist = false; config.filterList = "old-numbers";
  config.contentFilterEnabled = false; config.contentFilterIsWhitelist = true; config.contentFilterList = "old-keywords";
  config.mqttEnabled = true; config.mqttHaDiscovery = true;
  preferences = Preferences{}; statsMutex = nullptr; statsSaves = 0; configValid = true;
  testClockMs = (1ULL << 33) + 12345; lastTimerExec = 44; timerIntervalSec = 555;
  server.reset(); Serial.log.clear(); ESP.restarts = 0; WiFi.modeValue = WIFI_STA;
  pendingSmsCount = 0; for (auto& item : pendingSmsQueue) { item.phone = ""; item.message = ""; }
  mqttPendingPing = false; mqttPendingPingHost = "";
  mqttPendingRestart = mqttCommandsBusy = testModemBusy = false; duringSend = nullptr;
  sends.clear(); sentResults.clear(); pingResults.clear(); publications.clear(); errors.clear(); atCommands.clear();
  testEffects.clear(); sendResult = true;
}
using Handler = void (*)();
static void webRequest(Handler handler, const std::string& payload) { server.args = {{"plain", String(payload)}}; handler(); }
static void mqttRequest(const String& topic, const std::string& payload) {
  std::vector<byte> bytes(payload.begin(), payload.end()); // Deliberately no terminating NUL.
  mqttCallback(const_cast<char*>(topic.c_str()), bytes.data(), static_cast<unsigned int>(bytes.size()));
}
static void expectWebReply(int status, bool success) {
  require(server.statusCode == status && server.contentType == "application/json", "HTTP status and JSON MIME");
  JsonInput reply; bool actual = !success;
  require(reply.parse(String(server.body)) && reply.getBool("success", actual) && actual == success, "explicit JSON success flag");
  if (!success) {
    String reason;
    require(reply.getString("message", reason) && reason.length() > 0, "HTTP error explains rejection");
  }
}
static void noWebEffects(const Config& before) {
  require(sameConfig(config, before), "failed input cannot modify any config field");
  require(lastTimerExec == 44 && timerIntervalSec == 555 && configValid, "failed input cannot reset timer/config validity");
  require(sends.empty() && atCommands.empty() && preferences.saves == 0 && statsSaves == 0 && stats.smsSent == 7,
          "failed input cannot send, save or count");
  require(pendingSmsCount == 0 && !mqttPendingPing && ESP.restarts == 0, "failed Web command has no other side effects");
}
static void webReject(Handler handler, const std::string& payload) {
  resetState(); const Config before = config; webRequest(handler, payload);
  expectWebReply(400, false); noWebEffects(before);
}
static void mqttReject(const String& topic, const std::string& payload) {
  resetState(); const Config before = config; mqttRequest(topic, payload);
  require(!errors.empty(), "MQTT rejection is logged");
  require(pendingSmsCount == 0 && !mqttPendingPing && sends.empty() && atCommands.empty() && ESP.restarts == 0,
          "MQTT invalid input neither queues nor executes");
  require(sameConfig(config, before) && preferences.saves == 0 && statsSaves == 0 && stats.smsSent == 7, "MQTT invalid input leaves config/stats unchanged");
  if (topic == mqttTopicSmsSend) require(sentResults.size() == 1 && !sentResults[0].success, "invalid MQTT SMS returns false");
  if (topic == mqttTopicPing) require(pingResults.size() == 1 && !pingResults[0].success, "invalid MQTT ping returns false");
}

static const std::string escapedBody = R"json(  \t你好\u0041\uD83D\uDE00 \"quoted\" \\ \/\r\n尾  )json";
// Keep the non-BMP fixture ASCII-escaped so source edits cannot drop the character.
static const std::string decodedBody = u8"  \t你好A\U0001F600 \"quoted\" \\ /\r\n尾  ";
static const std::string webSms = R"({"phone":"10000","content":"text"})";
static const std::string mqttSms = R"({"phone":"10000","message":"text"})";
static const std::string timerJson = R"({"enabled":true,"type":1,"interval":50,"phone":"10000","message":"text"})";
static const std::string filterJson = R"({"enabled":false,"whitelist":true,"numbers":["10000"]})";
static const std::string contentJson = R"({"enabled":true,"whitelist":false,"keywords":"otp"})";
static std::string timerReply(uint64_t seconds) {
  return "{\"success\":true,\"message\":\"定时任务已保存\",\"remain\":" + std::to_string(seconds) + "}";
}

static void parserTests() {
  JsonInput input; String out = "keep"; bool flag = false; int number = 0;
  require(input.parse(String("{\"s\":\"") + escapedBody.c_str() + "\",\"b\":true,\"n\":-2147483648,\"a\":[\"a,b\",\"\",\"\\\"q\\\"\"]}"), "full parser fixture");
  require(input.getString("s", out) && text(out) == decodedBody, "Unicode, surrogate pair and all body whitespace decoded");
  require(input.getBool("b", flag) && flag && input.getInt("n", number) && number == INT_MIN, "strict scalars including INT_MIN");
  require(input.getStringList("a", out) && out == "a,b,,\"q\"", "array joins strings without reparsing quotes");
  require(input.getStringList("s", out) && text(out) == decodedBody, "string-list also accepts strings");
  out = "keep"; flag = false; number = 123;
  require(!input.getString("b", out) && out == "keep" && !input.getBool("n", flag) && !flag &&
          !input.getInt("s", number) && number == 123 && !input.getStringList("b", out), "no coercion; wrong getters preserve output");
  require(input.has("a") && !input.has("missing") && !input.has(nullptr), "has and missing field behavior");
  String source = R"({"s":"owned"})"; require(input.parse(source), "owned source parse"); source = "changed";
  require(input.getString("s", out) && out == "owned", "parser does not retain input references");
  require(input.parse(R"({"":"","s":"","a":[],"b":false,"i":2147483647})") && input.has("") &&
          input.getString("", out) && out.length() == 0 && input.getStringList("a", out) && out.length() == 0 &&
          input.getBool("b", flag) && !flag && input.getInt("i", number) && number == INT_MAX, "empty values/keys, false and INT_MAX");
  require(input.parse(R"({"s":"\b\f\n\r\t\/\\\""})") && input.getString("s", out) && out == "\b\f\n\r\t/\\\"", "all JSON escapes");
  require(input.parse(R"({"s":"\u0001\u007f\u0080\u07ff\u0800\ud7ff\ue000\uffff\udbff\udfff"})"), "valid Unicode boundary codepoints");

  std::vector<std::string> invalid = {
    "", " ", "[]", "null", "{", "{}{}", "{}tail", "{\"a\":1,}", "{,\"a\":1}", "{'a':1}",
    R"({"x":null})", R"({"x":{}})", R"({"x":[[]]})", R"({"x":[1]})", R"({"x":[true]})", R"({"x":["a",]})",
    R"({"x":True})", R"({"x":truefalse})", R"({"x":false0})", R"({"x":01})", R"({"x":-01})", R"({"x":+1})",
    R"({"x":1.0})", R"({"x":1e1})", R"({"x":-})", R"({"x":2147483648})", R"({"x":-2147483649})",
    R"({"x":999999999999999999999})", R"({"x":"a","x":"b"})", R"({"x":1,"\u0078":2})", R"({"":1,"":2})",
    u8R"({"é":1,"\u00e9":2})", R"({"x":"\q"})", R"({"x":"\u0000"})", R"({"x":"\u12"})", R"({"x":"\uZZZZ"})",
    R"({"x":"\ud800"})", R"({"x":"\udc00"})", R"({"x":"\ud800\u0041"})", R"({"x":"\udc00\ud800"})",
    R"({"x":"\udbff\udfff\ud800"})", "{\"x\":\"raw\nline\"}", "{\"x\":\"raw\tvalue\"}", "{\"x\":\"trailing\\"
  };
  for (const std::string& bytes : {std::string("\xc0\xaf"), std::string("\x80"), std::string("\xed\xa0\x80"),
       std::string("\xf4\x90\x80\x80"), std::string("\xf0\x80\x80\x80"), std::string("\xe2\x82"),
       std::string("\xc2x"), std::string("\xff"), std::string(1, '\0')}) {
    invalid.push_back("{\"x\":\"" + bytes + "\"}");
  }
  invalid.push_back(std::string("{}\0", 3));
  for (const auto& value : invalid) {
    require(input.parse(R"({"prior":"ok","flag":true,"i":5,"list":["a"]})"), "prime parser state");
    require(!input.parse(String(value)), "reject malformed JSON/Unicode: " + value);
    out = "keep"; flag = false; number = 99;
    require(!input.has("prior") && !input.getString("prior", out) && out == "keep" && !input.getBool("flag", flag) &&
            !input.getInt("i", number) && number == 99 && !input.getStringList("list", out), "failure invalidates entire prior parse");
  }
  require(input.parse(String(padded("{}", JsonInput::MAX_INPUT_BYTES))), "8192 byte input accepted");
  require(!input.parse(String(padded("{}", JsonInput::MAX_INPUT_BYTES + 1))), "8193 byte input rejected");
  std::vector<std::string> fields;
  for (int i = 0; i < 16; ++i) fields.push_back('"' + std::to_string(i) + "\":1");
  require(input.parse(String(object(fields))), "16 fields accepted"); fields.push_back("\"extra\":1");
  require(!input.parse(String(object(fields))), "17 fields rejected");
  require(input.parse(String("{\"a\":" + list(100) + "}")) && input.getStringList("a", out), "100 array items accepted");
  require(!input.parse(String("{\"a\":" + list(101) + "}")), "101 array items rejected");
  StringFaults::reserveAfter = 0; require(!input.parse("{}") && !input.has("a"), "parser reserve failure is fatal");
  StringFaults::concatAfter = 0; require(!input.parse(R"({"x":"a"})"), "parser append failure is fatal");
  require(input.parse(R"({"x":""})"), "empty getter OOM fixture"); out = "keep";
  StringFaults::reserveAfter = 0; require(!input.getString("x", out) && out == "keep", "empty getter cannot hide OOM");
  StringFaults::reset();
}

static void smsTests() {
  permutations({R"("ph\u006fne":" 10000 \t")", "\"content\":\"" + escapedBody + "\"",
                R"("unknown":["phone","a,b","\"quoted\""])"}, [](const std::string& json) {
    resetState(); webRequest(handleSendSms, json); expectWebReply(200, true);
    require(sends.size() == 1 && sends[0].phone == "10000" && sends[0].message == decodedBody, "Web SMS field order, escapes, body whitespace");
    require(stats.smsSent == 8 && statsSaves == 1 && preferences.saves == 0, "successful Web SMS counts once");
  });
  resetState(); webRequest(handleSendSms, R"({"phone":"10000","content":" \t\r\n "})");
  expectWebReply(200, true); require(sends[0].message == " \t\r\n ", "whitespace-only Web body is not trimmed away");
  resetState(); sendResult = false; webRequest(handleSendSms, webSms); expectWebReply(200, false);
  require(stats.smsSent == 7 && statsSaves == 0, "send failure is not counted");
  for (const std::string json : {R"({})", R"({"phone":"10000"})", R"({"phone":1,"content":"x"})",
       R"({"phone":"","content":"x"})", R"({"phone":" \t\r\n ","content":"x"})", R"({"phone":"10000","content":""})",
       R"({"phone":"10000","content":false})", R"({"phone":"10000","content":["x"]})"}) webReject(handleSendSms, json);

  permutations({"\"message\":\"" + escapedBody + "\"", R"("phone":" \t10000 ")", R"("unknown":"after, \"message\"")"}, [](const std::string& json) {
    resetState(); mqttRequest(mqttTopicSmsSend, json);
    require(pendingSmsCount == 1 && pendingSmsQueue[0].phone == "10000" && text(pendingSmsQueue[0].message) == decodedBody,
            "MQTT SMS arbitrary field order preserves exact decoded body");
    require(sends.empty() && atCommands.empty() && sentResults.empty(), "callback only enqueues");
    processMqttPendingCommands();
    require(sends.size() == 1 && sends[0].message == decodedBody && sentResults.size() == 1 && sentResults[0].success &&
            sentResults[0].message == decodedBody && stats.smsSent == 8 && statsSaves == 1, "real pending drain sends/counts/publishes once");
    require(testEffects == std::vector<std::string>({"send", "stats", "sms-ok"}), "pending result order unchanged");
  });
  resetState(); mqttRequest(mqttTopicSmsSend, R"({"phone":"10000","message":" \t\n "})");
  require(pendingSmsCount == 1 && pendingSmsQueue[0].message == " \t\n ", "MQTT whitespace-only message retained");
  sendResult = false; processMqttPendingCommands();
  require(sentResults.size() == 1 && !sentResults[0].success && stats.smsSent == 7 && statsSaves == 0, "pending send failure preserved");
  for (const std::string json : {R"({})", R"({"phone":"10000"})", R"({"phone":"","message":"x"})",
       R"({"phone":" \t ","message":"x"})", R"({"phone":10000,"message":"x"})", R"({"phone":"10000","message":""})",
       R"({"phone":"10000","message":true})", R"({"phone":"10000","message":["x"]})"}) mqttReject(mqttTopicSmsSend, json);
  resetState();
  for (unsigned int i = 0; i < MAX_PENDING_SMS; ++i) mqttRequest(mqttTopicSmsSend, "{\"phone\":\"10000\",\"message\":\" " + std::to_string(i) + " \"}");
  mqttRequest(mqttTopicSmsSend, mqttSms);
  require(pendingSmsCount == MAX_PENDING_SMS && sends.empty() && sentResults.size() == 1 && !sentResults[0].success &&
          sentResults[0].phone == "10000" && sentResults[0].message == "text" && !errors.empty(), "full queue reports false without overwrite");
  for (unsigned int i = 0; i < MAX_PENDING_SMS; ++i) {
    processMqttPendingCommands(); require(sends.size() == i + 1 && sends.back().message == " " + std::to_string(i) + " ", "one FIFO SMS drained per loop");
  }
  processMqttPendingCommands(); require(sends.size() == MAX_PENDING_SMS, "empty queue does not resend");

  resetState();
  for (unsigned int i = 0; i < MAX_PENDING_SMS; ++i) mqttRequest(mqttTopicSmsSend, "{\"phone\":\"10000\",\"message\":\"move " + std::to_string(i) + "\"}");
  StringFaults::copyAfter = 0;
  processMqttPendingCommands();
  StringFaults::reset();
  require(sends.size() == 1 && sends[0].message == "move 0" && pendingSmsCount == 3 &&
          pendingSmsQueue[0].message == "move 1" && pendingSmsQueue[2].message == "move 3", "dequeue/shift performs no fallible String copy");
  require(pendingSmsQueue[3].phone.length() == 0 && pendingSmsQueue[3].message.length() == 0, "consumed tail storage released");
  duringSend = [] {
    mqttRequest(mqttTopicSmsSend, R"({"phone":"10001","message":"new while sending"})");
    processMqttPendingCommands(); // Must not execute nested work from an idle callback.
  };
  processMqttPendingCommands(); duringSend = nullptr;
  require(sends.size() == 2 && sends.back().message == "move 1" && pendingSmsCount == 3 &&
          pendingSmsQueue[2].message == "new while sending", "waiting callback enqueue preserves FIFO and refuses reentrant drain");
  testModemBusy = true; processMqttPendingCommands();
  require(sends.size() == 2 && pendingSmsCount == 3, "busy modem does not consume pending command");
  testModemBusy = false;
  for (int i = 0; i < 3; ++i) processMqttPendingCommands();
  require(sends.size() == 5 && sends.back().message == "new while sending", "reentrant enqueue drained once after original FIFO");
}

static void timerTests() {
  permutations({R"("enabled":true)", R"("type":1)", R"("interval":2147483647)", R"("phone":" 10000 ")",
                "\"message\":\"" + escapedBody + "\""}, [](const std::string& json) {
    resetState();
    preferences.beforeSave = [] {
      require(config.timerEnabled && config.timerType == 1 && config.timerInterval == INT_MAX && config.timerPhone == "10000" &&
              text(config.timerMessage) == decodedBody, "all timer fields applied before persistence");
      require(lastTimerExec == testClockMs && timerIntervalSec == uint64_t(INT_MAX) * 86400ULL, "real reset precedes persistence and retains 64 bits");
    };
    webRequest(handleTimer, json);
    require(server.statusCode == 200 && server.contentType == "application/json", "timer returns HTTP 200 JSON");
    // The input subset intentionally rejects ints wider than int; verify the wide response in full.
    require(server.body == timerReply(uint64_t(INT_MAX) * 86400ULL), "remain serializes all 64 bits");
    require(preferences.saves == 1 && sends.empty(), "timer save does not execute it");
  });
  for (const auto& json : {R"({"enabled":true,"type":0,"interval":1,"phone":"","message":""})",
                          R"({"enabled":false,"type":0,"interval":60,"phone":"","message":""})"}) {
    resetState(); webRequest(handleTimer, json); expectWebReply(200, true);
    require(config.timerType == 0 && config.timerPhone.length() == 0 && config.timerMessage.length() == 0, "ping timer allows empty SMS fields");
    // Legacy /timer reports a full period even when disabled; the root countdown is separate.
    require(server.body == timerReply(config.timerEnabled ? 86400 : 5184000), "disabled /timer remain retains the full configured period");
    require(timerRemainingSeconds() == (config.timerEnabled ? timerIntervalSec : 0), "root countdown still reports zero when disabled");
  }
  resetState(); webRequest(handleTimer, R"({"enabled":false,"type":1,"interval":1,"phone":" 10000 ","message":" \t "})");
  expectWebReply(200, true); require(config.timerMessage == " \t " && !config.timerEnabled, "disabled SMS timer preserves whitespace body");
  resetState(); webRequest(handleTimer, R"({"enabled":true,"message":" \t\r\n "})");
  expectWebReply(200, true);
  require(config.timerType == 1 && config.timerPhone == "old-phone" && config.timerMessage == " \t\r\n ", "enabled SMS retains omitted type/phone and accepts a nonempty whitespace-only body");

  const std::vector<std::string> fields = {R"("enabled":true)", R"("type":1)", R"("interval":50)", R"("phone":"10000")", R"("message":"text")"};
  // Optional fields are patches, not required replacements: cover every presence combination.
  for (bool enabled : {false, true}) for (unsigned int mask = 0; mask < 16; ++mask) {
    resetState(); config.timerType = 0; config.timerInterval = 60; config.timerPhone = " \told-phone \r\n ";
    lastTimerExec = (1ULL << 32) + 77;
    Config expected = config; expected.timerEnabled = enabled;
    if (mask & 1) expected.timerType = 1;
    if (mask & 2) expected.timerInterval = 50;
    if (mask & 4) expected.timerPhone = "10000";
    if (mask & 8) expected.timerMessage = "text";
    std::vector<std::string> chosen = {enabled ? R"("enabled":true)" : R"("enabled":false)"};
    for (size_t i = 0; i < 4; ++i) if (mask & (1U << i)) chosen.push_back(fields[i + 1]);
    const uint64_t seconds = uint64_t(expected.timerInterval) * 86400ULL;
    preferences.beforeSave = [&] {
      require(sameConfig(config, expected), "partial update applies only provided fields and enabled before persistence");
      require(lastTimerExec == testClockMs && lastTimerExec > UINT32_MAX && timerIntervalSec == seconds, "partial update resets the real 64-bit schedule before persistence");
    };
    webRequest(handleTimer, object(chosen)); expectWebReply(200, true);
    require(sameConfig(config, expected), "omitted fields, original text whitespace and unrelated config stay intact");
    require(lastTimerExec == testClockMs && timerIntervalSec == seconds &&
            server.body == timerReply(seconds), "partial update reports a full period without truncating the new origin");
    require(preferences.saves == 1 && testEffects == std::vector<std::string>({"save"}) && configValid &&
            sends.empty() && atCommands.empty() && statsSaves == 0 && stats.smsSent == 7 &&
            pendingSmsCount == 0 && !mqttPendingPing && ESP.restarts == 0, "partial timer save has no unrelated effects or execution");
  }

  // Nonpositive native ints are valid legacy input and normalize, unlike wrong types/overflow.
  for (bool enabled : {false, true}) for (int interval : {0, -1, INT_MIN}) {
    resetState(); Config expected = config; expected.timerEnabled = enabled; expected.timerInterval = 1;
    webRequest(handleTimer, object({enabled ? R"("enabled":true)" : R"("enabled":false)", "\"interval\":" + std::to_string(interval)}));
    expectWebReply(200, true);
    require(sameConfig(config, expected) && preferences.saves == 1, "0/-1/INT_MIN normalize to one day without replacing omitted fields");
    require(lastTimerExec == testClockMs && timerIntervalSec == 86400ULL && server.body == timerReply(86400), "normalized interval uses the same 64-bit reset and full-period response");
  }
  for (bool enabled : {false, true}) {
    resetState(); const uint64_t origin = testClockMs, seconds = uint64_t(INT_MAX) * 86400ULL;
    preferences.beforeSave = [] { testClockMs += 2500; };
    webRequest(handleTimer, object({enabled ? R"("enabled":true)" : R"("enabled":false)", R"("interval":2147483647)"}));
    require(server.statusCode == 200 && server.contentType == "application/json" &&
            server.body == timerReply(seconds), "wide /timer remain is the complete period regardless of enabled state or save duration");
    require(lastTimerExec == origin && timerIntervalSec == seconds && timerRemainingSeconds() == (enabled ? seconds - 2 : 0), "root helper alone accounts for elapsed time/disabled state; save does not reset origin again");
  }

  // Empty SMS fields must be savable while disabled, so disabling never needs an SMS payload.
  for (const auto& json : {R"({"enabled":false,"type":1,"interval":1,"phone":"","message":""})",
                          R"({"enabled":false,"phone":" \t\r\n ","message":""})"}) {
    resetState(); webRequest(handleTimer, json); expectWebReply(200, true);
    require(!config.timerEnabled && config.timerType == 1 && config.timerPhone.length() == 0 && config.timerMessage.length() == 0 &&
            preferences.saves == 1 && lastTimerExec == testClockMs, "disabled SMS accepts explicit empty/trim-empty fields and saves once");
  }
  for (int emptyField : {0, 1, 2}) {
    resetState();
    if (emptyField < 2) config.timerPhone = emptyField == 0 ? "" : " \t\r\n\f\v ";
    else config.timerMessage = "";
    const Config before = config;
    webRequest(handleTimer, R"({"enabled":true})"); expectWebReply(400, false); noWebEffects(before);
    server.reset(); webRequest(handleTimer, R"({"enabled":false})"); expectWebReply(200, true);
    Config expected = before; expected.timerEnabled = false;
    require(sameConfig(config, expected) && preferences.saves == 1 && server.body == timerReply(4320000), "enabled rejects missing/trim-empty saved SMS data, but disabled preserves it unchanged");
  }

  const std::vector<std::vector<std::string>> bad = {
    {R"("enabled":"true")", R"("enabled":1)", R"("enabled":null)", R"("enabled":[])"},
    {R"("type":2)", R"("type":-1)", R"("type":"1")", R"("type":true)", R"("type":[])", R"("type":2147483648)"},
    {R"("interval":"50")", R"("interval":1.5)", R"("interval":1e2)", R"("interval":2147483648)",
     R"("interval":-2147483649)", R"("interval":true)", R"("interval":null)", R"("interval":[])"},
    {R"("phone":"")", R"("phone":" \t\n ")", R"("phone":10000)", R"("phone":false)", R"("phone":[])", R"("phone":null)"},
    {R"("message":"")", R"("message":false)", R"("message":[])", R"("message":123)", R"("message":null)"}
  };
  for (size_t i = 0; i < fields.size(); ++i) {
    // Only enabled remains required; optional omissions are positive cases above, not type coercions.
    auto changed = fields; changed.erase(changed.begin() + i);
    if (i == 0) webReject(handleTimer, object(changed));
    for (const auto& invalid : bad[i]) { changed = fields; changed[i] = invalid; webReject(handleTimer, object(changed)); }
  }
  for (const auto& invalid : {R"("type":2)", R"("type":"1")", R"("interval":false)", R"("interval":2147483648)",
                              R"("phone":10000)", R"("message":false)"}) {
    webReject(handleTimer, object({R"("enabled":false)", invalid}));
  }
  for (const auto& json : {R"({})", R"([{"enabled":false}])", R"({"enabled":false}tail)", R"({"enabled":false,"bad":"\q"})"}) {
    webReject(handleTimer, json);
  }
  resetState(); const Config before = config;
  const uint64_t priorOrigin = (1ULL << 33) + 42, priorSeconds = uint64_t(INT_MAX) * 86400ULL;
  lastTimerExec = priorOrigin; timerIntervalSec = priorSeconds;
  webRequest(handleTimer, R"({"enabled":false,"interval":0,"phone":"new-phone","message":false})"); expectWebReply(400, false);
  require(sameConfig(config, before) && configValid && lastTimerExec == priorOrigin && timerIntervalSec == priorSeconds,
          "late invalid provided optional field preserves the entire old config and 64-bit schedule");
  require(preferences.saves == 0 && statsSaves == 0 && stats.smsSent == 7 && testEffects.empty() && sends.empty() &&
          atCommands.empty() && pendingSmsCount == 0 && !mqttPendingPing && ESP.restarts == 0, "invalid partial input cannot normalize, save or execute anything");

  // A provided string getter failing allocation is not the same as omitting it, even when disabled/empty.
  for (const auto& json : {R"({"enabled":false,"phone":"new-phone"})", R"({"enabled":false,"message":"new message"})",
                          R"({"enabled":false,"phone":""})", R"({"enabled":false,"message":""})"}) {
    resetState(); const Config prior = config; server.args = {{"plain", json}};
    StringFaults::reserveAfter = 1; handleTimer(); expectWebReply(400, false); noWebEffects(prior);
  }
  resetState(); Config expected = config; expected.timerEnabled = false;
  server.args = {{"plain", R"({"enabled":false})"}};
  preferences.beforeSave = [&] {
    require(sameConfig(config, expected), "omitted strings cannot be lost to an implicit copy failure");
    StringFaults::reset(); // Stop the fault before the persistence substitute copies values.
  };
  StringFaults::copyAfter = 1; // Body retrieval may copy; omitted config strings need no copy.
  handleTimer(); expectWebReply(200, true);
  require(sameConfig(config, expected) && preferences.saves == 1, "enabled-only update preserves saved strings without copying them");
}

static void filterTests() {
  const std::string decodedList = "10000,10001,10002,10003,\"quoted\",[literal],\\path";
  for (const std::string numbers : {R"("10000\r\n10001\r10002\n10003，\"quoted\",[literal],\\path")",
       R"(["10000\r\n10001","10002\n10003","\"quoted\"","[literal]","\\path"])"}) {
    permutations({R"("enabled":false)", R"("whitelist":true)", "\"numbers\":" + numbers}, [&](const std::string& json) {
      resetState(); webRequest(handleFilterSave, json); expectWebReply(200, true);
      require(!config.filterEnabled && config.filterIsWhitelist && text(config.filterList) == decodedList, "numbers array/string normalized after decoding");
      require(preferences.saves == 1 && config.contentFilterList == "old-keywords" && lastTimerExec == 44, "number save stays scoped");
    });
  }
  resetState(); webRequest(handleFilterSave, R"({"enabled":true,"whitelist":false,"numbers":"[\"literal\"]","extra":["not numbers"]})");
  expectWebReply(200, true); require(config.filterList == "[\"literal\"]", "quoted string is not treated as JSON array");
  for (const auto& empty : {"\"\"", "[]", "[\"\"]"}) {
    resetState(); webRequest(handleFilterSave, "{\"enabled\":true,\"whitelist\":false,\"numbers\":" + std::string(empty) + "}");
    expectWebReply(200, true); require(config.filterList.length() == 0, "number list can be cleared");
  }
  resetState(); webRequest(handleFilterSave, "{\"enabled\":true,\"whitelist\":false,\"numbers\":" + list(100) + "}");
  expectWebReply(200, true); require(config.filterList.endsWith(",99"), "100 number entries accepted intact");
  webReject(handleFilterSave, "{\"enabled\":true,\"whitelist\":false,\"numbers\":" + list(101) + "}");

  permutations({R"("enabled":true)", R"("whitelist":false)", R"("keywords":"OTP\r\n银行\r快递\n通知，\"quoted\",[literal],\\path")"}, [](const std::string& json) {
    resetState(); webRequest(handleContentFilterSave, json); expectWebReply(200, true);
    require(config.contentFilterEnabled && !config.contentFilterIsWhitelist && config.contentFilterList == "OTP,银行,快递,通知,\"quoted\",[literal],\\path", "content separators and quotes normalized intact");
    require(preferences.saves == 1 && config.filterList == "old-numbers" && lastTimerExec == 44, "content save stays scoped");
  });
  resetState(); webRequest(handleContentFilterSave, R"({"enabled":true,"whitelist":true,"keywords":""})");
  expectWebReply(200, true); require(config.contentFilterList.length() == 0 && preferences.saves == 1, "empty keywords clears existing list");
  for (const auto& pair : std::vector<std::pair<Handler, std::string>>{{handleFilterSave, "numbers"}, {handleContentFilterSave, "keywords"}}) {
    const std::string field = "\"" + pair.second + "\"";
    webReject(pair.first, "{\"enabled\":true,\"whitelist\":false}");
    webReject(pair.first, "{" + field + ":\"x\",\"whitelist\":false}");
    webReject(pair.first, "{" + field + ":\"x\",\"enabled\":true}");
    for (const auto& value : {"true", "1", "null", "{}"}) webReject(pair.first, "{\"enabled\":true,\"whitelist\":false," + field + ":" + value + "}");
    for (const auto& value : {"\"true\"", "1", "null"}) {
      webReject(pair.first, "{\"enabled\":" + std::string(value) + ",\"whitelist\":false," + field + ":\"x\"}");
      webReject(pair.first, "{\"enabled\":true,\"whitelist\":" + std::string(value) + "," + field + ":\"x\"}");
    }
  }
  webReject(handleContentFilterSave, R"({"enabled":true,"whitelist":false,"keywords":[]})");
}

static void mqttControlTests() {
  permutations({R"("host":"exa\u006dple.test")", R"("note":"host,\"ignored\"")", R"("ok":true)"}, [](const std::string& json) {
    resetState(); mqttRequest(mqttTopicPing, json);
    require(mqttPendingPing && mqttPendingPingHost == "example.test" && atCommands.empty(), "ping parses escaped host independent of order and stays pending");
    mqttRequest(mqttTopicPing, R"({"host":"192.0.2.2"})");
    require(mqttPendingPingHost == "example.test", "pending ping cannot be overwritten");
    processMqttPendingCommands();
    require(atCommands == std::vector<std::string>({"AT+CGACT=1,1", "AT+MPING=\"example.test\",30,1", "AT+CGACT=0,1"}), "unchanged modem path gets exact safe host");
    require(!mqttPendingPing && pingResults.size() == 1 && pingResults[0].host == "example.test" && pingResults[0].success, "ping drains once with original result semantics");
  });
  for (const auto& json : {R"({})", R"({"other":"192.0.2.1"})"}) {
    resetState(); mqttRequest(mqttTopicPing, json); require(mqttPendingPing && mqttPendingPingHost == "8.8.8.8", "missing host defaults to 8.8.8.8");
  }
  for (const auto& host : {"192.0.2.1", "2001:db8::1", "::1", "my-host.test."}) {
    resetState(); mqttRequest(mqttTopicPing, std::string("{\"host\":\"") + host + "\"}");
    require(mqttPendingPing && mqttPendingPingHost == host, "ordinary IPv4/IPv6/DNS hosts accepted");
  }
  const size_t maxHost = 256 - (sizeof("AT+MPING=\"\",30,1") - 1);
  resetState(); mqttRequest(mqttTopicPing, "{\"host\":\"" + std::string(maxHost, 'a') + "\"}");
  require(mqttPendingPing, "host exactly fitting modem command bound accepted"); processMqttPendingCommands();
  require(atCommands[1].size() == 256, "ping wrapper counted in modem bound");
  mqttReject(mqttTopicPing, "{\"host\":\"" + std::string(maxHost + 1, 'a') + "\"}");
  for (const auto& host : {"", " ", "example.test ", "\\nexample.test", "example.test\\r", "ex ample.test", "example.test\\t", "x\\\",30,1;AT", "x;AT",
                         "x\\r\\nAT", "x\\u000aAT", "x\\u000dAT", "x\\u001b", "x\\u001a", "x\\u007f", "x\\\\AT", "https://example.test", "银行.test"}) {
    mqttReject(mqttTopicPing, std::string("{\"host\":\"") + host + "\"}");
  }
  for (const auto& value : {"true", "12", "[]", "null"}) mqttReject(mqttTopicPing, std::string("{\"host\":") + value + "}");

  for (const auto& action : {"restart", "reset", "re\\u0073tart"}) {
    permutations({std::string("\"action\":\"") + action + "\"", R"("unknown":["tail","\"q\""])"}, [](const std::string& json) {
      resetState(); mqttRequest(mqttTopicCmd, json);
      require(ESP.restarts == 0 && mqttPendingRestart && testEffects.empty(), "restart callback only queues intent during AT wait");
      testModemBusy = true; processMqttPendingCommands();
      require(ESP.restarts == 0 && mqttPendingRestart, "restart waits until AT transaction has ended");
      testModemBusy = false; processMqttPendingCommands();
      require(ESP.restarts == 1 && testEffects == std::vector<std::string>({"status:restarting", "delay:500", "restart"}), "restart/reset keeps status-delay-restart order");
    });
  }
  for (bool discovery : {false, true}) {
    resetState(); config.mqttHaDiscovery = discovery;
    mqttRequest(mqttTopicCmd, R"({"other":"tail","action":"status","flags":false})");
    require(publications.size() == (discovery ? 2U : 1U) && publications[0].topic == text(mqttTopicStatus) && publications[0].retained &&
            publications[0].payload == "{\"status\":\"online\"}" && ESP.restarts == 0, "status retains user/optional HA publishing semantics");
    if (discovery) require(publications[1].topic == text(mqttHaStatusTopic) && publications[1].retained, "HA status retained");
  }
  for (const auto& action : {"", "unknown", " restart ", "restart\\\"", "restart\\n"}) {
    resetState(); mqttRequest(mqttTopicCmd, std::string("{\"action\":\"") + action + "\"}");
    require(ESP.restarts == 0 && publications.empty() && !errors.empty() && Serial.log.find("未知命令") != std::string::npos, "unknown/empty action explicitly logged");
  }
  for (const auto& json : {R"({})", R"({"action":true})", R"({"action":["restart"]})"}) mqttReject(mqttTopicCmd, json);
  resetState(); mqttRequest("unsubscribed/topic", "not json"); require(testEffects.empty() && errors.empty(), "unknown topic remains ignored");
}

static void strictBoundaryTests() {
  const std::vector<std::pair<Handler, std::string>> webCases = {{handleSendSms, webSms}, {handleTimer, timerJson},
    {handleFilterSave, filterJson}, {handleContentFilterSave, contentJson}};
  const std::vector<std::pair<String, std::string>> mqttCases = {{mqttTopicSmsSend, mqttSms}, {mqttTopicPing, "{}"}, {mqttTopicCmd, R"({"action":"restart"})"}};
  const std::vector<std::string> tails = {R"("bad":null)", R"("bad":"\q")", R"("bad":"\ud800")", R"("dup":1,"\u0064up":2)",
    R"("bad":{"nested":"x"})", R"("bad":["a",false])", R"("bad":01)", R"("bad":"\u0000")"};
  for (const auto& item : webCases) {
    for (const auto& tail : tails) webReject(item.first, extend(item.second, tail));
    webReject(item.first, item.second + "trailing"); webReject(item.first, item.second.substr(0, item.second.size() - 1));
    webReject(item.first, item.second + std::string(1, '\0'));
    resetState(); webRequest(item.first, padded(item.second, 8192)); expectWebReply(200, true);
    require(StringFaults::maxReserve <= 8192, "Web never requests unbounded parser allocation");
    webReject(item.first, padded(item.second, 8193)); require(server.plainReads == 0, "overlength Web request rejected before body copy");
    webReject(item.first, extend(item.second, "\"extra\":" + list(101)));
    // Wrong HTTP length cannot turn a truncated/copy-failed body into a command.
    for (int length : {-1, 0, 1, 8193, INT_MAX, 2}) {
      resetState(); const Config before = config; server.declaredLength = length; webRequest(item.first, item.second);
      expectWebReply(400, false); noWebEffects(before);
      if (length != 2) require(server.plainReads == 0, "invalid declared length is checked first");
    }
    std::string sixteen = item.second;
    JsonInput countInput; require(countInput.parse(String(item.second)), "field-count fixture");
    const int initial = item.first == handleTimer ? 5 : (item.first == handleSendSms ? 2 : 3);
    for (int i = initial; i < 16; ++i) sixteen = extend(sixteen, "\"extra" + std::to_string(i) + "\":true");
    resetState(); webRequest(item.first, sixteen); expectWebReply(200, true);
    webReject(item.first, extend(sixteen, "\"seventeenth\":false"));
  }
  for (const auto& item : mqttCases) {
    for (const auto& tail : tails) mqttReject(item.first, extend(item.second, tail));
    mqttReject(item.first, item.second + "trailing"); mqttReject(item.first, item.second + std::string(1, '\0'));
    resetState(); mqttRequest(item.first, padded(item.second, 8192)); require(errors.empty(), "MQTT 8192-byte command with unknown fields accepted");
    mqttReject(item.first, padded(item.second, 8193)); require(StringFaults::reserveCalls == 0, "MQTT oversized buffer never reserved");
    resetState(); byte single = '{'; mqttCallback(const_cast<char*>(item.first.c_str()), &single, UINT_MAX);
    require(!errors.empty() && StringFaults::reserveCalls == 0 && atCommands.empty() && sends.empty(), "UINT_MAX MQTT length rejected before touching payload");
    resetState(); mqttCallback(const_cast<char*>(item.first.c_str()), nullptr, 2); require(!errors.empty(), "null payload fails safely");
    mqttReject(item.first, extend(item.second, "\"extra\":" + list(101)));
    std::string sixteen = item.second;
    const int initial = item.first == mqttTopicSmsSend ? 2 : (item.first == mqttTopicPing ? 0 : 1);
    for (int i = initial; i < 16; ++i) sixteen = extend(sixteen, "\"extra" + std::to_string(i) + "\":\"ok\"");
    resetState(); mqttRequest(item.first, sixteen); require(errors.empty(), "MQTT 16 fields including unknown text accepted");
    mqttReject(item.first, extend(sixteen, "\"seventeenth\":false"));
  }
  webReject(handleSendSms, extend(webSms, R"("ph\u006fne":"other")"));
  webReject(handleTimer, extend(timerJson, R"("enabled":false)"));
  mqttReject(mqttTopicSmsSend, extend(mqttSms, R"("message":"other")"));
  mqttReject(mqttTopicPing, R"({"host":"example.test","host":"other.test"})");
  mqttReject(mqttTopicCmd, R"({"action":"restart","\u0061ction":"status"})");
  // Malformed trailing fields cannot overwrite already pending valid work.
  resetState(); mqttRequest(mqttTopicSmsSend, mqttSms); mqttRequest(mqttTopicPing, "{}");
  mqttRequest(mqttTopicSmsSend, extend(mqttSms, R"("bad":null)")); mqttRequest(mqttTopicPing, R"({"host":"new.test","bad":null})");
  require(pendingSmsCount == 1 && pendingSmsQueue[0].message == "text" && mqttPendingPing && mqttPendingPingHost == "8.8.8.8", "invalid input preserves existing pending work");
}

static void allocationTests() {
  for (const auto& item : std::vector<std::pair<Handler, std::string>>{{handleSendSms, webSms}, {handleTimer, timerJson},
       {handleFilterSave, filterJson}, {handleContentFilterSave, contentJson}}) {
    const int getters = (item.first == handleSendSms || item.first == handleTimer) ? 2 : 1;
    for (int failAt = 0; failAt <= getters; ++failAt) {
      resetState(); const Config before = config; server.args = {{"plain", String(item.second)}};
      StringFaults::reserveAfter = failAt; item.first();
      expectWebReply(400, false); noWebEffects(before);
    }
    resetState(); const Config before = config; server.args = {{"plain", String(item.second)}};
    StringFaults::copyAfter = 0; item.first(); expectWebReply(400, false); noWebEffects(before);
    JsonInput probe; StringFaults::reset(); require(probe.parse(String(item.second)), "allocation fixture parse");
    const int parserAppends = static_cast<int>(StringFaults::concatCalls);
    for (int failAt : {0, parserAppends}) {
      resetState(); const Config prior = config; server.args = {{"plain", String(item.second)}};
      StringFaults::concatAfter = failAt; item.first(); expectWebReply(400, false); noWebEffects(prior);
    }
  }
  for (const auto& item : std::vector<std::pair<String, std::string>>{{mqttTopicSmsSend, mqttSms},
       {mqttTopicPing, R"({"host":"example.test"})"}, {mqttTopicCmd, R"({"action":"restart"})"}}) {
    const int getters = item.first == mqttTopicSmsSend ? 2 : 1;
    for (int failAt = 0; failAt < 2 + getters; ++failAt) {
      resetState(); StringFaults::reserveAfter = failAt; mqttRequest(item.first, item.second);
      require(!errors.empty() && pendingSmsCount == 0 && !mqttPendingPing && ESP.restarts == 0 && sends.empty(), "MQTT payload/parser/getter reserve failure has no command effect");
      if (item.first == mqttTopicSmsSend) require(sentResults.size() == 1 && !sentResults[0].success, "MQTT OOM returns SMS false");
    }
    for (int failAt : {0, 1}) {
      resetState(); StringFaults::concatAfter = failAt; mqttRequest(item.first, item.second);
      require(!errors.empty() && pendingSmsCount == 0 && !mqttPendingPing && ESP.restarts == 0, "MQTT construction/parser append failure rejected");
    }
  }
  StringFaults::reset();
}

static void fullSaveAndLoadTests() {
  resetState();
  server.args = {{"contentFilterEn", "true"}, {"contentFilterIsWhitelist", "false"},
    {"contentFilterList", "OTP\r\n银行\r快递\n通知，\"quoted\""}, {"webUser", "new-user"}, {"webPass", ""},
    {"smtpPass", "   "}, {"wifi0pass", ""}, {"mqttPass", ""}};
  handleSave(); expectWebReply(200, true);
  require(config.contentFilterEnabled && !config.contentFilterIsWhitelist && config.contentFilterList == "OTP,银行,快递,通知,\"quoted\"", "save-all reads all three content filter fields");
  require(config.webUser == "saved-admin" && config.webPass == "synthetic-web-secret" && config.smtpPass == "synthetic-smtp-secret" &&
          config.wifiNetworks[0].password == "synthetic-wifi-secret" && config.mqttPass.length() == 0, "original Web/WiFi/SMTP password preservation and MQTT blank overwrite unchanged");
  require(preferences.saves == 1 && lastTimerExec == 44, "full content-only save does not reset timer");
  const String expectedList = config.contentFilterList;
  config = Config{}; loadConfig();
  require(preferences.loads == 1 && config.contentFilterEnabled && !config.contentFilterIsWhitelist && config.contentFilterList == expectedList, "real NVS save/load round trip for content fields");
  server.reset(); handleRoot();
  require(server.statusCode == 200 && server.streamedBody().find("var cf={en:true,wl:false,ls:\"OTP,银行,快递,通知,\\\"quoted\\\"\"};") != std::string::npos,
          "real root loading interface exposes saved content fields");
  require(server.streamedBody().find("%CF_LIST%") == std::string::npos, "root resolves content list placeholder");

  // Compare dedicated and full-save normalization, rather than a mirrored normalizer.
  const std::string plainList = "A\r\nB\rC\nD，E,\"quoted\",[literal],\\path";
  server.reset(); webRequest(handleContentFilterSave, R"({"enabled":true,"whitelist":false,"keywords":"A\r\nB\rC\nD，E,\"quoted\",[literal],\\path"})");
  const String dedicated = config.contentFilterList;
  server.reset(); server.args = {{"contentFilterList", String(plainList)}}; handleSave();
  require(config.contentFilterList == dedicated && config.contentFilterEnabled && !config.contentFilterIsWhitelist, "full and dedicated save share separator/quote semantics");

  server.reset(); server.args = {{"contentFilterEn", "false"}}; handleSave();
  require(!config.contentFilterEnabled && !config.contentFilterIsWhitelist && config.contentFilterList == dedicated, "en-only form leaves mode/list untouched");
  server.reset(); server.args = {{"contentFilterIsWhitelist", "true"}}; handleSave();
  require(!config.contentFilterEnabled && config.contentFilterIsWhitelist && config.contentFilterList == dedicated, "mode-only form leaves en/list untouched");
  server.reset(); handleSave();
  require(!config.contentFilterEnabled && config.contentFilterIsWhitelist && config.contentFilterList == dedicated, "omitting all content fields preserves them");
  server.reset(); server.args = {{"contentFilterList", ""}}; handleSave();
  require(config.contentFilterList.length() == 0 && !config.contentFilterEnabled && config.contentFilterIsWhitelist, "explicit empty full-save list clears without resetting booleans");
  config = Config{}; loadConfig(); server.reset(); handleRoot();
  require(server.streamedBody().find("var cf={en:false,wl:true,ls:\"\"};") != std::string::npos, "cleared full-save list survives load and root rendering");
  config.contentFilterList = "repopulate";
  server.reset(); webRequest(handleContentFilterSave, R"({"enabled":true,"whitelist":false,"keywords":""})");
  config = Config{}; loadConfig(); server.reset(); handleRoot();
  require(config.contentFilterList.length() == 0 && server.streamedBody().find("var cf={en:true,wl:false,ls:\"\"};") != std::string::npos, "dedicated empty list survives actual save/load/root");

  resetState(); server.args = {{"contentFilterList", String(std::string(3999, 'x'))}}; handleSave(); expectWebReply(200, true);
  require(config.contentFilterList.length() == 3999, "full-save list accepts the actual NVS byte budget");
  for (const auto& args : std::vector<std::map<std::string, String>>{
       {{"contentFilterEn", "yes"}}, {{"contentFilterIsWhitelist", "1"}},
       {{"contentFilterList", String(std::string(4000, 'x'))}}, {{"contentFilterList", String(std::string(8193, 'x'))}}}) {
    resetState(); const Config before = config; server.args = args;
    server.args["webUser"] = "must-not-save"; server.args["webPass"] = "must-not-save";
    handleSave(); expectWebReply(400, false); noWebEffects(before);
  }
  // Config snapshot copies must fail closed at every string, including fields later overwritten.
  const int configStrings = 15 + MAX_WIFI_NETWORKS * 2 + MAX_PUSH_CHANNELS * 5;
  for (int fault = 0; fault < configStrings; ++fault) {
    resetState(); const Config before = config; server.args = {{"contentFilterList", "replace-list"}};
    StringFaults::copyAfter = fault;
    handleSave(); expectWebReply(503, false); noWebEffects(before);
  }
  for (Handler handler : {handleSendSms, handleTimer, handleFilterSave, handleContentFilterSave, handleSave}) {
    resetState(); const Config before = config; server.authorized = false;
    server.args = {{"plain", timerJson.c_str()}, {"contentFilterList", "must-not-save"}};
    handler(); require(server.statusCode == 401 && server.plainReads == 0, "auth enforced before reading JSON"); noWebEffects(before);
  }
}

#ifndef WEB_REGRESSION_REUSE
int main() {
  const std::vector<std::pair<const char*, std::function<void()>>> suites = {
    {"shared parser strict types/Unicode/escapes/duplicates/limits/OOM", parserTests},
    {"real Web/MQTT SMS whitespace/order/error/results/FIFO", smsTests},
    {"real timer validation/atomic apply/reset/save/64-bit remain", timerTests},
    {"real number/content list decoding/normalization/empty clearing", filterTests},
    {"real MQTT ping safety/default/pending and control/restart semantics", mqttControlTests},
    {"whole-command validation, byte/field/array bounds and pending preservation", strictBoundaryTests},
    {"failed copies/reserves/appends cannot execute or partially save", allocationTests},
    {"save-all three fields/optional updates/passwords/actual load/root/auth", fullSaveAndLoadTests}
  };
  int failures = 0;
  for (const auto& suite : suites) {
    try { suite.second(); std::cout << "PASS: " << suite.first << '\n'; }
    catch (const std::exception& error) { failures++; std::cerr << "FAIL: " << suite.first << ": " << error.what() << '\n'; }
    StringFaults::reset();
  }
  std::cout << checks << " assertions, " << suites.size() << " suites, " << failures << " failures.\n";
  return failures ? 1 : 0;
}
#endif
