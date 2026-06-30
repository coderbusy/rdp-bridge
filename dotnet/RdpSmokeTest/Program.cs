using System.Net.Sockets;
using CoderBusy.RdpBridge;

// ---------------------------------------------------------------------------
// Usage: RdpSmokeTest <host> <port> <username> <password> [domain]
// Exit codes: 0 = connected + frame received, 1 = connection failed, 2 = TCP unreachable
// ---------------------------------------------------------------------------

if (args.Length < 4)
{
    Console.Error.WriteLine("Usage: RdpSmokeTest <host> <port> <username> <password> [domain]");
    return 1;
}

var host     = args[0];
var port     = int.Parse(args[1]);
var username = args[2];
var password = args[3];
var domain   = args.Length >= 5 ? args[4] : null;

Console.WriteLine($"[smoke] target {host}:{port} user={username} domain={domain ?? "<none>"}");
Console.WriteLine($"[smoke] native lib: {RdpBridgeClient.GetExpectedNativeLibraryName()}");

// -- TCP probe ---------------------------------------------------------------
Console.Write("[smoke] TCP probe... ");
try
{
    using var tcp = new TcpClient();
    var connectTask = tcp.ConnectAsync(host, port);
    if (!connectTask.Wait(TimeSpan.FromSeconds(5)))
    {
        Console.WriteLine("TIMEOUT");
        return 2;
    }
    Console.WriteLine("OK");
}
catch (Exception ex)
{
    Console.WriteLine($"FAIL ({ex.Message})");
    return 2;
}

// -- RDP connect -------------------------------------------------------------
using var cts          = new CancellationTokenSource(TimeSpan.FromSeconds(30));
var frameReceived      = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
var stateLog           = new List<RdpState>();
var clipboardReceived  = new TaskCompletionSource<string>(TaskCreationOptions.RunContinuationsAsynchronously);

var options = new RdpConnectOptions
{
    Domain      = domain,
    ColorDepth  = 32,
    Experience  = RdpExperienceFlags.LowBandwidth,
};

var frameReceiver = new SmokeFrameReceiver(frameReceived);
using var client = new RdpBridgeClient(frameReceiver);

client.StateChanged += state =>
{
    stateLog.Add(state);
    Console.WriteLine($"[smoke] state → {state}");
    if (state == RdpState.Failed)
        frameReceived.TrySetResult(false);
};

client.StatusChanged += msg =>
    Console.WriteLine($"[smoke] status: {msg}");

client.ClipboardReceived += text =>
{
    Console.WriteLine($"[smoke] clipboard received: {text[..Math.Min(text.Length, 60)]}");
    clipboardReceived.TrySetResult(text);
};

client.Disconnected += () =>
{
    Console.WriteLine("[smoke] disconnected");
    frameReceived.TrySetResult(false);
};

Console.WriteLine("[smoke] connecting...");
try
{
    client.Connect(host, port, username, password, 1024, 768, options);
}
catch (Exception ex)
{
    Console.WriteLine($"[smoke] connect threw: {ex.Message}");
    return 1;
}

// Wait for first frame or failure
cts.Token.Register(() => frameReceived.TrySetCanceled());
bool success;
try
{
    success = await frameReceived.Task;
}
catch (OperationCanceledException)
{
    Console.WriteLine("[smoke] TIMEOUT waiting for frame");
    client.Disconnect();
    return 1;
}

if (!success)
{
    Console.WriteLine("[smoke] FAILED — no frame received");
    return 1;
}

Console.WriteLine("[smoke] frame OK — testing clipboard...");

// Test local→remote clipboard
var pushed = client.SetClipboardText("RdpBridge smoke test clipboard");
Console.WriteLine($"[smoke] clipboard push: {(pushed ? "OK" : "not available")}");

// Test resize
var resized = client.Resize(1280, 720);
Console.WriteLine($"[smoke] resize 1280x720: {(resized ? "sent" : "not available")}");

// Wait briefly for a second frame confirming the resize
await Task.Delay(2000);

client.Disconnect();
Console.WriteLine("[smoke] PASS");
return 0;

file sealed class SmokeFrameReceiver(TaskCompletionSource<bool> tcs) : IRdpFrameReceiver
{
    public void OnFrame(int width, int height, int stride, ReadOnlySpan<byte> pixels)
    {
        Console.WriteLine($"[smoke] frame {width}x{height} stride={stride} bytes={pixels.Length}");
        tcs.TrySetResult(true);
    }
}
