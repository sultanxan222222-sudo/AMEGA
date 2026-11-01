using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text.Json;
using AMEGA.Windows.Protocol;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.Logging;

namespace AMEGA.Windows.Networking;

/// <summary>
/// Stage-one LAN service. Discovery and read-only metadata are enabled.
/// File download is deliberately withheld until pairing and transfer authorization exist.
/// </summary>
internal sealed class SenderHost : IAsyncDisposable
{
    public const int DefaultHttpPort = 38471;
    public const int DefaultDiscoveryPort = 38470;

    private readonly string _senderId = Guid.NewGuid().ToString("D");
    private WebApplication? _app;
    private UdpClient? _receiverListener;
    private CancellationTokenSource? _cancellation;
    private Task? _beaconTask;
    private Task? _receiverListenTask;

    public event Action<DiscoveredReceiver>? ReceiverDiscovered;

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
            _receiverListener = new UdpClient(AddressFamily.InterNetwork);
            _receiverListener.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
            _receiverListener.Client.Bind(new IPEndPoint(IPAddress.Any, DefaultDiscoveryPort));

            var token = _cancellation.Token;
            _beaconTask = RunSenderBeaconLoopAsync(token);
            _receiverListenTask = RunReceiverListenerAsync(_receiverListener, token);
        }
        catch
        {
            await StopAsync();
            throw;
        }
    }

    private async Task RunSenderBeaconLoopAsync(CancellationToken cancellationToken)
    {
        using var udp = new UdpClient { EnableBroadcast = true };
        var endpoint = new IPEndPoint(IPAddress.Broadcast, DefaultDiscoveryPort);

        while (!cancellationToken.IsCancellationRequested)
        {
            var beacon = new DiscoveryBeacon(
                "amega", 1, "sender_announcement", _senderId,
                Convert.ToHexString(RandomNumberGenerator.GetBytes(16)),
                DefaultHttpPort, Environment.MachineName);

            var payload = JsonSerializer.SerializeToUtf8Bytes(beacon);
            await udp.SendAsync(payload, payload.Length, endpoint);
            await Task.Delay(TimeSpan.FromSeconds(3), cancellationToken);
        }
    }

    private async Task RunReceiverListenerAsync(UdpClient listener, CancellationToken cancellationToken)
    {
        while (!cancellationToken.IsCancellationRequested)
        {
            var datagram = await listener.ReceiveAsync(cancellationToken);
            if (datagram.Buffer.Length is 0 or > 1200)
                continue;

            try
            {
                using var document = JsonDocument.Parse(datagram.Buffer, new JsonDocumentOptions { MaxDepth = 8 });
                var root = document.RootElement;
                if (ReadString(root, "protocol") != "amega" ||
                    ReadInt(root, "major") != 1 ||
                    ReadString(root, "type") != "receiver_announcement")
                    continue;

                var receiverId = ReadString(root, "receiver_id");
                var nonce = ReadString(root, "instance_nonce");
                var name = ReadString(root, "display_name");
                if (!Guid.TryParse(receiverId, out _) ||
                    nonce is null || nonce.Length is < 32 or > 128 ||
                    !nonce.All(Uri.IsHexDigit) ||
                    string.IsNullOrWhiteSpace(name) || name.Length > 48)
                    continue;

                var receiver = new DiscoveredReceiver(
                    receiverId!, name, datagram.RemoteEndPoint.Address.ToString(),
                    ReadString(root, "receiver_version") ?? "unknown", DateTimeOffset.UtcNow);

                try { ReceiverDiscovered?.Invoke(receiver); }
                catch { /* A UI subscriber must not terminate the network listener. */ }
            }
            catch (JsonException)
            {
                // Ignore malformed or unrelated LAN broadcasts.
            }
        }
    }

    private static string? ReadString(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString()
            : null;

    private static int ReadInt(JsonElement element, string name) =>
        element.TryGetProperty(name, out var value) && value.TryGetInt32(out var number)
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
