# Offline only: actual C++ root/save functions -> temporary HTTP fixtures -> installed browser.
# No downloads, network, devices, real passwords or persistent NVS. PowerShell 7 required.
[CmdletBinding()]
param(
    [string]$CompilerPath = $env:SMS_TEST_CXX,
    [string]$SourceDirectory = (Join-Path $PSScriptRoot '../code'),
    [string]$BrowserPath,
    [switch]$Sanitize
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $BrowserPath) {
    $BrowserPath = @(
        "$env:ProgramFiles\Google\Chrome\Application\chrome.exe",
        "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
        "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe",
        "$env:LOCALAPPDATA\Google\Chrome\Application\chrome.exe",
        "$env:LOCALAPPDATA\Microsoft\Edge\Application\msedge.exe"
    ) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $BrowserPath -or -not (Test-Path -LiteralPath $BrowserPath)) {
    throw 'Installed Chrome/Edge required; use -BrowserPath. No download is performed.'
}
$root = Join-Path ([IO.Path]::GetTempPath()) ('msg-forward-web-regression-' + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $root
$utf8 = [Text.UTF8Encoding]::new($false)

$setup = @'
window.__webInjected=0;
window.__webErrors=[];
window.__unexpectedRequests=[];
window.addEventListener('error',event=>__webErrors.push(event.message));
window.addEventListener('unhandledrejection',event=>__webErrors.push(String(event.reason)));
window.__requests=[];
window.__responses=[];
window.__confirms=[];
window.confirm=message=>{__confirms.push(message);return false};
window.fetch=(url,options)=>{
  if(!options){
    if(url==='/stats')return Promise.resolve({ok:true,status:200,json:async()=>({received:0,sent:0,calls:0,boots:1,uptime:1,freeHeap:90000,wifiRssi:-45})});
    if(url==='/query?type=network'||url==='/query?type=signal')return Promise.resolve({ok:true,status:200,json:async()=>({success:true,message:'offline fixture'})});
    if(url==='/query?type=siminfo')return Promise.resolve({ok:true,status:200,json:async()=>({success:true})});
  }
  __requests.push({url,options});
  const response=__responses.shift();
  if(!response){__unexpectedRequests.push(String(url));return Promise.reject(new Error('No synthetic response'))}
  if(response.networkError)return Promise.reject(new Error(response.networkError));
  return Promise.resolve({status:response.status,ok:response.status>=200&&response.status<300,
    json:async()=>{if(response.invalidJson)throw new Error('invalid JSON');return response.body}});
};
'@

$tests = @'
(async()=>{
  const reports=[];let checks=0,failures=0;
  const params=new URLSearchParams(location.search);
  const mobile=params.get('view')==='mobile',ap=params.get('mode')==='ap';
  const $all=(selector,root=document)=>Array.from(root.querySelectorAll(selector));
  function assert(ok,message){checks++;if(!ok)throw new Error(message)}
  // Never print values: password fields are synthetic but treated as sensitive.
  function equal(actual,expected,message){assert(JSON.stringify(actual)===JSON.stringify(expected),message)}
  async function test(name,run){try{await run();reports.push('PASS: '+name)}catch(e){failures++;reports.push('FAIL: '+name+' -- '+e.message)}}
  async function settle(){for(let i=0;i<30;i++)await Promise.resolve()}
  function normalized(text){return text.replace(/\r\n?/g,'\n')}
  function noInjected(){
    equal(window.__webInjected,0,'no injected code ran');
    equal(document.querySelectorAll('img,iframe,object,embed').length,0,'no injected HTML nodes');
    equal(document.querySelectorAll('[autofocus],[onfocus],[onerror],[data-probe]').length,0,'no injected attributes');
    equal(window.__webErrors,[],'no page syntax or runtime errors');
  }
  await settle();
  await test('actual persisted root: attribute and textarea context, credentials not reinterpreted',async()=>{
    equal(window.innerWidth<=600,mobile,'real viewport matches scenario');
    const expected=fixtures.expected.hostile;
    const names=['webUser','webPass','smtpServer','smtpUser','smtpPass','smtpSendTo','mqttServer','mqttUser','mqttPass','mqttPrefix','mqttHaPrefix'];
    for(let i=0;i<3;i++)names.push('wifi'+i+'ssid','push'+i+'name','push'+i+'url','push'+i+'k1','push'+i+'k2');
    names.forEach(name=>{
      const node=document.querySelector('[name="'+name+'"]');assert(!!node,'original field exists');
      equal(node.getAttribute('value'),expected,'HTML attribute recovers exact text once');
      equal(node.value,expected.replace(/[\r\n]/g,''),'single-line inputs keep browser newline semantics');
    });
    for(let i=0;i<3;i++){
      const wifiPass=document.querySelector('[name="wifi'+i+'pass"]');
      equal(wifiPass.value,'','WiFi password is still blank');
      assert(wifiPass.placeholder.includes('留空则保留'),'saved-password hint remains');
      const body=document.querySelector('[name="push'+i+'body"]');
      equal(body.value,normalized(i===0?fixtures.expected.body:expected),'textarea closing tags, first newline and entities are literal');
    }
    equal(document.querySelectorAll('.nav-item').length,4,'four tabs remain');
    equal(document.querySelectorAll('.page').length,4,'four pages remain');
    equal(document.querySelectorAll('#cf details').length,7,'configuration sections unchanged');
    noInjected();
  });
  await test('inline JS round trip: quotes, CR/LF, slash, Unicode and script/comment closure',async()=>{
    equal(ft.ls,fixtures.expected.hostile,'filter JS data exact');
    equal(cf.ls,fixtures.expected.hostile,'content JS data exact');
    equal(tm.ph,fixtures.expected.timerPhone,'timer phone JS data exact');
    equal(tm.ms,fixtures.expected.timerMessage,'timer body JS data exact');
    equal($('ftList').value,normalized(ft.ls),'filter textarea preserves line breaks');
    equal($('cfList').value,normalized(cf.ls),'content textarea preserves line breaks');
    equal($('tmMsg').localName,'textarea','timer message supports multiline without layout relocation');
    equal($('tmMsg').value,normalized(tm.ms),'timer body does not lose newlines');
    equal($('tmMsg').rows,1,'timer body keeps original single-row footprint');
    const init=Array.from(document.scripts).find(script=>script.textContent.includes('var ft={'));
    assert(!!init,'initialization script survived hostile data');
    assert(!init.textContent.split('// 状态初始化')[0].toLowerCase().includes('</script'),'no raw script closure in initial data');
    noInjected();
  });
  await test('SSID and MQTT text retains static line layout in STA/AP',async()=>{
    equal($('wifiInfo').querySelectorAll('br').length,ap?2:1,'static WiFi line breaks retained');
    assert($('wifiInfo').textContent.includes(fixtures.expected.hostile),'active SSID rendered literally');
    equal($('mqTopics').querySelectorAll('br').length,2,'MQTT keeps three lines');
    equal($('mqTopics').textContent,fixtures.expected.hostile.repeat(3),'MQTT topics are text');
    noInjected();
  });
  await test('dynamic timer phone uses text leaves and original grid styling',async()=>{
    $('tmEn').value='true';$('tmType').value='1';$('tmInt').value='50';
    $('tmPh').value=fixtures.expected.hostile;
    updTmInfo();
    const grid=$('tmInfo').children[1],values=$all('b',grid);
    equal(grid.style.display,'grid','timer grid remains');
    equal(grid.style.gridTemplateColumns,'1fr 1fr','two columns remain');
    equal(values.length,4,'SMS mode includes phone');
    equal(values[3].textContent,$('tmPh').value,'phone rendered exactly as text');
    assert(values.every(node=>node.childElementCount===0),'dynamic timer leaves contain no elements');
    $('tmType').value='0';updTmInfo();equal($all('#tmInfo b').length,3,'ping mode has three fields');
    $('tmEn').value='false';updTmInfo();equal($('tmInfo').textContent,'⏸ 定时任务已禁用','disabled state');
    noInjected();
  });
  await test('legacy query tables rebuilt as text cells, not parsed HTML',async()=>{
    const payload=fixtures.expected.hostile;
    queryMessage($('modNet'),"<table class='info-table'><tr><td>运营商</td><td>"+payload+"</td></tr></table>");
    equal($('modNet').querySelector('table').className,'info-table','existing query table CSS retained');
    equal($all('td',$('modNet')).map(node=>node.textContent),['运营商',payload],'query cells are literal');
    queryMessage($('modSig'),payload);equal($('modSig').textContent,payload,'non-table message is literal');
    assert(!$('modSig').children.length,'plain message creates no nodes');
    noInjected();
  });

  const actions={timer:()=>saveTimer(),filter:()=>saveFilter(),content:()=>saveContentFilter(),all:()=>saveAll({preventDefault(){},target:$('cf')})};
  const urls={timer:'/timer',filter:'/filter',content:'/contentfilter',all:'/save'};
  async function invoke(name,response){
    __responses.push(response);const count=__requests.length;
    actions[name]();await settle();
    equal(__requests.length,count+1,'one local mocked POST only');
    equal(__requests.at(-1).url,urls[name],'correct save endpoint');
    equal(__responses.length,0,'response consumed');
    return $('toast').textContent;
  }
  await test('real backend 400/503 reaches UI without success or timer/restart effects',async()=>{
    for(const name of Object.keys(actions))for(const code of [400,503]){
      const response=fixtures[name+'-'+code];tm.rm=123;const count=__confirms.length;
      equal(await invoke(name,response),response.body.message,'exact backend failure message displayed');
      equal(tm.rm,123,'failed timer cannot replace countdown');
      equal(__confirms.length,count,'failed save cannot request restart');
    }
    noInjected();
  });
  await test('both HTTP and strict success required; errors never interpreted as HTML',async()=>{
    const message='Rejected '+fixtures.expected.hostile;
    for(const name of Object.keys(actions)){
      for(const response of [
        {status:400,body:{success:true,message,needRestart:true,remain:999}},
        {status:200,body:{success:false,message,needRestart:true,remain:999}},
        {status:200,body:{success:'true',message,needRestart:true,remain:999}},
        {status:200,body:{message,needRestart:true,remain:999}}
      ]){
        tm.rm=321;const confirms=__confirms.length;
        equal(await invoke(name,response),message,'server error text shown rather than saved message');
        equal(tm.rm,321,'non-success preserves countdown');equal(__confirms.length,confirms,'non-success skips restart');
        equal($('toast').childElementCount,0,'toast is a text leaf');
      }
    }
    noInjected();
  });
  await test('valid real success uses message, timer remain and full-save restart prompt',async()=>{
    for(const name of Object.keys(actions)){
      const response=fixtures[name+'-200'],before=__confirms.length;
      equal(await invoke(name,response),response.body.message,'success displays actual backend message');
      if(name==='timer')equal(tm.rm,response.body.remain,'successful timer updates remaining period');
      equal(__confirms.length,before+(name==='all'&&response.body.needRestart?1:0),'restart prompt only after true full save');
    }
    noInjected();
  });
  await test('postJ never calls success on failure and Ping exits its pending state',async()=>{
    let success=0,failure=0;
    __responses.push({status:400,body:{success:false,message:'backend rejected'}});
    await postJ('/probe',{},()=>success++,message=>{failure++;equal(message,'backend rejected','error callback retains message')});
    equal(success,0,'failure skips success callback');equal(failure,1,'error callback runs exactly once');
    const message='Ping failed '+fixtures.expected.hostile;
    __responses.push({status:503,body:{success:false,message}});act('ping');await settle();
    equal($('pLog').textContent,message,'Ping does not remain pending on error');
    equal($('pLog').style.color,'rgb(185, 28, 28)','Ping failure style preserved');
    equal($('pLog').childElementCount,0,'Ping error remains text');
    noInjected();
  });
  await test('invalid JSON and network errors handled without invoking success callbacks',async()=>{
    for(const name of Object.keys(actions)){
      tm.rm=456;
      assert((await invoke(name,{status:503,invalidJson:true})).includes('503'),'invalid JSON retains HTTP status');
      equal(await invoke(name,{networkError:'offline failure'}),'offline failure','network failure shown');
      equal(tm.rm,456,'transport error does not update timer');
    }
    equal(__unexpectedRequests,[],'no unmocked hardware/network requests');
    noInjected();
  });
  const result=document.createElement('pre');result.id='web-test-result';
  result.dataset.status=failures?'failed':'passed';
  result.textContent=reports.join('\n')+'\n'+checks+' assertions, '+reports.length+' scenarios, '+failures+' failures';
  document.body.appendChild(result);
})().catch(error=>{
  const result=document.createElement('pre');result.id='web-test-result';result.dataset.status='failed';
  result.textContent='FAIL: harness -- '+error.message;document.body.appendChild(result);
});
'@

try {
    & (Join-Path $PSScriptRoot 'test_json_handlers.ps1') -CompilerPath $CompilerPath -SourceDirectory $SourceDirectory `
        -TestName web_regression -FixtureDirectory $root -Sanitize:$Sanitize
    $data = [ordered]@{ expected = (Get-Content -LiteralPath (Join-Path $root 'expected.json') -Raw -Encoding UTF8 | ConvertFrom-Json) }
    foreach ($name in @('timer','filter','content','all')) {
        foreach ($code in @(200,400,503)) {
            $key = "$name-$code"
            $data[$key] = Get-Content -LiteralPath (Join-Path $root "$key.json") -Raw -Encoding UTF8 | ConvertFrom-Json
        }
    }
    $fixtureJson = $data | ConvertTo-Json -Depth 20 -Compress -EscapeHandling EscapeHtml
    # This escaping is only for the test harness; production output is used without rewriting.
    $fixtureJson = $fixtureJson.Replace([string][char]0x2028, '\u2028').Replace([string][char]0x2029, '\u2029')
    foreach ($mode in @('sta','ap')) {
        $name = if ($mode -eq 'ap') { 'page-ap.html' } else { 'page.html' }
        $page = Get-Content -LiteralPath (Join-Path $root $name) -Raw -Encoding UTF8
        $csp = '<meta http-equiv="Content-Security-Policy" content="default-src ''none''; script-src ''unsafe-inline''; style-src ''unsafe-inline''; connect-src ''none''">'
        $page = $page.Replace('<head>', '<head>' + $csp + '<script>' + $setup + '</script>')
        $page = $page.Replace('</body>', '<script>const fixtures=' + $fixtureJson + ';' + $tests + '</script></body>')
        $path = Join-Path $root ("browser-$mode.html")
        [IO.File]::WriteAllText($path, $page, $utf8)
        foreach ($view in @('desktop','mobile')) {
            $size = if ($view -eq 'mobile') { '500,900' } else { '1200,900' }
            $start = [Diagnostics.ProcessStartInfo]::new($BrowserPath)
            $start.UseShellExecute = $false
            $start.RedirectStandardOutput = $true
            $start.RedirectStandardError = $true
            $start.StandardOutputEncoding = [Text.Encoding]::UTF8
            $start.StandardErrorEncoding = [Text.Encoding]::UTF8
            foreach ($arg in @('--headless=new','--disable-gpu','--no-first-run','--no-default-browser-check',
                '--disable-background-networking','--disable-component-update','--disable-sync','--disable-extensions',
                '--disable-domain-reliability','--metrics-recording-only','--safebrowsing-disable-auto-update',
                '--host-resolver-rules=MAP * ~NOTFOUND','--disable-features=MediaRouter,OptimizationHints',
                '--force-device-scale-factor=1','--allow-file-access-from-files','--dump-dom','--virtual-time-budget=5000',
                "--window-size=$size","--user-data-dir=$(Join-Path $root "$mode-$view")",
                (([uri]$path).AbsoluteUri + "?view=$view&mode=$mode"))) { $start.ArgumentList.Add($arg) }
            $process = [Diagnostics.Process]::new(); $process.StartInfo = $start
            try {
                $null = $process.Start()
                $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
                if (-not $process.WaitForExit(45000)) { $process.Kill($true); $process.WaitForExit(); throw "Browser timeout: $mode/$view" }
                $output = $stdout.GetAwaiter().GetResult(); $null = $stderr.GetAwaiter().GetResult()
                $match = [regex]::Match($output, '(?s)<pre id="web-test-result" data-status="([^"]+)">(.*?)</pre>')
                if (-not $match.Success) { throw "No test result: $mode/$view (full HTML is not logged)" }
                Write-Output "[$mode/$view]`n$([Net.WebUtility]::HtmlDecode($match.Groups[2].Value))"
                if ($process.ExitCode -ne 0 -or $match.Groups[1].Value -ne 'passed') { throw "Web regression failed: $mode/$view" }
            } finally { $process.Dispose() }
        }
    }
    Write-Output 'PASS: actual root HTML + backend save replies in a real offline DOM. No hardware/network was accessed.'
} finally {
    for ($attempt = 0; $attempt -lt 10; $attempt++) {
        try { Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction Stop; break }
        catch { if ($attempt -eq 9) { Write-Warning "Temporary test directory retained: $root" } else { Start-Sleep -Milliseconds 200 } }
    }
}
