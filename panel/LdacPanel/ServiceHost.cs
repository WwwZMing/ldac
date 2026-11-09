using System;
using System.ServiceProcess;

namespace LdacPanel;

/// <summary>Windows 服务：唯一拥有发送端的地方。GUI 崩了也不影响声音。</summary>
internal sealed class LdacService : ServiceBase
{
    private Core? _core;
    private ApiServer? _api;

    public LdacService()
    {
        ServiceName = "LdacA2dpSvc";
        CanStop = true;
        CanShutdown = true;
        AutoLog = true;
    }

    protected override void OnStart(string[] args)
    {
        PanelLog.Write("service: starting");
        _core = new Core();
        _core.Start();
        _core.StartWatchdog();
        _api = new ApiServer(_core);
        _api.Start();
        PanelLog.Write("service: started");
    }

    protected override void OnStop()
    {
        PanelLog.Write("service: stopping");
        _api?.Stop();
        _core?.StopWatchdog();
        _core?.Stop();
        PanelLog.Write("service: stopped");
    }

    protected override void OnShutdown() => OnStop();
}
