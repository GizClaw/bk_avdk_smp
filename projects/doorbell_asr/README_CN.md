# BK7258 门铃 ASR 示例工程

* [English](./README.md)

## 1. 项目概述

`doorbell_asr` 是 BK7258 AP/CP 双核智能门铃示例，在门铃音视频业务基础上集成
Wanson ASR。工程支持：

- Wanson 离线语音识别；
- BLE 配网；
- 局域网 UDP/TCP 门铃服务；
- CS2 P2P 门铃服务；
- 板载或 UAC 音频输入输出及语音对讲；
- LCD 显示和多媒体处理框架。

当前配置中，ASR 在 AP 上运行并在系统启动时自动开启；Wi-Fi 和 BLE Controller
运行在 CP，BLE Host、门铃业务和多媒体服务运行在 AP。

## 2. 硬件要求

- BK7258 开发板；
- 8 MB PSRAM；
- UAC 麦克风（当前 ASR 默认使用 UAC 麦克风）；
- Wi-Fi 和 BLE 天线；
- CP UART0 串口，波特率 115200；
- 可选的扬声器、UVC/DVP 摄像头和 LCD。

具体 GPIO 配置见：

- `ap/config/bk7258_ap/usr_gpio_cfg.h`
- `cp/config/bk7258/usr_gpio_cfg.h`

## 3. 工程结构

```text
doorbell_asr/
├── ap/
│   ├── ap_main.c
│   ├── src/                         # 门铃、配网、网络、音视频和 ASR 逻辑
│   ├── include/
│   └── config/bk7258_ap/
├── cp/
│   ├── cp_main.c
│   ├── customer_msg/
│   ├── doorbell_service/
│   └── config/bk7258/
├── partitions/bk7258/
├── app.rst
├── CMakeLists.txt
└── Makefile
```

## 4. 编译与烧录

在 SDK 根目录执行：

```bash
make bk7258 PROJECT=doorbell_asr
```

也可以使用 Docker 编译：

```bash
./dbuild.sh make bk7258 PROJECT=doorbell_asr
```

生成的烧录文件位于：

```text
build/bk7258/doorbell_asr/package/all-app.bin
```

使用 BK7258 烧录工具将 `all-app.bin` 烧录到开发板。上电后通过 CP UART0
（115200）查看 CP 和 AP 日志。

## 5. 启动流程

1. CP 启动 Wi-Fi、BLE Controller 和 AP。
2. AP 初始化媒体服务、BLE 配网和门铃业务。
3. AP 自动调用 `doorbell_asr_turn_on()` 启动 ASR。
4. 完成 BLE 配网后，可根据客户端请求启动 UDP、TCP 或 CS2 服务。

本工程没有注册专用串口 CLI，门铃业务主要通过 BLE 配网协议和网络命令协议控制。

## 6. ASR 功能

当前配置使用：

- `CONFIG_ASR_SERVICE_WITH_MIC=y`
- `CONFIG_WANSON_ASR=y`
- `CONFIG_WANSON_NEW_LIB_SEG=y`

ASR 默认参数为 UAC 麦克风、8 kHz 输入、关闭 AEC，并重采样到识别引擎使用的
16 kHz。系统启动后 ASR 自动进入监听状态。识别成功时串口输出类似：

```text
asr_result : 按门铃
```

可用词条由 Wanson 词表决定，词表位于：

```text
ap/components/bk_thirdparty/asr/wanson/bk7258/fst_words.h
```

远程客户端打开摄像头或音频时，工程会停止 ASR；关闭相关业务或客户端断开后，
工程会重新启动 ASR，避免音频资源冲突。

## 7. BLE 配网与服务

设备 BLE 广播名称格式为 `doorbell_XXXXXX`。主要配网操作包括：

- 配置 Wi-Fi STA 的 SSID 和密码；
- 启动局域网 UDP 服务；
- 启动局域网 TCP 服务；
- 配置 CS2 DID、License、Key 和 Init String；
- 启动 CS2 P2P 服务；
- 关闭 BLE。

当前工程未启用 SoftAP 配网，应使用 BLE 配网。

同一时间只运行一种门铃传输服务。局域网服务使用以下端口：

| 用途 | 端口 |
| --- | ---: |
| SDP 本地端口 | 10000 |
| SDP 远端端口 | 52110 |
| 命令通道 | 7100 |
| UDP 图像 | 7180 |
| UDP 音频 | 7170 |
| TCP 图像 | 7150 |
| TCP 音频 | 7140 |

CS2 服务需要有效的 CS2 凭据和对应客户端，相关账号及凭据不包含在本工程中。

## 8. 基本测试

### 8.1 ASR

1. 烧录并启动工程。
2. 确认 CP UART0 输出 AP 启动及 ASR 初始化日志。
3. 通过 UAC 麦克风说出 Wanson 词表中的指令。
4. 确认串口输出 `asr_result` 日志。

### 8.2 BLE 配网和局域网服务

1. 使用支持本工程配网协议的客户端连接 `doorbell_XXXXXX`。
2. 写入 Wi-Fi SSID 和密码，等待设备获得 IP。
3. 选择启动 UDP 或 TCP 服务。
4. 客户端连接命令端口 7100，验证门铃控制和音频传输。
5. 验证远程音频开启时 ASR 停止、音频关闭后 ASR 恢复。

## 9. 关键配置

主要配置位于 `ap/config/bk7258_ap/config` 和 `cp/config/bk7258/config`：

- `CONFIG_INTEGRATION_DOORBELL`
- `CONFIG_INTEGRATION_DOORBELL_CS2`
- `CONFIG_ASR_SERVICE`
- `CONFIG_ASR_SERVICE_WITH_MIC`
- `CONFIG_WANSON_ASR`
- `CONFIG_VOICE_SERVICE`
- `CONFIG_CS2_P2P_SERVER`
- `CONFIG_BLUETOOTH_HOST_ONLY`
- `CONFIG_BTDM_CONTROLLER_ONLY`

## 10. 已知限制

- 摄像头打开和关闭的主体代码当前被条件编译禁用，摄像头功能需进一步适配。
- SoftAP 配网未启用。
- ASR 默认固定使用 UAC 麦克风和 8 kHz 输入；切换到板载麦克风需要修改工程配置或代码。
- ASR 识别后自动打开摄像头的调用当前被注释。
- CS2 凭据、配套手机应用和局域网测试客户端不随工程提供。
