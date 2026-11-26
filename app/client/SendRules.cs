// SPDX-License-Identifier: GPL-2.0-only
using System.IO;

namespace NekoDrop;

enum SendProblemKind
{
    Folder,
    Missing,
    NotRegular,
    Unreadable,
    SameName,
}

/// <summary>One file-type reason, with the exact wording the warning window shows.</summary>
sealed record SendProblem(SendProblemKind Kind, string Name, string Title, string Lead, string Body);

/// <summary>
/// The AirDrop engine's sender accepts regular files only (sender.py raises
/// `not_a_file` for everything else), while the picker, drag/drop and Explorer
/// selection can leave folders or vanished paths in Model.Files. Keep every
/// file-type rule and its wording here, so the preflight check, the engine
/// fallback and the warning window cannot drift apart.
/// </summary>
static class SendRules
{
    /// <summary>The first reason the selected paths cannot be sent, or null when all are regular files.</summary>
    public static SendProblem? Check(IEnumerable<string> paths)
    {
        var list = paths.ToList();
        if (list.Count == 0) return null;
        foreach (var path in list)
        {
            if (Directory.Exists(path)) return FolderProblem(path);
            if (File.Exists(path)) continue;
            return ProblemFor(path);
        }
        return SameNameProblem(list);
    }

    /// <summary>The engine's rejection, explained with the same wording as the preflight check.</summary>
    public static SendProblem? FromEngine(string reason, IEnumerable<string> paths)
    {
        var list = paths.ToList();
        if (reason == "not_a_file")
        {
            var folder = list.FirstOrDefault(Directory.Exists);
            if (folder is not null) return FolderProblem(folder);
            var path = list.FirstOrDefault(path => !File.Exists(path)) ?? list.FirstOrDefault();
            return path is null ? null : ProblemFor(path);
        }
        if (reason == "same_name") return SameNameProblem(list);
        if (reason == "unreadable") return UnreadableProblem(list.FirstOrDefault() ?? "");
        return null;
    }

    public static string Name(string path)
    {
        if (string.IsNullOrEmpty(path)) return "";
        try
        {
            var root = Path.GetPathRoot(path);
            var trimmed = path.Length > (root?.Length ?? 0)
                ? path.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)
                : path;
            var name = Path.GetFileName(trimmed);
            return string.IsNullOrEmpty(name) ? path : name;
        }
        catch (Exception)
        {
            return path;
        }
    }

    public static string FailureStatus(string reason, IEnumerable<string> paths) => reason switch
    {
        "not_a_file" => L.T("send.unsupportedType"),
        "same_name" => L.T("send.sameNameShort"),
        "unreadable" => L.T("send.unreadableShort"),
        _ => L.F("send.genericFailure", reason),
    };

    public static string FailureDetail(string reason, IEnumerable<string> paths)
    {
        var problem = FromEngine(reason, paths);
        return problem is null ? L.F("send.genericFailure", reason) : problem.Lead + "\n" + problem.Body;
    }

    static SendProblem ProblemFor(string path)
    {
        if (Directory.Exists(path)) return FolderProblem(path);
        if (IsDevicePath(path)) return NotRegularProblem(path);
        try
        {
            var attributes = File.GetAttributes(path);
            if ((attributes & FileAttributes.Directory) != 0) return FolderProblem(path);
            return NotRegularProblem(path);
        }
        catch (FileNotFoundException)
        {
            return MissingProblem(path);
        }
        catch (DirectoryNotFoundException)
        {
            return MissingProblem(path);
        }
        catch (UnauthorizedAccessException)
        {
            return UnreadableProblem(path);
        }
        catch (ArgumentException)
        {
            return NotRegularProblem(path);
        }
        catch (IOException)
        {
            return NotRegularProblem(path);
        }
        catch (Exception)
        {
            return NotRegularProblem(path);
        }
    }

    /// <summary>
    /// Windows device names and named pipes are not files even though they can
    /// be opened. File.GetAttributes reports some of them as FileNotFound, so
    /// recognize the prefixes and reserved names before that call.
    /// </summary>
    static bool IsDevicePath(string path)
    {
        if (path.StartsWith(@"\\.\", StringComparison.Ordinal)
            || path.StartsWith(@"\\?\GLOBALROOT\", StringComparison.OrdinalIgnoreCase)
            || path.StartsWith(@"\\?\Volume{", StringComparison.OrdinalIgnoreCase))
            return true;
        var name = Name(path);
        var stem = name.Split('.', 2)[0];
        return stem.Equals("CON", StringComparison.OrdinalIgnoreCase)
            || stem.Equals("PRN", StringComparison.OrdinalIgnoreCase)
            || stem.Equals("AUX", StringComparison.OrdinalIgnoreCase)
            || stem.Equals("NUL", StringComparison.OrdinalIgnoreCase)
            || stem.Equals("CONIN$", StringComparison.OrdinalIgnoreCase)
            || stem.Equals("CONOUT$", StringComparison.OrdinalIgnoreCase)
            || stem.Equals("CLOCK$", StringComparison.OrdinalIgnoreCase)
            || IsNumberedDevice(stem, "COM", 1, 9)
            || IsNumberedDevice(stem, "LPT", 1, 9);
    }

    static bool IsNumberedDevice(string stem, string prefix, int first, int last)
    {
        if (stem.Length != prefix.Length + 1
            || !stem.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
            return false;
        var digit = stem[^1];
        return digit >= '0' + first && digit <= '0' + last;
    }

    static SendProblem FolderProblem(string path)
    {
        var name = Name(path);
        return new SendProblem(SendProblemKind.Folder, name,
            L.T("send.folderTitle"),
            L.F("send.folderLead", name),
            L.T("send.unsupportedType"));
    }

    static SendProblem MissingProblem(string path)
    {
        var name = Name(path);
        return new SendProblem(SendProblemKind.Missing, name,
            L.T("send.fileTitle"),
            L.F("send.missingLead", name),
            L.T("send.missingBody"));
    }

    static SendProblem NotRegularProblem(string path)
    {
        var name = Name(path);
        return new SendProblem(SendProblemKind.NotRegular, name,
            L.T("send.fileTitle"),
            L.F("send.notRegularLead", name),
            L.T("send.unsupportedType"));
    }

    static SendProblem UnreadableProblem(string path)
    {
        var name = Name(path);
        return new SendProblem(SendProblemKind.Unreadable, name,
            L.T("send.fileTitle"),
            L.F("send.unreadableLead", name),
            L.T("send.unreadableBody"));
    }

    static SendProblem? SameNameProblem(IEnumerable<string> paths)
    {
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var path in paths)
        {
            var name = Name(path);
            if (name.Length == 0) continue;
            if (!seen.Add(name))
                return new SendProblem(SendProblemKind.SameName, name,
                    L.T("send.sameTitle"),
                    L.F("send.sameLead", name),
                    L.T("send.sameNameBody"));
        }
        return null;
    }
}
