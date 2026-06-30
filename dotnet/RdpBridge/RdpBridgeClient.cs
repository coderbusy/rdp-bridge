using System.Reflection;
using System.Runtime.InteropServices;
using System.Text;

namespace CoderBusy.RdpBridge;

public sealed class RdpBridgeClient : IDisposable
{
    private const string LibraryName = "RdpBridgeNative";
    private static readonly object DebugLogLock = new();
    private readonly FrameCallback _frameCallback;
    private readonly StatusCallback _statusCallback;
    private readonly DisconnectCallback _disconnectCallback;
    private readonly StateCallback _stateCallback;
    private readonly ClipboardCallback _clipboardCallback;
    private IntPtr _handle;

    static RdpBridgeClient()
    {
        NativeLibrary.SetDllImportResolver(typeof(RdpBridgeClient).Assembly, ResolveNativeLibrary);
    }

    public event EventHandler<RdpFramebufferEventArgs>? FramebufferUpdated;
    public event Action<string>? StatusChanged;
    public event Action? Disconnected;
    public event Action<RdpState>? StateChanged;
    public event Action<string>? ClipboardReceived;

    public static string DebugLogPath => Path.Combine(GetDebugLogDirectory(), "rdp-debug.log");

    public RdpBridgeClient()
    {
        _frameCallback      = OnFrame;
        _statusCallback     = OnStatus;
        _disconnectCallback = OnDisconnected;
        _stateCallback      = OnState;
        _clipboardCallback  = OnClipboard;
    }

    public void Connect(string host, int port, string username, string password, int width, int height)
    {
        Connect(host, port, username, password, width, height, options: null);
    }

    public void Connect(string host, int port, string username, string password, int width, int height,
        RdpConnectOptions? options)
    {
        Disconnect();

        DebugLog($"connect start host={SanitizeLogValue(host)} port={port} user={SanitizeLogValue(username)} size={width}x{height}");
        _handle = NativeMethods.RdpBridge_create();
        if (_handle == IntPtr.Zero)
        {
            DebugLog("connect failed: native create returned null");
            throw new InvalidOperationException("Failed to create RDP bridge session.");
        }

        NativeMethods.RdpBridge_set_callbacks(_handle, _frameCallback, _statusCallback, _disconnectCallback, IntPtr.Zero);
        NativeMethods.RdpBridge_set_state_callback(_handle, _stateCallback, IntPtr.Zero);
        NativeMethods.RdpBridge_set_clipboard_callback(_handle, _clipboardCallback, IntPtr.Zero);

        if (options != null)
        {
            NativeMethods.RdpBridge_set_options(_handle, options.Domain, options.ColorDepth, (uint)options.Experience);
            foreach (var drive in options.Drives)
                NativeMethods.RdpBridge_add_drive(_handle, drive.Name, drive.LocalPath);
        }

        var result = NativeMethods.RdpBridge_connect(_handle, host, port, username, password, width, height);
        if (result != 0)
        {
            var error = GetLastError();
            DebugLog($"connect failed result={result} error={SanitizeLogValue(error)}");
            Disconnect();
            throw new InvalidOperationException(string.IsNullOrWhiteSpace(error)
                ? $"RDP bridge connect failed: {result}"
                : error);
        }

        DebugLog("connect worker started");
    }

    public void SendPointer(ushort flags, ushort x, ushort y)
    {
        if (_handle != IntPtr.Zero)
            NativeMethods.RdpBridge_send_pointer(_handle, flags, x, y);
    }

    public void SendKey(uint key, bool down)
    {
        if (_handle != IntPtr.Zero)
            NativeMethods.RdpBridge_send_key(_handle, key, down ? 1 : 0);
    }

    public void SendUnicodeKey(char key, bool down)
    {
        if (_handle != IntPtr.Zero)
            NativeMethods.RdpBridge_send_unicode_key(_handle, key, down ? 1 : 0);
    }

    /// <summary>
    /// Push text to the remote clipboard. Requires clipboard callback to have been registered before Connect.
    /// </summary>
    public bool SetClipboardText(string text)
    {
        if (_handle == IntPtr.Zero) return false;
        return NativeMethods.RdpBridge_clipboard_set_text(_handle, text) == 0;
    }

    /// <summary>
    /// Request a dynamic desktop resize. Safe to call before or after Connect.
    /// </summary>
    public bool Resize(int width, int height)
    {
        if (_handle == IntPtr.Zero) return false;
        return NativeMethods.RdpBridge_resize(_handle, width, height) == 0;
    }

    public void Disconnect()
    {
        if (_handle == IntPtr.Zero)
            return;

        DebugLog("disconnect requested");
        NativeMethods.RdpBridge_disconnect(_handle);
        NativeMethods.RdpBridge_destroy(_handle);
        _handle = IntPtr.Zero;
        DebugLog("disconnect completed");
    }

    public void Dispose()
    {
        Disconnect();
    }

    private string GetLastError()
    {
        if (_handle == IntPtr.Zero)
            return string.Empty;

        var pointer = NativeMethods.RdpBridge_get_last_error(_handle);
        return pointer == IntPtr.Zero ? string.Empty : Marshal.PtrToStringUTF8(pointer) ?? string.Empty;
    }

    public static string GetExpectedNativeLibraryName()
    {
        if (OperatingSystem.IsWindows())
            return "RdpBridgeNative.dll";
        if (OperatingSystem.IsMacOS())
            return "libRdpBridgeNative.dylib";
        return "libRdpBridgeNative.so";
    }

    private static IntPtr ResolveNativeLibrary(string libraryName, Assembly assembly, DllImportSearchPath? searchPath)
    {
        if (!string.Equals(libraryName, LibraryName, StringComparison.Ordinal))
            return IntPtr.Zero;

        foreach (var candidate in GetNativeLibraryCandidates())
        {
            if (File.Exists(candidate) && NativeLibrary.TryLoad(candidate, out var handle))
                return handle;
        }

        return IntPtr.Zero;
    }

    private static string[] GetNativeLibraryCandidates()
    {
        var baseDirectory = AppContext.BaseDirectory;
        var fileName = GetExpectedNativeLibraryName();
        var rid = GetCurrentRid();

        // Prefer runtimes/<rid>/native/ before the base directory so the
        // managed RdpBridge.dll (project reference output) is not mistaken
        // for the native library.
        var candidates = new List<string>
        {
            Path.Combine(baseDirectory, "runtimes", rid, "native", fileName),
            Path.Combine(baseDirectory, fileName),
        };

        var processDirectory = Path.GetDirectoryName(Environment.ProcessPath);
        if (!string.IsNullOrWhiteSpace(processDirectory) &&
            !string.Equals(processDirectory, baseDirectory, StringComparison.OrdinalIgnoreCase))
        {
            candidates.Add(Path.Combine(processDirectory, "runtimes", rid, "native", fileName));
            candidates.Add(Path.Combine(processDirectory, fileName));
        }

        if (AppContext.GetData("NATIVE_DLL_SEARCH_DIRECTORIES") is string nativeSearchDirectories)
        {
            foreach (var directory in nativeSearchDirectories.Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
            {
                candidates.Add(Path.Combine(directory, "runtimes", rid, "native", fileName));
                candidates.Add(Path.Combine(directory, fileName));
            }
        }

        return candidates.Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
    }

    private static string GetCurrentRid()
    {
        var arch = RuntimeInformation.ProcessArchitecture switch
        {
            Architecture.Arm64 => "arm64",
            Architecture.X64 => "x64",
            Architecture.X86 => "x86",
            Architecture.Arm => "arm",
            _ => RuntimeInformation.ProcessArchitecture.ToString().ToLowerInvariant()
        };

        if (OperatingSystem.IsWindows()) return $"win-{arch}";
        if (OperatingSystem.IsMacOS()) return $"osx-{arch}";
        return $"linux-{arch}";
    }

    private void OnFrame(IntPtr userData, int width, int height, int stride, IntPtr data)
    {
        if (width <= 0 || height <= 0 || stride <= 0 || data == IntPtr.Zero)
            return;

        var length = checked(stride * height);
        var pixels = new byte[length];
        Marshal.Copy(data, pixels, 0, length);
        FramebufferUpdated?.Invoke(this, new RdpFramebufferEventArgs(width, height, stride, pixels));
    }

    private void OnStatus(IntPtr userData, IntPtr message)
    {
        var text = message == IntPtr.Zero ? string.Empty : Marshal.PtrToStringUTF8(message) ?? string.Empty;
        if (!string.IsNullOrWhiteSpace(text))
        {
            DebugLog($"status {SanitizeLogValue(text)}");
            StatusChanged?.Invoke(text);
        }
    }

    private void OnDisconnected(IntPtr userData)
    {
        DebugLog("disconnected callback");
        Disconnected?.Invoke();
    }

    private void OnState(IntPtr userData, RdpState state)
    {
        DebugLog($"state {state}");
        StateChanged?.Invoke(state);
    }

    private void OnClipboard(IntPtr userData, IntPtr text)
    {
        var value = text == IntPtr.Zero ? string.Empty : Marshal.PtrToStringUTF8(text) ?? string.Empty;
        if (!string.IsNullOrEmpty(value))
            ClipboardReceived?.Invoke(value);
    }

    private static string GetDebugLogDirectory()
    {
        var root = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        if (string.IsNullOrWhiteSpace(root))
            root = AppContext.BaseDirectory;
        return Path.Combine(root, "RdpBridge", "Logs");
    }

    private static void DebugLog(string message)
    {
        try
        {
            Directory.CreateDirectory(GetDebugLogDirectory());
            var line = $"{DateTimeOffset.Now:O} {message}{Environment.NewLine}";
            lock (DebugLogLock)
                File.AppendAllText(DebugLogPath, line, Encoding.UTF8);
        }
        catch
        {
            // Debug logging must not break the RDP session.
        }
    }

    private static string SanitizeLogValue(string? value)
    {
        if (string.IsNullOrWhiteSpace(value))
            return "<empty>";
        return value.Replace("\r", "\\r", StringComparison.Ordinal)
                    .Replace("\n", "\\n", StringComparison.Ordinal);
    }

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void FrameCallback(IntPtr userData, int width, int height, int stride, IntPtr bgraPixels);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void StatusCallback(IntPtr userData, IntPtr message);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void DisconnectCallback(IntPtr userData);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void StateCallback(IntPtr userData, RdpState state);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void ClipboardCallback(IntPtr userData, IntPtr utf8Text);

    private static class NativeMethods
    {
        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr RdpBridge_create();

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void RdpBridge_destroy(IntPtr handle);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void RdpBridge_set_callbacks(
            IntPtr handle,
            FrameCallback frameCallback,
            StatusCallback statusCallback,
            DisconnectCallback disconnectCallback,
            IntPtr userData);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int RdpBridge_connect(
            IntPtr handle,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string host,
            int port,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string username,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string password,
            int width,
            int height);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void RdpBridge_disconnect(IntPtr handle);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void RdpBridge_send_pointer(IntPtr handle, ushort flags, ushort x, ushort y);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void RdpBridge_send_key(IntPtr handle, uint key, int down);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void RdpBridge_send_unicode_key(IntPtr handle, ushort code, int down);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern IntPtr RdpBridge_get_last_error(IntPtr handle);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void RdpBridge_set_options(
            IntPtr handle,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string? domain,
            int colorDepth,
            uint experience);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void RdpBridge_add_drive(
            IntPtr handle,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string name,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string localPath);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void RdpBridge_set_state_callback(IntPtr handle, StateCallback stateCallback, IntPtr userData);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern void RdpBridge_set_clipboard_callback(IntPtr handle, ClipboardCallback clipboardCallback, IntPtr userData);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int RdpBridge_clipboard_set_text(
            IntPtr handle,
            [MarshalAs(UnmanagedType.LPUTF8Str)] string utf8Text);

        [DllImport(LibraryName, CallingConvention = CallingConvention.Cdecl)]
        public static extern int RdpBridge_resize(IntPtr handle, int width, int height);
    }
}
