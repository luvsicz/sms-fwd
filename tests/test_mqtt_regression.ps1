# Offline MQTT host regression. Only current production functions are extracted, never rewritten.
[CmdletBinding()]
param(
    [string]$CompilerPath = $env:SMS_TEST_CXX,
    [string]$SourceDirectory = (Join-Path $PSScriptRoot '../code'),
    [string]$PubSubClientPath = $env:MQTT_TEST_PUBSUBCLIENT,
    [switch]$Sanitize
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'sms_host_runner.ps1')
$CompilerPath = Resolve-SmsHostCompiler $CompilerPath
$SourceDirectory = (Resolve-Path -LiteralPath $SourceDirectory).Path
if (-not $PubSubClientPath) {
    $arduinoConfig = Join-Path $HOME '.arduinoIDE/arduino-cli.yaml'
    if (Test-Path -LiteralPath $arduinoConfig) {
        $userDirectory = [regex]::Match((Get-Content -LiteralPath $arduinoConfig -Raw), '(?m)^\s+user:\s*(.+?)\s*$')
        if ($userDirectory.Success) {
            $PubSubClientPath = Join-Path $userDirectory.Groups[1].Value.Trim().Trim('"', "'") 'libraries/PubSubClient/src'
        }
    }
}
if ($PubSubClientPath -and (Test-Path -LiteralPath (Join-Path $PubSubClientPath 'src/PubSubClient.cpp'))) {
    $PubSubClientPath = Join-Path $PubSubClientPath 'src'
}
if (-not $PubSubClientPath -or -not (Test-Path -LiteralPath (Join-Path $PubSubClientPath 'PubSubClient.cpp'))) {
    throw 'Installed PubSubClient 2.8 sources required: use -PubSubClientPath or MQTT_TEST_PUBSUBCLIENT. No downloads.'
}
$PubSubClientPath = (Resolve-Path -LiteralPath $PubSubClientPath).Path
$properties = Join-Path (Split-Path $PubSubClientPath) 'library.properties'
if (-not (Test-Path -LiteralPath $properties) -or
        (Get-Content -LiteralPath $properties -Raw) -notmatch '(?m)^version=2\.8\s*$') {
    throw 'These transport/allocation probes require the installed PubSubClient 2.8 library (not a rewritten codec).'
}
$root = Join-Path ([IO.Path]::GetTempPath()) ('msg-forward-mqtt-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $root
$utf8 = [Text.UTF8Encoding]::new($false)

# Comments/literals are masked for brace matching only; returned source is byte-for-byte text.
function Get-MqttProductionFunction([string]$Text, [string]$Name, [string]$FileName = 'mqtt_handler.ino') {
    $trivia = '(?s)R"(?<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)\k<delimiter>"|/\*.*?\*/|//[^\r\n]*|"(?:\\.|[^"\\])*"|''(?:\\.|[^''\\])*'''
    $masked = [regex]::Replace($Text, $trivia, [Text.RegularExpressions.MatchEvaluator]{
        param($token)
        return [regex]::Replace($token.Value, '[^\r\n]', ' ')
    })
    $pattern = '(?m)^[ \t]*(?:static\s+)?(?:String|bool|int|void)\s+' + [regex]::Escape($Name) + '\s*\([^;{}]*\)\s*\{'
    $headers = [regex]::Matches($masked, $pattern)
    if ($headers.Count -ne 1) { throw "Expected exactly one production definition: $Name" }
    $start = $headers[0].Index
    $depth = 1
    for ($i = $start + $headers[0].Length; $i -lt $masked.Length; $i++) {
        if ($masked[$i] -eq '{') { $depth++ }
        if ($masked[$i] -eq '}') { $depth-- }
        if ($depth -eq 0) {
            $line = 1 + [regex]::Matches($Text.Substring(0, $start), '\n').Count
            $path = (Join-Path $SourceDirectory $FileName).Replace('\', '/')
            return "#line $line `"$path`"`n" + $Text.Substring($start, $i - $start + 1)
        }
    }
    throw "Unbalanced production function: $Name"
}

try {
    foreach ($name in @('Arduino.h', 'Preferences.h', 'esp_system.h', 'WiFi.h', 'PubSubClient.h',
            'IPAddress.h', 'Client.h', 'Stream.h')) {
        [IO.File]::WriteAllText((Join-Path $root $name), "#include `"mqtt_host_stubs.h`"`n", $utf8)
    }
    foreach ($name in @('mqtt_handler.h', 'config.h', 'json_input.h')) {
        Copy-Item -LiteralPath (Join-Path $SourceDirectory $name) -Destination (Join-Path $root $name)
    }
    $realLibrary = Join-Path $root 'mqtt_real'
    $null = New-Item -ItemType Directory -Path $realLibrary
    foreach ($name in @('PubSubClient.h', 'PubSubClient.cpp')) {
        Copy-Item -LiteralPath (Join-Path $PubSubClientPath $name) -Destination (Join-Path $realLibrary $name)
        Write-Output ("PubSubClient 2.8 {0} SHA256: {1}" -f $name,
            (Get-FileHash -LiteralPath (Join-Path $realLibrary $name) -Algorithm SHA256).Hash)
    }
    # Only the class name and allocation calls are interposed; the library files are unmodified copies.
    [IO.File]::WriteAllText((Join-Path $root 'mqtt_library.cpp'), @'
#include "mqtt_host_stubs.h"
#define PubSubClient MqttRealPubSubClient
#define malloc MqttHeap::mallocProbe
#define realloc MqttHeap::reallocProbe
#define free MqttHeap::release
#include "mqtt_real/PubSubClient.cpp"
#undef free
#undef realloc
#undef malloc
#undef PubSubClient
'@, $utf8)
    $mqtt = Get-Content -LiteralPath (Join-Path $SourceDirectory 'mqtt_handler.ino') -Raw -Encoding UTF8
    $ready = [regex]::Matches($mqtt, '(?m)^static bool mqttBufferReady = false;\r?$')
    if ($ready.Count -ne 1) { throw 'Cannot locate production MQTT buffer state.' }
    $queue = [regex]::Matches($mqtt, '(?ms)^static const uint8_t MQTT_LOG_QUEUE_SIZE\b.*?^static bool mqttLogQueueBusy = false;\r?$')
    if ($queue.Count -ne 1) { throw 'Cannot locate production MQTT log queue declarations.' }
    $push = Get-Content -LiteralPath (Join-Path $SourceDirectory 'push_service.ino') -Raw -Encoding UTF8
    $stateStart = $mqtt.IndexOf('static bool mqttBufferReady = false;', [StringComparison]::Ordinal)
    $stateEnd = $mqtt.IndexOf('void serviceMqtt()', [StringComparison]::Ordinal)
    $commandStart = $mqtt.IndexOf('struct MqttPendingSms {', [StringComparison]::Ordinal)
    $commandEnd = $mqtt.IndexOf('void mqttCallback(', [StringComparison]::Ordinal)
    if ($stateStart -lt 0 -or $stateEnd -le $stateStart -or $commandStart -lt 0 -or $commandEnd -le $commandStart) { throw 'Missing MQTT state or command queue declarations.' }
    $actual = @($mqtt.Substring($stateStart, $stateEnd - $stateStart), $queue[0].Value,
        $mqtt.Substring($commandStart, $commandEnd - $commandStart), (Get-MqttProductionFunction $push 'jsonEscape' 'push_service.ino'))
    $functionNames = @('serviceMqtt', 'ensureMqttBufferSize', 'yieldMqttPublish', 'publishMqttPayload',
        'appendMqttJsonBytes', 'appendMqttJsonQuoted', 'writeMqttJsonFields', 'buildMqttJson',
        'logMqttElapsed', 'truncateLogText', 'mqttLogTimeString', 'mqttLogTopicByKind',
        'buildMqttLogPayload', 'enqueueMqttLogEntry', 'publishMqttLogEntryToTopic',
        'publishDeviceLog', 'publishTimingLog', 'publishErrorLog', 'processMqttLogQueue',
        'getMacSuffix', 'mqttResetStatusFields', 'buildMqttStatusJson', 'initMqttTopics',
        'publishHaStatusSensor', 'publishHaDiscoveryConfig', 'mqttReconnect',
        'publishMqttSmsReceived', 'publishMqttCallReceived', 'publishMqttSmsSent',
        'publishMqttPingResult', 'publishMqttStatus', 'publishMqttDeviceStatus', 'mqttCallback')
    foreach ($name in $functionNames) { $actual += Get-MqttProductionFunction $mqtt $name }
    [IO.File]::WriteAllText((Join-Path $root 'mqtt_actual.inc'), ($actual -join "`n`n"), $utf8)
    $processOptions = @{ WorkingDirectory = $root; CompilerPath = $CompilerPath }
    Write-Output "Host compiler: $CompilerPath"
    Write-Output "Production: $($functionNames.Count) MQTT functions + jsonEscape; actual queue/buffer state and headers."
    $flags = @('-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        # The untouched Arduino sources compare signed indexes with String::length().
        '-Wno-sign-compare', '-D_GLIBCXX_ASSERTIONS', '-fstack-protector-strong',
        '-finput-charset=UTF-8', '-fexec-charset=UTF-8', '-I', $root, '-I', $PSScriptRoot)
    if ($Sanitize) { $flags += @('-fsanitize=address,undefined', '-fno-omit-frame-pointer') }
    $object = Join-Path $root 'mqtt_library.o'
    Invoke-SmsHostProcess $CompilerPath ($flags + @('-c', (Join-Path $root 'mqtt_library.cpp'), '-o', $object)) @processOptions
    $exe = Join-Path $root 'mqtt_regression.exe'
    Invoke-SmsHostProcess $CompilerPath ($flags + @((Join-Path $PSScriptRoot 'mqtt_regression.cpp'), $object, '-o', $exe)) @processOptions
    $runtimeError = $null
    try { Invoke-SmsHostProcess $exe @() @processOptions -TimeoutMs 60000 } catch { $runtimeError = $_ }
    $summaryPath = Join-Path $root 'mqtt_summary.tsv'
    if (-not (Test-Path -LiteralPath $summaryPath)) {
        if ($runtimeError) { throw $runtimeError }
        throw 'MQTT host test did not produce an assertion summary.'
    }
    $summary = [regex]::Match([IO.File]::ReadAllText($summaryPath).Trim(), '^(\d+)\t(\d+)\t(\d+)$')
    if (-not $summary.Success) { throw 'Invalid native assertion summary.' }

    # Independent strict UTF-8/JSON oracle, including all captured HA configs and LWTs.
    # Binary length framing is test metadata, not a second MQTT/JSON encoder.
    $jsonAssertions = 0
    $jsonFailures = 0
    $strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
    $reader = [IO.BinaryReader]::new([IO.File]::OpenRead((Join-Path $root 'mqtt_json_observations.bin')))
    try {
        while ($reader.BaseStream.Position -lt $reader.BaseStream.Length) {
            $labelLength = $reader.ReadUInt32()
            if ($labelLength -gt 4096) { throw 'Invalid JSON observation label length.' }
            $label = $strictUtf8.GetString($reader.ReadBytes($labelLength))
            $length = $reader.ReadUInt32()
            if ($length -gt 65536) { throw 'Invalid JSON observation payload length.' }
            $bytes = $reader.ReadBytes($length)
            if ($bytes.Length -ne $length) { throw 'Truncated JSON observation.' }
            $jsonAssertions++
            $document = $null
            try {
                $document = [System.Text.Json.JsonDocument]::Parse($strictUtf8.GetString($bytes))
                if ($document.RootElement.ValueKind -ne [System.Text.Json.JsonValueKind]::Object) {
                    throw 'Expected a complete JSON object.'
                }
            } catch {
                $jsonFailures++
                Write-Output "FAIL JSON [$label]: $($_.Exception.Message)"
            } finally { if ($null -ne $document) { $document.Dispose() } }
        }
    } finally { $reader.Dispose() }
    if ($jsonAssertions -eq 0) { throw 'No outbound JSON was observed; independent validation cannot be skipped.' }
    $assertions = [int]$summary.Groups[1].Value + $jsonAssertions
    $failures = [int]$summary.Groups[2].Value + $jsonFailures
    $result = if ($runtimeError -or $failures) { 'FAIL' } else { 'PASS' }
    Write-Output "$result MQTT: $assertions assertions ($($summary.Groups[1].Value) native + $jsonAssertions independent JSON), $failures failures, $($summary.Groups[3].Value) native cases."
    Write-Output 'Scope: real PubSubClient codec, keepalive, inbound callback and outbound MQTT/HA/LWT contract; no broker LWT delivery, HA UI or real ESP/network timing.'
    if (-not $Sanitize) { Write-Output 'ASan/UBSan not run; allocator red zones and bounds-checked host String enabled.' }
    if ($runtimeError -or $failures) { throw 'MQTT regression failed; failures above are retained, not marked expected or bypassed.' }
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force
}