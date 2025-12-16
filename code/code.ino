#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <Preferences.h>
#include <pdulib.h>
#define ENABLE_SMTP
#define ENABLE_DEBUG
#include <ReadyMail.h>
#include <HTTPClient.h>
#include <base64.h>      // Base64编码

//wifi信息，需要你打开这个去改
#include "wifi_config.h"

// MQTT配置（可选功能）
#include "mqtt_config.h"
#ifdef ENABLE_MQTT
#include <PubSubClient.h>
#endif

//串口映射
#define TXD 3
#define RXD 4

// 推送通道类型
enum PushType {
  PUSH_TYPE_NONE = 0,      // 未启用
  PUSH_TYPE_POST_JSON = 1, // POST JSON格式 {"sender":"xxx","message":"xxx","timestamp":"xxx"}
  PUSH_TYPE_BARK = 2,      // Bark格式 POST {"title":"xxx","body":"xxx"}
  PUSH_TYPE_GET = 3,       // GET请求，参数放URL中
  PUSH_TYPE_CUSTOM = 4     // 自定义模板
};

// 最大推送通道数
#define MAX_PUSH_CHANNELS 5

// 推送通道配置（通用设计，支持多种推送方式）
struct PushChannel {
  bool enabled;           // 是否启用
  PushType type;          // 推送类型
  String name;            // 通道名称（用于显示）
  String url;             // 推送URL（webhook地址）
  String key1;            // 额外参数1（如：钉钉secret、pushplus token等）
  String key2;            // 额外参数2（备用）
  String customBody;      // 自定义请求体模板（使用 {sender} {message} {timestamp} 占位符）
};

// 配置参数结构体
struct Config {
  String smtpServer;
  int smtpPort;
  String smtpUser;
  String smtpPass;
  String smtpSendTo;
  String adminPhone;
  PushChannel pushChannels[MAX_PUSH_CHANNELS];  // 多推送通道
  String webUser;      // Web管理账号
  String webPass;      // Web管理密码
  // 定时任务配置
  bool timerEnabled;        // 是否启用定时任务
  int timerType;            // 0=Ping, 1=短信
  int timerInterval;        // 间隔时间（分钟）
  String timerPhone;        // 定时短信目标号码
  String timerMessage;      // 定时短信内容
};

// 默认Web管理账号密码
#define DEFAULT_WEB_USER "admin"
#define DEFAULT_WEB_PASS "admin123"

Config config;
Preferences preferences;
WiFiMulti WiFiMulti;
PDU pdu = PDU(4096);
WiFiClientSecure ssl_client;
SMTPClient smtp(ssl_client);
WebServer server(80);

bool configValid = false;  // 配置是否有效
unsigned long lastPrintTime = 0;  // 上次打印IP的时间

// 定时任务相关变量
unsigned long lastTimerExec = 0;  // 上次执行定时任务的时间
unsigned long timerIntervalMs = 0;  // 定时间隔（毫秒）

#define SERIAL_BUFFER_SIZE 500
#define MAX_PDU_LENGTH 300
char serialBuf[SERIAL_BUFFER_SIZE];
int serialBufLen = 0;

// 长短信合并相关定义
#define MAX_CONCAT_PARTS 10       // 最大支持的长短信分段数
#define CONCAT_TIMEOUT_MS 30000   // 长短信等待超时时间(毫秒)
#define MAX_CONCAT_MESSAGES 5     // 最多同时缓存的长短信组数

// 长短信分段结构
struct SmsPart {
  bool valid;           // 该分段是否有效
  String text;          // 分段内容
};

// 长短信缓存结构
struct ConcatSms {
  bool inUse;                           // 是否正在使用
  int refNumber;                        // 参考号
  String sender;                        // 发送者
  String timestamp;                     // 时间戳（使用第一个收到的分段的时间戳）
  int totalParts;                       // 总分段数
  int receivedParts;                    // 已收到的分段数
  unsigned long firstPartTime;          // 收到第一个分段的时间
  SmsPart parts[MAX_CONCAT_PARTS];      // 各分段内容
};

ConcatSms concatBuffer[MAX_CONCAT_MESSAGES];  // 长短信缓存

// ========== MQTT相关变量和函数声明 ==========
#ifdef ENABLE_MQTT
WiFiClient mqttWifiClient;
PubSubClient mqttClient(mqttWifiClient);

String mqttDeviceId = "";  // 设备唯一ID（基于MAC地址）
String mqttTopicStatus = "";      // 设备状态主题
String mqttTopicSmsReceived = ""; // 收到短信通知主题
String mqttTopicSmsSent = "";     // 发送短信结果主题
String mqttTopicPingResult = "";  // Ping结果主题
String mqttTopicSmsSend = "";     // 发送短信命令订阅主题
String mqttTopicPing = "";        // Ping命令订阅主题
String mqttTopicCmd = "";         // 控制命令订阅主题

unsigned long lastMqttReconnectAttempt = 0;
const unsigned long MQTT_RECONNECT_INTERVAL = 5000;  // MQTT重连间隔（毫秒）

// MQTT回调函数声明
void mqttCallback(char* topic, byte* payload, unsigned int length);
void mqttReconnect();
void initMqttTopics();
String getMacSuffix();
void publishMqttSmsReceived(const char* sender, const char* message, const char* timestamp);
void publishMqttSmsSent(const char* phone, const char* message, bool success);
void publishMqttPingResult(const char* host, bool success, const char* result);
void publishMqttStatus(const char* status);
#endif


// 保存配置到NVS
void saveConfig() {
  preferences.begin("sms_config", false);
  preferences.putString("smtpServer", config.smtpServer);
  preferences.putInt("smtpPort", config.smtpPort);
  preferences.putString("smtpUser", config.smtpUser);
  preferences.putString("smtpPass", config.smtpPass);
  preferences.putString("smtpSendTo", config.smtpSendTo);
  preferences.putString("adminPhone", config.adminPhone);
  preferences.putString("webUser", config.webUser);
  preferences.putString("webPass", config.webPass);
  
  // 保存推送通道配置
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String prefix = "push" + String(i);
    preferences.putBool((prefix + "en").c_str(), config.pushChannels[i].enabled);
    preferences.putUChar((prefix + "type").c_str(), (uint8_t)config.pushChannels[i].type);
    preferences.putString((prefix + "url").c_str(), config.pushChannels[i].url);
    preferences.putString((prefix + "name").c_str(), config.pushChannels[i].name);
    preferences.putString((prefix + "k1").c_str(), config.pushChannels[i].key1);
    preferences.putString((prefix + "k2").c_str(), config.pushChannels[i].key2);
    preferences.putString((prefix + "body").c_str(), config.pushChannels[i].customBody);
  }
  
  // 保存定时任务配置
  preferences.putBool("timerEn", config.timerEnabled);
  preferences.putInt("timerType", config.timerType);
  preferences.putInt("timerInt", config.timerInterval);
  preferences.putString("timerPhone", config.timerPhone);
  preferences.putString("timerMsg", config.timerMessage);
  
  preferences.end();
  Serial.println("配置已保存");
}

// 从NVS加载配置
void loadConfig() {
  preferences.begin("sms_config", true);
  config.smtpServer = preferences.getString("smtpServer", "");
  config.smtpPort = preferences.getInt("smtpPort", 465);
  config.smtpUser = preferences.getString("smtpUser", "");
  config.smtpPass = preferences.getString("smtpPass", "");
  config.smtpSendTo = preferences.getString("smtpSendTo", "");
  config.adminPhone = preferences.getString("adminPhone", "");
  config.webUser = preferences.getString("webUser", DEFAULT_WEB_USER);
  config.webPass = preferences.getString("webPass", DEFAULT_WEB_PASS);
  
  // 加载推送通道配置
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String prefix = "push" + String(i);
    config.pushChannels[i].enabled = preferences.getBool((prefix + "en").c_str(), false);
    config.pushChannels[i].type = (PushType)preferences.getUChar((prefix + "type").c_str(), PUSH_TYPE_POST_JSON);
    config.pushChannels[i].url = preferences.getString((prefix + "url").c_str(), "");
    config.pushChannels[i].name = preferences.getString((prefix + "name").c_str(), "通道" + String(i + 1));
    config.pushChannels[i].key1 = preferences.getString((prefix + "k1").c_str(), "");
    config.pushChannels[i].key2 = preferences.getString((prefix + "k2").c_str(), "");
    config.pushChannels[i].customBody = preferences.getString((prefix + "body").c_str(), "");
  }
  
  // 兼容旧配置：如果有旧的httpUrl配置，迁移到第一个通道
  String oldHttpUrl = preferences.getString("httpUrl", "");
  if (oldHttpUrl.length() > 0 && !config.pushChannels[0].enabled) {
    config.pushChannels[0].enabled = true;
    config.pushChannels[0].url = oldHttpUrl;
    config.pushChannels[0].type = preferences.getUChar("barkMode", 0) != 0 ? PUSH_TYPE_BARK : PUSH_TYPE_POST_JSON;
    config.pushChannels[0].name = "迁移通道";
    Serial.println("已迁移旧HTTP配置到推送通道1");
  }
  
  // 加载定时任务配置
  config.timerEnabled = preferences.getBool("timerEn", false);
  config.timerType = preferences.getInt("timerType", 0);
  config.timerInterval = preferences.getInt("timerInt", 30);  // 默认30天
  config.timerPhone = preferences.getString("timerPhone", "");
  config.timerMessage = preferences.getString("timerMsg", "保号短信");
  
  // 更新定时间隔（天转毫秒）
  timerIntervalMs = (unsigned long)config.timerInterval * 24UL * 60UL * 60UL * 1000UL;
  
  preferences.end();
  Serial.println("配置已加载");
}

// 检查推送通道是否有效配置
bool isPushChannelValid(const PushChannel& ch) {
  if (!ch.enabled) return false;
  
  switch (ch.type) {
    case PUSH_TYPE_POST_JSON:
    case PUSH_TYPE_BARK:
    case PUSH_TYPE_GET:
    case PUSH_TYPE_CUSTOM:
      return ch.url.length() > 0;
    default:
      return false;
  }
}

// 检查配置是否有效（至少配置了邮件或任一推送通道）
bool isConfigValid() {
  bool emailValid = config.smtpServer.length() > 0 && 
                    config.smtpUser.length() > 0 && 
                    config.smtpPass.length() > 0 && 
                    config.smtpSendTo.length() > 0;
  
  bool pushValid = false;
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (isPushChannelValid(config.pushChannels[i])) {
      pushValid = true;
      break;
    }
  }
  
  return emailValid || pushValid;
}

// 获取当前设备URL
String getDeviceUrl() {
  return "http://" + WiFi.localIP().toString() + "/";
}

// HTML配置页面（精简版）
const char* htmlPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>短信转发配置</title>
  <style>
    *{box-sizing:border-box}body{font-family:system-ui,-apple-system,sans-serif;margin:0;padding:15px;background:#fff}
    .c{max-width:600px;margin:0 auto;background:#fff;padding:20px;border-radius:8px;box-shadow:0 1px 3px rgba(0,0,0,.1)}
    h1{color:#333;text-align:center;margin:0 0 15px;font-size:1.4em}
    .nav{display:flex;gap:8px;margin-bottom:15px}.nav a{flex:1;text-align:center;padding:10px;background:#f5f5f5;border-radius:4px;text-decoration:none;color:#333}.nav a.on{background:#4CAF50;color:#fff}
    .st{padding:10px;background:#f9f9f9;border:1px solid #e0e0e0;border-radius:4px;margin-bottom:15px}
    .st b{display:block;margin-bottom:4px;color:#333}.mqtt-on{color:#4CAF50}.mqtt-off{color:#e53935}
    .s{border:1px solid #e5e5e5;padding:15px;margin-bottom:15px;border-radius:8px}
    .s-t{font-size:1.1em;font-weight:600;color:#333;margin-bottom:12px}
    .fg{margin-bottom:12px}label{display:block;margin-bottom:4px;font-weight:500;color:#555;font-size:.9em}
    input,select,textarea{width:100%;padding:10px;border:1px solid #ddd;border-radius:6px;font-size:.95em}
    textarea{resize:vertical;min-height:60px}
    button{width:100%;padding:12px;background:#4CAF50;color:#fff;border:none;border-radius:6px;cursor:pointer;font-size:1em;font-weight:500}button:hover{background:#43a047}
    .ch{border:1px solid #e0e0e0;padding:12px;margin-bottom:12px;border-radius:6px;background:#fafafa}
    .ch-h{display:flex;align-items:center}.ch-h input{width:auto;margin-right:8px}.ch-h label{margin:0}
    .ch-b{display:none;margin-top:10px}.ch.en .ch-b{display:block}
    .hint{font-size:.8em;color:#888;margin-top:4px;padding:6px;background:#f5f5f5;border-radius:4px}
    .warn{padding:8px;background:#fff3cd;border-left:3px solid #ffc107;margin-bottom:12px;font-size:.85em}
  </style>
</head>
<body>
  <div class="c">
    <h1>📱 短信转发器</h1>
    <div class="nav"><a href="/" class="on">⚙️ 配置</a><a href="/tools">🧰 工具</a></div>
    <div class="st">
      <b>设备IP: %IP%</b>
      <span>MQTT: <span class="%MQTT_CLASS%">%MQTT_STATUS%</span></span>
    </div>
    
    <form action="/save" method="POST">
      <div class="s">
        <div class="s-t">🔐 Web管理账号</div>
        <div class="warn">⚠️ 首次使用请修改默认密码！默认: admin / admin123</div>
        <div class="fg"><label>账号</label><input name="webUser" value="%WEB_USER%"></div>
        <div class="fg"><label>密码</label><input type="password" name="webPass" value="%WEB_PASS%"></div>
      </div>
      
      <div class="s">
        <div class="s-t">📧 邮件通知</div>
        <div class="fg"><label>SMTP服务器</label><input name="smtpServer" value="%SMTP_SERVER%" placeholder="smtp.qq.com"></div>
        <div class="fg"><label>端口</label><input type="number" name="smtpPort" value="%SMTP_PORT%" placeholder="465"></div>
        <div class="fg"><label>账号</label><input name="smtpUser" value="%SMTP_USER%"></div>
        <div class="fg"><label>密码/授权码</label><input type="password" name="smtpPass" value="%SMTP_PASS%"></div>
        <div class="fg"><label>接收邮箱</label><input name="smtpSendTo" value="%SMTP_SEND_TO%"></div>
      </div>
      
      <div class="s">
        <div class="s-t">🔗 HTTP推送通道</div>
        %PUSH_CHANNELS%
      </div>
      
      <div class="s">
        <div class="s-t">👤 管理员手机号</div>
        <div class="fg"><input name="adminPhone" value="%ADMIN_PHONE%" placeholder="13800138000"></div>
      </div>
      
      <button type="submit">💾 保存配置</button>
    </form>
  </div>
  <script>
    function tog(i){var c=document.getElementById('ch'+i),b=document.getElementById('en'+i);c.className=b.checked?'ch en':'ch'}
    function upd(i){var t=document.getElementById('tp'+i).value,h=document.getElementById('ht'+i),cf=document.getElementById('cf'+i);
      var m={1:'POST JSON: {"sender":"xxx","message":"xxx","timestamp":"xxx"}',2:'Bark: {"title":"xxx","body":"xxx"}',3:'GET: URL?sender=xxx&message=xxx',4:'自定义模板: 使用{sender}{message}{timestamp}占位符'};
      h.textContent=m[t]||'';cf.style.display=t==4?'block':'none'}
    document.addEventListener('DOMContentLoaded',function(){for(var i=0;i<5;i++){tog(i);upd(i)}})
  </script>
</body>
</html>
)rawliteral";

// HTML工具箱页面（精简版）
const char* htmlToolsPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>工具箱</title>
  <style>
    *{box-sizing:border-box}body{font-family:system-ui,-apple-system,sans-serif;margin:0;padding:15px;background:#fff}
    .c{max-width:600px;margin:0 auto;background:#fff;padding:20px;border-radius:8px;box-shadow:0 1px 3px rgba(0,0,0,.1)}
    h1{color:#333;text-align:center;margin:0 0 15px;font-size:1.4em}
    .nav{display:flex;gap:8px;margin-bottom:15px}.nav a{flex:1;text-align:center;padding:10px;background:#f5f5f5;border-radius:4px;text-decoration:none;color:#333}.nav a.on{background:#2196F3;color:#fff}
    .st{padding:10px;background:#f9f9f9;border:1px solid #e0e0e0;border-radius:4px;margin-bottom:15px}
    .st b{display:block;margin-bottom:4px;color:#333}.mqtt-on{color:#4CAF50}.mqtt-off{color:#e53935}
    .s{border:1px solid #e5e5e5;padding:15px;margin-bottom:15px;border-radius:8px}
    .s-t{font-size:1.1em;font-weight:600;color:#333;margin-bottom:12px}
    .fg{margin-bottom:12px}label{display:block;margin-bottom:4px;font-weight:500;color:#555;font-size:.9em}
    input,select,textarea{width:100%;padding:10px;border:1px solid #ddd;border-radius:6px;font-size:.95em}textarea{resize:vertical;min-height:80px}
    button{width:100%;padding:12px;background:#2196F3;color:#fff;border:none;border-radius:6px;cursor:pointer;font-size:1em;font-weight:500;margin-top:8px}button:hover{background:#1976D2}button:disabled{background:#ccc;cursor:not-allowed}
    .bg{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:8px}.bg button{flex:1;min-width:100px;margin:0}
    .bq{background:#9C27B0}.bq:hover{background:#7B1FA2}.bi{background:#607D8B}.bi:hover{background:#455A64}.bp{background:#FF9800}.bp:hover{background:#F57C00}
    .rb{margin-top:10px;padding:10px;border-radius:6px;display:none}.rs{background:#e8f5e9;border-left:3px solid #4CAF50;color:#2e7d32}.re{background:#ffebee;border-left:3px solid #f44336;color:#c62828}.rl{background:#fff3e0;border-left:3px solid #FF9800;color:#e65100}.ri{background:#e3f2fd;border-left:3px solid #2196F3;color:#1565c0}
    .it{width:100%;border-collapse:collapse;margin-top:8px}.it td{padding:5px 8px;border-bottom:1px solid #eee}.it td:first-child{font-weight:600;width:40%;color:#555}
    .hint{font-size:.8em;color:#888;margin-top:4px}
    .timer-box{background:#e8f5e9;padding:12px;border-radius:6px;margin-bottom:12px;text-align:center}
    .timer-off{background:#f5f5f5}.countdown{font-size:1.5em;font-weight:bold;color:#4CAF50}
    .sms-fields{display:none}
  </style>
</head>
<body>
  <div class="c">
    <h1>📱 短信转发器</h1>
    <div class="nav"><a href="/">⚙️ 配置</a><a href="/tools" class="on">🧰 工具</a></div>
    <div class="st">
      <b>设备IP: %IP%</b>
      <span>MQTT: <span class="%MQTT_CLASS%">%MQTT_STATUS%</span></span>
    </div>
    
    <form action="/sendsms" method="POST">
      <div class="s">
        <div class="s-t">📤 发送短信</div>
        <div class="fg"><label>目标号码</label><input name="phone" placeholder="13800138000" required></div>
        <div class="fg"><label>短信内容</label><textarea name="content" placeholder="请输入短信内容..." required oninput="document.getElementById('cc').textContent=this.value.length"></textarea><div class="hint">已输入 <span id="cc">0</span> 字符</div></div>
        <button type="submit">📨 发送短信</button>
      </div>
    </form>
    
    <div class="s">
      <div class="s-t">⏰ 定时任务</div>
      <div class="timer-box %TIMER_BOX_CLASS%" id="timerBox">
        <div id="timerStatus">%TIMER_STATUS%</div>
        <div class="countdown" id="countdown">%TIMER_COUNTDOWN%</div>
      </div>
      <div class="fg"><label><input type="checkbox" id="timerEn" %TIMER_CHECKED% style="width:auto;margin-right:6px">启用定时任务</label></div>
      <div class="fg"><label>任务类型</label>
        <select id="timerType" onchange="toggleSmsFields()">
          <option value="0" %TIMER_TYPE0%>定时Ping（消耗少量流量）</option>
          <option value="1" %TIMER_TYPE1%>定时发送短信</option>
        </select>
      </div>
      <div class="fg"><label>间隔时间（天）</label><input type="number" id="timerInt" value="%TIMER_INTERVAL%" min="1" max="365"></div>
      <div class="sms-fields" id="smsFields">
        <div class="fg"><label>目标号码</label><input id="timerPhone" value="%TIMER_PHONE%" placeholder="13800138000"></div>
        <div class="fg"><label>短信内容</label><input id="timerMsg" value="%TIMER_MSG%" placeholder="保号短信"></div>
      </div>
      <button style="background:#4CAF50" onclick="saveTimer()">💾 保存定时任务</button>
      <div class="rb" id="timerResult"></div>
    </div>
    
    <div class="s">
      <div class="s-t">📊 模组信息</div>
      <div class="bg"><button class="bq" onclick="q('ati')">📋 固件</button><button class="bq" onclick="q('signal')">📶 信号</button></div>
      <div class="bg"><button class="bi" onclick="q('siminfo')">💳 SIM卡</button><button class="bi" onclick="q('network')">🌍 网络</button><button class="bi" onclick="q('wifi')" style="background:#00BCD4">📡 WiFi</button></div>
      <div class="rb" id="qr"></div>
    </div>
    
    <div class="s">
      <div class="s-t">🌐 网络测试</div>
      <button class="bp" id="pb" onclick="p()">📡 Ping测试(消耗少量流量)</button>
      <div class="rb" id="pr"></div>
    </div>
  </div>
  <script>
    var timerRemain=%TIMER_REMAIN%;
    function updateCountdown(){
      if(timerRemain<=0){document.getElementById('countdown').textContent='--';return}
      timerRemain--;var d=Math.floor(timerRemain/86400),h=Math.floor((timerRemain%86400)/3600),m=Math.floor((timerRemain%3600)/60);
      document.getElementById('countdown').textContent=(d>0?d+'天':'')+(h>0?h+'时':'')+(m>0?m+'分':'')+'后执行';
      setTimeout(updateCountdown,1000)}
    function toggleSmsFields(){document.getElementById('smsFields').style.display=document.getElementById('timerType').value=='1'?'block':'none'}
    function saveTimer(){
      var r=document.getElementById('timerResult');r.className='rb rl';r.style.display='block';r.textContent='保存中...';
      var data={enabled:document.getElementById('timerEn').checked,type:parseInt(document.getElementById('timerType').value),
        interval:parseInt(document.getElementById('timerInt').value),phone:document.getElementById('timerPhone').value,
        message:document.getElementById('timerMsg').value};
      fetch('/timer',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)})
        .then(x=>x.json()).then(d=>{r.className='rb '+(d.success?'rs':'re');r.textContent=d.success?'✅ 保存成功':'❌ '+d.message;
          if(d.success){timerRemain=d.remain;updateCountdown();document.getElementById('timerBox').className='timer-box '+(data.enabled?'':'timer-off');
            document.getElementById('timerStatus').textContent=data.enabled?(data.type==0?'定时Ping':'定时短信'):'已禁用'}})
        .catch(e=>{r.className='rb re';r.textContent='❌ '+e})}
    function q(t){var r=document.getElementById('qr');r.className='rb rl';r.style.display='block';r.textContent='查询中...';
      fetch('/query?type='+t).then(x=>x.json()).then(d=>{r.className='rb '+(d.success?'ri':'re');r.innerHTML=d.success?d.message:'❌ '+d.message}).catch(e=>{r.className='rb re';r.textContent='❌ '+e})}
    function p(){var b=document.getElementById('pb'),r=document.getElementById('pr');b.disabled=true;b.textContent='⏳ Ping中...';r.className='rb rl';r.style.display='block';r.textContent='请稍候(最长30秒)...';
      fetch('/ping',{method:'POST'}).then(x=>x.json()).then(d=>{b.disabled=false;b.textContent='📡 Ping测试(消耗少量流量)';r.className='rb '+(d.success?'rs':'re');r.innerHTML=(d.success?'✅ ':'❌ ')+d.message}).catch(e=>{b.disabled=false;b.textContent='📡 Ping测试(消耗少量流量)';r.className='rb re';r.textContent='❌ '+e})}
    toggleSmsFields();updateCountdown();
  </script>
</body>
</html>
)rawliteral";

// 检查HTTP Basic认证
bool checkAuth() {
  if (!server.authenticate(config.webUser.c_str(), config.webPass.c_str())) {
    server.requestAuthentication(BASIC_AUTH, "SMS Forwarding", "请输入管理员账号密码");
    return false;
  }
  return true;
}

// 处理配置页面请求
void handleRoot() {
  if (!checkAuth()) return;
  
  String html = String(htmlPage);
  html.replace("%IP%", WiFi.localIP().toString());
  
  // MQTT状态
  #ifdef ENABLE_MQTT
  html.replace("%MQTT_STATUS%", mqttClient.connected() ? "已连接 ✓" : "未连接");
  html.replace("%MQTT_CLASS%", mqttClient.connected() ? "mqtt-on" : "mqtt-off");
  #else
  html.replace("%MQTT_STATUS%", "未启用");
  html.replace("%MQTT_CLASS%", "mqtt-off");
  #endif
  
  html.replace("%WEB_USER%", config.webUser);
  html.replace("%WEB_PASS%", config.webPass);
  html.replace("%SMTP_SERVER%", config.smtpServer);
  html.replace("%SMTP_PORT%", String(config.smtpPort));
  html.replace("%SMTP_USER%", config.smtpUser);
  html.replace("%SMTP_PASS%", config.smtpPass);
  html.replace("%SMTP_SEND_TO%", config.smtpSendTo);
  html.replace("%ADMIN_PHONE%", config.adminPhone);
  
  // 生成推送通道HTML（精简版）
  String channelsHtml = "";
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String idx = String(i);
    String enabledClass = config.pushChannels[i].enabled ? " en" : "";
    String checked = config.pushChannels[i].enabled ? " checked" : "";
    
    channelsHtml += "<div class=\"ch" + enabledClass + "\" id=\"ch" + idx + "\">";
    channelsHtml += "<div class=\"ch-h\"><input type=\"checkbox\" name=\"push" + idx + "en\" id=\"en" + idx + "\" onchange=\"tog(" + idx + ")\"" + checked + "><label>通道 " + String(i + 1) + "</label></div>";
    channelsHtml += "<div class=\"ch-b\">";
    
    // 通道名称
    channelsHtml += "<div class=\"fg\"><label>名称</label><input name=\"push" + idx + "name\" value=\"" + config.pushChannels[i].name + "\" placeholder=\"自定义名称\"></div>";
    
    // 推送类型（精简为4种）
    channelsHtml += "<div class=\"fg\"><label>推送方式</label>";
    channelsHtml += "<select name=\"push" + idx + "type\" id=\"tp" + idx + "\" onchange=\"upd(" + idx + ")\">";
    channelsHtml += "<option value=\"1\"" + String(config.pushChannels[i].type == PUSH_TYPE_POST_JSON ? " selected" : "") + ">POST JSON</option>";
    channelsHtml += "<option value=\"2\"" + String(config.pushChannels[i].type == PUSH_TYPE_BARK ? " selected" : "") + ">Bark</option>";
    channelsHtml += "<option value=\"3\"" + String(config.pushChannels[i].type == PUSH_TYPE_GET ? " selected" : "") + ">GET请求</option>";
    channelsHtml += "<option value=\"4\"" + String(config.pushChannels[i].type == PUSH_TYPE_CUSTOM ? " selected" : "") + ">自定义模板</option>";
    channelsHtml += "</select><div class=\"hint\" id=\"ht" + idx + "\"></div></div>";
    
    // URL
    channelsHtml += "<div class=\"fg\"><label>推送URL</label><input name=\"push" + idx + "url\" value=\"" + config.pushChannels[i].url + "\" placeholder=\"http://...\"></div>";
    
    // 自定义模板区域
    channelsHtml += "<div id=\"cf" + idx + "\" style=\"display:none\"><div class=\"fg\"><label>请求体模板</label><textarea name=\"push" + idx + "body\" rows=\"3\">" + config.pushChannels[i].customBody + "</textarea></div></div>";
    
    channelsHtml += "</div></div>";
  }
  html.replace("%PUSH_CHANNELS%", channelsHtml);
  
  server.send(200, "text/html", html);
}

// 处理工具箱页面请求
void handleToolsPage() {
  if (!checkAuth()) return;
  
  String html = String(htmlToolsPage);
  html.replace("%IP%", WiFi.localIP().toString());
  
  // MQTT状态
  #ifdef ENABLE_MQTT
  html.replace("%MQTT_STATUS%", mqttClient.connected() ? "已连接 ✓" : "未连接");
  html.replace("%MQTT_CLASS%", mqttClient.connected() ? "mqtt-on" : "mqtt-off");
  #else
  html.replace("%MQTT_STATUS%", "未启用");
  html.replace("%MQTT_CLASS%", "mqtt-off");
  #endif
  
  // 定时任务状态
  unsigned long remainMs = 0;
  if (config.timerEnabled && timerIntervalMs > 0) {
    unsigned long elapsed = millis() - lastTimerExec;
    if (elapsed < timerIntervalMs) {
      remainMs = timerIntervalMs - elapsed;
    }
  }
  int remainSec = remainMs / 1000;
  
  html.replace("%TIMER_BOX_CLASS%", config.timerEnabled ? "" : "timer-off");
  html.replace("%TIMER_STATUS%", config.timerEnabled ? (config.timerType == 0 ? "定时Ping" : "定时短信") : "已禁用");
  
  // 格式化剩余时间
  String countdown = "--";
  if (config.timerEnabled && remainSec > 0) {
    int d = remainSec / 86400;
    int h = (remainSec % 86400) / 3600;
    int m = (remainSec % 3600) / 60;
    countdown = "";
    if (d > 0) countdown += String(d) + "天";
    if (h > 0) countdown += String(h) + "时";
    if (m > 0) countdown += String(m) + "分";
    countdown += "后执行";
  }
  html.replace("%TIMER_COUNTDOWN%", countdown);
  html.replace("%TIMER_REMAIN%", String(remainSec));
  html.replace("%TIMER_CHECKED%", config.timerEnabled ? "checked" : "");
  html.replace("%TIMER_TYPE0%", config.timerType == 0 ? "selected" : "");
  html.replace("%TIMER_TYPE1%", config.timerType == 1 ? "selected" : "");
  html.replace("%TIMER_INTERVAL%", String(config.timerInterval));
  html.replace("%TIMER_PHONE%", config.timerPhone);
  html.replace("%TIMER_MSG%", config.timerMessage);
  
  server.send(200, "text/html", html);
}

// 发送AT命令并获取响应
String sendATCommand(const char* cmd, unsigned long timeout) {
  while (Serial1.available()) Serial1.read();
  Serial1.println(cmd);
  
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < timeout) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (resp.indexOf("OK") >= 0 || resp.indexOf("ERROR") >= 0) {
        delay(50);  // 等待剩余数据
        while (Serial1.available()) resp += (char)Serial1.read();
        return resp;
      }
    }
  }
  return resp;
}

// 处理模组信息查询请求
void handleQuery() {
  if (!checkAuth()) return;
  
  String type = server.arg("type");
  String json = "{";
  bool success = false;
  String message = "";
  
  if (type == "ati") {
    // 固件信息查询
    String resp = sendATCommand("ATI", 2000);
    Serial.println("ATI响应: " + resp);
    
    if (resp.indexOf("OK") >= 0) {
      success = true;
      // 解析ATI响应
      String manufacturer = "未知";
      String model = "未知";
      String version = "未知";
      
      // 按行解析
      int lineStart = 0;
      int lineNum = 0;
      for (int i = 0; i < resp.length(); i++) {
        if (resp.charAt(i) == '\n' || i == resp.length() - 1) {
          String line = resp.substring(lineStart, i);
          line.trim();
          if (line.length() > 0 && line != "ATI" && line != "OK") {
            lineNum++;
            if (lineNum == 1) manufacturer = line;
            else if (lineNum == 2) model = line;
            else if (lineNum == 3) version = line;
          }
          lineStart = i + 1;
        }
      }
      
      message = "<table class='info-table'>";
      message += "<tr><td>制造商</td><td>" + manufacturer + "</td></tr>";
      message += "<tr><td>模组型号</td><td>" + model + "</td></tr>";
      message += "<tr><td>固件版本</td><td>" + version + "</td></tr>";
      message += "</table>";
    } else {
      message = "查询失败";
    }
  }
  else if (type == "signal") {
    // 信号质量查询
    String resp = sendATCommand("AT+CESQ", 2000);
    Serial.println("CESQ响应: " + resp);
    
    if (resp.indexOf("+CESQ:") >= 0) {
      success = true;
      // 解析 +CESQ: <rxlev>,<ber>,<rscp>,<ecno>,<rsrq>,<rsrp>
      int idx = resp.indexOf("+CESQ:");
      String params = resp.substring(idx + 6);
      int endIdx = params.indexOf('\r');
      if (endIdx < 0) endIdx = params.indexOf('\n');
      if (endIdx > 0) params = params.substring(0, endIdx);
      params.trim();
      
      // 分割参数
      String values[6];
      int valIdx = 0;
      int startPos = 0;
      for (int i = 0; i <= params.length() && valIdx < 6; i++) {
        if (i == params.length() || params.charAt(i) == ',') {
          values[valIdx] = params.substring(startPos, i);
          values[valIdx].trim();
          valIdx++;
          startPos = i + 1;
        }
      }
      
      // RSRP转换为dBm (0-97映射到-140到-44 dBm, 99表示未知)
      int rsrp = values[5].toInt();
      String rsrpStr;
      if (rsrp == 99 || rsrp == 255) {
        rsrpStr = "未知";
      } else {
        int rsrpDbm = -140 + rsrp;
        rsrpStr = String(rsrpDbm) + " dBm";
        if (rsrpDbm >= -80) rsrpStr += " (信号极好)";
        else if (rsrpDbm >= -90) rsrpStr += " (信号良好)";
        else if (rsrpDbm >= -100) rsrpStr += " (信号一般)";
        else if (rsrpDbm >= -110) rsrpStr += " (信号较弱)";
        else rsrpStr += " (信号很差)";
      }
      
      // RSRQ转换 (0-34映射到-19.5到-3 dB)
      int rsrq = values[4].toInt();
      String rsrqStr;
      if (rsrq == 99 || rsrq == 255) {
        rsrqStr = "未知";
      } else {
        float rsrqDb = -19.5 + rsrq * 0.5;
        rsrqStr = String(rsrqDb, 1) + " dB";
      }
      
      message = "<table class='info-table'>";
      message += "<tr><td>信号强度 (RSRP)</td><td>" + rsrpStr + "</td></tr>";
      message += "<tr><td>信号质量 (RSRQ)</td><td>" + rsrqStr + "</td></tr>";
      message += "<tr><td>原始数据</td><td>" + params + "</td></tr>";
      message += "</table>";
    } else {
      message = "查询失败";
    }
  }
  else if (type == "siminfo") {
    // SIM卡信息查询
    success = true;
    message = "<table class='info-table'>";
    
    // 查询IMSI
    String resp = sendATCommand("AT+CIMI", 2000);
    String imsi = "未知";
    if (resp.indexOf("OK") >= 0) {
      int start = resp.indexOf('\n');
      if (start >= 0) {
        int end = resp.indexOf('\n', start + 1);
        if (end < 0) end = resp.indexOf('\r', start + 1);
        if (end > start) {
          imsi = resp.substring(start + 1, end);
          imsi.trim();
          if (imsi == "OK" || imsi.length() < 10) imsi = "未知";
        }
      }
    }
    message += "<tr><td>IMSI</td><td>" + imsi + "</td></tr>";
    
    // 查询ICCID
    resp = sendATCommand("AT+ICCID", 2000);
    String iccid = "未知";
    if (resp.indexOf("+ICCID:") >= 0) {
      int idx = resp.indexOf("+ICCID:");
      String tmp = resp.substring(idx + 7);
      int endIdx = tmp.indexOf('\r');
      if (endIdx < 0) endIdx = tmp.indexOf('\n');
      if (endIdx > 0) iccid = tmp.substring(0, endIdx);
      iccid.trim();
    }
    message += "<tr><td>ICCID</td><td>" + iccid + "</td></tr>";
    
    // 查询本机号码 (如果SIM卡支持)
    resp = sendATCommand("AT+CNUM", 2000);
    String phoneNum = "未存储或不支持";
    if (resp.indexOf("+CNUM:") >= 0) {
      int idx = resp.indexOf(",\"");
      if (idx >= 0) {
        int endIdx = resp.indexOf("\"", idx + 2);
        if (endIdx > idx) {
          phoneNum = resp.substring(idx + 2, endIdx);
        }
      }
    }
    message += "<tr><td>本机号码</td><td>" + phoneNum + "</td></tr>";
    
    message += "</table>";
  }
  else if (type == "network") {
    // 网络状态查询
    success = true;
    message = "<table class='info-table'>";
    
    // 查询网络注册状态
    String resp = sendATCommand("AT+CEREG?", 2000);
    String regStatus = "未知";
    if (resp.indexOf("+CEREG:") >= 0) {
      int idx = resp.indexOf("+CEREG:");
      String tmp = resp.substring(idx + 7);
      int commaIdx = tmp.indexOf(',');
      if (commaIdx >= 0) {
        String stat = tmp.substring(commaIdx + 1, commaIdx + 2);
        int s = stat.toInt();
        switch(s) {
          case 0: regStatus = "未注册，未搜索"; break;
          case 1: regStatus = "已注册，本地网络"; break;
          case 2: regStatus = "未注册，正在搜索"; break;
          case 3: regStatus = "注册被拒绝"; break;
          case 4: regStatus = "未知"; break;
          case 5: regStatus = "已注册，漫游"; break;
          default: regStatus = "状态码: " + stat;
        }
      }
    }
    message += "<tr><td>网络注册</td><td>" + regStatus + "</td></tr>";
    
    // 查询运营商
    resp = sendATCommand("AT+COPS?", 2000);
    String oper = "未知";
    if (resp.indexOf("+COPS:") >= 0) {
      int idx = resp.indexOf(",\"");
      if (idx >= 0) {
        int endIdx = resp.indexOf("\"", idx + 2);
        if (endIdx > idx) {
          oper = resp.substring(idx + 2, endIdx);
        }
      }
    }
    message += "<tr><td>运营商</td><td>" + oper + "</td></tr>";
    
    // 查询PDP上下文激活状态
    resp = sendATCommand("AT+CGACT?", 2000);
    String pdpStatus = "未激活";
    if (resp.indexOf("+CGACT: 1,1") >= 0) {
      pdpStatus = "已激活";
    } else if (resp.indexOf("+CGACT:") >= 0) {
      pdpStatus = "未激活";
    }
    message += "<tr><td>数据连接</td><td>" + pdpStatus + "</td></tr>";
    
    // 查询APN
    resp = sendATCommand("AT+CGDCONT?", 2000);
    String apn = "未知";
    if (resp.indexOf("+CGDCONT:") >= 0) {
      int idx = resp.indexOf(",\"");
      if (idx >= 0) {
        idx = resp.indexOf(",\"", idx + 2);  // 跳过PDP类型
        if (idx >= 0) {
          int endIdx = resp.indexOf("\"", idx + 2);
          if (endIdx > idx) {
            apn = resp.substring(idx + 2, endIdx);
            if (apn.length() == 0) apn = "(自动)";
          }
        }
      }
    }
    message += "<tr><td>APN</td><td>" + apn + "</td></tr>";
    
    message += "</table>";
  }
  else if (type == "wifi") {
    // WiFi状态查询
    success = true;
    message = "<table class='info-table'>";
    
    // WiFi连接状态
    String wifiStatus = WiFi.isConnected() ? "已连接" : "未连接";
    message += "<tr><td>连接状态</td><td>" + wifiStatus + "</td></tr>";
    
    // SSID
    String ssid = WiFi.SSID();
    if (ssid.length() == 0) ssid = "未知";
    message += "<tr><td>当前SSID</td><td>" + ssid + "</td></tr>";
    
    // 信号强度 RSSI
    int rssi = WiFi.RSSI();
    String rssiStr = String(rssi) + " dBm";
    if (rssi >= -50) rssiStr += " (信号极好)";
    else if (rssi >= -60) rssiStr += " (信号很好)";
    else if (rssi >= -70) rssiStr += " (信号良好)";
    else if (rssi >= -80) rssiStr += " (信号一般)";
    else if (rssi >= -90) rssiStr += " (信号较弱)";
    else rssiStr += " (信号很差)";
    message += "<tr><td>信号强度 (RSSI)</td><td>" + rssiStr + "</td></tr>";
    
    // IP地址
    message += "<tr><td>IP地址</td><td>" + WiFi.localIP().toString() + "</td></tr>";
    
    // 网关
    message += "<tr><td>网关</td><td>" + WiFi.gatewayIP().toString() + "</td></tr>";
    
    // 子网掩码
    message += "<tr><td>子网掩码</td><td>" + WiFi.subnetMask().toString() + "</td></tr>";
    
    // DNS
    message += "<tr><td>DNS服务器</td><td>" + WiFi.dnsIP().toString() + "</td></tr>";
    
    // MAC地址
    message += "<tr><td>MAC地址</td><td>" + WiFi.macAddress() + "</td></tr>";
    
    // BSSID (路由器MAC)
    message += "<tr><td>路由器BSSID</td><td>" + WiFi.BSSIDstr() + "</td></tr>";
    
    // 信道
    message += "<tr><td>WiFi信道</td><td>" + String(WiFi.channel()) + "</td></tr>";
    
    message += "</table>";
  }
  else {
    message = "未知的查询类型";
  }
  
  json += "\"success\":" + String(success ? "true" : "false") + ",";
  json += "\"message\":\"" + message + "\"";
  json += "}";
  
  server.send(200, "application/json", json);
}

// 前置声明
void sendEmailNotification(const char* subject, const char* body);
bool sendSMS(const char* phoneNumber, const char* message);

// 处理发送短信请求
void handleSendSms() {
  if (!checkAuth()) return;
  
  String phone = server.arg("phone");
  String content = server.arg("content");
  
  phone.trim();
  content.trim();
  
  bool success = false;
  String resultMsg = "";
  
  if (phone.length() == 0) {
    resultMsg = "错误：请输入目标号码";
  } else if (content.length() == 0) {
    resultMsg = "错误：请输入短信内容";
  } else {
    Serial.println("网页端发送短信请求");
    Serial.println("目标号码: " + phone);
    Serial.println("短信内容: " + content);
    
    success = sendSMS(phone.c_str(), content.c_str());
    resultMsg = success ? "短信发送成功！" : "短信发送失败，请检查模组状态";
  }
  
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta http-equiv="refresh" content="3;url=/sms">
  <title>发送结果</title>
  <style>
    body { font-family: Arial, sans-serif; text-align: center; padding-top: 100px; background: #f5f5f5; }
    .result { padding: 20px; border-radius: 10px; display: inline-block; }
    .success { background: #4CAF50; color: white; }
    .error { background: #f44336; color: white; }
  </style>
</head>
<body>
  <div class="result %CLASS%">
    <h2>%ICON% %MSG%</h2>
    <p>3秒后返回发送页面...</p>
  </div>
</body>
</html>
)rawliteral";
  
  html.replace("%CLASS%", success ? "success" : "error");
  html.replace("%ICON%", success ? "✅" : "❌");
  html.replace("%MSG%", resultMsg);
  
  server.send(200, "text/html", html);
}

// 处理Ping请求
void handlePing() {
  if (!checkAuth()) return;
  
  Serial.println("网页端发起Ping请求");
  
  // 清空串口缓冲区
  while (Serial1.available()) Serial1.read();
  
  // 先激活PDP上下文（数据连接）
  Serial.println("激活数据连接...");
  String activateResp = sendATCommand("AT+CGACT=1,1", 10000);
  Serial.println("CGACT响应: " + activateResp);
  
  // 检查激活是否成功（OK或已激活的情况）
  bool networkActivated = (activateResp.indexOf("OK") >= 0);
  if (!networkActivated) {
    Serial.println("数据连接激活失败，尝试继续执行...");
  }
  
  // 清空串口缓冲区
  while (Serial1.available()) Serial1.read();
  delay(500);  // 等待网络稳定
  
  // 发送MPING命令，ping 8.8.8.8，超时30秒，ping 1次
  Serial1.println("AT+MPING=\"8.8.8.8\",30,1");
  
  // 等待响应
  unsigned long start = millis();
  String resp = "";
  bool gotOK = false;
  bool gotError = false;
  bool gotPingResult = false;
  String pingResultMsg = "";
  
  // 等待最多35秒（30秒超时 + 5秒余量）
  while (millis() - start < 35000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      Serial.print(c);  // 调试输出
      
      // 检查是否收到OK
      if (resp.indexOf("OK") >= 0 && !gotOK) {
        gotOK = true;
      }
      
      // 检查是否收到ERROR
      if (resp.indexOf("+CME ERROR") >= 0 || resp.indexOf("ERROR") >= 0) {
        gotError = true;
        pingResultMsg = "模组返回错误";
        break;
      }
      
      // 检查是否收到Ping结果URC
      // 成功格式: +MPING: 1,8.8.8.8,32,xxx,xxx
      // 失败格式: +MPING: 2 或其他
      int mpingIdx = resp.indexOf("+MPING:");
      if (mpingIdx >= 0) {
        // 找到换行符确定完整的一行
        int lineEnd = resp.indexOf('\n', mpingIdx);
        if (lineEnd >= 0) {
          String mpingLine = resp.substring(mpingIdx, lineEnd);
          mpingLine.trim();
          Serial.println("收到MPING结果: " + mpingLine);
          
          // 解析结果
          // +MPING: <result>[,<ip>,<packet_len>,<time>,<ttl>]
          int colonIdx = mpingLine.indexOf(':');
          if (colonIdx >= 0) {
            String params = mpingLine.substring(colonIdx + 1);
            params.trim();
            
            // 获取第一个参数（result）
            int commaIdx = params.indexOf(',');
            String resultStr;
            if (commaIdx >= 0) {
              resultStr = params.substring(0, commaIdx);
            } else {
              resultStr = params;
            }
            resultStr.trim();
            int result = resultStr.toInt();
            
            gotPingResult = true;
            
            // result=0或1都表示成功（不同模组可能返回不同值）
            // 如果有完整的响应参数（IP、时间等），也视为成功
            bool pingSuccess = (result == 0 || result == 1) || (params.indexOf(',') >= 0 && params.length() > 5);
            
            if (pingSuccess) {
              // 成功，解析详细信息
              // 格式: 0/1,"8.8.8.8",16,时间,TTL
              int idx1 = params.indexOf(',');
              if (idx1 >= 0) {
                String rest = params.substring(idx1 + 1);
                // 处理IP地址（可能带引号）
                String ip;
                int idx2;
                if (rest.startsWith("\"")) {
                  // 带引号的IP
                  int quoteEnd = rest.indexOf('\"', 1);
                  if (quoteEnd >= 0) {
                    ip = rest.substring(1, quoteEnd);
                    idx2 = rest.indexOf(',', quoteEnd);
                  } else {
                    idx2 = rest.indexOf(',');
                    ip = rest.substring(0, idx2);
                  }
                } else {
                  idx2 = rest.indexOf(',');
                  ip = rest.substring(0, idx2);
                }
                
                if (idx2 >= 0) {
                  rest = rest.substring(idx2 + 1);
                  int idx3 = rest.indexOf(',');  // packet_len后
                  if (idx3 >= 0) {
                    rest = rest.substring(idx3 + 1);
                    int idx4 = rest.indexOf(',');  // time后
                    String timeStr, ttlStr;
                    if (idx4 >= 0) {
                      timeStr = rest.substring(0, idx4);
                      ttlStr = rest.substring(idx4 + 1);
                    } else {
                      timeStr = rest;
                      ttlStr = "N/A";
                    }
                    timeStr.trim();
                    ttlStr.trim();
                    pingResultMsg = "目标: " + ip + ", 延迟: " + timeStr + "ms, TTL: " + ttlStr;
                  }
                }
              }
              if (pingResultMsg.length() == 0) {
                pingResultMsg = "Ping成功";
              }
            } else {
              // 失败
              pingResultMsg = "Ping超时或目标不可达 (错误码: " + String(result) + ")";
            }
            break;
          }
        }
      }
    }
    
    if (gotError || gotPingResult) break;
    delay(10);
  }
  
  Serial.println("\nPing操作完成");
  
  // 关闭数据连接以节省流量
  Serial.println("关闭数据连接...");
  String deactivateResp = sendATCommand("AT+CGACT=0,1", 5000);
  Serial.println("CGACT关闭响应: " + deactivateResp);
  
  // 构建JSON响应
  String json = "{";
  if (gotPingResult && pingResultMsg.indexOf("延迟") >= 0) {
    json += "\"success\":true,";
    json += "\"message\":\"" + pingResultMsg + "\"";
  } else if (gotError) {
    json += "\"success\":false,";
    json += "\"message\":\"" + pingResultMsg + "\"";
  } else if (gotPingResult) {
    json += "\"success\":false,";
    json += "\"message\":\"" + pingResultMsg + "\"";
  } else {
    json += "\"success\":false,";
    json += "\"message\":\"操作超时，未收到Ping结果\"";
  }
  json += "}";
  
  server.send(200, "application/json", json);
}

// 处理定时任务配置保存
void handleTimer() {
  if (!checkAuth()) return;
  
  String body = server.arg("plain");
  Serial.println("收到定时任务配置: " + body);
  
  // 简单解析JSON
  bool enabled = body.indexOf("\"enabled\":true") >= 0;
  
  int typeIdx = body.indexOf("\"type\":");
  int timerType = 0;
  if (typeIdx >= 0) {
    timerType = body.substring(typeIdx + 7, typeIdx + 8).toInt();
  }
  
  int intervalIdx = body.indexOf("\"interval\":");
  int interval = 30;  // 默认30天
  if (intervalIdx >= 0) {
    int endIdx = body.indexOf(",", intervalIdx + 11);
    if (endIdx < 0) endIdx = body.indexOf("}", intervalIdx + 11);
    interval = body.substring(intervalIdx + 11, endIdx).toInt();
    if (interval < 1) interval = 1;
    if (interval > 365) interval = 365; // 最大365天
  }
  
  int phoneIdx = body.indexOf("\"phone\":\"");
  String phone = "";
  if (phoneIdx >= 0) {
    int endIdx = body.indexOf("\"", phoneIdx + 9);
    phone = body.substring(phoneIdx + 9, endIdx);
  }
  
  int msgIdx = body.indexOf("\"message\":\"");
  String message = "保号短信";
  if (msgIdx >= 0) {
    int endIdx = body.indexOf("\"", msgIdx + 11);
    message = body.substring(msgIdx + 11, endIdx);
  }
  
  // 更新配置
  config.timerEnabled = enabled;
  config.timerType = timerType;
  config.timerInterval = interval;
  config.timerPhone = phone;
  config.timerMessage = message;
  
  // 更新定时间隔（天转毫秒）
  timerIntervalMs = (unsigned long)interval * 24UL * 60UL * 60UL * 1000UL;
  
  // 重置执行时间
  lastTimerExec = millis();
  
  // 保存配置
  saveConfig();
  
  // 计算剩余时间
  int remainSec = timerIntervalMs / 1000;
  
  String json = "{\"success\":true,\"remain\":" + String(remainSec) + "}";
  server.send(200, "application/json", json);
  
  Serial.println("定时任务配置已保存: " + String(enabled ? "启用" : "禁用") + 
                 ", 类型: " + String(timerType) + 
                 ", 间隔: " + String(interval) + "分钟");
}

// 处理保存配置请求
void handleSave() {
  if (!checkAuth()) return;
  
  // 获取新的Web账号密码
  String newWebUser = server.arg("webUser");
  String newWebPass = server.arg("webPass");
  
  // 验证Web账号密码不能为空
  if (newWebUser.length() == 0) newWebUser = DEFAULT_WEB_USER;
  if (newWebPass.length() == 0) newWebPass = DEFAULT_WEB_PASS;
  
  config.webUser = newWebUser;
  config.webPass = newWebPass;
  config.smtpServer = server.arg("smtpServer");
  config.smtpPort = server.arg("smtpPort").toInt();
  if (config.smtpPort == 0) config.smtpPort = 465;
  config.smtpUser = server.arg("smtpUser");
  config.smtpPass = server.arg("smtpPass");
  config.smtpSendTo = server.arg("smtpSendTo");
  config.adminPhone = server.arg("adminPhone");
  
  // 保存推送通道配置
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    String idx = String(i);
    config.pushChannels[i].enabled = server.arg("push" + idx + "en") == "on";
    config.pushChannels[i].type = (PushType)server.arg("push" + idx + "type").toInt();
    config.pushChannels[i].url = server.arg("push" + idx + "url");
    config.pushChannels[i].name = server.arg("push" + idx + "name");
    config.pushChannels[i].key1 = server.arg("push" + idx + "key1");
    config.pushChannels[i].key2 = server.arg("push" + idx + "key2");
    config.pushChannels[i].customBody = server.arg("push" + idx + "body");
    if (config.pushChannels[i].name.length() == 0) {
      config.pushChannels[i].name = "通道" + String(i + 1);
    }
  }
  
  saveConfig();
  configValid = isConfigValid();
  
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta http-equiv="refresh" content="3;url=/">
  <title>保存成功</title>
  <style>
    body { font-family: Arial, sans-serif; text-align: center; padding-top: 100px; background: #f5f5f5; }
    .success { background: #4CAF50; color: white; padding: 20px; border-radius: 10px; display: inline-block; }
  </style>
</head>
<body>
  <div class="success">
    <h2>✅ 配置保存成功！</h2>
    <p>3秒后返回配置页面...</p>
    <p>如果修改了账号密码，请使用新的账号密码登录</p>
  </div>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
  
  // 如果配置有效，发送启动通知
  if (configValid) {
    Serial.println("配置有效，发送启动通知...");
    String subject = "短信转发器配置已更新";
    String body = "设备配置已更新\n设备地址: " + getDeviceUrl();
    sendEmailNotification(subject.c_str(), body.c_str());
  }
}

// 发送邮件通知函数
void sendEmailNotification(const char* subject, const char* body) {
  if (config.smtpServer.length() == 0 || config.smtpUser.length() == 0 || 
      config.smtpPass.length() == 0 || config.smtpSendTo.length() == 0) {
    Serial.println("邮件配置不完整，跳过发送");
    return;
  }
  
  auto statusCallback = [](SMTPStatus status) {
    Serial.println(status.text);
  };
  smtp.connect(config.smtpServer.c_str(), config.smtpPort, statusCallback);
  if (smtp.isConnected()) {
    smtp.authenticate(config.smtpUser.c_str(), config.smtpPass.c_str(), readymail_auth_password);

    SMTPMessage msg;
    String from = "sms notify <"; from += config.smtpUser; from += ">";
    msg.headers.add(rfc822_from, from.c_str());
    String to = "your_email <"; to += config.smtpSendTo; to += ">";
    msg.headers.add(rfc822_to, to.c_str());
    msg.headers.add(rfc822_subject, subject);
    msg.text.body(body);
    configTime(0, 0, "ntp.ntsc.ac.cn");
    while (time(nullptr) < 100000) delay(100);
    msg.timestamp = time(nullptr);
    smtp.send(msg);
    Serial.println("邮件发送完成");
  } else {
    Serial.println("邮件服务器连接失败");
  }
}

// 发送短信（PDU模式）
bool sendSMS(const char* phoneNumber, const char* message) {
  Serial.println("准备发送短信...");
  Serial.print("目标号码: "); Serial.println(phoneNumber);
  Serial.print("短信内容: "); Serial.println(message);

  // 使用pdulib编码PDU
  pdu.setSCAnumber();  // 使用默认短信中心
  int pduLen = pdu.encodePDU(phoneNumber, message);
  
  if (pduLen < 0) {
    Serial.print("PDU编码失败，错误码: ");
    Serial.println(pduLen);
    return false;
  }
  
  Serial.print("PDU数据: "); Serial.println(pdu.getSMS());
  Serial.print("PDU长度: "); Serial.println(pduLen);
  
  // 发送AT+CMGS命令
  String cmgsCmd = "AT+CMGS=";
  cmgsCmd += pduLen;
  
  while (Serial1.available()) Serial1.read();
  Serial1.println(cmgsCmd);
  
  // 等待 > 提示符
  unsigned long start = millis();
  bool gotPrompt = false;
  while (millis() - start < 5000) {
    if (Serial1.available()) {
      char c = Serial1.read();
      Serial.print(c);
      if (c == '>') {
        gotPrompt = true;
        break;
      }
    }
  }
  
  if (!gotPrompt) {
    Serial.println("未收到>提示符");
    return false;
  }
  
  // 发送PDU数据
  Serial1.print(pdu.getSMS());
  Serial1.write(0x1A);  // Ctrl+Z 结束
  
  // 等待响应
  start = millis();
  String resp = "";
  while (millis() - start < 30000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      Serial.print(c);
      if (resp.indexOf("OK") >= 0) {
        Serial.println("\n短信发送成功");
        return true;
      }
      if (resp.indexOf("ERROR") >= 0) {
        Serial.println("\n短信发送失败");
        return false;
      }
    }
  }
  Serial.println("短信发送超时");
  return false;
}

// 重启模组
void resetModule() {
  Serial.println("正在重启模组...");
  Serial1.println("AT+CFUN=1,1");
  delay(3000);
}

// 检查发送者是否为管理员
bool isAdmin(const char* sender) {
  if (config.adminPhone.length() == 0) return false;
  
  // 去除可能的国际区号前缀进行比较
  String senderStr = String(sender);
  String adminStr = config.adminPhone;
  
  // 去除+86前缀
  if (senderStr.startsWith("+86")) {
    senderStr = senderStr.substring(3);
  }
  if (adminStr.startsWith("+86")) {
    adminStr = adminStr.substring(3);
  }
  
  return senderStr.equals(adminStr);
}

// 处理管理员命令
void processAdminCommand(const char* sender, const char* text) {
  String cmd = String(text);
  cmd.trim();
  
  Serial.println("处理管理员命令: " + cmd);
  
  // 处理 SMS:号码:内容 命令
  if (cmd.startsWith("SMS:")) {
    int firstColon = cmd.indexOf(':');
    int secondColon = cmd.indexOf(':', firstColon + 1);
    
    if (secondColon > firstColon + 1) {
      String targetPhone = cmd.substring(firstColon + 1, secondColon);
      String smsContent = cmd.substring(secondColon + 1);
      
      targetPhone.trim();
      smsContent.trim();
      
      Serial.println("目标号码: " + targetPhone);
      Serial.println("短信内容: " + smsContent);
      
      bool success = sendSMS(targetPhone.c_str(), smsContent.c_str());
      
      // 发送邮件通知结果
      String subject = success ? "短信发送成功" : "短信发送失败";
      String body = "管理员命令执行结果:\n";
      body += "命令: " + cmd + "\n";
      body += "目标号码: " + targetPhone + "\n";
      body += "短信内容: " + smsContent + "\n";
      body += "执行结果: " + String(success ? "成功" : "失败");
      
      sendEmailNotification(subject.c_str(), body.c_str());
    } else {
      Serial.println("SMS命令格式错误");
      sendEmailNotification("命令执行失败", "SMS命令格式错误，正确格式: SMS:号码:内容");
    }
  }
  // 处理 RESET 命令
  else if (cmd.equals("RESET")) {
    Serial.println("执行RESET命令");
    
    // 先发送邮件通知（因为重启后就发不了了）
    sendEmailNotification("重启命令已执行", "收到RESET命令，即将重启模组和ESP32...");
    
    // 重启模组
    resetModule();
    
    // 重启ESP32
    Serial.println("正在重启ESP32...");
    delay(1000);
    ESP.restart();
  }
  else {
    Serial.println("未知命令: " + cmd);
  }
}

// 初始化长短信缓存
void initConcatBuffer() {
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    concatBuffer[i].inUse = false;
    concatBuffer[i].receivedParts = 0;
    for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
      concatBuffer[i].parts[j].valid = false;
      concatBuffer[i].parts[j].text = "";
    }
  }
}

// 查找或创建长短信缓存槽位
int findOrCreateConcatSlot(int refNumber, const char* sender, int totalParts) {
  // 先查找是否已存在
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].inUse && 
        concatBuffer[i].refNumber == refNumber &&
        concatBuffer[i].sender.equals(sender)) {
      return i;
    }
  }
  
  // 查找空闲槽位
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (!concatBuffer[i].inUse) {
      concatBuffer[i].inUse = true;
      concatBuffer[i].refNumber = refNumber;
      concatBuffer[i].sender = String(sender);
      concatBuffer[i].totalParts = totalParts;
      concatBuffer[i].receivedParts = 0;
      concatBuffer[i].firstPartTime = millis();
      for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
        concatBuffer[i].parts[j].valid = false;
        concatBuffer[i].parts[j].text = "";
      }
      return i;
    }
  }
  
  // 没有空闲槽位，查找最老的槽位覆盖
  int oldestSlot = 0;
  unsigned long oldestTime = concatBuffer[0].firstPartTime;
  for (int i = 1; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].firstPartTime < oldestTime) {
      oldestTime = concatBuffer[i].firstPartTime;
      oldestSlot = i;
    }
  }
  
  // 覆盖最老的槽位
  Serial.println("⚠️ 长短信缓存已满，覆盖最老的槽位");
  concatBuffer[oldestSlot].inUse = true;
  concatBuffer[oldestSlot].refNumber = refNumber;
  concatBuffer[oldestSlot].sender = String(sender);
  concatBuffer[oldestSlot].totalParts = totalParts;
  concatBuffer[oldestSlot].receivedParts = 0;
  concatBuffer[oldestSlot].firstPartTime = millis();
  for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
    concatBuffer[oldestSlot].parts[j].valid = false;
    concatBuffer[oldestSlot].parts[j].text = "";
  }
  return oldestSlot;
}

// 合并长短信各分段
String assembleConcatSms(int slot) {
  String result = "";
  for (int i = 0; i < concatBuffer[slot].totalParts; i++) {
    if (concatBuffer[slot].parts[i].valid) {
      result += concatBuffer[slot].parts[i].text;
    } else {
      result += "[缺失分段" + String(i + 1) + "]";
    }
  }
  return result;
}

// 清空长短信槽位
void clearConcatSlot(int slot) {
  concatBuffer[slot].inUse = false;
  concatBuffer[slot].receivedParts = 0;
  concatBuffer[slot].sender = "";
  concatBuffer[slot].timestamp = "";
  for (int j = 0; j < MAX_CONCAT_PARTS; j++) {
    concatBuffer[slot].parts[j].valid = false;
    concatBuffer[slot].parts[j].text = "";
  }
}

// 前置声明
void processSmsContent(const char* sender, const char* text, const char* timestamp);

// 检查长短信超时并转发
void checkConcatTimeout() {
  unsigned long now = millis();
  for (int i = 0; i < MAX_CONCAT_MESSAGES; i++) {
    if (concatBuffer[i].inUse) {
      if (now - concatBuffer[i].firstPartTime >= CONCAT_TIMEOUT_MS) {
        Serial.println("⏰ 长短信超时，强制转发不完整消息");
        Serial.printf("  参考号: %d, 已收到: %d/%d\n", 
                      concatBuffer[i].refNumber,
                      concatBuffer[i].receivedParts,
                      concatBuffer[i].totalParts);
        
        // 合并已收到的分段
        String fullText = assembleConcatSms(i);
        
        // 处理短信内容
        processSmsContent(concatBuffer[i].sender.c_str(), 
                         fullText.c_str(), 
                         concatBuffer[i].timestamp.c_str());
        
        // 清空槽位
        clearConcatSlot(i);
      }
    }
  }
}

// 发送短信数据到服务器
// URL编码辅助函数
String urlEncode(const String& str) {
  String encoded = "";
  char c;
  char code0;
  char code1;
  for (unsigned int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (c == ' ') {
      encoded += '+';
    } else if (isalnum(c)) {
      encoded += c;
    } else {
      code1 = (c & 0xf) + '0';
      if ((c & 0xf) > 9) code1 = (c & 0xf) - 10 + 'A';
      c = (c >> 4) & 0xf;
      code0 = c + '0';
      if (c > 9) code0 = c - 10 + 'A';
      encoded += '%';
      encoded += code0;
      encoded += code1;
    }
  }
  return encoded;
}

// JSON转义函数
String jsonEscape(const String& str) {
  String result = "";
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (c == '"') result += "\\\"";
    else if (c == '\\') result += "\\\\";
    else if (c == '\n') result += "\\n";
    else if (c == '\r') result += "\\r";
    else if (c == '\t') result += "\\t";
    else result += c;
  }
  return result;
}

// 发送单个推送通道
void sendToChannel(const PushChannel& channel, const char* sender, const char* message, const char* timestamp) {
  if (!channel.enabled) return;
  if (channel.url.length() == 0) return;
  
  HTTPClient http;
  String channelName = channel.name.length() > 0 ? channel.name : ("通道" + String(channel.type));
  Serial.println("发送到推送通道: " + channelName);
  
  int httpCode = 0;
  String senderEscaped = jsonEscape(String(sender));
  String messageEscaped = jsonEscape(String(message));
  String timestampEscaped = jsonEscape(String(timestamp));
  
  switch (channel.type) {
    case PUSH_TYPE_POST_JSON: {
      // 标准POST JSON格式
      http.begin(channel.url);
      http.addHeader("Content-Type", "application/json");
      String jsonData = "{";
      jsonData += "\"sender\":\"" + senderEscaped + "\",";
      jsonData += "\"message\":\"" + messageEscaped + "\",";
      jsonData += "\"timestamp\":\"" + timestampEscaped + "\"";
      jsonData += "}";
      Serial.println("POST JSON: " + jsonData);
      httpCode = http.POST(jsonData);
      break;
    }
    
    case PUSH_TYPE_BARK: {
      // Bark推送格式
      http.begin(channel.url);
      http.addHeader("Content-Type", "application/json");
      String jsonData = "{";
      jsonData += "\"title\":\"" + senderEscaped + "\",";
      jsonData += "\"body\":\"" + messageEscaped + "\"";
      jsonData += "}";
      Serial.println("BARK: " + jsonData);
      httpCode = http.POST(jsonData);
      break;
    }
    
    case PUSH_TYPE_GET: {
      // GET请求，参数放URL里
      String getUrl = channel.url;
      if (getUrl.indexOf('?') == -1) {
        getUrl += "?";
      } else {
        getUrl += "&";
      }
      getUrl += "sender=" + urlEncode(String(sender));
      getUrl += "&message=" + urlEncode(String(message));
      getUrl += "&timestamp=" + urlEncode(String(timestamp));
      Serial.println("GET: " + getUrl);
      http.begin(getUrl);
      httpCode = http.GET();
      break;
    }
    
    case PUSH_TYPE_CUSTOM: {
      // 自定义模板
      if (channel.customBody.length() == 0) {
        Serial.println("自定义模板为空，跳过");
        return;
      }
      http.begin(channel.url);
      http.addHeader("Content-Type", "application/json");
      String body = channel.customBody;
      body.replace("{sender}", senderEscaped);
      body.replace("{message}", messageEscaped);
      body.replace("{timestamp}", timestampEscaped);
      Serial.println("自定义: " + body);
      httpCode = http.POST(body);
      break;
    }
    
    default:
      Serial.println("未知推送类型");
      return;
  }
  
  if (httpCode > 0) {
    Serial.printf("[%s] 响应码: %d\n", channelName.c_str(), httpCode);
    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_CREATED) {
      String response = http.getString();
      Serial.println("响应: " + response);
    }
  } else {
    Serial.printf("[%s] HTTP请求失败: %s\n", channelName.c_str(), http.errorToString(httpCode).c_str());
  }
  http.end();
}

// 发送短信到所有启用的推送通道
void sendSMSToServer(const char* sender, const char* message, const char* timestamp) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi未连接，跳过推送");
    return;
  }
  
  bool hasEnabledChannel = false;
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (isPushChannelValid(config.pushChannels[i])) {
      hasEnabledChannel = true;
      break;
    }
  }
  
  if (!hasEnabledChannel) {
    Serial.println("没有启用的推送通道");
    return;
  }
  
  Serial.println("\n=== 开始多通道推送 ===");
  for (int i = 0; i < MAX_PUSH_CHANNELS; i++) {
    if (isPushChannelValid(config.pushChannels[i])) {
      sendToChannel(config.pushChannels[i], sender, message, timestamp);
      delay(100); // 短暂延迟避免请求过快
    }
  }
  Serial.println("=== 多通道推送完成 ===\n");
}

// 读取串口一行（含回车换行），返回行字符串，无新行时返回空
String readSerialLine(HardwareSerial& port) {
  static char lineBuf[SERIAL_BUFFER_SIZE];
  static int linePos = 0;

  while (port.available()) {
    char c = port.read();
    if (c == '\n') {
      lineBuf[linePos] = 0;
      String res = String(lineBuf);
      linePos = 0;
      return res;
    } else if (c != '\r') {  // 跳过\r
      if (linePos < SERIAL_BUFFER_SIZE - 1)
        lineBuf[linePos++] = c;
      else
        linePos = 0;  //超长报错保护，重头计
    }
  }
  return "";
}

// 检查字符串是否为有效的十六进制PDU数据
bool isHexString(const String& str) {
  if (str.length() == 0) return false;
  for (unsigned int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

// 处理最终的短信内容（管理员命令检查和转发）
void processSmsContent(const char* sender, const char* text, const char* timestamp) {
  Serial.println("=== 处理短信内容 ===");
  Serial.println("发送者: " + String(sender));
  Serial.println("时间戳: " + String(timestamp));
  Serial.println("内容: " + String(text));
  Serial.println("====================");

  // 检查是否为管理员命令
  if (isAdmin(sender)) {
    Serial.println("收到管理员短信，检查命令...");
    String smsText = String(text);
    smsText.trim();
    
    // 检查是否为命令格式
    if (smsText.startsWith("SMS:") || smsText.equals("RESET")) {
      processAdminCommand(sender, text);
      // 命令已处理，不再发送普通通知邮件
      return;
    }
  }

  // 发送通知http（推送到所有启用的通道）
  sendSMSToServer(sender, text, timestamp);
  
  // 发送MQTT通知
  #ifdef ENABLE_MQTT
  publishMqttSmsReceived(sender, text, timestamp);
  #endif
  
  // 发送通知邮件
  String subject = ""; subject+="短信";subject+=sender;subject+=",";subject+=text;
  String body = ""; body+="来自：";body+=sender;body+="，时间：";body+=timestamp;body+="，内容：";body+=text;
  sendEmailNotification(subject.c_str(), body.c_str());
}

// 处理URC和PDU
void checkSerial1URC() {
  static enum { IDLE,
                WAIT_PDU } state = IDLE;

  String line = readSerialLine(Serial1);
  if (line.length() == 0) return;

  // 打印到调试串口
  Serial.println("Debug> " + line);

  if (state == IDLE) {
    // 检测到短信上报URC头
    if (line.startsWith("+CMT:")) {
      Serial.println("检测到+CMT，等待PDU数据...");
      state = WAIT_PDU;
    }
  } else if (state == WAIT_PDU) {
    // 跳过空行
    if (line.length() == 0) {
      return;
    }
    
    // 如果是十六进制字符串，认为是PDU数据
    if (isHexString(line)) {
      Serial.println("收到PDU数据: " + line);
      Serial.println("PDU长度: " + String(line.length()) + " 字符");
      
      // 解析PDU
      if (!pdu.decodePDU(line.c_str())) {
        Serial.println("❌ PDU解析失败！");
      } else {
        Serial.println("✓ PDU解析成功");
        Serial.println("=== 短信内容 ===");
        Serial.println("发送者: " + String(pdu.getSender()));
        Serial.println("时间戳: " + String(pdu.getTimeStamp()));
        Serial.println("内容: " + String(pdu.getText()));
        
        // 获取长短信信息
        int* concatInfo = pdu.getConcatInfo();
        int refNumber = concatInfo[0];
        int partNumber = concatInfo[1];
        int totalParts = concatInfo[2];
        
        Serial.printf("长短信信息: 参考号=%d, 当前=%d, 总计=%d\n", refNumber, partNumber, totalParts);
        Serial.println("===============");

        // 判断是否为长短信
        if (totalParts > 1 && partNumber > 0) {
          // 这是长短信的一部分
          Serial.printf("📧 收到长短信分段 %d/%d\n", partNumber, totalParts);
          
          // 查找或创建缓存槽位
          int slot = findOrCreateConcatSlot(refNumber, pdu.getSender(), totalParts);
          
          // 存储该分段（partNumber从1开始，数组从0开始）
          int partIndex = partNumber - 1;
          if (partIndex >= 0 && partIndex < MAX_CONCAT_PARTS) {
            if (!concatBuffer[slot].parts[partIndex].valid) {
              concatBuffer[slot].parts[partIndex].valid = true;
              concatBuffer[slot].parts[partIndex].text = String(pdu.getText());
              concatBuffer[slot].receivedParts++;
              
              // 如果是第一个收到的分段，保存时间戳
              if (concatBuffer[slot].receivedParts == 1) {
                concatBuffer[slot].timestamp = String(pdu.getTimeStamp());
              }
              
              Serial.printf("  已缓存分段 %d，当前已收到 %d/%d\n", 
                           partNumber, 
                           concatBuffer[slot].receivedParts, 
                           totalParts);
            } else {
              Serial.printf("  ⚠️ 分段 %d 已存在，跳过\n", partNumber);
            }
          }
          
          // 检查是否已收齐所有分段
          if (concatBuffer[slot].receivedParts >= totalParts) {
            Serial.println("✅ 长短信已收齐，开始合并转发");
            
            // 合并所有分段
            String fullText = assembleConcatSms(slot);
            
            // 处理完整短信
            processSmsContent(concatBuffer[slot].sender.c_str(), 
                             fullText.c_str(), 
                             concatBuffer[slot].timestamp.c_str());
            
            // 清空槽位
            clearConcatSlot(slot);
          }
        } else {
          // 普通短信，直接处理
          processSmsContent(pdu.getSender(), pdu.getText(), pdu.getTimeStamp());
        }
      }
      
      // 返回IDLE状态
      state = IDLE;
    } 
    // 如果是其他内容（OK、ERROR等），也返回IDLE
    else {
      Serial.println("收到非PDU数据，返回IDLE状态");
      state = IDLE;
    }
  }
}

void blink_short(unsigned long gap_time = 500) {
  digitalWrite(LED_BUILTIN, LOW);
  delay(50);
  digitalWrite(LED_BUILTIN, HIGH);
  delay(gap_time);
}

bool sendATandWaitOK(const char* cmd, unsigned long timeout) {
  while (Serial1.available()) Serial1.read();
  Serial1.println(cmd);
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < timeout) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (resp.indexOf("OK") >= 0) return true;
      if (resp.indexOf("ERROR") >= 0) return false;
    }
  }
  return false;
}

bool waitCGATT1() {
  Serial1.println("AT+CGATT?");
  unsigned long start = millis();
  String resp = "";
  while (millis() - start < 2000) {
    while (Serial1.available()) {
      char c = Serial1.read();
      resp += c;
      if (resp.indexOf("+CGATT: 1") >= 0) return true;
      if (resp.indexOf("+CGATT: 0") >= 0) return false;
    }
  }
  return false;
}

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.begin(115200);
  Serial1.begin(115200, SERIAL_8N1, RXD, TXD);
  Serial1.setRxBufferSize(SERIAL_BUFFER_SIZE);
  
  // 初始化长短信缓存
  initConcatBuffer();
  
  // 加载配置
  loadConfig();
  configValid = isConfigValid();
  
  WiFiMulti.addAP(WIFI_SSID, WIFI_PASS);
  Serial.println("连接wifi");
  Serial.println(WIFI_SSID);
  while (WiFiMulti.run() != WL_CONNECTED) blink_short();
  Serial.println("wifi已连接");
  Serial.print("IP地址: ");
  Serial.println(WiFi.localIP());
  
  // 启动HTTP服务器
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/tools", handleToolsPage);
  server.on("/sms", handleToolsPage);  // 兼容旧链接
  server.on("/sendsms", HTTP_POST, handleSendSms);
  server.on("/ping", HTTP_POST, handlePing);
  server.on("/timer", HTTP_POST, handleTimer);
  server.on("/query", handleQuery);
  server.begin();
  Serial.println("HTTP服务器已启动");
  
  ssl_client.setInsecure();
  while (!sendATandWaitOK("AT", 1000)) {
    Serial.println("AT未响应，重试...");
    blink_short();
  }
  Serial.println("模组AT响应正常");
  //设置短信自动上报
  while (!sendATandWaitOK("AT+CNMI=2,2,0,0,0", 1000)) {
    Serial.println("设置CNMI失败，重试...");
    blink_short();
  }
  Serial.println("CNMI参数设置完成");
  //配置PDU模式
  while (!sendATandWaitOK("AT+CMGF=0", 1000)) {
    Serial.println("设置PDU模式失败，重试...");
    blink_short();
  }
  Serial.println("PDU模式设置完成");
  //等待CGATT附着
  while (!waitCGATT1()) {
    Serial.println("等待CGATT附着...");
    blink_short();
  }
  Serial.println("CGATT已附着");
  digitalWrite(LED_BUILTIN, LOW);
  
  // 如果配置有效，发送启动通知
  if (configValid) {
    Serial.println("配置有效，发送启动通知...");
    String subject = "短信转发器已启动";
    String body = "设备已启动\n设备地址: " + getDeviceUrl();
    sendEmailNotification(subject.c_str(), body.c_str());
  }
  
  // ========== MQTT初始化 ==========
  #ifdef ENABLE_MQTT
  Serial.println("初始化MQTT...");
  initMqttTopics();
  mqttClient.setServer(MQTT_SERVER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(1024);  // 增加缓冲区大小以支持较长消息
  
  // 首次连接MQTT
  mqttReconnect();
  Serial.println("MQTT初始化完成");
  #endif
}

void loop() {
  // 处理HTTP请求
  server.handleClient();
  
  // 如果配置无效，每秒打印一次IP地址
  if (!configValid) {
    if (millis() - lastPrintTime >= 1000) {
      lastPrintTime = millis();
      Serial.println("⚠️ 请访问 " + getDeviceUrl() + " 配置系统参数");
    }
  }
  // 检查定时任务执行
  if (config.timerEnabled && timerIntervalMs > 0 && configValid) {
    if (millis() - lastTimerExec >= timerIntervalMs) {
      Serial.println("⏰ 执行定时任务...");
      lastTimerExec = millis();
      
      if (config.timerType == 0) {
        // 定时Ping
        Serial.println("开始定时Ping...");
        if (sendATandWaitOK("AT+CGACT=1,1", 10000)) {
          // 这里简化处理，直接执行Ping，不解析详细结果，因为没有前端等待
          sendATandWaitOK("AT+MPING=1,\"8.8.8.8\",4,32,255", 30000);
          delay(2000);
          sendATandWaitOK("AT+CGACT=0,1", 5000);
          Serial.println("定时Ping完成");
          
          #ifdef ENABLE_MQTT
          publishMqttStatus("active_ping");
          #endif
        }
      } else if (config.timerType == 1 && config.timerPhone.length() > 0 && config.timerMessage.length() > 0) {
        // 定时发送短信
        Serial.println("发送保号短信...");
        sendSMS(config.timerPhone.c_str(), config.timerMessage.c_str());
        
        #ifdef ENABLE_MQTT
        publishMqttSmsSent(config.timerPhone.c_str(), config.timerMessage.c_str(), true);
        #endif
      }
    }
  }
  
  // ========== MQTT处理 ==========
  #ifdef ENABLE_MQTT
  if (WiFi.status() == WL_CONNECTED) {
    if (!mqttClient.connected()) {
      // 定期尝试重连
      unsigned long now = millis();
      if (now - lastMqttReconnectAttempt > MQTT_RECONNECT_INTERVAL) {
        lastMqttReconnectAttempt = now;
        mqttReconnect();
      }
    } else {
      // MQTT已连接，处理消息
      mqttClient.loop();
    }
  }
  #endif
  
  // 检查长短信超时
  checkConcatTimeout();
  
  // 本地透传
  if (Serial.available()) Serial1.write(Serial.read());
  // 检查URC和解析
  checkSerial1URC();
}

// ========== MQTT功能实现 ==========
#ifdef ENABLE_MQTT

// 获取MAC地址后缀作为设备唯一ID
String getMacSuffix() {
  String mac = WiFi.macAddress();
  mac.replace(":", "");
  mac.toLowerCase();
  return mac.substring(6);  // 取后6位
}

// 初始化MQTT主题
void initMqttTopics() {
  mqttDeviceId = getMacSuffix();
  String prefix = String(MQTT_TOPIC_PREFIX) + "/" + mqttDeviceId;
  
  // 发布主题
  mqttTopicStatus = prefix + "/status";
  mqttTopicSmsReceived = prefix + "/sms/received";
  mqttTopicSmsSent = prefix + "/sms/sent";
  mqttTopicPingResult = prefix + "/ping/result";
  
  // 订阅主题
  mqttTopicSmsSend = prefix + "/sms/send";
  mqttTopicPing = prefix + "/ping";
  mqttTopicCmd = prefix + "/cmd";
  
  Serial.println("MQTT设备ID: " + mqttDeviceId);
  Serial.println("MQTT主题前缀: " + prefix);
}

// MQTT重连函数
void mqttReconnect() {
  if (mqttClient.connected()) return;
  
  String clientId = String(MQTT_CLIENT_ID_PREFIX) + mqttDeviceId;
  Serial.println("连接MQTT服务器: " + String(MQTT_SERVER));
  Serial.println("客户端ID: " + clientId);
  
  bool connected = false;
  
  // 配置遗嘱消息（设备离线时自动发送）
  String willMessage = "{\"status\":\"offline\",\"device\":\"" + mqttDeviceId + "\"}";
  
  if (strlen(MQTT_USER) > 0) {
    connected = mqttClient.connect(
      clientId.c_str(),
      MQTT_USER,
      MQTT_PASS,
      mqttTopicStatus.c_str(),
      1,  // QoS
      true,  // retain
      willMessage.c_str()
    );
  } else {
    connected = mqttClient.connect(
      clientId.c_str(),
      mqttTopicStatus.c_str(),
      1,  // QoS
      true,  // retain
      willMessage.c_str()
    );
  }
  
  if (connected) {
    Serial.println("✅ MQTT连接成功");
    
    // 订阅命令主题
    mqttClient.subscribe(mqttTopicSmsSend.c_str());
    mqttClient.subscribe(mqttTopicPing.c_str());
    mqttClient.subscribe(mqttTopicCmd.c_str());
    Serial.println("已订阅主题:");
    Serial.println("  - " + mqttTopicSmsSend);
    Serial.println("  - " + mqttTopicPing);
    Serial.println("  - " + mqttTopicCmd);
    
    // 发布上线状态
    publishMqttStatus("online");
  } else {
    Serial.print("❌ MQTT连接失败, 错误码: ");
    Serial.println(mqttClient.state());
  }
}

// MQTT消息回调处理
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // 转换payload为字符串
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  
  Serial.println("=== MQTT消息接收 ===");
  Serial.println("主题: " + String(topic));
  Serial.println("内容: " + message);
  Serial.println("====================");
  
  // 处理发送短信命令
  if (String(topic) == mqttTopicSmsSend) {
    // 解析JSON: {"phone":"xxx","message":"xxx"}
    int phoneStart = message.indexOf("\"phone\"");
    int msgStart = message.indexOf("\"message\"");
    
    if (phoneStart >= 0 && msgStart >= 0) {
      // 提取phone值
      int phoneValStart = message.indexOf(":", phoneStart) + 1;
      int phoneValEnd = message.indexOf(",", phoneValStart);
      if (phoneValEnd < 0) phoneValEnd = message.indexOf("}", phoneValStart);
      String phoneRaw = message.substring(phoneValStart, phoneValEnd);
      phoneRaw.trim();
      // 去除引号
      if (phoneRaw.startsWith("\"")) phoneRaw = phoneRaw.substring(1);
      if (phoneRaw.endsWith("\"")) phoneRaw = phoneRaw.substring(0, phoneRaw.length() - 1);
      
      // 提取message值
      int msgValStart = message.indexOf(":", msgStart) + 1;
      int msgValEnd = message.lastIndexOf("\"");
      String msgRaw = message.substring(msgValStart, msgValEnd + 1);
      msgRaw.trim();
      // 去除首尾引号
      if (msgRaw.startsWith("\"")) msgRaw = msgRaw.substring(1);
      if (msgRaw.endsWith("\"")) msgRaw = msgRaw.substring(0, msgRaw.length() - 1);
      
      Serial.println("MQTT发送短信命令:");
      Serial.println("  目标: " + phoneRaw);
      Serial.println("  内容: " + msgRaw);
      
      bool success = sendSMS(phoneRaw.c_str(), msgRaw.c_str());
      publishMqttSmsSent(phoneRaw.c_str(), msgRaw.c_str(), success);
    } else {
      Serial.println("❌ 短信命令格式错误");
      publishMqttSmsSent("", "", false);
    }
  }
  // 处理Ping命令
  else if (String(topic) == mqttTopicPing) {
    String host = "8.8.8.8";  // 默认目标
    
    // 解析JSON: {"host":"xxx"} 或 {}
    int hostStart = message.indexOf("\"host\"");
    if (hostStart >= 0) {
      int hostValStart = message.indexOf(":", hostStart) + 1;
      int hostValEnd = message.indexOf("\"", hostValStart + 2);
      if (hostValEnd > hostValStart) {
        String hostRaw = message.substring(hostValStart, hostValEnd + 1);
        hostRaw.trim();
        if (hostRaw.startsWith("\"")) hostRaw = hostRaw.substring(1);
        if (hostRaw.endsWith("\"")) hostRaw = hostRaw.substring(0, hostRaw.length() - 1);
        if (hostRaw.length() > 0) host = hostRaw;
      }
    }
    
    Serial.println("MQTT Ping命令: " + host);
    
    // 执行Ping操作
    // 激活数据连接
    String activateResp = sendATCommand("AT+CGACT=1,1", 10000);
    delay(500);
    
    // 发送Ping命令
    String pingCmd = "AT+MPING=\"" + host + "\",30,1";
    while (Serial1.available()) Serial1.read();
    Serial1.println(pingCmd);
    
    unsigned long start = millis();
    String resp = "";
    bool gotResult = false;
    String resultMsg = "";
    bool pingSuccess = false;
    
    while (millis() - start < 35000) {
      while (Serial1.available()) {
        char c = Serial1.read();
        resp += c;
        
        int mpingIdx = resp.indexOf("+MPING:");
        if (mpingIdx >= 0) {
          int lineEnd = resp.indexOf('\n', mpingIdx);
          if (lineEnd >= 0) {
            String mpingLine = resp.substring(mpingIdx, lineEnd);
            mpingLine.trim();
            
            int colonIdx = mpingLine.indexOf(':');
            if (colonIdx >= 0) {
              String params = mpingLine.substring(colonIdx + 1);
              params.trim();
              
              int commaIdx = params.indexOf(',');
              int result = params.substring(0, commaIdx > 0 ? commaIdx : params.length()).toInt();
              
              gotResult = true;
              pingSuccess = (result == 0 || result == 1) || (params.indexOf(',') >= 0 && params.length() > 5);
              
              if (pingSuccess && commaIdx > 0) {
                // 解析详细信息
                resultMsg = params;
              } else {
                resultMsg = "错误码: " + String(result);
              }
            }
            break;
          }
        }
        
        if (resp.indexOf("ERROR") >= 0) {
          gotResult = true;
          pingSuccess = false;
          resultMsg = "模组错误";
          break;
        }
      }
      if (gotResult) break;
      delay(10);
    }
    
    // 关闭数据连接
    sendATCommand("AT+CGACT=0,1", 5000);
    
    if (!gotResult) {
      resultMsg = "超时";
    }
    
    publishMqttPingResult(host.c_str(), pingSuccess, resultMsg.c_str());
  }
  // 处理控制命令
  else if (String(topic) == mqttTopicCmd) {
    // 解析JSON: {"action":"xxx"}
    int actionStart = message.indexOf("\"action\"");
    if (actionStart >= 0) {
      int actionValStart = message.indexOf(":", actionStart) + 1;
      int actionValEnd = message.indexOf("\"", actionValStart + 2);
      String actionRaw = message.substring(actionValStart, actionValEnd + 1);
      actionRaw.trim();
      if (actionRaw.startsWith("\"")) actionRaw = actionRaw.substring(1);
      if (actionRaw.endsWith("\"")) actionRaw = actionRaw.substring(0, actionRaw.length() - 1);
      
      Serial.println("MQTT控制命令: " + actionRaw);
      
      if (actionRaw == "restart" || actionRaw == "reset") {
        Serial.println("执行重启命令...");
        publishMqttStatus("restarting");
        delay(500);
        ESP.restart();
      }
      else if (actionRaw == "status") {
        // 发送详细状态信息
        String statusJson = "{";
        statusJson += "\"status\":\"online\",";
        statusJson += "\"device\":\"" + mqttDeviceId + "\",";
        statusJson += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
        statusJson += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
        statusJson += "\"uptime\":" + String(millis() / 1000) + ",";
        statusJson += "\"free_heap\":" + String(ESP.getFreeHeap());
        statusJson += "}";
        mqttClient.publish(mqttTopicStatus.c_str(), statusJson.c_str(), true);
        Serial.println("已发送状态信息");
      }
      else {
        Serial.println("未知命令: " + actionRaw);
      }
    }
  }
}

// 发布收到短信通知
void publishMqttSmsReceived(const char* sender, const char* message, const char* timestamp) {
  if (!mqttClient.connected()) return;
  
  String json = "{";
  json += "\"sender\":\"" + jsonEscape(String(sender)) + "\",";
  json += "\"message\":\"" + jsonEscape(String(message)) + "\",";
  json += "\"timestamp\":\"" + jsonEscape(String(timestamp)) + "\",";
  json += "\"device\":\"" + mqttDeviceId + "\"";
  json += "}";
  
  mqttClient.publish(mqttTopicSmsReceived.c_str(), json.c_str());
  Serial.println("📤 MQTT发布收到短信通知");
}

// 发布发送短信结果
void publishMqttSmsSent(const char* phone, const char* message, bool success) {
  if (!mqttClient.connected()) return;
  
  String json = "{";
  json += "\"success\":" + String(success ? "true" : "false") + ",";
  json += "\"phone\":\"" + jsonEscape(String(phone)) + "\",";
  json += "\"message\":\"" + jsonEscape(String(message)) + "\",";
  json += "\"device\":\"" + mqttDeviceId + "\"";
  json += "}";
  
  mqttClient.publish(mqttTopicSmsSent.c_str(), json.c_str());
  Serial.println("📤 MQTT发布发送短信结果: " + String(success ? "成功" : "失败"));
}

// 发布Ping测试结果
void publishMqttPingResult(const char* host, bool success, const char* result) {
  if (!mqttClient.connected()) return;
  
  String json = "{";
  json += "\"success\":" + String(success ? "true" : "false") + ",";
  json += "\"host\":\"" + String(host) + "\",";
  json += "\"result\":\"" + jsonEscape(String(result)) + "\",";
  json += "\"device\":\"" + mqttDeviceId + "\"";
  json += "}";
  
  mqttClient.publish(mqttTopicPingResult.c_str(), json.c_str());
  Serial.println("📤 MQTT发布Ping结果: " + String(success ? "成功" : "失败"));
}

// 发布设备状态
void publishMqttStatus(const char* status) {
  if (!mqttClient.connected() && String(status) != "online") return;
  
  String json = "{";
  json += "\"status\":\"" + String(status) + "\",";
  json += "\"device\":\"" + mqttDeviceId + "\",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\"";
  json += "}";
  
  mqttClient.publish(mqttTopicStatus.c_str(), json.c_str(), true);  // retain=true
  Serial.println("📤 MQTT发布状态: " + String(status));
}

#endif  // ENABLE_MQTT
