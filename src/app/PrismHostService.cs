using System.Buffers.Binary;
using System.Diagnostics;
using System.IO;
using System.Net;
using System.Net.Security;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Authentication;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;

namespace PrismHost;

public enum QualityPreset { Performance, Balanced, Quality }

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct PointerEventPacket
{
    public byte ToolType;
    public byte Action;
    public ushort Buttons;
    public float NormX;
    public float NormY;
    public float Pressure;
    public short TiltX;
    public short TiltY;
    public uint PointerId;
}

public sealed class PrismHostService : IDisposable
{
    private const int Port = 38400;
    private const ushort Magic = 0x5052;

    [UnmanagedFunctionPointer(CallingConvention.StdCall)]
    private delegate void OnEncodedFrameDelegate(IntPtr data, int length, long timestampUs,
        [MarshalAs(UnmanagedType.I1)] bool isKeyFrame);

    [DllImport("PrismCore.dll")]
    private static extern IntPtr Prism_Create();
    [DllImport("PrismCore.dll")]
    private static extern bool Prism_Init(IntPtr e, int mon, uint w, uint h, uint fps, uint bps);
    [DllImport("PrismCore.dll")]
    private static extern void Prism_Tick(IntPtr e, OnEncodedFrameDelegate cb);
    [DllImport("PrismCore.dll")]
    private static extern void Prism_Inject(IntPtr e, PointerEventPacket pkt);
    [DllImport("PrismCore.dll")]
    private static extern void Prism_ForceIDR(IntPtr e);
    [DllImport("PrismCore.dll")]
    private static extern void Prism_Destroy(IntPtr e);

    private IntPtr _engine;
    private TcpListener? _listener;
    private Stream? _activeClientStream;
    private readonly SemaphoreSlim _writeLock = new(1, 1);
    private CancellationTokenSource? _cts;
    private readonly X509Certificate2 _serverCert;
    private OnEncodedFrameDelegate? _frameCallback;

    public string PairingPin { get; } = RandomNumberGenerator.GetInt32(100000, 999999).ToString();
    public event Action<string>? OnLog;
    public event Action<bool, string>? OnConnectionChanged;
    public event Action<double, double>? OnTelemetryUpdated;

    public PrismHostService()
    {
        _serverCert = GenerateSelfSignedCert();
    }

    private static X509Certificate2 GenerateSelfSignedCert()
    {
        using var rsa = RSA.Create(2048);
        var request = new CertificateRequest("CN=PrismHost", rsa,
            HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
        var cert = request.CreateSelfSigned(
            DateTimeOffset.Now.AddDays(-1), DateTimeOffset.Now.AddYears(5));
        return new X509Certificate2(cert.Export(X509ContentType.Pfx));
    }

    public void Start(int monitorIndex, uint width, uint height,
        QualityPreset preset, bool enableTlsWifi)
    {
        _cts = new CancellationTokenSource();
        var (fps, bitrate) = preset switch
        {
            QualityPreset.Performance => (120u, 18_000_000u),
            QualityPreset.Balanced => (60u, 25_000_000u),
            QualityPreset.Quality => (60u, 45_000_000u),
            _ => (60u, 25_000_000u)
        };

        _engine = Prism_Create();
        if (!Prism_Init(_engine, monitorIndex, width, height, fps, bitrate))
        {
            OnLog?.Invoke("ERROR: Failed to initialize DXGI Duplication or Hardware H.264 Encoder.");
            return;
        }

        _listener = new TcpListener(IPAddress.Any, Port);
        _listener.Start();
        OnLog?.Invoke($"Host listening on port {Port} ({width}x{height} @ {fps}FPS). PIN: {PairingPin}");

        _ = Task.Run(() => UsbAdbWatchdogLoop(_cts.Token));
        _ = Task.Run(() => UdpDiscoveryBeaconLoop(_cts.Token));
        _ = Task.Run(() => AcceptClientsLoop(enableTlsWifi, _cts.Token));
        _ = Task.Run(() => CaptureLoop(_cts.Token));
    }

    private async Task UsbAdbWatchdogLoop(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested)
        {
            try
            {
                var psi = new ProcessStartInfo("adb", $"reverse tcp:{Port} tcp:{Port}")
                {
                    CreateNoWindow = true,
                    UseShellExecute = false,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true
                };
                using var process = Process.Start(psi);
                if (process != null)
                    await process.WaitForExitAsync(ct);
            }
            catch (Exception ex) when (ex is InvalidOperationException
                or System.ComponentModel.Win32Exception
                or OperationCanceledException)
            {
            }

            try { await Task.Delay(2500, ct); }
            catch (OperationCanceledException) { break; }
        }
    }

    private async Task UdpDiscoveryBeaconLoop(CancellationToken ct)
    {
        using var udp = new UdpClient { EnableBroadcast = true };
        var endpoint = new IPEndPoint(IPAddress.Broadcast, 38401);
        byte[] payload = Encoding.UTF8.GetBytes(
            $"PRISM_HOST|{Environment.MachineName}|{Port}");

        while (!ct.IsCancellationRequested)
        {
            try { await udp.SendAsync(payload, payload.Length, endpoint); }
            catch { }

            try { await Task.Delay(1500, ct); }
            catch (OperationCanceledException) { break; }
        }
    }

    private async Task AcceptClientsLoop(bool useTls, CancellationToken ct)
    {
        while (!ct.IsCancellationRequested && _listener != null)
        {
            try
            {
                TcpClient tcpClient = await _listener.AcceptTcpClientAsync(ct);
                tcpClient.NoDelay = true;
                tcpClient.SendBufferSize = 1024 * 1024;
                _ = Task.Run(() => HandleClientSession(tcpClient, useTls, ct), ct);
            }
            catch (OperationCanceledException) { break; }
            catch (Exception ex)
            {
                OnLog?.Invoke($"Accept loop error: {ex.Message}");
            }
        }
    }

    private async Task HandleClientSession(TcpClient client, bool useTls, CancellationToken ct)
    {
        string remoteEp = client.Client.RemoteEndPoint?.ToString() ?? "Unknown";
        var remoteAddress = ((IPEndPoint)client.Client.RemoteEndPoint!).Address;
        bool isUsbLoopback = IPAddress.IsLoopback(remoteAddress);
        Stream stream = client.GetStream();

        try
        {
            if (useTls && !isUsbLoopback)
            {
                var ssl = new SslStream(stream, false);
                await ssl.AuthenticateAsServerAsync(
                    new SslServerAuthenticationOptions
                    {
                        ServerCertificate = _serverCert,
                        EnabledSslProtocols = SslProtocols.Tls12 | SslProtocols.Tls13
                    }, ct);
                stream = ssl;
            }

            byte[] header = new byte[16];
            await stream.ReadExactlyAsync(header, ct);
            if (BinaryPrimitives.ReadUInt16LittleEndian(header) != Magic || header[3] != 0x01)
                throw new InvalidOperationException("Invalid handshake");

            int len = BinaryPrimitives.ReadInt32LittleEndian(header.AsSpan(4));
            if (len < 0 || len > 1024)
                throw new InvalidOperationException("Invalid authentication payload");

            byte[] authPayload = new byte[len];
            await stream.ReadExactlyAsync(authPayload, ct);
            string clientPin = Encoding.UTF8.GetString(authPayload);

            if (!isUsbLoopback && clientPin != PairingPin)
            {
                OnLog?.Invoke($"Rejected unauthorized Wi-Fi client {remoteEp} (Bad PIN)");
                client.Close();
                return;
            }

            _activeClientStream = stream;
            Prism_ForceIDR(_engine);
            OnConnectionChanged?.Invoke(true,
                isUsbLoopback ? "USB 3.0 (ADB Tunnel)" : $"Wi-Fi ({remoteEp})");
            OnLog?.Invoke($"Client authenticated via {(isUsbLoopback ? "USB" : "Wi-Fi TLS")}");

            while (!ct.IsCancellationRequested && client.Connected)
            {
                await stream.ReadExactlyAsync(header, ct);
                byte packetType = header[3];
                int payloadLength = BinaryPrimitives.ReadInt32LittleEndian(header.AsSpan(4));
                if (payloadLength < 0 || payloadLength > 1024 * 1024)
                    throw new InvalidOperationException("Invalid packet length");

                byte[] body = new byte[payloadLength];
                if (payloadLength > 0)
                    await stream.ReadExactlyAsync(body, ct);

                if (packetType == 0x04 && payloadLength == Marshal.SizeOf<PointerEventPacket>())
                {
                    PointerEventPacket packet = MemoryMarshal.Read<PointerEventPacket>(body);
                    Prism_Inject(_engine, packet);
                }
                else if (packetType == 0x05)
                {
                    await SendPacketAsync(stream, 0x05, body,
                        BinaryPrimitives.ReadInt64LittleEndian(header.AsSpan(8)));
                }
            }
        }
        catch (Exception ex)
        {
            OnLog?.Invoke($"Session closed: {ex.Message}");
        }
        finally
        {
            _activeClientStream = null;
            OnConnectionChanged?.Invoke(false, "Disconnected");
            client.Dispose();
        }
    }

    private void CaptureLoop(CancellationToken ct)
    {
        long frameCount = 0;
        long bytesSent = 0;
        var stopwatch = Stopwatch.StartNew();

        _frameCallback = (dataPtr, length, timestampUs, isKeyFrame) =>
        {
            Stream? stream = _activeClientStream;
            if (stream == null)
                return;

            byte[] nalData = new byte[length];
            Marshal.Copy(dataPtr, nalData, 0, length);
            _ = SendPacketAsync(stream, 0x03, nalData, timestampUs);

            frameCount++;
            bytesSent += length;
        };

        while (!ct.IsCancellationRequested)
        {
            if (_activeClientStream != null)
                Prism_Tick(_engine, _frameCallback);
            else
                Thread.Sleep(10);

            if (stopwatch.ElapsedMilliseconds >= 1000)
            {
                double fps = frameCount / (stopwatch.ElapsedMilliseconds / 1000.0);
                double mbps = bytesSent * 8.0 / (stopwatch.ElapsedMilliseconds * 1000.0);
                OnTelemetryUpdated?.Invoke(fps, mbps);
                frameCount = 0;
                bytesSent = 0;
                stopwatch.Restart();
            }
        }
    }

    private async Task SendPacketAsync(
        Stream stream, byte type, byte[] payload, long timestampUs)
    {
        await _writeLock.WaitAsync();
        try
        {
            byte[] header = new byte[16];
            BinaryPrimitives.WriteUInt16LittleEndian(header.AsSpan(0), Magic);
            header[2] = 1;
            header[3] = type;
            BinaryPrimitives.WriteInt32LittleEndian(header.AsSpan(4), payload.Length);
            BinaryPrimitives.WriteInt64LittleEndian(header.AsSpan(8), timestampUs);

            await stream.WriteAsync(header);
            if (payload.Length > 0)
                await stream.WriteAsync(payload);
            await stream.FlushAsync();
        }
        catch { }
        finally
        {
            _writeLock.Release();
        }
    }

    public void Stop()
    {
        _cts?.Cancel();
        _listener?.Stop();
        if (_engine != IntPtr.Zero)
        {
            Prism_Destroy(_engine);
            _engine = IntPtr.Zero;
        }
    }

    public void Dispose() => Stop();
}
