using System.Buffers.Binary;
using System.Collections.Concurrent;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Numerics;
using System.Security.Cryptography;
using System.Text.Json;
using AMEGA.Windows.Protocol;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.Net.Http.Headers;
using Microsoft.Extensions.Logging;

namespace AMEGA.Windows.Networking;

internal sealed class SenderHost : IAsyncDisposable
{
    public const int DefaultHttpPort = 38471;
    public const int SenderDiscoveryPort = 38470;
    public const int ReceiverDiscoveryPort = 38472;
    private const int MaxHostsPerSubnet = 4096;
    private static readonly TimeSpan SweepInterval = TimeSpan.FromSeconds(15);

    private readonly string _senderId = Guid.NewGuid().ToString("D");
    private readonly string _instanceNonce = Convert.ToHexString(RandomNumberGenerator.GetBytes(16)).ToLowerInvariant();
    private WebApplication? _app;
    private UdpClient? _receiverListener;
    private CancellationTokenSource? _cancellation;
    private Task? _beaconTask;
    private Task? _receiverListenTask;
    private IReadOnlyList<LanInterface> _lanInterfaces = Array.Empty<LanInterface>();
    private TransferOffer? _offer;
    private string? _offerFilePath;
    private string? _transferToken;
    private string? _targetReceiverId;
    private DiscoveredReceiver? _acceptedReceiver;
    private DateTimeOffset _acceptedAt;
    private readonly object _stateLock = new();
    private readonly ConcurrentDictionary<string, DiscoveredReceiver> _discoveredReceivers = new(StringComparer.OrdinalIgnoreCase);
    private string? _rpiFilePath;
    private string? _rpiFileName;
    private string? _rpiSha256;
    private string? _rpiReceiverIp;
    private DateTimeOffset _rpiFileExpiresAt;
    private long _rpiFileSize;
    private long _rpiCoveredBytes;
    private readonly List<(long Start, long End)> _rpiServedRanges = new();

    public event Action<DiscoveredReceiver>? ReceiverDiscovered;
    public string NetworkSummary { get; private set; } = "сетевые адаптеры ещё не проверены";
    public bool IsRunning => _app is not null;

    public void UpdateOffer(string? selectedFile, string? sha256, string? targetReceiverId = null)
    {
        lock (_stateLock)
        {
            // Any file change revokes a previously issued receiver token.
            _offer = null;
            _offerFilePath = null;
            _transferToken = null;
            _acceptedReceiver = null;
            _acceptedAt = default;
            _targetReceiverId = null;

            if (string.IsNullOrWhiteSpace(selectedFile) || string.IsNullOrWhiteSpace(sha256))
                return;

            if (!string.IsNullOrWhiteSpace(targetReceiverId))
            {
                if (!Guid.TryParse(targetReceiverId, out _))
                    throw new ArgumentException("Target receiver ID must be a UUID.", nameof(targetReceiverId));
                _targetReceiverId = targetReceiverId;
            }

            if (sha256.Length != 64 || !sha256.All(Uri.IsHexDigit))
                throw new ArgumentException("SHA-256 must contain 64 hexadecimal characters.", nameof(sha256));

            var file = new FileInfo(selectedFile);
            if (!file.Exists)
                throw new FileNotFoundException("Selected package no longer exists.", selectedFile);

            string normalizedHash = sha256.ToLowerInvariant();
            _offer = new TransferOffer(
                "amega", 1, "transfer_offer", CreateStableTransferId(normalizedHash),
                _senderId, file.Name, file.Length,
                Path.GetExtension(file.Name).Equals(".pkg", StringComparison.OrdinalIgnoreCase) ? "pkg" : "test_file",
                normalizedHash, DateTimeOffset.UtcNow);
            _offerFilePath = file.FullName;
        }
    }

    public async Task StartAsync(string? selectedFile = null, string? sha256 = null)
    {
        if (_app is not null)
            throw new InvalidOperationException("AMEGA sender is already running.");

        // Discovery is independent of file hashing. A user can find the PS4 first,
        // then select a large PKG; the offer becomes available after SHA-256 finishes.
        UpdateOffer(selectedFile, sha256);

        _lanInterfaces = GetLanInterfaces();
        NetworkSummary = _lanInterfaces.Count == 0
            ? "активный IPv4-адаптер не найден"
            : string.Join("; ", _lanInterfaces.Select(n => $"{n.Name} {n.Address}/{n.PrefixLength}"));

        var builder = WebApplication.CreateBuilder(new WebApplicationOptions
        {
            Args = Array.Empty<string>(),
            ContentRootPath = AppContext.BaseDirectory
        });
        builder.Logging.ClearProviders();
        builder.WebHost.ConfigureKestrel(options => options.ListenAnyIP(DefaultHttpPort));

        var app = builder.Build();
        app.MapGet("/api/v1/health", () =>
        {
            lock (_stateLock)
            {
                return Results.Json(new
                {
                    protocol = "amega",
                    major = 1,
                    status = "discovery_only",
                    offer_ready = _offer is not null,
                    receiver_authorized = _acceptedReceiver is not null && _transferToken is not null,
                    sender_id = _senderId
                });
            }
        });
        app.MapGet("/api/v1/offer", () =>
        {
            lock (_stateLock)
            {
                IResult result = _offer is null
                    ? Results.Problem("No package has been selected and verified yet.", statusCode: 409)
                    : Results.Json(_offer);
                return result;
            }
        });
        app.MapPost("/api/v1/accept", (HttpContext context, AcceptTransferRequest request) => AcceptTransfer(context, request));
        app.MapMethods("/api/v1/file", new[] { HttpMethods.Get, HttpMethods.Head },
            (HttpContext context) => ServeAuthorizedFile(context));
        app.MapMethods("/api/v1/rpi-file", new[] { HttpMethods.Get, HttpMethods.Head },
            (HttpContext context) => ServeRpiFile(context));

        await app.StartAsync();
        _app = app;
        _cancellation = new CancellationTokenSource();

        try
        {
            var listener = new UdpClient(AddressFamily.InterNetwork);
            listener.Client.ExclusiveAddressUse = false;
            listener.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
            listener.Client.Bind(new IPEndPoint(IPAddress.Any, ReceiverDiscoveryPort));
            _receiverListener = listener;

            var token = _cancellation.Token;
            _beaconTask = RunSenderBeaconLoopAsync(token);
            _receiverListenTask = RunReceiverListenerAsync(listener, token);
        }
        catch
        {
            await StopAsync();
            throw;
        }
    }

    private async Task RunSenderBeaconLoopAsync(CancellationToken cancellationToken)
    {
        var beacon = new DiscoveryBeacon(
            "amega", 1, "sender_announcement", _senderId,
            _instanceNonce, DefaultHttpPort, Environment.MachineName);
        byte[] payload = JsonSerializer.SerializeToUtf8Bytes(beacon);
        DateTimeOffset lastSweep = DateTimeOffset.MinValue;

        while (!cancellationToken.IsCancellationRequested)
        {
            DateTimeOffset now = DateTimeOffset.UtcNow;
            bool runSweep = now - lastSweep >= SweepInterval;

            if (_lanInterfaces.Count == 0)
            {
                using var fallback = new UdpClient { EnableBroadcast = true };
                try
                {
                    await fallback.SendAsync(payload, payload.Length,
                        new IPEndPoint(IPAddress.Broadcast, SenderDiscoveryPort));
                }
                catch (SocketException) { }
            }
            else
            {
                foreach (LanInterface lan in _lanInterfaces)
                {
                    cancellationToken.ThrowIfCancellationRequested();
                    try
                    {
                        using var udp = new UdpClient(new IPEndPoint(lan.Address, 0))
                        {
                            EnableBroadcast = true
                        };
                        await udp.SendAsync(payload, payload.Length,
                            new IPEndPoint(lan.Broadcast, SenderDiscoveryPort));

                        if (runSweep)
                        {
                            foreach (IPAddress host in lan.EnumerateHosts(MaxHostsPerSubnet))
                            {
                                if (host.Equals(lan.Address)) continue;
                                cancellationToken.ThrowIfCancellationRequested();
                                try
                                {
                                    await udp.SendAsync(payload, payload.Length,
                                        new IPEndPoint(host, SenderDiscoveryPort));
                                }
                                catch (SocketException) { }
                            }
                        }
                    }
                    catch (SocketException)
                    {
                        // A transient adapter failure must not stop discovery on other NICs.
                    }
                }
            }

            if (runSweep) lastSweep = now;
            await Task.Delay(TimeSpan.FromSeconds(3), cancellationToken);
        }
    }

    private async Task RunReceiverListenerAsync(UdpClient listener, CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            UdpReceiveResult datagram = await listener.ReceiveAsync(cancellationToken);
            if (datagram.Buffer.Length is 0 or > 1200 ||
                datagram.RemoteEndPoint.Address.Equals(IPAddress.Any) ||
                IPAddress.IsLoopback(datagram.RemoteEndPoint.Address))
                continue;

            try
            {
                using var document = JsonDocument.Parse(datagram.Buffer, new JsonDocumentOptions { MaxDepth = 8 });
                JsonElement root = document.RootElement;
                if (root.ValueKind != JsonValueKind.Object ||
                    ReadString(root, "protocol") != "amega" ||
                    ReadInt(root, "major") != 1 ||
                    ReadString(root, "type") != "receiver_announcement")
                    continue;

                string? receiverId = ReadString(root, "receiver_id");
                string? nonce = ReadString(root, "instance_nonce");
                string? echoedNonce = ReadString(root, "reply_to_nonce");
                string? name = ReadString(root, "display_name");
                string? version = ReadString(root, "receiver_version");
                string? firmwareVersion = ReadString(root, "firmware_version");

                if (!Guid.TryParse(receiverId, out _) ||
                    nonce is null || nonce.Length != 32 || !nonce.All(Uri.IsHexDigit) ||
                    !string.Equals(echoedNonce, _instanceNonce, StringComparison.OrdinalIgnoreCase) ||
                    string.IsNullOrWhiteSpace(name) || name.Length > 48 ||
                    string.IsNullOrWhiteSpace(version) || version.Length > 32 ||
                    (firmwareVersion is not null && firmwareVersion.Length > 32))
                    continue;

                firmwareVersion = string.IsNullOrWhiteSpace(firmwareVersion)
                    ? "не определена"
                    : firmwareVersion.Trim();

                var receiver = new DiscoveredReceiver(
                    receiverId!, name.Trim(), datagram.RemoteEndPoint.Address.MapToIPv4().ToString(),
                    version, firmwareVersion, nonce, DateTimeOffset.UtcNow);
                _discoveredReceivers[receiver.ReceiverId] = receiver;

                try { ReceiverDiscovered?.Invoke(receiver); }
                catch { /* A UI subscriber must not terminate the network listener. */ }
            }
            catch (JsonException)
            {
                // Ignore malformed or unrelated LAN packets.
            }
        }
    }

    private IResult AcceptTransfer(HttpContext context, AcceptTransferRequest request)
    {
        if (!Guid.TryParse(request.TransferId, out _) ||
            !Guid.TryParse(request.ReceiverId, out _) ||
            !IsHexNonce(request.InstanceNonce) ||
            !string.Equals(request.ReplyToNonce, _instanceNonce, StringComparison.OrdinalIgnoreCase))
            return Results.BadRequest(new { accepted = false, error = "invalid_pairing_request" });

        if (!_discoveredReceivers.TryGetValue(request.ReceiverId, out DiscoveredReceiver? receiver) ||
            !string.Equals(receiver.InstanceNonce, request.InstanceNonce, StringComparison.OrdinalIgnoreCase) ||
            DateTimeOffset.UtcNow - receiver.LastSeen > TimeSpan.FromSeconds(15))
            return Results.Problem("Receiver is not currently discovered. Refresh discovery and try again.", statusCode: 409);

        string? requestIp = context.Connection.RemoteIpAddress?.MapToIPv4().ToString();
        if (requestIp is null || !string.Equals(requestIp, receiver.IpAddress, StringComparison.OrdinalIgnoreCase))
            return Results.StatusCode(StatusCodes.Status403Forbidden);

        lock (_stateLock)
        {
            if (_targetReceiverId is not null &&
                !string.Equals(_targetReceiverId, receiver.ReceiverId, StringComparison.OrdinalIgnoreCase))
                return Results.StatusCode(StatusCodes.Status403Forbidden);

            if (_offer is null || _offerFilePath is null)
                return Results.Problem("No verified package is ready.", statusCode: 409);

            if (!string.Equals(request.TransferId, _offer.TransferId, StringComparison.OrdinalIgnoreCase))
                return Results.Conflict(new { accepted = false, error = "offer_changed" });

            if (_acceptedReceiver is not null &&
                !string.Equals(_acceptedReceiver.ReceiverId, receiver.ReceiverId, StringComparison.OrdinalIgnoreCase) &&
                DateTimeOffset.UtcNow - _acceptedAt < TimeSpan.FromMinutes(30))
                return Results.Conflict(new { accepted = false, error = "another_receiver_is_authorized" });

            if (_acceptedReceiver is null ||
                !string.Equals(_acceptedReceiver.ReceiverId, receiver.ReceiverId, StringComparison.OrdinalIgnoreCase) ||
                _transferToken is null ||
                DateTimeOffset.UtcNow - _acceptedAt >= TimeSpan.FromMinutes(30))
            {
                _acceptedReceiver = receiver;
                _transferToken = Convert.ToHexString(RandomNumberGenerator.GetBytes(32)).ToLowerInvariant();
                _acceptedAt = DateTimeOffset.UtcNow;
            }

            return Results.Json(new
            {
                accepted = true,
                transfer_id = _offer.TransferId,
                access_token = _transferToken,
                expires_in_seconds = 1800
            });
        }
    }

    private IResult ServeAuthorizedFile(HttpContext context)
    {
        string? suppliedToken = context.Request.Headers["X-AMEGA-Token"].FirstOrDefault();
        string? path;
        string? fileName;
        string? sha256;
        string? receiverIp;

        lock (_stateLock)
        {
            if (_offer is null || _offerFilePath is null ||
                _acceptedReceiver is null || _transferToken is null ||
                DateTimeOffset.UtcNow - _acceptedAt >= TimeSpan.FromMinutes(30) ||
                !FixedTimeTokenEquals(suppliedToken, _transferToken))
                return Results.Unauthorized();

            path = _offerFilePath;
            fileName = _offer.FileName;
            sha256 = _offer.Sha256;
            receiverIp = _acceptedReceiver.IpAddress;
        }

        string? remoteIp = context.Connection.RemoteIpAddress?.MapToIPv4().ToString();
        if (remoteIp is null || !string.Equals(remoteIp, receiverIp, StringComparison.OrdinalIgnoreCase))
            return Results.StatusCode(StatusCodes.Status403Forbidden);

        if (!System.IO.File.Exists(path))
            return Results.Problem("Selected package is no longer available.", statusCode: 410);

        try
        {
            var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read,
                1024 * 1024, FileOptions.Asynchronous | FileOptions.SequentialScan);
            var etag = new EntityTagHeaderValue($"\"{sha256}\"");
            return Results.File(stream, "application/octet-stream", fileName,
                entityTag: etag, enableRangeProcessing: true);
        }
        catch (IOException)
        {
            return Results.Problem("Package could not be opened for reading.", statusCode: 409);
        }
    }

    // Enables one temporary, source-IP-restricted URL for the standalone RPI app.
    // The selected PKG path and digest are snapshotted; later UI selections cannot
    // silently change the bytes that an already accepted installation task fetches.
    public string EnableRpiFileAccess(string receiverIp, string selectedFile, string sha256)
    {
        if (_app is null)
            throw new InvalidOperationException("Start LAN discovery before enabling RPI access.");
        if (!IPAddress.TryParse(receiverIp, out IPAddress? receiverAddress) ||
            receiverAddress.AddressFamily != AddressFamily.InterNetwork ||
            IPAddress.IsLoopback(receiverAddress) || receiverAddress.Equals(IPAddress.Any))
            throw new ArgumentException("RPI receiver must be a valid IPv4 address.", nameof(receiverIp));
        if (string.IsNullOrWhiteSpace(selectedFile) ||
            !Path.GetExtension(selectedFile).Equals(".pkg", StringComparison.OrdinalIgnoreCase))
            throw new ArgumentException("Only a PS4 PKG file can be offered to RPI.", nameof(selectedFile));
        if (sha256.Length != 64 || !sha256.All(Uri.IsHexDigit))
            throw new ArgumentException("SHA-256 must contain 64 hexadecimal characters.", nameof(sha256));

        var file = new FileInfo(selectedFile);
        if (!file.Exists || file.Length < 4)
            throw new FileNotFoundException("Selected PKG is missing or empty.", selectedFile);

        string localAddress = ResolveLocalAddressForPeer(receiverAddress);
        lock (_stateLock)
        {
            _rpiFilePath = file.FullName;
            _rpiFileName = file.Name;
            _rpiSha256 = sha256.ToLowerInvariant();
            _rpiReceiverIp = receiverAddress.ToString();
            _rpiFileSize = file.Length;
            _rpiCoveredBytes = 0;
            _rpiServedRanges.Clear();
            _rpiFileExpiresAt = DateTimeOffset.UtcNow.AddHours(12);
        }

        return $"http://{localAddress}:{DefaultHttpPort}/api/v1/rpi-file";
    }

    public void RevokeRpiFileAccess()
    {
        lock (_stateLock)
        {
            _rpiFilePath = null;
            _rpiFileName = null;
            _rpiSha256 = null;
            _rpiReceiverIp = null;
            _rpiFileExpiresAt = default;
            _rpiFileSize = 0;
            _rpiCoveredBytes = 0;
            _rpiServedRanges.Clear();
        }
    }

    public (long BytesServed, long TotalBytes, bool IsActive) GetRpiTransferProgress()
    {
        lock (_stateLock)
        {
            bool active = _rpiFilePath is not null &&
                DateTimeOffset.UtcNow < _rpiFileExpiresAt;
            return (active ? _rpiCoveredBytes : 0,
                active ? _rpiFileSize : 0, active);
        }
    }

    private void RecordRpiRead(long start, int count)
    {
        if (count <= 0 || start < 0) return;

        lock (_stateLock)
        {
            if (_rpiFilePath is null || _rpiFileSize <= 0 ||
                DateTimeOffset.UtcNow >= _rpiFileExpiresAt || start >= _rpiFileSize)
                return;

            long mergedStart = start;
            long mergedEnd = Math.Min(_rpiFileSize, start + (long)count);
            int index = 0;
            while (index < _rpiServedRanges.Count &&
                _rpiServedRanges[index].End < mergedStart)
                index++;

            while (index < _rpiServedRanges.Count &&
                _rpiServedRanges[index].Start <= mergedEnd)
            {
                mergedStart = Math.Min(mergedStart, _rpiServedRanges[index].Start);
                mergedEnd = Math.Max(mergedEnd, _rpiServedRanges[index].End);
                _rpiServedRanges.RemoveAt(index);
            }

            _rpiServedRanges.Insert(index, (mergedStart, mergedEnd));
            long covered = 0;
            foreach (var range in _rpiServedRanges)
                covered += range.End - range.Start;
            _rpiCoveredBytes = Math.Min(_rpiFileSize, covered);
        }
    }

    private string ResolveLocalAddressForPeer(IPAddress peer)
    {
        uint peerValue = BinaryPrimitives.ReadUInt32BigEndian(peer.GetAddressBytes());
        var best = _lanInterfaces
            .Where(n =>
            {
                uint local = BinaryPrimitives.ReadUInt32BigEndian(n.Address.GetAddressBytes());
                uint mask = BinaryPrimitives.ReadUInt32BigEndian(n.Mask.GetAddressBytes());
                return (peerValue & mask) == (local & mask);
            })
            .OrderByDescending(n => n.PrefixLength)
            .FirstOrDefault();

        if (best is not null)
            return best.Address.ToString();

        // UDP connect selects the OS route without sending a packet.
        using var routeProbe = new Socket(AddressFamily.InterNetwork, SocketType.Dgram, ProtocolType.Udp);
        routeProbe.Connect(new IPEndPoint(peer, 12800));
        if (routeProbe.LocalEndPoint is IPEndPoint localEndPoint &&
            localEndPoint.Address.AddressFamily == AddressFamily.InterNetwork &&
            !IPAddress.IsLoopback(localEndPoint.Address) &&
            !localEndPoint.Address.Equals(IPAddress.Any))
            return localEndPoint.Address.ToString();

        throw new InvalidOperationException("Не удалось определить локальный IPv4-адрес ПК для связи с RPI.");
    }

    private IResult ServeRpiFile(HttpContext context)
    {
        string? path;
        string? fileName;
        string? sha256;
        string? receiverIp;

        lock (_stateLock)
        {
            if (_rpiFilePath is null || _rpiFileName is null || _rpiSha256 is null ||
                _rpiReceiverIp is null || DateTimeOffset.UtcNow >= _rpiFileExpiresAt)
                return Results.StatusCode(StatusCodes.Status410Gone);

            path = _rpiFilePath;
            fileName = _rpiFileName;
            sha256 = _rpiSha256;
            receiverIp = _rpiReceiverIp;
        }

        string? remoteIp = context.Connection.RemoteIpAddress?.MapToIPv4().ToString();
        if (remoteIp is null || !string.Equals(remoteIp, receiverIp, StringComparison.OrdinalIgnoreCase))
            return Results.StatusCode(StatusCodes.Status403Forbidden);

        if (!System.IO.File.Exists(path))
            return Results.StatusCode(StatusCodes.Status410Gone);

        try
        {
            var fileStream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read,
                1024 * 1024, FileOptions.Asynchronous | FileOptions.SequentialScan);
            var stream = new RangeTrackingReadStream(fileStream, RecordRpiRead);
            var etag = new EntityTagHeaderValue("\"" + sha256 + "\"");
            return Results.File(stream, "application/octet-stream", fileName,
                entityTag: etag, enableRangeProcessing: true);
        }
        catch (IOException)
        {
            return Results.Problem("PKG could not be opened for RPI.", statusCode: 409);
        }
    }

    private static string CreateStableTransferId(string sha256)
    {
        // A content-derived UUID keeps the receiver partial-download key
        // stable when the PC app restarts and the same PKG is offered again.
        // The access token remains random and independent.
        char[] id = sha256.Substring(0, 32).ToLowerInvariant().ToCharArray();
        id[12] = '5';
        int variantNibble = Convert.ToInt32(id[16].ToString(), 16) & 0x03;
        id[16] = "89ab"[variantNibble];
        string hex = new string(id);
        return $"{hex[..8]}-{hex[8..12]}-{hex[12..16]}-{hex[16..20]}-{hex[20..32]}";
    }

    private static bool FixedTimeTokenEquals(string? supplied, string expected)
    {
        if (string.IsNullOrWhiteSpace(supplied) || supplied.Length != expected.Length)
            return false;
        try
        {
            byte[] a = Convert.FromHexString(supplied);
            byte[] b = Convert.FromHexString(expected);
            return a.Length == b.Length && CryptographicOperations.FixedTimeEquals(a, b);
        }
        catch (FormatException)
        {
            return false;
        }
    }

    private static bool IsHexNonce(string? value) =>
        value is { Length: 32 } && value.All(Uri.IsHexDigit);

    private static List<LanInterface> GetLanInterfaces()
    {
        var result = new List<LanInterface>();
        NetworkInterface[] adapters;
        try { adapters = NetworkInterface.GetAllNetworkInterfaces(); }
        catch { return result; }

        foreach (NetworkInterface adapter in adapters)
        {
            try
            {
                if (adapter.OperationalStatus != OperationalStatus.Up ||
                    adapter.NetworkInterfaceType == NetworkInterfaceType.Loopback)
                    continue;

                foreach (UnicastIPAddressInformation unicast in adapter.GetIPProperties().UnicastAddresses)
                {
                    IPAddress address = unicast.Address;
                    IPAddress? mask = unicast.IPv4Mask;
                    if (address.AddressFamily != AddressFamily.InterNetwork ||
                        IPAddress.IsLoopback(address) || mask is null)
                        continue;

                    byte[] addressBytes = address.GetAddressBytes();
                    byte[] maskBytes = mask.GetAddressBytes();
                    if (addressBytes.Length != 4 || maskBytes.Length != 4) continue;

                    byte[] broadcastBytes = new byte[4];
                    for (int i = 0; i < 4; i++)
                        broadcastBytes[i] = (byte)(addressBytes[i] | (byte)~maskBytes[i]);

                    int prefix = BitOperations.PopCount(BinaryPrimitives.ReadUInt32BigEndian(maskBytes));
                    result.Add(new LanInterface(
                        adapter.Name, address, mask, new IPAddress(broadcastBytes), prefix));
                }
            }
            catch
            {
                // Some transient or virtual adapters do not expose IPv4 mask data.
            }
        }

        return result
            .GroupBy(n => $"{n.Address}/{n.PrefixLength}", StringComparer.OrdinalIgnoreCase)
            .Select(g => g.First())
            .ToList();
    }

    private sealed record LanInterface(
        string Name, IPAddress Address, IPAddress Mask, IPAddress Broadcast, int PrefixLength)
    {
        public IEnumerable<IPAddress> EnumerateHosts(int maxHosts)
        {
            uint address = BinaryPrimitives.ReadUInt32BigEndian(Address.GetAddressBytes());
            uint mask = BinaryPrimitives.ReadUInt32BigEndian(Mask.GetAddressBytes());
            ulong network = address & mask;
            ulong broadcast = network | (~(ulong)mask & 0xFFFFFFFFUL);

            if (PrefixLength >= 31)
            {
                byte[] networkBytes = new byte[4];
                BinaryPrimitives.WriteUInt32BigEndian(networkBytes, (uint)network);
                yield return new IPAddress(networkBytes);
                if (broadcast != network)
                {
                    byte[] broadcastBytes = new byte[4];
                    BinaryPrimitives.WriteUInt32BigEndian(broadcastBytes, (uint)broadcast);
                    yield return new IPAddress(broadcastBytes);
                }
                yield break;
            }

            ulong candidate = network + 1;
            int yielded = 0;
            while (candidate < broadcast && yielded < maxHosts)
            {
                byte[] bytes = new byte[4];
                BinaryPrimitives.WriteUInt32BigEndian(bytes, (uint)candidate);
                yield return new IPAddress(bytes);
                candidate++;
                yielded++;
            }
        }
    }

    private static string? ReadString(JsonElement element, string name) =>
        element.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.String
            ? value.GetString()
            : null;

    private static int ReadInt(JsonElement element, string name) =>
        element.TryGetProperty(name, out JsonElement value) && value.TryGetInt32(out int number)
            ? number
            : -1;

    public async Task StopAsync()
    {
        lock (_stateLock)
        {
            _transferToken = null;
            _acceptedReceiver = null;
            _acceptedAt = default;
            _rpiFilePath = null;
            _rpiFileName = null;
            _rpiSha256 = null;
            _rpiReceiverIp = null;
            _rpiFileExpiresAt = default;
            _rpiFileSize = 0;
            _rpiCoveredBytes = 0;
            _rpiServedRanges.Clear();
        }

        if (_cancellation is not null)
        {
            await _cancellation.CancelAsync();
            _receiverListener?.Dispose();
            await ObserveTaskAsync(_beaconTask);
            await ObserveTaskAsync(_receiverListenTask);
            _receiverListener = null;
            _beaconTask = null;
            _receiverListenTask = null;
            _cancellation.Dispose();
            _cancellation = null;
        }

        if (_app is not null)
        {
            await _app.StopAsync();
            await _app.DisposeAsync();
            _app = null;
        }
    }

    private static async Task ObserveTaskAsync(Task? task)
    {
        if (task is null) return;
        try { await task; }
        catch (OperationCanceledException) { }
        catch (ObjectDisposedException) { }
        catch (SocketException) { }
    }

    public async ValueTask DisposeAsync() => await StopAsync();
}
