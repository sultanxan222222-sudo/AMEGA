using System.Security.Cryptography;
using AMEGA.Windows.Networking;

namespace AMEGA.Windows;

internal sealed class MainForm : Form
{
    private readonly Button _chooseButton = new();
    private readonly Button _startButton = new();
    private readonly Button _stopButton = new();
    private readonly Button _sendButton = new();
    private readonly Button _rpiCheckButton = new();
    private readonly Button _rpiInstallButton = new();
    private readonly Label _rpiStatusLabel = new();
    private readonly RpiInstallerClient _rpiClient = new();
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
    private readonly System.Windows.Forms.Timer _rpiProgressTimer = new() { Interval = 4000 };
    private string? _selectedFile;
    private string? _selectedHash;
    private string? _selectedReceiverId;
    private string? _offeredReceiverId;
    private bool _offerSent;
    private bool _refreshingDevices;
    private bool _isSearching;
    private DiscoveredReceiver? _lastSelectedReceiver;
    private string? _rpiReadyReceiverId;
    private string? _rpiReadyIp;
    private bool _rpiBusy;
    private bool _rpiInstallActive;
    private bool _rpiInstallRequestInFlight;
    private bool _startInProgress;
    private bool _stopRequestedDuringStartup;
    private long? _rpiTaskId;
    private string? _rpiTaskConsoleIp;
    private bool _rpiPollInFlight;

    public MainForm()
    {
        Text = "AMEGA — отправка на PS4";
        StartPosition = FormStartPosition.CenterScreen;
        MinimumSize = new Size(760, 700);
        Size = new Size(820, 730);
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
        ConfigureButton(_sendButton, "Отправить PKG", 585, 135, 170);

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

        ConfigureButton(_rpiCheckButton, "Проверить RPI", 32, 465, 180);
        ConfigureButton(_rpiInstallButton, "Установить через RPI", 225, 465, 220);
        _rpiStatusLabel.SetBounds(462, 465, 310, 48);
        _rpiStatusLabel.Text = "RPI: не проверен. Нужен отдельный Standalone RPI на PS4.";
        _rpiStatusLabel.ForeColor = Color.LightSteelBlue;
        _rpiStatusLabel.AutoEllipsis = true;

        _stageLabel.SetBounds(32, 530, 740, 36);
        _stageLabel.Text = "Поиск PS4 можно запускать независимо от выбора и проверки PKG.";
        _stageLabel.ForeColor = Color.FromArgb(245, 190, 55);

        _statusLabel.SetBounds(32, 575, 740, 62);
        _statusLabel.Text = "Готово. Сначала можешь найти PS4 или выбрать PKG.";
        _statusLabel.ForeColor = Color.LightGreen;

        Controls.AddRange([
            title, subtitle, _chooseButton, _startButton, _stopButton, _sendButton,
            _fileLabel, _sizeLabel, _hashLabel, _hashProgress, _devicesTitle, _devicesList,
            _rpiCheckButton, _rpiInstallButton, _rpiStatusLabel, _stageLabel, _statusLabel
        ]);

        _sender.ReceiverDiscovered += ReceiverWasDiscovered;
        _deviceExpiryTimer.Tick += (_, _) => RemoveExpiredDevices();
        _rpiProgressTimer.Tick += (_, _) => UpdateRpiTransferProgress();
        _chooseButton.Click += ChooseFileClicked;
        _startButton.Click += StartClicked;
        _sendButton.Click += SendClicked;
        _rpiCheckButton.Click += CheckRpiClicked;
        _rpiInstallButton.Click += InstallViaRpiClicked;
        _devicesList.SelectedIndexChanged += DeviceSelectionChanged;
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
        _chooseButton.Enabled = !_rpiInstallActive && !_rpiBusy;
        // Keep the controls responsive during the asynchronous socket/Kestrel
        // startup. Search is disabled only while starting or already running;
        // Stop remains available during startup and during an active search.
        _startButton.Enabled = !_isSearching && !_startInProgress;
        _stopButton.Enabled = (_isSearching || _startInProgress || _hashCts is not null) &&
            !_rpiInstallRequestInFlight && !_stopRequestedDuringStartup;

        StyleButton(_chooseButton, enabled: true, Color.FromArgb(55, 64, 78), Color.White);
        StyleButton(_startButton, _startButton.Enabled, Color.FromArgb(245, 190, 55), Color.FromArgb(26, 29, 35));

        _stopButton.Text = _stopRequestedDuringStartup
            ? "Отмена запуска…"
            : _startInProgress ? "Отменить запуск"
            : _isSearching && _hashCts is not null ? "Отменить всё"
            : _hashCts is not null ? "Отменить проверку" : "Остановить поиск";
        StyleButton(_stopButton, _stopButton.Enabled, Color.FromArgb(190, 65, 58), Color.White);

        _sendButton.Text = _offerSent ? "Предложение активно" : "Отправить PKG";
        _sendButton.Enabled = _isSearching && !_rpiBusy && !_rpiInstallActive && !_offerSent &&
            _hashCts is null && !string.IsNullOrWhiteSpace(_selectedFile) &&
            !string.IsNullOrWhiteSpace(_selectedHash) && GetSelectedReceiver() is not null;
        StyleButton(_sendButton, _sendButton.Enabled,
            Color.FromArgb(99, 205, 145), Color.FromArgb(26, 29, 35));

        _devicesList.Enabled = !_rpiBusy && !_rpiInstallActive;
        DiscoveredReceiver? rpiTarget = GetRpiTarget();
        bool rpiTargetReady = rpiTarget is not null &&
            string.Equals(rpiTarget.ReceiverId, _rpiReadyReceiverId, StringComparison.OrdinalIgnoreCase) &&
            string.Equals(rpiTarget.IpAddress, _rpiReadyIp, StringComparison.OrdinalIgnoreCase);
        _rpiCheckButton.Enabled = _isSearching && !_rpiBusy && !_rpiInstallActive && rpiTarget is not null;
        _rpiInstallButton.Text = _rpiInstallActive ? "Снять доступ RPI" : "Установить через RPI";
        _rpiInstallButton.Enabled = _isSearching && !_rpiBusy &&
            (_rpiInstallActive || (rpiTargetReady && _hashCts is null &&
            !string.IsNullOrWhiteSpace(_selectedFile) && !string.IsNullOrWhiteSpace(_selectedHash)));
        StyleButton(_rpiCheckButton, _rpiCheckButton.Enabled,
            Color.FromArgb(65, 105, 140), Color.White);
        StyleButton(_rpiInstallButton, _rpiInstallButton.Enabled,
            Color.FromArgb(99, 205, 145), Color.FromArgb(26, 29, 35));
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
        _offerSent = false;
        _offeredReceiverId = null;
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
            // A verified PKG is only published to the PS4 after the user
            // selects a receiver and presses Send.
            _sender.UpdateOffer(null, null);
            _hashLabel.Text = $"SHA-256: {_selectedHash}";
            _hashProgress.Value = 100;
            _statusLabel.Text = _isSearching
                ? "PKG проверен. Выбери приставку и нажми «Отправить PKG»."
                : "PKG проверен. Запусти поиск PS4, затем нажми «Отправить PKG».";
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
        if (_isSearching || _startInProgress) return;

        _startInProgress = true;
        _stopRequestedDuringStartup = false;
        try
        {
            // Do not mark discovery active until the HTTP host and UDP
            // listener have both started successfully. This prevents a failed
            // startup from leaving the UI in a false "searching" state.
            _isSearching = false;
            _devices.Clear();
            _lastSelectedReceiver = null;
            _rpiReadyReceiverId = null;
            _rpiReadyIp = null;
            _rpiStatusLabel.Text = "RPI: выбери приставку и проверь Standalone RPI.";
            _devicesList.Items.Clear();
            _devicesList.Items.Add("Ищу приставки AMEGA в локальной сети…");
            _statusLabel.Text = "Запускаю обнаружение. Выбор PKG и SHA-256 не блокируют поиск.";
            UpdateActionButtons();

            _offerSent = false;
            _offeredReceiverId = null;
            _sender.UpdateOffer(null, null);
            // SenderHost.StartAsync performs adapter enumeration, Kestrel
            // construction and socket binding before its first incomplete await.
            // Run the whole startup off the WinForms UI thread so both buttons
            // remain clickable while network services are being prepared.
            await Task.Run(() => _sender.StartAsync());
            _isSearching = true;
            _startInProgress = false;

            // Stop can be pressed while Kestrel is starting. In that short
            // interval SenderHost has not yet finished opening its sockets,
            // so defer the stop until StartAsync has completed.
            if (_stopRequestedDuringStartup)
            {
                await StopSenderAsync();
                _statusLabel.Text = "Запуск поиска отменён пользователем.";
                return;
            }

            _deviceExpiryTimer.Start();
            _statusLabel.Text = "Поиск запущен. Запусти AMEGA на PS4 и оставь обе программы открытыми.";
            _stageLabel.Text = $"Сеть ПК: {_sender.NetworkSummary}. Ожидаю ответ PS4.";
        }
        catch (Exception ex)
        {
            _startInProgress = false;
            _stopRequestedDuringStartup = false;
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

    private DiscoveredReceiver? GetSelectedReceiver()
    {
        if (_devicesList.SelectedIndex < 0) return null;
        var ordered = _devices.Values.OrderBy(x => x.DisplayName).ToList();
        return _devicesList.SelectedIndex < ordered.Count
            ? ordered[_devicesList.SelectedIndex]
            : null;
    }

    private void DeviceSelectionChanged(object? sender, EventArgs e)
    {
        if (_refreshingDevices) return;
        var selected = GetSelectedReceiver();
        string? newId = selected?.ReceiverId;
        if (selected is not null) _lastSelectedReceiver = selected;
        if (_offerSent && !string.Equals(newId, _offeredReceiverId, StringComparison.OrdinalIgnoreCase))
        {
            _sender.UpdateOffer(null, null);
            _offerSent = false;
            _offeredReceiverId = null;
            _statusLabel.Text = "Выбрана другая приставка. Нажми «Отправить PKG» ещё раз.";
        }
        _selectedReceiverId = newId;
        UpdateActionButtons();
    }

    private void RefreshDeviceList()
    {
        var ordered = _devices.Values.OrderBy(x => x.DisplayName).ToList();
        _refreshingDevices = true;
        _devicesList.BeginUpdate();
        _devicesList.Items.Clear();

        if (ordered.Count == 0)
        {
            _devicesList.Items.Add("Пока приставки не найдены. Проверь одну Wi-Fi/LAN сеть и брандмауэр.");
            _selectedReceiverId = null;
        }
        else
        {
            foreach (var device in ordered)
                _devicesList.Items.Add(device.DisplayLabel);

            int selectedIndex = _selectedReceiverId is null
                ? -1
                : ordered.FindIndex(x => string.Equals(
                    x.ReceiverId, _selectedReceiverId, StringComparison.OrdinalIgnoreCase));
            if (selectedIndex < 0) selectedIndex = 0;
            _devicesList.SelectedIndex = selectedIndex;
            _selectedReceiverId = ordered[selectedIndex].ReceiverId;
            _lastSelectedReceiver = ordered[selectedIndex];
        }

        _devicesList.EndUpdate();
        _refreshingDevices = false;

        if (_offerSent && !string.Equals(_selectedReceiverId, _offeredReceiverId, StringComparison.OrdinalIgnoreCase))
        {
            _sender.UpdateOffer(null, null);
            _offerSent = false;
            _offeredReceiverId = null;
            _statusLabel.Text = "Целевая приставка больше не найдена. Выбери устройство и отправь PKG заново.";
        }
        UpdateActionButtons();
    }

    private DiscoveredReceiver? GetRpiTarget() => GetSelectedReceiver() ?? _lastSelectedReceiver;

    private async void UpdateRpiTransferProgress()
    {
        if (!_rpiInstallActive) { _rpiProgressTimer.Stop(); return; }

        if (_rpiTaskId is long taskId && !string.IsNullOrWhiteSpace(_rpiTaskConsoleIp) && !_rpiPollInFlight)
        {
            _rpiPollInFlight = true;
            try
            {
                RpiTaskProgress? task = await _rpiClient.GetTaskProgressAsync(_rpiTaskConsoleIp, taskId, CancellationToken.None);
                if (task is not null)
                {
                    if (task.ErrorCode != 0)
                    {
                        _rpiStatusLabel.Text = $"RPI сообщил ошибку задачи №{taskId}: 0x{unchecked((uint)task.ErrorCode):X8}. Проверь PS4.";
                        _statusLabel.Text = "RPI вернул ошибку задачи. Исходный PKG на компьютере сохранён; проверь сообщение на приставке.";
                        _rpiProgressTimer.Stop();
                        return;
                    }
                    long total = task.TotalBytes;
                    long transferred = Math.Clamp(task.TransferredBytes, 0, Math.Max(0, total));
                    int percent = total > 0 ? (int)Math.Clamp((double)transferred * 100d / total, 0d, 100d) : 0;
                    string amount = total > 0 ? $"{FormatBytes(transferred)} / {FormatBytes(total)}" : "размер пока не сообщён";
                    string phase = task.PreparingPercent < 100 && transferred == 0 ? $"подготовка {task.PreparingPercent}%" :
                        transferred < total ? "загрузка" : task.LocalCopyPercent < 100 ? $"установка на PS4 {task.LocalCopyPercent}%" : "установка завершена";
                    _rpiStatusLabel.Text = $"RPI задача №{taskId}: {phase}; передано {percent}% ({amount}).";
                    if (total > 0 && transferred >= total && task.LocalCopyPercent >= 100)
                    {
                        _rpiStatusLabel.Text = $"RPI подтвердил завершение задачи №{taskId}: PKG передан и локальное копирование завершено.";
                        _statusLabel.Text = "Получено подтверждение завершения от Standalone RPI. Исходный PKG на ПК не удалён; временный сетевой доступ можно закрыть.";
                        _rpiProgressTimer.Stop();
                    }
                    return;
                }
            }
            catch (Exception ex)
            {
                _rpiStatusLabel.Text = $"RPI API временно не отвечает; использую данные HTTP-источника. {ex.Message}";
            }
            finally { _rpiPollInFlight = false; }
        }

        var progress = _sender.GetRpiTransferProgress();
        if (!progress.IsActive || progress.TotalBytes <= 0)
        {
            _rpiStatusLabel.Text = "RPI: временный источник файла недоступен или истёк.";
            _rpiProgressTimer.Stop();
            return;
        }
        long delivered = Math.Clamp(progress.BytesServed, 0, progress.TotalBytes);
        int sourcePercent = (int)(delivered * 100L / progress.TotalBytes);
        string sourceAmount = $"{FormatBytes(delivered)} / {FormatBytes(progress.TotalBytes)}";
        _rpiStatusLabel.Text = sourcePercent >= 100
            ? $"Источник ПК прочитал все диапазоны PKG ({sourceAmount}). Ожидаю подтверждение RPI об установке."
            : delivered > 0 ? $"RPI запрашивает PKG с ПК: {sourcePercent}% ({sourceAmount}). Не закрывай AMEGA."
            : "RPI принял задачу. Ожидаю запросы PKG от PS4...";
        if (sourcePercent >= 100)
            _statusLabel.Text = "Все диапазоны PKG прочитаны HTTP-источником ПК. Это ещё не подтверждение завершения установки на PS4.";
    }
    private async void CheckRpiClicked(object? sender, EventArgs e)
    {
        DiscoveredReceiver? target = GetRpiTarget();
        if (!_isSearching || target is null) return;

        _rpiBusy = true;
        _rpiStatusLabel.Text = "RPI: проверяю API на PS4...";
        UpdateActionButtons();
        try
        {
            RpiProbeResult result = await _rpiClient.ProbeAsync(target.IpAddress, CancellationToken.None);
            if (result.IsResponsive)
            {
                _rpiReadyReceiverId = target.ReceiverId;
                _rpiReadyIp = target.IpAddress;
                _rpiStatusLabel.Text = $"RPI: готов. {result.Message}";
            }
            else
            {
                _rpiReadyReceiverId = null;
                _rpiReadyIp = null;
                _rpiStatusLabel.Text = $"RPI: не готов. {result.Message}";
                _statusLabel.Text =
                    "Открой на PS4 отдельную программу Remote Package Installer (Standalone RPI) и повтори проверку.";
            }
        }
        catch (Exception ex)
        {
            _rpiReadyReceiverId = null;
            _rpiReadyIp = null;
            _rpiStatusLabel.Text = $"RPI: ошибка проверки — {ex.Message}";
        }
        finally
        {
            _rpiBusy = false;
            UpdateActionButtons();
        }
    }

    private async void InstallViaRpiClicked(object? sender, EventArgs e)
    {
        if (_rpiInstallActive)
        {
            var releaseChoice = MessageBox.Show(this,
                "Снимать доступ можно только после того, как RPI закончил скачивать PKG. " +
                "Это не отменяет уже созданную задачу установки. Продолжить?",
                "Завершение сетевой раздачи RPI",
                MessageBoxButtons.YesNo, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2);
            if (releaseChoice == DialogResult.Yes)
            {
                _sender.RevokeRpiFileAccess();
                _rpiInstallActive = false;
                _rpiProgressTimer.Stop();
                _rpiStatusLabel.Text = "RPI: временный доступ к PKG закрыт.";
                UpdateActionButtons();
            }
            return;
        }

        DiscoveredReceiver? target = GetRpiTarget();
        string? path = _selectedFile;
        string? selectedHash = _selectedHash;
        if (!_isSearching || target is null ||
            !string.Equals(target.ReceiverId, _rpiReadyReceiverId, StringComparison.OrdinalIgnoreCase) ||
            !string.Equals(target.IpAddress, _rpiReadyIp, StringComparison.OrdinalIgnoreCase) ||
            string.IsNullOrWhiteSpace(path) || string.IsNullOrWhiteSpace(selectedHash))
        {
            _rpiStatusLabel.Text = "Сначала выбери PS4, проверь RPI и выбери проверенный PKG.";
            UpdateActionButtons();
            return;
        }

        var confirmation = MessageBox.Show(this,
            $"Передать на установку файл:\n{Path.GetFileName(path)}\n\n" +
            $"Приставка: {target.DisplayName} ({target.IpAddress})\n\n" +
            "Установка начнётся только после этого подтверждения. На PS4 должна быть открыта " +
            "программа Standalone Remote Package Installer. Не закрывай AMEGA на ПК, пока RPI скачивает PKG.\n\n" +
            "RPI вернёт номер задачи, но это ещё не подтверждение завершённой установки. Продолжить?",
            "Подтверждение установки PKG",
            MessageBoxButtons.YesNo, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2);
        if (confirmation != DialogResult.Yes) return;

        _rpiBusy = true;
        _rpiStatusLabel.Text = "Проверяю подпись и SHA-256 выбранного PKG...";
        _hashProgress.Visible = true;
        _hashProgress.Value = 0;
        UpdateActionButtons();

        try
        {
            if (!await HasPkgSignatureAsync(path))
            {
                _rpiStatusLabel.Text = "Отказ: нет стандартной сигнатуры PS4 PKG (7F 43 4E 54).";
                return;
            }

            var progress = new Progress<int>(value =>
            {
                _hashProgress.Value = Math.Clamp(value, 0, 100);
                _hashLabel.Text = $"SHA-256 перед установкой: {value}%";
            });
            var digest = await CalculateSha256Async(path, new FileInfo(path).Length, progress, CancellationToken.None);
            string actualHash = Convert.ToHexString(digest).ToLowerInvariant();
            if (!string.Equals(actualHash, selectedHash, StringComparison.OrdinalIgnoreCase))
            {
                _selectedHash = null;
                _sender.UpdateOffer(null, null);
                _rpiReadyReceiverId = null;
                _rpiReadyIp = null;
                _rpiStatusLabel.Text = "PKG изменился после выбора. Выбери файл заново и повтори проверку.";
                return;
            }

            string packageUrl = _sender.EnableRpiFileAccess(target.IpAddress, path, actualHash);
            _rpiInstallRequestInFlight = true;
            UpdateActionButtons();
            RpiInstallResult result = await _rpiClient.InstallAsync(
                target.IpAddress, packageUrl, CancellationToken.None);
            if (!result.Success || result.TaskId is null)
            {
                if (result.MayHaveBeenAccepted)
                {
                    // The console can accept the install request and then
                    // stop answering while it reads PKG metadata. Keep the
                    // source alive until the user confirms the task is done.
                    _rpiInstallActive = true;
                    _rpiProgressTimer.Start();
                    _rpiStatusLabel.Text = $"RPI: нет подтверждения номера задачи. {result.Message}";
                    _statusLabel.Text =
                        "Не отправляй тот же PKG повторно, пока не проверишь RPI на PS4: задача могла уже начаться. Источник файла оставлен открытым.";
                }
                else
                {
                    _sender.RevokeRpiFileAccess();
                    _rpiStatusLabel.Text = $"RPI: {result.Message}";
                }
                return;
            }

            _rpiInstallActive = true;
            _rpiTaskId = result.TaskId;
            _rpiTaskConsoleIp = target.IpAddress;
            _rpiProgressTimer.Start();
            _rpiStatusLabel.Text = $"RPI принял задачу №{result.TaskId}. Ожидается скачивание PKG.";
            _statusLabel.Text =
                "Задача передана RPI. Проверь её ход на PS4; AMEGA не заявляет об окончании установки. Оригинал на ПК сохранён.";
        }
        catch (Exception ex)
        {
            _sender.RevokeRpiFileAccess();
            _rpiStatusLabel.Text = $"Не удалось запустить установку: {ex.Message}";
        }
        finally
        {
            _hashProgress.Visible = false;
            _rpiInstallRequestInFlight = false;
            _rpiBusy = false;
            UpdateActionButtons();
        }
    }

    private static async Task<bool> HasPkgSignatureAsync(string path)
    {
        byte[] signature = new byte[4];
        await using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read,
            4096, FileOptions.Asynchronous | FileOptions.SequentialScan);
        int read = await stream.ReadAsync(signature.AsMemory(0, signature.Length));
        return read == 4 && signature[0] == 0x7f &&
            signature[1] == (byte)'C' && signature[2] == (byte)'N' && signature[3] == (byte)'T';
    }

    private void SendClicked(object? sender, EventArgs e)
    {
        var receiver = GetSelectedReceiver();
        if (!_isSearching || receiver is null ||
            string.IsNullOrWhiteSpace(_selectedFile) ||
            string.IsNullOrWhiteSpace(_selectedHash))
        {
            _statusLabel.Text = "Сначала выбери PKG, дождись проверки и выбери найденную PS4.";
            return;
        }

        try
        {
            _sender.UpdateOffer(_selectedFile, _selectedHash, receiver.ReceiverId);
            _offeredReceiverId = receiver.ReceiverId;
            _offerSent = true;
            _statusLabel.Text = $"Предложение отправлено для {receiver.DisplayName}. На PS4 выбери этот ПК и нажми X.";
        }
        catch (Exception ex)
        {
            _statusLabel.Text = $"Не удалось подготовить отправку: {ex.Message}";
            _offerSent = false;
            _offeredReceiverId = null;
        }
        UpdateActionButtons();
    }

    private async void StopClicked(object? sender, EventArgs e)
    {
        if (_startInProgress)
        {
            if (!_stopRequestedDuringStartup)
            {
                _stopRequestedDuringStartup = true;
                _statusLabel.Text = "Останавливаю запуск сетевого поиска...";
                UpdateActionButtons();
            }
            return;
        }

        if (_rpiInstallActive)
        {
            var choice = MessageBox.Show(this,
                "Остановка AMEGA закроет сетевой источник PKG. Если RPI ещё скачивает файл, установка прервётся. Остановить?",
                "RPI ещё может скачивать PKG",
                MessageBoxButtons.YesNo, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2);
            if (choice != DialogResult.Yes) return;
            _sender.RevokeRpiFileAccess();
            _rpiInstallActive = false;
            _rpiProgressTimer.Stop();
            _rpiStatusLabel.Text = "RPI: раздача остановлена пользователем.";
        }
        bool wasHashing = _hashCts is not null;
        if (_offerSent)
            _sender.UpdateOffer(null, null);
        _offerSent = false;
        _offeredReceiverId = null;
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
        if (_rpiInstallActive || _rpiInstallRequestInFlight)
        {
            string warning = _rpiInstallRequestInFlight
                ? "RPI ещё обрабатывает запрос установки. Закрытие AMEGA отключит источник PKG; задача могла уже начаться, но загрузка может оборваться. Всё равно закрыть?"
                : "RPI может ещё скачивать PKG. Закрытие AMEGA остановит источник файла и прервёт эту загрузку. Всё равно закрыть?";
            var choice = MessageBox.Show(this, warning,
                "Закрытие AMEGA",
                MessageBoxButtons.YesNo, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2);
            if (choice != DialogResult.Yes)
            {
                e.Cancel = true;
                return;
            }
            _sender.RevokeRpiFileAccess();
            _rpiInstallActive = false;
            _rpiProgressTimer.Stop();
        }
        _hashCts?.Cancel();
        await StopSenderAsync();
        _rpiProgressTimer.Stop();
        _rpiProgressTimer.Dispose();
        await _sender.DisposeAsync();
        _rpiClient.Dispose();
    }

    private async Task StopSenderAsync()
    {
        _deviceExpiryTimer.Stop();
        _startInProgress = false;
        _stopRequestedDuringStartup = false;
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
