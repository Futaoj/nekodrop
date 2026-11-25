// SPDX-License-Identifier: GPL-2.0-only
// Clipboard dual-format image+file copy and HEIC/Live Photo JPEG copy UX
// inspired by DropBridge (https://github.com/ndpyzwy-0w0/DropBridge).
using System.Collections.Specialized;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Media.Imaging;

namespace NekoDrop;

/// <summary>
/// File, URL, clipboard, and HEIC/Live Photo helpers shared by notifications,
/// the main flyout, and the transfer history window.
/// </summary>
public static class MediaHelper
{
    static readonly HashSet<string> ImageExts = new(StringComparer.OrdinalIgnoreCase)
    {
        ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".tif", ".tiff", ".heic", ".heif", ".webp",
    };

    static readonly HashSet<string> ToastHeroExts = new(StringComparer.OrdinalIgnoreCase)
    {
        ".png", ".jpg", ".jpeg", ".gif", ".bmp",
    };

    static readonly HashSet<string> HeicExts = new(StringComparer.OrdinalIgnoreCase)
    {
        ".heic", ".heif",
    };

    static readonly HashSet<string> MotionExts = new(StringComparer.OrdinalIgnoreCase)
    {
        ".mov", ".mp4",
    };

    public static bool IsImage(string path) => ImageExts.Contains(Path.GetExtension(path));
    public static bool IsToastPreviewable(string path) => ToastHeroExts.Contains(Path.GetExtension(path));
    public static bool IsHeic(string path) => HeicExts.Contains(Path.GetExtension(path));
    public static bool IsMotion(string path) => MotionExts.Contains(Path.GetExtension(path));

    /// <summary>
    /// Security boundary for nekodrop: protocol actions: ensures a path lies inside
    /// the configured SaveFolder so untrusted external URIs cannot read arbitrary files.
    /// </summary>
    public static bool IsInsideFolder(string file, string folder)
    {
        try
        {
            var root = Path.GetFullPath(folder).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
            var full = Path.GetFullPath(file);
            return full.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);
        }
        catch (Exception) { return false; }
    }

    /// <summary>
    /// Copies a web URL, a picture (both Bitmap for chat apps and FileDrop for Explorer),
    /// or one/more files to the Windows clipboard. Must be called on the UI (STA) thread.
    /// </summary>
    public static bool CopyToClipboard(IEnumerable<string> paths, string? url, out string label, out string error)
    {
        label = "";
        error = "";
        try
        {
            if (TransferHistory.IsWebUrl(url))
            {
                Clipboard.SetText(url!);
                label = TransferHistory.ShortUrlLabel(url!);
                return true;
            }
            var existing = paths.Where(p => File.Exists(p) || Directory.Exists(p)).Distinct(StringComparer.OrdinalIgnoreCase).ToList();
            if (existing.Count == 0)
            {
                error = L.T("clip.missing");
                return false;
            }

            // If a single .url / .webloc shortcut is copied, copy its URL text directly.
            if (existing.Count == 1)
            {
                var linkUrl = TransferHistory.TryReadUrlFile(existing[0]);
                if (TransferHistory.IsWebUrl(linkUrl))
                {
                    Clipboard.SetText(linkUrl);
                    label = TransferHistory.ShortUrlLabel(linkUrl);
                    return true;
                }
            }

            var drop = new StringCollection();
            foreach (var p in existing) drop.Add(p);
            var data = new DataObject();
            data.SetFileDropList(drop);

            // For a single image (or a Live Photo whose primary file is an image),
            // also put the decoded Bitmap on the clipboard so pasting in WeChat/QQ/etc.
            // sends the picture directly while Explorer still pastes the file.
            var imageFile = existing.FirstOrDefault(p => File.Exists(p) && IsImage(p));
            if (imageFile is not null && (existing.Count == 1 || existing.All(p => IsImage(p) || IsMotion(p))))
            {
                var bmp = TryLoadBitmap(imageFile);
                if (bmp is not null) data.SetImage(bmp);
            }

            Clipboard.SetDataObject(data, true);
            label = existing.Count == 1 ? Path.GetFileName(existing[0]) : L.F("done.items", existing.Count);
            return true;
        }
        catch (Exception e)
        {
            error = e.Message;
            return false;
        }
    }

    /// <summary>Opens a web URL in the default browser, or reveals a file/folder in Explorer.</summary>
    public static bool OpenRecord(TransferRecord record)
    {
        if (record.IsLink && TransferHistory.IsWebUrl(record.Url))
            return OpenUrl(record.Url);
        var path = record.Paths.FirstOrDefault(p => File.Exists(p) || Directory.Exists(p));
        return path is not null && RevealInExplorer(path);
    }

    public static bool OpenUrl(string url)
    {
        if (!TransferHistory.IsWebUrl(url)) return false;
        try
        {
            Process.Start(new ProcessStartInfo(url) { UseShellExecute = true });
            return true;
        }
        catch (Exception) { return false; }
    }

    public static bool RevealInExplorer(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                Process.Start("explorer.exe", $"/select,\"{path}\"");
                return true;
            }
            if (Directory.Exists(path))
            {
                Process.Start("explorer.exe", $"\"{path}\"");
                return true;
            }
        }
        catch (Exception) { }
        return false;
    }

    static BitmapSource? TryLoadBitmap(string path)
    {
        try
        {
            using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            var decoder = BitmapDecoder.Create(fs, BitmapCreateOptions.PreservePixelFormat, BitmapCacheOption.OnLoad);
            var frame = decoder.Frames.FirstOrDefault();
            if (frame is null) return null;
            var oriented = ApplyExifOrientation(frame);
            oriented.Freeze();
            return oriented;
        }
        catch (Exception) { return null; }
    }

    /// <summary>
    /// Converts a .heic / .heif photo into a sibling .jpg copy using Windows WIC
    /// (requires Microsoft's HEIF Image Extensions on Windows 10/11; fails gracefully if absent).
    /// </summary>
    public static bool TryConvertHeicToJpg(string sourcePath, out string jpgPath, out string error)
    {
        jpgPath = "";
        error = "";
        if (!File.Exists(sourcePath) || !IsHeic(sourcePath)) return false;
        try
        {
            var dir = Path.GetDirectoryName(sourcePath)!;
            var stem = Path.GetFileNameWithoutExtension(sourcePath);
            var target = UniquePath(dir, stem, ".jpg");

            using var fs = new FileStream(sourcePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            var decoder = BitmapDecoder.Create(fs, BitmapCreateOptions.PreservePixelFormat, BitmapCacheOption.OnLoad);
            var frame = decoder.Frames.FirstOrDefault();
            if (frame is null) { error = "no image frame"; return false; }
            var oriented = ApplyExifOrientation(frame);

            var encoder = new JpegBitmapEncoder { QualityLevel = 92 };
            encoder.Frames.Add(BitmapFrame.Create(oriented));
            var tmp = target + ".tmp";
            using (var outStream = new FileStream(tmp, FileMode.Create, FileAccess.Write, FileShare.None))
                encoder.Save(outStream);
            File.Move(tmp, target, overwrite: true);
            jpgPath = target;
            return true;
        }
        catch (Exception e)
        {
            error = e.Message;
            return false;
        }
    }

    static BitmapSource ApplyExifOrientation(BitmapFrame frame)
    {
        try
        {
            if (frame.Metadata is BitmapMetadata meta && meta.ContainsQuery("System.Photo.Orientation"))
            {
                var raw = meta.GetQuery("System.Photo.Orientation");
                var orientation = raw is ushort u ? u : raw is int i ? (ushort)i : (ushort)1;
                System.Windows.Media.Transform? transform = orientation switch
                {
                    2 => new System.Windows.Media.ScaleTransform(-1, 1),
                    3 => new System.Windows.Media.RotateTransform(180),
                    4 => new System.Windows.Media.ScaleTransform(1, -1),
                    6 => new System.Windows.Media.RotateTransform(90),
                    8 => new System.Windows.Media.RotateTransform(270),
                    _ => null,
                };
                if (transform is not null)
                    return new TransformedBitmap(frame, transform);
            }
        }
        catch (Exception) { /* metadata missing or codec doesn't expose EXIF orientation */ }
        return frame;
    }

    static string UniquePath(string dir, string stem, string ext)
    {
        var candidate = Path.Combine(dir, stem + ext);
        if (!File.Exists(candidate)) return candidate;
        for (var i = 2; i < 1000; i++)
        {
            var next = Path.Combine(dir, $"{stem} ({i}){ext}");
            if (!File.Exists(next)) return next;
        }
        return Path.Combine(dir, $"{stem}-{Guid.NewGuid().ToString("N")[..6]}{ext}");
    }

    public static long SumBytes(IEnumerable<string> paths)
    {
        long total = 0;
        foreach (var p in paths)
        {
            try
            {
                if (File.Exists(p)) total += new FileInfo(p).Length;
            }
            catch (Exception) { }
        }
        return total;
    }
}
