// AVION Player: a .NET 8 WinForms host of avion.dll (API 12 embedding). Usage: AvionPlayer.exe [video.mp4] [--mute] [--start m:ss] [--capture shot.png]
using System;
using System.Windows.Forms;

namespace AvionPlayer;

static class Program
{
    [STAThread]
    static void Main(string[] args)
    {
        Application.SetHighDpiMode(HighDpiMode.PerMonitorV2);
        Application.EnableVisualStyles();
        string? Option(string name) => Array.IndexOf(args, name) is var i and >= 0 && i + 1 < args.Length ? args[i + 1] : null;
        string? start = Option("--start"), capture = Option("--capture");
        var path = Array.Find(args, a => !a.StartsWith("--") && a != start && a != capture);
        Application.Run(new PlayerForm(path, Array.IndexOf(args, "--mute") >= 0 || capture != null, start, capture));
    }
}
