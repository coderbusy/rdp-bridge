using System.Threading.Tasks;
using Avalonia.Controls;
using Avalonia.Input.Platform;
using RdpDemo.ViewModels;

namespace RdpDemo.Views;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
        var vm = new MainWindowViewModel();
        DataContext = vm;

        Opened += (_, _) =>
        {
            var clipboard = Clipboard;
            if (clipboard == null) return;

            vm.SetClipboardAccessors(
                text => clipboard.SetTextAsync(text));
        };
    }
}


