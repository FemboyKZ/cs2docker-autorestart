using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
using CounterStrikeSharp.API;
using CounterStrikeSharp.API.Core;

namespace AutoRestart;

public class AutoRestartPlugin : BasePlugin
{
    public override string ModuleName => "Auto Restart";

    public override string ModuleVersion => "1.0.1";

    private string _buildVersion = null!;

    private Timer _timer = null!;
    
    private bool _restartNeeded = false;

    private Dictionary<string, string> _pluginVersions = new();

    public override void Load(bool hotReload)
    {
        RegisterListener<Listeners.OnMapEnd>(OnMapEnd);

        _buildVersion = Environment.GetEnvironmentVariable("build_ver")?.Trim()
            ?? throw new Exception("Environment variable 'build_ver' was not found, this plugin is meant to be used with cs2docker!");

        // Snapshot current plugin versions from watchdog layer latest.txt files
        _pluginVersions = ReadPluginVersions();

        _timer = new Timer(OnTimerCallback, null, 0, 10000);
    }

    public override void Unload(bool hotReload)
    {
        _timer.Dispose();
    }

    private static Dictionary<string, string> ReadPluginVersions()
    {
        var versions = new Dictionary<string, string>();
        var layersDir = "/watchdog/layers";
        if (!Directory.Exists(layersDir))
            return versions;

        foreach (var dir in Directory.EnumerateDirectories(layersDir))
        {
            var latestFile = Path.Combine(dir, "latest.txt");
            if (File.Exists(latestFile))
            {
                var name = Path.GetFileName(dir);
                versions[name] = File.ReadAllText(latestFile).Trim();
            }
        }
        return versions;
    }

    private bool IsServerOutOfDate()
    {
        var latestBuildVersion = File.ReadAllText("/watchdog/cs2/latest.txt").Trim();
        if (_buildVersion != latestBuildVersion)
            return true;

        // Check if any plugin layer has been updated since startup
        var currentVersions = ReadPluginVersions();
        foreach (var (name, startupVer) in _pluginVersions)
        {
            if (currentVersions.TryGetValue(name, out var currentVer) && currentVer != startupVer)
                return true;
        }

        return false;
    }

    private void OnTimerCallback(object? state)
    {
        if (IsServerOutOfDate())
        {
            Server.NextWorldUpdate(() =>
            {
                var numPlayers = Utilities.GetPlayers().Where(x => !x.IsBot).Count();
                if (numPlayers == 0)
                {
                    Server.ExecuteCommand("quit");
                }
                else if (!_restartNeeded)
                {
                    _restartNeeded = true;
                    Server.PrintToChatAll("The server will restart at the next opportunity!");
                }
            });
        }
    }

    public void OnMapEnd()
    {
        if (_restartNeeded || IsServerOutOfDate())
        {
            // Doesn't seem to be exiting cleanly without NextWorldUpdate?
            Server.NextWorldUpdate(() => Server.ExecuteCommand("quit"));
        }
    }
}
