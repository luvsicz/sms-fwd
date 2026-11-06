# 编译产物与刷写（arduino-cli / Arduino IDE on Windows）

面向“我要自己编出能烧的 bin，并在 Windows 上用 Arduino IDE 刷进去”的场景。
分区布局、OTA 原理见 [OTA 说明](OTA.md)；容量测量口径见 [编译验证记录](BUILD_VALIDATION.md)。

## 0. 三句话结论

1. 产物是**四个文件**（bootloader / partitions / boot_app0 / app），分别烧到 `0x0` / `0x8000` / `0xe000` / `0x10000`。
2. **Partition Scheme 必须选 `Custom`**（用仓库里的 `code/partitions.csv`，双 app 槽）；选错就等于把设备打回单槽，OTA 立刻失效。
3. `code.ino.merged.bin` 是 4MB 整片镜像，**任何情况下都别烧**——它会把 NVS 配置和 SPIFFS 短信历史一起覆盖。

## 1. 环境基线（可复现）

| 项 | 版本 |
|---|---|
| arduino-cli | 1.5.2（Arduino IDE 2.x 同样可以） |
| ESP32 Arduino Core | **3.3.10**（3.3.12 已发布，但本仓库的尺寸/余量是按 3.3.10 测的） |
| 库 | pdulib 0.5.11、PubSubClient 2.8、ReadyMail 0.4.2 |
| 板子 | ESP32C3（MakerGO ESP32 C3 SuperMini 或兼容板），4MB Flash |
| FQBN | `esp32:esp32:esp32c3:PartitionScheme=custom,FlashMode=dio,FlashFreq=80,FlashSize=4M` |

Boards Manager 地址：`https://espressif.github.io/arduino-esp32/package_esp32_index.json`

> Core 版本会直接改变镜像大小（3.3.10 → 3.3.12 差几 KB 到十几 KB）。要复现本文的数字就钉 3.3.10；
> 只求能烧则任意 3.3.x 都行，但必须重新看一遍“余量”是否还够（单槽 1,769,472 字节）。

## 2. 编译

### 2.1 脚本（推荐，Linux/macOS）

```bash
ARDUINO_CLI=/path/to/arduino-cli BUILD_PATH=build/tmp scripts/build-firmware.sh
```

它会：编译 → 从 ESP32 core 里捞出 `boot_app0.bin` → 把四个可烧文件 + `SHA256SUMS.txt` + `flash.sh`
汇总到 `build/firmware-<时间戳>/`，并检查 app 镜像是否超出单槽容量（超了直接报错退出）。

`BUILD_PATH` 复用编译缓存，第二次起只要几秒；不传则每次全新编译（慢，但更干净）。

### 2.2 arduino-cli 手敲

```bash
arduino-cli compile \
  --fqbn 'esp32:esp32:esp32c3:PartitionScheme=custom,FlashMode=dio,FlashFreq=80,FlashSize=4M' \
  --output-dir build/firmware code
```

`--output-dir` 里会出现：`code.ino.bin`、`code.ino.bootloader.bin`、`code.ino.partitions.bin`、
`code.ino.merged.bin`、`code.ino.elf`、`code.ino.map`。
**`boot_app0.bin` 不在里面**，要从 core 里取：
`~/.arduino15/packages/esp32/hardware/esp32/3.3.10/tools/partitions/boot_app0.bin`
（Windows：`C:\Users\<你>\AppData\Local\Arduino15\packages\esp32\hardware\esp32\3.3.10\tools\partitions\boot_app0.bin`）

### 2.3 Arduino IDE（Windows）

- 打开 `code/code.ino`（整个 `code/` 目录就是 sketch 目录，`partitions.csv` 必须和 `.ino` 同级）
- **Sketch → Export Compiled Binary** 会额外在 sketch 目录下生成 `code/build/`，里面的 bin 可以用，
  但仓库里已有的 `code/build/` 是**旧产物**，别直接拿来烧——先 Export 覆盖一次再说。

## 3. 产物清单（2026-11-06 实测，core 3.3.10）

| 文件 | 大小（字节） | 烧到 | 说明 |
|---|---:|---|---|
| `code.ino.bootloader.bin` | 18,672 | `0x0` | 二级引导 |
| `code.ino.partitions.bin` | 3,072 | `0x8000` | 双槽分区表 |
| `boot_app0.bin` | 8,192 | `0xe000` | 把 otadata 重置为“从 app0 启动”，**不在 output-dir** |
| `code.ino.bin` | **1,548,032** | `0x10000` | 应用镜像，单槽上限 1,769,472（余量 221,440） |
| `code.ino.merged.bin` | 4,194,304 | — | **禁止烧**，整片覆盖 NVS + SPIFFS |
| `code.ino.elf` / `.map` | — | — | 只用于调试与体积分析 |

- 编译日志里的 `Sketch uses 1547881 bytes` 和 bin 的 `1548032` 是**两个口径**（bin 含对齐/校验尾部）。
  判断装不装得下要看 **bin 长度 vs 1,769,472**，别看 `Sketch uses`。
- 本轮 SHA256（前 16 位，可用于比对产物是否同一份）：
  `code.ino.bin` = `41ed6b750282021b`，`bootloader` = `aea44226a71db48d`，
  `partitions` = `295240ef93436f57`，`boot_app0` = `f94c5d786a7a8fab`。

## 4. Windows + Arduino IDE 刷写（逐项）

### 4.1 选项清单

| IDE 选项 | 必须值 | 说明 |
|---|---|---|
| Board | `ESP32C3 Dev Module`（或 MakerGO ESP32 C3 SuperMini） | 选错会编出别的芯片的镜像 |
| **Partition Scheme** | **`Custom`** | 只在 sketch 目录有 `partitions.csv` 时出现；选别的 = 单槽，OTA 失效 |
| Flash Size | `4MB (32Mb)` | 板上就是 4MB；选 8MB 会写出板子装不下的布局 |
| Flash Mode | `DIO` | QIO 在部分 SuperMini 板子上会启动失败 |
| Flash Frequency | `80MHz` | 与烧录参数保持一致 |
| **USB CDC On Boot** | 看你要不要串口日志 | 默认 `Disabled`：`Serial` 走 UART0（GPIO20/21），**USB 串口监视器一片空白** |
| Upload Speed | `921600` | 连不上就降到 `115200` |
| Erase All Flash Before Sketch Upload | **`Disabled`** | 开了会清掉 NVS（WiFi/MQTT/推送配置）+ SPIFFS（短信历史） |
| Port | `USB JTAG/serial debug unit (COMx)` | 原生 USB，Win10/11 免驱 |

关于 `USB CDC On Boot`：本固件用 `Serial` 打日志，还支持在串口里敲调试命令（`TESTPUSH` 模拟一条短信走完整推送链路，
或单条普通 AT）。这些只有 `Enabled` 时才走 USB；`Disabled` 时它们跑到 UART0 引脚上，USB 端什么都看不到。
**线上现有固件是 `Disabled` 编的**（所以 `cat /dev/ttyACM0` 全空），要串口调试就带 `CDCOnBoot=cdc` 重新编：

```bash
arduino-cli compile --fqbn 'esp32:esp32:esp32c3:PartitionScheme=custom,CDCOnBoot=cdc,FlashMode=dio,FlashFreq=80,FlashSize=4M' --output-dir build/fw-debug code
```

### 4.2 常见报错与处置

| 现象 | 原因 / 处置 |
|---|---|
| 设备管理器有 `USB JTAG/serial debug unit`，IDE 端口列表里没有 | 换数据线（很多 Type-C 线只有电源芯）；换 USB 2.0 口；Win7 需装驱动，Win10/11 免驱 |
| `Failed to connect to ESP32-C3: No serial data received` | 手动进下载模式：按住 **BOOT** → 点一下 **RST/EN** → 松开 BOOT，再点上传；仍失败把 Upload Speed 降到 115200 |
| `Wrong boot mode detected (0x1)` / `Serial data received is not valid` | 同上（芯片没进下载模式） |
| `could not open port COMx: Access is denied` | 串口监视器/putty/另一个 IDE 窗口还占着口，全关掉再传 |
| 上传到一半报错，设备还在跑旧固件 | 重新上传；原生 USB 偶发，降速 + 换线最有效 |
| 出现两个 COM 口 | `CDC On Boot=Enabled` 时会多一个“应用 CDC”口：**烧录用 JTAG 那个**，敲 `TESTPUSH`/看日志用另一个 |
| 编译报找不到头文件（ReadyMail/pdulib/PubSubClient） | 库没装或装错版本，见 §1 |
| 路径含中文/空格/超长导致莫名错误 | 把仓库挪到 `C:\fw\ML307A-MSG-FORWARD` 这类纯英文短路径 |
| 上传卡在 `Connecting...` 不动 | 杀软/EDR 拦 `esptool.exe`，加白名单 |
| 上传成功但 WiFi 连不上 | 改 `code/wifi_config.h` 的默认 SSID/密码（首次必须能连上一个网） |

### 4.3 三个会“静默毁掉设备状态”的坑

1. **分区表选错**（`Huge App` / 默认 4MB with spiffs）：会把分区表写回单槽，`/otainfo` 变 404，
   OTA 全部拒绝，而且 SPIFFS 偏移变化 → 历史读不出来。改回来必须再用 `Custom` 刷一次。
2. **勾了 Erase All Flash**：NVS 里的 WiFi（最多 3 组）、MQTT、推送通道、统计全部清空。
   设备会退回 AP 模式 `SMS-Forwarder-AP`，要重新配网。
3. **烧了 merged.bin**：同上，且 SPIFFS 一起没了。

### 4.4 从单槽切到双槽之后的一次性善后

- 第一次以双槽启动时 SPIFFS 会被**重新格式化**，短信/来电历史清空（NVS 配置保留）。
  刷之前先在网页 `/history` 导一份。
- 如果刷完 `/history` 一直返回 `503 历史读取失败`：新 spiffs 区（`0x370000`）落进了旧 spiffs 区的中间，
  mount 到了脏数据。擦掉这块让它重新格式化即可（**会清空历史**）：

```bash
# Linux / macOS
python -m esptool --chip esp32c3 -p /dev/ttyACM0 erase-region 0x370000 0x80000
# Windows（esptool 独立 exe）
esptool.exe --chip esp32c3 -p COM5 erase-region 0x370000 0x80000
```

## 5. 刷完验证（三条就够）

1. 网页 `http://<设备IP>/` 返回 200（能打开面板）
2. `http://<设备IP>/otainfo` → `"dualSlot":true`，且 `running` 是 `app0` 或 `app1`（旧单槽固件这里 404）
3. `http://<设备IP>/query?type=network` → 已注册、数据连接已激活；`/stats` 里 `calls/pushOk/boots` 沿用旧值 = NVS 没丢

之后升级走网页 OTA / URL / MQTT 触发，不必再插线，见 [OTA 说明](OTA.md)。

## 6. 安全提醒

- 编译产物里嵌着 `code/wifi_config.h` 的默认 SSID 和密码，**不要把 bin、elf、map 提交到仓库或发给别人**。
- 网页端默认口令 `admin/admin123`，同一局域网任何人都能发短信、跑 AT 指令，刷完请改掉。
