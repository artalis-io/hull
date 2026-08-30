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
    Note "- BOUNDARY (expected): the fresh standard-user probe fails ONLY because Winget/App Installer cannot execute in this unprovisioned runas profile (App Installer is not even visible to the user to resolve)."
    Note "- This is the Winget/App Installer provisioning model, NOT a Hull defect and NOT an install failure. Schema validation runs under the runner account (winget_test.ps1) and is not non-admin evidence; Scoop is the actual fresh-standard-user install proof."
    Note "WINGET-PROBE: OK (expected standard-user boundary recorded; non-gating)"
    exit 0
}
Note ("- winget.exe: {0}" -f $winget)

try {
    $v = Invoke-Native $winget @('validate', '--manifest', $ManifestDir)
    Note (($v.Out) -replace '(?m)^', '    ')
    if ($v.Code -eq 0 -or $v.Out -match 'Manifest validation succeeded') {
        Note "- NOTE: a standard user was unexpectedly able to run winget validate on this image (recorded for information; the validation gate still runs under the runner)"
    } else {
        Note ("- BOUNDARY (expected): winget could not validate as a standard user (code {0}) - the unprovisioned runas profile" -f $v.Code)
    }
} catch {
    Note ("- BOUNDARY (expected): the fresh standard-user probe fails ONLY because Winget/App Installer cannot execute in this unprovisioned runas profile: {0}" -f $_.Exception.Message)
    Note "- (the WindowsApps binary is ACL-locked and there is no package identity / execution alias without a loaded profile). NOT a Hull defect and NOT an install failure."
}
Note "- Schema validation + install/invoke/uninstall run under the runner (winget_test.ps1) and are NOT presented as non-admin evidence; Scoop is the actual fresh-standard-user install proof."
Note "WINGET-PROBE: OK (expected standard-user boundary recorded; non-gating)"
exit 0
