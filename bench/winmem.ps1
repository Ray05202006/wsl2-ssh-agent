# Usage: powershell.exe -NoProfile -ExecutionPolicy Bypass -File winmem.ps1 <ImageName> [ExcludePids]
# One line per process with that image name (minus excluded pids and this script):
# working set, private working set, private bytes (commit), peak WS, threads, handles, CPU.
param([string]$Name, [string]$Exclude = "")
$ex = @($PID) + ($Exclude -split ',' | Where-Object { $_ } | ForEach-Object { [int]$_ })
foreach ($p in Get-CimInstance Win32_Process -Filter "Name='$Name'") {
  if ($ex -contains [int]$p.ProcessId) { continue }
  $perf = Get-CimInstance Win32_PerfRawData_PerfProc_Process -Filter "IDProcess=$($p.ProcessId)"
  $gp = Get-Process -Id $p.ProcessId -ErrorAction SilentlyContinue
  if (-not $gp) { continue }
  "win name={0} pid={1} ws_kb={2} privws_kb={3} private_kb={4} peak_ws_kb={5} threads={6} handles={7} cpu_ms={8}" -f `
    $Name, $p.ProcessId, [int]($gp.WorkingSet64/1kb), [int]($perf.WorkingSetPrivate/1kb), `
    [int]($gp.PrivateMemorySize64/1kb), [int]($gp.PeakWorkingSet64/1kb), $gp.Threads.Count, $gp.HandleCount, `
    [int]$gp.TotalProcessorTime.TotalMilliseconds
}
