# Dump the processes of a wedged cosmocc-on-Windows build.
#
# WHY POWERSHELL: only the Win32 process table exposes a process's full command
# line and cumulative CPU time. An MSYS `ps` shows neither usefully, and the one
# question worth answering about this hang is whether the stuck compiler is
# BURNING CPU or BLOCKED - which needs CPU sampled twice.
#
# Two snapshots `IntervalSeconds` apart, reporting the delta:
#   dcpu ~= 0        -> blocked (file lock, pipe, temp-file wait). Suspect the
#                       environment: cosmocc's mktemper temp files, AV scanning,
#                       a stalled pipe - not the optimizer.
#   dcpu ~= interval -> spinning. Suspect codegen/optimizer on that exact TU.
#
# Context: .github/workflows/windows-source-build.yml wedges intermittently
# (~1 run in 4). Two captured signatures so far, and they do NOT share a cause:
# miniz at -O2 (fixed - Keel never got HL_OPT), and stdlib_js_registry at -O0
# (open). Both showed ~0 CPU indefinitely, which is what this script exists to
# confirm or refute with actual numbers.
#
# SPDX-License-Identifier: AGPL-3.0-or-later

param([int]$IntervalSeconds = 5)

$ErrorActionPreference = 'Continue'

# Match on the WHOLE base name, never a substring. An unanchored alternation
# here is worse than useless: `as`, `ar` and `sh` match 1Password.exe,
# StartMenuExperienceHost.exe and ShellHost.exe, burying the one process the
# script exists to show. Anchored set + a cosmo-toolchain catch-all instead.
$exactNames = @(
    'cc1', 'cc1plus', 'gcc', 'g++', 'clang', 'make', 'sh', 'bash', 'dash',
    'ld', 'ld.bfd', 'as', 'ar', 'nm', 'xxd', 'ape', 'apelink', 'fixupobj',
    'mktemper', 'objbincopy', 'zipobj', 'assimilate'
)

function Get-BuildProcs {
    Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object {
            $base = $_.Name -replace '\.(exe|com|ape)$', ''
            ($exactNames -contains $base) -or ($base -like '*cosmo*')
        } |
        ForEach-Object {
            $p = Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue
            [PSCustomObject]@{
                ProcId = $_.ProcessId
                Parent = $_.ParentProcessId
                Name   = $_.Name
                Cpu    = if ($p) { [math]::Round($p.CPU, 2) } else { $null }
                RssMb  = if ($p) { [math]::Round($p.WorkingSet64 / 1MB, 1) } else { $null }
                Cmd    = $_.CommandLine
            }
        }
}

Write-Host "=== stuck-build diagnostics ==="
Write-Host ("sampling process CPU {0}s apart" -f $IntervalSeconds)
Write-Host ""

$first = @(Get-BuildProcs)
Start-Sleep -Seconds $IntervalSeconds
$second = @(Get-BuildProcs)

if ($second.Count -eq 0) {
    Write-Host "no matching build processes found - the wedge may not be a live process"
    exit 0
}

foreach ($proc in ($second | Sort-Object Cpu -Descending)) {
    $prev = $first | Where-Object { $_.ProcId -eq $proc.ProcId } | Select-Object -First 1
    # 'n/a' when the process appeared between samples, so no delta exists.
    # Format the unit into the value, not the template - otherwise it reads
    # "dcpu=n/as".
    $delta = 'n/a'
    if ($prev -and $null -ne $proc.Cpu -and $null -ne $prev.Cpu) {
        $delta = '{0}s' -f [math]::Round($proc.Cpu - $prev.Cpu, 2)
    }
    Write-Host ("pid={0} ppid={1} {2}  cpu={3}s  dcpu={4}  rss={5}MB" -f `
        $proc.ProcId, $proc.Parent, $proc.Name, $proc.Cpu, $delta, $proc.RssMb)
    if ($proc.Cmd) {
        Write-Host ("    cmd: {0}" -f $proc.Cmd)
    }
}

Write-Host ""
Write-Host "READ THIS AS:"
Write-Host "  dcpu ~0 on the stuck compiler -> BLOCKED (I/O, lock, pipe, temp file)."
Write-Host "     Next: which handle. Process Monitor, or check cosmocc's temp dir."
Write-Host ("  dcpu ~{0} -> SPINNING (optimizer/codegen). Next: that TU at -O0 vs -O1." -f $IntervalSeconds)
