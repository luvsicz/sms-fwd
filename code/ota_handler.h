#ifndef OTA_HANDLER_H
#define OTA_HANDLER_H

#include <Arduino.h>

// 在线升级（OTA）
// - 网页上传 app.bin：POST /update（原始二进制 body，边收边写 Flash，不在 RAM 缓存整包）
// - 从 URL 拉取：POST /updateurl {"url":"http://..."}，或 MQTT cmd {"action":"ota","url":"..."}
// - 需要双 app 槽分区表 code/partitions.csv（PartitionScheme=custom）；
//   单 app 槽（Huge App）固件必须先 USB 重刷一次分区表，之后才能无线升级。

bool otaDualSlotSupported();          // 当前分区表是否有可写的第二个 app 槽
void otaInit();                       // setup 早期调用：OTA 试用固件确认/自动回滚
void otaLoop();                       // loop 调用：延迟重启、待执行 URL 升级、试用确认
String otaInfoJson();                 // /otainfo 响应体
void handleOtaInfo();                 // GET /otainfo
void handleOtaUpload();               // POST /update 最终处理（结果回包）
void handleOtaUploadChunk();          // POST /update 请求体回调（raw/multipart 分流）
void handleOtaUrl();                  // POST /updateurl（JSON）
bool otaQueueUrl(const String& url);  // MQTT 调用：仅校验并入队，实际下载在 loop 中执行

#endif // OTA_HANDLER_H
