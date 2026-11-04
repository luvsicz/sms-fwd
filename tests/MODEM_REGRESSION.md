# SMS / modem offline regression

## Run

```powershell
pwsh -NoProfile -File tests/test_sms_regression.ps1
pwsh -NoProfile -File tests/test_modem_regression.ps1
```

Both scripts use `sms_host_runner.ps1`. Optional arguments: `-CompilerPath`, `-PduLibPath`,
`-SourceDirectory`, `-Sanitize`. Resolution checks `SMS_TEST_CXX` / PATH / already existing
local host toolchains and the installed pdulib path from Arduino's configuration. No downloads,
installation, device access or network requests. Temporary build artifacts are removed even on failure.
The modem suite requires 32-bit `unsigned long` (Windows host GCC); a compile-time assertion prevents
an LP64 host from falsely claiming ESP32 `millis()` rollover coverage.

The complete, unmodified `modem_handler.ino` and `sms_handler.ino` are compiled together with their real
headers and installed pdulib. The wrapper only counts real encode/decode calls. The SMS driver feeds
one line at a time to preserve its original driver checks. Arbitrary fragmentation and whole bursts
are tested separately by modem. No copied transaction/parser implementation is used by the tests.

## Historical-stage results

The early-stage figures below are retained with the failure-reproduction evidence, not as current final results.
The completed final SMS/modem regressions had zero failures; final totals are recorded in [TODO.md](../TODO.md).
The full Arduino firmware build also succeeded; see [BUILD_VALIDATION.md](../docs/BUILD_VALIDATION.md).
Final `precommit` revalidation passed; see [TODO.md](../TODO.md) for the completed evidence.

Host GCC 16.2.0, `-Wall -Wextra -Werror`, bounds-checked String, no ASan/UBSan:

- SMS: **16208 assertions, 9 suites, 0 failures**.
- Modem: **4628 assertions, 22 suites, 0 failures**.
- Before this fix, the existing modem baseline passed **4301 assertions / 18 suites**. The added
  callback/synchronization/call-end regressions produced **6 failing suites** against the old
  implementation (4211 assertions reached, 4205 passed / 6 failed; runner exit 1). After the fix both
  runners exit 0. No skipped or expected-failure assertions are used.
- Changed-file IDE checks reported no errors; the IDE build returned success. Host GCC compiling the
  actual production modem/SMS sources is the authoritative C++ check here, not a full Arduino build.

## Fixed boundary: callback RX beyond the poll snapshot

`pollModem()` intentionally reads only its initial `Serial1.available()` snapshot so continuous RX
cannot prevent wait-loop timeout checks. A single pre-read in `submitPdu()` was therefore insufficient:

1. Begin `AT+CMGS=1` and receive the no-LF prompt.
2. Queue a CMT/PDU or CLIP URC; its history/call callback appends old `OK` / `+CMGS: 42` bytes while the
   submission pre-read is dispatching it. Those bytes are outside that poll's snapshot.
3. The old implementation switched generation and wrote PDU/Ctrl+Z without reading those bytes.
4. A later poll classified the old lines as post-submit responses and returned **Ok** without any new
   modem response. `begin()` had the same window for an old OK satisfying a new query.

Both generation transitions now call `synchronizeRx()`: dispatch another snapshot while unread RX
remains, keep the old generation (and input-pending state for SMS), and yield/feed the registered
watchdog between attempts. The `MODEM_RX_SYNC_MS` retry budget is **20ms**, using unsigned subtraction
across `millis()` rollover. Once the observed RX queue is empty, the transition can proceed immediately;
there is no mandatory quiet wait or extra AT command.

If unread RX remains at the retry deadline, return **Busy** without switching generation or writing
the new command/PDU/Ctrl+Z. A true error received during SMS pre-submit dispatch also rejects input.
Ownership stays with the transaction until `end()`/destruction; unsubmitted SMS input gets ESC and
the existing **250ms** recovery. Neither unread RX nor partial line/CMT/PDU state is cleared.
Partial response lines retain their old generation, so their later LF cannot confirm the new submission.

`NO CARRIER` is isolated as a call-end URC in the modem dispatcher, not an SMS/query terminal or response
data. Tests deliberately leave the call dispatch stub unaware of it, checking production isolation,
including between CMT header/PDU and during prompt/final waits. `ERROR`, `+CMS ERROR:` and `+CME ERROR:`
remain failures; existing `NO ANSWER` / `BUSY` behavior is unchanged. This is not a dial/data-call result
classifier. The timer Ping's `delay(2000)` is replaced only with `modemDelay(2000)` to service URCs while
waiting; the timer itself is not executed by these host suites.

## Coverage

- Every split of a valid CMT/header/PDU/CRLF stream across transaction begin; each partial split across
  end and main-loop continuation; success/error-recovery transitions to another transaction.
- Real sendSMS encode snapshot survives actual pdulib receive decode, queued RX before begin, CMT/CLIP/
  RING/NO CARRIER during no-LF prompt wait and after submission; exact TX and one Ctrl+Z.
- Complete OK AND numeric CMGS, either order, missing halves, malformed receipts, embedded OK, echo,
  previous-transaction partial OK, queued pre-submit finals and cross-submit partial finals stay stale.
- History/call callback injection outside the pre-submit snapshot: both complete terminal orders,
  either single half, partial OK/CMGS; old finals at command begin; queued/callback-injected errors.
- Continuous callback RX rejects begin/submit at the synchronization deadline, also across 32-bit wrap;
  watchdog/yield counts, no command/PDU/Ctrl+Z write, ESC cleanup, retained URC delivery and next-command recovery.
- NO CARRIER does not fail SMS/query, consume response capacity or interrupt pending CMT/PDU delivery;
  genuine ERROR/CMS/CME still fail before prompt, during pre-submit synchronization and final waits.
- Prompt whitespace/inline `>`/unexpected/duplicate; terminal errors; prompt/final timeout;
  late orphan prompt ESC, bounded recovery, destructor/idempotent end, invalid input and short writes.
- Busy nesting, callback reentry and reader-only reentry guards; UART reads by legacy overloads refused.
- Full overlong/NUL-line discard including hex/prompt-like suffix, CMT wait reset, next clean URC recovery,
  maximum accepted line, exact/beyond response capacity and independent URC burst budget.
- MPING result plus OK either order, missing halves and late asynchronous receipts isolated from queries.
- Real 32-bit clock rollover for delay/prompt/final waits; watchdog status/counts; continuous ingress poll
  budget; actual `checkSerial1URC()` / `pollModem()` shared state, not copied parsing logic.

## Limits

**SIM and real serial timing are not verified.** UART FIFO capacity/backpressure, baud timing, ESP heap
allocation failures, real FreeRTOS scheduling/concurrency, hardware watchdog/reset behavior and sustained
hardware load are not modeled. CLIP/RING use a counting dispatch stub, not the actual call parser/dedup;
history/HTTP/MQTT/email are in-memory sinks, not transports. The actual full `code.ino` main loop is not
executed, only its production URC entry point. MPING receipt payload semantics are not validated beyond
the production prefix/final contract. Infinitely late complete OK lines without identifiers cannot be
reliably correlated to old commands and are outside this bounded recovery model. In particular, the final
empty-RX observation is not atomic with generation change or UART writes: an old response whose first byte
arrives afterwards (including during PDU/Ctrl+Z TX) can still look new. Old `+CMGS` receipts also lack a usable
transaction identifier before submission. No finite drain/quiet interval can eliminate arbitrary late
responses on this protocol. The 20ms budget bounds retries, not time spent inside a single snapshot or
URC callback; callback latency and hardware FIFO/TX timing remain unverified. These host runners do not
execute Arduino builds; see [BUILD_VALIDATION.md](../docs/BUILD_VALIDATION.md) for the separately completed
full firmware build.
