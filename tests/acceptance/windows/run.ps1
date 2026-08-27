<#
  run.ps1 - orchestrates the Windows acceptance run. Runs as the runner's admin
  account, but drives every acceptance phase AS a freshly-created STANDARD
  (non-admin) user, with Developer Mode disabled, so the non-admin / self-update
  / ACL evidence is meaningful. Aggregates a single evidence report and fails
  closed if any phase fails.

  It never creates, modifies, or deletes a GitHub release: it only downloads the
  previous release's Windows APE and drives `hull update --repo=<staging>`
  against the maintainer-prepopulated staging repos.

  Usage:
    run.ps1 -OfficialRepo artalis-io/hull -PrevTag v0.13.0 -PrevVersion 0.13.0 \
            -RcRepo <org/staging-rc> -ExpectVersion 0.14.0-rc1 \
            -Evidence <file> -WorkDir <dir>
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OfficialRepo,
    [Parameter(Mandatory=$true)][string]$PrevTag,
    [Parameter(Mandatory=$true)][string]$PrevVersion,
    [Parameter(Mandatory=$true)][string]$RcRepo,
    [Parameter(Mandatory=$true)][string]$ExpectVersion,
    [Parameter(Mandatory=$true)][string]$Evidence,
    [Parameter(Mandatory=$true)][string]$WorkDir,
    [string]$PrevRepo = ''
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
function Note($m) { Add-Content -Path $Evidence -Value $m; Write-Host $m }

New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
Set-Content -Path $Evidence -Value "# Windows acceptance evidence`n"

# ── Standard (non-admin) user ────────────────────────────────────────────────
$user = 'hullacc'
$pw   = ([guid]::NewGuid().ToString() + 'Aa1!')
$sec  = ConvertTo-SecureString $pw -AsPlainText -Force
if (Get-LocalUser -Name $user -ErrorAction SilentlyContinue) { Remove-LocalUser -Name $user }
New-LocalUser -Name $user -Password $sec -AccountNeverExpires -PasswordNeverExpires | Out-Null
Add-LocalGroupMember -Group 'Users' -Member $user -ErrorAction SilentlyContinue
# Explicitly NOT in Administrators. Assert.
if (Get-LocalGroupMember -Group 'Administrators' -Member $user -ErrorAction SilentlyContinue) {
    Note "FATAL: acceptance user is in Administrators"; exit 1
}
$cred = New-Object System.Management.Automation.PSCredential("$env:COMPUTERNAME\$user", $sec)
Note "- created standard user $user (not in Administrators)"

# ── Disable Developer Mode (machine policy) ──────────────────────────────────
$devKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock'
New-Item -Path $devKey -Force | Out-Null
Set-ItemProperty -Path $devKey -Name AllowDevelopmentWithoutDevLicense -Value 0 -Type DWord
Note "- Developer Mode disabled (AllowDevelopmentWithoutDevLicense=0)"

# ── Make WorkDir + the scripts readable/executable by the user ───────────────
& icacls $WorkDir /grant "${user}:(OI)(CI)M" | Out-Null
& icacls $here    /grant "${user}:(OI)(CI)RX" | Out-Null

# ── Download the previous release's Windows APE (read-only) ──────────────────
$prevExe = Join-Path $WorkDir 'hull-prev.com'
$url = "https://github.com/$OfficialRepo/releases/download/$PrevTag/hull-cosmo"
Invoke-WebRequest -Uri $url -OutFile $prevExe
Note "- downloaded previous APE $PrevTag ($url)"

# Working copies for the phases.
$suDir  = Join-Path $WorkDir 'selfupdate'; New-Item -ItemType Directory -Path $suDir  | Out-Null
$aclDir = Join-Path $WorkDir 'acl';        New-Item -ItemType Directory -Path $aclDir | Out-Null
$suHull  = Join-Path $suDir  'hull.com'; Copy-Item $prevExe $suHull
$aclHull = Join-Path $aclDir 'hull.com'; Copy-Item $prevExe $aclHull
& icacls $suDir  /grant "${user}:(OI)(CI)M" | Out-Null
& icacls $aclDir /grant "${user}:(OI)(CI)M" | Out-Null

# ── Run a phase AS the standard user; return its exit code ───────────────────
function Invoke-AsUser([string]$script, [string[]]$phaseArgs) {
    $a = @('-NoProfile','-ExecutionPolicy','Bypass','-File', (Join-Path $here $script)) + $phaseArgs
    $p = Start-Process -FilePath 'powershell.exe' -Credential $cred -ArgumentList $a `
                       -WorkingDirectory $WorkDir -Wait -PassThru
    return $p.ExitCode
}

$rc = 0
Note "`n--- PHASE: preconditions ---"
if ((Invoke-AsUser 'preconditions.ps1' @('-Evidence', $Evidence)) -ne 0) { $rc = 1 }

Note "`n--- PHASE: self-update (upgrade) ---"
if ((Invoke-AsUser 'self_update.ps1' @('-Hull',$suHull,'-Repo',$RcRepo,'-ExpectVersion',$ExpectVersion,'-Evidence',$Evidence)) -ne 0) { $rc = 1 }

Note "`n--- PHASE: rollback via real ACL ---"
if ((Invoke-AsUser 'rollback_acl.ps1' @('-Hull',$aclHull,'-Repo',$RcRepo,'-PrevVersion',$PrevVersion,'-Evidence',$Evidence)) -ne 0) { $rc = 1 }

Note "`n--- PHASE: extras (spaces / cosmocc / ping / nested / doctor) ---"
# $suHull is now the candidate (updated in the self-update phase); PrevHull is a
# fresh copy for the spaces self-update inside extras.
$prevForExtras = Join-Path $WorkDir 'hull-prev-extras.com'; Copy-Item $prevExe $prevForExtras
& icacls $prevForExtras /grant "${user}:RX" | Out-Null
if ((Invoke-AsUser 'extras.ps1' @('-Hull',$suHull,'-PrevHull',$prevForExtras,'-RcRepo',$RcRepo,'-ExpectVersion',$ExpectVersion,'-Evidence',$Evidence)) -ne 0) { $rc = 1 }

if ($PrevRepo -ne '') {
    Note "`n--- PHASE: downgrade (staging-based) ---"
    $dgDir = Join-Path $WorkDir 'downgrade'; New-Item -ItemType Directory -Path $dgDir | Out-Null
    $dgHull = Join-Path $dgDir 'hull.com'; Copy-Item $prevExe $dgHull
    & icacls $dgDir /grant "${user}:(OI)(CI)M" | Out-Null
    if ((Invoke-AsUser 'downgrade.ps1' @('-Hull',$dgHull,'-RcRepo',$RcRepo,'-PrevRepo',$PrevRepo,'-RcVersion',$ExpectVersion,'-PrevVersion',$PrevVersion,'-Evidence',$Evidence)) -ne 0) { $rc = 1 }
} else {
    Note "`n--- PHASE: downgrade SKIPPED (no prev staging repo provided) ---"
}

# ── Cleanup the local user ───────────────────────────────────────────────────
Remove-LocalUser -Name $user -ErrorAction SilentlyContinue

Note ("`n## RESULT: {0}" -f ($(if ($rc -eq 0) {'ALL PHASES PASSED'} else {'FAILURE'})))
exit $rc
