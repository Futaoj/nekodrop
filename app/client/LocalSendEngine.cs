// SPDX-License-Identifier: GPL-2.0-only
using System.Collections.Concurrent;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Net.NetworkInformation;
using System.Net.Security;
using System.Net.Sockets;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace NekoDrop;

/// <summary>
/// LocalSend (protocol v2, https://github.com/localsend/protocol) on the local
/// network, written from the protocol description:
///   - Discovery: a JSON announcement to UDP multicast 224.0.0.167:53317 on every
///     IPv4 interface; members answer with POST /api/localsend/v2/register (or a
///     multicast reply), and we answer theirs the same way.
///   - Receive: an HTTPS server on TCP 53317 (self-signed certificate; its SHA-256
///     is our fingerprint) for prepare-upload, upload and cancel.
///   - Send: prepare-upload to the receiver, then one upload per file.
/// Emits the same JSON events as Engine (AirDrop) and MtaEngine, with "ls:" peer
/// ids and "lsr:" request ids, so the radar, requests and send window need no
/// LocalSend-specific code.
/// </summary>
public sealed class LocalSendEngine : IDisposable
{
    public const string PeerPrefix = "ls:";
    public const string ReqPrefix = "lsr:";
    public const string ProtocolName = "localsend";
    public const int Port = 53317;
    static readonly IPAddress Group = IPAddress.Parse("224.0.0.167");
    const string ProtocolVersion = "2.1";
    static readonly TimeSpan PeerTtl = TimeSpan.FromSeconds(75);
    static readonly TimeSpan AnnounceEvery = TimeSpan.FromSeconds(10);
    static readonly TimeSpan AskTimeout = TimeSpan.FromSeconds(60);
    static readonly TimeSpan IdleTimeout = TimeSpan.FromSeconds(120);

    public event Action<JsonObject>? Event;
    public event Action<string>? Log;

    public static bool IsPeer(string id) => id.StartsWith(PeerPrefix, StringComparison.Ordinal);
    public static bool IsRequest(string id) => id.StartsWith(ReqPrefix, StringComparison.Ordinal);

    readonly object _lock = new();
    string _alias = Environment.MachineName;
    string _saveFolder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads", "nekodrop");
    bool _enabled, _discoverable = true, _looking, _disposed, _running;
    X509Certificate2? _cert;
    string _fingerprint = "";
    CancellationTokenSource? _run;
    TcpListener? _tcp;
    Socket? _udp;
    Timer? _timer;
    DateTime _lastAnnounce = DateTime.MinValue;
    HttpClient? _http;

    sealed class Peer
    {
        public required string Fingerprint;
        public string Alias = "LocalSend";
        public string Model = "";
        public string Type = "desktop";
        public required IPAddress Address;
        public int Port = LocalSendEngine.Port;
        public bool Https = true;
        public DateTime LastSeen;
        public string Emitted = "";
        public string Id => PeerPrefix + Fingerprint;
        public string BaseUrl => $"{(Https ? "https" : "http")}://{(Address.AddressFamily == AddressFamily.InterNetworkV6 ? $"[{Address}]" : Address.ToString())}:{Port}";
    }

    sealed class RxFile
    {
        public required string Id;
        public required string Name;
        public required long Size;
        public string Token = Convert.ToHexString(RandomNumberGenerator.GetBytes(12)).ToLowerInvariant();
        public string? Sha256;
        public long Done;
        public bool Finished;
        public string? SavedName;
    }

    sealed class RxSession
    {
        public required string RequestId;
        public required string SessionId;
        public required IPAddress From;
        public required Dictionary<string, RxFile> Files;
        public required string Folder;
        public DateTime LastActivity = DateTime.UtcNow;
        public CancellationTokenSource Cts = new();
        public long Total => Files.Values.Sum(f => f.Size);
        public long Done => Files.Values.Sum(f => f.Done);
    }

    readonly ConcurrentDictionary<string, Peer> _peers = new();
    readonly ConcurrentDictionary<string, TaskCompletionSource<bool>> _decisions = new();
    readonly ConcurrentDictionary<string, (CancellationTokenSource Cts, Peer Peer)> _sends = new();
    RxSession? _session; /* one at a time, as LocalSend does (409 for a second sender) */

    void Say(string line) => Log?.Invoke("localsend: " + line);
    void Emit(JsonObject o) => Event?.Invoke(o);

    public bool HasPeers => !_peers.IsEmpty;

    // ------------------------------------------------------------------ control

    public void Apply(Settings s)
    {
        bool start, stop;
        lock (_lock)
        {
            if (_disposed) return;
            _alias = string.IsNullOrWhiteSpace(s.DeviceName) ? Environment.MachineName : s.DeviceName.Trim();
            _saveFolder = s.SaveFolder;
            _discoverable = s.Discoverable;
            var was = _enabled;
            _enabled = s.LocalSendOn;
            start = _enabled && !was;
            stop = !_enabled && was;
        }
        if (stop) Stop();
        if (start) Start();
    }

    public void Look(bool on)
    {
        lock (_lock)
        {
            if (_disposed || _looking == on) return;
            _looking = on;
        }
        if (on) Announce();
    }

    public void Answer(string id, bool accept)
    {
        if (_decisions.TryRemove(id, out var tcs)) tcs.TrySetResult(accept);
        else if (!accept) Cancel(id);
    }

    public void Cancel(string id)
    {
        if (_decisions.TryRemove(id, out var tcs)) tcs.TrySetResult(false);
        RxSession? s;
        lock (_lock) s = _session?.RequestId == id ? _session : null;
        if (s is not null) EndSession(s, "cancelled");
        if (_sends.TryRemove(id, out var send)) try { send.Cts.Cancel(); } catch (Exception) { }
    }

    void Start()
    {
        CancellationToken ct;
        lock (_lock)
        {
            if (_running || _disposed) return;
            try { LoadCertificate(); }
            catch (Exception e) { Say("cannot create the TLS certificate: " + e.Message); return; }
            _run = new CancellationTokenSource();
            ct = _run.Token;
            _running = true;
            _http = new HttpClient(new SocketsHttpHandler
            {
                /* Members use self-signed certificates; the fingerprint, not a CA, says who they are. */
                SslOptions = new SslClientAuthenticationOptions { RemoteCertificateValidationCallback = (_, _, _, _) => true },
                ConnectTimeout = TimeSpan.FromSeconds(4),
                PooledConnectionLifetime = TimeSpan.FromMinutes(1),
            }) { Timeout = Timeout.InfiniteTimeSpan };
        }
        try
        {
            var tcp = new TcpListener(IPAddress.Any, Port);
            tcp.Start(32);
            _tcp = tcp;
            _ = Task.Run(() => AcceptLoop(tcp, ct));
        }
        catch (SocketException e)
        {
            Say($"TCP {Port} is in use ({e.SocketErrorCode}); is LocalSend itself running on this PC? Receiving is off, sending still works");
        }
        try
        {
            var udp = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
            udp.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
            udp.Bind(new IPEndPoint(IPAddress.Any, Port));
            udp.MulticastLoopback = false;
            foreach (var ip in LocalAddresses())
                try { udp.SetSocketOption(SocketOptionLevel.IP, SocketOptionName.AddMembership, new MulticastOption(Group, ip)); }
                catch (SocketException) { }
            _udp = udp;
            _ = Task.Run(() => UdpLoop(udp, ct));
        }
        catch (SocketException e)
        {
            Say($"UDP {Port} unavailable ({e.SocketErrorCode}); other LocalSend devices may not see this PC");
        }
        _timer = new Timer(_ => Tick(), null, TimeSpan.FromSeconds(2), TimeSpan.FromSeconds(5));
        Say($"on: \"{_alias}\", fingerprint {_fingerprint[..12]}...");
        Announce();
    }

    void Stop()
    {
        CancellationTokenSource? run;
        RxSession? s;
        lock (_lock)
        {
            if (!_running) return;
            _running = false;
            run = _run;
            _run = null;
            s = _session;
        }
        if (s is not null) EndSession(s, "cancelled");
        foreach (var id in _decisions.Keys) if (_decisions.TryRemove(id, out var t)) t.TrySetResult(false);
        foreach (var id in _sends.Keys) if (_sends.TryRemove(id, out var x)) try { x.Cts.Cancel(); } catch (Exception) { }
        try { run?.Cancel(); } catch (Exception) { }
        try { _tcp?.Stop(); } catch (Exception) { }
        try { _udp?.Dispose(); } catch (Exception) { }
        _timer?.Dispose();
        _http?.Dispose();
        _tcp = null; _udp = null; _timer = null; _http = null;
        foreach (var id in _peers.Keys)
            if (_peers.TryRemove(id, out var p)) Emit(new JsonObject { ["event"] = "peer_gone", ["id"] = p.Id });
        Say("off");
    }

    public void Dispose()
    {
        lock (_lock) _disposed = true;
        Stop();
    }

    void Tick()
    {
        bool looking;
        lock (_lock) looking = _looking && _running;
        if (looking && DateTime.UtcNow - _lastAnnounce >= AnnounceEvery) Announce();
        var cutoff = DateTime.UtcNow - PeerTtl;
        foreach (var (key, p) in _peers)
            if (p.LastSeen < cutoff && _peers.TryRemove(key, out _))
                Emit(new JsonObject { ["event"] = "peer_gone", ["id"] = p.Id });
        RxSession? s;
        lock (_lock) s = _session;
        if (s is not null && DateTime.UtcNow - s.LastActivity > IdleTimeout) EndSession(s, "timed out");
    }

    // ------------------------------------------------------------ identity, info

    void LoadCertificate()
    {
        if (_cert is not null) return;
        var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "nekodrop");
        Directory.CreateDirectory(dir);
        var path = Path.Combine(dir, "localsend-identity.pfx");
        X509Certificate2? cert = null;
        if (File.Exists(path))
            try { cert = new X509Certificate2(path, (string?)null, X509KeyStorageFlags.Exportable | X509KeyStorageFlags.UserKeySet); }
            catch (CryptographicException) { cert = null; }
        if (cert is null || !cert.HasPrivateKey || cert.NotAfter < DateTime.Now.AddDays(30))
        {
            using var rsa = RSA.Create(2048);
            var req = new CertificateRequest("CN=nekodrop LocalSend", rsa, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            using var made = req.CreateSelfSigned(DateTimeOffset.UtcNow.AddDays(-1), DateTimeOffset.UtcNow.AddYears(10));
            File.WriteAllBytes(path, made.Export(X509ContentType.Pfx));
            cert = new X509Certificate2(path, (string?)null, X509KeyStorageFlags.Exportable | X509KeyStorageFlags.UserKeySet);
        }
        _cert = cert;
        _fingerprint = Convert.ToHexString(SHA256.HashData(cert.RawData));
    }

    JsonObject Info(bool withPort = true, bool? announce = null)
    {
        string alias;
        lock (_lock) alias = _alias;
        var o = new JsonObject
        {
            ["alias"] = alias,
            ["version"] = ProtocolVersion,
            ["deviceModel"] = "Windows",
            ["deviceType"] = "desktop",
            ["fingerprint"] = _fingerprint,
        };
        if (withPort)
        {
            o["port"] = Port;
            o["protocol"] = "https";
        }
        o["download"] = false;
        if (announce is { } a) o["announce"] = a;
        return o;
    }

    static List<IPAddress> LocalAddresses()
    {
        var list = new List<IPAddress>();
        try
        {
            foreach (var ni in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (ni.OperationalStatus != OperationalStatus.Up || ni.NetworkInterfaceType == NetworkInterfaceType.Loopback) continue;
                if (!ni.Supports(NetworkInterfaceComponent.IPv4)) continue;
                foreach (var ua in ni.GetIPProperties().UnicastAddresses)
                {
                    var a = ua.Address;
                    if (a.AddressFamily != AddressFamily.InterNetwork) continue;
                    var b = a.GetAddressBytes();
                    if (b[0] == 169 && b[1] == 254) continue; /* no DHCP: not a network LocalSend devices are on */
                    list.Add(a);
                }
            }
        }
        catch (NetworkInformationException) { }
        return list;
    }

    // ---------------------------------------------------------------- discovery

    void Announce()
    {
        Socket? udp;
        lock (_lock)
        {
            if (!_running) return;
            udp = _udp;
            _lastAnnounce = DateTime.UtcNow;
        }
        if (udp is null) return;
        var payload = Encoding.UTF8.GetBytes(Info(announce: true).ToJsonString());
        foreach (var ip in LocalAddresses())
        {
            try
            {
                /* Join late-arriving interfaces too (Wi-Fi up after we started). */
                try { udp.SetSocketOption(SocketOptionLevel.IP, SocketOptionName.AddMembership, new MulticastOption(Group, ip)); }
                catch (SocketException) { }
                udp.SetSocketOption(SocketOptionLevel.IP, SocketOptionName.MulticastInterface, ip.GetAddressBytes());
                udp.SendTo(payload, new IPEndPoint(Group, Port));
            }
            catch (SocketException) { }
            catch (ObjectDisposedException) { return; }
        }
    }

    async Task UdpLoop(Socket udp, CancellationToken ct)
    {
        var buf = new byte[8192];
        while (!ct.IsCancellationRequested)
        {
            SocketReceiveFromResult r;
            try { r = await udp.ReceiveFromAsync(buf, SocketFlags.None, new IPEndPoint(IPAddress.Any, 0), ct); }
            catch (OperationCanceledException) { return; }
            catch (ObjectDisposedException) { return; }
            catch (SocketException) { continue; }
            if (r.RemoteEndPoint is not IPEndPoint from) continue;
            JsonObject? o;
            try { o = JsonNode.Parse(buf.AsSpan(0, r.ReceivedBytes)) as JsonObject; }
            catch (JsonException) { continue; }
            if (o is null) continue;
            var peer = Remember(o, from.Address);
            if (peer is null) continue;
            if ((bool?)o["announce"] == true || (bool?)o["announcement"] == true) _ = AnswerAnnounceAsync(peer, ct);
        }
    }

    /// <summary>A member's info (announcement, register, prepare-upload) as a peer; null for ourselves or junk.</summary>
    Peer? Remember(JsonObject o, IPAddress from)
    {
        var fp = (string?)o["fingerprint"];
        if (string.IsNullOrWhiteSpace(fp) || fp.Length > 256 || string.Equals(fp, _fingerprint, StringComparison.OrdinalIgnoreCase)) return null;
        if (from.IsIPv4MappedToIPv6) from = from.MapToIPv4();
        var now = DateTime.UtcNow;
        var p = _peers.GetOrAdd(fp, _ => new Peer { Fingerprint = fp, Address = from });
        p.Address = from;
        p.LastSeen = now;
        p.Alias = Trim((string?)o["alias"], "LocalSend");
        p.Model = Trim((string?)o["deviceModel"], "");
        p.Type = Trim((string?)o["deviceType"], "desktop");
        if (o["port"] is JsonValue pv && pv.TryGetValue<int>(out var port) && port is > 0 and < 65536) p.Port = port;
        if ((string?)o["protocol"] is { } proto) p.Https = !string.Equals(proto, "http", StringComparison.OrdinalIgnoreCase);
        var label = ModelLabel(p);
        var shown = p.Alias + "|" + label;
        if (p.Emitted != shown)
        {
            p.Emitted = shown;
            Emit(new JsonObject { ["event"] = "peer", ["id"] = p.Id, ["name"] = p.Alias, ["model"] = label, ["protocol"] = ProtocolName });
        }
        return p;
    }

    static string Trim(string? s, string fallback)
    {
        if (string.IsNullOrWhiteSpace(s)) return fallback;
        s = s.Trim();
        return s.Length > 64 ? s[..64] : s;
    }

    static string ModelLabel(Peer p)
    {
        var kind = p.Type switch { "mobile" => "Phone", "web" => "Browser", "headless" or "server" => "Server", _ => "Computer" };
        return string.IsNullOrEmpty(p.Model) ? $"LocalSend · {kind}" : $"LocalSend · {p.Model}";
    }

    async Task AnswerAnnounceAsync(Peer peer, CancellationToken ct)
    {
        var http = _http;
        if (http is null) return;
        try
        {
            using var cts = CancellationTokenSource.CreateLinkedTokenSource(ct);
            cts.CancelAfter(TimeSpan.FromSeconds(5));
            using var body = new StringContent(Info().ToJsonString(), Encoding.UTF8, "application/json");
            using var resp = await http.PostAsync(peer.BaseUrl + "/api/localsend/v2/register", body, cts.Token);
            if (resp.IsSuccessStatusCode) return;
        }
        catch (Exception) when (!ct.IsCancellationRequested) { }
        /* The member's server is not reachable: answer on multicast instead. */
        Socket? udp;
        lock (_lock) udp = _udp;
        if (udp is null) return;
        var payload = Encoding.UTF8.GetBytes(Info(announce: false).ToJsonString());
        foreach (var ip in LocalAddresses())
            try
            {
                udp.SetSocketOption(SocketOptionLevel.IP, SocketOptionName.MulticastInterface, ip.GetAddressBytes());
                udp.SendTo(payload, new IPEndPoint(Group, Port));
            }
            catch (Exception) { }
    }

    // ------------------------------------------------------------- HTTPS server

    async Task AcceptLoop(TcpListener tcp, CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            TcpClient client;
            try { client = await tcp.AcceptTcpClientAsync(ct); }
            catch (OperationCanceledException) { return; }
            catch (ObjectDisposedException) { return; }
            catch (SocketException) { continue; }
            _ = Task.Run(() => ServeAsync(client, ct));
        }
    }

    sealed class HttpReq
    {
        public required string Method;
        public required string Path;
        public required Dictionary<string, string> Query;
        public required Dictionary<string, string> Headers;
        public required Stream Body;
    }

    async Task ServeAsync(TcpClient client, CancellationToken ct)
    {
        using var _c = client;
        client.NoDelay = true;
        var remote = (client.Client.RemoteEndPoint as IPEndPoint)?.Address ?? IPAddress.None;
        if (remote.IsIPv4MappedToIPv6) remote = remote.MapToIPv4();
        try
        {
            await using var ssl = new SslStream(client.GetStream(), false);
            using (var hs = CancellationTokenSource.CreateLinkedTokenSource(ct))
            {
                hs.CancelAfter(TimeSpan.FromSeconds(10));
                await ssl.AuthenticateAsServerAsync(new SslServerAuthenticationOptions
                {
                    ServerCertificate = _cert,
                    ClientCertificateRequired = false,
                    EnabledSslProtocols = SslProtocols.Tls12 | SslProtocols.Tls13,
                }, hs.Token);
            }
            var reader = new HttpReader(ssl);
            var req = await reader.ReadRequestAsync(ct);
            if (req is null) return;
            var (code, json) = await RouteAsync(req, remote, ct);
            await WriteResponseAsync(ssl, code, json, ct);
        }
        catch (Exception e) when (e is IOException or AuthenticationException or OperationCanceledException or SocketException or InvalidDataException)
        {
        }
    }

    static async Task WriteResponseAsync(Stream s, int code, JsonNode? json, CancellationToken ct)
    {
        var body = json is null ? Array.Empty<byte>() : Encoding.UTF8.GetBytes(json.ToJsonString());
        var reason = code switch
        {
            200 => "OK", 204 => "No Content", 400 => "Bad Request", 401 => "Unauthorized", 403 => "Forbidden",
            404 => "Not Found", 409 => "Conflict", 422 => "Unprocessable Entity", 429 => "Too Many Requests", _ => "Internal Server Error",
        };
        var head = $"HTTP/1.1 {code} {reason}\r\nContent-Length: {body.Length}\r\n" +
                   (json is null ? "" : "Content-Type: application/json; charset=utf-8\r\n") +
                   "Connection: close\r\n\r\n";
        await s.WriteAsync(Encoding.ASCII.GetBytes(head), ct);
        if (body.Length > 0) await s.WriteAsync(body, ct);
        await s.FlushAsync(ct);
    }

    async Task<(int, JsonNode?)> RouteAsync(HttpReq r, IPAddress remote, CancellationToken ct)
    {
        const string api = "/api/localsend/v2/";
        const string apiV1 = "/api/localsend/v1/";
        var path = r.Path.StartsWith(apiV1, StringComparison.Ordinal) ? api + r.Path[apiV1.Length..] : r.Path;
        switch (r.Method, path)
        {
            case ("GET", api + "info"):
                return (200, Info(withPort: false));
            case ("POST", api + "register"):
            {
                var o = await ReadJsonAsync(r.Body, 64 * 1024, ct);
                if (o is null) return (400, null);
                Remember(o, remote);
                return (200, Info(withPort: false));
            }
            case ("POST", api + "prepare-upload"):
                return await PrepareUploadAsync(r, remote, ct);
            case ("POST", api + "upload"):
                return await UploadAsync(r, remote, ct);
            case ("POST", api + "cancel"):
            {
                RxSession? s;
                lock (_lock) s = _session;
                if (s is not null && r.Query.GetValueOrDefault("sessionId") == s.SessionId && s.From.Equals(remote))
                    EndSession(s, "cancelled");
                return (200, null);
            }
            default:
                return (404, null);
        }
    }

    static async Task<JsonObject?> ReadJsonAsync(Stream body, int limit, CancellationToken ct)
    {
        using var ms = new MemoryStream();
        var buf = new byte[16384];
        int n;
        while ((n = await body.ReadAsync(buf, ct)) > 0)
        {
            ms.Write(buf, 0, n);
            if (ms.Length > limit) return null;
        }
        try { return JsonNode.Parse(ms.ToArray()) as JsonObject; }
        catch (JsonException) { return null; }
    }

    async Task<(int, JsonNode?)> PrepareUploadAsync(HttpReq r, IPAddress remote, CancellationToken ct)
    {
        var o = await ReadJsonAsync(r.Body, 4 * 1024 * 1024, ct);
        if (o is null || o["files"] is not JsonObject filesObj || filesObj.Count == 0) return (400, null);
        string folder;
        lock (_lock)
        {
            if (!_discoverable) return (403, null);
            if (_session is not null) return (409, null);
            folder = _saveFolder;
        }
        var info = o["info"] as JsonObject ?? new JsonObject();
        var peer = Remember(info, remote);
        var sender = peer?.Alias ?? Trim((string?)info["alias"], "LocalSend");
        var model = peer is null ? "LocalSend" : ModelLabel(peer);

        var files = new Dictionary<string, RxFile>();
        var list = new JsonArray();
        foreach (var (key, node) in filesObj)
        {
            if (node is not JsonObject f || files.Count >= 10000) continue;
            var id = (string?)f["id"] ?? key;
            var name = SanitizeFileName((string?)f["fileName"] ?? "file");
            long size = 0;
            if (f["size"] is JsonValue sv) sv.TryGetValue(out size);
            if (size < 0 || string.IsNullOrEmpty(id) || files.ContainsKey(id)) continue;
            files[id] = new RxFile { Id = id, Name = name, Size = size, Sha256 = (string?)f["sha256"] };
            list.Add(new JsonObject { ["name"] = name, ["type"] = (string?)f["fileType"] ?? "application/octet-stream" });
        }
        if (files.Count == 0) return (400, null);

        var requestId = ReqPrefix + Convert.ToHexString(RandomNumberGenerator.GetBytes(6)).ToLowerInvariant();
        var session = new RxSession
        {
            RequestId = requestId,
            SessionId = Convert.ToHexString(RandomNumberGenerator.GetBytes(12)).ToLowerInvariant(),
            From = remote,
            Files = files,
            Folder = folder,
        };
        lock (_lock)
        {
            if (_session is not null) return (409, null);
            _session = session; /* held while the user decides, so a second sender gets 409 */
        }
        var tcs = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        _decisions[requestId] = tcs;
        Say($"{sender} ({remote}) asks to send {files.Count} file(s), {session.Total} bytes");
        Emit(new JsonObject
        {
            ["event"] = "asked",
            ["id"] = requestId,
            ["sender"] = sender,
            ["model"] = model,
            ["files"] = list,
            ["waits"] = true,
            ["contact"] = false,
            ["protocol"] = ProtocolName,
            ["peer"] = peer?.Id,
        });
        bool accepted;
        try { accepted = await tcs.Task.WaitAsync(AskTimeout, ct); }
        catch (TimeoutException) { accepted = false; }
        finally { _decisions.TryRemove(requestId, out _); }
        if (!accepted)
        {
            lock (_lock) if (_session == session) _session = null;
            Emit(new JsonObject { ["event"] = "lost", ["id"] = requestId, ["reason"] = "declined" });
            return (403, null);
        }
        Directory.CreateDirectory(folder);
        session.LastActivity = DateTime.UtcNow;
        var tokens = new JsonObject();
        foreach (var f in files.Values) tokens[f.Id] = f.Token;
        Emit(new JsonObject { ["event"] = "incoming", ["id"] = requestId, ["done"] = 0.0, ["total"] = (double)session.Total });
        /* Nothing to transfer (all empty files): store them now. */
        if (files.Values.All(f => f.Size == 0))
        {
            foreach (var f in files.Values) Save(session, f, null);
            Finish(session);
            return (204, null);
        }
        return (200, new JsonObject { ["sessionId"] = session.SessionId, ["files"] = tokens });
    }

    async Task<(int, JsonNode?)> UploadAsync(HttpReq r, IPAddress remote, CancellationToken ct)
    {
        RxSession? s;
        lock (_lock) s = _session;
        var q = r.Query;
        if (!q.TryGetValue("sessionId", out var sid) || !q.TryGetValue("fileId", out var fid) || !q.TryGetValue("token", out var token))
            return (400, null);
        if (s is null || s.SessionId != sid || !s.From.Equals(remote) || !s.Files.TryGetValue(fid, out var file) ||
            file.Token != token || file.Finished)
            return (403, null);
        if (_decisions.ContainsKey(s.RequestId)) return (409, null);

        var temp = Path.Combine(s.Folder, $".nekodrop-ls-{Guid.NewGuid():N}.part");
        using var linked = CancellationTokenSource.CreateLinkedTokenSource(ct, s.Cts.Token);
        var lastReport = Environment.TickCount64;
        try
        {
            using var hash = file.Sha256 is null ? null : IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            await using (var fs = new FileStream(temp, FileMode.CreateNew, FileAccess.Write, FileShare.None, 1 << 20, FileOptions.Asynchronous | FileOptions.SequentialScan))
            {
                var buf = new byte[1 << 20];
                int n;
                while ((n = await r.Body.ReadAsync(buf, linked.Token)) > 0)
                {
                    if (file.Done + n > file.Size) throw new InvalidDataException("more data than announced");
                    await fs.WriteAsync(buf.AsMemory(0, n), linked.Token);
                    hash?.AppendData(buf, 0, n);
                    file.Done += n;
                    s.LastActivity = DateTime.UtcNow;
                    var now = Environment.TickCount64;
                    if (now - lastReport >= 150)
                    {
                        lastReport = now;
                        Emit(new JsonObject { ["event"] = "incoming", ["id"] = s.RequestId, ["done"] = (double)s.Done, ["total"] = (double)s.Total });
                    }
                }
            }
            if (file.Done != file.Size) throw new InvalidDataException($"got {file.Done} of {file.Size} bytes");
            if (hash is not null && !string.Equals(Convert.ToHexString(hash.GetHashAndReset()), file.Sha256, StringComparison.OrdinalIgnoreCase))
            {
                File.Delete(temp);
                file.Done = 0;
                return (422, null);
            }
            Save(s, file, temp);
            Emit(new JsonObject { ["event"] = "incoming", ["id"] = s.RequestId, ["done"] = (double)s.Done, ["total"] = (double)s.Total });
            bool all;
            lock (_lock) all = s.Files.Values.All(f => f.Finished);
            if (all) Finish(s);
            return (200, null);
        }
        catch (Exception e) when (e is IOException or OperationCanceledException or InvalidDataException or UnauthorizedAccessException)
        {
            try { File.Delete(temp); } catch (Exception) { }
            file.Done = 0;
            if (!ct.IsCancellationRequested && !s.Cts.IsCancellationRequested)
            {
                Say($"upload of {file.Name} failed: {e.Message}");
                EndSession(s, e.Message);
            }
            return (500, null);
        }
    }

    void Save(RxSession s, RxFile f, string? temp)
    {
        lock (_lock)
        {
            var dst = UniqueFilePath(s.Folder, f.Name);
            if (temp is null) File.WriteAllBytes(dst, []);
            else File.Move(temp, dst);
            f.SavedName = Path.GetFileName(dst);
            f.Finished = true;
        }
    }

    void Finish(RxSession s)
    {
        lock (_lock)
        {
            if (_session != s) return;
            _session = null;
        }
        var names = new JsonArray();
        foreach (var f in s.Files.Values) if (f.SavedName is { } n) names.Add(n);
        Say($"received {names.Count} file(s), {s.Total} bytes");
        Emit(new JsonObject { ["event"] = "stored", ["id"] = s.RequestId, ["folder"] = s.Folder, ["names"] = names, ["protocol"] = ProtocolName });
    }

    void EndSession(RxSession s, string reason)
    {
        lock (_lock)
        {
            if (_session != s) return;
            _session = null;
        }
        try { s.Cts.Cancel(); } catch (Exception) { }
        if (_decisions.TryRemove(s.RequestId, out var t)) t.TrySetResult(false);
        /* Files already finished stay; partial ones were deleted by their upload. */
        Emit(new JsonObject { ["event"] = "lost", ["id"] = s.RequestId, ["reason"] = reason });
    }

    // -------------------------------------------------------------------- send

    public void SendFiles(string id, string peerId, IReadOnlyList<string> files)
    {
        var fp = peerId.StartsWith(PeerPrefix, StringComparison.Ordinal) ? peerId[PeerPrefix.Length..] : peerId;
        if (!_peers.TryGetValue(fp, out var peer) || _http is null)
        {
            Emit(new JsonObject { ["event"] = "send", ["id"] = id, ["state"] = "failed", ["reason"] = "peer_gone" });
            return;
        }
        var cts = new CancellationTokenSource();
        _sends[id] = (cts, peer);
        _ = Task.Run(async () =>
        {
            try { await SendAsync(id, peer, files, cts.Token); }
            catch (OperationCanceledException)
            {
                Emit(new JsonObject { ["event"] = "send", ["id"] = id, ["state"] = "failed", ["reason"] = "cancelled" });
            }
            catch (Exception e)
            {
                Say($"send to {peer.Alias} failed: {e.Message}");
                Emit(new JsonObject { ["event"] = "send", ["id"] = id, ["state"] = "failed", ["reason"] = e.Message });
            }
            finally { _sends.TryRemove(id, out _); }
        });
    }

    async Task SendAsync(string id, Peer peer, IReadOnlyList<string> paths, CancellationToken ct)
    {
        var http = _http ?? throw new InvalidOperationException("LocalSend is off");
        var infos = paths.Select(p => new FileInfo(p)).Where(f => f.Exists).ToList();
        if (infos.Count == 0) throw new FileNotFoundException("nothing to send");
        var ids = infos.Select(_ => Convert.ToHexString(RandomNumberGenerator.GetBytes(8)).ToLowerInvariant()).ToList();
        var filesObj = new JsonObject();
        for (int i = 0; i < infos.Count; i++)
        {
            var f = infos[i];
            filesObj[ids[i]] = new JsonObject
            {
                ["id"] = ids[i],
                ["fileName"] = f.Name,
                ["size"] = f.Length,
                ["fileType"] = MimeOf(f.Name),
                ["metadata"] = new JsonObject
                {
                    ["modified"] = f.LastWriteTimeUtc.ToString("yyyy-MM-ddTHH:mm:ss.fffZ"),
                    ["accessed"] = f.LastAccessTimeUtc.ToString("yyyy-MM-ddTHH:mm:ss.fffZ"),
                },
            };
        }
        var prepare = new JsonObject { ["info"] = Info(), ["files"] = filesObj };
        Emit(new JsonObject { ["event"] = "send", ["id"] = id, ["state"] = "asking" });
        Say($"asking {peer.Alias} ({peer.Address}) to take {infos.Count} file(s)");

        string sessionId;
        JsonObject tokens;
        using (var askCts = CancellationTokenSource.CreateLinkedTokenSource(ct))
        {
            askCts.CancelAfter(TimeSpan.FromMinutes(3)); /* the receiver's user decides meanwhile */
            using var body = new StringContent(prepare.ToJsonString(), Encoding.UTF8, "application/json");
            using var resp = await http.PostAsync(peer.BaseUrl + "/api/localsend/v2/prepare-upload", body, askCts.Token);
            switch ((int)resp.StatusCode)
            {
                case 200: break;
                case 204:
                    Emit(new JsonObject { ["event"] = "send", ["id"] = id, ["state"] = "sent", ["done"] = 0.0, ["total"] = 0.0 });
                    return;
                case 403:
                    Emit(new JsonObject { ["event"] = "send", ["id"] = id, ["state"] = "declined" });
                    return;
                case 401: throw new InvalidOperationException(L.T("ls.err.pin"));
                case 409: throw new InvalidOperationException(L.T("ls.err.busy"));
                default: throw new InvalidOperationException($"HTTP {(int)resp.StatusCode}");
            }
            var answer = JsonNode.Parse(await resp.Content.ReadAsStringAsync(askCts.Token)) as JsonObject;
            sessionId = (string?)answer?["sessionId"] ?? throw new InvalidDataException("no sessionId");
            tokens = answer?["files"] as JsonObject ?? new JsonObject();
        }

        var total = infos.Sum(f => f.Length);
        long before = 0;
        var lastTick = 0L;
        try
        {
            for (int i = 0; i < infos.Count; i++)
            {
                /* The receiver may take only some of the files: those without a token. */
                if ((string?)tokens[ids[i]] is not { } token) { before += infos[i].Length; continue; }
                var url = $"{peer.BaseUrl}/api/localsend/v2/upload?sessionId={Uri.EscapeDataString(sessionId)}" +
                          $"&fileId={Uri.EscapeDataString(ids[i])}&token={Uri.EscapeDataString(token)}";
                var start = before;
                await using var fs = new FileStream(infos[i].FullName, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20, FileOptions.Asynchronous | FileOptions.SequentialScan);
                using var content = new ProgressContent(fs, infos[i].Length, sent =>
                {
                    var now = Environment.TickCount64;
                    if (now - lastTick < 150) return;
                    lastTick = now;
                    Emit(new JsonObject { ["event"] = "send", ["id"] = id, ["state"] = "sending", ["done"] = (double)(start + sent), ["total"] = (double)total });
                });
                using var resp = await http.PostAsync(url, content, ct);
                if (!resp.IsSuccessStatusCode) throw new InvalidOperationException($"upload of {infos[i].Name}: HTTP {(int)resp.StatusCode}");
                before += infos[i].Length;
            }
        }
        catch (OperationCanceledException)
        {
            try
            {
                using var c = new CancellationTokenSource(TimeSpan.FromSeconds(3));
                using var r = await http.PostAsync($"{peer.BaseUrl}/api/localsend/v2/cancel?sessionId={Uri.EscapeDataString(sessionId)}", null, c.Token);
            }
            catch (Exception) { }
            throw;
        }
        Say($"sent {infos.Count} file(s), {total} bytes to {peer.Alias}");
        Emit(new JsonObject { ["event"] = "send", ["id"] = id, ["state"] = "sent", ["done"] = (double)total, ["total"] = (double)total });
    }

    sealed class ProgressContent(Stream src, long length, Action<long> progress) : HttpContent
    {
        protected override async Task SerializeToStreamAsync(Stream stream, TransportContext? context, CancellationToken ct)
        {
            var buf = new byte[1 << 20];
            long sent = 0;
            int n;
            while ((n = await src.ReadAsync(buf, ct)) > 0)
            {
                await stream.WriteAsync(buf.AsMemory(0, n), ct);
                sent += n;
                progress(sent);
            }
        }
        protected override Task SerializeToStreamAsync(Stream stream, TransportContext? context) =>
            SerializeToStreamAsync(stream, context, CancellationToken.None);
        protected override bool TryComputeLength(out long len) { len = length; return true; }
    }

    // ------------------------------------------------------------------ helpers

    internal static string SanitizeFileName(string name)
    {
        var leaf = Path.GetFileName(name.Replace('\\', '/').Replace('/', Path.DirectorySeparatorChar));
        foreach (var bad in Path.GetInvalidFileNameChars()) leaf = leaf.Replace(bad, '_');
        leaf = leaf.Trim().TrimEnd('.');
        if (leaf.Length > 200) leaf = leaf[..150] + Path.GetExtension(leaf);
        return string.IsNullOrWhiteSpace(leaf) || leaf is "." or ".." ? "file" : leaf;
    }

    static string UniqueFilePath(string dir, string name)
    {
        var path = Path.Combine(dir, name);
        if (!File.Exists(path)) return path;
        var stem = Path.GetFileNameWithoutExtension(name);
        var ext = Path.GetExtension(name);
        for (int i = 1; i < 10000; i++)
        {
            var candidate = Path.Combine(dir, $"{stem} ({i}){ext}");
            if (!File.Exists(candidate)) return candidate;
        }
        return Path.Combine(dir, $"{stem}_{Guid.NewGuid():N}{ext}");
    }

    static string MimeOf(string name) => Path.GetExtension(name).ToLowerInvariant() switch
    {
        ".jpg" or ".jpeg" => "image/jpeg", ".png" => "image/png", ".gif" => "image/gif", ".webp" => "image/webp",
        ".heic" => "image/heic", ".bmp" => "image/bmp", ".mp4" => "video/mp4", ".mov" => "video/quicktime",
        ".mkv" => "video/x-matroska", ".mp3" => "audio/mpeg", ".m4a" => "audio/mp4", ".wav" => "audio/wav",
        ".pdf" => "application/pdf", ".zip" => "application/zip", ".txt" => "text/plain", ".apk" => "application/vnd.android.package-archive",
        _ => "application/octet-stream",
    };

    /// <summary>Just enough HTTP/1.1 for LocalSend clients: one request per connection, Content-Length or chunked bodies.</summary>
    sealed class HttpReader(Stream s)
    {
        readonly byte[] _buf = new byte[64 * 1024];
        int _pos, _len;

        async Task<bool> FillAsync(CancellationToken ct)
        {
            if (_pos > 0 && _pos < _len) { Buffer.BlockCopy(_buf, _pos, _buf, 0, _len - _pos); _len -= _pos; _pos = 0; }
            else if (_pos >= _len) { _pos = _len = 0; }
            if (_len == _buf.Length) return false;
            var n = await s.ReadAsync(_buf.AsMemory(_len), ct);
            if (n <= 0) return false;
            _len += n;
            return true;
        }

        async Task<string?> ReadLineAsync(CancellationToken ct)
        {
            while (true)
            {
                for (int i = _pos; i + 1 < _len; i++)
                    if (_buf[i] == '\r' && _buf[i + 1] == '\n')
                    {
                        var line = Encoding.ASCII.GetString(_buf, _pos, i - _pos);
                        _pos = i + 2;
                        return line;
                    }
                if (!await FillAsync(ct)) return null;
            }
        }

        public async Task<HttpReq?> ReadRequestAsync(CancellationToken ct)
        {
            var first = await ReadLineAsync(ct);
            if (string.IsNullOrEmpty(first)) return null;
            var parts = first.Split(' ');
            if (parts.Length < 2) return null;
            var headers = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            for (int i = 0; i < 100; i++)
            {
                var line = await ReadLineAsync(ct);
                if (line is null) return null;
                if (line.Length == 0) break;
                var c = line.IndexOf(':');
                if (c > 0) headers[line[..c].Trim()] = line[(c + 1)..].Trim();
            }
            var target = parts[1];
            var qi = target.IndexOf('?');
            var path = qi < 0 ? target : target[..qi];
            var query = new Dictionary<string, string>(StringComparer.Ordinal);
            if (qi >= 0)
                foreach (var kv in target[(qi + 1)..].Split('&', StringSplitOptions.RemoveEmptyEntries))
                {
                    var e = kv.IndexOf('=');
                    var k = Uri.UnescapeDataString((e < 0 ? kv : kv[..e]).Replace('+', ' '));
                    var v = e < 0 ? "" : Uri.UnescapeDataString(kv[(e + 1)..].Replace('+', ' '));
                    query[k] = v;
                }
            Stream body;
            if (headers.TryGetValue("Transfer-Encoding", out var te) && te.Contains("chunked", StringComparison.OrdinalIgnoreCase))
                body = new ChunkedBody(this);
            else
            {
                long len = 0;
                if (headers.TryGetValue("Content-Length", out var cl) && (!long.TryParse(cl, out len) || len < 0)) return null;
                body = new FixedBody(this, len);
            }
            return new HttpReq { Method = parts[0].ToUpperInvariant(), Path = path, Query = query, Headers = headers, Body = body };
        }

        public async ValueTask<int> ReadRawAsync(Memory<byte> dst, CancellationToken ct)
        {
            if (_pos >= _len)
            {
                _pos = _len = 0;
                var n = await s.ReadAsync(dst, ct);
                return n;
            }
            var take = Math.Min(dst.Length, _len - _pos);
            _buf.AsMemory(_pos, take).CopyTo(dst);
            _pos += take;
            return take;
        }

        public Task<string?> LineAsync(CancellationToken ct) => ReadLineAsync(ct);
    }

    abstract class BodyStream : Stream
    {
        public override bool CanRead => true;
        public override bool CanSeek => false;
        public override bool CanWrite => false;
        public override long Length => throw new NotSupportedException();
        public override long Position { get => throw new NotSupportedException(); set => throw new NotSupportedException(); }
        public override void Flush() { }
        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
        public override int Read(byte[] buffer, int offset, int count) => ReadAsync(buffer.AsMemory(offset, count)).AsTask().GetAwaiter().GetResult();
        public override Task<int> ReadAsync(byte[] buffer, int offset, int count, CancellationToken ct) => ReadAsync(buffer.AsMemory(offset, count), ct).AsTask();
    }

    sealed class FixedBody(HttpReader r, long length) : BodyStream
    {
        long _left = length;
        public override async ValueTask<int> ReadAsync(Memory<byte> dst, CancellationToken ct = default)
        {
            if (_left <= 0 || dst.Length == 0) return 0;
            var n = await r.ReadRawAsync(dst[..(int)Math.Min(dst.Length, _left)], ct);
            if (n <= 0) throw new IOException("connection closed before the body ended");
            _left -= n;
            return n;
        }
    }

    sealed class ChunkedBody(HttpReader r) : BodyStream
    {
        long _left;
        bool _done;
        public override async ValueTask<int> ReadAsync(Memory<byte> dst, CancellationToken ct = default)
        {
            if (_done || dst.Length == 0) return 0;
            if (_left == 0)
            {
                var line = await r.LineAsync(ct) ?? throw new IOException("bad chunk");
                if (line.Length == 0) line = await r.LineAsync(ct) ?? throw new IOException("bad chunk");
                var semi = line.IndexOf(';');
                if (!long.TryParse(semi < 0 ? line : line[..semi], System.Globalization.NumberStyles.HexNumber, null, out _left) || _left < 0)
                    throw new IOException("bad chunk size");
                if (_left == 0)
                {
                    while (!string.IsNullOrEmpty(await r.LineAsync(ct))) { } /* trailers */
                    _done = true;
                    return 0;
                }
            }
            var n = await r.ReadRawAsync(dst[..(int)Math.Min(dst.Length, _left)], ct);
            if (n <= 0) throw new IOException("connection closed inside a chunk");
            _left -= n;
            if (_left == 0) await r.LineAsync(ct); /* CRLF after the chunk */
            return n;
        }
    }
}
