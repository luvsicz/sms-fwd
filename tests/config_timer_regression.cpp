// Real complete code.ino/config.ino/web_handlers.ino, with only hardware/dependency I/O substituted.
// Production receive/history/filter functions are executed too; no mirrored timer/filter algorithm.
#include "config_timer_host_stubs.h"
#include <climits>
#include <functional>
#include <iostream>
#include <fstream>
#include "code.ino"
#include "config.ino"
#include "web_handlers.ino"
#include "config_timer_receive.inc"
#include "web_query.ino"
#include "call_handler.ino"

static_assert(std::is_same<decltype(lastTimerExec), uint64_t>::value, "timer origin must be uint64_t milliseconds");
static_assert(std::is_same<decltype(timerIntervalSec), uint64_t>::value, "timer period must be uint64_t seconds");
static_assert(SERIAL_BUFFER_SIZE == 500, "do not enlarge the existing single-line parser limit");
static_assert(SERIAL_RX_BUFFER_SIZE >= SERIAL_BUFFER_SIZE, "RX capacity must hold a full line");

struct SentEvent { String phone, message; bool success; };
static std::vector<SentEvent> sentEvents;
static std::vector<std::string> atCommands;
static unsigned int sendCalls = 0, receiveHttp = 0, receiveMqtt = 0, receiveEmail = 0;
static bool sendResult = true, atResult = true;
static uint64_t sendDurationMs = 0;
static uint64_t originDuringSend = 0;

bool sendSMS(const char*, const char*) {
  sendCalls++; originDuringSend = lastTimerExec; testEffects.push_back("send");
  testClockMs += sendDurationMs;
  return sendResult;
}
void publishMqttSmsSent(const char* phone, const char* message, bool success) {
  sentEvents.push_back({phone, message, success}); testEffects.push_back(success ? "mqtt-ok" : "mqtt-fail");
}
void publishMqttStatus(const char* status) { testEffects.push_back(status); }
ModemResult runModemCommand(const char* command, unsigned long, String*, String* resultLine, const char*) {
  atCommands.emplace_back(command);
  if (resultLine) *resultLine = "+CGATT:1";
  return atResult ? ModemResult::Ok : ModemResult::Error;
}
void modemDelay(unsigned long amount) { delay(amount); }
static void (*testModemIdleHook)() = nullptr;
void setModemIdleHook(void (*hook)()) { testModemIdleHook = hook; }
void serviceMqtt() {}
void initConcatBuffer() {}
void checkConcatTimeout() {}
void checkSerial1URC() {}
void initSmtpLock() {}
void initMqttTopics() {}
bool ensureMqttBufferSize(size_t) { return true; } // MQTT allocation is outside this suite.
void mqttCallback(char*, byte*, unsigned int) {}
void mqttReconnect() {}
void publishMqttDeviceStatus() {}
void processMqttLogQueue() {}
void processMqttPendingCommands() {}
String getCurrentTimeString() { return "2026-04-18 12:34:56"; }
void sendSMSToServer(const char*, const char*, const char*) { receiveHttp++; }
void publishMqttSmsReceived(const char*, const char*, const char*) { receiveMqtt++; }
void publishMqttCallReceived(const char*, const char*) { receiveMqtt++; }
void sendEmailNotification(const char*, const char*) { receiveEmail++; }

static constexpr uint64_t dayMs = 86400000ULL;
static constexpr uint64_t daySec = 86400ULL;
static constexpr uint64_t wrap = 1ULL << 32;
static int checks = 0;
static void require(bool condition, const std::string& label) {
  checks++;
  if (!condition) throw std::runtime_error(label);
}
static void resetState(uint64_t startMs = 12345) {
  config = Config{}; stats = Statistics{};
  historyReserveFailure = -1; historyIo = {}; lastCallNumber = ""; lastCallNotifyTime = 0;
  ConfigLockFaults::reset(); statsMutex = nullptr;
  preferences.clearTestData(); SPIFFS.clear();
  Serial = HardwareSerial{}; Serial1 = HardwareSerial{}; server.reset();
  testClockMs = startMs; lastTimerExec = 0; timerIntervalSec = 0; lastPrintTime = 0;
  configValid = true; WiFi.mode(WIFI_STA); WiFi.ssid = "synthetic-wifi"; ESP = TestEsp{};
  sentEvents.clear(); atCommands.clear(); testEffects.clear();
  sendCalls = receiveHttp = receiveMqtt = receiveEmail = 0;
  sendResult = atResult = true; sendDurationMs = 0; originDuringSend = 0;
}
static void arm(int days, uint64_t startMs = 12345) {
  resetState(startMs);
  config.timerEnabled = true; config.timerType = 1; config.timerInterval = days;
  config.timerPhone = "10000"; config.timerMessage = "synthetic timer";
  resetTimerSchedule();
}
static uint32_t persistedSent() {
  preferences.begin("sms_stats", true);
  uint32_t result = preferences.getULong("sent", 0);
  preferences.end(); return result;
}

static void uartTests() {
  for (int failure : {0, 1, 2}) {
    resetState();
    if (failure == 1) Serial1.rxResult = 0;
    if (failure == 2) Serial1.allocationSucceeds = false;
    setup();
    require(testModemIdleHook == serviceMqtt, "setup registers protected MQTT service during AT waits");
    require(Serial1.initOrder == std::vector<std::string>({"rx", "begin"}), "RX configured before UART begin exactly once");
    require(Serial1.requestedRx == SERIAL_RX_BUFFER_SIZE && Serial1.requestedRx >= SERIAL_BUFFER_SIZE,
            "UART uses the independent receive capacity");
    require(Serial1.baudRate == 115200 && Serial1.rxPin == RXD && Serial1.txPin == TXD,
            "baud rate and GPIO mapping preserved");
    require((Serial.log.find("UART RX缓冲配置失败") != std::string::npos) == (failure == 1),
            "setRxBufferSize return value is checked and failure diagnosed");
    require((Serial.log.find("UART初始化/接收缓冲分配失败") != std::string::npos) == (failure == 2),
            "begin allocation failure is diagnosed separately");
  }
}

static void longTimerTests() {
  for (int days : {49, 50, 60}) {
    // Three firings per case pass both the first and second 32-bit millis() wraps.
    arm(days);
    for (int period = 0; period < 3; period++) {
      const uint64_t start = lastTimerExec;
      const uint64_t duration = static_cast<uint64_t>(days) * dayMs;
      const uint64_t due = start + duration;
      const auto count = sendCalls;
      require(timerIntervalSec == static_cast<uint64_t>(days) * daySec, "day-to-second conversion is exact");
      require(timerRemainingSeconds() == timerIntervalSec, "new origin reports full interval");
      for (uint64_t boundary : {wrap, wrap * 2}) {
        for (uint64_t point : {boundary - 1, boundary, boundary + 1}) {
          if (point > start && point < due) {
            testClockMs = point;
            require(millis() == static_cast<uint32_t>(point), "test millis is genuinely 32-bit");
            require(monotonicMillis() == point, "ESP clock retains high bits through millis wrap");
            processTimerTask();
            require(sendCalls == count, "millis wrap cannot cause an early firing");
          }
        }
      }
      testClockMs = due - 1;
      require(timerRemainingSeconds() == 1, "one millisecond before due rounds up to one second");
      processTimerTask(); require(sendCalls == count, "49/50/60-day task does not fire early");
      testClockMs = due;
      require(timerRemainingSeconds() == 0, "due countdown saturates at zero");
      processTimerTask();
      require(sendCalls == count + 1 && lastTimerExec == due && originDuringSend == due,
              "fires once at exact deadline and advances origin before send");
      require(stats.smsSent.load() == count + 1 && preferences.statsSaves == count + 1 && persistedSent() == count + 1,
              "successful send counted and persisted once");
      require(sentEvents.size() == count + 1 && sentEvents.back().success, "success event emitted once");
      processTimerTask(); require(sendCalls == count + 1, "no second firing at the same instant");
      require(timerRemainingSeconds() == timerIntervalSec, "post-send countdown starts a complete cycle");
    }
    require(testClockMs > 2 * wrap, "each 49/50/60-day fixture crosses millis twice");
  }
  // No intermediate polling is needed to carry high bits over two wraps.
  arm(120, 0);
  testClockMs = 2 * wrap + 321;
  processTimerTask();
  require(sendCalls == 0 && monotonicMillis() == testClockMs, "two missed millis wraps do not lose elapsed time");
  testClockMs = 120 * dayMs;
  processTimerTask(); require(sendCalls == 1, "long gap across two wraps eventually fires exactly once");

  arm(60);
  testClockMs += 4 * 60 * dayMs + 123;
  processTimerTask(); processTimerTask();
  require(sendCalls == 1 && lastTimerExec == testClockMs, "late loop skips missed cycles rather than burst catch-up");

  arm(INT_MAX);
  const uint64_t wideSeconds = static_cast<uint64_t>(INT_MAX) * daySec;
  require(timerIntervalSec == wideSeconds && timerIntervalSec > UINT32_MAX, "INT_MAX positive days are converted without 32-bit overflow");
  require(timerRemainingSeconds() == wideSeconds, "large countdown is not truncated");
  processTimerTask(); require(sendCalls == 0, "large interval does not fire immediately");
  for (int days : {INT_MIN, -1, 0, 1, 30}) {
    config.timerInterval = days; resetTimerSchedule();
    require(config.timerInterval == (days < 1 ? 1 : days), "nonpositive intervals retain the UI minimum");
    require(timerIntervalSec == static_cast<uint64_t>(days < 1 ? 1 : days) * daySec, "normalization precedes unsigned conversion");
  }
}

static void resultTests() {
  struct Case { bool success; uint64_t duration; const char* name; };
  for (const auto& item : {Case{true, 250, "success"}, Case{false, 100, "error"}, Case{false, 30000, "timeout"}}) {
    arm(50, 2 * wrap + 777);
    stats.smsSent.store(7); saveStats(); preferences.statsSaves = 0; testEffects.clear();
    sendResult = item.success; sendDurationMs = item.duration;
    const uint64_t due = lastTimerExec + 50 * dayMs;
    testClockMs = due; processTimerTask();
    require(sendCalls == 1 && lastTimerExec == due && originDuringSend == due, std::string(item.name) + " advances origin first");
    require(stats.smsSent.load() == (item.success ? 8U : 7U) && persistedSent() == stats.smsSent.load(),
            std::string(item.name) + " only success increments/persists smsSent");
    require(preferences.statsSaves == (item.success ? 1U : 0U), "failure does not write success stats");
    require(sentEvents.size() == 1 && sentEvents[0].success == item.success &&
            sentEvents[0].phone == config.timerPhone && sentEvents[0].message == config.timerMessage,
            "MQTT receives the real return value and unchanged phone/body");
    const std::vector<std::string> expected = item.success ? std::vector<std::string>{"send", "stats", "mqtt-ok"}
                                                         : std::vector<std::string>{"send", "mqtt-fail"};
    require(testEffects == expected, "side effects occur exactly once in send/stats/MQTT order");
    for (int i = 0; i < 20; i++) loop();
    require(sendCalls == 1, "success/error/timeout never busy-retries through loop");
    testClockMs = due + 50 * dayMs - 1; processTimerTask(); require(sendCalls == 1, "failure waits a full normal interval");
    testClockMs++; processTimerTask(); require(sendCalls == 2, "failure retries at the next regular deadline");
  }
  for (int gate : {0, 1, 2, 3, 4}) {
    arm(1);
    if (gate == 0) config.timerEnabled = false;
    if (gate == 1) timerIntervalSec = 0;
    if (gate == 2) configValid = false;
    if (gate == 3) config.timerPhone = "";
    if (gate == 4) config.timerType = 9;
    testClockMs += 2 * dayMs; processTimerTask();
    require(sendCalls == 0 && sentEvents.empty() && preferences.statsSaves == 0, "disabled/zero/invalid/empty-phone/type guards preserved");
    if (gate < 2) require(timerRemainingSeconds() == 0, "disabled/unconfigured countdown is zero");
  }
  arm(1); config.timerType = 0; testClockMs += dayMs; processTimerTask();
  require(atCommands == std::vector<std::string>({"AT+CGACT=1,1", "AT+MPING=1,\"8.8.8.8\",4,32,255", "AT+CGACT=0,1"}),
          "ping timer action order is unchanged");
  require(sendCalls == 0 && stats.smsSent.load() == 0 && sentEvents.empty(), "ping never changes SMS counters");
}

static void resetAndWebTests() {
  resetState(2 * wrap + 5000);
  loadConfig();
  require(!config.timerEnabled && config.timerInterval == 30 && config.timerMessage == "保号短信",
          "NVS/UI defaults unchanged");
  require(lastTimerExec == testClockMs && timerIntervalSec == 30 * daySec, "boot load establishes a 64-bit origin");

  arm(60, wrap + 8000);
  testClockMs += 49 * dayMs; resetTimerSchedule();
  const uint64_t resetOrigin = testClockMs;
  require(timerRemainingSeconds() == 60 * daySec, "manual plan reset restores a whole interval");
  testClockMs += 11 * dayMs; processTimerTask();
  require(sendCalls == 0 && lastTimerExec == resetOrigin, "old deadline is cancelled after reset");

  testClockMs = 3 * wrap + 123;
  server.args = {{"plain", "{\"enabled\":true,\"type\":1,\"interval\":50,\"phone\":\"10000\",\"message\":\"timer\"}"}};
  handleTimer();
  require(server.statusCode == 200 && config.timerInterval == 50 && lastTimerExec == testClockMs && timerIntervalSec == 50 * daySec,
          "dedicated save uses the common reset helper");
  require(server.body == String("{\"success\":true,\"message\":\"定时任务已保存\",\"remain\":") + String(timerIntervalSec) + "}",
          "dedicated response retains existing interval-based remain behavior");
  const uint64_t firstDedicatedOrigin = lastTimerExec;
  testClockMs += 2 * dayMs; server.reset();
  server.args = {{"plain", "{\"enabled\":true,\"type\":1,\"interval\":50,\"phone\":\"10000\",\"message\":\"timer\"}"}};
  handleTimer();
  require(server.statusCode == 200 && lastTimerExec == testClockMs && lastTimerExec != firstDedicatedOrigin &&
          timerIntervalSec == 50 * daySec && timerRemainingSeconds() == 50 * daySec,
          "identical dedicated save still explicitly restarts the schedule");
  require(server.body == String("{\"success\":true,\"message\":\"定时任务已保存\",\"remain\":") + String(timerIntervalSec) + "}",
          "identical dedicated save still reports a full interval");
  const uint64_t dedicatedOrigin = lastTimerExec;
  testClockMs += 2 * dayMs;
  server.reset();
  server.args = {{"timerEn", "true"}, {"timerType", "1"}, {"timerInterval", "60"},
                 {"timerPhone", "10001"}, {"timerMessage", "full save"}};
  handleSave();
  require(lastTimerExec == testClockMs && lastTimerExec != dedicatedOrigin && timerIntervalSec == 60 * daySec,
          "full save resets exactly like dedicated save");
  require(config.timerPhone == "10001" && config.timerMessage == "full save", "full save retains timer payloads");
  const uint64_t fullOrigin = lastTimerExec;
  testClockMs += 12345; server.reset(); handleSave();
  require(lastTimerExec == fullOrigin && timerIntervalSec == 60 * daySec, "full save without timer fields does not reset");
  saveConfig(); require(lastTimerExec == fullOrigin, "unrelated generic config persistence does not reset");

  config = Config{}; lastTimerExec = UINT64_MAX; timerIntervalSec = 0;
  testClockMs = 77; loadConfig();
  require(lastTimerExec == 77 && timerIntervalSec == 60 * daySec && config.timerEnabled,
          "simulated reboot reloads config but never a persisted monotonic origin");
  require(config.timerPhone == "10001" && config.timerMessage == "full save", "timer fields survive NVS round trip");
  configValid = true; processTimerTask(); require(sendCalls == 0, "reboot begins a fresh period");

  // Complete Web functions exercise 64-bit String and printf paths with values exceeding uint32_t.
  server.reset();
  server.args = {{"plain", "{\"enabled\":true,\"type\":1,\"interval\":2147483647,\"phone\":\"10000\",\"message\":\"wide\"}"}};
  handleTimer();
  const uint64_t wideSeconds = static_cast<uint64_t>(INT_MAX) * daySec;
  require(timerIntervalSec == wideSeconds && server.body == String("{\"success\":true,\"message\":\"定时任务已保存\",\"remain\":") + String(wideSeconds) + "}",
          "timer API remain is emitted as full 64-bit decimal");
  testClockMs += 50 * dayMs + 999;
  const uint64_t remain = wideSeconds - 50 * daySec;
  server.reset(); handleRoot();
  require(server.streamedBody().find(std::string("rm:") + std::to_string(remain) + "}") != std::string::npos,
          "root countdown is neither 32-bit truncated nor buffer-truncated");
  config.timerEnabled = false; server.reset(); handleRoot();
  require(server.streamedBody().find("rm:0}") != std::string::npos, "disabled root countdown remains zero");

  server.reset(); server.args = {{"plain", "{\"enabled\":false,\"interval\":0}"}}; handleTimer();
  require(config.timerInterval == 1 && timerIntervalSec == daySec && !config.timerEnabled &&
          server.body == "{\"success\":true,\"message\":\"定时任务已保存\",\"remain\":86400}", "disabled/zero interval preserves existing UI response defaults");
  const uint64_t prior = lastTimerExec;
  testClockMs++; server.reset(); server.authorized = false;
  handleTimer(); handleSave(); require(lastTimerExec == prior, "unauthorized saves do not change schedule");

  for (int days : {0, -60, INT_MAX}) {
    preferences.begin("sms_config", false); preferences.putInt("timerInt", days); preferences.end();
    testClockMs += 456; loadConfig();
    require(timerIntervalSec == static_cast<uint64_t>(days < 1 ? 1 : days) * daySec && lastTimerExec == testClockMs,
            "NVS load normalizes then performs the same wide conversion/reset");
  }
}

static void fullSaveTimerTests() {
  auto prepareForm = [] {
    server.reset();
    server.args = {{"timerEn", config.timerEnabled ? "true" : "false"}, {"timerType", String(config.timerType)},
                   {"timerInterval", String(config.timerInterval)}, {"timerPhone", config.timerPhone},
                   {"timerMessage", config.timerMessage}};
  };
  for (bool enabled : {false, true}) {
    arm(60, wrap + 8000); config.timerEnabled = enabled;
    const uint64_t origin = lastTimerExec;
    for (unsigned int save = 1; save <= 2; ++save) {
      testClockMs += 25 * dayMs;
      const uint64_t remaining = timerRemainingSeconds();
      prepareForm();
      if (save == 2) server.args["mqttPrefix"] = "unrelated-prefix";
      handleSave();
      require(server.statusCode == 200 && preferences.saves == save, "unchanged timer form still saves configuration");
      require(lastTimerExec == origin && timerIntervalSec == 60 * daySec && timerRemainingSeconds() == remaining,
              "unchanged full-form timer settings preserve the long-running origin, interval and countdown");
      if (save == 2) require(config.mqttPrefix == "unrelated-prefix", "unrelated edits save without restarting the timer");
    }
  }

  struct Change { const char* field; const char* value; };
  for (bool enabled : {false, true}) {
    const Change changes[] = {{"timerEn", enabled ? "false" : "true"}, {"timerType", "0"},
                              {"timerInterval", "61"}, {"timerPhone", "10001"}, {"timerMessage", "changed body"}};
    for (const auto& change : changes) {
      arm(60, wrap + 8000); config.timerEnabled = enabled;
      testClockMs += 50 * dayMs;
      prepareForm(); server.args[change.field] = change.value;
      handleSave();
      const uint64_t seconds = (std::string(change.field) == "timerInterval" ? 61 : 60) * daySec;
      require(server.statusCode == 200 && preferences.saves == 1, std::string(change.field) + " change saves successfully");
      require(lastTimerExec == testClockMs && timerIntervalSec == seconds &&
              timerRemainingSeconds() == (config.timerEnabled ? seconds : 0),
              std::string(change.field) + " change alone restarts the schedule");
    }
  }

  for (bool enabled : {false, true}) for (const char* interval : {"0", "-60", "01"}) {
    arm(1, wrap + 8000); config.timerEnabled = enabled;
    const uint64_t origin = lastTimerExec;
    testClockMs += dayMs / 2;
    prepareForm(); server.args["timerInterval"] = interval;
    handleSave();
    require(server.statusCode == 200 && config.timerInterval == 1 && lastTimerExec == origin && timerIntervalSec == daySec &&
            timerRemainingSeconds() == (enabled ? daySec / 2 : 0),
            "equivalent parsed/normalized interval does not restart the schedule");
  }
}

static void filterTests() {
  struct NumberCase { const char* list; const char* number; bool match; };
  const std::vector<NumberCase> numbers = {
    {"", "10000", false}, {" \t\r\n ", "10000", false}, {",,,", "10000", false},
    {" ,\t,\r\n, ", "10000", false}, {"+,-,(), + - () ", "10000", false},
    {"10000", "", false}, {"10000", " + - () ", false}, {"+,-,()", "+ - ()", false},
    {",,10000,,", "10000", true}, {"99999,10000", "10000", true},
    {"99999", "10000", false}, {"+86 (138) 0013-8000", "13800138000", true},
    {"10000", "8610000", true}, {"8610000", "10000", true},
    {"+,-,(),10000,", "10000", true}, {"+,-,(),99999,", "10000", false}
  };
  for (const auto& item : numbers) {
    for (bool enabled : {false, true}) for (bool white : {false, true}) {
      resetState();
      config.filterEnabled = enabled; config.filterIsWhitelist = white; config.filterList = item.list;
      const bool expected = enabled && (white ? !item.match : item.match);
      require(isNumberFiltered(item.number) == expected, "number disabled/white/black/empty/match matrix");
      saveConfig(); config = Config{}; loadConfig();
      require(config.filterEnabled == enabled && config.filterIsWhitelist == white && config.filterList == item.list &&
              isNumberFiltered(item.number) == expected, "number config persists and reloads without semantic changes");
    }
  }
  for (const char* empty : {"", " ", "+", "-", "()", " + - () "}) {
    require(!phoneNumbersMatch("10000", empty) && !phoneNumbersMatch(empty, "10000") && !phoneNumbersMatch(empty, empty),
            "neither exact nor suffix matching accepts empty normalized numbers");
  }
  require(normalizePhoneNumber(" +86 (138) 0013-8000 ") == "13800138000", "existing country-code normalization preserved");

  struct ContentCase { const char* list; const char* text; bool match; };
  const std::vector<ContentCase> contents = {
    {"", "verification", false}, {" \t\r\n ", "verification", false}, {",,,", "verification", false},
    {" ,\t,\r\n, ", "verification", false}, {",,OTP,,", "your otp 123456", true},
    {" otp, BANK ", "A bank notice", true}, {"otp", "weather notice", false},
    {"验证码,银行", "银行通知", true}, {"验证码", "快递通知", false},
    {"OTP", "", false}, {" , , ", "", false}, {" + ", "code + 1", true}
  };
  for (const auto& item : contents) {
    for (bool enabled : {false, true}) for (bool white : {false, true}) {
      resetState();
      config.contentFilterEnabled = enabled; config.contentFilterIsWhitelist = white; config.contentFilterList = item.list;
      const bool expected = enabled && (white ? !item.match : item.match);
      require(isContentFiltered(item.text) == expected, "content disabled/white/black/empty/case-insensitive/match matrix");
      saveConfig(); config = Config{}; loadConfig();
      require(config.contentFilterEnabled == enabled && config.contentFilterIsWhitelist == white && config.contentFilterList == item.list &&
              isContentFiltered(item.text) == expected, "content config persists and reloads without semantic changes");
    }
  }
}

static void historyTests() {
  for (int mode : {0, 1, 2, 3}) {
    resetState();
    config.filterEnabled = mode != 3; config.contentFilterEnabled = mode != 3;
    config.filterIsWhitelist = mode == 0;
    config.contentFilterIsWhitelist = mode == 1;
    config.filterList = " , , "; config.contentFilterList = " ,\t, ";
    processSmsContent("10000", "synthetic OTP 123456", "2026-04-18 12:34:56");
    const unsigned int expected = mode < 2 ? 0 : 1;
    require(SPIFFS.contents("/sms.txt").find("synthetic OTP 123456") != std::string::npos && stats.smsReceived.load() == 1,
            "actual processSmsContent persists history even for a rejecting empty whitelist");
    require(receiveHttp == expected && receiveMqtt == expected && receiveEmail == expected,
            "number/content empty white rejects; empty black/disabled forwards to all existing channels");
  }
}

static void fullHistoryTests() {
  resetState();
  const char* time = "2026-11-01 12:34:56";
  std::string chinese;
  for (int i = 0; i < 70; ++i) chinese += u8"中";
  const std::string body = chinese + "\r\nOTP\n123456\nhttps://example.test/tail\n<img src=x onerror=window.__historyInjected++>\n\"\\";
  addSmsToHistory("10086", body.c_str(), time);
  String one = getSmsHistory();
  require(one.length() > body.size() && one.indexOf("https://example.test/tail") >= 0, "real history retains >200 bytes, final OTP/URL and valid UTF8");
  JsonInput parsed; String decoded;
  require(parsed.parse(one.substring(1, one.length() - 1)) && parsed.getString("m", decoded) && decoded == String(body), "actual writer/reader JSON decodes to exact original text");
  const std::string persisted = SPIFFS.contents("/sms.txt");
  require(persisted.find(chinese) != std::string::npos, "no cut through multibyte boundary");
  historyReserveFailure = 0;
  addSmsToHistory("10086", "refuse allocation", time);
  require(SPIFFS.contents("/sms.txt") == persisted, "writer allocation failure leaves existing history unchanged");
  historyIo.writeLimit = 5;
  addSmsToHistory("10086", "partial", time);
  historyIo = {};
  addSmsToHistory("10086", "recovery", time);
  require(getSmsHistory().indexOf("recovery") >= 0 && getSmsHistory().indexOf("partial") < 0, "short write is skipped and next record recovers");
  historyIo.readLimit = 0;
  require(getSmsHistory().length() == 0, "read failure is distinguishable from an empty history");
  historyIo = {};
  historyReserveFailure = 0;
  require(getSmsHistory().length() == 0, "reader reserve failure returns no partial JSON");
  ESP.maxHeap = 31;
  server.reset(); handleSmsHistory();
  require(server.statusCode == 503 && server.streamedBody().empty(), "history HTTP reports low-memory failure without success data");
  ESP = TestEsp{};

  resetState();
  std::string large(SMS_HISTORY_MAX_MESSAGE_BYTES, 'x');
  large.replace(large.size() - 6, 6, "654321");
  addSmsToHistory("10086", large.c_str(), time);
  require(getSmsHistory().indexOf("654321") >= 0, "maximum supported SMS body retains its suffix");
  const auto before = SPIFFS.contents("/sms.txt");
  large += 'x'; addSmsToHistory("10086", large.c_str(), time);
  addSmsToHistory("10086", "invalid\xc3", time);
  require(SPIFFS.contents("/sms.txt") == before, "oversize and invalid UTF8 reject the whole new record");
  large.pop_back();
  for (int i = 0; i < 40; ++i) addSmsToHistory("10086", large.c_str(), time);
  require(SPIFFS.contents("/sms.txt").size() <= SMS_HISTORY_MAX_FILE_BYTES, "complete history file stays within byte budget");
  const String bounded = getSmsHistory();
  require(bounded.length() <= SMS_HISTORY_MAX_RESPONSE_BYTES && bounded.startsWith("[") && bounded.endsWith("]"), "large history response remains bounded and complete");
  require(bounded.indexOf("654321") >= 0, "budget drops old records, never slices current body");

  resetState();
  addSmsToHistory("10086", body.c_str(), time); addSmsToHistory("10010", chinese.c_str(), "2026-11-01 12:35:00");
  addCallToHistory("10086", time);
  server.reset(); handleSmsHistory();
  require(server.statusCode == 200 && server.terminators == 1 && server.streamedBody().find("123456") != std::string::npos, "actual history endpoint streams complete JSON");
  std::ofstream("history_fixture.json", std::ios::binary) << server.streamedBody();
  server.reset(); server.args = {{"type", "wifi"}}; WiFi.ssid = "office\"wifi\\path\nnext";
  handleQuery();
  require(server.statusCode == 200 && parsed.parse(server.body) && parsed.getString("message", decoded) &&
          decoded.indexOf(WiFi.ssid) >= 0, "actual query JSON preserves quotes/backslash/newline in SSID");
  std::ofstream("query_fixture.json", std::ios::binary) << server.body.c_str();
}

static void callFilterTests() {
  for (bool enabled : {false, true}) for (bool white : {false, true}) {
    for (const char* list : {"", " ,\t, ", "10086", "99999", "未知号码"}) {
      for (bool unknown : {false, true}) for (bool clcc : {false, true}) {
        resetState(); config.filterEnabled = enabled; config.filterIsWhitelist = white; config.filterList = list;
        const std::string number = unknown ? "" : "10086";
        const std::string line = clcc ? "+CLCC: 1,1,4,0,0,\"" + number + "\",129" : "+CLIP: \"" + number + "\",129,,,,0";
        require(handleCallUrc(String(line)), "real call handler recognizes CLIP/CLCC");
        const bool matched = !unknown && std::string(list) == "10086";
        const bool filtered = enabled && (unknown ? white : white ? !matched : matched);
        require(stats.callsReceived.load() == (filtered ? 0U : 1U) && receiveMqtt == (filtered ? 0U : 1U) &&
                receiveHttp == receiveMqtt && receiveEmail == receiveMqtt, "known/unknown call whitelist matrix covers storage and all forwarding channels");
        handleCallUrc(String(line));
        require(receiveMqtt <= 1, "existing call dedup still applies");
      }
    }
  }
}

static void rootTransportTests() {
  resetState(); handleRoot();
  const std::string plain = server.streamedBody();
  require(server.statusCode == 200 && !plain.empty() && server.terminators == 1,
          "HTTP/1.0 root finishes one complete streamed page");
  server.reset(); server.httpVersion = "HTTP/1.1"; handleRoot();
  require(server.statusCode == 200 && server.terminators == 1, "HTTP/1.1 success terminates once");
  const std::string framed = server.streamedBody();
  size_t offset = 0; std::string decoded;
  while (offset < framed.size()) {
    const size_t end = framed.find("\r\n", offset);
    require(end != std::string::npos, "chunk has a length header");
    const size_t size = std::strtoul(framed.substr(offset, end - offset).c_str(), nullptr, 16);
    offset = end + 2;
    require(size > 0 && size <= WebStream::BUFFER_SIZE && offset + size + 2 <= framed.size(), "bounded complete chunk");
    decoded.append(framed, offset, size); offset += size;
    require(framed.substr(offset, 2) == "\r\n", "chunk delimiter preserved"); offset += 2;
  }
  require(decoded == plain, "HTTP/1.1 framing does not alter encoded page bytes");
  for (const char* version : {"HTTP/1.0", "HTTP/1.1"}) {
    server.reset(); server.httpVersion = version; server.client().maxWrite = 1; handleRoot();
    require(server.client().stopped && server.client().calls == 1 && server.terminators == 0,
            "short header/body closes client and never emits successful terminator");
  }
  for (int failure : {0, 1}) {
    resetState();
    if (failure == 0) ESP.freeHeap = 16383;
    else ESP.maxHeap = 4095;
    handleRoot();
    require(server.statusCode == 503 && server.streamedBody().empty() && server.terminators == 0,
            "low total/contiguous heap rejects before streaming");
  }
}

int main() {
  const std::vector<std::pair<const char*, std::function<void()>>> suites = {
    {"UART RX order, capacity, setter failure and begin allocation failure", uartTests},
    {"64-bit 49/50/60 days, two millis wraps, boundaries and wide interval", longTimerTests},
    {"success/error/timeout counters, persistence, MQTT and no busy retry", resultTests},
    {"boot/reset/full/dedicated save consistency and Web 64-bit countdown", resetAndWebTests},
    {"full save preserves unchanged schedules and resets only changed timer settings", fullSaveTimerTests},
    {"number/content filter matrix, normalized empty entries and NVS round trip", filterTests},
    {"actual receive path records history before either filter", historyTests},
    {"full bounded UTF8 history, faults, streaming endpoint and escaped query", fullHistoryTests},
    {"actual CLIP/CLCC known and unknown caller filter matrix", callFilterTests},
    {"root HTTP/1.0/1.1 framing, short writes and low-heap refusal", rootTransportTests}
  };
  int failures = 0;
  for (const auto& suite : suites) {
    try { suite.second(); std::cout << "PASS: " << suite.first << '\n'; }
    catch (const std::exception& error) { failures++; std::cerr << "FAIL: " << suite.first << ": " << error.what() << '\n'; }
  }
  std::cout << checks << " assertions, " << suites.size() << " suites, " << failures << " failures.\n";
  return failures ? 1 : 0;
}
