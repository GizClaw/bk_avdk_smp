.. _project_app:

BK7258 Doorbell ASR
=============================

Overview
-----------------------------

本工程是 BK7258 AP/CP 双核智能门铃示例，集成 Wanson ASR、BLE 配网、
LAN UDP/TCP 服务、CS2 P2P 服务、语音对讲和多媒体框架。

默认配置下，Wi-Fi 和 BLE Controller 运行在 CP；BLE Host、门铃业务、
多媒体服务和 ASR 运行在 AP。ASR 在系统启动时自动开启。

完整说明见工程目录中的 ``README_CN.md`` 和 ``README.md``。

Hardware Requirements
------------------------------

- BK7258 开发板；
- 8 MB PSRAM；
- UAC 麦克风；
- Wi-Fi 和 BLE 天线；
- CP UART0 串口，波特率 115200；
- 可选的扬声器、UVC/DVP 摄像头和 LCD。

Configure and Build
-----------------------------

Configure the Project
****************************

AP 和 CP 的默认配置分别位于：

::

    ap/config/bk7258_ap/config
    cp/config/bk7258/config

关键功能配置包括：

- ``CONFIG_INTEGRATION_DOORBELL``
- ``CONFIG_INTEGRATION_DOORBELL_CS2``
- ``CONFIG_ASR_SERVICE_WITH_MIC``
- ``CONFIG_WANSON_ASR``
- ``CONFIG_VOICE_SERVICE``
- ``CONFIG_CS2_P2P_SERVER``

Build the Project
****************************

在 SDK 根目录执行：

::

    make bk7258 PROJECT=doorbell_asr

也可以使用 Docker 编译：

::

    ./dbuild.sh make bk7258 PROJECT=doorbell_asr

Flash
****************************

烧录文件位于：

::

    build/bk7258/doorbell_asr/package/all-app.bin

使用 BK7258 烧录工具烧录 ``all-app.bin``，然后连接 CP UART0（115200）
查看启动日志。

Running and Output
------------------------------

Operate
*****************************

工程上电后自动初始化媒体服务、BLE 配网、门铃业务和 ASR，不需要专用 CLI。

ASR 默认使用 UAC 麦克风采集 8 kHz 音频，并重采样到 16 kHz。可用识别词由
Wanson 词表定义。远程摄像头或音频业务开启时 ASR 会停止，业务关闭或客户端
断开后 ASR 会恢复。

设备 BLE 广播名称格式为 ``doorbell_XXXXXX``。使用兼容客户端完成 Wi-Fi
STA 配网后，可启动以下任一服务：

- LAN UDP；
- LAN TCP；
- CS2 P2P。

LAN 命令端口为 7100，UDP 图像/音频端口为 7180/7170，TCP 图像/音频端口为
7150/7140。同一时间只能运行一种门铃传输服务。

Output
*****************************

ASR 识别成功时会输出类似日志：

::

    asr_result : 按门铃

.. note::

    当前工程未启用 SoftAP 配网。摄像头打开和关闭的主体实现被条件编译禁用，
    需要进一步适配。CS2 凭据及配套客户端不随工程提供。
