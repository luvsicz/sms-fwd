# AGENTS.md

## Project overview
- This is an ESP32-C3 + ML307R-DC Arduino firmware for SMS/call forwarding, remote SMS sending, Web configuration, MQTT, and Home Assistant integration.
- The Arduino sketch lives in `code/`; `code/code.ino` is the entry point and owns globals, `setup()`, and `loop()`.
- Arduino concatenates `.ino` files in the sketch folder, but this project still keeps explicit declarations in headers such as `config.h`, `web_handlers.h`, `sms_handler.h`, `call_handler.h`, `push_service.h`, and `mqtt_handler.h`. Add new cross-file APIs to the matching header. Note that `code/web_query.ino` implements query and ping endpoints whose declarations reside in `web_handlers.h`.
- `homeassistant/` contains optional HA YAML/blueprints; keep MQTT topic/entity changes synchronized with firmware behavior and docs.

## Firmware architecture and data flow
- Startup in `code/code.ino`: load NVS config via `loadConfig()`, connect up to `MAX_WIFI_NETWORKS` with `WiFiMulti`, fall back to AP `SMS-Forwarder-AP`, start mDNS `sms.local`, register Web routes, initialize ML307R AT settings, NTP, MQTT, and watchdog.
- Main loop order matters: `server.handleClient()`, timer tasks, MQTT reconnect/loop/status, long-SMS timeout handling, serial passthrough, URC parsing, watchdog reset.
- SMS receive flow: ML307R URC/PDU parsing in `sms_handler.ino` -> long SMS assembly -> `processSmsContent()` -> append SPIFFS history before filters -> number/content filters -> HTTP push, MQTT publish, email.
- Call flow: `call_handler.ino` parses `+CLIP` / `+CLCC`, deduplicates with `CALL_NOTIFY_DEDUP_MS`, applies number filtering, stores call history, then pushes via HTTP/MQTT/email.
- Push channels are capped by `MAX_PUSH_CHANNELS` in `config.h`; channel behavior is centralized in `push_service.ino` (`POST_JSON`, `BARK`, `GET`, `CUSTOM`, `TELEGRAM`, `WECOM`, `DINGTALK`).
- HTTP push and email delivery are no longer executed inline from the receive path: `push_service.ino` uses FreeRTOS queues/tasks (`enqueueHttpPush()`, `httpPushWorkerTask()`, email queue worker) with short HTTP timeouts to keep the main loop responsive and avoid watchdog resets.
- Concurrency and TLS protection for SMTP: `push_service.ino` uses a global mutex `smtpMutex` with RAII guard `SmtpLockGuard` (initialized by `initSmtpLock()`) to serialize email worker jobs and external email calls, preventing concurrent TLS/heap corruption.
- SMTP multi-port and ReadyMail rules: `push_service.ino` supports SSL port 465, STARTTLS port 587 (`ssl_client.setPlainStart()` + `smtp.setStartTLS`), and plain port 25. ReadyMail async mode (`await=false`) strictly requires using `smtp.getMessage()` internal reference and clearing it with `msg.clear()`; local `SMTPMessage` instances cause error -109. NTP sync is checked before establishing SMTP connections with fallback to default timestamp (0) to prevent socket timeouts.
- Interrupted operation diagnostics: `recordOperationDiagnostic()` in `config.ino` logs high-risk operations (e.g. SMTP connect/auth/send stages) and heap snapshots into NVS namespace `sms_diag` before execution, cleared via `clearOperationDiagnostic()`. If a crash/watchdog/brownout reset occurs mid-flight, the snapshot is recovered in `loadStats()` and exposed to Web/MQTT diagnostics.
- Serial line reading isolation: `readSerialLine(port, lineBuf, bufSize, linePos)` in `sms_handler.ino` allows callers (e.g., `checkSerial1URC()`) to supply dedicated buffers to eliminate concurrency/reentrancy races. The single-argument overload protects its internal buffer with a FreeRTOS mutex and constructs the `String` outside the critical section.
- Long SMS assembly safety: `assembleConcatSms()` pre-allocates String memory with `reserve()`, and `clearConcatSlot()` validates slot bounds (`0 <= slot < MAX_CONCAT_MESSAGES`) to prevent heap corruption.
- MQTT topics are initialized in `initMqttTopics()`: user topics use `<mqttPrefix>/<mac_suffix>/...`; HA discovery uses `mqttHaPrefix` (default `homeassistant`) and publishes retained discovery configs.
- MQTT now also has a lightweight log/metric pipeline in `mqtt_handler.ino`: device logs go to `.../log`, slow operations to `.../log/slow`, errors to `.../log/error`, and timing metrics to `.../metric/timing`; log publishing is buffered through `processMqttLogQueue()` rather than sent synchronously from every call site.

## Configuration and persistence conventions
- Runtime config is the global `Config config` from `config.h`; persist every new field in both `saveConfig()` and `loadConfig()` in `config.ino`, with a sensible default.
- First WiFi defaults come from `code/wifi_config.h`; later Web changes are stored in ESP32 `Preferences` namespace `sms_config`.
- Thread-safe statistics: all `Statistics` counters in `config.h` use `std::atomic<uint32_t>` (accessed via `.load()`) with atomic increment helpers like `statsIncrementSmsReceived()`. NVS operations in `saveStats()`, `loadStats()`, and `resetStats()` are protected by `statsMutex` (initialized via `initStatsLock()`).
- Reset classification: firmware distinguishes `esp_reset_reason()` into boot counts, brownout, power-on, watchdog, software reboot, and panic crash counts (`lastResetPowerSuspected` flags potential power supply drops).
- NVS 15-character key limit: ESP32 `Preferences` enforces a strict 15-character key length limit. WiFi keys use compact prefixes generated by `makeNvsWifiKey()` (`w_s_`, `w_p_`, `w_e_`) with backwards compatibility fallback for legacy `wifi<i>...` keys. New persistent keys must never exceed 15 characters.
- Histories use SPIFFS JSON-lines files (`/sms.txt`, `/calls.txt`), not the legacy in-memory `SmsRecord` array; use `jsonEscape()` and existing cleanup helpers when touching history.
- Streaming history trimming & self-healing: `safeTrimHistoryFile()` uses a fixed 256-byte stack chunk buffer to stream-copy to `.trim.tmp` and checks `ESP.getMaxAllocHeap() < 4096` to abort safely without wiping history under low memory. `initSmsStorage()` performs self-healing on boot if `.trim.tmp` exists.
- Memory-safe history reading: `getSmsHistory()` and `getCallHistory()` allocate ring buffers on the heap (`std::unique_ptr<String[]>`) rather than the task stack (avoiding 1.6KB+ stack overflows) and pre-calculate length with `result.reserve()` to minimize heap fragmentation.
- Stats are updated close to the side effect (`addSmsToHistory()`, `addCallToHistory()`, send success/failure paths) and saved with `saveStats()`.
- Long operations in Web/config paths commonly call `esp_task_wdt_reset()` and `yield()` to avoid the 30s watchdog.
- Timer overflow safety: all timestamp difference comparisons with `millis()` must use unsigned subtraction `(millis() - startTime >= interval)` to remain safe against 32-bit overflow (49.7-day wraparound).

## Web UI/API patterns
- All admin routes in `web_handlers.ino` should call `checkAuth()` first; Basic Auth defaults are `admin` / `admin123`.
- Web pages are generated by placeholder replacement from `web_pages.h` (`html.replace("%TOKEN%", value)`). When adding config UI, update placeholders, `handleRoot()`, and `handleSave()` together.
- Heap safety for HTML rendering: `handleRoot()` verifies `ESP.getFreeHeap()` before reserving string capacity (`html.reserve(htmlReserveLength)`); returns HTTP 503 if memory is insufficient to avoid OOM crashes during Web access.
- Sensitive password updates: in `handleSave()`, password fields (`webPass`, `smtpPass`, `wifi<i>pass`) only overwrite existing credentials when a non-empty string is provided, preserving stored credentials if left blank.
- Existing APIs include `/save`, `/sendsms`, `/ping`, `/timer`, `/query`, `/restart`, `/history`, `/stats`, `/resetstats`, `/clearsmshistory`, `/clearcallhistory`, `/cleanhistory`, `/filter`, and `/contentfilter`.
- History cleanup endpoint: `/cleanhistory` accepts POST parameter `type=sms` or `type=call`, triggering `cleanupSmsHistory()` or `cleanupCallHistory()` to strip corrupted JSON lines while returning JSON with `kept` and `removed` line counts.
- Extended status telemetry: `/stats` and `/query` output structured reset reasons, power suspicion flags, reset counters, and interrupted operation diagnostic snapshots.
- `/tools` currently just redirects to `/`; keep that behavior in mind before adding separate toolbox UI logic.
- JSON parsing is intentionally lightweight string parsing to minimize dependencies; preserve that style unless introducing a dependency across the whole sketch is justified.

## Build, flash, and debugging
- Primary workflow is Arduino IDE, not PlatformIO: install ESP32 board support from `https://espressif.github.io/arduino-esp32/package_esp32_index.json`.
- Required libraries from README: `ReadyMail`, `pdulib`, and `PubSubClient`; ESP32 core supplies `WiFi`, `WebServer`, `ESPmDNS`, `Preferences`, `SPIFFS`, etc.
- Recommended board/flash settings from README: MakerGO ESP32 C3 SuperMini or compatible ESP32-C3 board, Partition Scheme `Huge App`, serial monitor `115200` baud.
- Hardware serial mapping is in `config.h`: ESP32 GPIO3 (`TXD`) -> ML307 RX, GPIO4 (`RXD`) -> ML307 TX; `Serial` is USB debug, `Serial1` talks to the modem and supports local AT passthrough.
- Watchdog configuration: uses ESP32 Arduino Core v3.0+ API (`esp_task_wdt_config_t`), checking `esp_task_wdt_status()` before calling `esp_task_wdt_init()` and `esp_task_wdt_add(NULL)` to avoid double-initialization panics.
- Local debug simulation commands: `handleDebugSerialCommand()` intercepts `TESTPUSH` on `Serial` (USB) to simulate receiving a verification code SMS and triggers Bark, MQTT, and SMTP notifications for end-to-end push verification. All other serial input is passed directly to `Serial1` for AT communication.
- There are no checked-in automated tests or build scripts; verify firmware changes by compiling the `code/` sketch in Arduino IDE and watching serial logs for WiFi, AT, MQTT, and watchdog messages.

## Integration details to preserve
- MQTT command payloads are simple JSON strings: `.../sms/send` expects `{"phone":"...","message":"..."}`, `.../ping` may include `{"host":"..."}`, and `.../cmd` handles actions such as restart.
- Home Assistant docs and blueprints assume topics like `sms/<device_id>/sms/received`; update `README.md`, `homeassistant/README.md`, and `homeassistant/sensors.yaml` if topic JSON fields or entity names change.
- HA entity & blueprint synchronization: `homeassistant/` maintains 5 automation blueprints (`sms_notification.yaml`, `sms_event_notification.yaml`, `sms_code_extract.yaml`, `call_notification.yaml`, `offline_alert.yaml`) and `sensors.yaml`. Whenever MQTT status payload fields (such as `last_reset_reason`, `brownout_resets`, `poweron_resets`, `interrupted_operation`) or event payloads change, keep discovery configs and YAML sensors aligned.
- HA auto-discovery now also creates log-related entities/events from `mqtt_handler.ino` (for example “最新日志”, “最近慢操作”, “最近错误日志”, “最新耗时”, and `event` entity `设备日志事件`); keep `homeassistant/README.md` aligned if discovery payloads or topic names change.
- Push channel key mappings: `PushChannel` fields `key1` and `key2` have channel-specific semantics in `push_service.ino`: Telegram uses `key1` for Chat ID; DingTalk uses `key1` for the HMAC-SHA256 signing secret; Bark uses `key2` for optional group categorization (`group`). Custom Webhook bodies send `application/json` with `{sender}`, `{message}`, and `{timestamp}` replacements.
- Multiple email recipients: `config.smtpSendTo` supports both comma `,` and semicolon `;` separators, parsed iteratively in `sendEmailNotificationNow()`.
- HTTPS push and SMTP intentionally use `ssl_client.setInsecure()` / insecure TLS behavior for embedded simplicity; document rather than silently changing this behavior.

