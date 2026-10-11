📋 项目代码审查报告
我已对整个 msg_forward（ESP32 短信转发器）项目进行了全面审查。以下是分级汇总：

🔴 严重问题（4项）
#
问题
文件
C1
WiFi 密码硬编码 — 真实密码 "1531828613" 直接写在 wifi_config.h 中，推送到公开仓库即泄露
wifi_config.h
C2
Web/SMTP 密码明文回显 — handleRoot() 将密码明文填入 HTML 模板，浏览器查看源码即可看到
web_handlers.ino
C3
stats 全局变量多线程竞争 — FreeRTOS 后台任务与主 loop 同时读写 stats，++ 不是原子操作，可能损坏数据
push_service.ino
C4
ssl_client/smtp 共享对象无互斥 — 全局 TLS 对象在后台邮件任务中使用，无锁保护，可能导致崩溃
code.ino, push_service.ino
🟠 高风险问题（4项）
#
问题
文件
H1
MQTT 手写 JSON 解析器可被注入 — 恶意消息可导致解析出任意 AT 命令（如发短信到任意号码）
mqtt_handler.ino
H2
MQTT 无认证远程重启 — 任何能发布到对应 topic 的人可重启设备
mqtt_handler.ino
H3
Web 接口 HTML/XSS 注入 — AT 响应和用户配置值未做 HTML 转义，直接拼入页面
web_query.ino, web_handlers.ino
H4
readSerialLine 使用 static 缓冲区 — 若多上下文调用会产生数据竞争
sms_handler.ino
🟡 中等问题（7项）
#
问题
文件
M2
SPIFFS 文件截断可能丢失所有短信历史 — readString() 在低内存时返回空字符串，文件被清空
config.ino
M5
String 碎片化 — 大量 += 拼接导致 ESP32 堆碎片化，长时间运行可能 OOM
全局多处
M6
getSmsHistory() 将整个文件读入内存 — 大文件时消耗大量堆
config.ino
M7
保存配置时 SMTP/MQTT 密码被意外清空 — 空值未像 WiFi 密码一样做保留处理
web_handlers.ino
M1
millis() 溢出虽安全但缺少注释
code.ino
M3
文件打开失败时仍调用 close()
config.ino
M4
NVS 键名长度在 MAX_WIFI_NETWORKS 增大后可能超限
config.ino
🟢 低风险 + 架构建议（13项）
JSON 解析器脆弱（建议引入 ArduinoJson 库统一处理）
web_query.ino JSON 字段含未转义 HTML
日志输出过多（建议增加日志级别控制）
decodeUcs2Hex 不支持 Emoji（UTF-16 代理对）
formatTimestamp 时区跨月逻辑不完整（建议用 mktime）
clearConcatSlot 缺少参数边界检查
30+ 全局变量耦合度高（建议封装为结构体/命名空间）
AT 命令无序列化 — Web/MQTT/定时任务可能并发执行 AT 命令，串口响应会交叉混乱
HA Blueprint 硬编码个人设备名 notify.mobile_app_wvw
sensors.yaml 占位符 a1b2c3 不够醒目
✅ 做得好的地方
✅ 推送异步化（FreeRTOS 队列 + 后台任务）
✅ 看门狗管理到位
✅ 操作诊断快照，便于排查异常复位
✅ 完整的长短信 PDU 合并（含超时、越界防护）
✅ 双重 PDU 解析器（pdulib + fallback）
✅ 历史记录自愈（cleanupHistoryFile）
✅ MQTT HA 自动发现协议完整实现

🎯 最优先修复建议 TOP 3
安全：密码处理 — wifi_config.h 加入 .gitignore；HTML 中不回显密码明文；保存时空密码字段保留原值
安全：MQTT 注入防护 — 引入 ArduinoJson 做严格解析；对电话号码做格式白名单校验
稳定性：并发保护 — stats 使用 std::atomic 或互斥锁；AT 命令引入队列/互斥锁串行化