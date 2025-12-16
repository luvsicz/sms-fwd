// MQTT配置 - 请根据你的MQTT服务器修改

// 是否启用MQTT功能（注释这行则禁用MQTT）
#define ENABLE_MQTT

// MQTT服务器地址（可以是IP或域名）
#define MQTT_SERVER "broker.emqx.io"

// MQTT服务器端口（通常1883为非加密，8883为加密）
#define MQTT_PORT 1883

// MQTT客户端ID前缀（会自动添加设备MAC后缀）
#define MQTT_CLIENT_ID_PREFIX "sms_forwarding_"

// MQTT认证（留空则不使用认证）
#define MQTT_USER ""
#define MQTT_PASS ""

// MQTT主题前缀（建议使用唯一前缀避免冲突）
#define MQTT_TOPIC_PREFIX "sms_forwarding"

// 各功能主题（会自动拼接设备MAC地址作为唯一标识）
// 发布主题（设备发送消息到这些主题）
// - {prefix}/{device_id}/status       设备状态（online/offline）
// - {prefix}/{device_id}/sms/received 收到短信通知
// - {prefix}/{device_id}/ping/result  Ping测试结果
// - {prefix}/{device_id}/sms/sent     发送短信结果

// 订阅主题（设备监听这些主题的命令）
// - {prefix}/{device_id}/sms/send     发送短信命令 {"phone":"xxx","message":"xxx"}
// - {prefix}/{device_id}/ping         Ping测试命令 {"host":"8.8.8.8"} 或 {}
// - {prefix}/{device_id}/cmd          其他控制命令 {"action":"restart"/"status"}
