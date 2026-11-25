<p align="center">
  <img src="app/assets/nekodrop.png" width="128" alt="nekodrop">
</p>

<h1 align="center">nekodrop</h1>

<p align="center">
  <b>用笔记本自带无线网卡在 Windows 上原生收发 AirDrop，Wi-Fi 照常上网。</b><br>
  不需要 USB 网卡，不需要 Linux 虚拟机，也不需要 Mac。
</p>

<p align="center">
  <a href="README.md">English</a> · 简体中文 · <a href="docs/COMPATIBILITY.zh-CN.md">网卡兼容性文档</a> · <a href="CHANGELOG.md">更新日志</a>
</p>

<p align="center">
  <img alt="platform" src="https://img.shields.io/badge/platform-Windows%2010%20%7C%2011%20x64-0078d4">
  <img alt="card" src="https://img.shields.io/badge/card-Intel%20AX201%20%7C%20AX200%20%7C%20AX210%20%7C%20AX211%20%7C%20BE200%20%C2%B7%20Realtek%20RTL8852CE%20%C2%B7%20MediaTek%20MT7902%20%7C%20MT7921%20%7C%20MT7922-6c5ce7">
  <img alt="license" src="https://img.shields.io/badge/license-GPL--2.0-blue">
  <img alt="status" src="https://img.shields.io/badge/status-preview-yellow">
</p>

<p align="center">
  <img src="docs/demo.gif" width="760" alt="实机拍摄：iPhone 与 Windows 11 用自带 Intel AX201 网卡双向收发原生 AirDrop，同时保持 Wi-Fi 在线">
  <br>
  <sub>
    <b>实机拍摄（未装任何第三方 App 的 iPhone ↔ Windows 11，自带 Intel AX201 在 5 GHz 信道 44 同卡保持 Wi-Fi 在线）：</b><br>
    ① iPhone 原生隔空投送直接发现电脑并发图 → ② Windows 保持 Wi-Fi 在线同时点击接收 → ③ 3556×2000 原图无损保存打开 → ④ 资源管理器右键 <i>Send with nekodrop</i> → ⑤ 选中附近 iPhone 发出 → ⑥ iPhone 灵动岛原生弹窗收图并打开。
  </sub>
</p>

---

## 功能

- **接收 iPhone、iPad、Mac 发来的文件**：电脑会像苹果设备一样出现在 AirDrop 列表里。文件存到你指定的文件夹（默认 `下载\nekodrop`），每次传输都可以在通知里选择接受或拒绝。
- **发送到苹果设备**：在资源管理器里右键文件，选 **用 nekodrop 发送**（Windows 11 第一级菜单、经典菜单和“发送到”里都有），再选附近的设备。
- **AirDrop 时照常上网**：在“同时联网和 AirDrop”模式下，nekodrop 自己驱动网卡，一个虚拟接口连你的 Wi-Fi，另一个跑 AWDL（苹果无线直连协议）。Windows 里上网走一块名叫 **nekodrop** 的“有线网卡”。
- **完整的 Wi-Fi 管理**：扫描、连接、隐藏网络、已保存网络和自动连接。会导入 Windows 里已保存的 Wi-Fi，还能识别酒店、机场、校园网这类需要网页登录的网络，并自动在浏览器里打开登录页。
- **自动恢复**：后台服务盯着 radio、AWDL 守护进程和 AirDrop 引擎，哪个停了就重启哪个。开机自动启动。
- **托盘旁的小面板**：中英双语，浅色 / 深色。点托盘图标或按快捷键（默认 Win+Shift+D，可以录入自己的组合键）从右下角弹出，点别处自动收起。附近设备显示在雷达（或列表）上，动效用 Windows 原生 Fluent 曲线：发送与接收时送出蓝色波纹、进度环匀速走满并显示实时传输速率（`MB/s` / `KB/s`，可在设置里开关），完成后变绿打勾。
- **点 AirDrop 标志切换谁能发给你**：点左上角的标志，选 **接收关闭**、**仅限联系人** 或 **所有人**，和 iPhone 一样。支持三档自动同意接收（关闭 / 仅限联系人 / 全部）、媒体格式偏好（兼容 JPEG/H.264 或原始 HEIC/HEVC 及可选自动转存 JPG 副本）、系统通知大图预览与一键复制，以及发完大文件后自动 BLE 重脉冲唤醒 iPhone。
- **仅限联系人（实验性）**：在电脑上登录 Apple ID（支持 `-27952` 与 2FA 503 自动重试，以及一键导出脱敏登录日志），或导入 Mac 的 AirDrop 证书，电脑就会像你的 Mac 一样出现在你自己的 iPhone 上，开了「仅限联系人」的 iPhone 也能用。见下方「仅限联系人」一节。
- **Windows「共享」菜单**：资源管理器和其他应用的分享面板里可以直接选 nekodrop。

## 界面截图

一个从托盘旁边弹出的小面板（点托盘图标或按快捷键打开，点别处自动收起）：附近的 iPhone 和 Mac 显示在雷达上（也可以切换成列表），左上角的 AirDrop 标志可以切换谁能发给你，标题栏里有状态和快捷开关，设置分五个标签页，主题色只有一种、可以切换（也可以选多彩），支持浅色和深色。接收文件时「本机」外圈转动显示进度，保存完成后变绿打勾。全局快捷键默认 Win+Shift+D，可以在设置里录入自己的组合键。

<table>
  <tr>
    <td align="center"><img src="docs/screenshots/main-radar.png" width="280" alt="主窗口：雷达"><br><sub>雷达：待接受的请求、发送中、已发送、被拒绝</sub></td>
    <td align="center"><img src="docs/screenshots/main-list-dark.png" width="280" alt="主窗口：列表（深色）"><br><sub>列表（深色）</sub></td>
    <td align="center"><img src="docs/screenshots/send.png" width="230" alt="发送窗口"><br><sub>文件右键 →「用 nekodrop 发送」</sub></td>
  </tr>
  <tr>
    <td align="center"><img src="docs/screenshots/settings.png" width="280" alt="设置"><br><sub>设置：主题色、自定义快捷键</sub></td>
    <td align="center"><img src="docs/screenshots/settings-appleid.png" width="280" alt="设置：Apple ID"><br><sub>Apple ID 与仅限联系人（实验性）</sub></td>
    <td align="center"><img src="docs/screenshots/wifi.png" width="250" alt="Wi-Fi 窗口"><br><sub>nekodrop 自己的 Wi-Fi</sub></td>
  </tr>
  <tr>
    <td align="center"><img src="docs/screenshots/tray.png" width="260" alt="托盘菜单"><br><sub>托盘菜单</sub></td>
    <td align="center" colspan="2"><img src="docs/screenshots/motion-send.png" width="560" alt="发送动画逐帧"><br><sub>发送动画逐帧：进度环走满、变绿、画出对号</sub></td>
  </tr>
</table>

## 实机测试

以下都是作者在自己的笔记本上实际测过的，不是纸上谈兵：

| | |
|---|---|
| 网卡 | **Intel Wi-Fi 6 AX201 160MHz**（CNVi，固件 `iwlwifi-QuZ-a0-hr-b0-68`） |
| 系统 | Windows 11 专业版 x64 |
| iPhone → Windows | ✅ 可用：照片和文件完整收到 |
| Windows → iPhone | ✅ 可用：右键 → 用 nekodrop 发送，iPhone 接受并收到 |
| AirDrop 速度 | 电脑 → iPhone 最高 **50.55 MiB/s（53.00 MB/s）**（846 MB 用 15.97 秒，Block Ack 确认率 99.85%，0 丢包）；iPhone → 电脑 净传输最高 **50.81 MiB/s（53.28 MB/s）**、端到端 **47.36 MiB/s（49.66 MB/s）**（846 MB 净收 15.84 秒 / 总计 17.00 秒，0 丢包；开启高吞吐 HE80 + MTU 1500 与零管道 SHM，`v2.2.9+`） |
| 同时上网 | ✅ 可用：AirDrop 运行时电脑通过 nekodrop 自己的 WPA2 连接保持在线，Wi-Fi 和 AWDL 在同一个 5 GHz 信道上也没问题 |

之后用户也在 AX200、AX210、AX211、AX411/AX1690i、9560 和 Realtek RTL8852CE 上确认了 AirDrop 可用，贡献者也在 MediaTek MT7902 和 MT7922 上实测过（见下表）。欢迎在 [Issues](../../issues) 反馈其他机器的结果。

## 支持的网卡

nekodrop 通过模块化底层驱动后端（`CardCatalog` + `radio_backend.h`）直接驱动无线网卡芯片，每个网卡家族一个用户态 radio：

- **Intel** 22000 / AX210 族，以及实验性的 **BE200**（`nekodrop-radio.exe`，移植自 [ForbiddenItlwm](https://github.com/retropineaple/ForbiddenItlwm) 的 `hal_iwx`）：同时连 Wi-Fi 或按需接管。
- **Realtek RTL8852CE**（`nekodrop-radio-rtl.exe`，基于 [winawdl](https://github.com/raph559/winawdl)/[devourer](https://github.com/raph559/devourer)）：按需接管。
- **MediaTek MT7902**（`nekodrop-radio-mt.exe`）和 **MT7921 / MT7922**（`nekodrop-radio-mtk.exe`），从 Linux mt76 移植到用户态：同时连 Wi-Fi（仅 5 GHz）或按需接管。

哪些在真机上测过，见下表。表外的网卡见[适配新网卡](#适配新网卡)。

**完整跨品牌网卡兼容性文档（Intel / Realtek / MediaTek / 高通）：[docs/COMPATIBILITY.zh-CN.md](docs/COMPATIBILITY.zh-CN.md)** · **更新日志：[CHANGELOG.md](CHANGELOG.md)** · **底层多网卡架构：[docs/HARDWARE.md](docs/HARDWARE.md)**

当前已内置启用的网卡速览：

| 网卡 | PCI ID | 固件 / 后端 | 状态 |
|---|---|---|---|
| **Intel Wi-Fi 6 AX201** 160 MHz（CNVi） | `8086:` `02F0` `06F0` `34F0` `3DF0` `4DF0` `43F0` `A0F0` | `Qu/QuZ-hr-b0-68`（`iwx`，同时连 Wi-Fi / 按需） | ✅ **已实测**（`43F0` 上的 QuZ） |
| Qu/QuZ 上的 Intel Wireless-AC 9560 / 9462 / 9461（CNVi） | 与 AX201 相同 | `Qu/QuZ-jf-b0-68`（`iwx`，同时连 Wi-Fi / 按需） | ✅ 用户实测可用（9560，[#45](../../issues/45)） |
| Intel Wi-Fi 6 AX200（独立网卡） | `8086:2723` | `cc-a0-68`（`iwx`，同时连 Wi-Fi / 按需） | ✅ 用户反馈双向 AirDrop 可用（[#1](../../issues/1)、[#4](../../issues/4)）；打开 A-MPDU 后发给 iPhone 9–25 MiB/s（[#4](../../issues/4)） |
| Intel Wi-Fi 6E AX210（独立网卡） | `8086:2725` | `ty-a0-gf-a0-68` + PNVM（`iwx`，同时连 Wi-Fi / 按需） | ✅ 用户反馈双向 AirDrop 可用（[#14](../../issues/14)、[#18](../../issues/18)、[#11](../../issues/11)） |
| Intel Wi-Fi 6E AX211（CNVi，第 12 代及以后） | `8086:` `7AF0` `7A70` `51F0` `51F1` `54F0` `7F70` | `so-a0-gf-a0-68` + PNVM（`iwx`，同时连 Wi-Fi / 按需） | ✅ 用户反馈双向 AirDrop 可用（[#9](../../issues/9)、[#29](../../issues/29)）；0.1.31 起 iPhone 能看到电脑（[#24](../../issues/24)） |
| Intel Wi-Fi 6E AX411 / Killer AX1690（CNVi） | 与 AX211 相同 | `so-a0-gf4-a0-68` + PNVM（`iwx`，同时连 Wi-Fi / 按需） | ✅ 用户实测可用（[#37](../../issues/37)） |
| So 平台上的 Intel Wi-Fi 6 AX203 / 9560 | 与 AX211 相同 | `so-a0-hr/jf-b0-68`（`iwx`，同时连 Wi-Fi / 按需） | ⚠️ 未测试 |
| **Realtek RTL8852CE** | `10EC:C852` | 内嵌固件（`devourer`，仅按需接管） | ✅ 已内置；用户实测双向可用，峰值约 **24.5 MB/s**（[#61](../../issues/61)） |
| **MediaTek MT7902** | `14C3:7902` | linux-firmware `WIFI_RAM_CODE_MT7902_1`（移植自 `mt76`，同时连 Wi-Fi（仅 5 GHz）与按需接管） | ✅ 已内置；双向 AirDrop（接收约 15 MB/s，发送约 5 MB/s）与同时连 Wi-Fi 均已真机实测（[#34](../../pull/34)、[#47](../../pull/47)） |
| **MediaTek MT7922 / AMD RZ616** | `14C3:0616` `14C3:7922` | linux-firmware `WIFI_RAM_CODE_MT7922_1`（`mtkradio`，同时连 Wi-Fi（仅 5 GHz，实验性）与按需接管） | 🧪 实验性，已内置；双向 AirDrop 已实测（[#40](../../pull/40)，[docs/MT7922.md](docs/MT7922.md)）；`v2.2.8`–`v2.2.9` 已补齐 HE80 2SS LWTBL、扫描 `FIF_BIT_CLR`、固件 RA 与 RX A-MSDU 修复（[#65](../../pull/65)、[#66](../../issues/66)） |
| **MediaTek MT7921 / AMD RZ608** | `14C3:7961` `14C3:0608` | linux-firmware `WIFI_RAM_CODE_MT7961_1`（`mtkradio`，同时连 Wi-Fi（仅 5 GHz，实验性）与按需接管） | ⚠️ 实验性，已内置；按需与同时连 Wi-Fi 均尚未在真机上测试 |
| Intel SnJ `2726`、QnJ `2720`、Meteor Lake `7E40`/`2729` | `8086:` | 上游没有固件 | ❌ |
| **Intel Wi-Fi 7 BE200**（独立网卡） | `8086:272B` | linux-firmware `gl-c0-fm-c0-100`（`iwx` 的 Bz/MLD 路径，同时连 Wi-Fi / 按需） | 🧪 实验性，已内置；`v2.2.9` 已加固 4096 字节页对齐与双 LMAC 诊断，欢迎实测反馈（[#53](../../issues/53)、[docs/BE200.md](docs/BE200.md)） |
| Intel BE201 / BE211（Wi-Fi 7，CNVi） | `8086:` `A840` `7740` | 需要 CNVi 硬件 | ❌ |
| 其他 Realtek、MediaTek、高通、博通 | | | ❌ |

怎么看自己的网卡：打开 *设备管理器 → 网络适配器*。没有受支持的无线网卡时，安装程序会拒绝安装，并列出本机检测到的无线网卡、PCI/USB ID 和不支持原因，以及运行 `installer/report.ps1` 的指引。在未实测的网卡上（包括新内置的 Realtek RTL8852CE 后端），nekodrop 可能启动不了 radio；这时网卡不会坏，运行下面的**一键回退**即可，也欢迎在 [Issues](../../issues) 附上日志反馈。

网卡按 PCI ID 自动识别。没识别出来时，可以在 *设置 → 网卡* 里手动选择网卡和型号（只对驱动包里列出的硬件 ID 有效）。想给 nekodrop 适配新的网卡家族，见 [docs/ADDING-A-CARD.md](docs/ADDING-A-CARD.md)。

不确定自己是什么网卡，或者希望支持别的网卡？[诊断报告](#反馈问题诊断报告)不装 nekodrop 也能用：下载 [`installer/report.ps1`](installer/report.ps1)，运行 `powershell -ExecutionPolicy Bypass -File report.ps1`。它会列出每块无线网卡的 PCI ID、芯片、驱动，以及 nekodrop 为什么能或不能在上面运行；欢迎贴到 [Issues](../../issues)。已安装 nekodrop 时，运行 `NekoDropService.exe --status` 可查看匹配到的网卡档案、能力与当前驱动提供者，`NekoDropService.exe --selftest` 可在无网卡、无管理员权限下校验全部 22 个活跃网卡档案以及多后端启动参数规划。Intel 的 radio 启动时会记一行 `backend …` 日志并在每次构建时运行 `iwx_selftest`；Realtek 的 radio 构建会同步运行四项离线单测（`block_ack_test`、`reorder_buffer_test`、`outgoing_session_test`、`dma_address_test`）；`install.ps1 -SelfTest`、`restore.ps1 -SelfTest` 和 `report.ps1 -SelfTest` 也会在每次构建时于 Windows PowerShell 5.1 下运行离线自检。支持其他网卡需要做什么，见 [docs/HARDWARE.md](docs/HARDWARE.md)（英文）。

### 出问题了？一键回退

开始菜单 → **nekodrop - restore Windows Wi-Fi**（或双击 `C:\Program Files\nekodrop` 里的 `restore.cmd`）。它是独立脚本（即使安装目录损坏也能运行），会停掉 nekodrop，解除 Intel 硬编码、按网卡厂商归还原厂驱动（Intel 网卡行为保持不变），删除 nekodrop 的驱动包并恢复 Windows 自带的 Wi-Fi，不管 nekodrop 把网卡留在什么状态。nekodrop 本身不会被卸载，想再用时重新运行安装程序即可。日志：`C:\ProgramData\nekodrop\restore.log`。

### 反馈问题：诊断报告

日志是我们修问题的依据，尤其是我们手上没有的网卡。提 [Issue](../../issues) 时请务必附上。

1. 开始菜单 → **nekodrop - diagnostic report**（或托盘图标 → **生成诊断报告…**，或主窗口底部的 **诊断报告**）。会弹出管理员权限提示，请允许，因为日志和驱动状态只有管理员能读。
2. 桌面上会生成两个文件，报告会用记事本打开：
   - `nekodrop-report-<时间>.md`：摘要、最近一次 radio 启动的逐步检查（驱动绑定 → 固件 → PNVM → 初始化 → 就绪）、系统、网卡、驱动、网络和蓝牙信息、发现设备的时间线（含发现过程每一步用了多少秒），以及关键日志行。它也已经复制到剪贴板，**直接粘贴到 issue 里**即可。
   - `nekodrop-logs-<时间>.txt`：完整日志，**拖进 issue 作为附件**。
3. SSID、MAC 地址、内网 IP、用户名和电脑名都会替换成 `<SSID-1>`、`<MAC-2>` 这样的占位符，不会上传任何东西。发出前请先看一眼报告内容。

**旧版本没有这个入口时**，可以下载 [`installer/report.ps1`](installer/report.ps1)，在命令提示符里运行：`powershell -ExecutionPolicy Bypass -File report.ps1`。也可以手动收集日志：

| 文件 | 内容 |
|---|---|
| `C:\ProgramData\nekodrop\service.log`（以及 `service.log.1`） | 服务、radio（固件、AWDL、Wi-Fi）和 awdld 的日志，需要管理员权限才能读 |
| `%LocalAppData%\nekodrop\engine.log`（以及 `engine.log.old`） | AirDrop 引擎：发现设备、蓝牙信标、收发文件 |
| `%LocalAppData%\nekodrop\appleid\appleid.log` | Apple ID 登录与 Anisette 辅助日志（`report.ps1` 自动脱敏收录，亦可在 设置 → Apple ID 一键导出） |
| `C:\ProgramData\nekodrop\install.log` | 安装过程：驱动、TAP 网卡、防火墙、服务 |
| `C:\ProgramData\nekodrop\restore.log` | 一键回退 |

## 适配新网卡

nekodrop 只支持上表里的网卡。**单纯要求支持其他网卡、不附代码的 issue 会直接关闭**，并附上本节链接。我们只能适配手上有的网卡，或者有人移植并在真机上测过的网卡。新笔记本里的网卡，比如 Intel Wi-Fi 7 的 CNVi 版本（BE201/BE211、Panther Lake `8086:E440`，Windows 里显示为「AX211」）、高通、博通，都需要重新移植驱动，不是加一个设备 ID 就能支持的。

如果你手上有这张卡，想自己适配，欢迎提 PR：

- **完整流程：** [`.claude/skills/add-wifi-card/SKILL.md`](.claude/skills/add-wifi-card/SKILL.md)。它是写给 AI 编程助手的 skill（Claude Code 打开这个仓库会自动识别），人直接读也能当教程用。内容包括：识别网卡、radio 驱动程序、服务、寄存器代理、安装程序和固件锁定、同时连 Wi-Fi 模式、验证方法，以及 PR 需要附带的东西。它是从 AX210 系列、RTL8852CE、MT7902、MT7922/MT7921 和 BE200 的实际适配过程中整理出来的。
- **架构说明：** [docs/ADDING-A-CARD.md](docs/ADDING-A-CARD.md)。
- **PR 需要附上真机运行的诊断报告：** 双向 AirDrop，并写明速度。只编译通过的代码会作为实验性支持发布。

## 系统要求

- Windows 10 2004（版本 19041）或更新的 64 位系统（Windows 10 22H2 在 0.1.16 及以上已确认双向可用，[#18](../../issues/18)）。
- 受支持的无线网卡，见[支持的网卡](#支持的网卡)：Intel Wi-Fi 6/6E（以及实验性的 BE200）、Realtek RTL8852CE、MediaTek MT7902 / MT7921 / MT7922。
- 安装需要管理员权限。
- 发送文件需要蓝牙：AirDrop 用蓝牙低功耗信标唤醒附近的苹果设备。
- 苹果设备那边把 AirDrop 设为 **所有人（10 分钟）**；这台电脑有了 Apple ID 身份后，也可以用 **仅限联系人**（见[仅限联系人](#仅限联系人apple-id实验性)）。

## 安装

1. 从 [Releases](../../releases) 下载 `nekodrop-<版本>-setup.exe` 并运行。
2. 安装程序会：
   - 把文件复制到 `C:\Program Files\nekodrop`；
   - **在你的电脑上**生成一个代码签名证书，用它给小小的寄存器代理驱动签名并信任它，然后**删除私钥**，这个证书以后再也签不了别的东西；
   - 创建名为 **nekodrop** 的 TAP 虚拟网卡（TAP-Windows，OpenVPN 签名）；
   - 添加两条只允许链路本地地址的入站防火墙规则（`TCP 8771-8779, 18771-18779, 28771-28779` 以避开 Hyper-V 保留端口段、`UDP 5353`）；
   - 安装并启动 **NekoDropService** 服务。
3. 托盘里会出现紫色小猫图标。打开它，确认 **允许接收** 已勾选。

第一次启动时，nekodrop 会从 Intel 驱动手里接管网卡，并自动连回 Windows 原来连着的网络。之后 Windows 自带的 Wi-Fi 图标会消失，改用 nekodrop 托盘菜单里的 **Wi-Fi…** 窗口管理网络。

### 更新

nekodrop 启动一分钟后检查 GitHub 上有没有新版本，之后每 6 小时检查一次，支持实时动态读取 Windows 系统代理；当 `api.github.com` 不可达或限流时自动回退到与浏览器同源的网页通道（[#62](../../issues/62)）。发现新版本会弹通知，点 **更新** 就会下载安装包并实时显示下载速率（`KB/s` / `MB/s`，[#64](../../pull/64)），先核对 GitHub 公布的 SHA-256，再运行安装（Windows 会请求管理员权限）。不会在你不知情时自己安装。*设置 → 关于与更新* 里可以关闭自动检查、选择是否包括预览版、安装完成后自动删除安装包（[#44](../../pull/44)）、选择更新完成后打开 **主面板** 或启动到 **托盘**（[#71](../../pull/71)），也可以手动 **检查更新**。版本分两个通道：**稳定版**（当前为 `v2.2.8`），以及修复一出就构建的 **预览版**。默认包括预览版；取消勾选「包括预览版」就只收稳定版。

### 卸载

在 *设置 → 应用 → nekodrop → 卸载*，或者运行安装目录里的 `uninstall.ps1`。卸载时会先把网卡还给 Intel 驱动，再删除服务、驱动包、证书信任和防火墙规则。

## 使用

| 想做什么 | 怎么做 |
|---|---|
| 接收 | 保持接收开启（左上角 AirDrop 标志是彩色、不是灰色）。在 iPhone 上点 分享 → AirDrop，选你电脑的名字 |
| 选谁能发给我 | 点 AirDrop 标志：**接收关闭**、**仅限联系人**（实验性，设置 → Apple ID）或 **所有人** |
| 发送 | 右键文件 → **用 nekodrop 发送** → 选设备；也可以在资源管理器里选中文件后按快捷键（默认 Win+Shift+D） |
| 换 Wi-Fi | 托盘 → **Wi-Fi…**，选网络、输入密码、点连接 |
| 连隐藏网络 | **Wi-Fi…** → *隐藏网络*，输入网络名和密码 |
| 酒店 Wi-Fi 登录 | 直接连接，登录页会自动在浏览器里打开 |
| 把网卡还给 Windows | 托盘 → *联网方式* → **按需接管网卡** 或 **关闭 AirDrop** |
| 改设备名、保存位置、语言 | 托盘 → **设置…** |
| 改快捷键 | 设置 → 常规：点一下快捷键，按下新的组合键 |

### 联网方式

| 模式 | 上网 | AirDrop |
|---|---|---|
| **同时联网和 AirDrop**（默认） | 走 nekodrop 自己的 Wi-Fi 连接（“nekodrop”网卡） | 一直可用 |
| **按需接管网卡** | 正常的 Intel 驱动 | 附近 iPhone 打开 AirDrop 或你发送文件时才接管网卡，传输期间会断网 |
| **关闭 AirDrop** | 正常的 Intel 驱动 | 关闭 |

### 支持的 Wi-Fi 加密

| 网络类型 | 同时模式下 |
|---|---|
| 开放网络 | ✅ |
| WPA/WPA2 个人版 | ✅ |
| WPA2/WPA3 混合 | ✅（用 WPA2 连接） |
| 要求管理帧保护的 WPA2、WPA2-PSK-SHA256 | 🧪 0.1.12，未实机测试 |
| 需要网页登录的网络 | ✅ 自动识别并打开登录页 |
| 隐藏网络 | ✅ |
| 纯 WPA3 个人版（SAE） | 🧪 0.1.12，未实机测试（只接受 hash-to-element 的网络暂不支持，多见于 6 GHz） |
| 增强型开放（OWE） | 🧪 0.1.12，未实机测试 |
| WPA2/WPA3 企业版（802.1X、PEAP 等） | ❌ 暂不支持，会自动改用“按需”模式 |
| WEP、WPA/TKIP | ❌（不安全，不计划支持） |

### 仅限联系人（Apple ID，实验性）

设置 →「Apple ID」里先打开顶部的实验性开关，再选择谁可以给这台电脑发送：「所有人」或「仅限联系人」。「仅限联系人」需要一个和 Mac 一样的 AirDrop 身份，有两种方式获得：

- **用 Apple ID 登录**：nekodrop 像 Mac 一样向 Apple 申请 AirDrop 证书（WirDrop 的 `appleid`，MIT），自动续期并读取 iCloud 通讯录。第一次登录会从 Apple 下载 iCloud for Windows 的一个组件（约 160 MB，不安装 iCloud）。密码只用一次、不保存。
- **从 Mac 导入**：在登录了你 Apple ID 的 Mac 上用 [airdrop-keychain-extractor](https://github.com/seemoo-lab/airdrop-keychain-extractor) 导出 `certificate.pem`、`key.pem`、`validation_record.cms`（需要临时部分关闭 SIP），在设置里选中它们所在的文件夹，再导入一个 `.vcf` 通讯录。

有了身份，你自己的 iPhone 会像对待你的 Mac 一样列出这台电脑；对方 iPhone 开「仅限联系人」时，只要它的通讯录里有你 Apple ID 的邮箱或手机号，也能看到这台电脑。接收端的「仅限联系人」过滤只读对方的确认记录、不校验 Apple 签名，只是过滤，不是身份证明。维护者已在 2.1.0 上真机确认可用。

### 实验性功能

在 *设置 → 网卡与高级 → 实验性功能* 里，默认都关闭；关掉后行为和原来完全一样。（已经转正、始终打开的：聚合发送 A-MPDU，0.1.35 起；Intel 网卡的蓝牙与 Wi-Fi 均衡共存，0.1.37 起，让蓝牙信标发得出去，电脑几秒内就能找到 iPhone 和 Mac，感谢 [@XXD051030](https://github.com/XXD051030)，[#26](../../pull/26)。）

- **高吞吐模式（Wi-Fi 6 HE80 + MTU 1500）**（参考 [`YiHoooong/owly`](https://github.com/YiHoooong/owly/tree/feat/windows-mtkradio)）：在 5 GHz 80 MHz 下广播 Apple AWDL Wi-Fi 6（HE80）能力并将 `awdl0` MTU 提至 `1500`。配合 4+4 MiB 无锁共享内存数据面、8 KiB 接收 DMA、8 MiB Wintun 环、`ev_win.c` 公平轮询与流式接收，AX201 实测双向均可达到 **50+ MiB/s（约 53 MB/s）**。
- **Wi-Fi 和 AirDrop 不在同一信道时按苹果的方式分时**：告诉 iPhone 电脑什么时候在路由器那边。nekodrop 连的 Wi-Fi 在另一个 5 GHz 信道时，一台 AX211 不开它一个 AirDrop 帧都收不到（[#35](../../issues/35)）；一台 AX200 上速度时快时慢（[#4](../../issues/4)）。
- **更快解锁被动 5 GHz 信道**：更早扫描，让拒绝国家码的网卡更快可以发射（[#11](../../issues/11)）。
- **待机 BLE 广播（`NEKODROP_BLE_STANDBY=1`）**：可选环境变量开关（[#69](../../pull/69)，默认关闭），开启后可在接收待命时维持低占空比 BLE AirDrop 广播。

## 进度

| 部分 | 状态 |
|---|---|
| 用户态 Intel 驱动：固件启动、监听、注入、扫描 | ✅ AX201、AX200、AX210、AX211、AX411/AX1690i、9560 上可用（后五者为用户反馈） |
| iPhone → Windows | ✅ 可用；AX201 实测净收最高 **50.81 MiB/s（53.28 MB/s）**、端到端 **47.36 MiB/s**（`v2.2.9+`，0 丢包） |
| Windows → iPhone | ✅ 可用；AX201 实测最高 **50.55 MiB/s（53.00 MB/s）**（`v2.2.9+`，BA 确认率 99.85%，0 丢包） |
| 联网同时 AirDrop | ✅ 可用（WPA2、DHCP、DNS 都走“nekodrop”网卡） |
| Wi-Fi 管理、已保存网络、网页登录识别 | ✅ |
| 服务、托盘程序、通知、右键菜单、安装包 | ✅ |
| 托盘面板、雷达、「共享」菜单、自定义快捷键、实时传输速率 | ✅ `v2.2.10` |
| 仅限联系人（Apple ID 登录、日志导出或 Mac 证书） | ✅ 实验性（`v2.1.0+`，`v2.2.9` 补齐 `-27952`/2FA 重试与日志脱敏导出） |
| 纯 WPA3、OWE | 🧪 0.1.12，未实机测试 |
| 802.1X（企业版） | ⏳ 计划中（eduroam PEAP 方案见 [#58](../../pull/58)） |
| AX200、AX210、AX211、AX411 / Killer AX1690i | ✅ 用户反馈双向 AirDrop 可用（[#1](../../issues/1)、[#14](../../issues/14)、[#24](../../issues/24)、[#37](../../issues/37)） |
| Qu 上的 9560 | ✅ 用户实测可用（[#45](../../issues/45)） |
| So 上的 9560、AX203 | 🧪 已照上游移植，还没有用户反馈 |
| Realtek RTL8852CE | ✅ 用户实测双向可用，峰值约 **24.5 MB/s**（[#61](../../issues/61)） |
| MediaTek MT7902 | ✅ 双向 AirDrop 和同时连 Wi-Fi，作者实测（[#34](../../pull/34)、[#47](../../pull/47)） |
| MediaTek MT7922 / MT7921 | 🧪 MT7922 按需模式已实测（[#40](../../pull/40)）；`v2.2.8`–`v2.2.9` 已补齐 HE80 2SS、扫描 `FIF_BIT_CLR`、固件 RA 与 RX A-MSDU 修复（[#65](../../pull/65)、[#66](../../issues/66)） |
| Intel BE200（Wi-Fi 7） | 🧪 实验性：`v2.2.9` 加固 Bz/MLD 4096 字节页对齐与双 LMAC 错误转储（[#53](../../issues/53)、[docs/BE200.md](docs/BE200.md)） |
| Windows 10 | ✅ 用户反馈 0.1.16 及以上双向 AirDrop 可用（[#18](../../issues/18)） |
| 内核 WiFiCx 驱动（`kdriver/`） | 🧊 已搁置：实验性，安装包**不会安装**它，见 [docs/KERNEL.md](docs/KERNEL.md) |

## 工作原理

```
 nekodrop.exe（托盘程序，用户会话）──JSON 管道──► NekoDropService（SYSTEM）
   │  AirDrop 引擎（Python）                        │ 接管 / 归还网卡
   │  通知 · 右键菜单 · 发送窗口                       │ Wi-Fi 管理、看门狗
   │                                                ▼
   └── awdl0（Wintun 8 MiB，IPv6）◄── awdld（AWDL，OWL）◄─SHM/管道─ nekodrop-radio（用户态驱动）
                                                         ├ MAC 0：AWDL，信道 6/44/149
     “nekodrop” TAP 网卡 ◄── 以太网桥接 ◄─────────────────┤ MAC 1：站点模式，连你的路由器
     （Windows 上网用）                                   └ UMDF 寄存器代理 + 用户态 DMA → 无线网卡
```

1. **寄存器代理**：一个很小的 UMDF 驱动（`driver/regproxy`）代替厂商驱动绑定网卡。它只负责把网卡寄存器和中断映射给一个用户态进程，我们自己的代码不在内核里运行。
2. **用户态驱动**：Intel 网卡用的 `nekodrop-radio`（`iwxradio/`）是把 OpenBSD/itlwm 的 `iwx` 驱动移植到用户态。它通过用户态 DMA（8 KiB 接收缓冲 + 与 `awdld` 的 4+4 MiB 无锁共享内存环）加载 Intel 固件，然后同时跑两个 MAC：一个监听和注入 AWDL 帧，另一个是普通的 Wi-Fi 站点，支持 WPA2（四次握手、密钥交给固件、速率控制）。Realtek 和 MediaTek 的 radio 为各自的芯片做同样的事，Wi-Fi 站点的代码是共用的。
3. **AWDL**：`awdld`（OWL，Open Wireless Link）通过共享内存 / radio 管道实现苹果的 AWDL 协议，把链路做成 IPv6 网卡 `awdl0`（8 MiB Wintun 环、`ev_win.c` 公平轮询、TCP GRO，可选 HE80 + MTU 1500）。
4. **AirDrop**：Python 引擎在 `awdl0` 上实现 AirDrop：mDNS 发现（含 macOS IPv6 应答），`8771` 端口上的 HTTPS（备用段 `8772-8779`、`18771-18779`、`28771-28779`），流式 DVZip→CPIO 解包与 4 MiB 异步落盘，`/Discover`、`/Ask`、`/Upload` 流程，以及带传输后自动重脉冲唤醒的蓝牙信标。
5. **上网**：站点 MAC 收发的帧桥接到“nekodrop” TAP 网卡，Windows 把它当成以太网，用 DHCP 拿地址。

设计细节：[docs/DESIGN.md](docs/DESIGN.md)（驱动、DMA、风险）· [docs/CLIENT.md](docs/CLIENT.md)（服务、客户端、联网模式）· [docs/TESTING.md](docs/TESTING.md)（传输速度测试方法，英文）。

## 常见问题

| 现象 | 试试 |
|---|---|
| iPhone 搜不到电脑 | 把 iPhone 的 AirDrop 设为“所有人（10 分钟）”（这台电脑有 Apple ID 身份时也可以用“仅限联系人”），关掉再打开（清掉缓存的旧记录），手机解锁并放在附近 |
| 电脑找 iPhone 很慢 | 确认蓝牙是开着的：电脑靠蓝牙信标叫醒 iPhone。发不出信标时，诊断报告的引擎日志里会有 `beacon: false` 和原因 |
| 发给 iPhone 很慢 | nekodrop 连的 5 GHz Wi-Fi 和 AirDrop（44 或 149）不在同一信道时会变慢；在 nekodrop 里断开 Wi-Fi，或者把路由器改到那个信道会好很多 |
| nekodrop 连上 Wi-Fi 后 AirDrop 就不行了 | 路由器的 5 GHz 信道和 AirDrop（44 或 149）不一样：打开 *设置 → 实验性功能 →「Wi-Fi 和 AirDrop 不在同一信道时按苹果的方式分时」*；电脑插着网线的话，也可以在 nekodrop 里断开 Wi-Fi（[#35](../../issues/35)） |
| iPhone 上出现两个电脑 | 之前运行留下的缓存记录，在 iPhone 上把 AirDrop 关掉再打开 |
| 没网 | 托盘 → **Wi-Fi…**，确认已连接。网络可能需要网页登录，或者是纯 WPA3 / 企业网（见上表） |
| Windows 的 Wi-Fi 图标没了 | 同时模式下是正常的：网卡归 nekodrop 管。用 **Wi-Fi…**，或者切到“按需” |
| 同时模式下某些软件或游戏登录不了、不显示二维码 | 有些软件要求系统里有 Windows 的无线网卡，而同时模式下没有（nekodrop 的网卡是有线类型）。用这类软件时请切到“按需”（[#33](../../issues/33)） |
| 其他问题 | 提 [Issue](../../issues)，并附上[诊断报告](#反馈问题诊断报告) |

## 从源码构建

需要 Visual Studio 2022 Build Tools（C++、Windows SDK、CMake、vcpkg）、.NET 8 SDK 和 Git。Inno Setup 6 可选，没装就输出 zip。不用装 WDK，脚本会用它的 NuGet 包。

```powershell
git clone https://github.com/zheznanohana/nekodrop.git
cd nekodrop
.\scripts\fetch-deps.ps1        # 下载 winawdl、devourer、ForbiddenItlwm（固件）、MinGW、WDK NuGet 到 third_party\
.\scripts\build-installer.ps1   # 全部构建 → build\nekodrop-<version>-setup.exe
.\scripts\build-radio.ps1       # 只构建 Intel 用户态驱动并跑离线自检
.\scripts\build-radio-rtl.ps1   # 只构建 Realtek RTL8852CE radio（devourer）并跑离线单测
```

| 目录 | 内容 |
|---|---|
| `iwxradio/` | 用户态 Intel 驱动（C++）：固件、AWDL 监听/注入、站点模式、WPA2、UMAC 扫描 |
| `driver/regproxy/` | UMDF 寄存器代理驱动 |
| `app/service/` | NekoDropService（C#）：网卡接管、radio 监管、Wi-Fi 管理 |
| `app/client/` | 托盘程序（C# WPF）：界面、通知、运行 AirDrop 引擎 |
| `app/shellext/` | Windows 11 右键菜单 |
| `installer/` | Inno Setup 脚本、安装/卸载脚本、TAP 配置 |
| `kdriver/` | 已搁置的内核 WiFiCx 驱动（实验性，不随安装包发布） |

## 代码来源

nekodrop 是在其他开源项目的基础上做的。这里写清楚哪些是**直接用了源码**（复制、移植或打包），哪些只是**参考**。

### 用到的源码

| 项目 | 许可 | 用了什么 | 在 nekodrop 里的位置 |
|---|---|---|---|
| [retropineaple/ForbiddenItlwm](https://github.com/retropineaple/ForbiddenItlwm) 0.6.0（itlwm 分支，macOS） | GPL-2.0 | Intel 22000 系列驱动 `hal_iwx`：固件加载、TLV 解析、context info、收发环、固件命令、寄存器头文件 `if_iwxreg.h`。还有它的 AWDL 策略：信道跟随（44/149 的 min 规则、国家码学习）、接收 Block Ack、BA 期间关混杂、速率阶梯、主机侧单播重发、信标提示 | `iwxradio/`，从内核 C 移植成用户态 C++ 驱动 |
| ForbiddenItlwm 的固件目录 | Intel 再分发许可 | iwlwifi 固件 `iwlwifi-Qu*-hr-b0-68.ucode` | 随安装包放在 `radio\firmware\` |
| [raph559/winawdl](https://github.com/raph559/winawdl) | MIT / GPL-2.0（`radio/`） | 在 Windows 上从用户态接管 PCIe 网卡的方法：UMDF 寄存器代理驱动、用户态 DMA、radio 管道协议，以及 RTL8852CE radio 守护进程（`radio/`） | `driver/regproxy/`（支持 Intel 与 RTL8852CE 的 PCI ID）、`iwxradio/`（管道和 DMA）、`radio\nekodrop-radio-rtl.exe` |
| [raph559/devourer](https://github.com/raph559/devourer)（`winawdl/pcie-windows`） | GPL-2.0 | Realtek RTL8852CE 用户态 PCIe 驱动（`KESTREL_8852C`，内嵌固件与双向 A-MPDU 栈） | 编译打包为 `radio\nekodrop-radio-rtl.exe`（`licenses\devourer-GPL-2.0.txt`） |
| winawdl 的 `awdl/`（`awdld`） | GPL-3.0 或更新（基于 OWL） | AWDL 守护进程：OWL 的协议核心加上 Windows I/O 层和 Wintun | 从源码编译，随安装包放在 `awdl\awdld.exe` |
| winawdl 的 `airdrop/` | MIT | Python 写的 AirDrop 引擎：mDNS、HTTPS 接收端、发送端、蓝牙信标 | 放在 `airdrop\`，打了两个小补丁（`scripts/patch-engine.py`：固定实例名、备用端口） |
| [JungyoKim/WirDrop](https://github.com/JungyoKim/WirDrop) | MIT | Apple ID 登录（`appleid`、`appleid-anisette`）：Apple 登录、向 aidc.apple.com 申请 AirDrop 证书、读取 iCloud 通讯录。`installer/nekodrop_identity.py` 参照了它的身份目录格式和 owly-airdrop（MIT）的确认记录 / 哈希处理 | `appleid\appleid.exe`、`appleid\appleid-anisette.exe`，从源码编译，打了一个补丁（`scripts/patch-appleid.py`：手机号默认 +86） |
| [seemoo-lab/owl](https://github.com/seemoo-lab/owl) | GPL-3.0 | `awdld` 里的 AWDL 实现 | 经由 winawdl |
| [Wintun](https://www.wintun.net/) | Wintun 许可 | 虚拟网卡 `awdl0` | `awdl\wintun.dll`（预编译） |
| [TAP-Windows6](https://github.com/OpenVPN/tap-windows6) | GPL-2.0 / OpenVPN | Windows 上网用的“nekodrop”虚拟以太网卡 | 安装时装上（OpenVPN 签名的安装程序） |

### 参考过的项目（没有复制代码）

| 项目 | 参考了什么 |
|---|---|
| OpenBSD `iwx(4)`、Linux `iwlwifi` | 固件、UMAC 扫描、站点模式、密钥的工作方式 |
| [seemoo-lab/opendrop](https://github.com/seemoo-lab/opendrop) | AirDrop 协议（`/Discover`、`/Ask`、`/Upload`、mDNS 记录） |
| [microsoft/Windows-driver-samples](https://github.com/microsoft/Windows-driver-samples) | 已搁置的内核驱动（`kdriver/`）的 WiFiCx 结构 |
| [t4t5/owly](https://github.com/t4t5/owly)、[`YiHoooong/owly`](https://github.com/YiHoooong/owly/tree/feat/windows-mtkradio)、[JungyoKim/WirDrop](https://github.com/JungyoKim/WirDrop) | 其他平台上实现 AirDrop 的方案，设计时对比参考（含 HE80 + MTU 1500 高吞吐思路） |
| [ndpyzwy-0w0/DropBridge](https://github.com/ndpyzwy-0w0/DropBridge) | 交互体验参考：系统通知大图预览 + 一键复制剪贴板（Bitmap + FileDrop 双格式复制与目录边界校验）以及 HEIC / Live Photo 自动伴生 JPG 副本 |

### nekodrop 自己写的部分

- `iwxradio/`：用户态移植本身，以及所有 Windows 特有的部分：两个 MAC 同时运行（AWDL 监听加上独立 PHY 上的 Wi-Fi 站点）、4+4 MiB 无锁共享内存数据面、8 KiB 接收 DMA、WPA2 站点模式（四次握手、密钥交给固件、TLC 速率控制）、TAP 桥接、固件 UMAC 扫描、信标解析和加密方式识别。
- `app/service/`：Windows 服务：网卡接管、radio 和 awdld 监管、Wi-Fi 管理（DPAPI 加密保存的网络、自动连接、隐藏网络、网页登录识别）。
- `app/client/`：托盘程序（WPF）、通知、实时传输速率显示、发送窗口、Wi-Fi 窗口、Apple ID 日志脱敏导出、中英文界面。
- `app/shellext/`：Windows 11 右键菜单。
- `installer/`：Inno Setup 安装包、本机驱动签名、TAP 配置、诊断报告（`report.ps1`）、BLE 信标（`airdrop-beacon.ps1`）和卸载。

## 贡献者

感谢提交代码、文档和细致测试报告的各位：

- [@BaiBaiAi](https://github.com/BaiBaiAi)：Windows → iPhone 发送不再出示客户端证书（[#13](../../pull/13)）、AWDL 发送流水线化让大文件能传完（[#15](../../pull/15)），以及第一份 AX210 实测报告（[#14](../../issues/14)）。
- [@XXD051030](https://github.com/XXD051030)：Intel 网卡的蓝牙均衡共存模式，让电脑几秒内就能找到 iPhone 和 Mac，并附了一份 AX200 上的详细前后对照（[#26](../../pull/26)）。
- [@GGBond-cer](https://github.com/GGBond-cer)：大量详细的 AX200 实测报告，定位 A-MPDU 和速度问题（[#4](../../issues/4)）、卸载残留根因（[#22](../../issues/22)），撰写传输速度测试指南（[#28](../../pull/28)），修复卸载清理竞态（[#38](../../pull/38)），实现升级后自动删除安装包（[#44](../../pull/44)）、多屏不同缩放下窗口不跨屏（[#49](../../pull/49)）、更新下载实时速率显示（[#64](../../pull/64)），以及更新完成后可选打开主面板或托盘启动（[#71](../../pull/71)）。
- [@DaiJiequn](https://github.com/DaiJiequn)：MediaTek MT7902 支持，从 Linux mt76 移植的用户态驱动，按需模式下双向 AirDrop 可用（[#34](../../pull/34)）；以及它的「同时连 Wi-Fi」模式，这部分共用的联网代码现在 MT7921/MT7922 也在用（[#47](../../pull/47)）。
- [@sh17632](https://github.com/sh17632)：提出 AirDrop 风格的雷达界面和「共享」菜单方案，成为 2.0 新界面的基础（[#36](../../issues/36)）；贡献 AX211 实测反馈（[#35](../../issues/35)）、macOS IPv6-only mDNS 地址查询应答（[#55](../../pull/55)）、实验性待机 BLE 广播（`NEKODROP_BLE_STANDBY=1`，[#69](../../pull/69)），以及深入的 macOS 15 / iOS 18 AWDL 与 BLE 抓包分析（[#51](../../issues/51)、[#67](../../issues/67)）。
- [@YiHoooong](https://github.com/YiHoooong)：MediaTek MT7922/MT7921 用户态无线电驱动（`mtkradio`，[#40](../../pull/40)），修复 MT7922 切道超时与 RX 过滤恢复并加入 HE80 2SS LWTBL（[#65](../../pull/65)），并分享 `owly` 的 HE80 + MTU 1500 高吞吐实测经验。
- [@Yanlyn](https://github.com/Yanlyn)：补充 Windows + AX201 连接 eduroam PEAP 校园网文档（[#58](../../pull/58)），以及社区 MT7927 驱动适配研究（[#57](../../issues/57)）。
- 同时感谢所有提交完整诊断日志、帮助定位 AX211、AX411/AX1690i、BE200、RTL8852CE、MT7922 及 Apple ID 登录问题的热心测试者（[#30](../../issues/30)、[#37](../../issues/37)、[#42](../../issues/42)、[#53](../../issues/53)、[#54](../../issues/54)、[#61](../../issues/61)、[#62](../../issues/62)、[#66](../../issues/66) 等）。

## 许可

nekodrop 整体以 **GPL-2.0** 发布（见 [LICENSE](LICENSE)），因为 `iwxradio/` 派生自 itlwm/ForbiddenItlwm。

- `if_iwxreg.h`：BSD/GPL-2.0 双许可。
- `driver/regproxy` 和 AirDrop 引擎：沿用 winawdl 的 MIT 许可。
- `nekodrop-radio-rtl.exe`（`raph559/winawdl` `radio/` + `raph559/devourer`）：GPL-2.0（`licenses\devourer-GPL-2.0.txt`）。
- `awdld`：GPL-3.0 或更新，来自 winawdl/OWL，是通过管道通信的独立程序。
- iwlwifi 固件：Intel 再分发许可（`licenses\iwlwifi-firmware.txt`）。
- Wintun 和 TAP-Windows：各按其许可。

安装包会把所有许可证复制到 `licenses\`。

## 社区友链

- [LINUX DO](https://linux.do/) — 新的理想型社区

## 免责声明

nekodrop 是独立项目，与 Apple、Intel 没有任何关系，也未获得它们的认可。AirDrop、iPhone、iPad、Mac 是 Apple Inc. 的商标，Intel 是 Intel Corporation 的商标。nekodrop 运行期间会替换你的 Wi-Fi 驱动。这是实验性软件，按原样提供，不附带任何担保。
