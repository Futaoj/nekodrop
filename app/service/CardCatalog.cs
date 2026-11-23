// SPDX-License-Identifier: GPL-2.0-only
namespace NekoDrop.Service;

/// <summary>Which user-mode radio drives a card family.</summary>
public enum RadioBackendKind
{
    /// <summary>nekodrop-radio: the Intel 22000 / AX210 driver ported from ForbiddenItlwm (iwxradio).</summary>
    Iwx,
    /// <summary>winawdl's devourer (Realtek RTL8852CE): monitor and inject only, no station.</summary>
    Devourer,
    /// <summary>mtkradio: user-mode MediaTek MT7921 / MT7922 (RZ608 / RZ616) Connac2 driver.</summary>
    MtkRadio,
}

/// <summary>
/// What a card family can do in nekodrop. The service asks these instead of
/// assuming Intel, so a card that cannot keep Wi-Fi beside AirDrop (no station
/// in its radio) runs on demand, and radio options it lacks are never passed.
/// </summary>
/// <param name="SupportsConcurrentWifi">the radio runs a Wi-Fi station beside AWDL (mode Concurrent)</param>
/// <param name="SupportsFastSend">the radio takes --tx-window (experimental faster sending)</param>
/// <param name="SupportsAggregate">the radio takes --aggregate (experimental A-MPDU sending)</param>
/// <param name="SupportsKernelDriver">nekodrop-wlan.sys can own the card (Windows' Wi-Fi and AWDL on one driver)</param>
/// <param name="MinBarBytes">the smallest register BAR the register proxy must map (driver/regproxy MIN_BAR_LEN)</param>
public sealed record AdapterCapabilities(bool SupportsConcurrentWifi, bool SupportsFastSend, bool SupportsAggregate,
                                         bool SupportsKernelDriver, uint MinBarBytes)
{
    /// <summary>
    /// The Intel cards nekodrop runs today. Also what the service assumes before
    /// it has seen a card, so a missing or unreadable card behaves as before.
    /// </summary>
    public static readonly AdapterCapabilities IntelDefault = new(true, true, true, true, 0x2000);
}

/// <summary>
/// One PCI device nekodrop knows: who makes it, which radio drives it, what it
/// can do, which register-proxy INF takes it, and how the vendor's own driver
/// is told apart from the proxy when the card is given back.
/// </summary>
/// <param name="VendorId">PCI vendor ID, four upper-case hex digits ("8086")</param>
/// <param name="DeviceId">PCI device ID, four upper-case hex digits ("43F0")</param>
/// <param name="VendorName">for logs and messages ("Intel's INF is not known")</param>
/// <param name="FamilyName">the chip family, for logs and --status ("AX200")</param>
/// <param name="Backend">which user-mode radio drives it</param>
/// <param name="Capabilities">what the service may ask of it (mode, radio options, kernel driver, BAR)</param>
/// <param name="ProxyInfName">the register proxy's INF in the install folder's driver\ directory</param>
/// <param name="ProxyService">the service a card bound to the proxy runs under</param>
/// <param name="ProxyProvider">the Provider in the proxy's INF: how it is told apart once Windows renamed it oemNN.inf</param>
/// <param name="VendorProviderPatterns">substrings of the vendor driver's ProviderName (OEM repackages included)</param>
/// <param name="Active">false: listed for diagnostics and the self-test only, never taken</param>
public sealed record AdapterProfile(string VendorId, string DeviceId, string VendorName, string FamilyName,
                                    RadioBackendKind Backend, AdapterCapabilities Capabilities,
                                    string ProxyInfName, string ProxyService, string ProxyProvider,
                                    string[] VendorProviderPatterns, bool Active)
{
    /// <summary>The hardware ID Windows binds a driver to (UpdateDriverForPlugAndPlayDevices).</summary>
    public string HardwareId => $"PCI\\VEN_{VendorId}&DEV_{DeviceId}";

    /// <summary>The proxy INF's name without extension, as found inside a renamed oemNN.inf (issue #12).</summary>
    public string ProxyInfStem => Path.GetFileNameWithoutExtension(ProxyInfName);
}

/// <summary>
/// The service's single list of Wi-Fi cards and what each can do. Pure data and
/// logic (no SetupAPI, registry or files), so `NekoDropService --selftest` checks
/// it on a build machine without a card or administrator rights.
///
/// Keep the active Intel IDs in step with `kDeviceIds` in iwxradio/src/cards.h,
/// the register proxy's INF, `$SupportedIntelIds` in installer/report.ps1 and the
/// check in installer/install.ps1 (docs/HARDWARE.md).
/// </summary>
public static class CardCatalog
{
    // nekodrop's one register proxy (driver/regproxy). A future Realtek card
    // goes through the same proxy once it picks its BAR by device (HARDWARE.md).
    const string ProxyInf = "winawdl_regproxy.inf";
    const string ProxyService = "WUDFRd";
    const string ProxyProvider = "winairdrop project";
    // Killer (Rivet Networks) cards are Intel chips with Intel's driver repackaged.
    static readonly string[] IntelProviders = ["Intel", "Killer", "Rivet Networks"];
    static readonly string[] MediaTekProviders = ["MediaTek", "AMD", "Lenovo", "HP", "ASUS", "Microsoft"];
    static readonly AdapterCapabilities MediaTekCaps = new(false, false, false, false, 0x100000);

    static AdapterProfile Intel(string device, string family) =>
        new("8086", device, "Intel", family, RadioBackendKind.Iwx, AdapterCapabilities.IntelDefault,
            ProxyInf, ProxyService, ProxyProvider, IntelProviders, Active: true);

    static AdapterProfile MediaTek(string device, string family) =>
        new("14C3", device, "MediaTek", family, RadioBackendKind.MtkRadio, MediaTekCaps,
            ProxyInf, ProxyService, ProxyProvider, MediaTekProviders, Active: true);

    /// <summary>Every card listed, taken or not.</summary>
    public static readonly IReadOnlyList<AdapterProfile> AllProfiles =
    [
        // Qu/QuZ CNVi: AX201, 9560 class.
        Intel("43F0", "AX201 / 9560 (Qu/QuZ)"), Intel("A0F0", "AX201 / 9560 (Qu/QuZ)"),
        Intel("02F0", "AX201 / 9560 (Qu/QuZ)"), Intel("06F0", "AX201 / 9560 (Qu/QuZ)"),
        Intel("34F0", "AX201 / 9560 (Qu/QuZ)"), Intel("3DF0", "AX201 / 9560 (Qu/QuZ)"),
        Intel("4DF0", "AX201 / 9560 (Qu/QuZ)"),
        Intel("2723", "AX200"),
        // The AX210 family (Ty/So): AX210, AX211, AX411.
        Intel("2725", "AX210 / AX211 / AX411"), Intel("7A70", "AX210 / AX211 / AX411"),
        Intel("7AF0", "AX210 / AX211 / AX411"), Intel("51F0", "AX210 / AX211 / AX411"),
        Intel("51F1", "AX210 / AX211 / AX411"), Intel("54F0", "AX210 / AX211 / AX411"),
        Intel("7F70", "AX210 / AX211 / AX411"),
        // MediaTek Connac2 (MT7921 / MT7922 / AMD RZ608 / RZ616) via user-mode mtkradio.exe.
        MediaTek("0616", "MT7922 (RZ616)"),
        MediaTek("7922", "MT7922"),
        MediaTek("7961", "MT7921"),
        MediaTek("0608", "MT7921 (RZ608)"),
        // Not taken (docs/HARDWARE.md, step 4): winawdl drives it with devourer,
        // which has no station (so no Wi-Fi beside AirDrop), and its registers
        // are a BAR of at least 64 KiB. The proxy's INF does not match it yet.
        new AdapterProfile("10EC", "C852", "Realtek", "RTL8852CE", RadioBackendKind.Devourer,
            new AdapterCapabilities(false, false, false, false, 0x10000),
            ProxyInf, ProxyService, ProxyProvider, ["Realtek"], Active: false),
    ];

    /// <summary>The cards the service takes.</summary>
    public static readonly IReadOnlyList<AdapterProfile> ActiveProfiles = AllProfiles.Where(p => p.Active).ToArray();

    /// <summary>
    /// Reads the vendor and device IDs from a PnP instance or hardware ID
    /// ("PCI\VEN_8086&amp;DEV_43F0&amp;SUBSYS_00748086&amp;REV_00\3&amp;11583659&amp;0&amp;A3"),
    /// in any case. The device ID must end at '&amp;', '\' or the end, so
    /// "DEV_43F01" is not "DEV_43F0".
    /// </summary>
    public static bool TryParsePciId(string instanceId, out string vendorId, out string deviceId)
    {
        vendorId = deviceId = "";
        const string pci = "PCI\\VEN_", dev = "&DEV_";
        var s = instanceId ?? "";
        if (s.Length < pci.Length + 4 + dev.Length + 4 || !s.StartsWith(pci, StringComparison.OrdinalIgnoreCase)) return false;
        var v = s.Substring(pci.Length, 4);
        if (!IsHex4(v) || string.Compare(s, pci.Length + 4, dev, 0, dev.Length, StringComparison.OrdinalIgnoreCase) != 0) return false;
        var at = pci.Length + 4 + dev.Length;
        var d = s.Substring(at, 4);
        if (!IsHex4(d)) return false;
        if (s.Length > at + 4 && s[at + 4] is not ('&' or '\\')) return false;
        vendorId = v.ToUpperInvariant();
        deviceId = d.ToUpperInvariant();
        return true;
    }

    static bool IsHex4(string s) => s.Length == 4 && s.All(Uri.IsHexDigit);

    /// <summary>The profile of a card the service takes, or null (unknown, Ethernet, or listed but not active).</summary>
    public static AdapterProfile? MatchActive(string instanceId) => Classify(instanceId) is { Active: true } p ? p : null;

    /// <summary>Any listed profile, active or not (diagnostics and the self-test).</summary>
    public static AdapterProfile? Classify(string instanceId) =>
        TryParsePciId(instanceId, out var v, out var d)
            ? AllProfiles.FirstOrDefault(p => p.VendorId == v && p.DeviceId == d)
            : null;

    /// <summary>
    /// The mode the card can run: one whose radio has no station cannot keep
    /// Wi-Fi beside AirDrop, so Concurrent becomes OnDemand (Windows keeps
    /// Wi-Fi, the card is taken only for AirDrop). Every Intel card supports
    /// Concurrent, so for them this is the requested mode unchanged.
    /// </summary>
    public static NetMode EffectiveMode(NetMode requested, AdapterCapabilities caps) =>
        requested == NetMode.Concurrent && !caps.SupportsConcurrentWifi ? NetMode.OnDemand : requested;

    /// <summary>
    /// Whether a bound driver's provider is the card vendor's own driver: never
    /// the register proxy, otherwise any of the vendor's provider names.
    /// </summary>
    public static bool IsVendorProviderMatch(AdapterProfile profile, string provider)
    {
        if (string.IsNullOrWhiteSpace(provider)) return false;
        if (string.Equals(provider.Trim(), profile.ProxyProvider, StringComparison.OrdinalIgnoreCase)) return false;
        return profile.VendorProviderPatterns.Any(p => provider.Contains(p, StringComparison.OrdinalIgnoreCase));
    }

    /// <summary>Offline checks of the list and the functions above; returns the failures (empty: all passed).</summary>
    public static IReadOnlyList<string> SelfTest()
    {
        var fail = new List<string>();
        void Check(bool ok, string what) { if (!ok) fail.Add(what); }

        // The 15 Intel IDs + 4 MediaTek Connac2 IDs (MT7921/MT7922).
        string[] intel = ["43F0", "A0F0", "02F0", "06F0", "34F0", "3DF0", "4DF0",
                          "2723", "2725", "7A70", "7AF0", "51F0", "51F1", "54F0", "7F70"];
        string[] mtk = ["0616", "7922", "7961", "0608"];
        Check(ActiveProfiles.Count == intel.Length + mtk.Length,
              $"{ActiveProfiles.Count} active profiles, expected {intel.Length + mtk.Length}");
        foreach (var dev in mtk)
        {
            var id = $"PCI\\VEN_14C3&DEV_{dev}&SUBSYS_E0DF105B&REV_00\\4&79D7C7&0&000F";
            var p = MatchActive(id);
            if (p is null) { fail.Add($"14C3:{dev} is not taken"); continue; }
            Check(p.VendorName == "MediaTek" && p.Backend == RadioBackendKind.MtkRadio,
                  $"14C3:{dev} vendor {p.VendorName} backend {p.Backend}");
            Check(p.Capabilities.MinBarBytes == 0x100000, $"14C3:{dev} BAR {p.Capabilities.MinBarBytes:x}");
            Check(EffectiveMode(NetMode.Concurrent, p.Capabilities) == NetMode.OnDemand,
                  $"14C3:{dev} Concurrent not forced to OnDemand");
        }
        foreach (var dev in intel)
        {
            var id = $"PCI\\VEN_8086&DEV_{dev}&SUBSYS_00748086&REV_00\\3&11583659&0&A3";
            var p = MatchActive(id);
            if (p is null) { fail.Add($"8086:{dev} is not taken"); continue; }
            Check(p.HardwareId == $"PCI\\VEN_8086&DEV_{dev}", $"8086:{dev} hardware ID {p.HardwareId}");
            Check(p.VendorName == "Intel" && p.Backend == RadioBackendKind.Iwx, $"8086:{dev} vendor {p.VendorName} backend {p.Backend}");
            Check(p.Capabilities == AdapterCapabilities.IntelDefault, $"8086:{dev} capabilities {p.Capabilities}");
            Check(p.Capabilities.MinBarBytes == 0x2000, $"8086:{dev} BAR {p.Capabilities.MinBarBytes:x}");
            Check(p.ProxyInfName == "winawdl_regproxy.inf" && p.ProxyInfStem == "winawdl_regproxy",
                  $"8086:{dev} proxy INF {p.ProxyInfName}");
            Check(p.ProxyService == "WUDFRd" && p.ProxyProvider == "winairdrop project", $"8086:{dev} proxy service/provider");
            foreach (var m in Enum.GetValues<NetMode>())
                Check(EffectiveMode(m, p.Capabilities) == m, $"8086:{dev} mode {m} became {EffectiveMode(m, p.Capabilities)}");
        }

        // Parsing: case, boundaries, other buses.
        Check(TryParsePciId("pci\\ven_8086&dev_43f0&subsys_00748086\\3&1", out var v1, out var d1) && v1 == "8086" && d1 == "43F0",
              "lower-case instance ID not parsed");
        Check(MatchActive("pci\\ven_8086&dev_43f0&subsys_00748086\\3&1") is not null, "lower-case instance ID not taken");
        Check(TryParsePciId("PCI\\VEN_8086&DEV_43F0", out _, out _), "bare hardware ID not parsed");
        Check(TryParsePciId("PCI\\VEN_8086&DEV_43F0\\3&1", out _, out _), "ID ending in '\\' not parsed");
        Check(!TryParsePciId("PCI\\VEN_8086&DEV_43F01&SUBSYS_0", out _, out _), "five-digit device ID parsed");
        Check(!TryParsePciId("PCI\\VEN_8086&DEV_43F", out _, out _), "three-digit device ID parsed");
        Check(!TryParsePciId("PCI\\VEN_80G6&DEV_43F0", out _, out _), "non-hex vendor ID parsed");
        Check(!TryParsePciId("PCI\\VEN_8086&SUBSYS_00748086", out _, out _), "ID without DEV_ parsed");
        Check(!TryParsePciId("USB\\VID_0BDA&PID_8812\\1", out _, out _), "USB ID parsed");
        Check(!TryParsePciId("", out _, out _), "empty ID parsed");

        // Listed but not active: classified, never taken, forced to on demand.
        const string rtl = "PCI\\VEN_10EC&DEV_C852&SUBSYS_C85210EC\\4&1";
        Check(MatchActive(rtl) is null, "10EC:C852 would be taken");
        var r = Classify(rtl);
        if (r is null) fail.Add("10EC:C852 is not classified");
        else
        {
            Check(r.VendorName == "Realtek" && r.Backend == RadioBackendKind.Devourer, $"10EC:C852 vendor {r.VendorName} backend {r.Backend}");
            Check(!r.Capabilities.SupportsConcurrentWifi && !r.Capabilities.SupportsFastSend &&
                  !r.Capabilities.SupportsAggregate && !r.Capabilities.SupportsKernelDriver, "10EC:C852 capabilities");
            Check(r.Capabilities.MinBarBytes == 0x10000, $"10EC:C852 BAR {r.Capabilities.MinBarBytes:x}");
            Check(EffectiveMode(NetMode.Concurrent, r.Capabilities) == NetMode.OnDemand, "10EC:C852 Concurrent not forced to OnDemand");
            Check(EffectiveMode(NetMode.OnDemand, r.Capabilities) == NetMode.OnDemand, "10EC:C852 OnDemand changed");
            Check(EffectiveMode(NetMode.Off, r.Capabilities) == NetMode.Off, "10EC:C852 Off changed");
            Check(IsVendorProviderMatch(r, "Realtek Semiconductor Corp."), "Realtek provider not matched");
            Check(!IsVendorProviderMatch(r, "Intel"), "Intel provider matched as Realtek's");
        }

        // Ethernet chips from the same vendors and Wi-Fi cards nekodrop does not know.
        foreach (var other in new[] { "PCI\\VEN_8086&DEV_15F3&SUBSYS_00008086\\1", "PCI\\VEN_10EC&DEV_8168&SUBSYS_00000000\\1",
                                      "PCI\\VEN_14C3&DEV_7925&SUBSYS_00000000\\1", "PCI\\VEN_8086&DEV_7E40&SUBSYS_00908086\\1" })
        {
            Check(MatchActive(other) is null, $"{other} would be taken");
            Check(Classify(other) is null, $"{other} is classified");
        }

        // The vendor's driver is told from the proxy's, OEM repackages included.
        var i = ActiveProfiles[0];
        Check(!IsVendorProviderMatch(i, "winairdrop project"), "proxy provider matched as Intel's");
        Check(!IsVendorProviderMatch(i, "WinAirDrop Project"), "proxy provider (other case) matched as Intel's");
        Check(!IsVendorProviderMatch(i, ""), "empty provider matched");
        Check(IsVendorProviderMatch(i, "Intel"), "\"Intel\" not matched");
        Check(IsVendorProviderMatch(i, "Intel Corporation"), "\"Intel Corporation\" not matched");
        Check(IsVendorProviderMatch(i, "Killer"), "\"Killer\" not matched");
        Check(!IsVendorProviderMatch(i, "Realtek Semiconductor Corp."), "Realtek provider matched as Intel's");

        // The list itself.
        var dup = AllProfiles.GroupBy(p => p.VendorId + ":" + p.DeviceId).Where(g => g.Count() > 1).Select(g => g.Key).ToArray();
        Check(dup.Length == 0, "duplicate IDs: " + string.Join(", ", dup));
        foreach (var p in AllProfiles)
        {
            var key = $"{p.VendorId}:{p.DeviceId}";
            Check(IsHex4(p.VendorId) && p.VendorId == p.VendorId.ToUpperInvariant() &&
                  IsHex4(p.DeviceId) && p.DeviceId == p.DeviceId.ToUpperInvariant(), $"{key}: IDs must be four upper-case hex digits");
            Check(p.VendorName != "" && p.FamilyName != "" && p.ProxyInfName != "" && p.ProxyService != "" &&
                  p.ProxyProvider != "" && p.VendorProviderPatterns.Length > 0 && p.VendorProviderPatterns.All(s => s != ""),
                  $"{key}: empty field");
            Check(p.Capabilities.MinBarBytes > 0, $"{key}: no minimum BAR");
            Check(TryParsePciId(p.HardwareId, out var hv, out var hd) && hv == p.VendorId && hd == p.DeviceId,
                  $"{key}: hardware ID {p.HardwareId} does not parse back");
        }
        return fail;
    }
}
