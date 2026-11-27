# 全景架构

[English](ARCHITECTURE.md) · 简体中文 · [返回 README](../README.md) · [工作原理](HOW-IT-WORKS.zh-CN.md)

下图展示了 nekodrop 在 Windows 上的完整四层结构：从周边三大生态设备、用户态客户端与跨生态规则中继，到虚拟网卡与系统服务，再到底层 4 套纯用户态无线电驱动和 UMDF 寄存器代理。

```mermaid
flowchart TB
    subgraph Peers["周边设备 · 三大生态"]
        direction LR
        Apple["Apple 设备<br/>iPhone · iPad · Mac<br/>原生 AirDrop（AWDL + BLE）"]
        Android["安卓互传联盟机型<br/>一加 · OPPO · 小米 · vivo · 荣耀 · 真我<br/>MTA v2（BLE + 局域网）"]
        LocalSend["全平台设备<br/>Android · iOS · macOS · Win · Linux<br/>LocalSend v2（mTLS 局域网）"]
    end

    subgraph Client["用户会话 · nekodrop.exe（C# WPF）"]
        direction TB
        UI["交互入口<br/>雷达主界面 · 系统托盘 · 右键菜单 · Windows 共享 · 通知预览"]
        Relay["跨生态规则中继<br/>单向 / 双向按设备规则自动转发 · 可视化流向图"]
        subgraph Engines["多协议传输引擎"]
            direction LR
            AirDropEng["AirDrop 引擎（Python）<br/>mDNS · HTTPS 8771 · BLE 唤醒<br/>appleid（Rust，仅限联系人）"]
            MtaEng["互传联盟引擎（C#）<br/>BLE 发现与脱敏诊断<br/>ZIP_STORED 流式解包"]
            LsEng["LocalSend 引擎（C#）<br/>UDP / TCP 53317<br/>双向 TLS 客户端证书"]
        end
        UI --- Relay
        Relay <--> AirDropEng
        Relay <--> MtaEng
        Relay <--> LsEng
    end

    subgraph NetSvc["虚拟网络接口与系统服务"]
        direction LR
        Awdl["awdl0 虚拟网卡<br/>Wintun 8 MiB IPv6<br/>awdld（OWL 协议栈）"]
        Lan["局域网通道<br/>nekodrop TAP 虚拟网卡<br/>或 Windows 原生 Wi-Fi / 有线"]
        Svc["NekoDropService（SYSTEM）<br/>CardCatalog 网卡目录 · 并发 / 按需切换<br/>Wi-Fi 管理 · 看门狗自动恢复"]
    end

    subgraph Driver["纯用户态无线电驱动与硬件层（零内核驱动）"]
        direction TB
        Radio["4 套用户态无线电后端（SHM / 命名管道）<br/>Intel: iwxradio（AX200~AX211 / AX203 / BE200）<br/>MediaTek: mtradio（MT7902）· mtkradio（MT7922 / MT7921）<br/>Realtek: devourer（RTL8852CE）"]
        Proxy["UMDF 寄存器代理驱动（winawdl_regproxy）<br/>用户态 DMA · 无需关闭安全启动 / 测试签名"]
        Nic["笔记本 / 主板自带 PCIe / CNVi 无线网卡<br/>MAC 0: AWDL（ch 6 / 44 / 149）· MAC 1: 路由器 Wi-Fi 上网"]
        Radio --> Proxy --> Nic
    end

    Apple <-->|"AWDL + BLE"| AirDropEng
    Android <-->|"BLE + LAN"| MtaEng
    LocalSend <-->|"LAN 53317"| LsEng

    AirDropEng <--> Awdl
    MtaEng <--> Lan
    LsEng <--> Lan
    UI -.->|"JSON 命名管道"| Svc

    Awdl <-->|"4+4 MiB 无锁 SHM"| Radio
    Lan <-->|"以太网桥接（并发模式）"| Radio
    Svc -->|"接管 / 还卡 / 监管"| Radio

    style Peers fill:transparent,stroke:#8b5cf6,stroke-width:1.5px
    style Client fill:transparent,stroke:#3b82f6,stroke-width:1.5px
    style Engines fill:transparent,stroke:#60a5fa,stroke-dasharray: 4 3
    style NetSvc fill:transparent,stroke:#10b981,stroke-width:1.5px
    style Driver fill:transparent,stroke:#f59e0b,stroke-width:1.5px
```

## 分层说明

1. **周边设备层**：同时面向 Apple 原生 AirDrop、安卓互传联盟（MTA v2）与跨平台 LocalSend v2，统一呈现在同一套雷达和发送窗口中。
2. **用户会话层（`nekodrop.exe`）**：负责雷达 UI、资源管理器右键、系统通知与**规则化跨生态中继**；内部调度 Python AirDrop 引擎（含 Rust `appleid` 证书工具）、C# MTA v2 引擎与 C# LocalSend v2 引擎。
3. **虚拟网络与系统服务层**：`awdld` 通过 Wintun 暴露 IPv6 接口 `awdl0` 跑 AirDrop；局域网流量在并发模式下经 `"nekodrop"` TAP 虚拟网卡桥接，在按需/有线模式下直接走 Windows 原生网卡；`NekoDropService` 负责 `CardCatalog` 网卡匹配、模式切换、Wi-Fi 站点管理与看门狗恢复。
4. **用户态驱动与硬件层**：`winawdl_regproxy`（UMDF）将 PCIe/CNVi 网卡寄存器与中断映射给用户态无线电进程，不运行任何自定义内核驱动，支持 Intel / MediaTek / Realtek 三大阵营 4 套用户态后端。

更多实现细节见 [工作原理](HOW-IT-WORKS.zh-CN.md)、[底层架构设计](DESIGN.md) 与 [客户端与服务说明](CLIENT.md)。
