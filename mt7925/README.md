# NekoDrop 的 MT7925 E138 移植

该移植最初在 NekoDrop 2.2.13 上完成实机测速。日常收发从 NekoDrop 托盘和资源管理器进入。兼容模式已在 E138 与 iPhone 之间完成双向文件校验。

## 安装和使用

1. 在设备管理器中打开 MT7925 的「属性 → 详细信息 → 硬件 ID」。本版匹配 `PCI\VEN_14C3&DEV_7925&SUBSYS_E138105B&REV_00`。
2. 使用 Windows 11 x64 build 26100 或更高版本。实验性内核驱动需要 Windows 当前这一次启动已启用测试签名。安装程序会检查运行环境。若用原开发版在跑无线会话，先正常结束会话并退出托盘。
3. 运行本分支生成的安装包。它与常规 NekoDrop 共用应用和 Windows 服务，安装属于替换当前运行版本。安装脚本会在本机签名并预置专用驱动。
4. 从托盘打开 NekoDrop，打开接收，或用发送窗口选文件。此卡采用按需模式，占用时普通 Wi-Fi 会中断。需要同时联网时可使用以太网或另一张网卡。iPhone 端打开 Wi-Fi、蓝牙，首次联调可选择「所有人，10 分钟」。
5. 需要手动交还网卡时，使用开始菜单的「nekodrop - 恢复 Windows Wi-Fi」。恢复目标是这张卡的原厂 Windows 网络驱动。安装与恢复共用的诊断信息在 `%ProgramData%\nekodrop`。

## 无线配置

在中国大陆室内使用时，可在 NekoDrop 设置中勾选「MT7925 高速模式（中国大陆室内）」并保存。更改设置会重启无线连接，请在传输结束后操作。取消勾选并保存即可切回兼容模式。

| 配置 | 默认兼容模式 | 中国大陆室内高速模式 |
|---|---|---|
| 无线链路 | 信道 6，20 MHz，单空间流 | 在信道 6 发现对端，再按对端广播的时隙选择 5 GHz 信道 44 或 149。使用 80 MHz、VHT 和块确认。接收空间流数取自固件能力，发射使用单流 |
| 无线进程 | `nekodrop-radio-mt7925.exe` | `nekodrop-radio-mt7925-fast.exe` |
| AWDL 进程 | `awdld-mt7925.exe` | `awdld.exe`，通过共享内存传输帧 |

两种模式均通过 Wintun 的 `awdl0` 收发文件。请求发射功率上限为 6 dBm，还要满足设备的固件监管表、CLC 和板级 SAR 限制。高速模式在固件授予信道并且 AWDL 接口就绪后报告已启动。固件取自 linux-firmware 提交 `5a3f625f2e9d9f70134af2340d82af3f19ae6891`，构建时校验 PATCH 与 RAM 的 SHA-256。

2026-11-26 至 27 日在 E138 上实测 iPhone 发送同一份 127,814,851 字节文件。接收端以开始上传到文件保存完成计时，信道 149，80 MHz。1 MB 为 1,000,000 字节。

| 接收设置 | 整份用时 | 平均速度 |
| --- | ---: | ---: |
| 每批最多合并 4 个 TCP 段 | 73.548 秒 | 1.738 MB/s |
| 每批最多合并 24 个 TCP 段，优化确认包队列 | 16.089 秒 | 7.944 MB/s |
| 同上，硬件接收环临时扩到 1024 项 | 19.811 秒 | 6.452 MB/s |
| 恢复 256 项接收环，在 Block Ack 协商中启用手机请求的原生 A-MSDU | 14.821 秒 | 8.624 MB/s |

1024 项接收环的占用峰值采样为 90 项。启用 A-MSDU 的一次传输收到了 46,345 个原生聚合帧。以上为各设置的单次整文件实测。

最终版在已建立的对端数据队列上以 24 Mbps 回送 TCP 确认包，继续使用逐帧 MAC 确认和失败重试。Windows 在上传开始时使用较小的 TCP 合并批次，传输进入稳定阶段后最多合并 40 段。接收端确认压缩流和 CPIO 归档均完整后，只在两个 HTTP 块之间缩短等待下一块首字节的时间。文件保存和归档校验仍计入总耗时。

2026-11-27，iPhone 距电脑天线约半米、无遮挡，向同一台 E138 连续测试同一版本，接收文件均为 127,814,851 字节，三份文件 SHA-256 一致。

| 同版本测试 | 从上传开始到保存完成 | 整份文件平均速度 |
| --- | ---: | ---: |
| 第一次 | 3.215 秒 | 39.756 MB/s |
| 连续复测 1 | 3.268 秒 | 39.111 MB/s |
| 连续复测 2 | 3.118 秒 | 40.993 MB/s |

用总字节数除以总耗时，连续两次复测为 40.030 MB/s，三次合计为 39.938 MB/s。

## 从源码构建

沿用 NekoDrop 的 .NET 8 SDK、Visual Studio C++、Windows SDK/WDK 和 MinGW-w64 工具链。仓库根目录下执行 `.\scripts\fetch-deps.ps1` 后，`.\scripts\build-installer.ps1` 可生成整包。单独构建兼容组件可执行 `.\scripts\build-mt7925.ps1` 和 `.\mt7925\build-awdl.ps1`。高速组件使用 `.\scripts\build-mt7925-fast.ps1`、`.\scripts\build-mt7925-sar.ps1` 和 `.\scripts\build-awdl.ps1 -MinGW .\third_party\winlibs\mingw64\bin`。

无线及内核代码来自 Dy1an-Zou 的提交 `74606871b001bdba5e447f84a43bb96124fb495a`。上游的实机记录对应 `60001A3B`。本分支针对 E138 使用独立的硬件匹配、固件、运行参数和驱动切换流程。[上游项目](https://github.com/Dy1an-Zou/Airdrop-for-Windows/tree/74606871b001bdba5e447f84a43bb96124fb495a)

各组件保留原许可。[内核 BSD-3-Clause-Clear](driver/LICENSE) · [无线进程 GPL-2.0](radio/LICENSE) · [AWDL 与 OWL GPL-3.0](awdl/LICENSE) · [radiotap ISC](upstream/owl/radiotap/COPYING)
