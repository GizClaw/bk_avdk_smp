# 博通集成 Armino SMP 开发框架

- [English](./README.md)

## 概述

**Armino SMP** 是 BEKEN 发布并开源的 AIoT 音视频与智能终端软件开发框架。框架提供包括 BLE、Wi-Fi、蓝牙、音视频编解码、LCD 显示、语音与多媒体、低功耗等基础能力以及对应的上层应用组件、示例工程。旨在帮助开发者在 BEKEN 芯片平台上快速开发落地产品应用。

## 文档

- [Armino SMP 在线文档](https://docs.bekencorp.com/arminodoc/bk_avdk_smp/smp_doc/bk7258/zh_CN/v3.1.1/index.html)

## 版本策略

`release/v3.1.1` 分支面向 **BK7258** 芯片。本框架采用“维护分支 + 发布标签”的版本管理方式：

- `release/v3.1.1` 为持续维护分支，用于该版本系列的功能迭代和问题修复，始终包含最新代码，但其中可能包含尚未完成完整发布测试的改动。
- `release/v3.1.1.x` 为正式发布标签，例如 `release/v3.1.1.1`、`release/v3.1.1.2`。每个标签均经过完整的测试与发布流程，可作为量产版本使用。
- 若同时使用基于本 SDK 的上层产品解决方案仓库，方案代码与 Armino SMP SDK 建议使用相同的标签版本。

建议您选择 `release/v3.1.1.x` 系列中版本号最大的标签进行开发和量产。仅需体验最新功能或参与开发时，才建议使用 `release/v3.1.1` 分支。

## 系统架构

BK7258 采用 **AP + CP** 双子系统架构：CP 以 CPU0 单核运行，负责连接与低功耗保活；AP 以 CPU1+CPU2 SMP 运行，负责多媒体与应用。两侧独立镜像，经 Mailbox / 共享内存协同后打包为 `all-app.bin`。

详细介绍请参阅 [AP(SMP)+CP 架构](https://docs.bekencorp.com/arminodoc/bk_avdk_smp/smp_doc/bk7258/zh_CN/v3.1.1/developer-guide/architecture/index.html)。

## 主要特性

- **异构双镜像**：AP / CP 独立组件树与中间件，按分区表打包为统一固件。
- **SMP 支持**：AP 为 CPU1+CPU2 双核 SMP；CP 为 CPU0 单核。
- **连接与低功耗**：Wi-Fi / BLE 协议栈与低功耗保活、电源管理主要由 CP 侧承载。
- **音视频与 GUI**：Camera / Display / 编解码、UVC、音频播放与服务框架及 LVGL 等能力由 AP 侧承载。
- **可配置构建**：Make + CMake + Kconfig / `menuconfig`，支持本地编译与 Docker 编译。
- **丰富示例**：`projects/` 提供默认应用、音视频与显示、LVGL、蓝牙、Wi-Fi、低功耗等工程。

## 获取代码

Armino SMP SDK 同步发布至 GitHub、Gitee 和 GitLab，可根据网络环境及访问权限选择代码源。

- GitHub：<https://github.com/bekencorp/bk_avdk_smp>
- Gitee：<https://gitee.com/bekencorp/bk_avdk_smp>
- GitLab：<https://gitlab.bekencorp.com/armino/bk_avdk_smp>

GitHub 和 Gitee 可公开访问。GitLab 仅面向企业客户开放；企业客户如需访问，请联系对接的 FAE 或销售人员申请开通权限。

**Windows 用户注意**：使用 Git for Windows 获取代码时，建议在克隆前关闭自动换行符转换，避免脚本或源文件被转换为 CRLF 而导致编译失败。Linux、macOS 和 WSL 环境无需执行。

```bash
git config --global core.autocrlf false
```

如果代码已经克隆，修改该配置不会自动恢复文件，建议设置后重新克隆。

以下命令以 GitHub 和 `release/v3.1.1` 分支为例。**实际获取代码时，请优先选择最新正式发布标签** `release/v3.1.1.x`。使用其他代码源时，替换对应的仓库地址即可。

```bash
mkdir -p ~/armino && cd ~/armino

git clone --branch release/v3.1.1 https://github.com/bekencorp/bk_avdk_smp.git
```

## 编译环境安装

Armino SMP 支持本地编译和 Docker 编译，可根据开发平台选择以下方式部署编译环境。

### Linux 本地编译

进入 SDK 目录并运行环境安装脚本：

```bash
# 安装脚本位于 bk_avdk_smp 仓库内
cd ~/armino/bk_avdk_smp
sudo bash tools/env_tools/setup/armino_env_setup.sh
```

### Windows 本地编译

下载并安装 [Armino Bash](https://dl.bekencorp.com/tools/arminosdk/WindowsInstaller/Armino-Bash-Setup_0.3.0.exe)。

### Docker 编译

Docker 编译镜像为 [bekencorp/armino-idk](https://hub.docker.com/r/bekencorp/armino-idk/tags)，请选择 `1.5` 或更高版本的镜像标签，支持 Windows / Linux / macOS。

详细安装步骤请参阅 [环境部署及编译](https://docs.bekencorp.com/arminodoc/bk_avdk_smp/smp_doc/bk7258/zh_CN/v3.1.1/get-started/index.html#smp-get-started-env)。

## 编译工程

以 `app` 工程为例（位于 `projects/app`），在 SDK 根目录本地编译：

```bash
cd ~/armino/bk_avdk_smp
make bk7258 PROJECT=app        # PROJECT 可选，默认为 app
```

也支持 Docker 编译（Linux / macOS 用 `./dbuild.sh`，Windows PowerShell 用 `.\dbuild.ps1`）：

```bash
cd ~/armino/bk_avdk_smp
./dbuild.sh make bk7258 PROJECT=app
```

编译成功后，用于烧录的固件文件位于以下路径（相对于 SDK 仓库根目录）：

```text
build/bk7258/app/package/all-app.bin
```

## 烧录固件

可选择以下任一方式烧录固件：

- 下载并使用 [BKFIL 本地烧录工具](https://dl.bekencorp.com/tools/bkfil/v4)
- 使用 [BKFIL 网页烧录工具](https://connect.aclsemi.com/)

烧录时请选择上一节编译生成的 `all-app.bin` 固件文件。

详细烧录流程请参阅 [烧录代码](https://docs.bekencorp.com/arminodoc/bk_avdk_smp/smp_doc/bk7258/zh_CN/v3.1.1/get-started/index.html#smp-get-started-flash)。
## 参考工程
以下列出参考工程，可点击查看工程详细说明。<!-- readme-projects:begin -->
<table>
<tbody>
<tr><td><a href="/projects/ap_powerdown_keepalive/">ap_powerdown_keepalive</a></td><td><a href="/projects/app/">app</a></td><td><a href="/projects/app_ab/">app_ab</a></td></tr>
<tr><td><a href="/projects/asr_service_example/">asr_service_example</a></td><td><a href="/projects/audio_player_example/">audio_player_example</a></td><td><a href="/projects/bluetooth/central/">central</a></td></tr>
<tr><td><a href="/projects/bluetooth/headset/">headset</a></td><td><a href="/projects/bluetooth/hfp_ag/">hfp_ag</a></td><td><a href="/projects/bluetooth/polar/">polar</a></td></tr>
<tr><td><a href="/projects/dma2d_example/">dma2d_example</a></td><td><a href="/projects/doorbell_asr/">doorbell_asr</a></td><td><a href="/projects/draw_osd_example/">draw_osd_example</a></td></tr>
<tr><td><a href="/projects/dvp_example/">dvp_example</a></td><td><a href="/projects/encoder_example/">encoder_example</a></td><td><a href="/projects/eth_example/">eth_example</a></td></tr>
<tr><td><a href="/projects/jpeg_decode_example/">jpeg_decode_example</a></td><td><a href="/projects/lvgl/86box/">86box</a></td><td><a href="/projects/lvgl/benchmark/">benchmark</a></td></tr>
<tr><td><a href="/projects/lvgl/benchmark_v9/">benchmark_v9</a></td><td><a href="/projects/lvgl/freetype_font/">freetype_font</a></td><td><a href="/projects/lvgl/img_decode/">img_decode</a></td></tr>
<tr><td><a href="/projects/lvgl/music/">music</a></td><td><a href="/projects/lvgl/widgets/">widgets</a></td><td><a href="/projects/lvgl/widgets_v9/">widgets_v9</a></td></tr>
<tr><td><a href="/projects/lwip_psram_example/">lwip_psram_example</a></td><td><a href="/projects/mcu_lcd_example/">mcu_lcd_example</a></td><td><a href="/projects/player_service_example/">player_service_example</a></td></tr>
<tr><td><a href="/projects/pm_ap_powerdown/">pm_ap_powerdown</a></td><td><a href="/projects/qspi_lcd_example/">qspi_lcd_example</a></td><td><a href="/projects/rgb_lcd_example/">rgb_lcd_example</a></td></tr>
<tr><td><a href="/projects/secureboot_xip/">secureboot_xip</a></td><td><a href="/projects/secureboot_xip_no_encrypt/">secureboot_xip_no_encrypt</a></td><td><a href="/projects/spi_lcd_example/">spi_lcd_example</a></td></tr>
<tr><td><a href="/projects/sram_lite/">sram_lite</a></td><td><a href="/projects/test/platform/">platform</a></td><td><a href="/projects/uvc_dualstream_example/">uvc_dualstream_example</a></td></tr>
<tr><td><a href="/projects/uvc_example/">uvc_example</a></td><td><a href="/projects/video_pipeline_example/">video_pipeline_example</a></td><td><a href="/projects/video_player_example/">video_player_example</a></td></tr>
<tr><td><a href="/projects/voice_service_example/">voice_service_example</a></td><td><a href="/projects/wifi/bridge/">bridge</a></td><td><a href="/projects/wifi/iperf/">iperf</a></td></tr>
<tr><td><a href="/projects/wifi/p2p/">p2p</a></td><td><a href="/projects/wifi/rlk_demo/">rlk_demo</a></td></tr>
</tbody>
</table>
<!-- readme-projects:end -->
新建工程、分区与配置方法请参阅 [新建工程介绍](https://docs.bekencorp.com/arminodoc/bk_avdk_smp/smp_doc/bk7258/zh_CN/v3.1.1/developer-guide/create_new_project_rule/index.html)。

## 产品解决方案

以下列出基于本 SDK 的上层产品解决方案；方案代码与 Armino SMP SDK 建议使用相同的标签版本。

| 方案名 | 主要功能 | 详细说明 |
| --- | --- | --- |
| `bk_solution_ai` | AI 设备：语音唤醒、端到云对话、图像识别与双屏显示 | [AI 方案在线文档](https://docs.bekencorp.com/arminodoc/bk_ai_smp/bk7258/zh_CN/v3.1.1/index.html) |
| `bk_solution_dashboard` | 两轮车仪表：LVGL 仪表显示、投屏、Dashcam、蓝牙音频与配网 | [两轮车方案在线文档](https://docs.bekencorp.com/arminodoc/bk_dashboard/bk7258/zh_CN/v3.1.1/index.html) |
| `bk_solution_doorbell` | 可视门铃：摄像头采集、网络图传、双向对讲、低功耗保活 | [门铃方案在线文档](https://docs.bekencorp.com/arminodoc/bk_doorbell/bk7258/zh_CN/v3.1.1/index.html) |

## BEKEN 相关资源

- [BEKEN 官网](https://www.bekencorp.com/)
- [ARMINO 开发者论坛](https://armino.bekencorp.com/)
- [BEKEN 文档中心](https://docs.bekencorp.com/)
- [BK7258 Datasheet](https://docs.bekencorp.com/spec/BK7258/BK7258%C2%A0Datasheet.pdf)
