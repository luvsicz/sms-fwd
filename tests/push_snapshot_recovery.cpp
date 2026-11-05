#include "push_host_stubs.h"
#include "push_service.h"
#include <iostream>
#include <limits>
#include <type_traits>

Config config{};
Statistics stats;
WiFiClientSecure ssl_client;
PushSmtp smtp;

struct SavedStats { uint32_t success, failed; size_t liveJobs; };
std::vector<SavedStats> recoverySaves;
bool recoveryDiagnostic = false;
void saveStats() {
  recoverySaves.push_back({stats.pushSuccess.load(), stats.pushFailed.load(), pushJobs.live});
  pushEvents.emplace_back("stats.save");
}
void recordOperationDiagnostic(const char*, const char*) { recoveryDiagnostic = true; }
void clearOperationDiagnostic() { recoveryDiagnostic = false; }

// No production branches are replaced. Only external APIs and the clock are host substitutes.
#define time pushHostTime
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "push_snapshot_actual.inc"
#pragma GCC diagnostic pop
#undef time

static_assert(std::is_default_constructible<EmailJob>::value, "producer must construct an empty job");
static_assert(!std::is_copy_constructible<EmailJob>::value, "EmailJob must not copy Strings implicitly");
static_assert(!std::is_copy_assignable<EmailJob>::value, "EmailJob must not assign Strings implicitly");

// Observe real production new/delete. Fixed storage keeps delete instrumentation allocation-free.
struct ReleasedJob { size_t eventCount; bool unlocked, messageEmpty, diagnosticClear; };
ReleasedJob recoveryReleases[64]{};
size_t recoveryReleaseCount = 0;
void* operator new(std::size_t size) {
  const bool job = pushJobs.active && size == sizeof(EmailJob);
  if (job && ++pushJobs.calls == pushJobs.failAt) { ++pushJobs.faultHits; throw std::bad_alloc(); }
  void* p = std::malloc(size ? size : 1);
  if (!p) throw std::bad_alloc();
  if (job) pushJobs.remember(p);
  return p;
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try { return ::operator new(size); } catch (...) { return nullptr; }
}
void operator delete(void* p) noexcept {
  for (const void* tracked : pushJobs.pointers) {
    if (p && tracked == p) {
      if (recoveryReleaseCount == 64) std::abort();
      recoveryReleases[recoveryReleaseCount++] = {pushEvents.size(), !pushRtos.mutex.held,
        smtp.message.headers.values.empty() && smtp.message.text.value.empty(), !recoveryDiagnostic};
      break;
    }
  }
  pushJobs.release(p);
  std::free(p);
}
void operator delete(void* p, std::size_t) noexcept { ::operator delete(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { ::operator delete(p); }

namespace {
size_t cases = 0, assertions = 0, failures = 0, initialStringInstances = 0;
std::string currentCase;
String Config::*smtpFields[] = {&Config::smtpServer, &Config::smtpUser, &Config::smtpPass, &Config::smtpSendTo};
const char* fieldNames[] = {"subject", "body", "smtpServer", "smtpUser", "smtpPass", "smtpSendTo"};
const char* subjectA = u8"  subject A 短信快照  ";
const char* bodyA = u8"  第一行\r\nsecond line 😀 must keep whitespace \n";

void check(bool ok, const char* expression, int line) {
  ++assertions;
  if (!ok) { ++failures; std::cerr << "FAIL [" << currentCase << ':' << line << "] " << expression << '\n'; }
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

void configure(char id = 'A', int port = 587) {
  const std::string suffix(1, id);
  config.emailEnabled = true;
  config.smtpServer = (" \tsmtp-" + suffix + ".mail.invalid\r\n").c_str();
  config.smtpUser = (" sender-" + suffix + "@mail.invalid ").c_str();
  config.smtpPass = (" offline-password-" + suffix + "\t").c_str();
  config.smtpSendTo = (" first-" + suffix + "@mail.invalid ; ;\tsecond-long-" + suffix + "@mail.invalid,, ").c_str();
  config.smtpPort = port;
}
void cleanup() {
  pushJobs.active = false;
  PushStringHeap::resetProbe();
  if (emailQueue) {
    for (void* p : emailQueue->items) delete static_cast<EmailJob*>(p);
    delete emailQueue;
    emailQueue = nullptr;
  }
  for (void* p : pushJobs.pointers) if (p) delete static_cast<EmailJob*>(p);
}
void reset() {
  cleanup();
  emailTaskHandle = nullptr; smtpMutex = nullptr;
  pushJobs = {}; pushRtos = {}; pushHttp = {}; smtp = PushSmtp{};
  config = Config{}; stats = Statistics{};
  ssl_client.plain = false; ssl_client.stops = ssl_client.upgrades = 0;
  WiFi.state = WL_CONNECTED; pushEpoch = 1700000000; pushMillis = 0;
  recoverySaves.clear(); recoveryDiagnostic = false; recoveryReleaseCount = 0;
  configure();
  PushStringHeap::resetProbe(); pushEvents.clear();
}
template<class F> void run(const std::string& name, F test) {
  ++cases; currentCase = name;
  const size_t before = failures;
  try { reset(); test(); }
  catch (const std::exception& error) { check(false, error.what(), __LINE__); }
  catch (...) { check(false, "unexpected exception", __LINE__); }
  // Assert leaks BEFORE recovery cleanup; otherwise delete in the fixture could hide a regression.
  CHECK(pushJobs.live == 0 && (emailQueue == nullptr || emailQueue->items.empty()));
  CHECK(!pushRtos.mutex.held && !recoveryDiagnostic);
  CHECK(smtp.message.headers.values.empty() && smtp.message.text.value.empty());
  CHECK(pushTlsAlive.size() == 1 && pushTlsAlive.count(&ssl_client) == 1);
  for (size_t i = 0; i < recoveryReleaseCount; ++i) {
    CHECK(recoveryReleases[i].unlocked && recoveryReleases[i].messageEmpty && recoveryReleases[i].diagnosticClear);
  }
  CHECK(PushStringHeap::guardsOk());
  PushStringHeap::resetProbe(); config = Config{};
  CHECK(PushStringHeap::blocks.empty() && PushStringHeap::instances == initialStringInstances);
  std::cout << (before == failures ? "PASS " : "FAIL ") << name << '\n';
  cleanup();
}
void produce(const char* subject = subjectA, const char* body = bodyA) {
  struct AllocationScope {
    AllocationScope() { pushJobs.active = true; }
    ~AllocationScope() { pushJobs.active = false; }
  } scope;
  sendEmailNotification(subject, body);
}
EmailJob& queued(size_t index = 0) {
  if (!emailQueue || emailQueue->items.size() <= index || !emailQueue->items[index]) {
    throw std::runtime_error("expected a queued production EmailJob");
  }
  return *static_cast<EmailJob*>(emailQueue->items[index]);
}
void drain() {
  if (!emailTaskHandle || pushRtos.task.entry != emailWorkerTask) throw std::runtime_error("missing production worker");
  try { pushRtos.task.entry(nullptr); }
  catch (const PushQueueDrained&) { return; } // next blocking receive, after send/save/delete
  throw std::runtime_error("worker unexpectedly returned");
}
void saved(uint32_t success, uint32_t failed, size_t count) {
  CHECK(stats.pushSuccess.load() == success && stats.pushFailed.load() == failed);
  CHECK(recoverySaves.size() == count);
  if (count && !recoverySaves.empty()) {
    CHECK(recoverySaves.back().success == success && recoverySaves.back().failed == failed);
  }
}
size_t event(const char* name) {
  const auto found = std::find(pushEvents.begin(), pushEvents.end(), name);
  CHECK(found != pushEvents.end());
  return static_cast<size_t>(found - pushEvents.begin());
}
void checkAB() {
  CHECK(smtp.attempts.size() == 2);
  CHECK(smtp.attempts.at(0).server == "smtp-A.mail.invalid");
  CHECK(smtp.attempts.at(1).server == "smtp-B.mail.invalid");
}
void retryB() {
  PushStringHeap::resetProbe(); pushJobs.failAt = 0;
  configure('B'); produce("subject B", "body B"); drain();
}

void snapshotsAndQueues() {
  run("A/B-owned-buffers-survive-reconfigure-and-disable", [] {
    std::string subject = subjectA, body = bodyA;
    body.append(4096, 'a');
    const auto expectedBody = body;
    const std::string originalServer = config.smtpServer.c_str();
    produce(subject.c_str(), body.c_str());
    const auto& a = queued();
    CHECK(originalServer == config.smtpServer.c_str() && a.smtpServer == "smtp-A.mail.invalid");
    CHECK(a.smtpServer.c_str() != config.smtpServer.c_str());
    const uintptr_t server = reinterpret_cast<uintptr_t>(a.smtpServer.c_str());
    const uintptr_t user = reinterpret_cast<uintptr_t>(a.smtpUser.c_str());
    const uintptr_t pass = reinterpret_cast<uintptr_t>(a.smtpPass.c_str());
    const uintptr_t subjectPtr = reinterpret_cast<uintptr_t>(a.subject.c_str());
    const uintptr_t bodyPtr = reinterpret_cast<uintptr_t>(a.body.c_str());
    subject.assign(8192, 'x'); body.assign(8192, 'x');
    configure('B', 25); produce("subject B", "body B");
    config = Config{}; produce("disabled", "must not allocate");
    CHECK(pushJobs.calls == 2 && smtp.connects == 0 && recoverySaves.empty());
    PushStringHeap::resetProbe(); drain();
    CHECK(PushStringHeap::copies == 0);
    checkAB();
    const auto& sent = smtp.attempts.at(0);
    CHECK(sent.user == "sender-A@mail.invalid" && sent.pass == "offline-password-A");
    CHECK(sent.serverSource == server && sent.userSource == user && sent.passSource == pass);
    CHECK(sent.subjectSource == subjectPtr && sent.bodySource == bodyPtr);
    CHECK(sent.subject == subjectA && sent.body == expectedBody);
    CHECK((sent.to == std::vector<std::string>{"your_email <first-A@mail.invalid>", "your_email <second-long-A@mail.invalid>"}));
    CHECK(sent.port == 587 && sent.ssl && sent.startTls);
    CHECK(smtp.attempts.at(1).port == 25 && !smtp.attempts.at(1).ssl && !smtp.attempts.at(1).startTls);
    CHECK(recoverySaves.at(0).liveJobs == 2 && recoverySaves.at(1).liveJobs == 1);
    CHECK(event("smtp.stop") < event("lock.give") && event("lock.give") < event("stats.save"));
    CHECK(event("stats.save") < recoveryReleases[0].eventCount);
    CHECK(pushJobs.frees == 2 && smtp.message.clears == 4);
    saved(2, 0, 2);
  });
  run("full-queue-preserves-FIFO-and-releases-only-rejection", [] {
    for (size_t i = 0; i < EMAIL_QUEUE_LENGTH; ++i) {
      configure(static_cast<char>('A' + i)); produce(std::to_string(i).c_str());
    }
    const auto* first = &queued();
    const size_t blocks = PushStringHeap::blocks.size();
    produce("rejected");
    CHECK(&queued() == first && emailQueue->items.size() == EMAIL_QUEUE_LENGTH);
    CHECK(pushJobs.frees == 1 && pushJobs.live == EMAIL_QUEUE_LENGTH && PushStringHeap::blocks.size() == blocks);
    saved(0, 1, 1);
    drain();
    for (size_t i = 0; i < EMAIL_QUEUE_LENGTH; ++i) CHECK(smtp.attempts.at(i).subject == std::to_string(i));
    saved(EMAIL_QUEUE_LENGTH, 1, EMAIL_QUEUE_LENGTH + 1);
  });
  run("disabled-producer-does-not-touch-allocators", [] {
    config.emailEnabled = false; PushStringHeap::failAt = 1; pushJobs.failAt = 1;
    produce();
    CHECK(pushRtos.queueCreates == 0 && pushRtos.taskCreates == 0 && pushRtos.sends == 0);
    CHECK(pushJobs.calls == 0 && PushStringHeap::allocations == 0);
    saved(0, 0, 0);
  });
  for (bool queueFailure : {true, false}) {
    run(queueFailure ? "queue-allocation-recovery" : "worker-allocation-recovery", [=] {
      pushRtos.failQueue = queueFailure; pushRtos.failTask = !queueFailure;
      produce(); saved(0, 1, 1);
      CHECK(pushJobs.calls == 0 && pushRtos.sends == 0);
      pushRtos.failQueue = pushRtos.failTask = false;
      retryB(); saved(1, 1, 2);
    });
  }
  run("job-OOM-preserves-previous-snapshot", [] {
    produce(); const auto* a = &queued(); configure('B');
    const size_t blocks = PushStringHeap::blocks.size();
    pushJobs.failAt = pushJobs.calls + 1; produce();
    CHECK(pushJobs.faultHits == 1 && pushJobs.live == 1 && &queued() == a);
    CHECK(PushStringHeap::blocks.size() == blocks); saved(0, 1, 1);
    retryB(); checkAB(); saved(2, 1, 3);
  });
}

void snapshotFailures() {
  for (const std::string mode : {"reserve", "concat", "short-concat"}) {
    for (size_t field = 0; field < 6; ++field) {
      run("pending-A/failed-B-" + mode + '-' + fieldNames[field], [=] {
        produce(); const auto* a = &queued(); configure('B');
        const size_t blocks = PushStringHeap::blocks.size(), strings = PushStringHeap::instances;
        PushStringHeap::resetProbe();
        if (mode == "reserve") PushStringHeap::failAt = field + 1;
        if (mode == "concat") PushStringHeap::failConcat = field + 1;
        if (mode == "short-concat") PushStringHeap::shortConcat = field + 1;
        produce("subject B", "body B");
        CHECK(PushStringHeap::faultHits == 1 && PushStringHeap::copies == 0);
        CHECK(pushJobs.frees == 1 && pushJobs.live == 1 && pushRtos.sends == 1);
        CHECK(&queued() == a && a->smtpServer == "smtp-A.mail.invalid");
        CHECK(PushStringHeap::blocks.size() == blocks && PushStringHeap::instances == strings);
        saved(0, 1, 1); retryB(); checkAB(); saved(2, 1, 3);
      });
    }
  }
  for (size_t field = 0; field < 4; ++field) {
    run(std::string("invalid-source-String-") + fieldNames[field + 2], [=] {
      produce(); configure('B');
      PushStringHeap::resetProbe(); PushStringHeap::failAt = 1;
      config.*smtpFields[field] = "a replacement long enough to require allocation after a prior OOM";
      CHECK((config.*smtpFields[field]).c_str() == nullptr && (config.*smtpFields[field]).length() == 0);
      PushStringHeap::resetProbe(); produce();
      CHECK(pushJobs.frees == 1 && pushJobs.live == 1 && pushRtos.sends == 1);
      saved(0, 1, 1); retryB(); checkAB(); saved(2, 1, 3);
    });
  }
}

void senderRecovery() {
  for (int reason = 0; reason < 7; ++reason) {
    run("send-early-return-and-recovery-" + std::to_string(reason), [=] {
      if (reason >= 3) config.*smtpFields[reason - 3] = " \t\r\n";
      produce();
      if (reason == 0) WiFi.state = 0;
      if (reason == 1) pushRtos.failMutex = true;
      if (reason == 2) pushRtos.failTake = true;
      drain();
      CHECK(pushJobs.frees == 1 && smtp.connects == 0 && pushRtos.gives == 0);
      saved(0, 1, 1);
      WiFi.state = WL_CONNECTED; pushRtos.failMutex = pushRtos.failTake = false;
      retryB(); saved(1, 1, 2);
    });
  }
  const PushSmtpFault faults[] = {PushSmtpFault::ConnectStart, PushSmtpFault::ConnectStatus,
    PushSmtpFault::ConnectDisconnected, PushSmtpFault::ConnectTimeout, PushSmtpFault::AuthStart,
    PushSmtpFault::AuthStatus, PushSmtpFault::AuthUnconfirmed, PushSmtpFault::AuthTimeout,
    PushSmtpFault::SendStart, PushSmtpFault::SendStatus, PushSmtpFault::SendIncomplete, PushSmtpFault::SendTimeout};
  for (auto fault : faults) {
    run("SMTP-failure-then-success-" + std::to_string(static_cast<int>(fault)), [=] {
      produce(); smtp.fault = fault; drain();
      CHECK(pushJobs.frees == 1 && smtp.stops >= 1 && ssl_client.stops >= 1);
      CHECK(pushRtos.gives == 1 && !recoveryDiagnostic);
      CHECK(event("smtp.stop") < event("lock.give") && event("lock.give") < event("stats.save"));
      CHECK(event("stats.save") < recoveryReleases[0].eventCount);
      saved(0, 1, 1);
      smtp.fault = PushSmtpFault::None; retryB();
      CHECK(smtp.attempts.back().server == "smtp-B.mail.invalid" && pushRtos.gives == 2);
      saved(1, 1, 2);
    });
  }
  for (const std::string mode : {"reserve", "concat", "short-concat"}) {
    for (size_t at = 1; at <= (mode == "reserve" ? 3U : 9U); ++at) {
      run("header-OOM-does-not-poison-next-job-" + mode + '-' + std::to_string(at), [=] {
        config.smtpUser = "u@mail.invalid";
        config.smtpSendTo = "first-long-recipient@mail.invalid;second-much-longer-recipient-address@mail.invalid";
        produce(); configure('B'); produce("subject B", "body B");
        PushStringHeap::resetProbe();
        if (mode == "reserve") PushStringHeap::failAt = at;
        if (mode == "concat") PushStringHeap::failConcat = at;
        if (mode == "short-concat") PushStringHeap::shortConcat = at;
        drain();
        CHECK(PushStringHeap::faultHits == 1 && PushStringHeap::copies == 0);
        CHECK(smtp.attempts.size() == 1 && smtp.attempts.at(0).subject == "subject B");
        CHECK(smtp.message.clears == 4 && smtp.stops == 2 && ssl_client.stops == 2 && pushRtos.gives == 2);
        CHECK(pushJobs.frees == 2 && recoverySaves.at(0).failed == 1 && recoverySaves.at(0).liveJobs == 2);
        saved(1, 1, 2);
      });
    }
  }
  run("SMTP-timeout-across-millis-wrap-then-recovery", [] {
    produce(); pushMillis = std::numeric_limits<unsigned long>::max() - 100UL;
    const unsigned long start = pushMillis;
    smtp.fault = PushSmtpFault::ConnectTimeout; drain();
    CHECK(pushMillis - start >= SMTP_STAGE_TIMEOUT_MS && pushMillis - start < SMTP_STAGE_TIMEOUT_MS + 100);
    saved(0, 1, 1); smtp.fault = PushSmtpFault::None; retryB(); saved(1, 1, 2);
  });
}

void httpLifetimes() {
  for (bool tls : {false, true}) {
    for (int code : {0, -7, 503, 204}) {
      run(std::string(tls ? "HTTPS" : "HTTP") + "-destruction-" + std::to_string(code), [=] {
        pushHttp.beginOk = code != 0; pushHttp.code = code; pushHttp.stream.response.assign(2048, 'r');
        CHECK(sendHttpRequest(tls ? "https://offline.invalid" : "http://offline.invalid", "POST", "application/json", "{}") == (code ? code : -1));
        CHECK(pushHttp.destructors == 1 && pushHttp.lifetimeErrors == 0);
        CHECK(pushHttp.ends == (code ? 1U : 0U) && pushHttp.requests == (code ? 1U : 0U));
        CHECK(event("http.destroy") < event("tls.destroy"));
        if (code) CHECK(event("http.end") < event("http.destroy"));
        CHECK(pushHttp.stream.cursor == (code > 0 ? 256U : 0U));
        saved(code == 204 ? 1 : 0, code == 204 ? 0 : 1, 0);
        // A fresh request after any return path must still have a live TLS client at HTTP destruction.
        pushHttp = {}; pushEvents.clear();
        CHECK(sendHttpRequest("https://offline.invalid/retry", "GET", "", "") == 200);
        CHECK(pushHttp.lifetimeErrors == 0 && event("http.destroy") < event("tls.destroy"));
        saved(code == 204 ? 2 : 1, code == 204 ? 0 : 1, 0);
      });
    }
  }
}
} // namespace

int main() {
  initialStringInstances = PushStringHeap::instances;
  snapshotsAndQueues();
  snapshotFailures();
  senderRecovery();
  httpLifetimes();
  std::cout << "Push recovery: " << cases << " cases, " << assertions << " assertions, " << failures << " failures\n";
  return failures ? 1 : 0;
}
