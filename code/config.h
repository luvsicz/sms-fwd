#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>
#include <vector>
#include <atomic>
#include <memory>

// 串口映射
#define TXD 3
#define RXD 4

// mDNS 主机名（访问地址为 http://sms.local）
#define MDNS_HOSTNAME "sms"

// 推送通道类型
enum PushType {
  PUSH_TYPE_NONE = 0,      // 未启用
  PUSH_TYPE_POST_JSON = 1, // POST JSON格式 {"sender":"xxx","message":"xxx","timestamp":"xxx"}
  PUSH_TYPE_BARK = 2,      // Bark格式 POST {"title":"xxx","body":"xxx"}
  PUSH_TYPE_GET = 3,       // GET请求，参数放URL中
  PUSH_TYPE_CUSTOM = 4,    // 自定义模板
  PUSH_TYPE_TELEGRAM = 5,  // Telegram Bot
  PUSH_TYPE_WECOM = 6,     // 企业微信机器人
  PUSH_TYPE_DINGTALK = 7   // 钉钉机器人
};

// 最大推送通道数
#define MAX_PUSH_CHANNELS 3
static_assert(MAX_PUSH_CHANNELS < 10000000, "Push NVS keys must fit 15 characters");

// NVS string 的 4000 字节限制包含结尾 NUL；String::length() 是 UTF-8 字节数。
static constexpr size_t CONFIG_NVS_MAX_STRING_BYTES = 3999;

// 短信历史记录设置
#define MAX_SMS_HISTORY 100

// 黑白名单设置
#define MAX_FILTER_NUMBERS 100  // 扩容到100个

// 推送通道配置
struct PushChannel {
  bool enabled;
  PushType type;
  String name;
  String url;
  String key1;       // 按类型复用：Telegram Chat ID / 钉钉加签密钥
  String key2;       // 按类型复用：Bark group
  String customBody;
};

// 短信历史记录结构 (旧结构保留，实际上改用 SPIFFS)
struct SmsRecord {
  String sender;
  String message;
  String timestamp;
  bool valid;
};

// 统计计数器结构（使用 std::atomic 支持多任务/多线程并发安全访问）
struct Statistics {
  std::atomic<uint32_t> smsReceived{0};    // 收到短信数
  std::atomic<uint32_t> smsSent{0};        // 发送短信数
  std::atomic<uint32_t> callsReceived{0};  // 收到来电数
  std::atomic<uint32_t> pushSuccess{0};    // 推送成功数
  std::atomic<uint32_t> pushFailed{0};     // 推送失败数
  std::atomic<uint32_t> bootCount{0};      // 启动次数
  std::atomic<uint32_t> brownoutResets{0}; // 欠压复位次数（强烈指向供电不足）
  std::atomic<uint32_t> powerOnResets{0};  // 上电复位次数（可能是断电/供电跌落）
  std::atomic<uint32_t> watchdogResets{0}; // 看门狗复位次数
  std::atomic<uint32_t> softwareResets{0}; // 软件主动重启次数
  std::atomic<uint32_t> panicResets{0};    // 异常崩溃复位次数

  Statistics() = default;
  Statistics(const Statistics& other) {
    smsReceived.store(other.smsReceived.load());
    smsSent.store(other.smsSent.load());
    callsReceived.store(other.callsReceived.load());
    pushSuccess.store(other.pushSuccess.load());
    pushFailed.store(other.pushFailed.load());
    bootCount.store(other.bootCount.load());
    brownoutResets.store(other.brownoutResets.load());
    powerOnResets.store(other.powerOnResets.load());
    watchdogResets.store(other.watchdogResets.load());
    softwareResets.store(other.softwareResets.load());
    panicResets.store(other.panicResets.load());
  }
  Statistics& operator=(const Statistics& other) {
    if (this != &other) {
      smsReceived.store(other.smsReceived.load());
      smsSent.store(other.smsSent.load());
      callsReceived.store(other.callsReceived.load());
      pushSuccess.store(other.pushSuccess.load());
      pushFailed.store(other.pushFailed.load());
      bootCount.store(other.bootCount.load());
      brownoutResets.store(other.brownoutResets.load());
      powerOnResets.store(other.powerOnResets.load());
      watchdogResets.store(other.watchdogResets.load());
      softwareResets.store(other.softwareResets.load());
      panicResets.store(other.panicResets.load());
    }
    return *this;
  }
};

struct HistoryCleanupResult {
  int kept;
  int removed;
};

// 上次复位前尚未完成的高风险操作快照
struct InterruptedOperationDiagnostic {
  bool valid;
  String operation;
  String stage;
  unsigned long uptimeSeconds;
  uint32_t freeHeap;
  uint32_t minFreeHeap;
  uint32_t maxAllocHeap;
};

// WiFi 配置结构
#define MAX_WIFI_NETWORKS 3
// 确保即使 MAX_WIFI_NETWORKS 增大，生成的紧凑 NVS 键名（如 "w_s_" + idx）也不会超过 15 字节
static_assert(MAX_WIFI_NETWORKS < 100000000, "MAX_WIFI_NETWORKS is too large for 15-character NVS key");
struct WifiNetwork {
  String ssid;
  String password;
  bool enabled;
};

// 配置参数结构体
struct Config {
  // WiFi 配置（支持多个网络）
  WifiNetwork wifiNetworks[MAX_WIFI_NETWORKS];
  
  String smtpServer;
  int smtpPort;
  String smtpUser;
  String smtpPass;
  String smtpSendTo;

  bool emailEnabled;
  PushChannel pushChannels[MAX_PUSH_CHANNELS];
  String webUser;
  String webPass;
  
  // 定时任务配置
  bool timerEnabled;
  int timerType;
  int timerInterval;  // 天；加载/应用计划时至少为 1，NVS 默认仍为 30
  String timerPhone;
  String timerMessage;
  
  // MQTT 配置
  bool mqttEnabled;
  bool mqttControlOnly;
  String mqttServer;
  int mqttPort;
  String mqttUser;
  String mqttPass;
  String mqttPrefix;
  
  // Home Assistant MQTT 自动发现
  bool mqttHaDiscovery;     // 启用 HA 自动发现
  String mqttHaPrefix;      // HA 发现前缀，默认 "homeassistant"
  
  // 黑白名单配置（按号码过滤）
  bool filterEnabled;          // 是否启用过滤
  bool filterIsWhitelist;      // true=白名单, false=黑名单
  // 使用单个长字符串存储，内存中逗号分隔，避免定义大数组占用栈空间
  String filterList;  
  
  // 内容关键词过滤
  bool contentFilterEnabled;      // 是否启用内容过滤
  bool contentFilterIsWhitelist;  // true=白名单(只转发包含关键词的), false=黑名单(拦截包含关键词的)
  String contentFilterList;       // 关键词列表，逗号分隔
};

// 默认Web管理账号密码
#define DEFAULT_WEB_USER "admin"
#define DEFAULT_WEB_PASS "admin123"

// 长短信合并相关定义
#define MAX_CONCAT_PARTS 10
#define CONCAT_TIMEOUT_MS 30000
#define MAX_CONCAT_MESSAGES 5

// 历史按 UTF-8 字节计：每段保守预留 160 septet * 3 字节，覆盖 GSM7/UCS2 及缺段标记。
// 超限/非法 UTF-8/OOM 拒绝整条并记录错误，不截断正文或尾部 OTP/URL。
static constexpr size_t SMS_HISTORY_MAX_MESSAGE_BYTES = MAX_CONCAT_PARTS * 160U * 3U; // 4800
static constexpr size_t HISTORY_MAX_NUMBER_BYTES = 64;
static constexpr size_t HISTORY_MAX_TIMESTAMP_BYTES = 32;
// 含 JSON 最坏 6 倍转义与结构/LF；读取使用固定块 + 偏移索引，不缓存 100 条正文。
static constexpr size_t SMS_HISTORY_MAX_LINE_BYTES =
  6 * (SMS_HISTORY_MAX_MESSAGE_BYTES + HISTORY_MAX_NUMBER_BYTES + HISTORY_MAX_TIMESTAMP_BYTES) + 32;
static constexpr size_t CALL_HISTORY_MAX_LINE_BYTES =
  6 * (HISTORY_MAX_NUMBER_BYTES + HISTORY_MAX_TIMESTAMP_BYTES) + 24;
static constexpr size_t SMS_HISTORY_MAX_RESPONSE_BYTES = 32 * 1024;
static constexpr size_t CALL_HISTORY_MAX_RESPONSE_BYTES = 8 * 1024;
static constexpr size_t SMS_HISTORY_MAX_FILE_BYTES = 128 * 1024;
static constexpr size_t SMS_HISTORY_TRIM_BYTES = 64 * 1024;
static constexpr size_t CALL_HISTORY_MAX_FILE_BYTES = 30000;
static constexpr size_t CALL_HISTORY_TRIM_BYTES = 15000;
static constexpr size_t HISTORY_MIN_FREE_HEAP = 16 * 1024;
static_assert(SMS_HISTORY_MAX_LINE_BYTES + 2 <= SMS_HISTORY_MAX_RESPONSE_BYTES,
              "History response must fit one complete maximum SMS");
static_assert(SMS_HISTORY_TRIM_BYTES + SMS_HISTORY_MAX_LINE_BYTES + 2 < SMS_HISTORY_MAX_FILE_BYTES,
              "History trim must leave room for a complete maximum SMS");

#define SERIAL_BUFFER_SIZE 500  // 单行解析上限，不是 UART RX 环形缓冲容量
#define SERIAL_RX_BUFFER_SIZE 2048  // 独立接收缓冲，容纳连续 PDU/URC 突发
static_assert(SERIAL_RX_BUFFER_SIZE >= SERIAL_BUFFER_SIZE, "UART RX buffer must hold at least one serial line");
#define MAX_PDU_LENGTH 300

// 长短信分段结构
struct SmsPart {
  bool valid;
  String text;
};

// 长短信缓存结构
struct ConcatSms {
  bool inUse;
  int refNumber;
  String sender;
  String timestamp;
  int totalParts;
  int receivedParts;
  unsigned long firstPartTime;
  SmsPart parts[MAX_CONCAT_PARTS];
};

// 全局变量声明 (extern)
extern Config config;
extern Preferences preferences;
extern bool configValid;
extern unsigned long lastPrintTime;
extern uint64_t lastTimerExec;     // 本次启动的单调毫秒，不持久化
extern uint64_t timerIntervalSec;  // 完整周期（秒），不能降为 32 位
extern String lastCallNumber;
extern unsigned long lastCallNotifyTime;
extern ConcatSms concatBuffer[MAX_CONCAT_MESSAGES];
extern char serialBuf[SERIAL_BUFFER_SIZE];
extern int serialBufLen;

#define CALL_NOTIFY_DEDUP_MS 8000

// 短信历史和统计
extern SmsRecord smsHistory[MAX_SMS_HISTORY];
extern int smsHistoryIndex;
extern Statistics stats;
extern esp_reset_reason_t lastResetReasonCode;
extern String lastResetReasonText;
extern bool lastResetPowerSuspected;
extern InterruptedOperationDiagnostic lastInterruptedOperation;

// 函数声明
bool configStringFitsNvs(const String& value);
bool configFitsNvs(const Config& value);  // 全部持久化字符串，包含未启用的字段
// 主任务调用；检查每个写入结果，error 为静态文本，不含配置值。
// 保留 saveConfig() 调用形式；忽略 bool 返回值的旧调用仍可编译。
// 多键写入非掉电原子事务；失败可能留下部分 NVS 写入，调用者须保留/恢复 RAM。
bool saveConfig(const char** error = nullptr);
void loadConfig();
// 定时 API（主循环任务）：应用所有 timer 字段后重置；仅保存无关配置不重置。
// loadConfig() 也重置起点，重启不恢复上次剩余时间；非正天数统一按 UI 下限 1 天处理。
uint64_t monotonicMillis();
void resetTimerSchedule();
uint64_t timerRemainingSeconds();  // 禁用/到期为 0，其余向上取整到秒
void processTimerTask();           // 到期先推进起点，再执行一次；失败不立即重试
bool isPushChannelValid(const PushChannel& ch);
bool isConfigValid();
String getDeviceUrl();
void initSmsStorage();
void addSmsToHistory(const char* sender, const char* message, const char* timestamp);
String getSmsHistory();
void addCallToHistory(const char* caller, const char* timestamp);
String getCallHistory();
HistoryCleanupResult cleanupSmsHistory();
HistoryCleanupResult cleanupCallHistory();
void clearSmsHistory();
void clearCallHistory();
bool safeTrimHistoryFile(const char* path, size_t maxSize, size_t targetSize);
String normalizePhoneNumber(const String& rawNumber);
bool phoneNumbersMatch(const String& senderNum, const String& filterNum);
bool isNumberFiltered(const char* number);
bool isContentFiltered(const char* content);
void saveStats();
void initStatsLock();
void statsIncrementSmsReceived();
void statsIncrementSmsSent();
void statsIncrementCallsReceived();
void statsIncrementPushSuccess();
void statsIncrementPushFailed();
void loadStats();
void resetStats(bool preserveBootCount = true);
String resetReasonToString(esp_reset_reason_t reason);
bool isPowerRelatedReset(esp_reset_reason_t reason);
void recordOperationDiagnostic(const char* operation, const char* stage);
void clearOperationDiagnostic();

#endif // CONFIG_H
