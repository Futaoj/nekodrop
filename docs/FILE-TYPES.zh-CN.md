# NekoDrop 文件类型支持清单

本清单是发送端规则，依据 AirDrop 引擎的 `airdrop/sender.py` 在 2026-11-27 实测。
判断对象是资源管理器里选中的**文件系统对象**，不是扩展名。

## 结论

引擎接受所有**普通文件**，没有扩展名黑名单。`UNIFORM_TYPES` 只是告诉接收端
这个文件对应哪种 Apple UTI；未知扩展名统一回退到 `public.data`，仍然可以发送。

会被拒绝的对象只有以下几类，外加一条发送层的同名规则：

| 被拒绝的对象 | 典型例子 | 引擎结果 | 客户端提示 |
|---|---|---|---|
| 文件夹、包或 bundle | 任意目录、`规则`、`.app`、`.pvt`、`.photoslibrary`、`.rtfd`、`.bundle`、`.framework`、`.plugin`、`.kext`、`.xcodeproj`、`.xcworkspace`、`.playground`、`.dSYM`、`.xcassets`、`.lproj`、目录形态的 `.pkg` | `not_a_file` | “不能直接发送文件夹” |
| 通过 junction 或目录符号链接到达的目录 | `dir-junction` | `not_a_file` | 同上 |
| 已删除、已移动或不存在的路径 | 被删掉的下载文件、改名后的文件、不可用的网络路径 | `not_a_file` | “不能发送该文件” |
| 非普通文件系统对象 | `\\.\NUL`、`CON`、`NUL`、`COM1`、`LPT1`、`\\.\pipe\…`、备用数据流 `file.txt:stream`、含 NUL 字符的非法路径 | `not_a_file` | “不能发送该文件” |
| 两个待发送文件基名相同（不区分大小写） | `a\same.txt` + `b\same.txt` | `same_name` | “不能一起发送同名文件” |

最后一条是名字冲突，不是文件类型。目录形态的包和普通目录走同一条规则：Windows
把它报告为目录，所以不能作为一个项目直接发送。**平铺的** `.pkg`、`.zip`、`.dmg`、
`.iso` 是普通文件，可以发送。

## 可以发送的对象

以下对象都通过了 `sender.outgoing()`，并生成了文件条目：

- 已知扩展名、未知扩展名、无扩展名的普通文件；
- 空文件、只读文件、隐藏文件、系统文件；
- Windows 快捷方式 `.lnk`，作为不透明文件发送；
- 长路径（实测 337 个字符）；
- ZIP 及其他压缩包，作为普通文件发送；
- 指向普通文件的符号链接，按目标文件的内容发送，不保留链接本身；
- 常规文档、图片、音频、视频、安装包和磁盘镜像。

UTI 表只是给接收端的提示：

| 扩展名 | UTI |
|---|---|
| `.jpg`、`.jpeg` | `public.jpeg` |
| `.png` | `public.png` |
| `.heic` | `public.heic` |
| `.gif` | `com.compuserve.gif` |
| `.mov` | `com.apple.quicktime-movie` |
| `.mp4` | `public.mpeg-4` |
| `.m4v` | `com.apple.m4v-video` |
| `.mp3` | `public.mp3` |
| `.pdf` | `com.adobe.pdf` |
| `.txt` | `public.plain-text` |
| `.zip` | `public.zip-archive` |
| 其他扩展名 | `public.data` |

`public.data` 仍然可以传过去。iOS 可能只能保存、不能直接预览，或者要借助能处理该
扩展名的 App 打开；这是接收端的体验限制，不是发送端“不支持的类型”。加扩展名黑名单
会破坏目前能发送的文件，所以客户端不做黑名单。

## 权限和占用

类型检查不会打开文件。因权限不足或被其他程序独占而无法读取的文件，会先通过类型
检查，发送时以 `unreadable` 失败；仍然弹出同一个警告卡片，标题为“不能发送该文件”，
正文说明权限或占用问题。

## 客户端规则

`SendRules.Check()` 在调用引擎前检查，`SendRules.FromEngine()` 处理引擎返回的
`send` 失败。两者返回同一个 `SendProblem` 记录，由 `WarningWindow` 统一渲染。
所有发送端的文件类型错误因此共用一套卡片布局、一套警告用语，以及同一句“不支持该类型
文件的发送操作”。

这个警告不显示“压缩成 ZIP”或“选中文件夹里的文件”等替代步骤，只说明不能发送。
