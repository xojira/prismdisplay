using System;
using System.Windows;

namespace PrismHost;

public partial class MainWindow : Window
{
    private readonly PrismHostService _host = new();
    private bool _isStreaming;

    public MainWindow()
    {
        InitializeComponent();
        TxtPairingPin.Text = _host.PairingPin;
        TxtConnStatus.Text = "Ready";

        _host.OnLog += message =>
            Dispatcher.Invoke(() =>
            {
                LstLogs.Items.Add($"{DateTime.Now:HH:mm:ss}  {message}");
                LstLogs.ScrollIntoView(LstLogs.Items[^1]);
            });
        _host.OnConnectionChanged += (connected, description) =>
            Dispatcher.Invoke(() =>
            {
                TxtConnStatus.Text = connected ? "Connected" : "Disconnected";
                TxtTransportType.Text = description;
            });
        _host.OnTelemetryUpdated += (fps, mbps) =>
            Dispatcher.Invoke(() =>
            {
                TxtFps.Text = $"Frame Rate: {fps:F1} FPS";
                TxtBitrate.Text = $"Throughput: {mbps:F1} Mbps";
            });
    }

    private void BtnToggleStream_Click(object sender, RoutedEventArgs e)
    {
        if (_isStreaming)
        {
            _host.Stop();
            _isStreaming = false;
            BtnToggleStream.Content = "Start Streaming";
            TxtConnStatus.Text = "Stopped";
            CmbDisplay.IsEnabled = true;
            CmbResolution.IsEnabled = true;
            CmbPreset.IsEnabled = true;
            ChkTls.IsEnabled = true;
            return;
        }

        uint width = CmbResolution.SelectedIndex == 1 ? 2560u : 1920u;
        uint height = CmbResolution.SelectedIndex == 1 ? 1600u : 1080u;
        QualityPreset preset = CmbPreset.SelectedIndex switch
        {
            0 => QualityPreset.Performance,
            2 => QualityPreset.Quality,
            _ => QualityPreset.Balanced
        };

        _host.Start(CmbDisplay.SelectedIndex, width, height, preset, ChkTls.IsChecked == true);
        _isStreaming = true;
        BtnToggleStream.Content = "Stop Streaming";
        CmbDisplay.IsEnabled = false;
        CmbResolution.IsEnabled = false;
        CmbPreset.IsEnabled = false;
        ChkTls.IsEnabled = false;
    }

    protected override void OnClosed(EventArgs args)
    {
        _host.Dispose();
        base.OnClosed(args);
    }
}
