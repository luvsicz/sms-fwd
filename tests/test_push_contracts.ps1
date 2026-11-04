# Offline source contracts only: no firmware execution, network, hardware or dependencies.
# These guards do not replace compiling the complete Arduino sketch.
[CmdletBinding()]
param(
    [string]$SourcePath = (Join-Path $PSScriptRoot '../code/push_service.ino')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$script:passed = 0

function Assert-Contract([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "FAIL: $Message" }
    $script:passed++
    Write-Output "PASS: $Message"
}

function Get-CodeBlock([string]$Text, [string]$HeaderPattern, [string]$Name) {
    $headers = [regex]::Matches($Text, $HeaderPattern)
    if ($headers.Count -ne 1) { throw "Expected exactly one definition of $Name" }
    $open = $headers[0].Index + $headers[0].Length - 1
    $depth = 1
    for ($i = $open + 1; $i -lt $Text.Length; $i++) {
        if ($Text[$i] -eq '{') { $depth++ }
        if ($Text[$i] -eq '}') { $depth-- }
        if ($depth -eq 0) { return $Text.Substring($open + 1, $i - $open - 1) }
    }
    throw "Unbalanced braces in $Name"
}

$source = Get-Content -LiteralPath $SourcePath -Raw -Encoding UTF8
# Mask comments and ordinary C++ string/character literals before matching or counting braces.
# This avoids mistaking URLs, diagnostic text and escaped quotes for executable code.
$trivia = '(?s)/\*.*?\*/|//[^\r\n]*|"(?:\\.|[^"\\])*"|''(?:\\.|[^''\\])*'''
$code = [regex]::Replace($source, $trivia, [System.Text.RegularExpressions.MatchEvaluator]{
    param($token)
    return [regex]::Replace($token.Value, '[^\r\n]', ' ')
})

$job = Get-CodeBlock $code '\bstruct\s+EmailJob\s*\{' 'EmailJob'
$blocks = @{}
foreach ($name in @('sendHttpRequest', 'enqueueEmail', 'ensureEmailWorker',
                    'emailWorkerTask', 'sendEmailNotificationNow', 'waitForSmtpProgress',
                    'copyEmailString', 'buildEmailAddressHeader', 'sendEmailNotification')) {
    $header = '(?m)^\s*(?:static\s+)?(?:void|bool|int)\s+' + $name + '\s*\([^;{}]*\)\s*\{'
    $blocks[$name] = Get-CodeBlock $code $header $name
}
$enqueue = $blocks['enqueueEmail']
$worker = $blocks['emailWorkerTask']
$send = $blocks['sendEmailNotificationNow']

Assert-Contract ($blocks['sendHttpRequest'] -cmatch
    '\bWiFiClientSecure\s+localSslClient\s*;\s*HTTPClient\s+http\s*;') `
    'P1-03: TLS client is declared before HTTPClient in the same scope (destroyed last)'

$smtpStrings = @('smtpServer', 'smtpUser', 'smtpPass', 'smtpSendTo')
foreach ($field in (@('subject', 'body') + $smtpStrings)) {
    Assert-Contract ($job -cmatch "\bString\s+$field\s*;") "EmailJob owns $field by value"
}
Assert-Contract ($job -cmatch '\bint\s+smtpPort\s*;') 'EmailJob owns the SMTP port by value'
Assert-Contract ($job -cmatch '\bEmailJob\s*\(\s*\)\s*=\s*default\s*;') `
    'EmailJob remains default-constructible for checked field-by-field initialization'
Assert-Contract ($job -cmatch '\bEmailJob\s*\(\s*const\s+EmailJob\s*&\s*\)\s*=\s*delete\s*;') `
    'EmailJob rejects unchecked implicit String copy construction'
Assert-Contract ($job -cmatch '\bEmailJob\s*&\s*operator\s*=\s*\(\s*const\s+EmailJob\s*&\s*\)\s*=\s*delete\s*;') `
    'EmailJob rejects unchecked implicit String copy assignment'
Assert-Contract ($enqueue -cmatch '\A\s*if\s*\(\s*!config\.emailEnabled\s*\)\s*return\s+true\s*;') `
    'P1-04: disabled email is a no-op before allocation/enqueue; enabled state is decided by the producer'

$queueSend = [regex]::Match($enqueue, '\bxQueueSend\s*\(\s*emailQueue\s*,\s*&job\s*,\s*0\s*\)')
Assert-Contract $queueSend.Success 'Enqueue passes the EmailJob pointer, not String object bytes'
foreach ($field in $smtpStrings) {
    $copy = [regex]::Match($enqueue, "\bcopyEmailString\s*\(\s*job->$field\s*,\s*config\.$field\.c_str\s*\(\s*\)\s*,\s*config\.$field\.length\s*\(\s*\)\s*\)")
    Assert-Contract ($copy.Success -and $copy.Index -lt $queueSend.Index) `
        "P1-04: producer checks the full $field copy from config before queue publication"
}
$portCopy = [regex]::Match($enqueue, '\bjob->smtpPort\s*=\s*config\.smtpPort\s*;')
Assert-Contract ($portCopy.Success -and $portCopy.Index -lt $queueSend.Index) `
    'P1-04: producer copies smtpPort from config before queue publication'
foreach ($field in @('subject', 'body')) {
    $copy = [regex]::Match($enqueue, "\bcopyEmailString\s*\(\s*job->$field\s*,\s*$field\s*,\s*$field\s*\?\s*strlen\s*\(\s*$field\s*\)\s*:\s*0\s*\)")
    Assert-Contract ($copy.Success -and $copy.Index -lt $queueSend.Index) `
        "Producer checks the full $field copy before queue publication"
}
Assert-Contract ($blocks['copyEmailString'] -cmatch
    '(?s)\bsource\s*!=\s*nullptr\s*&&\s*target\.reserve\s*\(\s*length\s*\)\s*&&\s*target\.concat\s*\(\s*source\s*,\s*length\s*\)\s*&&\s*target\.length\s*\(\s*\)\s*==\s*length') `
    'Snapshot copy checks null input, reserve, concat and final byte length without a temporary String'
$copyFailure = [regex]::Match($enqueue,
    '(?s)if\s*\(\s*!copied\s*\)\s*\{[^{}]*\bdelete\s+job\s*;\s*stats\.pushFailed\+\+\s*;\s*return\s+false\s*;')
Assert-Contract ($copyFailure.Success -and $copyFailure.Index -lt $queueSend.Index) `
    'Incomplete snapshot is deleted and counted once before queue publication'
Assert-Contract ($blocks['sendEmailNotification'] -cmatch
    'if\s*\(\s*!enqueueEmail\s*\(\s*subject\s*,\s*body\s*\)\s*\)\s*\{\s*saveStats\s*\(\s*\)\s*;') `
    'Public producer persists enqueue failure statistics'

Assert-Contract ($blocks['ensureEmailWorker'] -cmatch
    '\bxQueueCreate\s*\(\s*EMAIL_QUEUE_LENGTH\s*,\s*sizeof\s*\(\s*EmailJob\s*\*\s*\)\s*\)') `
    'Email queue elements remain EmailJob pointers'
Assert-Contract ($enqueue -cmatch '\bEmailJob\s*\*\s*job\s*=\s*new\s*\(\s*std::nothrow\s*\)\s*EmailJob\s*\(\s*\)\s*;') `
    'Producer allocates an independently owned EmailJob with non-throwing allocation'
Assert-Contract ($enqueue -cmatch '(?s)\bxQueueSend\b[^{}]*\{[^{}]*\bdelete\s+job\s*;') `
    'Producer releases the job when queue publication fails'
Assert-Contract ($worker -cmatch '\bxQueueReceive\s*\(\s*emailQueue\s*,\s*&job\s*,\s*portMAX_DELAY\s*\)') `
    'Worker receives the job pointer'
Assert-Contract ($worker -cmatch '(?s)\bsendEmailNotificationNow\s*\(\s*\*job\s*\)\s*;.*\bdelete\s+job\s*;') `
    'Worker retains job ownership through sending and then deletes it'
Assert-Contract ($worker -cmatch '\bsendEmailNotificationNow\s*\(\s*\*job\s*\)\s*;\s*saveStats\s*\(\s*\)\s*;\s*delete\s+job\s*;\s*job\s*=\s*nullptr\s*;') `
    'Worker persists every send result and releases/clears the pointer before the next receive'
foreach ($terminator in @(';', '\{')) {
    Assert-Contract ($code -cmatch ('\bstatic\s+void\s+sendEmailNotificationNow\s*\(\s*const\s+EmailJob\s*&\s*job\s*\)\s*' + $terminator)) `
        'Internal sender declaration/definition takes a const EmailJob reference'
}
foreach ($name in @('emailWorkerTask', 'sendEmailNotificationNow', 'waitForSmtpProgress', 'ensureEmailWorker')) {
    Assert-Contract ($blocks[$name] -cnotmatch '\bconfig\b') "P1-04: $name never accesses global config"
}
foreach ($field in $smtpStrings) {
    $trim = [regex]::Match($enqueue, "\bjob->$field\.trim\s*\(\s*\)\s*;")
    Assert-Contract ($trim.Success -and $trim.Index -gt $copyFailure.Index -and $trim.Index -lt $queueSend.Index) `
        "Producer normalizes owned $field only after copy validation and before publication"
    Assert-Contract ($send -cmatch "\bconst\s+String\s*&\s*$field\s*=\s*job\.$field\s*;") `
        "Sender references const snapshot $field without copying or trimming it"
}
Assert-Contract ($send -cnotmatch '\bString\s+\w+\s*=\s*(?:job\.|smtpSendTo\b)|\.substring\s*\(') `
    'Sender does not copy whole snapshot fields or allocate recipient substrings'
Assert-Contract ($send -cmatch '\bconst\s+int\s+smtpPort\s*=\s*job\.smtpPort\s*>\s*0\s*\?\s*job\.smtpPort\s*:\s*465\s*;') `
    'SMTP port is read from the snapshot with the existing 465 fallback'
Assert-Contract ($send -cmatch '\bsmtp\.connect\s*\(\s*smtpServer\s*,\s*smtpPort\s*,' -and
    $send -cmatch '\bsmtp\.authenticate\s*\(\s*smtpUser\s*,\s*smtpPass\s*,') `
    'ReadyMail connect/authenticate receive validated String references, not implicit temporary copies'
Assert-Contract ($send -cmatch '\bmsg\.headers\.add\s*\(\s*rfc822_subject\s*,\s*job\.subject\s*\)') `
    'Message subject is passed directly as the owned snapshot String'
Assert-Contract ($send -cmatch '\bmsg\.text\.body\s*\(\s*job\.body\.c_str\s*\(\s*\)\s*,\s*job\.body\.length\s*\(\s*\)\s*\)') `
    'ReadyMail static body overload borrows the job buffer through async completion'
Assert-Contract ($send -cnotmatch '\bmsg\.headers\.add\s*\([^;]*\.c_str\s*\(') `
    'Header calls never reconstruct a temporary String from a checked buffer'
Assert-Contract ($send -cmatch '(?s)if\s*\([^{}]*smtpServer\.length[^{}]*\)\s*\{[^{}]*stats\.pushFailed\+\+\s*;\s*return\s*;') `
    'Incomplete SMTP configuration is counted as a failed send'
Assert-Contract ($send -cmatch '(?s)if\s*\(\s*!headersOk\s*\)\s*\{[^{}]*msg\.clear\s*\(\s*\)\s*;\s*smtp\.stop\s*\(\s*\)\s*;\s*ssl_client\.stop\s*\(\s*\)\s*;\s*clearOperationDiagnostic\s*\(\s*\)\s*;\s*stats\.pushFailed\+\+\s*;\s*return\s*;') `
    'Address header OOM clears the message, closes SMTP/TLS, clears diagnostics and counts failure'
Assert-Contract ($blocks['buildEmailAddressHeader'] -cmatch
    '(?s)header\.reserve\s*\(\s*expectedLength\s*\)\s*&&.*header\.concat.*header\.length\s*\(\s*\)\s*==\s*expectedLength') `
    'Address header builder checks reserve, concatenations and final byte length'

# Guard the pre-existing SMTP design while changing only where its inputs come from.
Assert-Contract ($send -cmatch '\bbool\s+useSsl\s*=\s*\(\s*smtpPort\s*!=\s*25\s*\)\s*;') `
    'SMTP preserves TLS except for port 25'
Assert-Contract ($send -cmatch 'if\s*\(\s*smtpPort\s*==\s*587\s*\)\s*\{\s*ssl_client\.setPlainStart\s*\(\s*\)\s*;\s*smtp\.setStartTLS') `
    'Port 587 retains STARTTLS setup'
Assert-Contract ($send -cmatch 'else\s+if\s*\(\s*smtpPort\s*==\s*25\s*\)\s*\{\s*ssl_client\.setPlainStart\s*\(\s*\)\s*;\s*smtp\.setStartTLS\s*\(\s*NULL\s*,\s*false\s*\)') `
    'Port 25 retains plain SMTP setup'
Assert-Contract ($send -cmatch '\bSmtpLockGuard\s+lock\s*\(\s*smtpMutex\s*,\s*pdMS_TO_TICKS\s*\(\s*35000\s*\)\s*\)') `
    'SMTP serialization retains the existing RAII lock'
Assert-Contract ($send -cmatch '\bSMTPMessage\s*&\s*msg\s*=\s*smtp\.getMessage\s*\(\s*\)\s*;\s*msg\.clear\s*\(\s*\)\s*;') `
    'ReadyMail retains its internal SMTPMessage reference and initial clear'
Assert-Contract ($send -cmatch '\bmsg\.clear\s*\(\s*\)\s*;\s*smtp\.stop\s*\(\s*\)\s*;') `
    'ReadyMail message is cleared before final connection cleanup'

Write-Output "All $script:passed offline push contracts passed. Arduino compilation is still required."
