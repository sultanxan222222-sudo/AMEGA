using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace AMEGA.Windows.Networking;

internal sealed class RpiInstallerClient : IDisposable
{
    private readonly HttpClient _httpClient;
    private readonly int _port;
    private readonly bool _allowLoopbackForTests;

    public RpiInstallerClient(int port = 12800, bool allowLoopbackForTests = false)
    {
        if (port is < 1 or > 65535)
            throw new ArgumentOutOfRangeException(nameof(port));

        _port = port;
        _allowLoopbackForTests = allowLoopbackForTests;
        var handler = new SocketsHttpHandler
        {
            UseProxy = false,
            ConnectTimeout = TimeSpan.FromSeconds(3),
            PooledConnectionLifetime = TimeSpan.FromMinutes(1)
        };
        _httpClient = new HttpClient(handler)
        {
            Timeout = Timeout.InfiniteTimeSpan
        };
    }

    public async Task<RpiProbeResult> ProbeAsync(string consoleIp, CancellationToken cancellationToken)
    {
        var reply = await PostAsync(consoleIp, "/api/is_exists",
            new { title_id = "CUSA00000" }, TimeSpan.FromSeconds(8), cancellationToken);

        if (!TryReadStatus(reply.Body, out string status))
            return new RpiProbeResult(false,
                reply.StatusCode == 0
                    ? $"RPI не отвечает на TCP {_port}. {reply.Body}"
                    : $"На порту {_port} получен ответ HTTP {reply.StatusCode}, но это не ответ API Remote Package Installer.");

        // is_exists is deliberately a read-only probe. A 'fail' response can
        // still prove the API is alive when the harmless test title is absent.
        string probeDetail = FormatError(reply.Body);
        return new RpiProbeResult(true,
            status.Equals("success", StringComparison.OrdinalIgnoreCase)
                ? "RPI отвечает на проверочный API-запрос."
                : $"RPI отвечает (API status: {status}). {probeDetail}");
    }

    public async Task<RpiInstallResult> InstallAsync(
        string consoleIp, string packageUrl, CancellationToken cancellationToken)
    {
        if (!Uri.TryCreate(packageUrl, UriKind.Absolute, out Uri? url) ||
            url.Scheme != Uri.UriSchemeHttp ||
            url.HostNameType != UriHostNameType.IPv4 ||
            url.Port != SenderHost.DefaultHttpPort ||
            url.AbsolutePath != "/api/v1/rpi-file" ||
            !IPAddress.TryParse(url.Host, out IPAddress? fileHost) ||
            !IsPrivateIPv4(fileHost))
            return new RpiInstallResult(false, null, "RPI разрешён только локальный URL AMEGA для выбранного PKG.");

        // RPI reads package metadata synchronously before replying. This can
        // take much longer than the lightweight readiness check.
        var reply = await PostAsync(consoleIp, "/api/install",
            new { type = "direct", packages = new[] { packageUrl } },
            TimeSpan.FromSeconds(150), cancellationToken);

        if (reply.StatusCode == 0)
            return new RpiInstallResult(false, null,
                $"Нет подтверждения от RPI: {reply.Body} Задача могла быть принята; не закрывай AMEGA, пока не проверишь PS4.",
                MayHaveBeenAccepted: true);

        if (reply.StatusCode is < 200 or >= 300)
            return new RpiInstallResult(false, null,
                $"RPI отклонил запрос (HTTP {reply.StatusCode}). {FormatError(reply.Body)}");

        if (!TryReadStatus(reply.Body, out string status) ||
            !status.Equals("success", StringComparison.OrdinalIgnoreCase))
            return new RpiInstallResult(false, null,
                $"RPI не подтвердил запуск установки. {FormatError(reply.Body)}");

        Match taskMatch = Regex.Match(reply.Body,
            "\"task_id\"\\s*:\\s*\"?(?<id>[0-9]+)\"?",
            RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
        if (!taskMatch.Success ||
            !long.TryParse(taskMatch.Groups["id"].Value, out long taskId))
            return new RpiInstallResult(false, null,
                "RPI ответил success, но не вернул номер задачи. Проверь состояние на PS4; пока не закрывай AMEGA.",
                MayHaveBeenAccepted: true);

        return new RpiInstallResult(true, taskId, $"RPI принял задачу №{taskId}.");
    }

    public async Task<RpiTaskProgress?> GetTaskProgressAsync(
        string consoleIp, long taskId, CancellationToken cancellationToken)
    {
        if (taskId < 0) return null;

        // RPI shares a single-threaded event loop with its package HTTP server.
        // During large downloads this endpoint can legitimately stop answering;
        // use a short timeout and let the caller fall back to local Range coverage.
        var reply = await PostAsync(consoleIp, "/api/get_task_progress",
            new { task_id = taskId }, TimeSpan.FromSeconds(4), cancellationToken);

        if (reply.StatusCode is < 200 or >= 300 ||
            !RpiTaskProgressParser.TryParse(reply.Body, out RpiTaskProgress? progress))
            return null;

        return progress;
    }

    private async Task<RpiHttpReply> PostAsync(
        string consoleIp, string path, object payload, TimeSpan requestTimeout,
        CancellationToken cancellationToken)
    {
        if (!IPAddress.TryParse(consoleIp, out IPAddress? address) ||
            address.AddressFamily != AddressFamily.InterNetwork ||
            (IPAddress.IsLoopback(address) && !_allowLoopbackForTests) || address.Equals(IPAddress.Any))
            return new RpiHttpReply(0, "Некорректный IPv4-адрес PS4.");

        var endpoint = new UriBuilder(Uri.UriSchemeHttp, address.ToString(), _port, path).Uri;
        string json = JsonSerializer.Serialize(payload);
        using var request = new HttpRequestMessage(HttpMethod.Post, endpoint)
        {
            Content = new StringContent(json, Encoding.UTF8, "application/json")
        };

        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(requestTimeout);

        try
        {
            using HttpResponseMessage response = await _httpClient.SendAsync(
                request, HttpCompletionOption.ResponseHeadersRead, timeout.Token);
            await using Stream stream = await response.Content.ReadAsStreamAsync(timeout.Token);
            using var memory = new MemoryStream();
            byte[] buffer = new byte[1024];

            while (memory.Length <= 8192)
            {
                int count = await stream.ReadAsync(buffer.AsMemory(0,
                    (int)Math.Min(buffer.Length, 8193 - memory.Length)), timeout.Token);
                if (count == 0) break;
                memory.Write(buffer, 0, count);
            }

            if (memory.Length > 8192)
                return new RpiHttpReply((int)response.StatusCode, "Ответ RPI слишком большой.");

            return new RpiHttpReply((int)response.StatusCode, Encoding.UTF8.GetString(memory.ToArray()));
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            return new RpiHttpReply(0, "Время ожидания RPI истекло. Проверь, что приложение открыто на PS4.");
        }
        catch (HttpRequestException ex)
        {
            return new RpiHttpReply(0, $"PS4 недоступна по TCP {_port}: {ex.Message}");
        }
        catch (IOException ex)
        {
            return new RpiHttpReply(0, $"Ошибка чтения ответа RPI: {ex.Message}");
        }
    }

    private static bool TryReadStatus(string body, out string status)
    {
        // RPI versions sometimes emit hexadecimal error_code values without
        // JSON quoting (0x........), which makes the whole document invalid JSON.
        // We only extract the small, fixed "status" string and never deserialize
        // the malformed response as an object.
        Match match = Regex.Match(body,
            "\"status\"\\s*:\\s*\"(?<value>[a-zA-Z_]+)\"",
            RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
        status = match.Success ? match.Groups["value"].Value : string.Empty;
        return match.Success && status.Length <= 32;
    }

    private static bool IsPrivateIPv4(IPAddress address)
    {
        if (address.AddressFamily != AddressFamily.InterNetwork) return false;
        byte[] bytes = address.GetAddressBytes();
        return bytes[0] == 10 ||
            (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31) ||
            (bytes[0] == 192 && bytes[1] == 168) ||
            (bytes[0] == 169 && bytes[1] == 254);
    }

    private static string FormatError(string body)
    {
        Match match = Regex.Match(body,
            "\"error_code\"\\s*:\\s*(?<code>0x[0-9a-fA-F]+|[0-9]+)",
            RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
        if (!match.Success) return string.Empty;

        string code = match.Groups["code"].Value.ToLowerInvariant();
        return code switch
        {
            "0x80990085" => "На PS4 недостаточно свободного места.",
            "0x80990088" => "Для этого содержимого уже существует задача установки.",
            "0x8099000e" => "Это содержимое уже установлено.",
            "0x80020016" => "RPI отклонил PKG: неверный параметр или пакет несовместим.",
            _ => $"Код ошибки RPI: {code}."
        };
    }

    public void Dispose() => _httpClient.Dispose();

    private sealed record RpiHttpReply(int StatusCode, string Body);
}

internal sealed record RpiProbeResult(bool IsResponsive, string Message);
internal sealed record RpiInstallResult(
    bool Success, long? TaskId, string Message, bool MayHaveBeenAccepted = false);
