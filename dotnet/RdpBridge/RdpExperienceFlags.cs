namespace CoderBusy.RdpBridge;

[Flags]
public enum RdpExperienceFlags : uint
{
    None                  = 0,
    DisableWallpaper      = 0x01u,
    DisableFullWindowDrag = 0x02u,
    DisableMenuAnims      = 0x04u,
    DisableThemes         = 0x08u,
    EnableFontSmoothing   = 0x10u,

    /// <summary>Minimal bandwidth preset: disables all decorations.</summary>
    LowBandwidth = DisableWallpaper | DisableFullWindowDrag | DisableMenuAnims | DisableThemes
}
