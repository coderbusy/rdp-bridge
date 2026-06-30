using System;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Interactivity;
using Avalonia.Media;
using RdpDemo.ViewModels;

namespace RdpDemo.Views;

public partial class RdpView : UserControl
{
    private ushort _buttonFlags;
    private bool _syntheticShiftDown;

    public RdpView()
    {
        InitializeComponent();
        AddHandler(KeyDownEvent, OnKeyDown, RoutingStrategies.Tunnel);
        AddHandler(KeyUpEvent, OnKeyUp, RoutingStrategies.Tunnel);
        AttachedToVisualTree += (_, _) => Focus();
    }

    // ── Pointer ──────────────────────────────────────────────────────────────

    private void OnPointerMoved(object? sender, PointerEventArgs e)
        => SendPointer(e, 0x0800);

    private void OnPointerPressed(object? sender, PointerPressedEventArgs e)
    {
        Focus();
        var point = e.GetCurrentPoint(FramebufferImage);
        if (point.Properties.IsLeftButtonPressed)   _buttonFlags |= 0x1000;
        if (point.Properties.IsMiddleButtonPressed) _buttonFlags |= 0x4000;
        if (point.Properties.IsRightButtonPressed)  _buttonFlags |= 0x2000;
        SendPointer(e, (ushort)(0x8000 | _buttonFlags));
        e.Handled = true;
    }

    private void OnPointerReleased(object? sender, PointerReleasedEventArgs e)
    {
        var flag = e.InitialPressMouseButton switch
        {
            MouseButton.Left   => (ushort)0x1000,
            MouseButton.Right  => (ushort)0x2000,
            MouseButton.Middle => (ushort)0x4000,
            _                  => (ushort)0
        };
        if (flag != 0)
        {
            SendPointer(e, flag);
            _buttonFlags &= (ushort)~flag;
        }
        e.Handled = true;
    }

    private void OnPointerWheelChanged(object? sender, PointerWheelEventArgs e)
    {
        var flag = e.Delta.Y > 0 ? (ushort)0x0200 : (ushort)(0x0200 | 0x0100);
        SendPointer(e, flag);
        e.Handled = true;
    }

    private void SendPointer(PointerEventArgs e, ushort flags)
    {
        if (DataContext is not MainWindowViewModel vm || FramebufferImage is null) return;
        var (x, y) = MapPointer(
            e.GetPosition(FramebufferImage),
            FramebufferImage.Bounds.Size,
            vm.RemoteWidth, vm.RemoteHeight, vm.IsFitToWindow);
        vm.SendPointer(flags, (ushort)x, (ushort)y);
    }

    private static (int X, int Y) MapPointer(Point pt, Size imageSize, int rw, int rh, bool fit)
    {
        if (rw <= 0 || rh <= 0 || imageSize.Width <= 0 || imageSize.Height <= 0) return (0, 0);
        var scale = fit ? Math.Min(imageSize.Width / rw, imageSize.Height / rh) : 1.0;
        var ox = (imageSize.Width  - rw * scale) / 2;
        var oy = (imageSize.Height - rh * scale) / 2;
        var x = (int)Math.Round((pt.X - ox) / scale);
        var y = (int)Math.Round((pt.Y - oy) / scale);
        return (Math.Clamp(x, 0, rw - 1), Math.Clamp(y, 0, rh - 1));
    }

    // ── Keyboard ─────────────────────────────────────────────────────────────

    private void OnKeyDown(object? sender, KeyEventArgs e) => SendKey(e, true);
    private void OnKeyUp(object? sender, KeyEventArgs e)   => SendKey(e, false);

    private void SendKey(KeyEventArgs e, bool down)
    {
        if (DataContext is not MainWindowViewModel vm) return;

        if (TryGetPrintableScancode(e, out var scancode, out var needsShift))
        {
            if (!needsShift && down && _syntheticShiftDown)
            {
                vm.SendKey(0x2A, false);
                _syntheticShiftDown = false;
            }
            if (needsShift && down && !_syntheticShiftDown)
            {
                vm.SendKey(0x2A, true);
                _syntheticShiftDown = true;
            }

            vm.SendKey(scancode, down);

            if (!down && _syntheticShiftDown)
            {
                vm.SendKey(0x2A, false);
                _syntheticShiftDown = false;
            }
            e.Handled = true;
            return;
        }

        var sc = ToRdpScancode(e);
        if (sc == 0) return;
        vm.SendKey(sc, down);
        e.Handled = true;
    }

    private static bool TryGetPrintableScancode(KeyEventArgs e, out uint scancode, out bool needsShift)
    {
        scancode   = 0;
        needsShift = e.KeyModifiers.HasFlag(KeyModifiers.Shift);

        if (e.Key >= Key.A && e.Key <= Key.Z)
        {
            scancode = e.Key switch
            {
                Key.A => 0x1E, Key.B => 0x30, Key.C => 0x2E, Key.D => 0x20, Key.E => 0x12,
                Key.F => 0x21, Key.G => 0x22, Key.H => 0x23, Key.I => 0x17, Key.J => 0x24,
                Key.K => 0x25, Key.L => 0x26, Key.M => 0x32, Key.N => 0x31, Key.O => 0x18,
                Key.P => 0x19, Key.Q => 0x10, Key.R => 0x13, Key.S => 0x1F, Key.T => 0x14,
                Key.U => 0x16, Key.V => 0x2F, Key.W => 0x11, Key.X => 0x2D, Key.Y => 0x15,
                Key.Z => 0x2C, _ => 0
            };
            return scancode != 0;
        }

        if (e.Key >= Key.D0 && e.Key <= Key.D9)
        {
            var idx = e.Key - Key.D0;
            scancode = idx == 0 ? 0x0Bu : 0x01u + (uint)idx;
            return true;
        }

        if (e.Key >= Key.NumPad0 && e.Key <= Key.NumPad9)
        {
            scancode   = 0x52u + (uint)(e.Key - Key.NumPad0);
            needsShift = false;
            return true;
        }

        scancode = e.Key switch
        {
            Key.Space           => 0x39,
            Key.OemMinus        => 0x0C,
            Key.OemPlus         => 0x0D,
            Key.OemOpenBrackets => 0x1A,
            Key.OemCloseBrackets => 0x1B,
            Key.OemPipe         => 0x2B,
            Key.OemSemicolon    => 0x27,
            Key.OemQuotes       => 0x28,
            Key.OemComma        => 0x33,
            Key.OemPeriod       => 0x34,
            Key.OemQuestion     => 0x35,
            Key.OemTilde        => 0x29,
            Key.Decimal         => 0x53,
            Key.Add             => 0x4E,
            Key.Subtract        => 0x4A,
            Key.Multiply        => 0x37,
            Key.Divide          => 0x0100 | 0x35,
            _ => 0
        };
        return scancode != 0;
    }

    private static uint ToRdpScancode(KeyEventArgs e) => e.Key switch
    {
        Key.Escape    => 0x01,
        Key.Back      => 0x0E,
        Key.Tab       => 0x0F,
        Key.Enter     => 0x1C,
        Key.LeftShift  or Key.RightShift  => 0x2A,
        Key.LeftCtrl   or Key.RightCtrl   => 0x1D,
        Key.LeftAlt    or Key.RightAlt    => 0x38,
        Key.CapsLock  => 0x3A,
        Key.F1  => 0x3B, Key.F2  => 0x3C, Key.F3  => 0x3D, Key.F4  => 0x3E,
        Key.F5  => 0x3F, Key.F6  => 0x40, Key.F7  => 0x41, Key.F8  => 0x42,
        Key.F9  => 0x43, Key.F10 => 0x44, Key.F11 => 0x57, Key.F12 => 0x58,
        Key.Home     => 0x0100 | 0x47,
        Key.Up       => 0x0100 | 0x48,
        Key.PageUp   => 0x0100 | 0x49,
        Key.Left     => 0x0100 | 0x4B,
        Key.Right    => 0x0100 | 0x4D,
        Key.End      => 0x0100 | 0x4F,
        Key.Down     => 0x0100 | 0x50,
        Key.PageDown => 0x0100 | 0x51,
        Key.Insert   => 0x0100 | 0x52,
        Key.Delete   => 0x0100 | 0x53,
        _ => 0
    };
}
