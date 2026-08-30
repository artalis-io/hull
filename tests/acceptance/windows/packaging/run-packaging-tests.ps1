<#
  run-packaging-tests.ps1 - orchestrates the Winget + Scoop package tests.

  Runs as the runner's own account. Two integrity contexts, by design:

  * Fresh STANDARD (non-admin) user, Developer Mode off - the real
    non-admin proof. Drives: preconditions, Winget manifest VALIDATION, and the
    full Scoop install / invoke (`hull version`) / uninstall. Scoop is per-user
    by design, so it is the complete fresh-standard-user package proof.

  * Runner account, NON-ELEVATED (Medium integrity) - drives the Winget
    install / invoke / uninstall. Winget's per-user App Installer MSIX is not
    provisionable for a freshly-created standard user in a runas session on the
    GitHub image (no loaded profile / package identity), so the install path is
    exercised where App Installer has a valid identity. This is a runner/profile
    limitation and NOT evidence that Hull needs administrator privileges; the
    install is per-user and non-elevated. The runner account is in the
    Administrators group, so it is NOT characterized as a true non-admin account.

  Read-only w.r.t. GitHub releases (downloads the official hull-cosmo; never
  submits to or mutates a package index / bucket).

  Usage: run-packaging-tests.ps1 -RepoRoot <dir> -Evidence <file> -WorkDir <dir>
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$RepoRoot,
    [Parameter(Mandatory = $true)][string]$Evidence,
    [Parameter(Mandatory = $true)][string]$WorkDir
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
function Note($m) { Add-Content -Path $Evidence -Value $m; Write-Host $m }

New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
Set-Content -Path $Evidence -Value "# Windows packaging test evidence`n"

# Standard (non-admin) user + Developer Mode off.
$user = 'hullpkg'
$pw   = ([guid]::NewGuid().ToString() + 'Aa1!')
$sec  = ConvertTo-SecureString $pw -AsPlainText -Force
if (Get-LocalUser -Name $user -ErrorAction SilentlyContinue) { Remove-LocalUser -Name $user }
New-LocalUser -Name $user -Password $sec -AccountNeverExpires -PasswordNeverExpires | Out-Null
Add-LocalGroupMember -Group 'Users' -Member $user -ErrorAction SilentlyContinue
if (Get-LocalGroupMember -Group 'Administrators' -Member $user -ErrorAction SilentlyContinue) { Note "FATAL: packaging test user is in Administrators"; exit 1 }
$cred = New-Object System.Management.Automation.PSCredential("$env:COMPUTERNAME\$user", $sec)
Note "- created standard user $user (not in Administrators)"

$devKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock'
New-Item -Path $devKey -Force | Out-Null
Set-ItemProperty -Path $devKey -Name AllowDevelopmentWithoutDevLicense -Value 0 -Type DWord
Note "- Developer Mode disabled"

# Stage the package metadata + test scripts into a user-writable dir.
Copy-Item (Join-Path $RepoRoot 'packaging\windows') (Join-Path $WorkDir 'packaging') -Recurse -Force
Copy-Item (Join-Path $here 'winget_validate.ps1') (Join-Path $WorkDir 'winget_validate.ps1') -Force
Copy-Item (Join-Path $here 'winget_test.ps1')     (Join-Path $WorkDir 'winget_test.ps1')     -Force
Copy-Item (Join-Path $here 'scoop_test.ps1')      (Join-Path $WorkDir 'scoop_test.ps1')      -Force
Copy-Item (Join-Path (Split-Path -Parent $here) 'preconditions.ps1') (Join-Path $WorkDir 'preconditions.ps1') -Force

& icacls $WorkDir  /grant "${user}:(OI)(CI)M"  | Out-Null
& icacls $Evidence /grant "${user}:M"          | Out-Null
$homeDir = Join-Path $WorkDir 'home'; $tmpDir = Join-Path $WorkDir 'tmp'
$appData = Join-Path $WorkDir 'appdata'; $localApp = Join-Path $WorkDir 'localappdata'
foreach ($d in @($homeDir, $tmpDir, $appData, $localApp)) { New-Item -ItemType Directory -Path $d -Force | Out-Null }
Note ("- standard-user child env: HOME/TEMP/LOCALAPPDATA under {0}" -f $WorkDir)

$pwsh7 = Join-Path $PSHOME 'pwsh.exe'
if (-not (Test-Path $pwsh7)) { $pwsh7 = 'pwsh' }

# Run a staged script AS the standard user. The child inherits the parent env at
# launch, so we redirect HOME/TEMP/APPDATA/LOCALAPPDATA to the granted workdir
# only around the launch, then restore - keeping the orchestrator's real env for
# the runner-context Winget phase.
function Invoke-AsUser([string]$script, [string[]]$phaseArgs, [string]$label) {
    $names = 'HOME', 'USERPROFILE', 'TEMP', 'TMP', 'APPDATA', 'LOCALAPPDATA'
    $snap = @{ }; foreach ($n in $names) { $snap[$n] = [Environment]::GetEnvironmentVariable($n) }
    try {
        $env:HOME = $homeDir; $env:USERPROFILE = $homeDir
        $env:TEMP = $tmpDir;  $env:TMP = $tmpDir
        $env:APPDATA = $appData; $env:LOCALAPPDATA = $localApp
        $o = Join-Path $WorkDir ("phase-$label.out.log"); $e = Join-Path $WorkDir ("phase-$label.err.log")
        $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $WorkDir $script)) + $phaseArgs
        $p = Start-Process -FilePath $pwsh7 -Credential $cred -ArgumentList $a `
                           -WorkingDirectory $WorkDir -Wait -PassThru `
                           -RedirectStandardOutput $o -RedirectStandardError $e
        $code = $p.ExitCode
    } finally {
        foreach ($n in $names) {
            if ($null -eq $snap[$n]) { Remove-Item -Path ("Env:{0}" -f $n) -ErrorAction SilentlyContinue }
            else { Set-Item -Path ("Env:{0}" -f $n) -Value $snap[$n] }
        }
    }
    foreach ($f in @($o, $e)) {
        if ((Test-Path $f) -and (Get-Item $f).Length -gt 0) { Get-Content $f | ForEach-Object { Note ("    [$label] " + $_) } }
    }
    return $code
}

# Run a staged script AS the runner (Medium integrity), in a child pwsh with the
# real env, so it isolates `exit` from this orchestrator.
function Invoke-AsRunner([string]$script, [string[]]$phaseArgs, [string]$label) {
    $o = Join-Path $WorkDir ("phase-$label.out.log"); $e = Join-Path $WorkDir ("phase-$label.err.log")
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $WorkDir $script)) + $phaseArgs
    $p = Start-Process -FilePath $pwsh7 -ArgumentList $a -Wait -PassThru `
                       -RedirectStandardOutput $o -RedirectStandardError $e
    foreach ($f in @($o, $e)) {
        if ((Test-Path $f) -and (Get-Item $f).Length -gt 0) { Get-Content $f | ForEach-Object { Note ("    [$label] " + $_) } }
    }
    return $p.ExitCode
}

$wingetManifest = Join-Path $WorkDir 'packaging\winget'
$scoopManifest  = Join-Path $WorkDir 'packaging\scoop\hull.json'
$rc = 0

Note "`n--- PHASE: preconditions (fresh standard user, Dev-Mode off) ---"
if ((Invoke-AsUser 'preconditions.ps1' @('-Evidence', $Evidence) 'precond') -ne 0) { $rc = 1 }

Note "`n--- PHASE: Winget manifest validation (fresh standard user) ---"
if ((Invoke-AsUser 'winget_validate.ps1' @('-ManifestDir', $wingetManifest, '-Evidence', $Evidence) 'wgvalidate') -ne 0) { $rc = 1 }

Note "`n--- PHASE: Scoop install / invoke / uninstall (fresh standard user) ---"
if ((Invoke-AsUser 'scoop_test.ps1' @('-ScoopManifest', $scoopManifest, '-Evidence', $Evidence) 'scoop') -ne 0) { $rc = 1 }

Note "`n--- PHASE: Winget install / invoke / uninstall (runner, non-elevated) ---"
if ((Invoke-AsRunner 'winget_test.ps1' @('-ManifestDir', $wingetManifest, '-Evidence', $Evidence) 'winget') -ne 0) { $rc = 1 }

Remove-LocalUser -Name $user -ErrorAction SilentlyContinue
Note ("`n## RESULT: {0}" -f ($(if ($rc -eq 0) { 'ALL PACKAGING TESTS PASSED' } else { 'FAILURE' })))
exit $rc
