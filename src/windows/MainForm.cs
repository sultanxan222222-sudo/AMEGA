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
    private readonly SenderHost _sender = new();
    private string? _selectedFile;
    private string? _selectedHash;

    public MainForm()
    {
        Text = "AMEGA — отправка на PS4";
        StartPosition = FormStartPosition.CenterScreen;
        MinimumSize = new Size(720, 480);
        Size = new Size(820, 540);
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
        ConfigureButton(_startButton, "Запустить поиск PS4", 215, 135, 210);
        ConfigureButton(_stopButton, "Остановить", 440, 135, 150);
        _startButton.Enabled = false;
        _stopButton.Enabled = false;

        _fileLabel.SetBounds(32, 205, 740, 28);
        _sizeLabel.SetBounds(32, 242, 740, 25);
        _hashLabel.SetBounds(32, 278, 740, 48);
        _hashLabel.AutoEllipsis = true;
        _hashLabel.ForeColor = Color.LightSteelBlue;

        _stageLabel.SetBounds(32, 350, 740, 45);
        _stageLabel.Text = "Этап 1: выбор файла и обнаружение компьютера в локальной сети.";
        _stageLabel.ForeColor = Color.FromArgb(245, 190, 55);

        _statusLabel.SetBounds(32, 410, 740, 38);
        _statusLabel.Text = "Готово к работе. Выберите PKG-файл.";
        _statusLabel.ForeColor = Color.LightGreen;

        Controls.AddRange([
            title, subtitle, _chooseButton, _startButton, _stopButton,
            _fileLabel, _sizeLabel, _hashLabel, _stageLabel, _statusLabel
        ]);

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

        if (dialog.ShowDialog(this) != DialogResult.OK)
            return;

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
            _statusLabel.Text = "Файл проверен. Можно запустить сетевое обнаружение.";
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
        if (_selectedFile is null || _selectedHash is null)
            return;

        try
        {
            _startButton.Enabled = false;
            _chooseButton.Enabled = false;
            _statusLabel.Text = "Запускаю локальный сервер и объявление в сети…";
            await _sender.StartAsync(_selectedFile, _selectedHash);
            _stopButton.Enabled = true;
            _statusLabel.Text = $"AMEGA объявляет себя в локальной сети. HTTP-порт: {SenderHost.DefaultHttpPort}, UDP-порт: {SenderHost.DefaultDiscoveryPort}.";
            _stageLabel.Text = "Режим обнаружения. Приём и передача файлов будут включены после реализации pairing и защищённого канала.";
        }
        catch (Exception ex)
        {
            _startButton.Enabled = true;
            _chooseButton.Enabled = true;
            _statusLabel.Text = $"Не удалось запустить сетевой режим: {ex.Message}";
        }
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
