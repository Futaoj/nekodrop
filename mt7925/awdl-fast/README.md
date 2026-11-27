# MT7925 E138 的 5 GHz AWDL 主机

NekoDrop 的安装程序通过 `scripts/build-awdl.ps1` 编译共用的 `awdl/awdld.exe`。构建脚本使用 `scripts/fetch-deps.ps1` 固定的 winawdl 版本，先应用 NekoDrop 通用改动，再从本目录应用主机和 OWL 补丁。需要重新构建时，请使用仓库中的构建脚本。重复构建会检查已有补丁。

服务启动主机时使用 `-c <44|149> -r -s <1|2>` 和 `NEKODROP_MT7925_DISCOVERY_WINDOW=1`。信道由无线后端实际授权和设置。`-s` 来自无线进程针对当前板卡和信道宽度确认的接收流数。省略该参数时，E138 使用一条接收流。主机用 VHT MCS map 宣告接收 MCS 0–9，发送宣告一条流、MCS 0–8。OWL 的短 HT TLV 提供接收 MCS 位图，E138 清除其中的发送 STBC 位并关闭 HE 广告。

快速主机通过无线进程的 HELLO 核对 `reserved[1] == 0x25`、启动参数指定的信道和 80 MHz，并实际打开收发各 4 MiB 的共享内存环。单播数据走共享内存，广播、多播和管理帧走无线进程的控制管道。取得 `awdl0` 接口索引后才输出 `MT7925 fast host ready | ch44/80` 或 `MT7925 fast host ready | ch149/80`。无线进程取得信道使用权另有独立就绪日志。

在 149 信道上，OWL 使用原有的发现窗口。在 44 信道上，发现调度复用了原有的多播队列。在最近 512 ms 内获得有效同步与信道序列，且双方都处于 44 信道的可发送时段时，一个额外的共享可用窗口最多发送一个排队的 IPv6 mDNS 包。仅有一个已确认的邻居时，旧多播时段若与该邻居错频，可以将队首 mDNS 延后一轮至紧邻的共同窗口。其他多播流量沿用 OWL 的原调度。

在仓库根目录执行 `.\scripts\build-awdl.ps1 -MinGW (Resolve-Path .\third_party\winlibs\mingw64\bin)` 即可生成 `third_party/winawdl/build/awdl/awdld.exe`。同目录下的 `frame_fixup_test.exe` 检查 E138 一条和两条接收流的实际广播字节，也检查未带标记的无线后端。`mt7925_discovery_test.exe` 检查共享内存、信道宽度、时序边界、发现流量类型和重复延期。两项测试都可离线运行。实际文件速率需用对端完成传输后测量。
