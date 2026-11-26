# NekoDrop commit notes — 2026-11-27

This change finishes the AirDrop folder-send failure handling reported in the 02:05 diagnostic bundle. It also records the transfer evidence used to separate the folder failure from the earlier throughput work.

## English

### What changed

- Added `app/client/SendRules.cs`, one shared rule for what the AirDrop sender can accept.
- `App.SendTo` now preflights the selected paths before creating a send id:
  - a directory is rejected immediately;
  - a path that no longer exists is rejected with its own message;
  - valid files keep the existing send path unchanged.
- Added a detailed `MessageBox` for a folder: reason, the engine limit, the `not_a_file` code, and three alternatives (select files inside the folder, compress the folder to ZIP, or send in smaller batches).
- `not_a_file` events from the engine are no longer rendered as the old generic `发送失败 · not_a_file`. The same detailed reason/limit/alternatives text is used in the device status, transfer history, and failure toast; a folder that slips past preflight also gets the detailed modal.
- Added bilingual entries in `L10n.cs` for folder, missing-file, duplicate-name, and generic failures.
- The engine itself was not changed: `sender.py` still accepts regular files only. The fix prevents the predictable failure before the engine call and keeps a correct fallback if the state changes after preflight.

### Effect

- Before: selecting `Desktop\测试` produced 12 `not_a_file` failures between 02:02:42 and 02:03:48; the UI only showed `发送失败 · not_a_file`, so the user had no reason or next step.
- After: clicking send with a folder shows “不能直接发送文件夹” with the limitation and ZIP/file alternatives, and no engine send is attempted.
- File sends are unchanged. In the same log, 95.0 MB and 190.1 MB file sends completed at 22.6 MB/s and 21.3 MB/s; 4 ZIP files (199,330,824 B) were received at about 21.4 MB/s.
- The data-plane switch is unchanged: off = 20.10 Mbps (10 threads), on = 162.58 Mbps (10 threads) / 166.15 Mbps (40 threads), best observed 40-thread run = 199.97 Mbps. This commit does not alter the switch or its default (off).

### Verification

- `dotnet publish` for the client completed with exit code 0.
- Runtime checks against the built assembly:
  - a regular file is not treated as a folder;
  - a directory is detected by `SendRules.FirstFolder`;
  - Chinese and English folder messages contain the reason, limitation, and all three alternatives;
  - a missing path gets the separate missing-file message.
- The reported folder path and the repeated `not_a_file` events were reproduced from the attached logs.
- Full timeline and filtered evidence are in the separate `nekodrop-key-timeline-20261127.md` deliverable.

## 中文

### 改动内容

- 新增 `app/client/SendRules.cs`，把“AirDrop 发送端只接受普通文件”的规则集中到一处。
- `App.SendTo` 在创建发送任务前先做预检：
  - 目录直接拦截；
  - 已不存在或不是普通文件的路径单独提示；
  - 正常文件保持原有发送流程，不受影响。
- 文件夹失败时显示详细弹窗：具体原因、引擎限制、`not_a_file` 状态码，以及三个替代方案（打开文件夹选择文件、压缩成 ZIP、分批发送）。
- 引擎返回的 `not_a_file` 不再显示为旧的通用“发送失败 · not_a_file”：设备状态、传输历史、失败通知统一使用同一套详细文案；如果文件夹在预检之后才进入发送状态，也会弹出同样的详细说明。
- 在 `L10n.cs` 中补充文件夹、文件缺失、同名文件、通用失败的中英双语文案。
- 引擎代码未改：`sender.py` 仍然只接受普通文件。本次是在调用引擎前拦住可预见的失败，同时保留状态变化后的兜底提示。

### 效果变化

- 修改前：选择桌面文件夹 `测试` 后，02:02:42–02:03:48 连续出现 12 次 `not_a_file` 失败；界面只显示“发送失败 · not_a_file”，用户不知道原因，也不知道下一步怎么做。
- 修改后：发送文件夹会显示“不能直接发送文件夹”，说明限制，并给出选择文件或压缩 ZIP 的替代方案；不会向引擎发起无效发送。
- 文件发送保持原样：同一份日志中，95.0 MB 与 190.1 MB 文件分别以 22.6 MB/s、21.3 MB/s 发送成功；4 个 ZIP（199,330,824 B）以约 21.4 MB/s 接收成功。
- 数据面实验开关不变：关闭时 10 线程 20.10 Mbps，开启时 10 线程 162.58 Mbps、40 线程 166.15 Mbps，历史最好 40 线程 199.97 Mbps。本次提交不改开关，默认仍为关闭。

### 验证

- 客户端 `dotnet publish` 退出码 0。
- 对构建产物做了运行时检查：
  - 普通文件不会被误判为文件夹；
  - 目录能被 `SendRules.FirstFolder` 识别；
  - 中英文文件夹提示都包含原因、限制和三个替代方案；
  - 路径不存在时返回单独的文件缺失提示。
- 已从附件日志复现文件夹路径与多次 `not_a_file` 失败。
- 完整时间线和过滤后的证据见单独的 `nekodrop-key-timeline-20261127.md` 交付文件。
