param(
    [int]$Port = 8090,
    [string]$LogPath = 'C:\Users\Public\ldac-probe.log',
    [int]$TailLines = 400
)
$ErrorActionPreference = 'Stop'

$page = @'
<!doctype html>
<html><head><meta charset="utf-8"><title>LDAC driver log</title>
<style>
 body{background:#111;color:#ddd;font:13px/1.45 Consolas,monospace;margin:0}
 .bar{position:sticky;top:0;background:#1a1a1a;padding:6px;border-bottom:1px solid #333}
 #log{padding:6px;white-space:pre-wrap;word-break:break-all}
 button{background:#222;color:#ddd;border:1px solid #555;padding:2px 8px;cursor:pointer}
</style></head><body>
<div class="bar">
 <button id="p">pause</button>
 <label><input type="checkbox" id="f" checked> follow</label>
 <span id="st"></span>
</div>
<div id="log"></div>
<script>
let paused=false;
document.getElementById('p').onclick=function(){paused=!paused;this.textContent=paused?'resume':'pause';};
async function tick(){
  if(!paused){
    try{
      const r=await fetch('/tail',{cache:'no-store'});
      const j=await r.json();
      const el=document.getElementById('log');
      el.textContent=(j.lines||[]).join('\n');
      if(document.getElementById('f').checked) window.scrollTo(0,document.body.scrollHeight);
      document.getElementById('st').textContent=(j.lines||[]).length+' lines  '+new Date().toLocaleTimeString();
    }catch(e){document.getElementById('st').textContent='ERR '+e;}
  }
  setTimeout(tick,700);
}
tick();
</script></body></html>
'@

$listener = New-Object System.Net.HttpListener
$listener.Prefixes.Add("http://+:$Port/")
$listener.Start()
Write-Host "listening on $Port, log=$LogPath"

while ($true) {
    $ctx = $listener.GetContext()
    try {
        $path = $ctx.Request.Url.AbsolutePath
        if ($path -eq '/tail') {
            $all = @()
            if (Test-Path $LogPath) {
                $all = @(Get-Content -Path $LogPath -Tail $TailLines -Encoding utf8 -ErrorAction SilentlyContinue | ForEach-Object { [string]$_ })
            }
            $json = @{ lines = $all } | ConvertTo-Json -Compress -Depth 3
            $bytes = [Text.Encoding]::UTF8.GetBytes($json)
            $ctx.Response.ContentType = 'application/json; charset=utf-8'
        } else {
            $bytes = [Text.Encoding]::UTF8.GetBytes($page)
            $ctx.Response.ContentType = 'text/html; charset=utf-8'
        }
        $ctx.Response.ContentLength64 = $bytes.Length
        $ctx.Response.OutputStream.Write($bytes, 0, $bytes.Length)
    } catch { } finally { $ctx.Response.Close() }
}