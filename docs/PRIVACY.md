# Privacy / 隐私说明

**nekodrop does not collect telemetry and has no server of its own.** It does not transfer any information to other networked systems unless specifically requested by the user or the person installing or operating it. The connections it can make are listed below.

**nekodrop 不收集任何遥测数据，也没有自己的服务器。** 除非用户（或安装、操作它的人）明确要求，它不会把任何信息发送到其他联网系统。它可能建立的连接全部列在下面。

## English

| What | When | Where it goes | What is sent |
|---|---|---|---|
| AirDrop | You turn on receiving, or send a file | Apple devices next to you, over Wi-Fi (AWDL) and Bluetooth | What AirDrop needs: your device name, and the files you choose to send |
| Android Share (MTA, experimental) | Only when you turn it on in Settings | Android phones on your own Wi-Fi network | Your device name, and the files you choose to send |
| LocalSend (experimental) | Only when you turn it on in Settings | Devices running LocalSend on your own network (UDP multicast 224.0.0.167 and HTTPS on port 53317) | Your device name, and the files you choose to send |
| Update check | On by default; turn off in *Settings → Updates* | GitHub (`api.github.com`, `github.com`) | A normal HTTPS request for the release list; no personal data |
| Contacts Only with Apple ID (experimental) | Only when you sign in with your Apple ID | Apple (`gsa.apple.com` and related Apple sign-in services); Apple's own iCloud for Windows installer is downloaded once from `apple.com` | Your Apple ID and password go to Apple only, as in Apple's own apps. Your credentials and certificate stay on this PC |
| Link previews | When you receive a web link by AirDrop | The site in that link (for example YouTube or Bilibili) | A request for the page's title and thumbnail |
| Internet check | While nekodrop manages Wi-Fi | Microsoft's connectivity test (`msftconnecttest.com`), as Windows itself does | A plain HTTP request |
| Wi-Fi | Concurrent mode | The Wi-Fi network you choose | Normal network traffic |

The **diagnostic report** (Start menu → *nekodrop - report*) is made on your PC and is never uploaded by nekodrop. It replaces SSIDs, MAC and IP addresses and user/computer names with placeholders; you decide whether to post it.

Logs stay on your PC (`C:\ProgramData\nekodrop`, `%LocalAppData%\nekodrop`). Uninstalling removes nekodrop and restores Windows' own Wi-Fi driver.

## 中文

| 内容 | 什么时候 | 发往哪里 | 发送什么 |
|---|---|---|---|
| AirDrop | 你打开接收或发送文件时 | 身边的苹果设备（Wi-Fi AWDL 和蓝牙） | AirDrop 必需的信息：设备名，以及你选择发送的文件 |
| 安卓互传（MTA，实验性） | 只在你在设置里打开后 | 同一 Wi-Fi 下的安卓手机 | 设备名，以及你选择发送的文件 |
| LocalSend 互传（实验性） | 只在你在设置里打开后 | 同一网络里运行 LocalSend 的设备（UDP 组播 224.0.0.167、HTTPS 53317 端口） | 设备名，以及你选择发送的文件 |
| 检查更新 | 默认开启，可在 *设置 → 更新* 关闭 | GitHub（`api.github.com`、`github.com`） | 普通的 HTTPS 版本列表请求，不含个人信息 |
| 仅限联系人 / Apple ID（实验性） | 只在你登录 Apple ID 时 | Apple（`gsa.apple.com` 等 Apple 登录服务）；会从 `apple.com` 下载一次 Apple 官方的 iCloud for Windows 安装包 | Apple ID 和密码只发给 Apple，和 Apple 自家应用一样；凭据和证书只保存在本机 |
| 链接预览 | 通过 AirDrop 收到网页链接时 | 该链接所在网站（例如 YouTube、哔哩哔哩） | 获取页面标题和缩略图的请求 |
| 联网检测 | nekodrop 管理 Wi-Fi 时 | 微软的联网检测地址（`msftconnecttest.com`），和 Windows 自己的做法一样 | 普通 HTTP 请求 |
| Wi-Fi | 同时连 Wi-Fi（Concurrent）模式 | 你选择连接的 Wi-Fi 网络 | 正常的上网流量 |

**诊断报告**（开始菜单 → *nekodrop - report*）在本机生成，nekodrop 不会上传。SSID、MAC、IP 地址和用户名、计算机名都会替换成占位符，是否发布由你决定。

日志只保存在本机（`C:\ProgramData\nekodrop`、`%LocalAppData%\nekodrop`）。卸载会移除 nekodrop 并恢复 Windows 自带的 Wi-Fi 驱动。
