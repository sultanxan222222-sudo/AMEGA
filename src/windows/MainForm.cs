using System.Security.Cryptography;
using AMEGA.Windows.Networking;

namespace AMEGA.Windows;

internal sealed class MainForm : Form
{
    private readonly Button _chooseButton = new();
    private readonly Button _startButton = new();
    private readonly Button _stopButton = new();
    private readonly Button _sendButton = new();
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
    private bool _searchRunning;

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
        ConfigureButton(_stopButton, "Остановить", 400, 135, 150);
        ConfigureButton(_sendButton, "Отправить", 565, 135, 170);
        _startButton.Enabled = false;
        _stopButton.Enabled = false;
        _sendButton.Enabled = false;

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
        _stageLabel.Text = "Этап 1: двустороннее обнаружение в локальной сети.";
        _stageLabel.ForeColor = Color.FromArgb(245, 190, 55);

        _statusLabel.SetBounds(32, 518, 740, 48);
        _statusLabel.Text = "Готово к работе. Выберите PKG-файл.";
        _statusLabel.ForeColor = Color.LightGreen;

        Controls.AddRange([
            title, subtitle, _chooseButton, _startButton, _stopButton, _sendButton,
            _fileLabel, _sizeLabel, _hashLabel, _hashProgress, _devicesTitle, _devicesList,
            _stageLabel, _statusLabel
        ]);

        _sender.ReceiverDiscovered += ReceiverWasDiscovered;
        _deviceExpiryTimer.Tick += (_, _) => RemoveExpiredDevices();
        _chooseButton.Click += ChooseFileClicked;
        _startButton.Click += StartClicked;
        _stopButton.Click += StopClicked;
        _sendButton.Click += SendClicked;
        _devicesList.SelectedIndexChanged += (_, _) =>
            _sendButton.Enabled = _searchRunning && _devicesList.SelectedIndex >= 0 && _devices.Count > 0;
        FormClosing += FormIsClosing;
    }

    private static void ConfigureButton(Button button, string text, int x, int y, int width)
    {
        button.Text = text;
        button.SetBounds(x, y, width, 44);
        button.FlatStyle = FlatStyle.Flat;
        button.FlatAppearance.BorderColor = Color.FromArgb(245, 190, 55);
        button.BackColor = Color.FromArgb(44, 49, 59);
        button.ForeColor = Color.White;
        button.Cursor = Cursors.Hand;
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

        var path = dialog.FileName;
        var info = new FileInfo(path);
        using var cts = new CancellationTokenSource();
        _hashCts = cts;
        _chooseButton.Enabled = false;
        _startButton.Enabled = false;
        _stopButton.Enabled = true;
        _stopButton.Text = "Отменить";
        _hashProgress.Value = 0;
        _hashProgress.Visible = true;
        _fileLabel.Text = $"Файл: {info.Name}";
        _sizeLabel.Text = $"Размер: {FormatBytes(info.Length)}";
        _hashLabel.Text = "SHA-256: вычисляется…";
        _statusLabel.Text = "Проверяю файл. Не закрывай программу — можно отменить.";
        var progress = new Progress<int>(value =>
        {
            _hashProgress.Value = Math.Clamp(value, 0, 100);
            _statusLabel.Text = $"Вычисляю SHA-256: {value}%";
        });

        try
        {
            var digest = await CalculateSha256Async(path, info.Length, progress, cts.Token);
            _selectedFile = path;
            _selectedHash = Convert.ToHexString(digest).ToLowerInvariant();
            _hashLabel.Text = $"SHA-256: {_selectedHash}";
            _hashProgress.Value = 100;
            _startButton.Enabled = true;
            _statusLabel.Text = "Файл проверен. Теперь можно нажать «Искать PS4».";
        }
        catch (OperationCanceledException)
        {
            _selectedFile = null;
            _selectedHash = null;
            _hashLabel.Text = "SHA-256: проверка отменена.";
            _statusLabel.Text = "Проверка отменена. Выбери PKG ещё раз.";
        }
        catch (Exception ex)
        {
            _selectedFile = null;
            _selectedHash = null;
            _hashLabel.Text = "SHA-256: ошибка.";
            _statusLabel.Text = $"Не удалось прочитать файл: {ex.Message}";
        }
        finally
        {
            if (ReferenceEquals(_hashCts, cts)) _hashCts = null;
            _stopButton.Text = "Остановить";
            _stopButton.Enabled = false;
            _chooseButton.Enabled = true;
        }
    }

    private static async Task<byte[]> CalculateSha256Async(
        string path, long fileLength, IProgress<int> progress, CancellationToken cancellationToken)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        await using var stream = new FileStream(
            path, FileMode.Open, FileAccess.Read, FileShare.Read,
            4 * 1024 * 1024, FileOptions.Asynchronous | FileOptions.SequentialScan);
        var buffer = new byte[4 * 1024 * 1024];
        long totalRead = 0;
        int read;
        while ((read = await stream.ReadAsync(buffer.AsMemory(0, buffer.Length), cancellationToken)) > 0)
        {
            hash.AppendData(buffer, 0, read);
            totalRead += read;
            if (fileLength > 0)
                progress.Report((int)Math.Min(100L, totalRead * 100L / fileLength));
        }
        cancellationToken.ThrowIfCancellationRequested();
        return hash.GetHashAndReset();
    }

    private async void StartClicked(object? sender, EventArgs e)
    {
        if (_selectedFile is null || _selectedHash is null) return;

        try
        {
            _startButton.Enabled = false;
            _chooseButton.Enabled = false;
            _devices.Clear();
            _devicesList.Items.Clear();
            _devicesList.Items.Add("Ищу приставки AMEGA в локальной сети…");
            _statusLabel.Text = "Запускаю двустороннее сетевое обнаружение…";
            await _sender.StartAsync(_selectedFile, _selectedHash);
            _searchRunning = true;
            _deviceExpiryTimer.Start();
            _stopButton.Enabled = true;
            _statusLabel.Text = $"Поиск активен: UDP {SenderHost.SenderDiscoveryPort}/{SenderHost.ReceiverDiscoveryPort}. Выбери найденную PS4 и нажми «Отправить».";
            _stageLabel.Text = $"Сеть: {_sender.NetworkSummary}. Directed broadcast и резервный поиск по адресам подсети активны.";
        }
        catch (Exception ex)
        {
            _startButton.Enabled = true;
            _chooseButton.Enabled = true;
            _statusLabel.Text = $"Не удалось запустить сетевой режим: {ex.Message}";
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
                _statusLabel.Text = $"Найдена приставка: {receiver.DisplayName} ({receiver.IpAddress}). Выбери её и нажми «Отправить»; на PS4 подтверди кнопкой X.";
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
        var previous = _devicesList.SelectedItem as string;
        _devicesList.Items.Clear();
        if (_devices.Count == 0)
        {
            _devicesList.Items.Add("Пока приставки не найдены. Запусти AMEGA на PS4.");
            _sendButton.Enabled = false;
            return;
        }

        var ordered = _devices.Values.OrderBy(x => x.DisplayName).ToArray();
        foreach (var device in ordered)
            _devicesList.Items.Add(device.DisplayLabel);

        int selected = previous is null ? -1 :
            Array.FindIndex(ordered, device => device.DisplayLabel == previous);
        if (selected < 0 && ordered.Length == 1) selected = 0;
        if (selected >= 0) _devicesList.SelectedIndex = selected;
        _sendButton.Enabled = _searchRunning && _devicesList.SelectedIndex >= 0;
    }

    private void SendClicked(object? sender, EventArgs e)
    {
        var ordered = _devices.Values.OrderBy(x => x.DisplayName).ToArray();
        int index = _devicesList.SelectedIndex;
        if (!_searchRunning || index < 0 || index >= ordered.Length)
        {
            _statusLabel.Text = "Сначала найди и выбери приставку PS4.";
            return;
        }

        var receiver = ordered[index];
        try
        {
            _sender.ArmTransfer(receiver.IpAddress);
            _sendButton.Enabled = false;
            _statusLabel.Text = $"Предложение отправлено на {receiver.DisplayName} ({receiver.IpAddress}). На PS4 нажми X для приёма файла.";
            _stageLabel.Text = $"PKG доступен только выбранной приставке. HTTP Range включён для продолжения после обрыва.";
        }
        catch (Exception ex)
        {
            _statusLabel.Text = $"Не удалось отправить предложение: {ex.Message}";
        }
    }

    private async void StopClicked(object? sender, EventArgs e)
    {
        if (_hashCts is not null)
        {
            _hashCts.Cancel();
            _statusLabel.Text = "Отменяю проверку файла…";
            return;
        }
        await StopSenderAsync();
        _statusLabel.Text = "Сетевой режим остановлен.";
    }

    private async void FormIsClosing(object? sender, FormClosingEventArgs e)
    {
        await StopSenderAsync();
        await _sender.DisposeAsync();
    }

    private async Task StopSenderAsync()
    {
        _deviceExpiryTimer.Stop();
        _searchRunning = false;
        try { await _sender.StopAsync(); }
        catch (Exception ex) { _statusLabel.Text = $"Ошибка остановки: {ex.Message}"; }
        _stopButton.Enabled = false;
        _sendButton.Enabled = false;
        _startButton.Enabled = _selectedFile is not null;
        _chooseButton.Enabled = true;
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
