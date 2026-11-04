#ifndef PUSH_SERVICE_H
#define PUSH_SERVICE_H

#include <Arduino.h>
#include "config.h"

// 函数声明
String urlEncode(const String& str);
String jsonEscape(const String& str);
String getCurrentTimeString();
void sendToChannel(const PushChannel& channel, const char* sender, const char* message, const char* timestamp);
void sendSMSToServer(const char* sender, const char* message, const char* timestamp);
// 仅主任务调用，与 Web 配置更新串行；完整复制后入队，后续切配置/禁用不影响已排队邮件。
// SMTP 锁只保护发送，不保护配置或队列初始化；新增并发 producer 前必须同步这些访问。
void sendEmailNotification(const char* subject, const char* body);
void initSmtpLock();

#endif // PUSH_SERVICE_H
