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
    private readonly Label _stageLabel = new();
    private readonly Label _devicesTitle = new();
    private readonly ListBox _devicesList = new();
    private readonly Dictionary<string, DiscoveredReceiver> _devices = new();
    private readonly SenderHost _sender = new();
    private readonly System.Windows.Forms.Timer _deviceExpiryTimer = new() { Interval = 3000 };
    private string? _selectedFile;
    private string? _selectedHash;

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
        _startButton.Enabled = false;
        _stopButton.Enabled = false;

        _fileLabel.SetBounds(32, 205, 740, 28);
        _sizeLabel.SetBounds(32, 239, 740, 25);
        _hashLabel.SetBounds(32, 271, 740, 42);
        _hashLabel.AutoEllipsis = true;
        _hashLabel.ForeColor = Color.LightSteelBlue;

        _devicesTitle.SetBounds(32, 322, 740, 24);
        _devicesTitle.Text = "Найденные приставки:";
        _devicesTitle.ForeColor = Color.Gainsboro;

        _devicesList.SetBounds(32, 351, 740, 82);
        _devicesList.BackColor = Color.FromArgb(36, 41, 50);
        _devicesList.ForeColor = Color.White;
        _devicesList.BorderStyle = BorderStyle.FixedSingle;
        _devicesList.Items.Add("Пока приставки не найдены. Запусти AMEGA на PS4.");

        _stageLabel.SetBounds(32, 447, 740, 42);
        _stageLabel.Text = "Этап 1: двустороннее обнаружение в локальной сети.";
        _stageLabel.ForeColor = Color.FromArgb(245, 190, 55);

        _statusLabel.SetBounds(32, 500, 740, 48);
        _statusLabel.Text = "Готово к работе. Выберите PKG-файл.";
        _statusLabel.ForeColor = Color.LightGreen;

        Controls.AddRange([
            title, subtitle, _chooseButton, _startButton, _stopButton,
            _fileLabel, _sizeLabel, _hashLabel, _devicesTitle, _devicesList,
            _stageLabel, _statusLabel
        ]);

        _sender.ReceiverDiscovered += ReceiverWasDiscovered;
        _deviceExpiryTimer.Tick += (_, _) => RemoveExpiredDevices();
        _chooseButton.Click += ChooseFileClicked;
        _startButton.Click += StartClicked;
        _stopButton.Click += StopClicked;
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

        try
        {
            _chooseButton.Enabled = false;
            _statusLabel.Text = "Проверяю файл и вычисляю SHA-256…";
            var path = dialog.FileName;
            var info = new FileInfo(path);

            await using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1024 * 1024, useAsync: true);
            var digest = await SHA256.HashDataAsync(stream);

            _selectedFile = path;
            _selectedHash = Convert.ToHexString(digest).ToLowerInvariant();
            _fileLabel.Text = $"Файл: {info.Name}";
            _sizeLabel.Text = $"Размер: {FormatBytes(info.Length)}";
            _hashLabel.Text = $"SHA-256: {_selectedHash}";
            _startButton.Enabled = true;
            _statusLabel.Text = "Файл проверен. Можно запустить поиск приставки.";
        }
        catch (Exception ex)
        {
            _selectedFile = null;
            _selectedHash = null;
            _startButton.Enabled = false;
            _statusLabel.Text = $"Не удалось прочитать файл: {ex.Message}";
        }
        finally
        {
            _chooseButton.Enabled = true;
        }
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
            _deviceExpiryTimer.Start();
            _stopButton.Enabled = true;
            _statusLabel.Text = $"ПК объявлен в сети. Ожидаю PS4 на UDP-порту {SenderHost.DefaultDiscoveryPort}.";
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
            _devicesList.Items.Add("Пока приставки не найдены. Запусти AMEGA на PS4.");
            return;
        }

        foreach (var device in _devices.Values.OrderBy(x => x.DisplayName))
            _devicesList.Items.Add(device.DisplayLabel);
    }

    private async void StopClicked(object? sender, EventArgs e)
    {
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
        try { await _sender.StopAsync(); }
        catch (Exception ex) { _statusLabel.Text = $"Ошибка остановки: {ex.Message}"; }
        _stopButton.Enabled = false;
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
