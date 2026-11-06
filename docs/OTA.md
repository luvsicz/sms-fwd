# 固件在线升级（OTA）

本文说明如何让本固件支持**无线升级**：网页上传、从 URL 拉取、MQTT 远程触发，以及失败回滚。

## 1. 结论先说：为什么必须先用 USB 刷一次

当前仓库原先推荐的 `Huge App` 分区表只有一个 app 槽（`0x10000` / `0x300000`，3MiB），
没有第二个槽可以写入"另一份固件"。任何 OTA 方案都需要**双槽**（A/B）才能保证写入过程中
设备还能正常跑现有固件，所以：

- **一次性**：用 USB 刷入本仓库的 `code/partitions.csv`（双 app 槽）。
- **之后**：所有升级都可以在网页点一下，或者用 MQTT 触发，不再需要插线。

> 单槽固件升级本仓库新版本时，接口会明确返回 400/409 并说明原因，不会误写当前运行分区。

## 2. 双槽分区布局（4MB Flash）

| 分区 | 偏移 | 大小 | 说明 |
|---|---:|---:|---|
| nvs | `0x9000` | 20KB | 配置/统计，**与旧布局一致，升级不丢** |
| otadata | `0xe000` | 8KB | 记录当前启动槽 |
| app0 | `0x10000` | 1728KB | 1,769,472 字节 |
| app1 | `0x1C0000` | 1728KB | OTA 写入目标 |
| spiffs | `0x370000` | 512KB | 短信/来电历史（原 896KB） |
| coredump | `0x3F0000` | 64KB | 崩溃转储 |

容量实测（arduino-cli 1.5.2 + ESP32 Core 3.3.10，2026-11-06，固件 1.2.0）：

- app 镜像 `code.ino.bin` = **1,549,040** 字节
- 单槽容量 **1,769,472** 字节，余量 **220,432** 字节（已用 87.5%）
- 也就是说固件还能再长 ~215KB；超过就要么砍功能，要么换 8MB Flash 的板子

> SPIFFS 从 896KB 缩到 512KB。历史文件上限仍是 128KB + 30KB，加修剪临时文件约 300KB，够用但不再宽裕。

## 3. 一次性 USB 刷入

### 3.1 Arduino IDE

1. 开发板：`ESP32C3`（MakerGO ESP32 C3 SuperMini 或兼容板）
2. **Partition Scheme 选 `Custom`**。本仓库 `code/partitions.csv` 会被自动复制进构建目录；
   只要 sketch 目录里有这个文件，它会优先于菜单里的其它方案。
3. Flash Size `4MB`，Flash Mode `DIO`，Flash Frequency `80MHz`
4. 编译上传，串口 115200 观察启动日志

### 3.2 arduino-cli（Linux/macOS/Windows 通用）

```bash
arduino-cli compile \
  --fqbn 'esp32:esp32:esp32c3:PartitionScheme=custom,FlashMode=dio,FlashFreq=80,FlashSize=4M' \
  --output-dir build code

arduino-cli upload -p /dev/ttyACM0 \
  --fqbn 'esp32:esp32:esp32c3:PartitionScheme=custom,FlashMode=dio,FlashFreq=80,FlashSize=4M' \
  --input-dir build code
```

### 3.3 esptool 手动写入（保留 NVS 配置）

```bash
python -m esptool --chip esp32c3 -p /dev/ttyACM0 -b 460800 \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB \
  0x0     build/code.ino.bootloader.bin \
  0x8000  build/code.ino.partitions.bin \
  0xe000  <esp32-core>/tools/partitions/boot_app0.bin \
  0x10000 build/code.ino.bin
```

- `boot_app0.bin` 在 ESP32 Core 的 `tools/partitions/` 下，它把 otadata 重置为"从 app0 启动"。
- **不要**直接烧 `code.ino.merged.bin`：它是 4MB 整片镜像，会连 NVS 和 SPIFFS 一起覆盖，
  配置和历史全部丢失。
- 写分区表不会擦 NVS（`0x9000` 位置和大小都没变），所以 WiFi/MQTT/推送配置保留；
  SPIFFS 偏移从 `0x310000` 移到 `0x370000`，首次启动会重新格式化，**短信/来电历史会清空**，
  需要的话先导出。

### 3.4 刷完 `/history` 一直 503？擦一下 spiffs 区

新布局的 spiffs 区（`0x370000`）正好落在**旧 spiffs 区（`0x310000` 起 896KB）的中段**，那块是旧文件数据、
不是 SPIFFS 超级块。`SPIFFS.begin(true)` 有时没能干净格式化，表现为 `/history` 一直返回
`{"success":false,"message":"历史读取失败，请稍后重试"}`，`/sms.txt` 404。

判据（不用猜）：读 `0x370000` 起 512KB，SPIFFS 魔数 `0x20140501` 出现 **0** 次。

```bash
# 会清空短信/来电历史，先导 /history 再擦
esptool --chip esp32c3 -p /dev/ttyACM0 erase-region 0x370000 0x80000
# Windows: esptool.exe --chip esp32c3 -p COM5 erase-region 0x370000 0x80000
```

擦完重启，固件自动重新格式化，`/history` 恢复 200 且返回 `{"smsHistory":[],"callHistory":[]}`。

## 4. 之后怎么升级

### 4.1 网页（最常用）

`控制` 页 → `固件升级` 卡片 → 选择 `code.ino.bin` → `上传并升级`。

- 页面显示上传百分比，写完后设备自动重启（约 10~30 秒）
- 只接受 `.bin`，最大 1728KB
- 需要 Web 管理账号（Basic Auth）

### 4.2 从 URL 拉取（适合自己有文件服务器/HA）

```bash
curl -u admin:你的密码 -X POST \
  -H 'Content-Type: application/json' \
  -d '{"url":"http://192.168.31.5/fw/code.ino.bin"}' \
  http://sms.local/updateurl
```

仅支持 `http://`（HTTPS 需要额外 TLS 内存，本固件不做）。下载完自动写入并重启。

### 4.3 MQTT 远程触发（可在 HA 里做按钮/自动化）

主题：`<前缀>/<设备ID>/cmd`

```json
{"action":"ota","url":"http://192.168.31.5/fw/code.ino.bin"}
```

设备会先回一条日志事件，然后在主循环里下载、写入、重启。
MQTT 断开后设备重启是正常的（升级结束本来就重启）。

### 4.4 查询当前版本/分区

```bash
curl -u admin:你的密码 http://sms.local/otainfo
```

```json
{"success":true,"version":"1.2.0","running":"app1","slotBytes":1769472,
 "dualSlot":true,"otherSlotValid":true,"sketchBytes":1549040,
 "freeSketchBytes":1769472,"freeHeap":120696,"lastError":""}
```

`dualSlot:false` 说明还是单槽分区，需要先做第 3 步。

## 5. 失败回滚（固件层）

写入成功但新固件跑不起来时，设备会自己回到旧固件：

1. `Update.end()` 成功后，在 NVS `sms_ota` 里写 `trial=1, boots=0`，然后重启。
2. 新固件每次启动把 `boots+1`；只要启动满 15 秒且已经连上 WiFi 或开了热点，就把 `trial` 清掉（视为健康）。
3. 若连续 **3 次**启动都没能确认，就把启动分区切回另一个槽并重启（仅当另一槽首字节是 `0xE9`，
   即确实存在应用镜像时才切，避免切到空槽变砖）。

边界说明：

- Arduino 预编译 bootloader **没有**开启 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`，
  所以这是**固件层**回滚，不是 bootloader 级回滚。
- 只能兜住"启动早期崩溃/反复重启"。如果新固件能启动、能联网，但某个功能坏了，
  不会被自动回滚——这时候用网页再刷回旧版本即可（旧固件还在另一个槽里，但 OTA 是整包写入，
  所以请自己保留上一版 `.bin`）。
- 试用确认只看"能启动 + 能联网"，不校验 MQTT/短信链路。

## 6. 已知限制

- 升级期间主循环被占用（网页上传 10~30 秒），期间会继续分流模组 URC，但密集短信仍可能延迟。
- 只支持 `http://` 拉取；HTTPS 请先在本地起个 http 服务，或者用网页上传。
- 上传走的是 ESP32 WebServer 的 raw/multipart 流式回调，按 1436 字节块写 Flash，
  不在内存里缓存整包（设备可用堆只有 ~120KB）。
- 4MB Flash 下 app 槽 1728KB 是硬上限；固件接近这个体积时请重新评估分区表。

## 7. 验证记录

**编译（2026-11-06，固件 1.2.0 = 上游电信/Bark 修复 + 双槽 OTA）**

- arduino-cli 1.5.2 + ESP32 Core 3.3.10 + ReadyMail 0.4.2 / pdulib 0.5.11 / PubSubClient 2.8；
  FQBN `esp32:esp32:esp32c3:PartitionScheme=custom,FlashMode=dio,FlashFreq=80,FlashSize=4M`，退出码 0；
  程序 1,548,883 字节、`code.ino.bin` **1,549,040** 字节、静态 RAM 56,532 字节；
  分区表 `gen_esp32part.py` 校验为 app0/app1 双槽。

**实板（2026-11-06，同一台 .25 设备，USB 经 PVE PCI 直通到烧录机）**

- USB 刷入双槽：回读 `0x8000` 确认 `app0 1728KB / app1 1728KB / spiffs 512KB@0x370000`；
  刷前整片 4MB 已备份（回滚用）。
- 真实 OTA 往返：`POST /update`（`Content-Type: application/octet-stream`，1.5MB，约 16 秒）
  → `{"success":true}` → 重启后 `/otainfo` 报 `running:"app1", otherSlotValid:true, dualSlot:true`。
- 配置保留：`/stats` 的 `calls/pushOk/boots` 沿用旧计数；MQTT discovery 主题
  `homeassistant/sensor/sms_forwarder_7af874/...` 的 `sw_version` 随固件更新。

**未做**：断电/断网中断上传、`trial/boots` 自动回滚（第 5 节）的实测。要验收这两条请按第 8 节演练。

## 8. 首次上线建议演练

1. USB 刷入双槽固件，确认 `/otainfo` 返回 `dualSlot:true`。
2. 用同一份 `.bin` 做一次网页 OTA（内容相同，但会完整走一遍写入→重启）。
3. 重启后确认：`/stats` 的 `boots` 加一、配置仍在、`/otainfo` 的 `running` 变成 `app1`。
4. 故意传一个非固件文件（如把 txt 改名 .bin）验证失败提示，并确认设备仍正常。
5. 需要更严格的验证时，断电中断一次上传，确认设备重启后仍跑旧固件。
