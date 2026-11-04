# P2-06/07/08/09: compile/run the actual config, timer and Web timer code with a 64-bit test clock.
# PowerShell 7 + installed host GCC/Clang. Reuses sms_host_stubs.h without modifying existing tests.
# No downloads, devices, network or NVS writes. All artifacts and synthetic WiFi defaults are temporary.
[CmdletBinding()]
param(
    [string]$CompilerPath = $env:SMS_TEST_CXX,
    [string]$SourceDirectory = (Join-Path $PSScriptRoot '../code'),
    [switch]$Sanitize,
    [switch]$SkipBrowser,
    [string]$BrowserPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'sms_host_runner.ps1')
$CompilerPath = Resolve-SmsHostCompiler $CompilerPath
$SourceDirectory = (Resolve-Path -LiteralPath $SourceDirectory).Path
$root = Join-Path ([IO.Path]::GetTempPath()) ('msg-forward-config-timer-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $root
$utf8 = [Text.UTF8Encoding]::new($false)

function Invoke-Checked([string]$Executable, [string[]]$Arguments, [int]$TimeoutMs = 120000) {
    $start = [Diagnostics.ProcessStartInfo]::new($Executable)
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.Encoding]::UTF8
    $start.StandardErrorEncoding = [Text.Encoding]::UTF8
    $start.WorkingDirectory = $root
    $start.Environment['PATH'] = (Split-Path $CompilerPath) + [IO.Path]::PathSeparator + $env:PATH
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $start
    try {
        $null = $process.Start()
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit($TimeoutMs)) {
            $process.Kill($true); $process.WaitForExit()
            throw "Timed out: $Executable"
        }
        $output = $stdout.GetAwaiter().GetResult()
        $diagnostics = $stderr.GetAwaiter().GetResult()
        if ($output) { Write-Output $output.TrimEnd() }
        if ($diagnostics) { Write-Output $diagnostics.TrimEnd() }
        if ($process.ExitCode -ne 0) { throw "Failed with exit $($process.ExitCode): $Executable" }
    } finally { $process.Dispose() }
}

# Extract complete unchanged functions, not reimplementations. Mask comments/literals while counting braces.
function Get-RealFunction([string]$Text, [string]$Name) {
    $trivia = '(?s)/\*.*?\*/|//[^\r\n]*|"(?:\\.|[^"\\])*"|''(?:\\.|[^''\\])*'''
    $masked = [regex]::Replace($Text, $trivia, [Text.RegularExpressions.MatchEvaluator]{
        param($token)
        return [regex]::Replace($token.Value, '[^\r\n]', ' ')
    })
    $pattern = '(?m)^[ \t]*(?:static\s+)?(?:String|bool|int|void)\s+' + [regex]::Escape($Name) + '\s*\([^;{}]*\)\s*\{'
    $headers = [regex]::Matches($masked, $pattern)
    if ($headers.Count -ne 1) { throw "Expected exactly one definition: $Name" }
    $start = $headers[0].Index
    $depth = 1
    for ($i = $start + $headers[0].Length; $i -lt $masked.Length; $i++) {
        if ($masked[$i] -eq '{') { $depth++ }
        if ($masked[$i] -eq '}') { $depth-- }
        if ($depth -eq 0) { return $Text.Substring($start, $i - $start + 1) }
    }
    throw "Unbalanced function: $Name"
}

try {
    $shims = @('Arduino.h', 'Preferences.h', 'esp_system.h', 'esp_timer.h', 'esp_task_wdt.h',
        'freertos/FreeRTOS.h', 'freertos/semphr.h', 'WiFi.h', 'WiFiMulti.h', 'WiFiClientSecure.h',
        'WebServer.h', 'ESPmDNS.h', 'SPIFFS.h', 'pdulib.h', 'ReadyMail.h', 'HTTPClient.h',
        'base64.h', 'PubSubClient.h')
    foreach ($name in $shims) {
        $path = Join-Path $root $name
        $null = New-Item -ItemType Directory -Force -Path (Split-Path $path)
        [IO.File]::WriteAllText($path, "#include `"config_timer_host_stubs.h`"`n", $utf8)
    }
    # Copy the exact files under test. Relative wifi_config.h must resolve to synthetic values, not secrets.
    foreach ($name in @('code.ino', 'config.ino', 'config.h', 'web_handlers.ino')) {
        Copy-Item -LiteralPath (Join-Path $SourceDirectory $name) -Destination (Join-Path $root $name)
    }
    [IO.File]::WriteAllText((Join-Path $root 'wifi_config.h'), "#define WIFI_SSID `"test-wifi`"`n#define WIFI_PASS `"test-pass`"`n", $utf8)

    $sms = Get-Content -LiteralPath (Join-Path $SourceDirectory 'sms_handler.ino') -Raw -Encoding UTF8
    $push = Get-Content -LiteralPath (Join-Path $SourceDirectory 'push_service.ino') -Raw -Encoding UTF8
    $functions = @('cleanSmsContent', 'smsDaysInMonth', 'readSmsDecimal', 'formatTimestamp',
        'extractVerifyCode', 'isTimestampLikelyValidSms', 'processPreparedSmsContent', 'processSmsContent')
    $actual = @('// Generated unchanged production functions for history-before-filter integration.')
    foreach ($name in $functions) { $actual += Get-RealFunction $sms $name }
    $actual += Get-RealFunction $push 'jsonEscape'
    [IO.File]::WriteAllText((Join-Path $root 'config_timer_receive.inc'), ($actual -join "`n`n"), $utf8)

    # smsSent belongs to the caller, not sendSMS: prevents a later double-count regression.
    $sender = Get-RealFunction $sms 'sendSMS'
    if ($sender -match '\bstats\s*\.\s*smsSent|\bstatsIncrementSmsSent\s*\(') {
        throw 'sendSMS must not duplicate the timer caller success counter.'
    }
    Invoke-Checked $CompilerPath @('--version')
    $flags = @('-std=c++17', '-O1', '-g', '-D_GLIBCXX_ASSERTIONS', '-fstack-protector-strong', '-Wno-sign-compare',
        '-finput-charset=UTF-8', '-fexec-charset=UTF-8', '-Wall', '-Wextra', '-Werror')
    if ($Sanitize) { $flags += @('-fsanitize=address,undefined', '-fno-omit-frame-pointer') }
    $exe = Join-Path $root 'config_timer_regression.exe'
    Invoke-Checked $CompilerPath ($flags + @('-I', $root, '-I', $PSScriptRoot, '-I', $SourceDirectory,
        (Join-Path $PSScriptRoot 'config_timer_regression.cpp'), '-o', $exe))
    Invoke-Checked $exe @() 60000
    $strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
    foreach ($fixture in @('history_fixture.json', 'query_fixture.json')) {
        $text = $strictUtf8.GetString([IO.File]::ReadAllBytes((Join-Path $root $fixture)))
        $document = [System.Text.Json.JsonDocument]::Parse($text)
        $document.Dispose()
        Write-Output "PASS: independent strict UTF-8/JSON validation of $fixture"
    }
    if (-not $SkipBrowser) {
        & (Join-Path $PSScriptRoot 'test_history_dom.ps1') -SourcePath (Join-Path $SourceDirectory 'web_pages.h') `
            -BrowserPath $BrowserPath -HistoryFixturePath (Join-Path $root 'history_fixture.json')
    }
    Write-Output 'PASS: actual config/timer/Web timer code, in-memory persistence and controllable 64-bit clock.'
    Write-Output 'Scope: real config/timer and root streaming; strict JSON/save failures and browser DOM have separate regressions. Hardware endurance is not covered.'
    if (-not $Sanitize) { Write-Output 'ASan/UBSan not enabled; use -Sanitize with a supporting host compiler.' }
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force
}
