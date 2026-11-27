// SPDX-License-Identifier: GPL-2.0-only
using System.Diagnostics;
using System.Net.NetworkInformation;

namespace NekoDrop.Service;

/// <summary>
/// The MAC address nekodrop's own Wi-Fi connection shows the router (#96). In
/// Concurrent mode the station sends from the "nekodrop" TAP adapter, whose
/// MAC is its own; routers then see a new device (another DHCP lease, MAC
/// filters and device names no longer apply). Before the card is taken, the
/// address Windows' Wi-Fi adapter was using is remembered, and the TAP
/// adapter is given it. The two never run at once: while nekodrop has the
/// card, Windows' adapter is gone; once it is given back, the TAP adapter is
/// disconnected.
/// </summary>
static class TapMac
{
    const string Adapter = "nekodrop";

    /// <summary>The address Windows' Wi-Fi adapter uses now (12 hex digits), or null.</summary>
    public static string? WindowsWifiMac()
    {
        try
        {
            var wifi = NetworkInterface.GetAllNetworkInterfaces()
                .Where(n => n.NetworkInterfaceType == NetworkInterfaceType.Wireless80211 && n.Name != Adapter &&
                            !n.Description.Contains("Virtual", StringComparison.OrdinalIgnoreCase) &&
                            !n.Description.Contains("Direct", StringComparison.OrdinalIgnoreCase))
                .OrderByDescending(n => n.OperationalStatus == OperationalStatus.Up)
                .FirstOrDefault();
            return wifi is null ? null : Valid(wifi.GetPhysicalAddress().ToString());
        }
        catch (NetworkInformationException) { return null; }
    }

    /// <summary>The TAP adapter's address now, or null.</summary>
    public static string? TapMacNow()
    {
        try
        {
            var tap = NetworkInterface.GetAllNetworkInterfaces().FirstOrDefault(n => n.Name == Adapter);
            return tap is null ? null : Valid(tap.GetPhysicalAddress().ToString());
        }
        catch (NetworkInformationException) { return null; }
    }

    /// <summary>12 hex digits of a unicast address that is not all zeros, upper case; else null.</summary>
    public static string? Valid(string? mac)
    {
        if (mac is null) return null;
        mac = mac.Replace("-", "").Replace(":", "").Trim().ToUpperInvariant();
        if (mac.Length != 12 || !mac.All(Uri.IsHexDigit) || mac == "000000000000") return null;
        return (Convert.ToByte(mac[..2], 16) & 1) != 0 ? null : mac; /* multicast bit */
    }

    /// <summary>Formats a valid 12-hex MAC as "AA:BB:CC:DD:EE:FF", or null.</summary>
    public static string? FormatColon(string? mac)
    {
        var v = Valid(mac);
        if (v is null) return null;
        return $"{v[0..2]}:{v[2..4]}:{v[4..6]}:{v[6..8]}:{v[8..10]}:{v[10..12]}";
    }

    /// <summary>
    /// Gives the TAP adapter this address (null: back to its own). The adapter
    /// restarts. Returns "" or what went wrong.
    /// </summary>
    public static string Apply(string? mac)
    {
        var command = mac is null
            /* Reset takes no -RegistryKeyword: pipe the property in. */
            ? $"Get-NetAdapterAdvancedProperty -Name '{Adapter}' -RegistryKeyword NetworkAddress -ErrorAction Stop | Reset-NetAdapterAdvancedProperty -ErrorAction Stop"
            : $"Set-NetAdapterAdvancedProperty -Name '{Adapter}' -RegistryKeyword NetworkAddress -RegistryValue '{mac}' -ErrorAction Stop";
        try
        {
            var psi = new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "WindowsPowerShell", "v1.0", "powershell.exe"))
            {
                UseShellExecute = false, CreateNoWindow = true,
                RedirectStandardOutput = true, RedirectStandardError = true,
            };
            foreach (var a in new[] { "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", command })
                psi.ArgumentList.Add(a);
            using var p = Process.Start(psi);
            if (p is null) return "cannot start PowerShell";
            var output = p.StandardOutput.ReadToEndAsync();
            var errors = p.StandardError.ReadToEndAsync();
            if (!p.WaitForExit(30000)) { try { p.Kill(); } catch (Exception) { } return "PowerShell did not finish"; }
            if (p.ExitCode != 0)
                return ((errors.Wait(1000) ? errors.Result : "") + " " + (output.Wait(1000) ? output.Result : ""))
                    .Replace("\r", " ").Replace("\n", " ").Trim();
            /* The adapter restarts: wait until it reports the new address. */
            for (int i = 0; i < 40; i++)
            {
                if (mac is null || TapMacNow() == mac) return "";
                Thread.Sleep(250);
            }
            return mac is not null && TapMacNow() != mac ? "adapter kept its own MAC address" : "";
        }
        catch (Exception e) { return e.Message; }
    }
}
