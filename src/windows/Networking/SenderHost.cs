using System.Buffers.Binary;
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

    public event Action<DiscoveredReceiver>? ReceiverDiscovered;
    public string NetworkSummary { get; private set; } = "сетевые адаптеры ещё не проверены";

    public async Task StartAsync(string selectedFile, string sha256)
    {
        if (_app is not null)
            throw new InvalidOperationException("AMEGA sender is already running.");

        var file = new FileInfo(selectedFile);
        if (!file.Exists)
            throw new FileNotFoundException("Selected package no longer exists.", selectedFile);

        var offer = new TransferOffer(
            "amega", 1, "transfer_offer", Guid.NewGuid().ToString("D"),
            _senderId, file.Name, file.Length,
            Path.GetExtension(file.Name).Equals(".pkg", StringComparison.OrdinalIgnoreCase) ? "pkg" : "test_file",
            sha256, DateTimeOffset.UtcNow);

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
        app.MapGet("/api/v1/health", () => Results.Json(new
        {
            protocol = "amega",
            major = 1,
            status = "discovery_only",
            sender_id = _senderId
        }));
        app.MapGet("/api/v1/offer", () => Results.Json(offer));

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

                if (!Guid.TryParse(receiverId, out _) ||
                    nonce is null || nonce.Length != 32 || !nonce.All(Uri.IsHexDigit) ||
                    !string.Equals(echoedNonce, _instanceNonce, StringComparison.OrdinalIgnoreCase) ||
                    string.IsNullOrWhiteSpace(name) || name.Length > 48 ||
                    string.IsNullOrWhiteSpace(version) || version.Length > 32)
                    continue;

                var receiver = new DiscoveredReceiver(
                    receiverId!, name.Trim(), datagram.RemoteEndPoint.Address.ToString(),
                    version, DateTimeOffset.UtcNow);

                try { ReceiverDiscovered?.Invoke(receiver); }
                catch { /* A UI subscriber must not terminate the network listener. */ }
            }
            catch (JsonException)
            {
                // Ignore malformed or unrelated LAN packets.
            }
        }
    }

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
