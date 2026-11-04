// Reuse the exact production functions and existing regressions, not mirrored handlers.
#define WEB_REGRESSION_REUSE
#include "json_handlers_regression.cpp"
#include <fstream>
#include <filesystem>

static std::string repeat(const std::string& part, size_t count) {
  std::string result;
  while (count--) result += part;
  return result;
}
static std::vector<String*> storedStrings(Config& value) {
  std::vector<String*> result = {&value.smtpServer, &value.smtpUser, &value.smtpPass, &value.smtpSendTo,
    &value.webUser, &value.webPass, &value.timerPhone, &value.timerMessage, &value.mqttServer,
    &value.mqttUser, &value.mqttPass, &value.mqttPrefix, &value.mqttHaPrefix, &value.filterList, &value.contentFilterList};
  for (auto& wifi : value.wifiNetworks) { result.push_back(&wifi.ssid); result.push_back(&wifi.password); }
  for (auto& channel : value.pushChannels) {
    for (String* field : {&channel.name, &channel.url, &channel.key1, &channel.key2, &channel.customBody}) result.push_back(field);
  }
  return result;
}
static void noRuntimeEffects(const Config& before, uint64_t origin = 44, uint64_t seconds = 555) {
  require(sameConfig(config, before), "failure restores every RAM config field");
  require(lastTimerExec == origin && timerIntervalSec == seconds && configValid, "failure restores timer and validity");
  require(sends.empty() && atCommands.empty() && statsSaves == 0 && stats.smsSent == 7 && ESP.restarts == 0,
          "failed save cannot execute actions, count or restart");
  require(!preferences.isOpen() && ConfigLockFaults::held == 0, "failure releases namespace and lock");
  require(Serial.log.find("配置已保存") == std::string::npos, "failure cannot log success");
}

struct SaveCase { Handler handler; const char* json; const char* name; };
static const SaveCase saveCases[] = {
  {handleSave, nullptr, "all"},
  {handleTimer, R"({"enabled":false,"type":0,"interval":60,"phone":"new-phone","message":"new timer"})", "timer"},
  {handleFilterSave, R"({"enabled":false,"whitelist":true,"numbers":"new-number"})", "filter"},
  {handleContentFilterSave, R"({"enabled":true,"whitelist":false,"keywords":"new-keyword"})", "content"}
};
static void prepare(const SaveCase& item) {
  server.reset();
  if (item.json) server.args = {{"plain", item.json}};
  else server.args = {{"webUser", "new-user"}, {"webPass", "synthetic-new-secret"},
    {"wifi0ssid", "new-wifi"}, {"wifi0pass", "synthetic-new-wifi"}, {"wifi0en", "true"},
    {"smtpEn", "true"}, {"smtpServer", "mail.example.test"}, {"smtpPort", "587"},
    {"smtpUser", "user@example.test"}, {"smtpPass", "synthetic-new-smtp"}, {"smtpSendTo", "to@example.test"},
    {"push0en", "true"}, {"push0type", "4"}, {"push0url", "https://example.test/push"}, {"push0name", "new-channel"},
    {"push0k1", "synthetic-key"}, {"push0k2", "group"}, {"push0body", "new-body"},
    {"mqttEn", "true"}, {"mqttServer", "broker.example.test"}, {"mqttPort", "1883"}, {"mqttUser", "new-mqtt"},
    {"mqttPass", "synthetic-new-mqtt"}, {"mqttPrefix", "new-prefix"}, {"mqttCtrlOnly", "true"},
    {"mqttHaDiscovery", "true"}, {"mqttHaPrefix", "new-ha"},
    {"filterEn", "false"}, {"filterIsWhitelist", "true"}, {"filterList", "new-number"},
    {"contentFilterEn", "true"}, {"contentFilterIsWhitelist", "false"}, {"contentFilterList", "new-keyword"},
    {"timerEn", "false"}, {"timerType", "0"}, {"timerInterval", "60"}, {"timerPhone", "new-phone"}, {"timerMessage", "new timer"}};
}

static void nvsContractTests() {
  resetState();
  require(preferences.begin("limits", false), "open synthetic NVS");
  require(preferences.putString("value", std::string(3999, 'x').c_str()) == 3999, "NVS accepts 3999 payload bytes");
  require(preferences.putString("value", std::string(4000, 'x').c_str()) == 0 && preferences.getString("value").length() == 3999,
          "NVS rejects 4000 without truncation or overwrite");
  require(preferences.putString("1234567890123456", "x") == 0, "NVS enforces 15 byte keys");
  require(preferences.putString("empty", "") == 0 && preferences.isKey("empty"), "empty successful write returns zero");
  char empty[1] = {'x'};
  require(preferences.getString("empty", empty, 1) == 1 && !empty[0], "empty read includes NUL");
  require(preferences.putInt("int", 1) == 4 && preferences.putBool("bool", false) == 1 && preferences.putUChar("char", 7) == 1,
          "scalar writes use real Preferences widths");
  preferences.end();

  const size_t fields = storedStrings(config).size();
  require(fields == 36, "all current persistent string fields enumerated independently");
  // U+1F600 occupies four UTF-8 bytes: 999 * 4 + 3 == 3999; 1000 * 4 == 4000.
  const std::vector<std::string> valid = {std::string(3999, 'a'), repeat(u8"中", 1333), repeat(u8"\U0001F600", 999) + "abc"};
  const std::vector<std::string> invalid = {std::string(4000, 'a'), repeat(u8"中", 1333) + "a", repeat(u8"\U0001F600", 1000), std::string("a\0b", 3)};
  for (size_t field = 0; field < fields; ++field) {
    for (const auto& value : valid) {
      resetState(); *storedStrings(config)[field] = String(value);
      Config expected = config;
      const char* error = "stale";
      require(configFitsNvs(config) && saveConfig(&error) && !error, "every persistent field accepts its UTF-8 byte boundary");
      config = Config{}; loadConfig();
      require(*storedStrings(config)[field] == *storedStrings(expected)[field], "accepted field survives real save/load");
      require(lastTimerExec == testClockMs, "successful load still resets the timer origin");
    }
    for (const auto& value : invalid) {
      resetState(); *storedStrings(config)[field] = String(value);
      const char* error = nullptr;
      require(!configFitsNvs(config) && !saveConfig(&error) && error, "every persistent field rejects oversize/NUL");
      require(preferences.saves == 0 && preferences.writeAttempts == 0 && ConfigLockFaults::takes == 0,
              "all fields validated before lock or any write");
    }
  }
  resetState();
  require(saveConfig(), "old no-argument saveConfig source API remains usable");
  const auto keys = preferences.attemptedKeys;
  require(std::all_of(keys.begin(), keys.end(), [](const std::string& key) { return key.size() <= 15; }), "all actual emitted NVS keys fit");

  resetState(); preferences.begin("sms_config", false);
  preferences.putString("wifi0ssid", "legacy-ssid"); preferences.putString("wifi0pass", "synthetic-legacy-secret");
  preferences.putBool("wifi0en", true); preferences.end(); loadConfig();
  require(config.wifiNetworks[0].ssid == "legacy-ssid" && config.wifiNetworks[0].enabled, "legacy WiFi keys still load");
  preferences.failKey = "cfList"; require(!saveConfig(), "late save failure reported");
  preferences.begin("sms_config", true); require(preferences.isKey("wifi0ssid"), "legacy keys survive failed migration"); preferences.end();
  preferences.failKey.clear(); require(saveConfig(), "migration retry succeeds");
  preferences.begin("sms_config", true);
  require(!preferences.isKey("wifi0ssid") && preferences.getString("w_s_0") == "legacy-ssid", "legacy cleanup only after full successful write");
  preferences.end();

  resetState(); StringFaults::reset(); require(saveConfig(), "save with checked const-char strings");
  require(StringFaults::copyCalls == 0 && StringFaults::reserveCalls == 0 && StringFaults::concatCalls == 0,
          "save keys, values and empty readback need no hidden String allocations");
}

static void saveFailureTests() {
  resetState(); require(saveConfig(), "discover actual persistence keys");
  const auto keys = preferences.attemptedKeys;
  for (const SaveCase& item : saveCases) {
    // Fail each actual put, not just the final list write. Empty strings exercise readback too.
    for (const auto& key : keys) {
      resetState(); const Config before = config; prepare(item); preferences.failKey = key;
      item.handler(); expectWebReply(503, false); noRuntimeEffects(before);
      require(preferences.attemptedKeys.back() == key, "stop writing after first failing key");
      preferences.failKey.clear(); server.reset(); prepare(item); item.handler(); expectWebReply(200, true);
      require(ConfigLockFaults::held == 0 && !preferences.isOpen(), "retry can acquire and release storage");
    }
    for (int mode : {0, 1, 2, 3}) {
      resetState(); const Config before = config; prepare(item);
      if (mode == 0) ConfigLockFaults::failCreate = true;
      if (mode == 1) ConfigLockFaults::failTake = true;
      if (mode == 2) preferences.failBegin = true;
      if (mode == 3) preferences.shortWriteKey = "webPass";
      item.handler(); expectWebReply(503, false); noRuntimeEffects(before);
      if (mode < 2) require(preferences.saves == 0 && ConfigLockFaults::gives == 0, "never unlock or write when lock acquisition failed");
      if (mode == 2) require(preferences.writeAttempts == 0, "begin failure never writes");
    }
    // An unchanged empty string may read back as empty even when put returns zero: desired state is already present.
    resetState(); require(saveConfig(), "seed config before late persistent failure");
    const Config before = config; const uint64_t origin = lastTimerExec, seconds = timerIntervalSec;
    Serial.log.clear(); prepare(item); preferences.failKey = "cfList";
    item.handler(); expectWebReply(503, false); noRuntimeEffects(before, origin, seconds);
  }

  // Empty clear failure must not be mistaken for successful zero-length putString.
  for (int mode : {0, 1}) {
    resetState(); require(saveConfig(), "seed nonempty lists");
    const Config before = config; Serial.log.clear();
    preferences.failKey = mode ? "timerMsg" : "cfList";
    webRequest(mode ? handleTimer : handleContentFilterSave,
      mode ? R"({"enabled":false,"message":""})" : R"({"enabled":true,"whitelist":true,"keywords":""})");
    expectWebReply(503, false); noRuntimeEffects(before);
    preferences.failKey.clear();
    webRequest(mode ? handleTimer : handleContentFilterSave,
      mode ? R"({"enabled":false,"message":""})" : R"({"enabled":true,"whitelist":true,"keywords":""})");
    expectWebReply(200, true); config = Config{}; loadConfig();
    require(mode ? config.timerMessage.length() == 0 : config.contentFilterList.length() == 0,
            "explicit empty persists rather than restoring a nonempty default");
  }
  resetState(); preferences.failRead = true;
  require(!saveConfig() && ConfigLockFaults::held == 0, "empty readback failure is not success");

  // All implicit full-save snapshot and provided-argument copies are fallible.
  resetState(); prepare(saveCases[0]); size_t copiesBeforeWrite = 0;
  preferences.beforeSave = [&] { copiesBeforeWrite = StringFaults::copyCalls; };
  StringFaults::reset(); handleSave(); expectWebReply(200, true);
  require(copiesBeforeWrite > 36, "copy probe includes snapshot and form values");
  for (size_t fail = 0; fail < copiesBeforeWrite; ++fail) {
    resetState(); const Config before = config; prepare(saveCases[0]);
    server.args["timerEn"] = "true"; server.args["timerType"] = "1";
    StringFaults::copyAfter = static_cast<int>(fail);
    handleSave(); StringFaults::reset();
    require(server.statusCode == 400 || server.statusCode == 503, "failed String copy cannot claim success");
    expectWebReply(server.statusCode, false); noRuntimeEffects(before);
    require(preferences.saves == 0, "failed copy cannot persist any candidate");
  }
  // Failed full save including a new Web password leaves old RAM credentials usable.
  resetState(); const String priorUser = config.webUser, priorPassword = config.webPass;
  prepare(saveCases[0]); preferences.failKey = "cfList"; handleSave(); expectWebReply(503, false);
  require(config.webUser == priorUser && config.webPass == priorPassword, "failed full save keeps prior login credentials");
  require(Serial.log.find("synthetic-") == std::string::npos && server.body.find("synthetic-") == std::string::npos,
          "save logs and errors never include synthetic credentials");
  resetState(); const Config before = config; server.args = {{"contentFilterList", "next"}};
  preferences.failBegin = true; handleSave(); expectWebReply(503, false); noRuntimeEffects(before);
  preferences.failBegin = false; handleSave(); expectWebReply(200, true);
  require(lastTimerExec == 44 && timerIntervalSec == 555, "full save without timer never resets it on failure or success");
}

static std::string jsonWithText(const SaveCase& item, const std::string& value) {
  if (item.handler == handleTimer) return "{\"enabled\":false,\"message\":\"" + value + "\"}";
  if (item.handler == handleFilterSave) return "{\"enabled\":true,\"whitelist\":true,\"numbers\":\"" + value + "\"}";
  return "{\"enabled\":true,\"whitelist\":true,\"keywords\":\"" + value + "\"}";
}
static void handlerLengthTests() {
  const std::vector<std::string> valid = {std::string(3999, 'x'), repeat(u8"中", 1333), repeat(u8"\U0001F600", 999) + "abc"};
  for (const SaveCase& item : saveCases) {
    for (const auto& value : valid) {
      resetState();
      if (item.json) webRequest(item.handler, jsonWithText(item, value));
      else { server.args = {{"contentFilterList", String(value)}}; handleSave(); }
      expectWebReply(200, true);
      const String stored = item.handler == handleTimer ? config.timerMessage :
        (item.handler == handleFilterSave ? config.filterList : config.contentFilterList);
      require(text(stored) == value && stored.length() == 3999, "handler accepts exact UTF-8 storage budget without rewriting");
      server.reset(); config = Config{}; loadConfig();
      require(text(item.handler == handleTimer ? config.timerMessage :
        (item.handler == handleFilterSave ? config.filterList : config.contentFilterList)) == value, "dedicated/full accepted value survives load");
    }
    for (const auto& value : {std::string(4000, 'x'), repeat(u8"中", 1333) + "a", repeat(u8"\U0001F600", 1000)}) {
      resetState(); const Config before = config;
      if (item.json) webRequest(item.handler, jsonWithText(item, value));
      else { server.args = {{"contentFilterList", String(value)}}; handleSave(); }
      expectWebReply(400, false); noWebEffects(before);
    }
  }
  const std::vector<const char*> ordinary = {"smtpServer", "smtpUser", "smtpPass", "smtpSendTo", "webUser", "webPass",
    "timerPhone", "timerMessage", "mqttServer", "mqttUser", "mqttPass", "mqttPrefix", "mqttHaPrefix", "filterList", "contentFilterList",
    "wifi0ssid", "wifi0pass", "wifi1ssid", "wifi1pass", "wifi2ssid", "wifi2pass",
    "push0name", "push0url", "push0k1", "push0k2", "push0body", "push1name", "push1url", "push1k1", "push1k2", "push1body",
    "push2name", "push2url", "push2k1", "push2k2", "push2body"};
  for (const char* name : ordinary) {
    resetState(); const Config before = config; prepare(saveCases[0]); server.args[name] = String(repeat(u8"中", 1334));
    handleSave(); expectWebReply(400, false); noWebEffects(before);
  }
  // Budget after separator normalization, not the JSON source escape length or glyph count.
  for (const SaveCase& item : {saveCases[2], saveCases[3]}) {
    resetState(); webRequest(item.handler, jsonWithText(item, repeat(u8"，", 2000)));
    expectWebReply(200, true);
    require((item.handler == handleFilterSave ? config.filterList : config.contentFilterList).length() == 2000,
            "normalized separators consume actual persisted bytes");
  }
  resetState(); webRequest(handleContentFilterSave, R"({"enabled":true,"whitelist":true,"keywords":")" + repeat("\\u4e2d", 1333) + "\"}");
  expectWebReply(200, true); require(config.contentFilterList.length() == 3999, "Unicode escapes budget decoded UTF-8 bytes");
  webReject(handleContentFilterSave, R"({"enabled":true,"whitelist":true,"keywords":")" + repeat("\\u4e2d", 1334) + "\"}");
  resetState(); webRequest(handleTimer, R"({"enabled":false,"phone":")" + std::string(4000, 'x') + "\"}");
  expectWebReply(400, false);
  resetState();
  server.args = {{"contentFilterList", String(std::string("x\0y", 3))}}; handleSave(); expectWebReply(400, false);
  for (const char* field : {"filterList", "contentFilterList"}) {
    resetState(); server.args = {{"filterEn", "true"}, {field, String(repeat(u8"，", 2000))}};
    handleSave(); expectWebReply(200, true);
    require((std::strcmp(field, "filterList") == 0 ? config.filterList : config.contentFilterList).length() == 2000,
            "full-save validation uses normalized byte count, matching dedicated handlers");
  }
}

static std::string sinkOutput;
static size_t sinkCalls = 0, sinkMaxChunk = 0;
static int failSinkAt = -1;
static bool sink(const char* data, size_t length) {
  require(length > 0 && length <= WebStream::BUFFER_SIZE, "encoded output stays in fixed nonempty chunks");
  sinkCalls++; sinkMaxChunk = std::max(sinkMaxChunk, length);
  if (failSinkAt >= 0 && static_cast<int>(sinkCalls) == failSinkAt) return false;
  sinkOutput.append(data, length); return true;
}
static std::string encode(const std::string& input, WebEncoding encoding) {
  sinkOutput.clear(); sinkCalls = sinkMaxChunk = 0; failSinkAt = -1;
  WebOutput out(sink); require(out.encoded(input.c_str(), encoding) && out.flush(), "production encoder succeeds");
  return sinkOutput;
}
static void outputTests() {
  const std::string attack = u8"O'Reilly\r\n\\\"&<>\t\x01 中文\u2028\u2029</ScRiPt><!--<script>";
  const std::string js = encode(attack, WebEncoding::JsString);
  require(js.find('<') == std::string::npos && js.find('&') == std::string::npos && js.find('\n') == std::string::npos,
          "inline string cannot close, comment or double-escape the script element");
  JsonInput decoder; String value;
  require(decoder.parse(String("{\"value\":" + js + "}")) && decoder.getString("value", value) && text(value) == attack,
          "JS string encoding preserves quotes, slashes, controls, Unicode and line separators");
  require(encode("\"'&<>\r\n\t", WebEncoding::HtmlAttribute) == "&quot;&#39;&amp;&lt;&gt;&#13;&#10;&#9;", "attribute context escapes both quotes and controls");
  require(encode("\"'&<>\r\n", WebEncoding::HtmlText) == "\"'&amp;&lt;&gt;&#13;\n", "text and textarea context keeps text without entity decoding");
  const std::string expanded = encode(std::string(3999, '<'), WebEncoding::JsString);
  require(expanded.size() == 3999 * 6 + 2 && sinkMaxChunk == 1024, "worst-case encoding expansion remains streaming");
  sinkOutput.clear(); sinkCalls = 0;
  WebOutput out(sink);
  const WebOutputValue values[] = {{"%A%", "%B% </script>", WebEncoding::JsString}, {"%B%", "unwanted"}};
  const std::string templateText = "x:%A%; width:50%; %UNKNOWN%";
  require(out.render(templateText.data(), templateText.size(), values, 2) && out.flush(), "render complete fixture");
  require(sinkOutput.find("%B% \\u003c/script\\u003e") != std::string::npos && sinkOutput.find("unwanted") == std::string::npos &&
          sinkOutput.find("50%; %UNKNOWN%") != std::string::npos, "values are opaque, not rescanned for template tokens");
  sinkOutput.clear(); sinkCalls = 0; failSinkAt = 2;
  WebOutput broken(sink);
  require(!broken.encoded(std::string(3999, '<').c_str(), WebEncoding::JsString) && !broken.flush() && sinkCalls == 2,
          "short output failure propagates without a success terminator or further writes");
  failSinkAt = -1;
}

static const std::string hostile = u8"O'Reilly &amp; \" autofocus onfocus=\"window.__webInjected++\" data-probe=\"'\\ <b>text</b></textarea><img src=x onerror=\"window.__webInjected++\"></ScRiPt><script>window.__webInjected++</script><!--<script>\nnext\r\nlast\t\u2028\u2029 中文 %CF_LIST%";
static void hostileConfig() {
  resetState();
  for (String* value : storedStrings(config)) *value = String(hostile);
  config.mqttEnabled = true; config.timerEnabled = true; config.timerType = 1; config.timerInterval = 50;
  config.filterEnabled = true; config.contentFilterEnabled = true;
  for (auto& wifi : config.wifiNetworks) wifi.enabled = true;
  for (auto& channel : config.pushChannels) { channel.enabled = true; channel.type = PUSH_TYPE_CUSTOM; }
  config.pushChannels[0].customBody = String("\n" + hostile);
  WiFi.ssid = String(hostile);
  mqttTopicStatus = mqttTopicSmsReceived = mqttTopicCallReceived = String(hostile);
  require(saveConfig(), "persist hostile config with actual production save");
  config = Config{}; loadConfig(); // Real load normalization, then actual streaming renderer.
  server.reset(); handleRoot();
  require(server.statusCode == 200, "actual root renders persisted hostile config");
}
static void rootOutputTests() {
  hostileConfig();
  const std::string html = server.streamedBody();
  require(html.find("name=\"webUser\" value=\"O&#39;Reilly &amp;amp; &quot; autofocus onfocus=&quot;") != std::string::npos,
          "attribute payload uses attribute encoding, not text encoding");
  require(html.find("</ScRiPt>") == std::string::npos, "case-insensitive script closure from config is encoded");
  require(html.find("&lt;/textarea&gt;") != std::string::npos && html.find("\\u003c/ScRiPt\\u003e") != std::string::npos,
          "actual root uses distinct HTML and JS encoding contexts");
  require(html.find("ls:\"O\\u0027Reilly") != std::string::npos && html.find("window.__webInjected++") != std::string::npos,
          "data retained rather than filtered/removed");
  require(html.find("%PUSH_CHANNELS%") == std::string::npos && html.find("%WIFI_NETWORKS%") == std::string::npos,
          "layout placeholders fully rendered");
  server.reset(); server.authorized = false; handleRoot();
  require(server.statusCode == 401 && server.streamedBody().empty(), "root auth precedes output");
}

static void writeFile(const std::filesystem::path& path, const std::string& content) {
  std::ofstream file(path, std::ios::binary); file << content;
  require(file.good(), "write synthetic browser fixture");
}
static void writeReply(const std::filesystem::path& directory, const std::string& name) {
  writeFile(directory / (name + ".json"), "{\"status\":" + std::to_string(server.statusCode) + ",\"body\":" + server.body + "}");
}
static void browserFixtures(const std::filesystem::path& directory) {
  hostileConfig();
  writeFile(directory / "page.html", server.streamedBody());
  WiFi.modeValue = WIFI_AP; server.reset(); handleRoot();
  writeFile(directory / "page-ap.html", server.streamedBody());
  std::string expected = "{\"hostile\":" + encode(hostile, WebEncoding::JsString);
  expected += ",\"timerPhone\":" + encode(text(config.timerPhone), WebEncoding::JsString);
  expected += ",\"timerMessage\":" + encode(text(config.timerMessage), WebEncoding::JsString);
  expected += ",\"body\":" + encode(text(config.pushChannels[0].customBody), WebEncoding::JsString) + "}";
  writeFile(directory / "expected.json", expected);
  for (const SaveCase& item : saveCases) {
    resetState();
    if (item.json) webRequest(item.handler, jsonWithText(item, std::string(4000, 'x')));
    else { server.args = {{"contentFilterList", String(std::string(4000, 'x'))}}; handleSave(); }
    expectWebReply(400, false); writeReply(directory, std::string(item.name) + "-400");
    resetState(); prepare(item); ConfigLockFaults::failTake = true; item.handler();
    expectWebReply(503, false); writeReply(directory, std::string(item.name) + "-503");
    resetState(); prepare(item); item.handler(); expectWebReply(200, true);
    writeReply(directory, std::string(item.name) + "-200");
  }
}

int main(int argc, char** argv) {
  const std::vector<std::pair<const char*, std::function<void()>>> suites = {
    // Also execute pre-existing suites so shared fixtures/helpers remain exercised under -Werror.
    {"shared JSON/parser", parserTests}, {"Web/MQTT SMS", smsTests}, {"timer compatibility", timerTests},
    {"filter semantics", filterTests}, {"MQTT controls", mqttControlTests}, {"JSON boundaries", strictBoundaryTests},
    {"JSON OOM", allocationTests}, {"full save/load", fullSaveAndLoadTests},
    {"real NVS byte/return/key contracts and all stored fields", nvsContractTests},
    {"every write failure, lock/begin failure, RAM rollback and retry", saveFailureTests},
    {"all form fields plus dedicated UTF-8/normalized storage boundaries", handlerLengthTests},
    {"context encoder, expansion, opaque replacement and short sink", outputTests},
    {"actual persisted hostile config through complete root renderer", rootOutputTests}
  };
  int failures = 0;
  for (const auto& suite : suites) {
    try { suite.second(); std::cout << "PASS: " << suite.first << '\n'; }
    catch (const std::exception& error) { failures++; std::cerr << "FAIL: " << suite.first << ": " << error.what() << '\n'; }
    StringFaults::reset(); ConfigLockFaults::reset();
  }
  if (!failures && argc == 2) {
    try { browserFixtures(std::filesystem::u8path(argv[1])); }
    catch (const std::exception& error) { failures++; std::cerr << "FAIL: browser fixtures: " << error.what() << '\n'; }
  }
  std::cout << checks << " assertions, " << suites.size() << " suites, " << failures << " failures.\n";
  return failures ? 1 : 0;
}
