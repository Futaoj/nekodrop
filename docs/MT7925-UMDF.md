# MT7925 E138 用户态无线后端验证

本实验针对 `PCI\VEN_14C3&DEV_7925&SUBSYS_E138105B&REV_00`。本分支扩展了 NekoDrop 的 UMDF 寄存器代理，由它读取板载 ACPI 方法 `MTDS`、`MTGS`，再由用户态无线后端解码功率限制。安装版 NekoDrop 的原有代理不包含本次新增的查询接口。

## E138 实机结果（2026-11-27）

Windows 已将本分支的 UMDF 代理绑定到 E138 REV_00。PnP 报告的设备栈为 `WUDFRd → ACPI → pci`，设备上、下层过滤器均为空。测试机启动时已开启测试签名，整个验证沿用此设置。默认启动安全配置下的安装仍待验证。

| 只读检查 | 实机结果 |
| --- | --- |
| 向默认 UMDF 下层目标发送方法请求 | Windows 驱动框架报告事件 10121 并终止驱动宿主，客户端得到错误 1291 |
| E138 专用 FileHandle 配置，通过 WDF 发送请求 | `MTDS`、`MTGS` 均返回 `STATUS_FILE_CLOSED`（`0xc0000128`），零字节 |
| 通过 UMDF 下层文件句柄读取，并先核对设备地址 | 探针收到 Win32 错误 6（`ERROR_INVALID_HANDLE`）。独立只读诊断复现时，下层目标创建和打开均成功，但 `WdfIoTargetWdmGetTargetFileHandle` 返回空句柄，未发出地址核对请求 `_ADR` |
| 在 E138 专用只读诊断包中转发应用原始请求 | 应用的打开操作成功，WDF 已提交固定的 `_ADR` 请求，下层以 `STATUS_FILE_CLOSED` 完成，返回 0 字节。 |

Windows 的 ACPI 事件跟踪记录了 PnP 绑定期间同一设备的三次 `_ADR` 求值，以及探针运行期间零次 `_ADR` 求值。本轮没有证据能把错误归因于某个特定的下层驱动。Windows 可通过平台扩展动态提供 ACPI 方法，静态固件表不足以证明运行时缺少 SAR 方法。[Microsoft 对 ACPI 平台扩展的说明](https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/using-peps-for-acpi-services)

诊断时已核对设备的 `UmdfDispatcher=FileHandle` 配置。以上结果无法判定固件是否提供板级 SAR 方法。实机流程因此止于只读探针，未启动无线发射或进行 AirDrop 收发。每轮均恢复 MediaTek 原厂驱动并清除了本轮的驱动包与临时证书信任。

## 离线构建与检查

在 Windows x64 的仓库根目录运行。使用 Visual Studio C++ Build Tools、Windows SDK、CMake 和 Ninja。构建代理还需要 WDK 的 UMDF 2.31 头文件和库。

```powershell
.\scripts\fetch-deps.ps1 -E138Validation
.\scripts\build-mt7925-fast.ps1
.\driver\regproxy\build.ps1
.\scripts\build-e138-sar-probe.ps1
```

无线后端构建脚本校验两份固定版本固件的 SHA-256，运行 CCMP、传输、板级 SAR、监管策略和 ROM patch 分段校验五项 CTest，并以固件文件为输入再次运行传输测试。代理构建脚本编译 DLL 和校验 INF。以上步骤仅生成或检查文件。

## 服务与图形界面验证构建

按仓库说明准备依赖后，可用
`scripts/build-installer.ps1 -E138Validation` 构建验证包。这个开关同时打包 E138
无线进程、固件，并为服务和 PnP 注册精确到板号和修订号的设备。常规安装包不注册该设备。
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

## 实机验收范围

当前主动 AWDL 入口只接受已确认的中国大陆室内场景、主信道 44 或 149、80 MHz。请求的传导功率上限为 6 dBm，板级与固件限制可以进一步收紧。实机验收应分别记录代理绑定与签名环境、SAR 查询结果、固件信道授权，以及双向 AirDrop 的落盘字节数、SHA-256 和整份用时。早期带内核过滤驱动版本的约 40 MB/s 成绩只适用于该版本的 iPhone → Windows 测试。
