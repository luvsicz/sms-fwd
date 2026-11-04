# Offline browser regression for web_pages.h (PowerShell 7 + an already installed Chrome/Edge).
# Run: pwsh -NoProfile -File tests/test_history_dom.ps1
# No downloads, server, firmware or hardware. localStorage simulates persisted /history JSON,
# not SPIFFS. Only temporary HTML/browser profiles are written, then removed.
[CmdletBinding()]
param(
    [string]$SourcePath = (Join-Path $PSScriptRoot '../code/web_pages.h'),
    [string]$BrowserPath,
    [string]$HistoryFixturePath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$source = Get-Content -LiteralPath $SourcePath -Raw -Encoding UTF8
$style = [regex]::Match($source, '(?s)<style>.*?</style>').Value
$commonScript = [regex]::Match($source, '(?s)<script>(.*?)</script>').Groups[1].Value
$historyHtml = [regex]::Match($source,
    '(?s)<!-- ============ 历史页 ============ -->.*?(?=<!-- ============ 配置页 ============ -->)').Value
$historyScript = [regex]::Match($source,
    '(?s)// 短信历史数据和当前选中的联系人.*?(?=\r?\nfunction saveFilter\()').Value
foreach ($part in @($style, $commonScript, $historyHtml, $historyScript)) {
    if ([string]::IsNullOrWhiteSpace($part)) { throw 'Cannot extract the actual history UI from web_pages.h' }
}
if ($historyScript -match '\b(innerHTML|outerHTML|insertAdjacentHTML)\b|\.onclick\b|\bonclick\s*=') {
    throw 'FAIL: history rendering must not use HTML sinks or inline onclick handlers'
}

if (-not $BrowserPath) {
    $candidates = @(
        "$env:ProgramFiles\Google\Chrome\Application\chrome.exe",
        "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
        "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe",
        "$env:LOCALAPPDATA\Google\Chrome\Application\chrome.exe",
        "$env:LOCALAPPDATA\Microsoft\Edge\Application\msedge.exe"
    )
    $BrowserPath = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $BrowserPath -or -not (Test-Path -LiteralPath $BrowserPath)) {
    throw 'An installed Chrome/Edge is required; supply -BrowserPath. No dependencies will be installed.'
}

$setup = @'
const result = document.getElementById('history-test-result');
const errors = [], frames = [];
let rejectFetch = false, failHttp = false, fetchCount = 0, frameCount = 0, checks = 0, failures = 0;
const reports = [];
window.__historyInjected = 0;
window.addEventListener('error', event => errors.push(event.message));
console.error = (...args) => errors.push(args.join(' '));
document.querySelector('.page').classList.add('active');
// The real renderer runs against a real DOM. Only I/O and animation scheduling are replaced.
window.fetch = url => {
  fetchCount++;
  if (url !== '/history') throw new Error('Unexpected request: ' + url);
  if (rejectFetch) return Promise.reject(new Error('offline failure'));
  return Promise.resolve({ok: !failHttp, json: () => Promise.resolve(JSON.parse(localStorage.getItem('history-fixture')))});
};
window.requestAnimationFrame = callback => { frameCount++; frames.push(callback); return frameCount; };
function persist(data) { localStorage.setItem('history-fixture', JSON.stringify(data)); }
function flushFrames() { while (frames.length) frames.shift()(0); }
async function settle() {
  for (let i = 0; i < 12; i++) await Promise.resolve();
  flushFrames();
}
function assert(condition, message) {
  checks++;
  if (!condition) throw new Error(message);
}
function equal(actual, expected, message) {
  assert(JSON.stringify(actual) === JSON.stringify(expected), message + ': ' + JSON.stringify(actual));
}
function all(selector, root = document) { return Array.from(root.querySelectorAll(selector)); }
function texts(selector, root = document) { return all(selector, root).map(node => node.textContent); }
function contacts() { return all('#contactList .contact-item'); }
function clickContact(name) {
  const item = contacts().find(node => node.dataset.name === name);
  assert(!!item, 'contact exists: ' + name);
  // Click a child to exercise event bubbling and the captured, unescaped contact name.
  item.querySelector('.contact-name').click();
  flushFrames();
  equal(curContact, name, 'clicked contact identity');
  equal(contacts().filter(node => node.classList.contains('active')).map(node => node.dataset.name),
    [name], 'only clicked contact is active');
}
function safeTree() {
  for (const id of ['contactList', 'chatTitle', 'chatMessages']) {
    const nodes = all('*', document.getElementById(id));
    assert(nodes.every(node => ['div', 'span', 'svg', 'path'].includes(node.localName)), id + ': no injected elements');
    assert(nodes.every(node => Array.from(node.attributes).every(attr => !/^on/i.test(attr.name))),
      id + ': no inline event attributes');
  }
  assert(all('.contact-name,.contact-preview,.contact-time,.contact-avatar,.chat-day span,.msg-time,.msg-bubble > div,#chatName')
    .every(node => node.childElementCount === 0), 'untrusted fields remain text-only leaves');
  equal(window.__historyInjected, 0, 'no injected code ran');
}
async function refresh() {
  const before = fetchCount;
  loadHist();
  equal(document.getElementById('contactList').textContent, '加载中...', 'contact loading state');
  equal(texts('#chatMessages .chat-empty > div'), ['\uD83D\uDCAC', '加载中...'], 'chat loading state');
  await settle();
  equal(fetchCount, before + 1, 'one mocked history fetch');
}
async function test(name, body) {
  try { await body(); reports.push('PASS: ' + name); }
  catch (error) { failures++; reports.push('FAIL: ' + name + ' -- ' + error.message); }
}
'@

$tests = @'
(async () => {
  const mobile = window.innerWidth <= 600;
  const normal = [
    {s:'10086', t:'2026-10-31 14:25:00', m:'最新通知'},
    {s:'10010', t:'2026-10-30 08:01:00', m:'另一号码'},
    {s:'10086', t:'2026-10-29 09:05:00', m:'旧消息'},
    {s:'10086', t:'2026-10-31 10:10:00', m:'同日通知'}
  ];
  const name = `"><img src=x onerror="window.__historyInjected++">'\\中文\uD83D\uDE00`;
  const body = '<b>预览</b> &<> "\'\\\n第二行\r\n\uD83D\uDE00 café e\u0301\u2028末行\n'
    + '<img src=x onerror="window.__historyInjected++">\n'
    + '<svg onload="window.__historyInjected++"></svg>\n'
    + '<script>window.__historyInjected++</' + 'script>\n'
    + '&lt;b&gt;literal &amp; entity&lt;/b&gt;';
  const badTime = '<b>day</b> <svg>';
  const injected = [
    {s:name, t:badTime, m:body},
    {s:"');window.__historyInjected++;//\\\"", t:'2026-10-30 08:01:00', m:'<i>x</i>'},
    {s:'<b', t:'2026-10-29 09:05:00', m:'引号 "\' 与反斜杠 \\'},
    {s:'&amp;', t:'2026-10-28 07:04:00', m:'实体 &amp; 不应解码'}
  ];

  await test('normal SMS: grouping, sorting, preview, counts and selection', async () => {
    equal(new URLSearchParams(location.search).get('view'), mobile ? 'mobile' : 'desktop', 'actual viewport');
    persist({smsHistory:normal});
    await refresh();
    equal(contacts().map(node => node.dataset.name), ['10086', '10010'], 'latest contact first');
    equal(texts('.contact-preview'), ['最新通知', '另一号码'], 'contact previews');
    equal(texts('.contact-time'), ['10-31 14:25', '10-30 08:01'], 'short contact times');
    equal(texts('.contact-badge'), ['3'], 'badge only for multiple records');
    equal($('chatName').textContent, '10086', 'first contact selected automatically');
    equal($('chatCount').textContent, '3条短信', 'SMS count');
    equal(texts('.msg-bubble > div'), ['旧消息', '同日通知', '最新通知'], 'oldest message first');
    equal(texts('.chat-day span'), ['2026-10-29', '2026-10-31'], 'one divider per day');
    equal(texts('.msg-time'), ['09:05', '10:10', '14:25'], 'message times');
    equal($('chatHeader').style.display, 'flex', 'header layout');
    equal($('chatCount').style.marginLeft, '36px', 'count alignment');
    const avatar = $('chatTitle').querySelector('.contact-avatar');
    equal([avatar.style.width, avatar.style.height, avatar.style.fontSize], ['28px', '28px', '0.8em'], 'title avatar styling');
    assert(!!avatar.style.background, 'avatar palette retained');
    clickContact('10010');
    equal($('chatName').textContent, '10010', 'name changes on contact click');
    equal($('chatCount').textContent, '1条短信', 'single SMS count');
    safeTree();
  });

  await test('responsive contact navigation and reusable back listener', async () => {
    for (let i = 0; i < 2; i++) {
      clickContact('10086');
      equal($s('.chat-area').classList.contains('show'), mobile, 'mobile chat view only');
      if (mobile) {
        equal($s('.contact-list').style.display, 'none', 'mobile list hidden');
        const back = $('chatTitle').querySelector('.chat-back');
        equal(back.querySelector('svg').getAttribute('viewBox'), '0 0 24 24', 'back icon preserved');
        back.click();
        assert(!$s('.chat-area').classList.contains('show'), 'back closes mobile chat');
        await new Promise(resolve => setTimeout(resolve, 350));
        equal($s('.contact-list').style.display, 'block', 'list restored after transition');
      }
    }
    safeTree();
  });

  await test('HTML, handlers, quotes, Unicode and hostile timestamps are literal text', async () => {
    assert([name, body].every(value => Array.from(value).some(ch => ch.codePointAt(0) === 0x1F600)),
      'non-BMP fixtures retain U+1F600');
    persist({smsHistory:injected});
    await refresh();
    for (const row of injected) {
      const item = contacts().find(node => node.dataset.name === row.s);
      assert(!!item, 'raw data-name is preserved');
      equal(item.querySelector('.contact-name').textContent, row.s, 'contact name is literal');
      equal(item.querySelector('.contact-preview').textContent,
        row.m.substring(0,20) + (row.m.length > 20 ? '...' : ''), 'preview is literal and truncated as before');
      equal(item.querySelector('.contact-time').textContent, row.t.substring(5,16), 'contact time is literal');
      equal(item.querySelector('.contact-avatar').textContent, row.s.length > 2 ? row.s.substring(0,1) : row.s,
        'avatar is literal, including short names');
      clickContact(row.s);
      equal($('chatName').textContent, row.s, 'title name is literal');
      equal(texts('.msg-bubble > div'), [row.m], 'body preserved exactly');
      equal(texts('.chat-day span'), [row.t.substring(0,10)], 'day is literal');
      equal(texts('.msg-time'), [row.t.substring(11,16)], 'message time is literal');
      equal(getComputedStyle($s('.msg-bubble > div')).whiteSpace, 'pre-wrap', 'line breaks preserved without HTML');
      safeTree();
    }
  });

  await test('persisted JSON can be loaded again without double decoding or execution', async () => {
    const stored = localStorage.getItem('history-fixture');
    smsData = []; callData = []; curContact = null;
    $('contactList').textContent = ''; $('chatMessages').textContent = '';
    await refresh();
    equal(localStorage.getItem('history-fixture'), stored, 'rendering does not rewrite persisted payload');
    equal(contacts().length, injected.length, 'all persisted contacts reloaded');
    clickContact(name);
    equal(texts('.msg-bubble > div'), [body], 'persisted body stays literal after reload');
    safeTree();
  });

  await test('prototype-like contact keys stay separate and leave Object.prototype intact', async () => {
    const keys = ['__proto__', 'constructor', 'prototype', 'toString', 'hasOwnProperty'];
    const before = Object.getOwnPropertyDescriptors(Object.prototype);
    const rows = keys.map((s, i) => ({s, t:'2026-10-31 10:0' + i + ':00', m:'text for ' + s}));
    rows.push({s:'__proto__', t:'2026-10-30 08:00:00', m:'older proto message'});
    persist({smsHistory:rows});
    await refresh();
    equal(contacts().length, keys.length, 'every special key gets its own bucket');
    for (const key of keys) {
      clickContact(key);
      equal($('chatName').textContent, key, 'special key can be selected');
      equal(all('.msg-bubble').length, key === '__proto__' ? 2 : 1, 'special-key bucket count');
    }
    const after = Object.getOwnPropertyDescriptors(Object.prototype);
    equal(Object.keys(after), Object.keys(before), 'prototype property names unchanged');
    assert(Object.keys(before).every(key => ['value','get','set','writable','enumerable','configurable']
      .every(field => before[key][field] === after[key][field])), 'prototype descriptors unchanged');
    safeTree();
  });

  await test('call history shares safe rendering, special keys and mode switching', async () => {
    persist({smsHistory:normal, callHistory:[
      {n:name, t:badTime},
      {n:'__proto__', t:'2026-10-31 09:01:00'},
      {n:'__proto__', t:'2026-10-30 08:02:00'},
      {n:'constructor', t:'2026-10-29 07:03:00'}
    ]});
    setHistMode('call');
    equal($('chatHeader').style.display, 'none', 'mode switch hides old header while loading');
    await settle();
    assert($('histModeCall').classList.contains('b-ok') && $('histModeSms').classList.contains('b-wait'), 'call tab badges');
    clickContact('__proto__');
    equal($('chatCount').textContent, '2次来电', 'call count');
    equal(contacts().find(node => node.dataset.name === '__proto__').querySelector('.contact-preview').textContent,
      '来电 2 次', 'call preview');
    equal(texts('.msg-bubble > div'), [' 来电记录', ' 来电记录'], 'call bubbles need no SMS body');
    equal(texts('.msg-time'), ['08:02', '09:01'], 'calls sorted oldest first');
    clickContact(name);
    equal($('chatName').textContent, name, 'call name is literal');
    equal(texts('.chat-day span'), [badTime.substring(0,10)], 'call day is literal');
    equal(texts('.msg-time'), [badTime.substring(11,16)], 'call time is literal');
    safeTree();
    setHistMode('sms');
    await settle();
    assert($('histModeSms').classList.contains('b-ok') && $('histModeCall').classList.contains('b-wait'), 'SMS tab badges');
    equal($('chatCount').textContent, '3条短信', 'switching back restores SMS data');
  });

  await test('legacy history response, multiline text and scroll-to-bottom', async () => {
    const longBody = body + ('\n长短信换行测试 ' + '文字 '.repeat(20)).repeat(100);
    persist({history:[{s:name, t:'2026-10-31 12:34:56', m:longBody}]});
    const before = frameCount;
    await refresh();
    equal(texts('.msg-bubble > div'), [longBody], 'legacy history body including all newlines');
    assert(frameCount > before, 'scroll scheduled with requestAnimationFrame');
    const box = $('chatMessages');
    assert(box.scrollHeight > box.clientHeight && box.clientHeight > 0, 'real browser has overflowing chat content');
    assert(Math.abs(box.scrollTop - (box.scrollHeight - box.clientHeight)) <= 1, 'scrolled to bottom');
    safeTree();
  });

  await test('empty, missing-contact, failed-load and recovery states', async () => {
    persist({});
    await refresh();
    equal($('contactList').textContent, '暂无短信', 'empty SMS list');
    equal(texts('.chat-empty > div'), ['\uD83D\uDCED', '暂无短信记录'], 'empty SMS chat');
    setHistMode('call');
    await settle();
    equal($('contactList').textContent, '暂无来电', 'empty call list');
    equal(texts('.chat-empty > div'), ['\uD83D\uDCED', '暂无来电记录'], 'empty call chat');
    selContact('missing');
    equal($('chatMessages').textContent, '无消息', 'unknown contact state');
    equal(errors.length, 0, 'no unexpected rendering or script errors');
    rejectFetch = true;
    await refresh();
    equal($('contactList').textContent, '加载失败', 'failed fetch state');
    equal(getComputedStyle($('contactList').firstElementChild).color, 'rgb(185, 28, 28)', 'error styling');
    equal(errors.length, 1, 'expected fetch error logged once');
    rejectFetch = false;
    persist({smsHistory:normal});
    setHistMode('sms');
    await settle();
    equal($('chatName').textContent, '10086', 'refresh recovers after a failed request');
    safeTree();
  });

  await test('HTTP failure is not shown as empty history', async () => {
    failHttp=true; await refresh(); failHttp=false;
    equal($('contactList').textContent,'加载失败','HTTP 503 is an error');
    equal(texts('.chat-empty > div'),['⚠','加载失败'],'chat loading state is cleared');
  });

  if(window.productionHistory)await test('actual firmware persistence and history endpoint to safe DOM',async()=>{
    persist(window.productionHistory);histMode='sms';await refresh();clickContact('10086');
    const expected=window.productionHistory.smsHistory.filter(m=>m.s==='10086').map(m=>m.m);
    equal(texts('.msg-bubble > div'),expected,'entire persisted body survives JSON and DOM');
    assert(expected[0].includes('123456')&&expected[0].includes('https://example.test/tail'),'OTP and URL after byte 200 retained');
    assert(!expected[0].includes('\ufffd'),'UTF8 not cut into replacement characters');safeTree();
    await refresh();clickContact('10086');equal(texts('.msg-bubble > div'),expected,'reload keeps text only');safeTree();
  });

  result.dataset.status = failures ? 'failed' : 'passed';
  result.textContent = reports.join('\n') + '\n' + (failures ? 'FAIL' : 'PASS')
    + ': ' + checks + ' assertions, ' + reports.length + ' scenarios, ' + (mobile ? 'mobile' : 'desktop');
})().catch(error => {
  result.dataset.status = 'failed';
  result.textContent = 'FAIL: test harness -- ' + error.stack;
});
'@

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ('msg-forward-history-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $tempRoot
try {
    $productionSetup = 'window.productionHistory=null;'
    if ($HistoryFixturePath) {
        $strictUtf8 = [Text.UTF8Encoding]::new($false, $true)
        $fixture = $strictUtf8.GetString([IO.File]::ReadAllBytes($HistoryFixturePath))
        $document = [System.Text.Json.JsonDocument]::Parse($fixture); $document.Dispose()
        $productionSetup = 'window.productionHistory=' + $fixture.Replace('<', '\u003c').Replace('>', '\u003e').Replace('&', '\u0026') + ';'
    }
    $html = '<!doctype html><html><head><meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">' +
        '<meta http-equiv="Content-Security-Policy" content="default-src ''none''; script-src ''unsafe-inline''; style-src ''unsafe-inline''; connect-src ''none''">' +
        $style + '</head><body><pre id="history-test-result" data-status="pending">PENDING</pre><div class="c">' +
        $historyHtml + '</div><script>' + $productionSetup + $setup + '</script><script>' + $commonScript + '</script><script>' +
        $historyScript + '</script><script>' + $tests + '</script></body></html>'
    $htmlPath = Join-Path $tempRoot 'history-test.html'
    [IO.File]::WriteAllText($htmlPath, $html, [Text.UTF8Encoding]::new($false))
    Write-Output 'PASS: no HTML sinks or inline onclick in the history renderer'
    foreach ($view in @('desktop', 'mobile')) {
        $size = if ($view -eq 'mobile') { '500,900' } else { '1200,900' }
        $start = [Diagnostics.ProcessStartInfo]::new($BrowserPath)
        $start.UseShellExecute = $false
        $start.RedirectStandardOutput = $true
        $start.RedirectStandardError = $true
        $start.StandardOutputEncoding = [Text.Encoding]::UTF8
        $start.StandardErrorEncoding = [Text.Encoding]::UTF8
        foreach ($argument in @(
            '--headless=new', '--disable-gpu', '--no-first-run', '--no-default-browser-check',
            '--disable-background-networking', '--disable-component-update', '--disable-sync',
            '--disable-extensions', '--disable-domain-reliability', '--metrics-recording-only',
            '--safebrowsing-disable-auto-update', '--host-resolver-rules=MAP * ~NOTFOUND',
            '--disable-features=MediaRouter,OptimizationHints', '--force-device-scale-factor=1',
            '--allow-file-access-from-files', '--dump-dom', '--virtual-time-budget=5000',
            "--window-size=$size", "--user-data-dir=$(Join-Path $tempRoot $view)",
            (([uri]$htmlPath).AbsoluteUri + '?view=' + $view)
        )) { $start.ArgumentList.Add($argument) }
        $process = [Diagnostics.Process]::new()
        $process.StartInfo = $start
        try {
            $null = $process.Start()
            $stdout = $process.StandardOutput.ReadToEndAsync()
            $stderr = $process.StandardError.ReadToEndAsync()
            if (-not $process.WaitForExit(45000)) {
                $process.Kill($true)
                $process.WaitForExit()
                throw "Browser timed out for $view"
            }
            $output = $stdout.GetAwaiter().GetResult()
            $diagnostics = $stderr.GetAwaiter().GetResult()
            $match = [regex]::Match($output, '(?s)<pre id="history-test-result" data-status="([^"]+)">(.*?)</pre>')
            if (-not $match.Success) { throw "No browser test result for ${view}: $diagnostics" }
            $report = [Net.WebUtility]::HtmlDecode($match.Groups[2].Value)
            Write-Output "[$view]`n$report"
            if ($process.ExitCode -ne 0 -or $match.Groups[1].Value -ne 'passed') {
                throw "History DOM regression failed for $view (browser exit $($process.ExitCode))"
            }
        } finally {
            $process.Dispose()
        }
    }
    Write-Output 'All offline history DOM tests passed. Firmware compilation/SPIFFS hardware validation is not covered.'
} finally {
    # Browser child processes may briefly retain profile files after the headless parent exits.
    for ($attempt = 0; $attempt -lt 10; $attempt++) {
        try { Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction Stop; break }
        catch {
            if ($attempt -eq 9) { Write-Warning "Could not remove temporary test directory: $tempRoot" }
            else { Start-Sleep -Milliseconds 200 }
        }
    }
}
