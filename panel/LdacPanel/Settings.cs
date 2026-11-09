using System;
using Microsoft.Win32;

namespace LdacPanel;

/// <summary>极小配置：HKCU 下几项，不引 JSON/配置文件依赖。</summary>
internal static class Settings
{
    private const string Key = @"Software\LdacPanel";

    public static bool AutoConnect
    {
        get => ReadBool("AutoConnect", true);
        set => Write("AutoConnect", value ? 1 : 0);
    }

    public static bool AutoStart
    {
        get => ReadBool("AutoStart", false);
        set
        {
            Write("AutoStart", value ? 1 : 0);
            SetRunKey(value);
        }
    }

    public static string SenderPath
    {
        get => ReadString("SenderPath", "");
        set => WriteString("SenderPath", value);
    }

    private static bool ReadBool(string name, bool fallback)
    {
        using var key = Registry.CurrentUser.OpenSubKey(Key);
        var value = key?.GetValue(name);
        return value is int i ? i != 0 : fallback;
    }

    private static string ReadString(string name, string fallback)
    {
        using var key = Registry.CurrentUser.OpenSubKey(Key);
        return key?.GetValue(name) as string ?? fallback;
    }

    private static void Write(string name, int value)
    {
        using var key = Registry.CurrentUser.CreateSubKey(Key);
        key?.SetValue(name, value, RegistryValueKind.DWord);
    }

    private static void WriteString(string name, string value)
    {
        using var key = Registry.CurrentUser.CreateSubKey(Key);
        key?.SetValue(name, value, RegistryValueKind.String);
    }

    private const string RunKey = @"Software\Microsoft\Windows\CurrentVersion\Run";

    private static void SetRunKey(bool on)
    {
        using var run = Registry.CurrentUser.OpenSubKey(RunKey, writable: true);
        if (run is null) return;
        if (on)
        {
            var exe = Environment.ProcessPath ?? "";
            if (exe.Length > 0) run.SetValue("LdacPanel", $"\"{exe}\" --minimized");
        }
        else
        {
            run.DeleteValue("LdacPanel", throwOnMissingValue: false);
        }
    }
}
