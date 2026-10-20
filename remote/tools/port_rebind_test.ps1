# 0.8.2: after serving requests, a host stopped and started again must get the
# SAME port back (0.8.1: WSAEADDRINUSE from its own TIME_WAIT connections ->
# the next port, owner's log 23:11:34). Dry-run spike, no Tailscale, own port.
param([string]$Spike, [int]$Port = 8971, [string]$Out)
$log1 = "$env:TEMP\rebind1.log"; $log2 = "$env:TEMP\rebind2.log"
Remove-Item $log1, $log2 -ErrorAction SilentlyContinue
$p = Start-Process $Spike -ArgumentList "--port", $Port, "--token", "rebindtest0123456789abcdefABCDEF", "--no-tailscale", "--log", $log1, "--duration", "6" -PassThru -WindowStyle Hidden
Start-Sleep 2
$ip = (Select-String $log1 -Pattern 'http: listening on (\d+\.\d+\.\d+\.\d+):' | Select-Object -First 1).Matches[0].Groups[1].Value
1..5 | ForEach-Object { try { $null = Invoke-WebRequest -UseBasicParsing "http://${ip}:$Port/rebindtest0123456789abcdefABCDEF/" -TimeoutSec 5 } catch {} }
$p.WaitForExit()
$p2 = Start-Process $Spike -ArgumentList "--port", $Port, "--no-tailscale", "--log", $log2, "--duration", "2" -PassThru -WindowStyle Hidden
$p2.WaitForExit()
$bind = Select-String $log2 -Pattern 'http: (listening on|bind) [^ ]+' | ForEach-Object { $_.Line.Substring(0, [Math]::Min(110, $_.Line.Length)) }
$tw = (Get-NetTCPConnection -LocalPort $Port -State TimeWait -ErrorAction SilentlyContinue | Measure-Object).Count
$r = "restart on the same port right after 5 requests:`n" + ($bind -join "`n") + "`nTIME_WAIT sockets on port ${Port} afterwards: $tw"
Set-Content -Encoding utf8 $Out $r
$r
