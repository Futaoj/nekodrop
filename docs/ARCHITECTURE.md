# Architecture Overview

English · [简体中文](ARCHITECTURE.zh-CN.md) · [Back to README](../README.en.md) · [How it works](HOW-IT-WORKS.md)

The diagram below shows all four layers of nekodrop on Windows: nearby devices across three ecosystems, the user-mode client and rule-based relay, the virtual interfaces and system service, and the four user-mode radio backends driving the built-in Wi-Fi hardware through a UMDF register proxy.

```mermaid
flowchart TB
    subgraph Peers["Nearby Devices · Three Ecosystems"]
        direction LR
        Apple["Apple Devices<br/>iPhone · iPad · Mac<br/>Native AirDrop (AWDL + BLE)"]
        Android["Android MTA Phones<br/>OnePlus · OPPO · Xiaomi · vivo · Honor · realme<br/>MTA v2 (BLE + LAN)"]
        LocalSend["Cross-Platform Peers<br/>Android · iOS · macOS · Win · Linux<br/>LocalSend v2 (mTLS LAN)"]
    end

    subgraph Client["User Session · nekodrop.exe (C# WPF)"]
        direction TB
        UI["User Interface<br/>Radar · System Tray · Explorer Context Menu · Share Sheet · Rich Toasts"]
        Relay["Rule-Based Cross-Ecosystem Relay<br/>One-way / Two-way Per-Device Rules · Visual Flow Diagram"]
        subgraph Engines["Protocol Engines"]
            direction LR
            AirDropEng["AirDrop Engine (Python)<br/>mDNS · HTTPS 8771 · BLE Wake<br/>appleid (Rust, Contacts Only)"]
            MtaEng["Android MTA Engine (C#)<br/>BLE Discovery & Redacted Scanner<br/>Streaming ZIP_STORED Unpack"]
            LsEng["LocalSend Engine (C#)<br/>UDP / TCP 53317<br/>Mutual TLS Client Cert"]
        end
        UI --- Relay
        Relay <--> AirDropEng
        Relay <--> MtaEng
        Relay <--> LsEng
    end

    subgraph NetSvc["Virtual Interfaces & System Service"]
        direction LR
        Awdl["awdl0 Virtual Interface<br/>Wintun 8 MiB IPv6<br/>awdld (OWL AWDL Stack)"]
        Lan["LAN Data Path<br/>nekodrop TAP Adapter<br/>or Native Windows Wi-Fi / Ethernet"]
        Svc["NekoDropService (SYSTEM)<br/>CardCatalog · Concurrent / On-Demand<br/>Wi-Fi Manager · Recovery Watchdogs"]
    end

    subgraph Driver["Pure User-Mode Radio Drivers & Hardware (Zero Kernel Code)"]
        direction TB
        Radio["4 User-Mode Radio Backends (SHM / Pipe)<br/>Intel: iwxradio (AX200~AX211 / AX203 / BE200)<br/>MediaTek: mtradio (MT7902) · mtkradio (MT7922 / MT7921)<br/>Realtek: devourer (RTL8852CE)"]
        Proxy["UMDF Register-Proxy Driver (winawdl_regproxy)<br/>User-Mode DMA · No Secure Boot Off / No Test Signing"]
        Nic["Built-In PCIe / CNVi Wi-Fi Card<br/>MAC 0: AWDL (ch 6 / 44 / 149) · MAC 1: Router Wi-Fi Station"]
        Radio --> Proxy --> Nic
    end

    Apple <-->|"AWDL + BLE"| AirDropEng
    Android <-->|"BLE + LAN"| MtaEng
    LocalSend <-->|"LAN 53317"| LsEng

    AirDropEng <--> Awdl
    MtaEng <--> Lan
    LsEng <--> Lan
    UI -.->|"JSON Named Pipe"| Svc

    Awdl <-->|"4+4 MiB Lock-Free SHM"| Radio
    Lan <-->|"Ethernet Bridge (Concurrent)"| Radio
    Svc -->|"Card Ownership & Supervision"| Radio

    style Peers fill:transparent,stroke:#8b5cf6,stroke-width:1.5px
    style Client fill:transparent,stroke:#3b82f6,stroke-width:1.5px
    style Engines fill:transparent,stroke:#60a5fa,stroke-dasharray: 4 3
    style NetSvc fill:transparent,stroke:#10b981,stroke-width:1.5px
    style Driver fill:transparent,stroke:#f59e0b,stroke-width:1.5px
```

## Layer Summary

1. **Nearby Devices:** Connects to native Apple AirDrop, Android Mutual Transmission Alliance (MTA v2), and cross-platform LocalSend v2 peers under one unified radar and send window.
2. **User Session (`nekodrop.exe`):** Hosts the WPF UI, Explorer context menu, notifications, and the **rule-based cross-ecosystem relay**, coordinating the Python AirDrop engine (with Rust `appleid` for Contacts Only), the C# MTA v2 engine, and the C# LocalSend v2 engine.
3. **Virtual Interfaces & System Service:** `awdld` exposes `awdl0` (Wintun IPv6) for AirDrop; LAN traffic bridges through the `"nekodrop"` TAP adapter in Concurrent mode or uses native Windows Wi-Fi/Ethernet in On-Demand/wired setups; `NekoDropService` handles `CardCatalog` matching, mode switching, Wi-Fi management, and recovery watchdogs.
4. **User-Mode Radio Drivers & Hardware:** `winawdl_regproxy` (UMDF) maps PCIe/CNVi registers and interrupts into the user-mode radio process with zero custom kernel code, powering four user-mode backends across Intel, MediaTek, and Realtek cards.

For deeper internals, see [How it works](HOW-IT-WORKS.md), [Design notes](DESIGN.md), and [Client & service](CLIENT.md).
