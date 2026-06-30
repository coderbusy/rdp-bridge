using System;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using Avalonia;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using Avalonia.Threading;
using CommunityToolkit.Mvvm.ComponentModel;
using CommunityToolkit.Mvvm.Input;
using LuYao.RdpBridge;

namespace RdpDemo.ViewModels;

public partial class MainWindowViewModel : ObservableObject, IRdpFrameReceiver, IDisposable
{
    private RdpBridgeClient? _client;
    private bool _started;

    // Injected by the View after AttachedToVisualTree; used for clipboard access.
    private Func<Task<string?>>? _getLocalClipboard;
    private Func<string, Task>?  _setLocalClipboard;

    // Last text pushed to / received from local clipboard — used to break feedback loops.
    private string? _lastLocalClipboard;
    private string? _lastRemoteClipboard;

    private DispatcherTimer? _clipboardPollTimer;

    [ObservableProperty] private string _host = "192.168.1.1";
    [ObservableProperty] private string _port = "3389";
    [ObservableProperty] private string _username = string.Empty;
    [ObservableProperty] private string _password = string.Empty;
    [ObservableProperty] private string _desktopWidth = "1280";
    [ObservableProperty] private string _desktopHeight = "800";

    [ObservableProperty] private WriteableBitmap? _framebuffer;
    [ObservableProperty] private string _statusText = "Not connected";
    [ObservableProperty] private bool _isConnected;
    [ObservableProperty] private bool _isFitToWindow = true;
    [ObservableProperty] private int _remoteWidth;
    [ObservableProperty] private int _remoteHeight;

    public string ScaleModeText => IsFitToWindow ? "Fit" : "1:1";

    partial void OnIsFitToWindowChanged(bool value) => OnPropertyChanged(nameof(ScaleModeText));

    /// <summary>
    /// Called by the View once it has a TopLevel reference.
    /// </summary>
    public void SetClipboardAccessors(Func<Task<string?>> get, Func<string, Task> set)
    {
        _getLocalClipboard = get;
        _setLocalClipboard = set;
    }

    [RelayCommand]
    private void Connect()
    {
        if (_started) return;

        if (!int.TryParse(Port, out var port) || port < 1 || port > 65535) port = 3389;
        if (!int.TryParse(DesktopWidth, out var width) || width < 1) width = 1280;
        if (!int.TryParse(DesktopHeight, out var height) || height < 1) height = 800;

        _started = true;
        StatusText = "Connecting...";

        _client?.Dispose();
        _client = new RdpBridgeClient(this);
        _client.StatusChanged  += msg => Dispatcher.UIThread.Post(() => StatusText = msg);
        _client.StateChanged   += state => Dispatcher.UIThread.Post(() =>
        {
            IsConnected = state == RdpState.Connected;
            if (state == RdpState.Connected)
            {
                StartClipboardSync();
            }
            else if (state is RdpState.Disconnected or RdpState.Failed)
            {
                _started = false;
                StopClipboardSync();
                StatusText = state == RdpState.Failed ? "Connection failed" : "Disconnected";
            }
        });
        _client.Disconnected   += () => Dispatcher.UIThread.Post(() =>
        {
            _started    = false;
            IsConnected = false;
            StopClipboardSync();
            StatusText  = "Disconnected";
        });
        _client.ClipboardReceived += text => Dispatcher.UIThread.Post(() => OnRemoteClipboardReceived(text));

        _ = Task.Run(() =>
        {
            try
            {
                _client.Connect(Host, port, Username, Password, width, height);
            }
            catch (DllNotFoundException)
            {
                Dispatcher.UIThread.Post(() =>
                {
                    _started   = false;
                    StatusText = $"{RdpBridgeClient.GetExpectedNativeLibraryName()} not found — build native library first.";
                });
            }
            catch (Exception ex)
            {
                Dispatcher.UIThread.Post(() =>
                {
                    _started   = false;
                    StatusText = $"Error: {ex.Message}";
                });
            }
        });
    }

    [RelayCommand]
    private void Disconnect()
    {
        _client?.Disconnect();
        _started    = false;
        IsConnected = false;
        StopClipboardSync();
        StatusText  = "Disconnected";
    }

    [RelayCommand]
    private void ToggleScaleMode() => IsFitToWindow = !IsFitToWindow;

    public void SendPointer(ushort flags, ushort x, ushort y)
    {
        if (IsConnected) _client?.SendPointer(flags, x, y);
    }

    public void SendKey(uint scancode, bool down)
    {
        if (IsConnected) _client?.SendKey(scancode, down);
    }

    // ── Clipboard sync ────────────────────────────────────────────────────────

    private void StartClipboardSync()
    {
        if (_clipboardPollTimer != null) return;
        _lastLocalClipboard  = null;
        _lastRemoteClipboard = null;

        _clipboardPollTimer = new DispatcherTimer(TimeSpan.FromMilliseconds(500),
            DispatcherPriority.Background, OnClipboardPollTick);
        _clipboardPollTimer.Start();
    }

    private void StopClipboardSync()
    {
        _clipboardPollTimer?.Stop();
        _clipboardPollTimer = null;
    }

    private async void OnClipboardPollTick(object? sender, EventArgs e)
    {
        if (_getLocalClipboard == null || _client == null || !IsConnected) return;

        try
        {
            var text = await _getLocalClipboard();
            if (text == null || text == _lastLocalClipboard) return;
            _lastLocalClipboard = text;

            // Don't echo back text that arrived from the remote side.
            if (text == _lastRemoteClipboard) return;

            _client.SetClipboardText(text);
        }
        catch
        {
            // Clipboard access can throw; never crash the session.
        }
    }

    private async void OnRemoteClipboardReceived(string text)
    {
        if (string.IsNullOrEmpty(text) || _setLocalClipboard == null) return;
        if (text == _lastRemoteClipboard) return;

        _lastRemoteClipboard = text;
        _lastLocalClipboard  = text; // suppress the next poll round-trip

        try
        {
            await _setLocalClipboard(text);
        }
        catch
        {
            // Clipboard access can throw; never crash the session.
        }
    }

    // ── Frame delivery ────────────────────────────────────────────────────────

    unsafe void IRdpFrameReceiver.OnFrame(int width, int height, int stride, ReadOnlySpan<byte> pixels)
    {
        var copy = new byte[pixels.Length];
        pixels.CopyTo(copy);

        Dispatcher.UIThread.Post(() =>
        {
            var bmp = new WriteableBitmap(
                new PixelSize(width, height),
                new Vector(96, 96),
                PixelFormat.Bgra8888,
                AlphaFormat.Opaque);

            using var locked = bmp.Lock();
            var dst = new Span<byte>((void*)locked.Address, locked.RowBytes * locked.Size.Height);
            var src = copy.AsSpan(0, Math.Min(copy.Length, dst.Length));
            src.CopyTo(dst);

            Framebuffer  = bmp;
            RemoteWidth  = width;
            RemoteHeight = height;
        });
    }

    public void Dispose()
    {
        StopClipboardSync();
        _client?.Dispose();
        _client = null;
    }
}

