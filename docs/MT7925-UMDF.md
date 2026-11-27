# MT7925 E138 用户态无线后端验证

本实验针对 `PCI\VEN_14C3&DEV_7925&SUBSYS_E138105B&REV_00`。本分支扩展了 NekoDrop 的 UMDF 寄存器代理，由它读取板载 ACPI 方法 `MTDS`、`MTGS`，再由用户态无线后端解码功率限制。安装版 NekoDrop 的原有代理不包含本次新增的查询接口。

## 与已有后端对照

| 主线后端 | 硬件相关代码放在哪里 | E138 可以复用的部分 |
| --- | --- | --- |
| Intel AX200/201、AX210/211 及后续型号 | `iwxradio` 根据芯片代际选择固件及 NVM/PNVM；固件的 MCC 响应给出信道限制。不同型号各自验证。 | 通过固件取得当前硬件能力和适用信道，再报告能力。Intel 的固件命令仅适用于 Intel。 |
| Realtek RTL8852CE | 服务按需启动 winawdl/devourer 的 PCIe 用户态后端，使用共用的 radio 管道和 AirDrop 引擎。 | 按需接管、退出后恢复原厂驱动，发现和文件传输交给同一个 AWDL 主机。 |
| MediaTek MT7902 | `mtradio` 从 mt76 移植硬件初始化，选择芯片内 EFUSE、初始化 MAC、向固件下发国家和信道域。 | 保留原厂校准数据；先验证固件、被动收包和信道，再验证主动收发。 |
| MediaTek MT7921/MT7922 | `mtkradio` 使用同一个程序识别两款芯片，选择相应固件。MT7922 有实机双向记录，MT7921 在主线兼容表中仍待实测。 | 共用 MediaTek 程序、寄存器代理、服务和 radio 管道。E138 的 Connac3 固件格式、MCU 命令及收发描述符需要单独实现。 |

MT7925 的 Linux 驱动把主板 SAR 表作为可选输入。缺少动态表时，驱动继续由信道功率和每速率限制生成 SKU 表；有板级限制时取较低值。对应实现见 [MT792x 的 ACPI 初始化](https://github.com/openwrt/mt76/blob/4d7f2bed49cde7f1aa3b6e6e2d0bd8dc869f722e/mt792x_acpi_sar.c#L223-L259) 和 [MT7925 的功率表生成](https://github.com/openwrt/mt76/blob/4d7f2bed49cde7f1aa3b6e6e2d0bd8dc869f722e/mt7925/mcu.c#L4224-L4229)。Windows 上，请求在到达 ACPI 方法前返回 `STATUS_FILE_CLOSED` 时，板级限制仍然未知。固件读回能检查无线芯片实际报告的功率配置；板级 SAR 是否适用是单独的问题。

## E138 实机结果（2026-11-27）

Windows 已将本分支的 UMDF 代理绑定到 E138 REV_00。PnP 报告的设备栈为 `WUDFRd → ACPI → pci`，设备上、下层过滤器均为空。测试机启动时已开启测试签名，整个验证沿用此设置。默认启动安全配置下的安装仍待验证。

| 只读检查 | 实机结果 |
| --- | --- |
| 向默认 UMDF 下层目标发送方法请求 | Windows 驱动框架报告事件 10121 并终止驱动宿主，客户端得到错误 1291 |
| E138 专用 FileHandle 配置，通过 WDF 发送请求 | `MTDS`、`MTGS` 均返回 `STATUS_FILE_CLOSED`（`0xc0000128`），零字节 |
| 通过 UMDF 下层文件句柄读取，并先核对设备地址 | 探针收到 Win32 错误 6（`ERROR_INVALID_HANDLE`）。独立只读诊断复现时，下层目标创建和打开均成功，但 `WdfIoTargetWdmGetTargetFileHandle` 返回空句柄，未发出地址核对请求 `_ADR` |
| 在 E138 专用只读诊断包中转发应用原始请求 | 应用的打开操作成功，WDF 已提交固定的 `_ADR` 请求，下层以 `STATUS_FILE_CLOSED` 完成，返回 0 字节。 |

Windows 的 ACPI 事件跟踪记录了 PnP 绑定期间同一设备的三次 `_ADR` 求值，以及探针运行期间零次 `_ADR` 求值。本轮没有证据能把错误归因于某个特定的下层驱动。Windows 可通过平台扩展动态提供 ACPI 方法，静态固件表不足以证明运行时缺少 SAR 方法。[Microsoft 对 ACPI 平台扩展的说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/using-peps-for-acpi-services)

E138 验证包使用与 MT7921/MT7922 相同的 `nekodrop-radio-mtk.exe`。被动检查和主动模式共用 PCI 实例核对、固件启动、EFUSE 校准、MAC 初始化及初始被动信道域。主动模式在设置发射功率时读取板级限制。已确认动态表缺失或禁用时，地理修正表不参与计算；动态表的传输错误仍会中止发射。

使用从本分支重新编译的代理和共用程序，信道 44 扫描 250 ms，固件报告 9 个信标，主机收到 9 帧，退出码为 0。上游 TMAC 查询从 PHY 0 读回 417/417 个速率项，原始值为 0 至 37。读回发生在被动扫描结束后，不代表信道 44 的板级限值或实际发射功率。

随后通过服务使用的主动模式入口启动同一个程序。芯片加载了 1,275,712 字节固件，返回 7 条世界域规则，完成 12 条 CLC 请求。板级查询返回 Win32 错误 6；程序退出码为 1，未进入 AWDL 收发。测试结束后已恢复 `oem10.inf / mtkwecx` 和 NekoDrop 服务，并清理测试驱动包及临时证书。

## 离线构建与检查

在 Windows x64 的仓库根目录运行。使用 Visual Studio C++ Build Tools、Windows SDK、CMake 和 Ninja。构建代理还需要 WDK 的 UMDF 2.31 头文件和库。

```powershell
.\scripts\fetch-deps.ps1 -E138Validation
.\scripts\build-radio-mtk.ps1 -E138Validation
.\driver\regproxy\build.ps1
.\scripts\build-e138-sar-probe.ps1
```

无线后端构建脚本校验两份固定版本固件的 SHA-256，运行 CCMP、被动诊断参数、传输、板级 SAR、监管策略和 ROM patch 分段校验六项 CTest，并以固件文件为输入再次运行传输测试。代理构建脚本编译 DLL 和校验 INF。以上步骤仅生成或检查文件。

## 服务与图形界面验证构建

按仓库说明准备依赖后，可用
`scripts/build-installer.ps1 -E138Validation` 构建验证包。这个开关同时打包 E138
固件，并在 MT7921/MT7922 使用的同一个 `nekodrop-radio-mtk.exe` 中启用 E138 模式。服务和 PnP 按板号和修订号匹配。常规安装包不注册该设备。
同一套服务自检可用 `NekoDropService.exe --selftest` 离线执行。

在验证包的“设置”中选择 E138，并仅在电脑确实位于中国大陆室内时选择 44 或 149 信道。
出厂选项为“关闭”。每次按需启动均由无线进程独立验证板级功率限制。服务需先收到无线进程
的就绪通知，再等主机建立 awdl0 和固件确认所选信道，才报告已启动。启动失败、进程异常
退出或按需释放时，服务会重新绑定这台 PCI 设备的原厂网卡驱动。能否收发及实际速率仍需
在板级验证通过后实测。

## 在 E138 上只读检查 SAR

以下步骤适用于本分支代理已绑定到目标网卡、其他无线进程已释放代理的情形。在管理员 PowerShell 中按设备实例选择网卡。该探针不启动网卡固件，也不发射无线帧。

```powershell
$boards = @(Get-PnpDevice -PresentOnly | Where-Object {
    $_.InstanceId -like 'PCI\VEN_14C3&DEV_7925&SUBSYS_E138105B&REV_00\*'
})
if ($boards.Count -ne 1) { throw 'Expected one E138 REV_00 board' }
& .\build\e138-sar-probe\e138-sar-probe.exe --instance $boards[0].InstanceId
if ($LASTEXITCODE) { throw "SAR probe failed ($LASTEXITCODE)" }
```

输出分别给出 `MTDS` 和 `MTGS` 的 NTSTATUS 与字节数。退出码 0 表示每个方法返回了带有效 ACPI 包头的响应，或明确报告方法不存在（`0xc0000034`，0 字节）。退出码 1 表示打开代理、查询或响应检查失败，2 表示参数有误。探针检查的是查询通路。无线后端在启用发射前还会解码完整 SAR 表，并核对固件监管域、CLC 和信道功率限制。

## 检查实际固件和被动接收

在验证代理已接管准确的 E138 实例、服务已释放无线进程时，可在具备锁定内存页权限的会话中运行下面的单次检查。`$boards` 使用上文的精确实例筛选。程序尝试一次信道 44 被动扫描，并单独报告 TMAC 读回结果。退出码 0 表示扫描完成；请分别查看收帧数和 TMAC 日志确认各项结果。

```powershell
$stopFile = Join-Path $env:TEMP ("e138-passive-" + [guid]::NewGuid().ToString("N"))
& .\build\mtkradio-e138\nekodrop-radio-mtk.exe --check-passive `
    --instance $boards[0].InstanceId --firmware (Resolve-Path .\third_party\linux-firmware-mt7925).Path `
    --stop-file $stopFile
```

## 实机验收范围

当前主动 AWDL 入口只接受已确认的中国大陆室内场景、主信道 44 或 149、80 MHz。请求的传导功率上限为 6 dBm，板级与固件限制可以进一步收紧。实机验收应分别记录代理绑定与签名环境、SAR 查询结果、固件信道授权，以及双向 AirDrop 的落盘字节数、SHA-256 和整份用时。早期带内核过滤驱动版本的约 40 MB/s 成绩只适用于该版本的 iPhone → Windows 测试。
