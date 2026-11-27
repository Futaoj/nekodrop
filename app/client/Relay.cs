// SPDX-License-Identifier: GPL-2.0-only
namespace NekoDrop;

/// <summary>
/// A device in a relay rule, by protocol and identity. LocalSend devices are
/// known by their certificate fingerprint (their peer id); AirDrop and Android
/// Share devices only by the name they show, which is all an "Everyone"
/// AirDrop sender reveals, so renaming the phone means choosing it again.
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

/// <summary>A relay rule as stored in Settings.RelayRules (two RelayEnd.Serialize strings).</summary>
public sealed class RelayRuleData
{
    public string From { get; set; } = "";
    public string To { get; set; } = "";
}

/// <summary>"Files from From go on to To."</summary>
public sealed record RelayRule(RelayEnd From, RelayEnd To)
{
    public RelayRuleData ToData() => new() { From = From.Serialize(), To = To.Serialize() };

    public static RelayRule? FromData(RelayRuleData d) =>
        RelayEnd.Parse(d.From) is { } f && RelayEnd.Parse(d.To) is { } t ? new RelayRule(f, t) : null;
}

/// <summary>The relay rules, kept free of UI so they can be checked on their own.</summary>
public static class Relay
{
    /// <summary>The protocol of a request or peer id.</summary>
    public static string ProtocolOf(string id) =>
        MtaEngine.IsMtaRequest(id) || MtaEngine.IsMtaPeer(id) ? RelayEnd.Mta
        : LocalSendEngine.IsRequest(id) || LocalSendEngine.IsPeer(id) ? RelayEnd.LocalSend
        : RelayEnd.AirDrop;

    public static List<RelayRule> Load(IEnumerable<RelayRuleData> data) =>
        data.Select(RelayRule.FromData).OfType<RelayRule>().ToList();

    public static List<RelayRuleData> Save(IEnumerable<RelayRule> rules) => rules.Select(r => r.ToData()).ToList();

    /// <summary>
    /// Adds a rule. A source has one destination, so a rule from a device that
    /// already has one replaces it (returns true). A rule from a device to
    /// itself is refused (returns null).
    /// </summary>
    public static bool? Add(List<RelayRule> rules, RelayRule rule)
    {
        if (rule.From.Same(rule.To)) return null;
        var i = rules.FindIndex(r => r.From.Same(rule.From));
        if (i >= 0) { rules[i] = rule; return true; }
        rules.Add(rule);
        return false;
    }

    /// <summary>Why this rule cannot work right now ("" when it can): an end's protocol is off.</summary>
    public static string Blocker(RelayRule rule, Settings s)
    {
        if (rule.From.Same(rule.To)) return L.T("relay.samePair");
        foreach (var end in new[] { rule.From, rule.To })
        {
            if (end.Protocol == RelayEnd.Mta && !s.MtaOn) return L.F("relay.needMta", end.Name);
            if (end.Protocol == RelayEnd.LocalSend && !s.LocalSendOn) return L.F("relay.needLocalSend", end.Name);
            if (end.Protocol == RelayEnd.AirDrop && !s.Discoverable) return L.F("relay.needAirDrop", end.Name);
        }
        return "";
    }

    /// <summary>The rules that can work now.</summary>
    public static List<RelayRule> Working(IEnumerable<RelayRule> rules, Settings s) => rules.Where(r => Blocker(r, s) == "").ToList();

    /// <summary>
    /// Where a transfer from this sender goes: the destination of the rule whose
    /// source it is, or null (a normal receive). The first matching rule wins,
    /// though Add keeps one rule per source.
    /// </summary>
    public static RelayEnd? TargetFor(IEnumerable<RelayRule> rules, string protocol, string? peerId, string sender) =>
        rules.FirstOrDefault(r => !r.From.Same(r.To) && r.From.IsSender(protocol, peerId, sender))?.To;
}
