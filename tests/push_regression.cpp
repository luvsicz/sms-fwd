#include "push_host_stubs.h"
#include "push_service.h"
#include <iostream>

Config config{};
Statistics stats;
WiFiClientSecure ssl_client;
PushSmtp smtp;
std::vector<std::pair<uint32_t, uint32_t>> savedPushStats;
std::vector<std::string> pushDiagnostics;
bool pushDiagnosticActive = false;
size_t pushDiagnosticClears = 0;
void saveStats() {
  savedPushStats.emplace_back(stats.pushSuccess.load(), stats.pushFailed.load());
  pushEvents.emplace_back("stats.save");
}
void recordOperationDiagnostic(const char* operation, const char* stage) {
  pushDiagnostics.emplace_back(std::string(operation) + ":" + stage); pushDiagnosticActive = true;
}
void clearOperationDiagnostic() { ++pushDiagnosticClears; pushDiagnosticActive = false; }

// Crypto is outside these transport tests; no signing secret or real endpoint is used.
String hmacSha256Base64(const String&, const String&) { return "offline-signature"; }
#include "push_config_actual.inc"

// Only the clock is redirected here; the extracted production code is not rewritten.
#define time pushHostTime
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "push_actual.inc"
#pragma GCC diagnostic pop
#undef time

// Interpose allocation, not EmailJob or its constructor/destructor. Unrelated host STL allocations
// are not faulted. Tracking remains active through production delete, even after producer scope ends.
void* operator new(std::size_t size) {
  const bool job = pushJobs.active && (size == sizeof(EmailJob) || size == sizeof(HttpPushJob));
  if (job && ++pushJobs.calls == pushJobs.failAt) { ++pushJobs.faultHits; throw std::bad_alloc(); }
  void* p = std::malloc(size ? size : 1);
  if (!p) throw std::bad_alloc();
  if (job) pushJobs.remember(p, size);
  return p;
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try { return ::operator new(size); } catch (...) { return nullptr; }
}
void operator delete(void* p) noexcept { pushJobs.release(p); std::free(p); }
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }

namespace {
size_t assertions = 0, failures = 0, cases = 0, emptyStringInstances = 0;
std::string currentCase;
const char* subjectText = u8"  收到短信 snapshot subject A  ";
const char* bodyText = u8"  第一行\r\n第二行  body must not be trimmed  \n";
const char* serverA = "smtp-a.mail.invalid";
const char* userA = "sender-a@mail.invalid";
const char* passA = "offline-password-A";
const char* toA = "first-recipient@mail.invalid ; ;\tsecond-long-recipient@mail.invalid,,third@mail.invalid;";
const char* fieldNames[] = {"subject", "body", "smtpServer", "smtpUser", "smtpPass", "smtpSendTo"};
String Config::*smtpFields[] = {&Config::smtpServer, &Config::smtpUser, &Config::smtpPass, &Config::smtpSendTo};

void check(bool condition, const char* expression, int line) {
  ++assertions;
  if (!condition) { ++failures; std::cerr << "FAIL [" << currentCase << ':' << line << "] " << expression << '\n'; }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

void configureA() {
  config.emailEnabled = true;
  config.smtpServer = " \t smtp-a.mail.invalid \r\n";
  config.smtpUser = " sender-a@mail.invalid\t";
  config.smtpPass = "\r\n offline-password-A ";
  config.smtpSendTo = " first-recipient@mail.invalid ; ;\tsecond-long-recipient@mail.invalid,,third@mail.invalid; ";
  config.smtpPort = 587;
}
std::vector<std::string> configStrings() {
  std::vector<std::string> result;
  for (auto field : smtpFields) result.emplace_back((config.*field).c_str());
  return result;
}
void cleanupFixture() {
  pushJobs.active = false; PushStringHeap::resetProbe();
  if (emailQueue) {
    for (void* p : emailQueue->items) delete static_cast<EmailJob*>(p);
    delete emailQueue; emailQueue = nullptr;
  }
  if (httpPushQueue) {
    for (void* p : httpPushQueue->items) delete static_cast<HttpPushJob*>(p);
    delete httpPushQueue; httpPushQueue = nullptr;
  }
  // Failures are asserted BEFORE this recovery, so a regressed worker cannot hide a leak.
  for (size_t i = 0; i < 32; ++i) if (void* p = pushJobs.pointers[i]) {
    if (pushJobs.sizes[i] == sizeof(HttpPushJob)) delete static_cast<HttpPushJob*>(p);
    else delete static_cast<EmailJob*>(p);
  }
}
void resetFixture() {
  cleanupFixture();
  emailTaskHandle = nullptr; smtpMutex = nullptr;
  for (auto& handle : httpPushTaskHandles) handle = nullptr;
  pushJobs = {}; pushRtos = {}; pushHttp = {}; smtp = PushSmtp{};
  ssl_client.plain = false; ssl_client.stops = ssl_client.upgrades = 0;
  config = Config{}; configureA(); stats = Statistics{};
  savedPushStats.clear(); pushDiagnostics.clear(); pushDiagnosticActive = false; pushDiagnosticClears = 0;
  pushMillis = 0; pushEpoch = 1700000000; pushNtpCalls = pushYields = pushWdtChecks = pushWdtResets = 0;
  pushWdtStatus = ESP_OK; WiFi.state = WL_CONNECTED;
  PushStringHeap::resetProbe(); pushEvents.clear(); pushSerialLog.clear();
}
template<class F> void runCase(const std::string& name, F body) {
  currentCase = name; ++cases;
  const size_t before = failures;
  try { resetFixture(); body(); }
  catch (const std::exception& error) { check(false, error.what(), __LINE__); }
  catch (...) { check(false, "unexpected exception", __LINE__); }
  CHECK(pushJobs.live == 0);
  CHECK(emailQueue == nullptr || emailQueue->items.empty());
  CHECK(httpPushQueue == nullptr || httpPushQueue->items.empty());
  CHECK(!pushRtos.mutex.held && !pushDiagnosticActive);
  CHECK(smtp.message.headers.values.empty() && smtp.message.text.value.empty());
  CHECK(pushTlsAlive.size() == 1 && pushTlsAlive.count(&ssl_client) == 1);
  CHECK(PushStringHeap::guardsOk());
  config = Config{};
  CHECK(PushStringHeap::blocks.empty());
  CHECK(PushStringHeap::instances == emptyStringInstances);
  std::cout << (before == failures ? "PASS " : "FAIL ") << name << '\n';
  cleanupFixture();
}
void produce(const char* subject = subjectText, const char* body = bodyText) {
  struct Scope { Scope() { pushJobs.active = true; } ~Scope() { pushJobs.active = false; } } scope;
  sendEmailNotification(subject, body); // PUBLIC production producer, including failure persistence.
}
EmailJob& queued(size_t index = 0) {
  if (!emailQueue || emailQueue->items.size() <= index || !emailQueue->items.at(index)) {
    throw std::runtime_error("expected an independently owned queued EmailJob");
  }
  return *static_cast<EmailJob*>(emailQueue->items.at(index));
}
void drainWorker() {
  if (!emailTaskHandle || pushRtos.task.entry != emailWorkerTask) throw std::runtime_error("production task not created");
  const size_t waits = pushRtos.waits;
  try { pushRtos.task.entry(nullptr); }
  catch (const PushQueueDrained&) { CHECK(pushRtos.waits == waits + 1); return; }
  throw std::runtime_error("infinite production worker unexpectedly returned");
}
size_t eventIndex(const char* name) {
  const auto found = std::find(pushEvents.begin(), pushEvents.end(), name);
  CHECK(found != pushEvents.end());
  return static_cast<size_t>(found - pushEvents.begin());
}
void checkSaved(uint32_t success, uint32_t failed, size_t saves) {
  CHECK(stats.pushSuccess.load() == success && stats.pushFailed.load() == failed);
  CHECK(savedPushStats.size() == saves);
  if (saves) CHECK((savedPushStats.back() == std::make_pair(success, failed)));
}

void testSnapshotsAndQueue() {
  runCase("snapshot-A-then-B-and-disabled-before-consumption", [] {
    const auto sharedBefore = configStrings();
    std::string subject = subjectText, body = bodyText;
    for (int i = 0; i < 100; ++i) body += u8"长正文\r\n";
    const std::string originalBody = body;
    produce(subject.c_str(), body.c_str());
    const auto& a = queued();
    CHECK(configStrings() == sharedBefore); // producer normalization must not mutate Web config.
    CHECK(a.smtpServer == serverA && a.smtpUser == userA && a.smtpPass == passA && a.smtpSendTo == toA);
    CHECK(a.subject == subject.c_str() && a.body == originalBody.c_str() && a.smtpPort == 587);
    CHECK(a.smtpServer.c_str() != config.smtpServer.c_str() && a.body.c_str() != body.c_str());
    const uintptr_t serverPtr = reinterpret_cast<uintptr_t>(a.smtpServer.c_str());
    const uintptr_t userPtr = reinterpret_cast<uintptr_t>(a.smtpUser.c_str());
    const uintptr_t passPtr = reinterpret_cast<uintptr_t>(a.smtpPass.c_str());
    const uintptr_t subjectPtr = reinterpret_cast<uintptr_t>(a.subject.c_str());
    const uintptr_t bodyPtr = reinterpret_cast<uintptr_t>(a.body.c_str());
    subject.assign(500, 'B'); body.assign(5000, 'B'); // invalidate the caller's original buffers.
    config.smtpServer = "smtp-b.mail.invalid"; config.smtpUser = "sender-b@mail.invalid";
    config.smtpPass = "offline-password-B"; config.smtpSendTo = "only-b@mail.invalid"; config.smtpPort = 25;
    produce("subject B", "body B");
    config.emailEnabled = false; produce("must not queue", "disabled");
    CHECK(emailQueue->items.size() == 2 && smtp.connects == 0 && savedPushStats.empty());
    PushStringHeap::resetProbe();
    drainWorker();
    CHECK(PushStringHeap::copies == 0); // Neither the snapshot nor the recipient list is copied by worker.
    CHECK(smtp.attempts.size() == 2 && pushJobs.frees == 2 && pushRtos.gives == 2);
    const auto& sentA = smtp.attempts.at(0);
    CHECK(sentA.server == serverA && sentA.user == userA && sentA.pass == passA && sentA.port == 587);
    CHECK(sentA.ssl && sentA.startTls && sentA.from == "sms notify <sender-a@mail.invalid>");
    CHECK((sentA.to == std::vector<std::string>{"your_email <first-recipient@mail.invalid>",
        "your_email <second-long-recipient@mail.invalid>", "your_email <third@mail.invalid>"}));
    CHECK(sentA.subject == subjectText && sentA.body == originalBody);
    CHECK(sentA.serverSource == serverPtr && sentA.userSource == userPtr && sentA.passSource == passPtr);
    CHECK(sentA.subjectSource == subjectPtr && sentA.bodySource == bodyPtr);
    const auto& sentB = smtp.attempts.at(1);
    CHECK(sentB.server == "smtp-b.mail.invalid" && sentB.user == "sender-b@mail.invalid" && sentB.pass == "offline-password-B");
    CHECK(sentB.port == 25 && !sentB.ssl && !sentB.startTls && sentB.subject == "subject B" && sentB.body == "body B");
    CHECK((sentB.to == std::vector<std::string>{"your_email <only-b@mail.invalid>"}));
    CHECK(smtp.message.clears == 4 && smtp.stops == 2 && ssl_client.stops == 2 && pushDiagnosticClears == 2);
    CHECK(eventIndex("message.clear") < eventIndex("smtp.send") && eventIndex("smtp.stop") < eventIndex("lock.give"));
    checkSaved(2, 0, 2);
  });
  runCase("disabled-before-allocation-is-no-op", [] {
    config.emailEnabled = false; pushJobs.failAt = 1; PushStringHeap::failAt = 1;
    produce();
    CHECK(pushRtos.queueCreates == 0 && pushRtos.taskCreates == 0 && pushRtos.sends == 0);
    CHECK(pushJobs.calls == 0 && PushStringHeap::allocations == 0);
    checkSaved(0, 0, 0);
  });
  runCase("full-queue-releases-only-rejected-job", [] {
    for (size_t i = 0; i < EMAIL_QUEUE_LENGTH; ++i) produce();
    const size_t blocks = PushStringHeap::blocks.size();
    CHECK(pushJobs.live == EMAIL_QUEUE_LENGTH && savedPushStats.empty());
    produce();
    CHECK(emailQueue->items.size() == EMAIL_QUEUE_LENGTH && pushJobs.live == EMAIL_QUEUE_LENGTH);
    CHECK(pushJobs.calls == EMAIL_QUEUE_LENGTH + 1 && pushJobs.frees == 1 && PushStringHeap::blocks.size() == blocks);
    CHECK(pushRtos.sends == EMAIL_QUEUE_LENGTH + 1);
    checkSaved(0, 1, 1);
    drainWorker();
    CHECK(pushJobs.frees == EMAIL_QUEUE_LENGTH + 1 && smtp.attempts.size() == EMAIL_QUEUE_LENGTH);
    checkSaved(EMAIL_QUEUE_LENGTH, 1, EMAIL_QUEUE_LENGTH + 1);
  });
  runCase("worker-ignores-failed-and-null-receives", [] {
    produce(); emailQueue->items.push_front(nullptr); pushRtos.skipReceive = true;
    drainWorker();
    CHECK(pushRtos.receives == 4 && pushJobs.frees == 1 && smtp.sends == 1);
    checkSaved(1, 0, 1);
  });
}

void testProducerFailures() {
  for (const bool queueFailure : {true, false}) {
    runCase(queueFailure ? "queue-create-failure-and-retry" : "task-create-failure-and-retry", [=] {
      pushRtos.failQueue = queueFailure; pushRtos.failTask = !queueFailure;
      produce();
      CHECK(pushJobs.calls == 0 && pushRtos.sends == 0);
      checkSaved(0, 1, 1);
      pushRtos.failQueue = pushRtos.failTask = false; produce(); drainWorker();
      CHECK(pushRtos.queueCreates == (queueFailure ? 2U : 1U));
      CHECK(pushRtos.taskCreates == (queueFailure ? 1U : 2U));
      checkSaved(1, 1, 2);
    });
  }
  runCase("nothrow-EmailJob-new-failure-and-retry", [] {
    CHECK(ensureEmailWorker()); pushJobs.failAt = 1;
    produce();
    CHECK(pushJobs.calls == 1 && pushJobs.faultHits == 1 && pushJobs.live == 0 && pushJobs.frees == 0);
    CHECK(PushStringHeap::allocations == 0 && pushRtos.sends == 0);
    checkSaved(0, 1, 1);
    pushJobs.failAt = 0; produce(); drainWorker(); checkSaved(1, 1, 2);
  });
  for (const std::string fault : {"reserve", "concat", "short-concat"}) {
    for (size_t field = 0; field < 6; ++field) {
      runCase("snapshot-" + fault + "-" + fieldNames[field], [=] {
        CHECK(ensureEmailWorker());
        const size_t blocks = PushStringHeap::blocks.size(), instances = PushStringHeap::instances;
        const auto sharedBefore = configStrings();
        if (fault == "reserve") PushStringHeap::failAt = field + 1;
        if (fault == "concat") PushStringHeap::failConcat = field + 1;
        if (fault == "short-concat") PushStringHeap::shortConcat = field + 1;
        produce();
        CHECK(PushStringHeap::faultHits == 1 && PushStringHeap::allocations == field + 1);
        CHECK(pushJobs.calls == 1 && pushJobs.frees == 1 && pushJobs.live == 0 && pushRtos.sends == 0);
        CHECK(PushStringHeap::blocks.size() == blocks && PushStringHeap::instances == instances);
        CHECK(configStrings() == sharedBefore && smtp.connects == 0);
        checkSaved(0, 1, 1);
        PushStringHeap::resetProbe(); produce(); drainWorker(); checkSaved(1, 1, 2);
      });
    }
  }
  for (const bool subjectNull : {true, false}) {
    runCase(subjectNull ? "null-subject-releases-job" : "null-body-releases-job", [=] {
      produce(subjectNull ? nullptr : subjectText, subjectNull ? bodyText : nullptr);
      CHECK(pushJobs.frees == 1 && pushRtos.sends == 0); checkSaved(0, 1, 1);
    });
  }
  runCase("empty-subject-and-body-are-valid", [] {
    produce("", ""); drainWorker();
    CHECK(smtp.attempts.at(0).subject.empty() && smtp.attempts.at(0).body.empty()); checkSaved(1, 0, 1);
  });
}

void testSenderExits() {
  for (size_t field = 0; field < 4; ++field) {
    runCase(std::string("blank-config-") + fieldNames[field + 2], [=] {
      config.*smtpFields[field] = " \t\r\n"; produce(); drainWorker();
      CHECK(smtp.connects == 0 && pushRtos.takes == 0 && pushJobs.frees == 1); checkSaved(0, 1, 1);
    });
  }
  for (const std::string exit : {"wifi", "mutex-create", "lock-timeout"}) {
    runCase("early-exit-" + exit, [=] {
      produce();
      if (exit == "wifi") WiFi.state = 0;
      if (exit == "mutex-create") pushRtos.failMutex = true;
      if (exit == "lock-timeout") pushRtos.failTake = true;
      drainWorker();
      CHECK(smtp.connects == 0 && pushRtos.gives == 0 && pushJobs.frees == 1);
      CHECK(pushDiagnostics.empty()); checkSaved(0, 1, 1);
    });
  }
  struct Failure { PushSmtpFault fault; const char* name; int stage; bool timeout; };
  const Failure failuresToTry[] = {
    {PushSmtpFault::ConnectStart, "connect-start", 1, false},
    {PushSmtpFault::ConnectStatus, "connect-status", 1, false},
    {PushSmtpFault::ConnectDisconnected, "connect-disconnected", 1, false},
    {PushSmtpFault::ConnectTimeout, "connect-timeout", 1, true},
    {PushSmtpFault::AuthStart, "auth-start", 2, false},
    {PushSmtpFault::AuthStatus, "auth-status", 2, false},
    {PushSmtpFault::AuthUnconfirmed, "auth-unconfirmed", 2, false},
    {PushSmtpFault::AuthTimeout, "auth-timeout", 2, true},
    {PushSmtpFault::SendStart, "send-start", 3, false},
    {PushSmtpFault::SendStatus, "send-status", 3, false},
    {PushSmtpFault::SendIncomplete, "send-incomplete", 3, false},
    {PushSmtpFault::SendTimeout, "send-timeout", 3, true}
  };
  for (const auto failure : failuresToTry) {
    runCase(std::string("SMTP-") + failure.name + "-cleans-up", [=] {
      produce(); smtp.fault = failure.fault; drainWorker();
      CHECK(smtp.connects == 1 && smtp.auths == (failure.stage >= 2 ? 1U : 0U));
      CHECK(smtp.sends == (failure.stage == 3 ? 1U : 0U) && pushJobs.frees == 1);
      CHECK(smtp.stops >= 1 && ssl_client.stops >= 1 && pushRtos.gives == 1 && pushDiagnosticClears == 1);
      if (failure.stage == 3) CHECK(smtp.liveJobsAtSend == 1 && smtp.message.clears == 2);
      if (failure.timeout) CHECK(pushMillis >= SMTP_STAGE_TIMEOUT_MS && pushMillis < SMTP_STAGE_TIMEOUT_MS + 100);
      CHECK(eventIndex("smtp.stop") < eventIndex("lock.give") && eventIndex("lock.give") < eventIndex("stats.save"));
      checkSaved(0, 1, 1);
    });
  }
  for (const int port : {-1, 0, 25, 465, 587}) {
    runCase("SMTP-port-" + std::to_string(port), [=] {
      config.smtpPort = port; ssl_client.plain = true;
      produce(); drainWorker();
      const auto& sent = smtp.attempts.at(0);
      CHECK(sent.port == (port > 0 ? port : 465) && sent.ssl == (port != 25) && sent.startTls == (port == 587));
      CHECK(ssl_client.plain == (port == 25 || port == 587) && ssl_client.upgrades == (port == 587 ? 1U : 0U));
      CHECK(sent.timestamp == static_cast<uint32_t>(pushEpoch)); checkSaved(1, 0, 1);
    });
  }
  runCase("NTP-fallback-before-SMTP-connect", [] {
    produce(); pushEpoch = 0; drainWorker();
    CHECK(pushNtpCalls == 1 && smtp.connectAt >= 8000 && smtp.attempts.at(0).timestamp == 0);
    checkSaved(1, 0, 1);
  });
}

void testWorkerStringFailures() {
  runCase("ReadyMail-boundaries-borrow-validated-snapshot-without-temporary-Strings", [] {
    config.smtpUser = "u@mail.invalid";
    config.smtpSendTo = "first-long-recipient@mail.invalid;second-much-longer-recipient-address@mail.invalid";
    const std::string longSubject(512, 's'), longBody(8192, 'b');
    produce(longSubject.c_str(), longBody.c_str());
    const uintptr_t subject = reinterpret_cast<uintptr_t>(queued().subject.c_str());
    const uintptr_t body = reinterpret_cast<uintptr_t>(queued().body.c_str());
    PushStringHeap::resetProbe();
    // Only the three checked address-header growths may allocate in application code.
    // Old c_str() -> const String& calls would hit this next-allocation fault.
    PushStringHeap::failAt = 4;
    drainWorker();
    CHECK(PushStringHeap::allocations == 3 && PushStringHeap::faultHits == 0 && PushStringHeap::copies == 0);
    CHECK(smtp.attempts.size() == 1 && smtp.attempts.at(0).subject == longSubject && smtp.attempts.at(0).body == longBody);
    CHECK(smtp.attempts.at(0).subjectSource == subject && smtp.attempts.at(0).bodySource == body);
    CHECK(smtp.message.text.borrowed && smtp.liveJobsAtSend == 1 && pushJobs.frees == 1);
    checkSaved(1, 0, 1);
    PushStringHeap::resetProbe();
    produce("next subject", "next body"); drainWorker();
    CHECK(smtp.attempts.at(1).body == "next body" && pushJobs.frees == 2);
    checkSaved(2, 0, 2);
  });
  // Three growing headers force three real allocations in the no-SSO substitute.
  for (const std::string mode : {"reserve", "concat", "short-concat"}) {
    const size_t points = mode == "reserve" ? 3 : 9;
    for (size_t at = 1; at <= points; ++at) {
      runCase("worker-address-" + mode + "-" + std::to_string(at), [=] {
        config.smtpUser = "u@mail.invalid";
        config.smtpSendTo = "first-long-recipient@mail.invalid;second-much-longer-recipient-address@mail.invalid";
        produce(); PushStringHeap::resetProbe();
        if (mode == "reserve") PushStringHeap::failAt = at;
        if (mode == "concat") PushStringHeap::failConcat = at;
        if (mode == "short-concat") PushStringHeap::shortConcat = at;
        drainWorker();
        CHECK(PushStringHeap::faultHits == 1 && PushStringHeap::copies == 0);
        CHECK(smtp.connects == 1 && smtp.auths == 1 && smtp.sends == 0 && smtp.message.clears == 2);
        CHECK(smtp.stops == 1 && ssl_client.stops == 1 && pushRtos.gives == 1 && pushJobs.frees == 1);
        CHECK(pushDiagnosticClears == 1); checkSaved(0, 1, 1);
      });
    }
  }
}

void testHttpLifetime() {
  for (const bool tls : {false, true}) {
    for (const int code : {0, 200, 201, 202, 204, 299, 300, 404, 500, -1}) {
      runCase(std::string(tls ? "HTTPS-" : "HTTP-") + (code ? std::to_string(code) : "begin-fails"), [=] {
        pushHttp.beginOk = code != 0; pushHttp.code = code; pushHttp.stream.response.assign(4096, 'r');
        const int result = sendHttpRequest(tls ? "https://offline.invalid/push" : "http://offline.invalid/push",
                                          tls ? "POST" : "GET", tls ? "application/json" : "", "{\"test\":true}");
        CHECK(result == (code ? code : -1));
        CHECK(pushHttp.begins == 1 && pushHttp.requests == (code ? 1U : 0U) && pushHttp.ends == (code ? 1U : 0U));
        CHECK(pushHttp.destructors == 1 && pushHttp.lifetimeErrors == 0);
        CHECK(eventIndex("tls.construct") < eventIndex("http.construct"));
        CHECK(eventIndex("http.destroy") < eventIndex("tls.destroy"));
        const bool success = code >= 200 && code < 300;
        CHECK(stats.pushSuccess.load() == (success ? 1U : 0U) && stats.pushFailed.load() == (success ? 0U : 1U));
        CHECK(pushHttp.stream.cursor == (code > 0 ? 256U : 0U));
        if (tls) CHECK(pushHttp.tlsTimeout == 10000 && pushHttp.tlsHandshake == 15);
        if (code) {
          CHECK(pushHttp.timeout == 10000 && pushHttp.connectTimeout == 5000);
          CHECK(!pushHttp.reuse);
          CHECK(pushHttp.method == (tls ? "POST" : "GET") && pushHttp.contentType == (tls ? "application/json" : ""));
          if (tls) CHECK(pushHttp.body == "{\"test\":true}");
        }
        CHECK(pushWdtChecks > 0 && pushWdtChecks == pushWdtResets && pushYields >= pushWdtResets);
      });
    }
  }
  for (const std::string mode : {"idle", "null-stream", "read-zero"}) {
    runCase("HTTP-preview-" + mode, [=] {
      pushHttp.stream.idleConnected = true;
      pushHttp.nullStream = mode == "null-stream";
      if (mode == "read-zero") { pushHttp.stream.response = "x"; pushHttp.stream.failRead = true; }
      pushWdtStatus = -1;
      CHECK(sendHttpRequest("https://offline.invalid", "GET", "", "") == 200);
      CHECK(mode == "idle" ? (pushMillis > 0 && pushMillis <= 1000) : pushMillis == 0);
      CHECK(pushHttp.stream.cursor == 0 && pushHttp.lifetimeErrors == 0 && pushWdtResets == 0);
      CHECK(stats.pushSuccess.load() == 1 && stats.pushFailed.load() == 0);
      CHECK(eventIndex("http.destroy") < eventIndex("tls.destroy"));
    });
  }
}

void configureBark() {
  auto& channel = config.pushChannels[0];
  channel.enabled = true; channel.type = PUSH_TYPE_BARK;
  channel.name = "Bark offline"; channel.url = "https://bark.invalid/offline-device-key";
  channel.key2 = u8"家庭短信";
}
void drainHttpWorker() {
  CHECK(httpPushTaskHandles[0] && pushRtos.task.entry == httpPushWorkerTask);
  try { httpPushWorkerTask(nullptr); }
  catch (const PushQueueDrained&) { return; }
  throw std::runtime_error("HTTP worker unexpectedly returned");
}
bool produceHttp(const char* sender = "10690000000", const char* message = "body A") {
  pushJobs.active = true;
  const bool ok = enqueueHttpPush(config.pushChannels[0], sender, message, "2026-11-05 19:40:45");
  pushJobs.active = false;
  return ok;
}
void testBarkDelivery() {
  runCase("Bark-public-producer-queues-and-preserves-configuration-and-text", [] {
    configureBark();
    config.pushChannels[0].url = " \thttps://bark.invalid/offline-device-key\r\n";
    const std::string message = u8"验证码\"6362\"\n第二行\\结束";
    sendSMSToServer("10690000000", message.c_str(), "2026-11-05 19:40:45");
    CHECK(pushHttp.requests == 0 && httpPushQueue && httpPushQueue->items.size() == 1);
    CHECK(config.pushChannels[0].url == " \thttps://bark.invalid/offline-device-key\r\n");
    config.pushChannels[0].url = "https://bark.invalid/different-key";
    config.pushChannels[0].key2 = "different-group"; config.pushChannels[0].enabled = false;
    pushHttp.stream.response = "{\"code\":200,\"message\":\"success\",\"timestamp\":1793900000}";
    pushHttp.contentLength = static_cast<int>(pushHttp.stream.response.size());
    pushHttp.stream.idleConnected = true;
    const auto start = pushMillis;
    drainHttpWorker();
    CHECK(pushHttp.url == "https://bark.invalid/offline-device-key" && pushHttp.method == "POST");
    CHECK(pushHttp.contentType == "application/json");
    CHECK(pushHttp.body == u8"{\"title\":\"10690000000\",\"body\":\"验证码\\\"6362\\\"\\n第二行\\\\结束\",\"group\":\"家庭短信\"}");
    CHECK(pushHttp.requests == 1 && pushMillis - start == 10); // no wait after the complete Content-Length.
    checkSaved(1, 0, 2);
  });
  for (const std::string stage : {"connect", "handshake", "response"}) {
    runCase("Bark-network-slower-than-one-second-" + stage, [=] {
      configureBark(); CHECK(produceHttp());
      if (stage == "connect") pushHttp.connectDelay = 1800;
      if (stage == "handshake") pushHttp.handshakeDelay = 2500;
      if (stage == "response") pushHttp.headerDelay = 2500;
      drainHttpWorker();
      CHECK(pushHttp.requests == 1 && pushMillis >= 1800 && pushMillis < 10000);
      checkSaved(1, 0, 1);
    });
  }
  for (const int code : {400, 401, 404, 418, 429, 500, 503}) {
    runCase("Bark-server-error-diagnostic-" + std::to_string(code), [=] {
      configureBark(); CHECK(produceHttp()); pushHttp.code = code;
      pushHttp.stream.response = "{\"code\":500,\"message\":\"push failed: BadDeviceToken offline-device-key private-message\"}";
      drainHttpWorker();
      CHECK(pushHttp.requests == 1 && pushSerialLog.find("BadDeviceToken") != std::string::npos);
      CHECK(pushSerialLog.find("offline-device-key") == std::string::npos);
      CHECK(pushSerialLog.find("private-message") == std::string::npos);
      checkSaved(0, 1, 1);
    });
  }
  for (const std::string stage : {"connect", "handshake", "response"}) {
    const unsigned long budget = stage == "connect" ? 5000 : (stage == "handshake" ? 15000 : 10000);
    for (const unsigned long offset : {0UL, 1UL}) {
      runCase("HTTP-timeout-boundary-" + stage + "-" + std::to_string(offset), [=] {
        configureBark(); CHECK(produceHttp());
        if (stage == "connect") pushHttp.connectDelay = budget + offset;
        if (stage == "handshake") pushHttp.handshakeDelay = budget + offset;
        if (stage == "response") pushHttp.headerDelay = budget + offset;
        drainHttpWorker();
        CHECK(pushMillis == budget + 10 && pushHttp.requests == 1);
        checkSaved(offset == 0 ? 1 : 0, offset == 0 ? 0 : 1, 1);
      });
    }
  }
  for (const int length : {0, 16, 256, 4096}) {
    runCase("HTTP-error-content-length-" + std::to_string(length), [=] {
      configureBark(); CHECK(produceHttp()); pushHttp.code = 500;
      pushHttp.stream.response.assign(length, 'x'); pushHttp.contentLength = length;
      pushHttp.stream.idleConnected = true;
      drainHttpWorker();
      CHECK(pushHttp.stream.cursor == min(static_cast<size_t>(length), size_t(256)) && pushMillis == 10);
      checkSaved(0, 1, 1);
    });
  }
  runCase("HTTP-unknown-errors-never-log-echoed-secret", [] {
    configureBark(); CHECK(produceHttp()); pushHttp.code = 404;
    pushHttp.stream.response = "/offline-device-key/ private-message";
    drainHttpWorker();
    CHECK(pushSerialLog.find("offline-device-key") == std::string::npos &&
          pushSerialLog.find("private-message") == std::string::npos);
    checkSaved(0, 1, 1);
  });
  for (const std::string stage : {"connect", "handshake", "response"}) {
    runCase("Bark-network-timeout-no-automatic-duplicate-" + stage, [=] {
      configureBark(); CHECK(produceHttp());
      if (stage == "connect") pushHttp.connectDelay = 60000;
      if (stage == "handshake") pushHttp.handshakeDelay = 60000;
      if (stage == "response") pushHttp.headerDelay = 60000;
      drainHttpWorker();
      CHECK(pushHttp.requests == 1 && pushMillis > 1000 && pushMillis <= 20010);
      CHECK(pushHttp.lifetimeErrors == 0); checkSaved(0, 1, 1);
      pushHttp.connectDelay = pushHttp.handshakeDelay = pushHttp.headerDelay = 0;
      CHECK(produceHttp()); drainHttpWorker(); checkSaved(1, 1, 2);
    });
  }
  runCase("Bark-empty-group-is-omitted", [] {
    configureBark(); config.pushChannels[0].key2 = "";
    CHECK(produceHttp()); drainHttpWorker();
    CHECK(pushHttp.body == "{\"title\":\"10690000000\",\"body\":\"body A\"}");
    checkSaved(1, 0, 1);
  });
  for (const std::string mode : {"reserve", "concat", "short-concat"}) {
    for (size_t field = 1; field <= 8; ++field) {
      runCase("HTTP-snapshot-" + mode + "-" + std::to_string(field), [=] {
        configureBark(); config.pushChannels[0].key1 = "unused-key"; config.pushChannels[0].customBody = "unused-body";
        CHECK(ensureHttpPushWorker()); PushStringHeap::resetProbe();
        if (mode == "reserve") PushStringHeap::failAt = field;
        if (mode == "concat") PushStringHeap::failConcat = field;
        if (mode == "short-concat") PushStringHeap::shortConcat = field;
        const bool ok = produceHttp();
        CHECK(!ok && pushJobs.live == 0 && httpPushQueue->items.empty());
        CHECK(stats.pushFailed.load() == 1 && pushHttp.requests == 0);
        PushStringHeap::resetProbe();
        if (ok) drainHttpWorker(); // permit cleanup when run against a regressed implementation.
        CHECK(produceHttp()); drainHttpWorker();
        CHECK(pushHttp.requests == 1 && stats.pushSuccess.load() == 1 && stats.pushFailed.load() == 1);
      });
    }
  }
  runCase("HTTP-full-queue-rejects-only-new-job", [] {
    configureBark();
    for (size_t i = 0; i < HTTP_PUSH_QUEUE_LENGTH; ++i) CHECK(produceHttp());
    CHECK(!produceHttp() && pushJobs.live == HTTP_PUSH_QUEUE_LENGTH);
    drainHttpWorker(); CHECK(pushHttp.requests == HTTP_PUSH_QUEUE_LENGTH);
    checkSaved(HTTP_PUSH_QUEUE_LENGTH, 1, HTTP_PUSH_QUEUE_LENGTH);
  });
  runCase("HTTP-job-allocation-failure-then-recovers", [] {
    configureBark(); CHECK(ensureHttpPushWorker()); pushJobs.failAt = 1;
    CHECK(!produceHttp() && pushJobs.faultHits == 1 && pushJobs.live == 0);
    pushJobs.failAt = 0; CHECK(produceHttp()); drainHttpWorker(); checkSaved(1, 1, 1);
  });
  for (const bool queueFailure : {true, false}) {
    runCase(queueFailure ? "HTTP-queue-create-recovery" : "HTTP-worker-create-recovery", [=] {
      configureBark(); pushRtos.failQueue = queueFailure; pushRtos.failTask = !queueFailure;
      CHECK(!produceHttp() && pushJobs.calls == 0 && pushHttp.requests == 0);
      pushRtos.failQueue = pushRtos.failTask = false;
      CHECK(produceHttp()); drainHttpWorker(); checkSaved(1, 1, 1);
    });
  }
  for (const bool nullSender : {false, true}) {
    runCase(nullSender ? "HTTP-null-sender" : "HTTP-null-body", [=] {
      configureBark();
      CHECK(!produceHttp(nullSender ? nullptr : "10086", nullSender ? "text" : nullptr));
      CHECK(pushJobs.live == 0 && httpPushQueue->items.empty() && pushHttp.requests == 0);
      checkSaved(0, 1, 0);
    });
  }
  runCase("HTTP-empty-url-after-trim-is-rejected", [] {
    configureBark(); config.pushChannels[0].url = " \t\r\n";
    CHECK(!produceHttp() && pushJobs.live == 0 && httpPushQueue->items.empty());
    CHECK(pushHttp.requests == 0); checkSaved(0, 1, 0);
  });
  for (const PushType type : {PUSH_TYPE_POST_JSON, PUSH_TYPE_GET, PUSH_TYPE_CUSTOM,
                              PUSH_TYPE_TELEGRAM, PUSH_TYPE_WECOM, PUSH_TYPE_DINGTALK}) {
    runCase("HTTP-existing-channel-compatible-" + std::to_string(type), [=] {
      configureBark(); auto& channel = config.pushChannels[0];
      channel.type = type; channel.key1 = "12345"; channel.customBody = "{\"text\":\"{message}\"}";
      CHECK(produceHttp("10086", "text")); drainHttpWorker();
      CHECK(pushHttp.requests == 1 && pushHttp.method == (type == PUSH_TYPE_GET ? "GET" : "POST"));
      if (type == PUSH_TYPE_POST_JSON) CHECK(pushHttp.body == "{\"sender\":\"10086\",\"message\":\"text\",\"timestamp\":\"2026-11-05 19:40:45\"}");
      if (type == PUSH_TYPE_GET) CHECK(pushHttp.url.find("?sender=10086&message=text&timestamp=") != std::string::npos);
      if (type == PUSH_TYPE_CUSTOM) CHECK(pushHttp.body == "{\"text\":\"text\"}");
      if (type == PUSH_TYPE_TELEGRAM) CHECK(pushHttp.body.find("\"chat_id\":\"12345\"") != std::string::npos);
      if (type == PUSH_TYPE_WECOM || type == PUSH_TYPE_DINGTALK) CHECK(pushHttp.body.find("\"msgtype\":\"text\"") != std::string::npos);
      if (type == PUSH_TYPE_DINGTALK) CHECK(pushHttp.url.find("&sign=offline-signature") != std::string::npos);
      checkSaved(1, 0, 1);
    });
  }
}
} // namespace

int main() {
  emptyStringInstances = PushStringHeap::instances;
  testSnapshotsAndQueue();
  testProducerFailures();
  testSenderExits();
  testWorkerStringFailures();
  testHttpLifetime();
  testBarkDelivery();
  std::cout << "Push host: " << cases << " cases, " << assertions << " assertions, " << failures << " failures\n";
  return failures ? 1 : 0;
}
