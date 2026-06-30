namespace CoderBusy.RdpBridge;

public sealed class RdpFramebufferEventArgs : EventArgs
{
    public RdpFramebufferEventArgs(int width, int height, int stride, byte[] pixels)
    {
        Width = width;
        Height = height;
        Stride = stride;
        Pixels = pixels;
    }

    public int Width { get; }
    public int Height { get; }
    public int Stride { get; }
    public byte[] Pixels { get; }
}
