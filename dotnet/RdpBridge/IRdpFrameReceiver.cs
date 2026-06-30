namespace LuYao.RdpBridge;

public interface IRdpFrameReceiver
{
    void OnFrame(int width, int height, int stride, ReadOnlySpan<byte> pixels);
}
