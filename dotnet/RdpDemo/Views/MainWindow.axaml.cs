using Avalonia.Controls;
using RdpDemo.ViewModels;

namespace RdpDemo.Views;

public partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
        DataContext = new MainWindowViewModel();
    }
}
