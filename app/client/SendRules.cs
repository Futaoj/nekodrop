// SPDX-License-Identifier: GPL-2.0-only
using System.IO;

namespace NekoDrop;

/// <summary>
/// The AirDrop engine's sender accepts regular files only (sender.py raises
/// `not_a_file` for anything else), while the picker and drag/drop can leave
/// folders in Model.Files. Keep the rule and its wording in one place so the
/// preflight check and the engine-event fallback say the same thing.
/// </summary>
static class SendRules
{
    public static string? FirstFolder(IEnumerable<string> paths)
    {
        foreach (var path in paths)
            if (Directory.Exists(path)) return path;
        return null;
    }

    public static string Name(string path)
    {
        var trimmed = path.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        var name = Path.GetFileName(trimmed);
        return string.IsNullOrEmpty(name) ? trimmed : name;
    }

    public static bool IsFolderFailure(string reason, IEnumerable<string> paths) =>
        reason == "not_a_file" && FirstFolder(paths) is not null;

    public static string FailureStatus(string reason, IEnumerable<string> paths)
    {
        var folder = FirstFolder(paths);
        if (reason == "not_a_file" && folder is not null)
            return L.F("send.folderShort", Name(folder));
        if (reason == "not_a_file")
            return L.T("send.notFileShort");
        if (reason == "same_name")
            return L.T("send.sameNameShort");
        return L.F("send.genericFailure", reason);
    }

    public static string FailureDetail(string reason, IEnumerable<string> paths)
    {
        var folder = FirstFolder(paths);
        if (reason == "not_a_file" && folder is not null)
            return L.F("send.folderBody", Name(folder));
        if (reason == "not_a_file")
        {
            var missing = paths.FirstOrDefault(path => !File.Exists(path)) ?? paths.FirstOrDefault() ?? "";
            return L.F("send.notFileBody", Name(missing));
        }
        if (reason == "same_name")
            return L.T("send.sameNameBody");
        return L.F("send.genericFailure", reason);
    }
}
