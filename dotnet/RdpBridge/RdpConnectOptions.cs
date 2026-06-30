namespace CoderBusy.RdpBridge;

/// <summary>
/// Optional parameters passed to <see cref="RdpBridgeClient.Connect(string,int,string,string,int,int,RdpConnectOptions?)"/>.
/// </summary>
public sealed class RdpConnectOptions
{
    /// <summary>Windows domain. Null or empty = no domain.</summary>
    public string? Domain { get; set; }

    /// <summary>Color depth: 16, 24, or 32 (default).</summary>
    public int ColorDepth { get; set; } = 32;

    /// <summary>Bitmask of experience flags.</summary>
    public RdpExperienceFlags Experience { get; set; }

    /// <summary>Local drives to redirect to the remote session.</summary>
    public List<RdpDriveEntry> Drives { get; } = [];
}

/// <summary>A local directory exposed as a network drive on the remote desktop.</summary>
public sealed class RdpDriveEntry
{
    public RdpDriveEntry(string name, string localPath)
    {
        Name = name;
        LocalPath = localPath;
    }

    /// <summary>Share name visible on the remote (e.g. "LocalDisk").</summary>
    public string Name { get; set; }

    /// <summary>Local filesystem path.</summary>
    public string LocalPath { get; set; }
}
