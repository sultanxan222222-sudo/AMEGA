using System.Security.Cryptography;
using AMEGA.Windows.Networking;

namespace AMEGA.Windows;

internal sealed class MainForm : Form
{
    private readonly Button _chooseButton = new();
    private readonly Button _startButton = new();
    private readonly Button _stopButton = new();
    private readonly Label _fileLabel = new();
    private readonly Label _sizeLabel = new();
    private readonly Label _hashLabel = new();
    private readonly Label _statusLabel = new();
    private readonly ProgressBar _hashProgress = new();
    private CancellationTokenSource? _hashCts;
    private readonly Label _stageLabel = new();
    private readonly Label _devicesTitle = new();
    private readonly ListBox _devicesList = new();
    private readonly Dictionary<string, DiscoveredReceiver> _devices = new();
    private readonly SenderHost _sender = new();
    private readonly System.Windows.Forms.Timer _deviceExpiryTimer = new() { Interval = 3000 };
    private string? _selectedFile;
    private string? _selectedHash;
    private bool _isSearching;

    public MainForm()
    {
        Text = "AMEGA — отправка на PS4";
        StartPosition = FormStartPosition.CenterScreen;
        MinimumSize = new Size(740, 600);
        Size = new Size(820, 650);
        BackColor = Color.FromArgb(24, 27, 33);
        ForeColor = Color.White;
        Font = new Font("Segoe UI", 10F);

        var title = new Label
        {
            Text = "AMEGA",
            Font = new Font("Segoe UI", 27F, FontStyle.Bold),
            ForeColor = Color.FromArgb(245, 190, 55),
            AutoSize = true,
            Location = new Point(28, 22)
        };
        var subtitle = new Label
        {
            Text = "Передача игр с компьютера на PlayStation 4 по локальной сети",
            ForeColor = Color.Gainsboro,
            AutoSize = true,
            Location = new Point(31, 76)
        };

        ConfigureButton(_chooseButton, "Выбрать PKG", 30, 135, 170);
        ConfigureButton(_startButton, "Искать PS4", 215, 135, 170);
        ConfigureButton(_stopButton, "Остановить", 400, 135, 170);

        _fileLabel.SetBounds(32, 205, 740, 28);
        _sizeLabel.SetBounds(32, 239, 740, 25);
        _hashLabel.SetBounds(32, 271, 740, 42);
        _hashLabel.AutoEllipsis = true;
        _hashLabel.ForeColor = Color.LightSteelBlue;

        _hashProgress.SetBounds(32, 316, 740, 12);
        _hashProgress.Minimum = 0;
        _hashProgress.Maximum = 100;
        _hashProgress.Value = 0;
        _hashProgress.Visible = false;

        _devicesTitle.SetBounds(32, 340, 740, 24);
        _devicesTitle.Text = "Найденные приставки:";
        _devicesTitle.ForeColor = Color.Gainsboro;

        _devicesList.SetBounds(32, 369, 740, 82);
        _devicesList.BackColor = Color.FromArgb(36, 41, 50);
        _devicesList.ForeColor = Color.White;
        _devicesList.BorderStyle = BorderStyle.FixedSingle;
        _devicesList.Items.Add("Пока приставки не найдены. Запусти AMEGA на PS4.");

        _stageLabel.SetBounds(32, 465, 740, 42);
        _stageLabel.Text = "Поиск PS4 можно запускать независимо от выбора и проверки PKG.";
        _stageLabel.ForeColor = Color.FromArgb(245, 190, 55);

        _statusLabel.SetBounds(32, 518, 740, 48);
        _statusLabel.Text = "Готово. Сначала можешь найти PS4 или выбрать PKG.";
        _statusLabel.ForeColor = Color.LightGreen;

        Controls.AddRange([
            title, subtitle, _chooseButton, _startButton, _stopButton,
            _fileLabel, _sizeLabel, _hashLabel, _hashProgress, _devicesTitle, _devicesList,
            _stageLabel, _statusLabel
        ]);

        _sender.ReceiverDiscovered += ReceiverWasDiscovered;
        _deviceExpiryTimer.Tick += (_, _) => RemoveExpiredDevices();
        _chooseButton.Click += ChooseFileClicked;
        _startButton.Click += StartClicked;
        _stopButton.Click += StopClicked;
        FormClosing += FormIsClosing;
        UpdateActionButtons();
    }

    private static void ConfigureButton(Button button, string text, int x, int y, int width)
    {
        button.Text = text;
        button.SetBounds(x, y, width, 44);
        button.FlatStyle = FlatStyle.Flat;
        button.FlatAppearance.BorderSize = 1;
        button.Font = new Font("Segoe UI", 10F, FontStyle.Bold);
        button.ForeColor = Color.White;
        button.Cursor = Cursors.Hand;
    }

    private void UpdateActionButtons()
    {
        _chooseButton.Enabled = true;
        _startButton.Enabled = !_isSearching;
        _stopButton.Enabled = _isSearching || _hashCts is not null;

        StyleButton(_chooseButton, enabled: true, Color.FromArgb(55, 64, 78), Color.White);
        StyleButton(_startButton, _startButton.Enabled, Color.FromArgb(245, 190, 55), Color.FromArgb(26, 29, 35));

        _stopButton.Text = _isSearching && _hashCts is not null
            ? "Отменить всё"
            : _hashCts is not null ? "Отменить проверку" : "Остановить поиск";
        StyleButton(_stopButton, _stopButton.Enabled, Color.FromArgb(190, 65, 58), Color.White);
    }

    private static void StyleButton(Button button, bool enabled, Color activeBack, Color activeFore)
    {
        button.BackColor = enabled ? activeBack : Color.FromArgb(48, 52, 60);
        button.ForeColor = enabled ? activeFore : Color.FromArgb(125, 130, 140);
        button.FlatAppearance.BorderColor = enabled ? activeBack : Color.FromArgb(65, 70, 78);
        button.Cursor = enabled ? Cursors.Hand : Cursors.Default;
    }

    private async void ChooseFileClicked(object? sender, EventArgs e)
    {
        using var dialog = new OpenFileDialog
        {
            Title = "Выберите пакет игры",
            Filter = "Пакеты PlayStation 4 (*.pkg)|*.pkg|Все файлы (*.*)|*.*",
            CheckFileExists = true,
            Multiselect = false
        };
        if (dialog.ShowDialog(this) != DialogResult.OK) return;

        // A new selection supersedes any hash already in progress.
        _hashCts?.Cancel();
        var path = dialog.FileName;
        var info = new FileInfo(path);
        _selectedFile = path;
        _selectedHash = null;
        _sender.UpdateOffer(null, null);

        using var cts = new CancellationTokenSource();
        _hashCts = cts;

        _hashProgress.Value = 0;
        _hashProgress.Visible = true;
        _fileLabel.Text = $"Файл: {info.Name}";
        _sizeLabel.Text = $"Размер: {FormatBytes(info.Length)}";
        _hashLabel.Text = "SHA-256: вычисляется — поиск PS4 при этом может продолжаться.";
        UpdateActionButtons();

        var progress = new Progress<int>(value =>
        {
            int safeValue = Math.Clamp(value, 0, 100);
            _hashProgress.Value = safeValue;
            _hashLabel.Text = $"SHA-256: проверка файла — {safeValue}%";
        });

        try
        {
            var digest = await CalculateSha256Async(path, info.Length, progress, cts.Token);
            if (!ReferenceEquals(_hashCts, cts) || !string.Equals(_selectedFile, path, StringComparison.OrdinalIgnoreCase))
                return;

            _selectedHash = Convert.ToHexString(digest).ToLowerInvariant();
            _sender.UpdateOffer(path, _selectedHash);
            _hashLabel.Text = $"SHA-256: {_selectedHash}";
            _hashProgress.Value = 100;
            _statusLabel.Text = _isSearching
                ? "PKG проверен. Поиск PS4 продолжается; предложение обновлено."
                : "PKG проверен. Теперь можно запустить поиск PS4.";
        }
        catch (OperationCanceledException)
        {
            if (ReferenceEquals(_hashCts, cts))
            {
                _selectedHash = null;
                _sender.UpdateOffer(null, null);
                _hashLabel.Text = "SHA-256: проверка отменена.";
                _statusLabel.Text = "Проверка отменена. Можно выбрать другой PKG или продолжить поиск PS4.";
            }
        }
        catch (Exception ex)
        {
            if (ReferenceEquals(_hashCts, cts))
            {
                _selectedHash = null;
                _sender.UpdateOffer(null, null);
                _hashLabel.Text = "SHA-256: ошибка.";
                _statusLabel.Text = $"Не удалось прочитать файл: {ex.Message}";
            }
        }
        finally
        {
            if (ReferenceEquals(_hashCts, cts))
            {
                _hashCts = null;
                UpdateActionButtons();
            }
        }
    }

    private static async Task<byte[]> CalculateSha256Async(
        string path, long fileLength, IProgress<int> progress, CancellationToken cancellationToken)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        await using var stream = new FileStream(
            path, FileMode.Open, FileAccess.Read, FileShare.Read,
            8 * 1024 * 1024, FileOptions.Asynchronous | FileOptions.SequentialScan);
        var buffer = new byte[8 * 1024 * 1024];
        long totalRead = 0;
        int lastPercent = -1;
        int read;

        while ((read = await stream.ReadAsync(buffer.AsMemory(0, buffer.Length), cancellationToken).ConfigureAwait(false)) > 0)
        {
            hash.AppendData(buffer, 0, read);
            totalRead += read;

            if (fileLength > 0)
            {
                int percent = (int)Math.Min(100L, totalRead * 100L / fileLength);
                if (percent != lastPercent)
                {
                    lastPercent = percent;
                    progress.Report(percent);
                }
            }
        }

        cancellationToken.ThrowIfCancellationRequested();
        return hash.GetHashAndReset();
    }

    private async void StartClicked(object? sender, EventArgs e)
    {
        if (_isSearching) return;

        try
        {
            _isSearching = true;
            _devices.Clear();
            _devicesList.Items.Clear();
            _devicesList.Items.Add("Ищу приставки AMEGA в локальной сети…");
            _statusLabel.Text = "Запускаю обнаружение. Выбор PKG и SHA-256 не блокируют поиск.";
            UpdateActionButtons();

            await _sender.StartAsync(_selectedFile, _selectedHash);
            _deviceExpiryTimer.Start();
            _statusLabel.Text = "Поиск запущен. Запусти AMEGA на PS4 и оставь обе программы открытыми.";
            _stageLabel.Text = $"Сеть ПК: {_sender.NetworkSummary}. Ожидаю ответ PS4.";
        }
        catch (Exception ex)
        {
            _isSearching = false;
            _statusLabel.Text = $"Не удалось запустить сетевой режим: {ex.Message}";
            UpdateActionButtons();
        }
    }

    private void ReceiverWasDiscovered(DiscoveredReceiver receiver)
    {
        if (IsDisposed || !IsHandleCreated) return;
        try
        {
            BeginInvoke(new Action(() =>
            {
                _devices[receiver.ReceiverId] = receiver;
                RefreshDeviceList();
                _statusLabel.Text = $"Найдена приставка: {receiver.DisplayName} ({receiver.IpAddress}).";
            }));
        }
        catch (InvalidOperationException) { }
    }

    private void RemoveExpiredDevices()
    {
        var cutoff = DateTimeOffset.UtcNow.AddSeconds(-12);
        foreach (var id in _devices.Where(pair => pair.Value.LastSeen < cutoff).Select(pair => pair.Key).ToArray())
            _devices.Remove(id);
        RefreshDeviceList();
    }

    private void RefreshDeviceList()
    {
        _devicesList.Items.Clear();
        if (_devices.Count == 0)
        {
            _devicesList.Items.Add("Пока приставки не найдены. Проверь одну Wi-Fi/LAN сеть и брандмауэр.");
            return;
        }

        foreach (var device in _devices.Values.OrderBy(x => x.DisplayName))
            _devicesList.Items.Add(device.DisplayLabel);
    }

    private async void StopClicked(object? sender, EventArgs e)
    {
        bool wasHashing = _hashCts is not null;
        _hashCts?.Cancel();
        if (_isSearching)
            await StopSenderAsync();

        _statusLabel.Text = wasHashing && !_isSearching
            ? "Отмена проверки и остановка поиска запрошены."
            : "Сетевой поиск остановлен.";
        UpdateActionButtons();
    }

    private async void FormIsClosing(object? sender, FormClosingEventArgs e)
    {
        _hashCts?.Cancel();
        await StopSenderAsync();
        await _sender.DisposeAsync();
    }

    private async Task StopSenderAsync()
    {
        _deviceExpiryTimer.Stop();
        _isSearching = false;
        try { await _sender.StopAsync(); }
        catch (Exception ex) { _statusLabel.Text = $"Ошибка остановки: {ex.Message}"; }
        UpdateActionButtons();
    }

    private static string FormatBytes(long bytes)
    {
        string[] units = ["Б", "КБ", "МБ", "ГБ", "ТБ"];
        double value = bytes;
        var unit = 0;
        while (value >= 1024 && unit < units.Length - 1)
        {
            value /= 1024;
            unit++;
        }
        return $"{value:0.##} {units[unit]}";
    }
}
