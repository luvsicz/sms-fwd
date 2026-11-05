# 编译验证（2026-11-05）

本记录为已确认的**纯编译**测量：未烧录、未擦除、未修改设备分区，也未更新仓库旧 `code/build/` 产物；不代表实板运行或长稳验证。

## 本次 Bark / 电信补修（2026-11-05）

- 在原环境 **Arduino CLI 1.5.1 / ESP32 Core 3.3.10 / pdulib 0.5.11 / PubSubClient 2.8 / ReadyMail 0.4.2** 执行完整 `--clean --warnings all --jobs 4` 编译，退出 **0**；IDE 构建检查也通过。
- FQBN 保持 `esp32:esp32:esp32c3:PartitionScheme=huge_app,FlashMode=dio,FlashFreq=80,FlashSize=4M`，未改分区或任务栈。**22 个已跟踪 sketch 源文件哈希与编译前记录完全一致**。

| 指标（字节） | 上轮 `2dc3511` 对应测量 | 本次实测 | 差值 |
|---|---:|---:|---:|
| 程序统计 | 1,531,307 | 1,532,369 | +1,062 |
| 应用镜像 `code.ino.bin` | 1,531,456 | 1,532,528 | +1,072 |
| 静态 RAM | 54,132 | 54,132 | 0 |

本次应用分区仍为 **3,145,728 字节**，镜像余量 **1,613,200 字节（约 1.54 MiB）**。测试和文档均在 sketch 外，不烧入固件；运行时堆/TLS 栈占用未因静态 RAM 不变而获得实机验证。构建记录 **51 条 warning**（包含既有源码/库警告），不是零警告构建。

本机私有证据目录：`C:\Users\liu_r\AppData\Local\Temp\msg-forward-bark-af40ab4571ba450dbc8fff4bec6cc515`；`compile.exit` 为 0，`compile.log`、`source-before-build.json`、`build/partitions.csv` 和 `firmware/code.ino.bin` 分别保留编译、输入哈希、分区和镜像证据。**产物可能含 WiFi 配置，不公开或提交；未连接 COM3，未发送真实通知。** 当前测试结果见 [TODO.md](../TODO.md) 的 2026-11-05 补修记录。

## 上轮环境与测量（2026-11-04 历史记录）

- Arduino CLI **1.5.1**；ESP32 Arduino Core **3.3.10**。
- pdulib **0.5.11**、PubSubClient **2.8**、ReadyMail **0.4.2**。
- FQBN：`esp32:esp32:esp32c3:PartitionScheme=huge_app,FlashMode=dio,FlashFreq=80,FlashSize=4M`；有效 Flash 参数：**DIO / 80MHz / 4MB**。最终两组均以 `--clean --warnings all --jobs 4` 和同一完整 `--fqbn` 实跑成功。
- 基线为 `HEAD 49ab84c`，与当前 `precommit/code/` snapshot 使用相同环境；当前 snapshot 的 **22 个源码文件 SHA256 在最终编译及十组测试执行时与工作区全一致**，见下述 `precommit/` 证据目录中的 `source-hashes.json`。此后仅删除 EOF 额外空行，部分文件不再原始字节相等；主代理已核对提交源码与快照在统一 CRLF/LF 并去除尾部换行后，22 个文件主体全部相同。

以下单位均为字节；`app.bin` 指应用镜像 `code.ino.bin`，不是整片 merged 镜像。

| 输入 | 程序 / 上限 | 静态 RAM / 上限 | app.bin 长度 | compile 退出码 |
|---|---:|---:|---:|---:|
| 基线 `49ab84c` | 1,507,563 / 3,145,728 | 52,156 / 327,680 | 1,507,712 | 0 |
| 当前 `precommit` snapshot | 1,531,307 / 3,145,728 | 54,132 / 327,680 | 1,531,456 | 0 |

相对基线，程序增加 **23,744** 字节，静态 RAM 增加 **1,976** 字节。当前静态剩余 **273,548** 字节**不是运行时可用堆**；任务栈、动态分配及 TLS 开销仍需实板验证。程序统计与 bin 文件长度是不同口径。构建存在既有 format、未用变量及 Core 警告，**不是零警告构建**。

本机最终证据目录：`C:\Users\liu_r\AppData\Local\Temp\msg-forward-final-2b9585fc03b741b19484331019b92b9a\precommit`，其中 `precommit-build/` 为最终构建目录，`precommit-firmware/` 为最终产物目录；`baseline-compile.exit` 与 `precommit-compile.exit` 均为 **0**。旧 `verified` 构建及日志保留作前一轮记录，最终大小/哈希以 `precommit/` 为准。中间文件/固件含 WiFi 秘密，日志也须按敏感材料处理；**不公开、不提交或附加这些二进制/原始材料**。临时目录不是发布附件。

## 容量与升级风险

本机 Huge App 分区表的关键项：

| 分区 | offset | size |
|---|---|---|
| app0 | `0x10000` | `0x300000`（3,145,728 字节） |
| SPIFFS | `0x310000` | `0xE0000`（917,504 字节） |

最终 app.bin 的分区余量为 **1,614,272 字节**。只有一个 app 分区，**无双槽 OTA**。本机 `default.csv` 的单 app 槽为 `0x140000`（1,310,720 字节），装不下当前固件。实板需至少 **4MB 物理 Flash**，不能靠改菜单把物理容量变大；编译成功不证明板上容量/布局兼容。

切换分区或烧录整片 merged 镜像可能覆盖 NVS 配置和 SPIFFS 历史；操作前先备份配置、短信/来电历史并核对板上布局，默认不要 `erase-all`。不要把旧 `code/build/` 当作本轮新固件，建议按 [README 烧录步骤](../README.md#烧录步骤) 在 IDE 中重新编译后上传；本文命令不上传。

## 可复制的 PowerShell 7 纯编译步骤

预先安装上述版本；按需修改项目、CLI 和 IDE 配置路径（以下为本机布局）。命令不下载/升级依赖，也不指定串口；只将编译输出写入新的独立临时目录，不使用 `--upload` 或 `--export-binaries`。完整日志仅保留本地，不直接打印。

```powershell
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$repo = 'C:\Users\liu_r\IdeaProjects\msg_forward'
$cli = Join-Path $env:LOCALAPPDATA 'Programs/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe'
$cliConfig = Join-Path $HOME '.arduinoIDE/arduino-cli.yaml'
foreach ($path in @($cli, $cliConfig, (Join-Path $repo 'code/code.ino'))) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing file: $path" }
}
$root = Join-Path ([IO.Path]::GetTempPath()) ('msg-forward-compile-' + [guid]::NewGuid().ToString('N'))
$build = Join-Path $root 'build'
$out = Join-Path $root 'firmware'
$null = New-Item -ItemType Directory -Path $build, $out
$log = Join-Path $root 'compile.log'
$compileArgs = @(
    '--config-file', $cliConfig, 'compile'
    '--fqbn', 'esp32:esp32:esp32c3:PartitionScheme=huge_app,FlashMode=dio,FlashFreq=80,FlashSize=4M'
    '--build-path', $build, '--output-dir', $out
    '--clean', '--warnings', 'all', '--jobs', '4'
    (Join-Path $repo 'code')
)
& $cli @compileArgs *> $log
$compileExit = $LASTEXITCODE
if ($compileExit -ne 0) { throw "compile exit ${compileExit}; inspect locally: $log" }
$app = Get-Item -LiteralPath (Join-Path $out 'code.ino.bin')
if ($app.Length -le 0 -or $app.Length -gt 0x300000) {
    throw "Invalid app image length: $($app.Length)"
}
Write-Output "compile exit $compileExit; log (private): $log"
$app | Select-Object FullName, Length
```

检查退出码为 0、应用镜像存在且长度不超过 app 槽；最终 `precommit` 测量 snapshot 的长度为 `1,531,456` 字节，源码/依赖/初始 WiFi 改变后应重新测量。新临时目录同样包含秘密，不公开产物。`tests/` 和仓库 Markdown 位于 sketch 外，不会编入/烧入固件。

离线入口与验证边界见 [tests/README.md](../tests/README.md)。2026-11-04 十个默认回归入口已在相同 `precommit` 源码快照上全部退出 0，见上述证据目录的 `regressions.json`；逐项结果见 [TODO.md](../TODO.md)。代码/离线验收 14/14，硬件验收仍为 0/14。
