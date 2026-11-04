# Offline behavioral regression of the actual modem_handler.ino + sms_handler.ino.
# PowerShell 7, installed host GCC/Clang and pdulib only; no downloads/devices/network.
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
Invoke-SmsHostRegression -TestName modem_regression -CompilerPath $CompilerPath -PduLibPath $PduLibPath `
    -SourceDirectory $SourceDirectory -Sanitize:$Sanitize
