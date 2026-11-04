# P2-04/P2-05: host-compile shared JsonInput and unchanged extracted production handlers.
# Offline: no downloads, hardware, credentials, network or persistent NVS access.
[CmdletBinding()]
param(
    [string]$CompilerPath = $env:SMS_TEST_CXX,
    [string]$SourceDirectory = (Join-Path $PSScriptRoot '../code'),
    [switch]$Sanitize,
    [ValidateSet('json_handlers_regression', 'web_regression')]
    [string]$TestName = 'json_handlers_regression',
    [string]$FixtureDirectory
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'sms_host_runner.ps1')
$CompilerPath = Resolve-SmsHostCompiler $CompilerPath
$SourceDirectory = (Resolve-Path -LiteralPath $SourceDirectory).Path
$root = Join-Path ([IO.Path]::GetTempPath()) ('msg-forward-json-handlers-' + [guid]::NewGuid().ToString('N'))
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
            $process.Kill($true); $process.WaitForExit(); throw "Timed out: $Executable"
        }
        $output = $stdout.GetAwaiter().GetResult()
        $diagnostics = $stderr.GetAwaiter().GetResult()
        if ($output) { Write-Output $output.TrimEnd() }
        if ($diagnostics) { Write-Output $diagnostics.TrimEnd() }
        if ($process.ExitCode -ne 0) { throw "Failed with exit $($process.ExitCode): $Executable" }
    } finally { $process.Dispose() }
}

# Preserve exact function bodies; mask raw strings, strings and comments only while counting braces.
function Get-RealFunction([string]$Text, [string]$Name) {
    $trivia = '(?s)R"(?<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)\k<delimiter>"|/\*.*?\*/|//[^\r\n]*|"(?:\\.|[^"\\])*"|''(?:\\.|[^''\\])*'''
    $masked = [regex]::Replace($Text, $trivia, [Text.RegularExpressions.MatchEvaluator]{
        param($token)
        return [regex]::Replace($token.Value, '[^\r\n]', ' ')
    })
    $pattern = '(?m)^[ \t]*(?:static\s+)?(?:String|bool|int|void|uint64_t)\s+' + [regex]::Escape($Name) + '\s*\([^;{}]*\)\s*\{'
    $headers = [regex]::Matches($masked, $pattern)
    if ($headers.Count -ne 1) { throw "Expected exactly one function definition: $Name" }
    $start = $headers[0].Index
    $depth = 1
    for ($i = $start + $headers[0].Length; $i -lt $masked.Length; $i++) {
        if ($masked[$i] -eq '{') { $depth++ }
        if ($masked[$i] -eq '}') { $depth-- }
        if ($depth -eq 0) { return $Text.Substring($start, $i - $start + 1) }
    }
    throw "Unbalanced production function: $Name"
}

try {
    foreach ($name in @('Arduino.h', 'Preferences.h', 'esp_system.h')) {
        [IO.File]::WriteAllText((Join-Path $root $name), "#include `"json_handlers_host_stubs.h`"`n", $utf8)
    }
    # Keep the actual parser/header/templates byte-for-byte; never read wifi_config.h.
    foreach ($name in @('json_input.h', 'config.h', 'web_stream.h', 'web_output.h', 'web_pages.h', 'modem_handler.h')) {
        Copy-Item -LiteralPath (Join-Path $SourceDirectory $name) -Destination (Join-Path $root $name)
    }
    $web = Get-Content -LiteralPath (Join-Path $SourceDirectory 'web_handlers.ino') -Raw -Encoding UTF8
    $mqtt = Get-Content -LiteralPath (Join-Path $SourceDirectory 'mqtt_handler.ino') -Raw -Encoding UTF8
    $config = Get-Content -LiteralPath (Join-Path $SourceDirectory 'config.ino') -Raw -Encoding UTF8
    $actual = @('// Generated exact production functions; only dependencies and I/O are substitutes.')
    foreach ($name in @('makeNvsWifiKey', 'monotonicMillis', 'resetTimerSchedule', 'timerRemainingSeconds',
            'isPushChannelValid', 'isConfigValid', 'configStringFitsNvs', 'configFitsNvs',
            'putConfigString', 'configSaveFailure', 'saveConfig', 'loadConfig')) {
        $actual += Get-RealFunction $config $name
    }
    foreach ($name in @('checkAuth', 'sendWebChunk', 'handleRoot', 'sendConfigSaveError', 'normalizeConfigList', 'configCopyComplete', 'handleSave', 'handleSendSms',
            'handleTimer', 'handleFilterSave', 'handleContentFilterSave')) {
        $actual += Get-RealFunction $web $name
    }
    # The existing status command now delegates retained/HA publication to this helper.
    foreach ($name in @('logMqttElapsed', 'publishMqttStatus')) {
        $actual += Get-RealFunction $mqtt $name
    }
    # Queue globals are extracted too, so pending semantics/capacity are not mirrored in the tests.
    $queueStart = $mqtt.IndexOf('struct MqttPendingSms {', [StringComparison]::Ordinal)
    $callbackStart = $mqtt.IndexOf('void mqttCallback(', [StringComparison]::Ordinal)
    if ($queueStart -lt 0 -or $callbackStart -le $queueStart) { throw 'Cannot locate actual MQTT queue declarations.' }
    $actual += $mqtt.Substring($queueStart, $callbackStart - $queueStart)
    $actual += Get-RealFunction $mqtt 'mqttCallback'
    $actual += Get-RealFunction $mqtt 'processMqttPendingCommands'
    [IO.File]::WriteAllText((Join-Path $root 'json_handlers_actual.inc'), ($actual -join "`n`n"), $utf8)

    Invoke-Checked $CompilerPath @('--version')
    $flags = @('-std=c++17', '-O1', '-g', '-D_GLIBCXX_ASSERTIONS', '-fstack-protector-strong',
        '-finput-charset=UTF-8', '-fexec-charset=UTF-8', '-Wall', '-Wextra', '-Werror')
    if ($Sanitize) { $flags += @('-fsanitize=address,undefined', '-fno-omit-frame-pointer') }
    $exe = Join-Path $root ($TestName + '.exe')
    Invoke-Checked $CompilerPath ($flags + @('-I', $root, '-I', $PSScriptRoot,
        (Join-Path $PSScriptRoot ($TestName + '.cpp')), '-o', $exe))
    $runArguments = if ($FixtureDirectory) { @($FixtureDirectory) } else { @() }
    Invoke-Checked $exe $runArguments 60000
    Write-Output 'PASS: shared parser + extracted Web/MQTT/config load/save/root handlers; I/O is substituted.'
    if (-not $Sanitize) { Write-Output 'ASan/UBSan not enabled. Hardware, Arduino build and HTTP transport buffering are outside this offline test.' }
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force
}
