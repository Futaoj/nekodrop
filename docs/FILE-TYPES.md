# File types NekoDrop can and cannot send

This is the sender-side contract, checked against the AirDrop engine's
`airdrop/sender.py` on 2026-11-27. It is about the **object** selected in
Explorer, not about the file extension.

## Short version

The engine accepts every **regular file**, whatever its extension. It has no
extension blacklist. `UNIFORM_TYPES` only chooses the Apple UTI advertised to the
receiver; an unknown extension falls back to `public.data` and still uploads.

What it refuses is an object that `os.path.isfile()` does not accept, plus one
send-level collision:

| Refused object | Examples | Engine result | Client warning |
|---|---|---|---|
| Directory, package or bundle | any folder, `Rules`, `.app`, `.pvt`, `.photoslibrary`, `.rtfd`, `.bundle`, `.framework`, `.plugin`, `.kext`, `.xcodeproj`, `.xcworkspace`, `.playground`, `.dSYM`, `.xcassets`, `.lproj`, a directory-shaped `.pkg` | `not_a_file` | "不能直接发送文件夹" |
| Directory reached through a junction or directory symlink | a junction such as `dir-junction` | `not_a_file` | "不能直接发送文件夹" |
| Missing or moved path | a deleted download, a renamed file, an unavailable network path | `not_a_file` | "不能发送该文件" |
| Non-regular filesystem object | `\\.\NUL`, `CON`, `NUL`, `COM1`, `LPT1`, `\\.\pipe\…`, an alternate data stream such as `file.txt:stream`, an invalid path with a NUL character | `not_a_file` | "不能发送该文件" |
| Two selected files with the same base name (case-insensitive) | `a\same.txt` + `b\same.txt` | `same_name` | "不能一起发送同名文件" |

The last row is a send-level naming rule, not a file-type rule. A directory-shaped
package is the same case as a directory: Windows reports it as a directory, so it
cannot be sent as one item. A **flat** `.pkg`, `.zip`, `.dmg` or `.iso` file is a
regular file and is accepted.

## What is accepted

These all reached `sender.outgoing()` and produced a file entry:

- regular files with a known extension, an unknown extension, or no extension;
- empty files, read-only files, hidden files and system files;
- Windows shortcuts (`.lnk`) as ordinary opaque files;
- long paths (tested at 337 characters);
- ZIP and other archives as files;
- a symbolic link to a regular file, treated as the file it points to (the link
  itself is not preserved);
- all ordinary documents, images, audio, video, installers and disk images.

The UTI table is only a hint for the receiver:

| Extension | UTI |
|---|---|
| `.jpg`, `.jpeg` | `public.jpeg` |
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
| anything else | `public.data` |

`public.data` is still transferable. iOS may save it without a preview or open
it only through an app that handles the extension; that is a receiver UX limit,
not a sender-side unsupported type. Adding an extension blacklist would break
files that work today, so the client does not do that.

## Per-file permission and busy files

The type check does not open the file. A file that cannot be read because of
permissions or another program holding it exclusively is accepted by the type
check, then fails at send time as `unreadable`; it gets the same warning card with
"文件无法读取" and the permission/busy-file explanation.

## Client rule

`SendRules.Check()` runs before the engine call and `SendRules.FromEngine()` runs
on an engine `send` failure. Both return the same `SendProblem` record, and
`WarningWindow` renders it. All sender file-type errors therefore use one card
layout, one warning vocabulary and one exact sentence for an unsupported object:

> 不支持该类型文件的发送操作。

No "compress it to ZIP" or "select the files inside" replacement steps are shown
for this warning.
