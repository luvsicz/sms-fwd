# Additional independent offline push tests. Existing push tests/stubs remain read-only.
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
    foreach ($pattern in @('C:/msys64/ucrt64/bin/g++.exe', 'C:/msys64/mingw64/bin/g++.exe',
            (Join-Path $HOME 'scoop/apps/gcc/current/bin/g++.exe'),
            (Join-Path ([IO.Path]::GetTempPath()) 'msg-forward-sms-toolchain-*/w64devkit/bin/g++.exe'))) {
        $found = Resolve-Path -Path $pattern -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($found) { $CompilerPath = $found.Path; break }
    }
}
if (-not $CompilerPath -or -not (Test-Path -LiteralPath $CompilerPath -PathType Leaf)) {
    throw 'Installed host g++/clang++ required: use -CompilerPath or PUSH_TEST_CXX. No downloads.'
}
if ($CompilerPath -match '(?i)(riscv|xtensa).*elf') { throw 'A host compiler, not an ESP cross-compiler, is required.' }
$CompilerPath = (Resolve-Path -LiteralPath $CompilerPath).Path
$SourceDirectory = (Resolve-Path -LiteralPath $SourceDirectory).Path
$sourcePath = Join-Path $SourceDirectory 'push_service.ino'
$source = Get-Content -LiteralPath $sourcePath -Raw -Encoding UTF8
$trivia = '(?s)R"(?<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)\k<delimiter>"|/\*.*?\*/|//[^\r\n]*|"(?:\\.|[^"\\])*"|''(?:\\.|[^''\\])*'''
$masked = [regex]::Replace($source, $trivia, [Text.RegularExpressions.MatchEvaluator]{
    param($token)
    return [regex]::Replace($token.Value, '[^\r\n]', ' ')
})

function Get-SourceRange([int]$Start, [int]$Length) {
    $line = 1 + [regex]::Matches($source.Substring(0, $Start), '\n').Count
    return "#line $line `"$($sourcePath.Replace('\', '/'))`"`n" + $source.Substring($Start, $Length)
}
function Get-Declaration([string]$Pattern, [string]$Name) {
    $found = [regex]::Matches($masked, $Pattern)
    if ($found.Count -ne 1) { throw "Expected one production declaration: $Name" }
    return Get-SourceRange $found[0].Index $found[0].Length
}
function Get-Block([string]$Name, [switch]$Struct) {
    $pattern = if ($Struct) { '(?m)^[ \t]*struct\s+' + $Name + '\s*\{' }
               else { '(?m)^[ \t]*(?:static\s+)?(?:void|bool|int)\s+' + $Name + '\s*\([^;{}]*\)\s*\{' }
    $found = [regex]::Matches($masked, $pattern)
    if ($found.Count -ne 1) { throw "Expected one production definition: $Name" }
    $start = $found[0].Index
    $depth = 1
    for ($i = $start + $found[0].Length; $i -lt $masked.Length; $i++) {
        if ($masked[$i] -eq '{') { $depth++ }
        if ($masked[$i] -eq '}') { $depth-- }
        if ($depth -eq 0) {
            if ($Struct) {
                $i++
                while ($i -lt $masked.Length -and [char]::IsWhiteSpace($masked[$i])) { $i++ }
                if ($i -ge $masked.Length -or $masked[$i] -ne ';') { throw "Missing terminator: $Name" }
            }
            return Get-SourceRange $start ($i - $start + 1)
        }
    }
    throw "Unbalanced definition: $Name"
}
function Invoke-Host([string]$Executable, [string[]]$Arguments, [int]$TimeoutMs = 120000) {
    $start = [Diagnostics.ProcessStartInfo]::new($Executable)
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = $start.StandardErrorEncoding = [Text.Encoding]::UTF8
    $start.WorkingDirectory = $tempRoot
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
        if ($process.ExitCode -ne 0) { throw "Exit $($process.ExitCode): $Executable" }
    } finally { $process.Dispose() }
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('msg-forward-push-recovery-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $tempRoot
$utf8 = [Text.UTF8Encoding]::new($false)
try {
    foreach ($header in @('Arduino.h', 'Preferences.h', 'esp_system.h')) {
        [IO.File]::WriteAllText((Join-Path $tempRoot $header), "#include `"push_host_stubs.h`"`n", $utf8)
    }
    $actual = @(
        (Get-Declaration '(?m)^static SemaphoreHandle_t smtpMutex\s*=\s*nullptr\s*;' 'smtpMutex'),
        (Get-Block 'SmtpLockGuard' -Struct), (Get-Block 'EmailJob' -Struct)
    )
    foreach ($name in @('HTTP_PUSH_TIMEOUT_MS', 'EMAIL_QUEUE_LENGTH', 'EMAIL_TASK_STACK_SIZE',
                        'SMTP_STAGE_TIMEOUT_MS', 'SMTP_PROGRESS_DELAY_TICKS')) {
        $actual += Get-Declaration ('(?m)^static const \w+\s+' + $name + '\s*=\s*[^;]+;') $name
    }
    foreach ($name in @('emailQueue', 'emailTaskHandle')) {
        $actual += Get-Declaration ('(?m)^static \w+\s+' + $name + '\s*=\s*nullptr\s*;') $name
    }
    $actual += Get-Declaration '(?m)^static void sendEmailNotificationNow\s*\([^;{}]*\)\s*;' 'sender prototype'
    $functions = @('initSmtpLock', 'feedPushWatchdog', 'copyEmailString', 'emailWorkerTask', 'waitForSmtpProgress',
        'ensureEmailWorker', 'enqueueEmail', 'sendHttpRequest', 'sendEmailNotification',
        'buildEmailAddressHeader', 'sendEmailNotificationNow')
    foreach ($name in $functions) { $actual += Get-Block $name }
    [IO.File]::WriteAllText((Join-Path $tempRoot 'push_snapshot_actual.inc'), ($actual -join "`n`n"), $utf8)
    Write-Output "Production: $($functions.Count) verbatim functions, real EmailJob/lock/queue declarations and headers."
    Write-Output "push_service.ino SHA256: $((Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash)"
    Write-Output "Compiler: $CompilerPath"
    $flags = @('-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-D_GLIBCXX_ASSERTIONS',
        '-fstack-protector-strong', '-finput-charset=UTF-8', '-fexec-charset=UTF-8',
        '-I', $tempRoot, '-I', $PSScriptRoot, '-I', $SourceDirectory)
    if ($Sanitize) { $flags += @('-fsanitize=address,undefined', '-fno-omit-frame-pointer') }
    $executable = Join-Path $tempRoot 'push_snapshot_recovery.exe'
    Invoke-Host $CompilerPath ($flags + @((Join-Path $PSScriptRoot 'push_snapshot_recovery.cpp'), '-o', $executable))
    Invoke-Host $executable @() -TimeoutMs 60000
    Write-Output 'Scope: offline production control flow with String/RTOS/ReadyMail/transport substitutes; no sockets or hardware.'
    Write-Output 'Not verified: real ESP scheduling, ReadyMail internal allocations or TLS/network behavior.'
    if (-not $Sanitize) { Write-Output 'ASan/UBSan off; String red zones, job ownership and HTTP/TLS lifetime probes on.' }
} finally {
    Remove-Item -LiteralPath $tempRoot -Recurse -Force
}
