#ifndef MODEM_HANDLER_H
#define MODEM_HANDLER_H

#include <Arduino.h>

// 仅 setup/loop 所在主任务使用；HTTP/MQTT/推送 worker 不得另读 Serial1。
// RX 行缓冲属于 pollModem，事务开始/结束均不清空 RX 或未完成行。
static const size_t MODEM_RESPONSE_LIMIT = 2048;
static const unsigned long MODEM_RX_SYNC_MS = 20;  // 切代前排空重试预算；到期仍有未读 RX 则拒绝写入
static const unsigned long MODEM_RECOVERY_MS = 250;

enum class ModemResult { Pending, Ok, Error, Timeout, Busy, Overflow, Invalid };

void pollModem();
// 主任务注册非阻塞服务（MQTT 保活/仅入队回调），不能执行 Web 或嵌套 AT。
void setModemIdleHook(void (*hook)());
void modemDelay(unsigned long durationMs);  // 分流 URC、喂狗并运行受保护的等待期服务
bool modemBusy();

class ModemTransaction {
public:
  ModemTransaction();
  ~ModemTransaction();
  ModemTransaction(const ModemTransaction&) = delete;
  ModemTransaction& operator=(const ModemTransaction&) = delete;

  bool acquired() const { return owns_; }
  bool begin(const char* command, const char* requiredPrefix = nullptr, bool expectPrompt = false);
  bool waitForPrompt(unsigned long timeoutMs);
  bool submitPdu(const char* hexPdu);  // 私有快照，纯十六进制；本方法只发送一次 Ctrl+Z
  ModemResult waitForFinal(unsigned long timeoutMs);  // 完整 OK 行 AND requiredPrefix 行，顺序不限
  void end();  // 析构自动调用；未完成输入发送 ESC 并有界收尾，期间仍分流 URC

  ModemResult result() const { return result_; }
  const String& response() const { return response_; }
  const String& resultLine() const { return resultLine_; }

private:
  friend void pollModem();
  bool synchronizeRx();
  void acceptLine(const String& line);
  void acceptPrompt();
  void fail(ModemResult result, const char* reason);

  bool owns_ = false;
  bool started_ = false;
  bool expectPrompt_ = false;
  bool gotPrompt_ = false;
  bool inputPending_ = false;
  bool sawOK_ = false;
  uint32_t generation_ = 0;
  ModemResult result_ = ModemResult::Pending;
  String command_;
  String requiredPrefix_;
  String response_;
  String resultLine_;
};

// 查询/Web/MQTT/timer 共用。AT+MPING= 自动要求 +MPING: 和最终 OK 都到达。
// 返回值是事务状态，不用响应子串猜测成功；输出缓冲始终有界。
ModemResult runModemCommand(const char* command, unsigned long timeoutMs,
                            String* response = nullptr, String* resultLine = nullptr,
                            const char* requiredPrefix = nullptr);

#endif // MODEM_HANDLER_H
