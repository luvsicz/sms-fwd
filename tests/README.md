# 离线测试

十个现有 `test_*.ps1` 入口用于小型宿主机回归，不连接设备、不下载依赖、不烧录。测试读取生产源码，在独立临时目录中编译/生成夹具，结束时清理；不更新 `code/build/`。`tests/` 和仓库 Markdown 均在 Arduino sketch `code/` 外，不会编入或烧入固件。

## 依赖与运行

- Windows、PowerShell 7（`pwsh`）；C++ 测试需支持 C++17 的**宿主** GCC/Clang，不能使用 ESP 交叉编译器。本机环境为 **GCC 16.2.0**；modem 测试要求 `unsigned long` 为 32 位，LP64 宿主不适用。
- SMS/modem 需已安装的 **pdulib 0.5.11** 源码；MQTT 需 **PubSubClient 2.8** 源码（脚本校验版本）。默认从 `$HOME/.arduinoIDE/arduino-cli.yaml` 定位库目录；可分别传 `-PduLibPath`（`src` 目录）、`-PubSubClientPath`（库根目录或 `src`）。
- DOM/Web 及默认 config/timer 测试需已安装的 **Chrome/Edge**；自动查找常用安装位置，也可传 `-BrowserPath`。无需浏览器下载器、Web 服务或真实网络服务。
- 编译器可用 `-CompilerPath` 指定；SMS/modem/MQTT/JSON/Web/config 使用 `SMS_TEST_CXX`，两个 push 宿主测试使用 `PUSH_TEST_CXX`，否则查找 PATH/已有本地工具链。MQTT 库路径也可用 `MQTT_TEST_PUBSUBCLIENT`。

在 PowerShell 7 中逐项运行十个入口，任一非零退出即停止；路径按实际工作区调整：

```powershell
$ErrorActionPreference = 'Stop'
$repo = 'C:\Users\liu_r\IdeaProjects\msg_forward'
$entries = @(
    'test_config_timer.ps1'
    'test_history_dom.ps1'
    'test_json_handlers.ps1'
    'test_modem_regression.ps1'
    'test_mqtt_regression.ps1'
    'test_push_contracts.ps1'
    'test_push_host.ps1'
    'test_push_snapshot_recovery.ps1'
    'test_sms_regression.ps1'
    'test_web_regression.ps1'
)
foreach ($entry in $entries) {
    & pwsh -NoProfile -File (Join-Path $repo "tests/$entry")
    $testExit = $LASTEXITCODE
    if ($testExit -ne 0) { throw "${entry}: exit $testExit" }
}
```

需自定义路径时，可单独用 `pwsh -NoProfile -File <入口路径>` 加该入口支持的参数；不要将所有入口视为接受相同参数。默认未启用 ASan/UBSan，C++ 入口的 `-Sanitize` 仅适用于支持它的已安装工具链；`test_config_timer.ps1 -SkipBrowser` 会减少覆盖，不能算完整默认运行。

## 十个入口与覆盖

| 入口（相对 `tests/`） | 主要覆盖 | 附加依赖 |
|---|---|---|
| `test_config_timer.ps1` | 实际配置/定时逻辑、内存持久化、64 位时钟、历史到 DOM（默认） | C++、浏览器 |
| `test_history_dom.ps1` | 历史页真实 DOM、文本安全、桌面/移动交互 | 浏览器 |
| `test_json_handlers.ps1` | JsonInput、提取的 Web/MQTT/config 处理器、保存失败 | C++ |
| `test_modem_regression.ps1` | 实际 modem/SMS 事务、URC、分片/恢复、32 位回绕 | C++、pdulib |
| `test_mqtt_regression.ps1` | 实际 PubSubClient 编码/keepalive、MQTT/HA/LWT 契约 | C++、PubSubClient 2.8 |
| `test_push_contracts.ps1` | 邮件快照/所有权、TLS 生命周期的源码约束（不执行固件） | 无 |
| `test_push_host.ps1` | 实际入队/worker、SMTP/HTTP 控制流，API 为替身 | C++ |
| `test_push_snapshot_recovery.ps1` | 邮件快照失败、资源回收和后续恢复 | C++ |
| `test_sms_regression.ps1` | 实际 SMS 源码与 pdulib 收发/拼接回归 | C++、pdulib |
| `test_web_regression.ps1` | 实际根页面/后端响应到离线浏览器，STA/AP、桌面/移动 | C++、浏览器 |

`sms_host_runner.ps1` 是复用辅助文件，不是第十一个入口；config/timer 和 Web 入口会复用其他测试。SMS/modem 细节见 [MODEM_REGRESSION.md](MODEM_REGRESSION.md)。

## 结果与边界

2026-11-04 十个默认入口全量重跑均退出 0、零失败；逐项断言/场景数及修复前失败记录见 [TODO.md](../TODO.md)。结果针对该次源码快照，后续修改应重新运行。完整固件的编译测量另见 [BUILD_VALIDATION.md](../docs/BUILD_VALIDATION.md)。

宿主测试包含实际源码/函数与部分真实库，但 String、FreeRTOS、NVS/SPIFFS、串口和多数传输均为替身；浏览器使用本地夹具/模拟请求。它们不证明实板 UART/SIM 时序、并发调度、运行时堆/TLS/ReadyMail 内部分配、真实 SMTP/HTTP 投递、broker LWT 投递、HA 界面或断电持久化可靠性，也不替代完整 Arduino 编译与硬件长稳验证。源码约束检查不是行为测试；未启用的 sanitizer、被跳过的浏览器和未执行的硬件验证不得计为通过。
