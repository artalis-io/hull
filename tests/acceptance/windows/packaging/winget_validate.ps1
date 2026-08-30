<#
  winget_validate.ps1 - PROBE whether a freshly-created standard (non-admin) user
  can run Winget at all on this image.

  This is a boundary probe, NOT the validation gate (the gate is `winget validate`
  in the runner phase, winget_test.ps1). On the GitHub image a fresh standard user
  cannot execute winget: it has no App Installer package identity / execution
  alias without a loaded profile, and the raw package binary under
  `Program Files\WindowsApps\...` is ACL-locked ("Access is denied"). We record
  whichever outcome occurs and exit 0 either way - the constraint is documented in
  the evidence, and all real Winget operations run under the runner account.

  Usage: winget_validate.ps1 -ManifestDir <dir> -Evidence <file>
         (winget.exe path is passed via $env:HULL_WINGET_EXE by the orchestrator)
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ManifestDir,
    [Parameter(Mandatory = $true)][string]$Evidence
)
$WingetExe = $env:HULL_WINGET_EXE

$ErrorActionPreference = 'Stop'
function Note($m) { Add-Content -Path $Evidence -Value $m; Write-Host $m }
function Invoke-Native([string]$Exe, [string[]]$CmdArgs, [int]$TimeoutSec = 300) {
    $o = [System.IO.Path]::GetTempFileName(); $e = [System.IO.Path]::GetTempFileName()
    try {
        $p = Start-Process -FilePath $Exe -ArgumentList $CmdArgs -NoNewWindow -PassThru -RedirectStandardOutput $o -RedirectStandardError $e
        if (-not $p.WaitForExit($TimeoutSec * 1000)) { try { $p.Kill() } catch { }; return @{ Code = $null; Out = ''; TimedOut = $true } }
        return @{ Code = $p.ExitCode; Out = ((Get-Content -LiteralPath $o -Raw -EA SilentlyContinue) + "`n" + (Get-Content -LiteralPath $e -Raw -EA SilentlyContinue)); TimedOut = $false }
    } finally { Remove-Item -LiteralPath $o, $e -Force -EA SilentlyContinue }
}

Note "## Winget standard-user boundary probe (as $(whoami), a true standard user)"

$winget = $null
if ($WingetExe -and (Test-Path -LiteralPath $WingetExe)) { $winget = $WingetExe }
else {
    $pkg = Get-AppxPackage -Name Microsoft.DesktopAppInstaller -EA SilentlyContinue | Select-Object -First 1
    if ($pkg) { $cand = Join-Path $pkg.InstallLocation 'winget.exe'; if (Test-Path -LiteralPath $cand) { $winget = $cand } }
    if (-not $winget) { $cmd = Get-Command winget -ErrorAction SilentlyContinue; if ($cmd) { $winget = $cmd.Source } }
}
if (-not $winget) {
    Note "- BOUNDARY: a fresh standard user cannot resolve winget.exe (App Installer not visible to the user)"
    Note "- validation runs under the runner account instead (winget_test.ps1); Scoop is the non-admin proof"
    Note "WINGET-PROBE: standard user cannot run winget (documented boundary; non-gating)"
    exit 0
}
Note ("- winget.exe: {0}" -f $winget)

try {
    $v = Invoke-Native $winget @('validate', '--manifest', $ManifestDir)
    Note (($v.Out) -replace '(?m)^', '    ')
    if ($v.Code -eq 0 -or $v.Out -match 'Manifest validation succeeded') {
        Note "- NOTE: a standard user was able to run winget validate on this image"
    } else {
        Note ("- BOUNDARY: winget ran but validate did not succeed as a standard user (code {0})" -f $v.Code)
    }
} catch {
    Note ("- BOUNDARY: a fresh standard user cannot execute winget.exe on this image: {0}" -f $_.Exception.Message)
    Note "- (expected: WindowsApps binary is ACL-locked / no package identity without a profile)"
}
Note "- validation gate + install/invoke/uninstall run under the runner (winget_test.ps1); Scoop is the non-admin proof"
Note "WINGET-PROBE: OK (standard-user boundary recorded; non-gating)"
exit 0
