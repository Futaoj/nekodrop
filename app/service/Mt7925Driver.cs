// SPDX-License-Identifier: GPL-2.0-only
using System.ComponentModel;
using System.Runtime.InteropServices;
using System.Text;

namespace NekoDrop.Service;

internal static class Mt7925Driver
{
    public static bool MatchesPackage(Card.Info card, string service, ReadOnlySpan<byte> expectedInf) =>
        card.Problem == 0 && card.Service.Equals(service, StringComparison.OrdinalIgnoreCase) &&
        File.Exists(card.InfPath) && File.ReadAllBytes(card.InfPath).AsSpan().SequenceEqual(expectedInf);

    public static void RequireSingleBoard(Card.Info card)
    {
        if (Card.Candidates().Count(c => c.HardwareId.Equals(card.HardwareId, StringComparison.OrdinalIgnoreCase)) != 1 ||
            Card.Find(card.InstanceId) is null)
            throw new CardBindException("MT7925 needs exactly one matching E138 PCI adapter to switch drivers",
                new InvalidOperationException());
    }

    public static void Bind(Card.Info card, string inf)
    {
        // UpdateDriverForPlugAndPlayDevices binds every present device with this exact hardware ID.
        RequireSingleBoard(card);
        if (Card.BindNow(card.HardwareId, inf))
            throw new CardBindException("Windows requires a restart to finish switching the MT7925 driver",
                new Win32Exception(3010));
    }

    // RegProxy and the OEM Wi-Fi driver have different setup
    // classes. Look in the published Net packages even when the device currently
    // belongs to a different class. Prefer the OEM package recorded before takeover.
    public static string VendorInf(Card.Info card, string preferredInf)
    {
        string full = card.HardwareId;
        string subsystem = full[..full.IndexOf("&REV_", StringComparison.Ordinal)];
        string generic = subsystem[..subsystem.IndexOf("&SUBSYS_", StringComparison.Ordinal)];
        var ids = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
            { full, subsystem, generic + full[subsystem.Length..], generic };
        string directory = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "INF");
        var candidates = new DirectoryInfo(directory).EnumerateFiles("oem*.inf")
            .Where(f => (f.Attributes & FileAttributes.ReparsePoint) == 0)
            .OrderByDescending(f => f.FullName.Equals(preferredInf, StringComparison.OrdinalIgnoreCase))
            .ThenByDescending(f => f.LastWriteTimeUtc);
        foreach (var file in candidates)
        {
            IntPtr inf = SetupOpenInfFileW(file.FullName, "Net", 2 /* INF_STYLE_WIN4 */, IntPtr.Zero);
            if (inf == new IntPtr(-1)) continue;
            try
            {
                var provider = new StringBuilder(512);
                if (!SetupGetLineTextW(IntPtr.Zero, inf, "Version", "Provider", provider, provider.Capacity, out _) ||
                    !CardCatalog.IsVendorProviderMatch(card.Profile, provider.ToString())) continue;
                foreach (var raw in File.ReadLines(file.FullName))
                {
                    string line = raw.Split(';', 2)[0];
                    int assignment = line.IndexOf('=');
                    if (assignment < 0) continue;
                    if (line[(assignment + 1)..].Split(',').Skip(1).Any(field => ids.Contains(field.Trim().Trim('"'))))
                        return file.FullName;
                }
            }
            finally { SetupCloseInfFile(inf); }
        }
        throw new CardBindException($"No installed MediaTek Windows Wi-Fi INF matches {card.HardwareId}",
            new FileNotFoundException());
    }

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode)]
    static extern IntPtr SetupOpenInfFileW(string path, string className, uint style, IntPtr errorLine);
    [DllImport("setupapi.dll", CharSet = CharSet.Unicode)]
    static extern bool SetupGetLineTextW(IntPtr context, IntPtr inf, string section, string key,
        StringBuilder buffer, int length, out uint required);
    [DllImport("setupapi.dll")]
    static extern void SetupCloseInfFile(IntPtr inf);
}
