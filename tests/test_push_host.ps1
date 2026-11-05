# Independent offline push regression: production functions + API substitutes, no downloads/services.
[CmdletBinding()]
param(
    [string]$CompilerPath = $env:PUSH_TEST_CXX,
    [string]$SourceDirectory = (Join-Path $PSScriptRoot '../code'),
    [switch]$Sanitize
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $CompilerPath) {
    $compiler = Get-Command g++, clang++ -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($compiler) { $CompilerPath = $compiler.Source }
}
if (-not $CompilerPath) {
    # Reuse installed tools only. A path may also be supplied explicitly on any host.
    foreach ($pattern in @('C:/msys64/ucrt64/bin/g++.exe', 'C:/msys64/mingw64/bin/g++.exe',
            (Join-Path $HOME 'scoop/apps/gcc/current/bin/g++.exe'),
            (Join-Path ([IO.Path]::GetTempPath()) 'msg-forward-sms-toolchain-*/w64devkit/bin/g++.exe'))) {
        $found = Resolve-Path -Path $pattern -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) { $CompilerPath = $found.Path; break }
    }
}
if (-not $CompilerPath -or -not (Test-Path -LiteralPath $CompilerPath -PathType Leaf)) {
    throw 'Installed host g++/clang++ required: use -CompilerPath or PUSH_TEST_CXX. Nothing is downloaded.'
}
if ($CompilerPath -match '(?i)(riscv|xtensa).*elf') { throw 'ESP cross-compilers cannot execute host regressions.' }
$CompilerPath = (Resolve-Path -LiteralPath $CompilerPath).Path
$SourceDirectory = (Resolve-Path -LiteralPath $SourceDirectory).Path
$sourcePath = Join-Path $SourceDirectory 'push_service.ino'
$source = Get-Content -LiteralPath $sourcePath -Raw -Encoding UTF8
# Mask trivia for matching only; compiled definitions retain the original source text and #line locations.
$trivia = '(?s)R"(?<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)\k<delimiter>"|/\*.*?\*/|//[^\r\n]*|"(?:\\.|[^"\\])*"|''(?:\\.|[^''\\])*'''
$masked = [regex]::Replace($source, $trivia, [Text.RegularExpressions.MatchEvaluator]{
    param($token)
    return [regex]::Replace($token.Value, '[^\r\n]', ' ')
})
function Get-PushSourceRange([int]$Start, [int]$Length) {
    $line = 1 + [regex]::Matches($source.Substring(0, $Start), '\n').Count
    return "#line $line `"$($sourcePath.Replace('\', '/'))`"`n" + $source.Substring($Start, $Length)
}
function Get-PushDeclaration([string]$Pattern, [string]$Name) {
    $matches = [regex]::Matches($masked, $Pattern)
    if ($matches.Count -ne 1) { throw "Expected exactly one production declaration: $Name" }
    return Get-PushSourceRange $matches[0].Index $matches[0].Length
}
function Get-PushBlock([string]$Name, [switch]$Struct) {
    $pattern = if ($Struct) { '(?m)^[ \t]*struct\s+' + $Name + '\s*\{' }
               else { '(?m)^[ \t]*(?:static\s+)?(?:void|bool|int|String)\s+' + $Name + '\s*\([^;{}]*\)\s*\{' }
    $headers = [regex]::Matches($masked, $pattern)
    if ($headers.Count -ne 1) { throw "Expected exactly one production definition: $Name" }
    $start = $headers[0].Index
    $depth = 1
    for ($i = $start + $headers[0].Length; $i -lt $masked.Length; $i++) {
        if ($masked[$i] -eq '{') { $depth++ }
        if ($masked[$i] -eq '}') { $depth-- }
        if ($depth -eq 0) {
            if ($Struct) {
                $i++
                while ($i -lt $masked.Length -and [char]::IsWhiteSpace($masked[$i])) { $i++ }
                if ($i -ge $masked.Length -or $masked[$i] -ne ';') { throw "Missing struct terminator: $Name" }
            }
            return Get-PushSourceRange $start ($i - $start + 1)
        }
    }
    throw "Unbalanced production definition: $Name"
}
function Invoke-PushProcess([string]$Executable, [string[]]$Arguments, [int]$TimeoutMs = 120000) {
    $start = [Diagnostics.ProcessStartInfo]::new($Executable)
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.Encoding]::UTF8
    $start.StandardErrorEncoding = [Text.Encoding]::UTF8
    $start.WorkingDirectory = $tempRoot
    # DLL search change is child-only; preserve caller/system PATH.
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

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('msg-forward-push-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $tempRoot
$utf8 = [Text.UTF8Encoding]::new($false)
try {
    foreach ($header in @('Arduino.h', 'Preferences.h', 'esp_system.h')) {
        [IO.File]::WriteAllText((Join-Path $tempRoot $header), "#include `"push_host_stubs.h`"`n", $utf8)
    }
    $actual = @(
        (Get-PushDeclaration '(?m)^static SemaphoreHandle_t smtpMutex\s*=\s*nullptr\s*;' 'smtpMutex'),
        (Get-PushBlock 'SmtpLockGuard' -Struct), (Get-PushBlock 'EmailJob' -Struct),
        (Get-PushBlock 'HttpPushJob' -Struct)
    )
    foreach ($name in @('HTTP_PUSH_TIMEOUT_MS', 'HTTP_PUSH_QUEUE_LENGTH', 'HTTP_PUSH_TASK_STACK_SIZE',
                        'HTTP_PUSH_WORKER_COUNT', 'EMAIL_QUEUE_LENGTH', 'EMAIL_TASK_STACK_SIZE',
                        'SMTP_STAGE_TIMEOUT_MS', 'SMTP_PROGRESS_DELAY_TICKS')) {
        $actual += Get-PushDeclaration ('(?m)^static const \w+\s+' + $name + '\s*=\s*[^;]+;') $name
    }
    # Include any additional HTTP timeout budgets from the current implementation, also allowing
    # this runner to prove failure against the original one-second implementation.
    foreach ($name in @('HTTP_PUSH_CONNECT_TIMEOUT_MS', 'HTTP_PUSH_TLS_HANDSHAKE_TIMEOUT_SEC',
                        'HTTP_PUSH_PREVIEW_TIMEOUT_MS')) {
        if ($masked -match ('\b' + $name + '\s*=')) {
            $actual += Get-PushDeclaration ('(?m)^static const \w+\s+' + $name + '\s*=\s*[^;]+;') $name
        }
    }
    foreach ($name in @('emailQueue', 'emailTaskHandle', 'httpPushQueue')) {
        $actual += Get-PushDeclaration ('(?m)^static \w+\s+' + $name + '\s*=\s*nullptr\s*;') $name
    }
    $actual += Get-PushDeclaration '(?m)^static TaskHandle_t httpPushTaskHandles\[[^;]+;' 'HTTP worker handles'
    $actual += Get-PushDeclaration '(?m)^static void sendEmailNotificationNow\s*\([^;{}]*\)\s*;' 'sender prototype'
    $functions = @('initSmtpLock', 'feedPushWatchdog', 'copyEmailString', 'emailWorkerTask', 'waitForSmtpProgress',
        'ensureEmailWorker', 'enqueueEmail', 'httpPushWorkerTask', 'ensureHttpPushWorker', 'enqueueHttpPush',
        'urlEncode', 'jsonEscape', 'telegramEscape', 'sendHttpRequest', 'sendToChannel', 'sendSMSToServer', 'sendEmailNotification',
        'buildEmailAddressHeader', 'sendEmailNotificationNow')
    foreach ($name in $functions) { $actual += Get-PushBlock $name }
    [IO.File]::WriteAllText((Join-Path $tempRoot 'push_actual.inc'), ($actual -join "`n`n"), $utf8)
    $configPath = Join-Path $SourceDirectory 'config.ino'
    $configSource = Get-Content -LiteralPath $configPath -Raw -Encoding UTF8
    $validation = [regex]::Match($configSource, '(?ms)^bool isPushChannelValid\(const PushChannel& ch\) \{.*?^\}')
    if (-not $validation.Success) { throw 'Missing real isPushChannelValid definition' }
    [IO.File]::WriteAllText((Join-Path $tempRoot 'push_config_actual.inc'), $validation.Value, $utf8)
    Write-Output "Production: $($functions.Count) unmodified functions, actual EmailJob/lock/queue declarations and config/push headers."
    Write-Output "push_service.ino SHA256: $((Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash)"
    Write-Output "Host compiler: $CompilerPath"
    Invoke-PushProcess $CompilerPath @('--version')
    $flags = @('-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-D_GLIBCXX_ASSERTIONS', '-fstack-protector-strong', '-finput-charset=UTF-8', '-fexec-charset=UTF-8',
        '-I', $tempRoot, '-I', $PSScriptRoot, '-I', $SourceDirectory)
    if ($Sanitize) { $flags += @('-fsanitize=address,undefined', '-fno-omit-frame-pointer') }
    $executable = Join-Path $tempRoot 'push_regression.exe'
    Invoke-PushProcess $CompilerPath ($flags + @((Join-Path $PSScriptRoot 'push_regression.cpp'), '-o', $executable))
    Invoke-PushProcess $executable @() -TimeoutMs 60000
    Write-Output 'Scope: offline production enqueue/worker/SMTP/HTTP control flow; String/RTOS/ReadyMail/transports are substitutes.'
    Write-Output 'No hardware, sockets, mail service, real ESP scheduling or third-party internal allocation behavior is verified.'
    if (-not $Sanitize) { Write-Output 'ASan/UBSan not enabled; String allocation red zones and ownership/lifetime probes are enabled.' }
} finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force
}
