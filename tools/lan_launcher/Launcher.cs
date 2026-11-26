using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Markup;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using Ellipse = System.Windows.Shapes.Ellipse;
using System.Windows.Threading;

internal sealed class Launcher
{
    private const string Port = "7777";
    // Mirrors connect_timeout in mt_netplay.cpp.
    private const int WaitSeconds = 25, DoublesWaitSeconds = 60;
    private static readonly Brush ReadyColor = new SolidColorBrush(Color.FromRgb(203, 239, 120));
    private static readonly Brush ErrorColor = new SolidColorBrush(Color.FromRgb(255, 173, 146));
    private static readonly Regex LobbyOccupancy = new Regex(@"\[netplay\] lobby occupancy ([1-4]/[24])",
        RegexOptions.RightToLeft);
    private readonly string folder = AppDomain.CurrentDomain.BaseDirectory;
    private readonly Window window;
    private readonly Button hostButton, joinButton, copyButton, cancelButton, logButton;
    private readonly ComboBox hostAddress, modePicker, guestSeat;
    private readonly TextBox joinAddress;
    private readonly TextBlock statusText, addressHint, modeGuide;
    private readonly Ellipse signal;
    private readonly DispatcherTimer timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(240) };
    private readonly Stopwatch elapsed = new Stopwatch();
    private Process game;
    private string logPath;
    private DateTime startedUtc;
    private bool hosting, connected, booted;

    [STAThread]
    private static void Main()
    {
        try { new Application().Run(new Launcher().window); }
        catch (Exception error)
        {
            MessageBox.Show("Não foi possível abrir o launcher.\n\n" + error.Message,
                            "Mario Tennis 64 Online", MessageBoxButton.OK, MessageBoxImage.Error);
        }
    }

    private Launcher()
    {
        var assembly = Assembly.GetExecutingAssembly();
        using (var layout = assembly.GetManifestResourceStream("Launcher.xaml"))
            window = (Window)XamlReader.Load(layout);

        hostButton = Part<Button>("HostButton");
        joinButton = Part<Button>("JoinButton");
        copyButton = Part<Button>("CopyButton");
        cancelButton = Part<Button>("CancelButton");
        logButton = Part<Button>("LogButton");
        hostAddress = Part<ComboBox>("HostAddress");
        joinAddress = Part<TextBox>("JoinAddress");
        statusText = Part<TextBlock>("StatusText");
        addressHint = Part<TextBlock>("AddressHint");
        modeGuide = Part<TextBlock>("ModeGuide");
        modePicker = Part<ComboBox>("ModePicker");
        guestSeat = Part<ComboBox>("GuestSeat");
        modePicker.ItemsSource = new[] { "1v1 · 2 PCs", "Duplas · 2 PCs", "Duplas · 4 PCs" };
        guestSeat.ItemsSource = new[] { "Jogador 2", "Jogador 3", "Jogador 4" };
        modePicker.SelectedIndex = guestSeat.SelectedIndex = 0;
        signal = Part<Ellipse>("Signal");

        using (var image = assembly.GetManifestResourceStream("Court.jpg"))
        {
            var bitmap = new BitmapImage();
            bitmap.BeginInit();
            bitmap.StreamSource = image;
            bitmap.CacheOption = BitmapCacheOption.OnLoad;
            bitmap.EndInit();
            bitmap.Freeze();
            ((ImageBrush)Part<Border>("CourtPanel").Background).ImageSource = bitmap;
        }

        logPath = new[] { "host.log", "convidado.log" }.Select(name => Path.Combine(folder, name))
            .Where(File.Exists).OrderByDescending(File.GetLastWriteTimeUtc).FirstOrDefault();

        var addresses = LocalAddresses();
        hostAddress.ItemsSource = addresses.Count == 0 ? (System.Collections.IEnumerable)
            new[] { "Nenhum endereço IPv4 detectado" } : addresses;
        hostAddress.SelectedIndex = 0;
        hostButton.Click += delegate { StartGame(true); };
        joinButton.Click += delegate { StartGame(false); };
        copyButton.Click += delegate { CopyAddress(); };
        logButton.Click += delegate { OpenLog(); };
        cancelButton.Click += delegate { StopGame(); };
        hostAddress.SelectionChanged += delegate { RefreshControls(); };
        joinAddress.TextChanged += delegate { RefreshControls(); };
        joinAddress.KeyDown += delegate(object sender, KeyEventArgs e)
        {
            if (e.Key != Key.Enter) return;
            StartGame(false);
            e.Handled = true;
        };
        modePicker.SelectionChanged += delegate { RefreshControls(); };
        window.Closing += Closing;
        timer.Tick += delegate { RefreshSession(); };
        RefreshControls();
    }

    private T Part<T>(string name) where T : class { return (T)window.FindName(name); }

    private sealed class LocalAddress
    {
        public string Address, Adapter;
        public int Priority;
        public override string ToString() { return Address + "  ·  " + Adapter; }
    }

    private static IList<LocalAddress> LocalAddresses()
    {
        var addresses = new Dictionary<string, LocalAddress>();
        try
        {
            foreach (var adapter in NetworkInterface.GetAllNetworkInterfaces())
            {
                if (adapter.OperationalStatus != OperationalStatus.Up ||
                    adapter.NetworkInterfaceType == NetworkInterfaceType.Loopback) continue;

                var properties = adapter.GetIPProperties();
                bool hasGateway = properties.GatewayAddresses.Any(gateway =>
                    gateway.Address.AddressFamily == AddressFamily.InterNetwork &&
                    !gateway.Address.Equals(IPAddress.Any));
                foreach (var item in properties.UnicastAddresses)
                {
                    if (item.Address.AddressFamily != AddressFamily.InterNetwork ||
                        IPAddress.IsLoopback(item.Address)) continue;

                    string address = item.Address.ToString();
                    int priority = hasGateway ? 0 : 1;
                    LocalAddress previous;
                    if (!addresses.TryGetValue(address, out previous) || priority < previous.Priority)
                        addresses[address] = new LocalAddress { Address = address, Adapter = adapter.Name, Priority = priority };
                }
            }
        }
        catch (NetworkInformationException)
        {
            // Hosting can still listen when Windows cannot list the local adapters.
        }
        return addresses.Values.OrderBy(item => item.Priority).ThenBy(item => item.Adapter)
                         .ThenBy(item => item.Address).ToList();
    }

    private static bool ParseIPv4(string text, out string address)
    {
        address = null;
        string[] parts = text.Trim().Split('.');
        if (parts.Length != 4) return false;
        var bytes = new byte[4];
        for (int i = 0; i < parts.Length; ++i)
        {
            if (parts[i].Length > 3 ||
                !byte.TryParse(parts[i], NumberStyles.None, CultureInfo.InvariantCulture, out bytes[i])) return false;
        }
        if (bytes[0] == 0 || bytes[0] >= 224) return false;
        address = new IPAddress(bytes).ToString();
        return true;
    }

    private void RefreshControls()
    {
        bool idle = game == null;
        hostButton.IsEnabled = joinButton.IsEnabled = hostAddress.IsEnabled = joinAddress.IsEnabled = idle;
        modePicker.IsEnabled = guestSeat.IsEnabled = idle;
        guestSeat.Visibility = modePicker.SelectedIndex == 2 ? Visibility.Visible : Visibility.Collapsed;
        switch (modePicker.SelectedIndex)
        {
            case 1:
                modeGuide.Text = "Escolham 4 Players no jogo. Quem hospeda usa P1 e P2. Quem entra usa P3 e P4. Por PC, use dois controles ou teclado e um controle.";
                break;
            case 2:
                modeGuide.Text = "Escolham 4 Players no jogo. Quem hospeda fica com P1. Cada convidado escolhe um lugar diferente entre P2, P3 e P4.";
                break;
            default:
                modeGuide.Text = "Escolham 2 Players no jogo. Quem hospeda é o jogador 1. Quem entra é o jogador 2.";
                break;
        }
        copyButton.IsEnabled = hostAddress.SelectedItem is LocalAddress;
        addressHint.Visibility = joinAddress.Text.Length == 0 ? Visibility.Visible : Visibility.Collapsed;
        cancelButton.Visibility = idle ? Visibility.Collapsed : Visibility.Visible;
        cancelButton.Content = connected ? "Encerrar" : "Cancelar";
        logButton.Visibility = File.Exists(logPath) ? Visibility.Visible : Visibility.Collapsed;
    }

    private void SetStatus(string text, bool error = false)
    {
        statusText.Text = text;
        signal.Fill = error ? ErrorColor : ReadyColor;
    }

    private void CopyAddress()
    {
        var selected = hostAddress.SelectedItem as LocalAddress;
        if (selected == null) return;
        try
        {
            Clipboard.SetText(selected.Address);
            if (game == null) SetStatus("Endereço copiado. Envie ao seu amigo.");
        }
        catch (ExternalException)
        {
            SetStatus("O Windows não conseguiu copiar. Passe o endereço que aparece acima.", true);
        }
    }

    private void StartGame(bool asHost)
    {
        if (game != null) return;
        string peer = null;
        if (!asHost && !ParseIPv4(joinAddress.Text, out peer))
        {
            SetStatus("Digite o endereço IPv4 que seu amigo enviou. Ex. 192.168.1.20.", true);
            joinAddress.Focus();
            return;
        }

        string gameFolder = Path.Combine(folder, "jogo");
        string executable = Path.Combine(gameFolder, "MarioTennisRecompiled.exe");
        if (!File.Exists(executable) || !File.Exists(Path.Combine(gameFolder, "mt.n64.us.z64")))
        {
            SetStatus("Extraia o ZIP inteiro para manter a pasta jogo junto ao launcher.", true);
            return;
        }

        hosting = asHost;
        connected = booted = false;
        string logName = hosting ? "host.log" : "convidado.log";
        logPath = Path.Combine(folder, logName);
        var launch = new ProcessStartInfo(executable)
        {
            WorkingDirectory = gameFolder, UseShellExecute = false, CreateNoWindow = true
        };
        launch.EnvironmentVariables.Remove("MT_NETPLAY_HOST");
        launch.EnvironmentVariables.Remove("MT_NETPLAY_JOIN");
        launch.EnvironmentVariables[hosting ? "MT_NETPLAY_HOST" : "MT_NETPLAY_JOIN"] =
            hosting ? Port : peer + ":" + Port;
        string[] layouts = { "1+1", "2+2", "1+1+1+1" };
        launch.EnvironmentVariables["MT_NETPLAY_MODE"] = layouts[modePicker.SelectedIndex];
        launch.EnvironmentVariables.Remove("MT_NETPLAY_SEAT");
        if (!asHost && modePicker.SelectedIndex == 2)
            launch.EnvironmentVariables["MT_NETPLAY_SEAT"] = (guestSeat.SelectedIndex + 2).ToString(CultureInfo.InvariantCulture);
        launch.EnvironmentVariables["MT_LOG"] = Path.Combine("..", logName);

        try
        {
            startedUtc = DateTime.UtcNow;
            game = Process.Start(launch);
        }
        catch (Win32Exception)
        {
            SetStatus("O Windows não conseguiu abrir o jogo. Extraia o pacote novamente.", true);
            return;
        }

        elapsed.Restart();
        RefreshSession();
        if (game != null) timer.Start();
    }

    private string ReadLog()
    {
        try
        {
            if (!File.Exists(logPath) || File.GetLastWriteTimeUtc(logPath) < startedUtc.AddSeconds(-1)) return "";
            using (var stream = new FileStream(logPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            {
                const int tailLength = 65536;
                if (stream.Length > tailLength) stream.Seek(-tailLength, SeekOrigin.End);
                using (var reader = new StreamReader(stream)) return reader.ReadToEnd();
            }
        }
        catch (IOException) { return ""; }
    }

    private void RefreshSession()
    {
        if (game == null) return;
        string log = ReadLog();
        connected |= log.Contains("[netplay] compatibility verified");
        booted |= log.Contains("[mt] boot:");
        if (game.HasExited)
        {
            int code = game.ExitCode;
            string message = ExitMessage(code, log);
            ClearSession();
            SetStatus(message, code != 0);
            return;
        }

        if (connected)
        {
            SetStatus(booted ? (modePicker.SelectedIndex == 0 ? "Conectado. Escolham 2 Players no jogo." : "Conectado. Escolham 4 Players no jogo.") : "Conectado. Abrindo o jogo…");
        }
        else
        {
            int timeout = modePicker.SelectedIndex == 0 ? WaitSeconds : DoublesWaitSeconds;
            int remaining = Math.Max(0, timeout - (int)elapsed.Elapsed.TotalSeconds);
            string waiting = hosting ? "Aguardando seu amigo" : "Conectando ao host";
            if (modePicker.SelectedIndex != 0)
            {
                var occupancy = LobbyOccupancy.Match(log);
                waiting = occupancy.Success ? "PCs conectados " + occupancy.Groups[1].Value
                                             : "Aguardando os jogadores";
            }
            SetStatus(waiting + (remaining > 0 ? "… " + remaining + " s" : "…"));
        }
        RefreshControls();
    }

    private string ExitMessage(int code, string log)
    {
        if (code == 0) return "Sessão encerrada. Vocês podem iniciar outra.";
        if (log.Contains("seat already occupied"))
            return "Esta vaga já está ocupada. Escolha outro jogador e tente entrar de novo.";
        if (log.Contains("compatibility handshake timed out") && modePicker.SelectedIndex != 0)
            return "A sala ficou incompleta. Confiram o modo, o endereço e as vagas.";
        if (log.Contains("compatibility handshake timed out"))
            return hosting ? "Seu amigo não entrou a tempo. Clique em Hospedar para tentar de novo."
                           : "Não conectou. Confirme o endereço e peça ao amigo para abrir Hospedar.";
        if (log.Contains("settings differ") || log.Contains("rejected game compatibility") ||
            log.Contains("incompatible recomp-net"))
            return "Os dois jogadores precisam usar a mesma versão deste pacote.";
        if (log.Contains("could not open UDP session"))
            return "Não foi possível abrir a conexão. Feche outra sessão que esteja aberta neste PC.";
        if (log.Contains("Store a valid"))
            return "A ROM do pacote está ausente ou corrompida. Extraia o ZIP novamente.";
        if (log.Contains("court desync") || log.Contains("input desync"))
            return "As duas partidas divergiram. Guardem os logs para investigar.";
        if (log.Contains("peer left") || log.Contains("timed out waiting for input"))
            return "A conexão com o outro jogador caiu. Guardem os logs se isso se repetir.";
        return "O jogo encerrou com erro. Abra o log para conferir os detalhes.";
    }

    private void OpenLog()
    {
        try
        {
            if (File.Exists(logPath)) Process.Start(new ProcessStartInfo(logPath) { UseShellExecute = true });
        }
        catch (Win32Exception) { SetStatus("O log está na pasta do launcher.", true); }
    }

    private void ClearSession()
    {
        timer.Stop();
        game.Dispose();
        game = null;
        connected = false;
        RefreshControls();
    }

    private void StopGame()
    {
        if (game == null) return;
        try
        {
            if (!game.HasExited)
            {
                game.Kill();
                if (!game.WaitForExit(700)) return;
            }
        }
        catch (InvalidOperationException) { /* The game exited during cancellation. */ }
        catch (Win32Exception)
        {
            SetStatus("Não foi possível encerrar. Feche a janela do jogo.", true);
            return;
        }

        ClearSession();
        SetStatus("Sessão encerrada.");
    }

    private void Closing(object sender, CancelEventArgs e)
    {
        if (game == null) return;
        if (connected && MessageBox.Show(window, "Encerrar a partida e fechar o launcher?",
            "Encerrar partida", MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes)
        {
            e.Cancel = true;
            return;
        }
        StopGame();
        e.Cancel = game != null;
    }
}

