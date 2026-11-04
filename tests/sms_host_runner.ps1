# Shared offline SMS/modem host compiler runner. Dot-source from the two entry scripts.
# Sources are read-only; only a unique temporary directory is written and removed.
Set-StrictMode -Version Latest

function Resolve-SmsHostCompiler([string]$CompilerPath) {
    if (-not $CompilerPath) {
        $compiler = Get-Command g++, clang++ -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($compiler) { $CompilerPath = $compiler.Source }
    }
    if (-not $CompilerPath) {
        # Reuse already installed tooling; never fetch or install a compiler.
        $patterns = @(
            (Join-Path ([IO.Path]::GetTempPath()) 'msg-forward-sms-toolchain-*/w64devkit/bin/g++.exe'),
            'C:/msys64/ucrt64/bin/g++.exe', 'C:/msys64/mingw64/bin/g++.exe',
            (Join-Path $HOME 'scoop/apps/gcc/current/bin/g++.exe'))
        foreach ($pattern in $patterns) {
            $found = Resolve-Path -Path $pattern -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($found) { $CompilerPath = $found.Path; break }
        }
    }
    if (-not $CompilerPath -or -not (Test-Path -LiteralPath $CompilerPath -PathType Leaf)) {
        throw 'Installed host C++ compiler required: use -CompilerPath or SMS_TEST_CXX. No downloads are performed.'
    }
    if ($CompilerPath -match '(?i)(riscv|xtensa).*elf') { throw 'An ESP cross-compiler cannot run host regressions.' }
    return (Resolve-Path -LiteralPath $CompilerPath).Path
}

function Resolve-SmsHostPduLib([string]$PduLibPath) {
    if (-not $PduLibPath) {
        $arduinoConfig = Join-Path $HOME '.arduinoIDE/arduino-cli.yaml'
        if (Test-Path -LiteralPath $arduinoConfig) {
            $userDirectory = [regex]::Match((Get-Content -LiteralPath $arduinoConfig -Raw), '(?m)^\s+user:\s*(.+?)\s*$')
            if ($userDirectory.Success) {
                $PduLibPath = Join-Path $userDirectory.Groups[1].Value.Trim().Trim('"', "'") 'libraries/pdulib/src'
            }
        }
    }
    if (-not $PduLibPath -or -not (Test-Path -LiteralPath (Join-Path $PduLibPath 'pdulib.cpp'))) {
        throw 'Installed pdulib sources required: use -PduLibPath pointing to its src directory.'
    }
    return (Resolve-Path -LiteralPath $PduLibPath).Path
}

function Invoke-SmsHostProcess([string]$Executable, [string[]]$Arguments,
        [string]$WorkingDirectory, [string]$CompilerPath, [int]$TimeoutMs = 120000) {
    $start = [Diagnostics.ProcessStartInfo]::new($Executable)
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.Encoding]::UTF8
    $start.StandardErrorEncoding = [Text.Encoding]::UTF8
    $start.WorkingDirectory = $WorkingDirectory
    # Child-only DLL lookup; no changes to the caller/system PATH.
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

function Invoke-SmsHostRegression {
    param(
        [Parameter(Mandatory)][ValidateSet('sms_regression', 'modem_regression')][string]$TestName,
        [string]$CompilerPath = $env:SMS_TEST_CXX,
        [string]$PduLibPath,
        [string]$SourceDirectory = (Join-Path $PSScriptRoot '../code'),
        [switch]$Sanitize
    )
    $CompilerPath = Resolve-SmsHostCompiler $CompilerPath
    $PduLibPath = Resolve-SmsHostPduLib $PduLibPath
    $SourceDirectory = (Resolve-Path -LiteralPath $SourceDirectory).Path
    $root = Join-Path ([IO.Path]::GetTempPath()) ('msg-forward-' + $TestName + '-' + [guid]::NewGuid().ToString('N'))
    $null = New-Item -ItemType Directory -Path $root
    $processOptions = @{ WorkingDirectory = $root; CompilerPath = $CompilerPath }
    try {
        foreach ($name in @('Arduino.h', 'Preferences.h', 'esp_system.h', 'esp_task_wdt.h',
                'freertos/FreeRTOS.h', 'freertos/semphr.h')) {
            $path = Join-Path $root $name
            $null = New-Item -ItemType Directory -Force -Path (Split-Path $path)
            [IO.File]::WriteAllText($path, "#include `"sms_host_stubs.h`"`n", [Text.UTF8Encoding]::new($false))
        }
        # Exact copies avoid non-ASCII Windows toolchain paths; production headers/inos remain in code/.
        foreach ($name in @('pdulib.cpp', 'pdulib.h')) {
            Copy-Item -LiteralPath (Join-Path $PduLibPath $name) -Destination (Join-Path $root $name)
        }
        Write-Output "Host compiler: $CompilerPath"
        Invoke-SmsHostProcess $CompilerPath @('--version') @processOptions
        $flags = @('-std=c++17', '-O1', '-g', '-D_GLIBCXX_ASSERTIONS', '-fstack-protector-strong',
            '-finput-charset=UTF-8', '-fexec-charset=UTF-8')
        if ($Sanitize) { $flags += @('-fsanitize=address,undefined', '-fno-omit-frame-pointer') }
        $object = Join-Path $root 'pdulib.o'
        Invoke-SmsHostProcess $CompilerPath ($flags + @('-DDESKTOP_PDU', '-DPDU=SmsRealPdu', '-I', $root,
            '-c', (Join-Path $root 'pdulib.cpp'), '-o', $object)) @processOptions
        $executable = Join-Path $root ($TestName + '.exe')
        Invoke-SmsHostProcess $CompilerPath ($flags + @('-Wall', '-Wextra', '-Werror', '-Wno-sign-compare',
            '-I', $root, '-I', $PSScriptRoot, '-I', $SourceDirectory,
            (Join-Path $PSScriptRoot ($TestName + '.cpp')), $object, '-o', $executable)) @processOptions
        Write-Output 'Scope: SIM, real UART timing, hardware, network and Arduino build are NOT verified.'
        if (-not $Sanitize) { Write-Output 'ASan/UBSan not enabled; use -Sanitize with a supporting installed host compiler.' }
        Invoke-SmsHostProcess $executable @() @processOptions -TimeoutMs 60000
        Write-Output 'PASS: actual SMS + modem sources; bounds-checked host String and in-memory I/O.'
    } finally {
        Remove-Item -LiteralPath $root -Recurse -Force
    }
}
