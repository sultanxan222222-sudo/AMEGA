using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text.Json;
using AMEGA.Windows.Protocol;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Http;
using Microsoft.Extensions.Hosting;

namespace AMEGA.Windows.Networking;

/// <summary>
/// Stage-one sender service. It publishes discovery and read-only metadata only.
/// File download endpoints are intentionally not enabled until pairing/authentication
/// and resumable transfer are implemented.
/// </summary>
internal sealed class SenderHost : IAsyncDisposable
{
    public const int DefaultHttpPort = 38471;
    public const int DefaultDiscoveryPort = 38470;

    private readonly string _senderId = Guid.NewGuid().ToString("D");
    private WebApplication? _app;
    private CancellationTokenSource? _beaconCancellation;
    private Task? _beaconTask;

    public async Task StartAsync(string selectedFile, string sha256)
    {
        if (_app is not null)
            throw new InvalidOperationException("AMEGA sender is already running.");

        var file = new FileInfo(selectedFile);
        if (!file.Exists)
            throw new FileNotFoundException("Selected package no longer exists.", selectedFile);

        var offer = new TransferOffer(
            "amega",
            1,
            "transfer_offer",
            Guid.NewGuid().ToString("D"),
            _senderId,
            Path.GetFileName(file.Name),
            file.Length,
            Path.GetExtension(file.Name).Equals(".pkg", StringComparison.OrdinalIgnoreCase) ? "pkg" : "test_file",
            sha256,
            DateTimeOffset.UtcNow);

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

        _beaconCancellation = new CancellationTokenSource();
        _beaconTask = RunBeaconLoopAsync(_beaconCancellation.Token);
    }

    private async Task RunBeaconLoopAsync(CancellationToken cancellationToken)
    {
        using var udp = new UdpClient { EnableBroadcast = true };
        var endpoint = new IPEndPoint(IPAddress.Broadcast, DefaultDiscoveryPort);

        while (!cancellationToken.IsCancellationRequested)
        {
            var beacon = new DiscoveryBeacon(
                "amega",
                1,
                "sender_announcement",
                _senderId,
                Convert.ToHexString(RandomNumberGenerator.GetBytes(16)),
                DefaultHttpPort,
                Environment.MachineName);

            var payload = JsonSerializer.SerializeToUtf8Bytes(beacon);
            await udp.SendAsync(payload, payload.Length, endpoint);
            await Task.Delay(TimeSpan.FromSeconds(3), cancellationToken);
        }
    }

    public async Task StopAsync()
    {
        if (_beaconCancellation is not null)
        {
            await _beaconCancellation.CancelAsync();
            if (_beaconTask is not null)
            {
                try { await _beaconTask; }
                catch (OperationCanceledException) { }
                catch (ObjectDisposedException) { }
            }
            _beaconCancellation.Dispose();
            _beaconCancellation = null;
            _beaconTask = null;
        }

        if (_app is not null)
        {
            await _app.StopAsync();
            await _app.DisposeAsync();
            _app = null;
        }
    }

    public async ValueTask DisposeAsync() => await StopAsync();
}
