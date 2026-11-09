$Log = 'C:\Users\Public\ldac-probe.log'
try { $host.UI.RawUI.WindowTitle = 'LDAC driver log  (close this window to stop)' } catch {}
Write-Host "watching $Log  --  Ctrl+C / close window to stop" -ForegroundColor Cyan
while ($true) {
    try {
        Get-Content -Path $Log -Wait -Tail 40 -Encoding utf8 -ErrorAction Stop | ForEach-Object {
            $l = [string]$_
            if ($l -match 'link-up|endpoint installed|STREAMING|ioctl START') { Write-Host $l -ForegroundColor Green }
            elseif ($l -match 'FAIL|!!|error|ERROR') { Write-Host $l -ForegroundColor Red }
            elseif ($l -match 'link-down|endpoint removed|jack:') { Write-Host $l -ForegroundColor Cyan }
            else { Write-Host $l }
        }
    } catch {
        Start-Sleep -Seconds 1
    }
    Start-Sleep -Seconds 1
}