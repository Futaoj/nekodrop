// SPDX-License-Identifier: GPL-2.0-only
namespace NekoDrop;

/// <summary>
/// One end of the relay pair: a device the user chose, by protocol and identity.
/// LocalSend devices are known by their certificate fingerprint (their peer id);
/// AirDrop and Android Share devices only by the name they show, which is all an
/// "Everyone" AirDrop sender reveals, so renaming the phone means choosing again.
/// </summary>
public sealed record RelayEnd(string Protocol, string Key, string Name)
{
    public const string AirDrop = "airdrop";
    public const string Mta = "mta2";
    public const string LocalSend = LocalSendEngine.ProtocolName;

    public string Serialize() => $"{Protocol}\n{Key}\n{Name}";

    public static RelayEnd? Parse(string? s)
    {
        if (string.IsNullOrWhiteSpace(s)) return null;
        var p = s.Split('\n');
        if (p.Length != 3 || p[0] is not (AirDrop or Mta or LocalSend) || p[1].Length == 0) return null;
        return new RelayEnd(p[0], p[1], p[2].Length == 0 ? p[1] : p[2]);
    }

    public static RelayEnd FromDevice(Device d) => new(d.Protocol, d.Protocol == LocalSend ? d.Id : d.Name, d.Name);

    public bool Same(RelayEnd other) =>
        Protocol == other.Protocol && string.Equals(Key, other.Key, Protocol == LocalSend ? StringComparison.Ordinal : StringComparison.OrdinalIgnoreCase);

    /// <summary>A device on the radar that is this end (to send to).</summary>
    public bool Is(Device d) => !d.ForAsk && Same(FromDevice(d));

    /// <summary>A sender of an incoming request that is this end.</summary>
    public bool IsSender(string protocol, string? peerId, string sender) =>
        Protocol == protocol &&
        (Protocol == LocalSend ? peerId == Key : string.Equals(sender.Trim(), Key.Trim(), StringComparison.OrdinalIgnoreCase));

    public string Label => $"{Name} · {ProtocolLabel(Protocol)}";

    public static string ProtocolLabel(string protocol) => protocol switch
    {
        Mta => L.T("mta.tag"),
        LocalSend => "LocalSend",
        _ => "AirDrop",
    };
}

/// <summary>The relay rules, kept free of UI so they can be checked on their own.</summary>
public static class Relay
{
    /// <summary>The protocol of a request or peer id.</summary>
    public static string ProtocolOf(string id) =>
        MtaEngine.IsMtaRequest(id) || MtaEngine.IsMtaPeer(id) ? RelayEnd.Mta
        : LocalSendEngine.IsRequest(id) || LocalSendEngine.IsPeer(id) ? RelayEnd.LocalSend
        : RelayEnd.AirDrop;

    /// <summary>
    /// Where a transfer from this sender goes: files from A go to B, from B to A,
    /// from anyone else nowhere (a normal receive). Null when the pair is not set,
    /// is the same device twice, or the sender is not one of them.
    /// </summary>
    public static RelayEnd? PartnerFor(RelayEnd? a, RelayEnd? b, string protocol, string? peerId, string sender)
    {
        if (a is null || b is null || a.Same(b)) return null;
        if (a.IsSender(protocol, peerId, sender)) return b;
        if (b.IsSender(protocol, peerId, sender)) return a;
        return null;
    }

    /// <summary>Why the pair cannot relay right now ("" when it can): an end's protocol is off.</summary>
    public static string Blocker(RelayEnd? a, RelayEnd? b, Settings s)
    {
        if (a is null || b is null) return L.T("relay.needPair");
        if (a.Same(b)) return L.T("relay.samePair");
        foreach (var end in new[] { a, b })
        {
            if (end.Protocol == RelayEnd.Mta && !s.MtaOn) return L.F("relay.needMta", end.Name);
            if (end.Protocol == RelayEnd.LocalSend && !s.LocalSendOn) return L.F("relay.needLocalSend", end.Name);
            if (end.Protocol == RelayEnd.AirDrop && !s.Discoverable) return L.F("relay.needAirDrop", end.Name);
        }
        return "";
    }
}
