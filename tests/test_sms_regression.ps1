# Offline behavioral regression: compile/run the real SMS source with installed pdulib.
# Requires PowerShell 7 and a host GCC/Clang compiler (NOT riscv32-esp-elf-g++).
# Example: pwsh -NoProfile -File tests/test_sms_regression.ps1 -CompilerPath C:/tools/bin/g++.exe
# No downloads/installation, serial devices, network, or firmware writes. Artifacts are temporary.
[CmdletBinding()]
param(
    [string]$CompilerPath = $env:SMS_TEST_CXX,
    [string]$PduLibPath,
    [string]$SourceDirectory = (Join-Path $PSScriptRoot '../code'),
    [switch]$Sanitize
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'sms_host_runner.ps1')
Invoke-SmsHostRegression -TestName sms_regression -CompilerPath $CompilerPath -PduLibPath $PduLibPath `
    -SourceDirectory $SourceDirectory -Sanitize:$Sanitize
