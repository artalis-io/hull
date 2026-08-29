<#
  run.ps1 - orchestrates the Windows acceptance run. Runs as the runner's admin
  account but drives every phase AS a freshly-created STANDARD (non-admin) user
  with Developer Mode disabled, so the non-admin / self-update / ACL evidence is
  meaningful. Aggregates a single evidence report; fails closed if any phase
  fails.

  Honest before/after self-update contract (v0.14.0):
    - the OLD v0.13.0 APE `hull update` MUST FAIL on Windows (its update code
      predates the deferred-swap fix - a running .exe can't be replaced);
    - the CANDIDATE performing an update MUST SUCCEED via the deferred swap
      (force-reinstall to the RC, and downgrade to the previous release);
    - the ACL-induced rollback and the path-with-spaces coverage run FROM the
      candidate.

  Read-only w.r.t. releases: downloads the previous + candidate APEs and drives
  `hull update --repo=<staging>` against maintainer-prepopulated staging repos.

  Usage:
    run.ps1 -OfficialRepo artalis-io/hull -RcTag v0.14.0-rc1 -PrevTag v0.13.0 \
            -PrevVersion 0.13.0 -RcRepo <org/staging-rc> -ExpectVersion 0.14.0-rc1 \
            -PrevRepo <org/staging-prev> -Evidence <file> -WorkDir <dir>
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$OfficialRepo,
    [Parameter(Mandatory=$true)][string]$RcTag,
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

# ── Grants + a fully user-writable environment for the child phases ──────────
# Start-Process -Credential inherits THIS process's environment, which by default
# points HOME/TEMP/APPDATA at the RUNNER-ADMIN's profile (unwritable by hullacc).
# Redirect all of them at user-granted dirs so a runas child (no loaded profile)
# has writable home/temp/appdata for hull's ~/.hull and for scratch files.
& icacls $WorkDir  /grant "${user}:(OI)(CI)M"  | Out-Null
& icacls $here     /grant "${user}:(OI)(CI)RX" | Out-Null
& icacls $Evidence /grant "${user}:M"          | Out-Null
$homeDir = Join-Path $WorkDir 'home'
$tmpDir  = Join-Path $WorkDir 'tmp'
$appData = Join-Path $WorkDir 'appdata'
$localApp= Join-Path $WorkDir 'localappdata'
foreach ($d in @($homeDir,$tmpDir,$appData,$localApp)) { New-Item -ItemType Directory -Path $d -Force | Out-Null }
$env:HOME          = $homeDir
$env:USERPROFILE   = $homeDir
$env:TEMP          = $tmpDir
$env:TMP           = $tmpDir
$env:APPDATA       = $appData
$env:LOCALAPPDATA  = $localApp
Note ("- child env: HOME/TEMP/APPDATA under {0}" -f $WorkDir)

# ── Download the previous + candidate Windows APEs (read-only) ───────────────
$prevExe = Join-Path $WorkDir 'hull-prev.com'
$candExe = Join-Path $WorkDir 'hull-cand.com'
Invoke-WebRequest -Uri "https://github.com/$OfficialRepo/releases/download/$PrevTag/hull-cosmo" -OutFile $prevExe
Invoke-WebRequest -Uri "https://github.com/$OfficialRepo/releases/download/$RcTag/hull-cosmo"   -OutFile $candExe
Note "- downloaded previous APE $PrevTag and candidate APE $RcTag"

# Per-phase working copies (each in its own granted dir).
function New-PhaseCopy([string]$name, [string]$src) {
    $d = Join-Path $WorkDir $name; New-Item -ItemType Directory -Path $d -Force | Out-Null
    & icacls $d /grant "${user}:(OI)(CI)M" | Out-Null
    $p = Join-Path $d 'hull.com'; Copy-Item $src $p
    return $p
}
$oldHull  = New-PhaseCopy 'oldfail'    $prevExe   # v0.13.0: update MUST fail
$candHull = New-PhaseCopy 'selfupdate' $candExe   # candidate --force: MUST succeed
$aclHull  = New-PhaseCopy 'acl'        $candExe   # candidate + ACL: rollback
$dgHull   = New-PhaseCopy 'downgrade'  $candExe   # candidate -> prev
$exHull   = New-PhaseCopy 'extras'     $candExe   # candidate: spaces/cosmocc/ping/...

# ── Run a phase AS the standard user; fold its output into the evidence ──────
function Invoke-AsUser([string]$script, [string[]]$phaseArgs) {
    $base = [IO.Path]::GetFileNameWithoutExtension($script)
    $out  = Join-Path $WorkDir ("phase-$base.out.log")
    $err  = Join-Path $WorkDir ("phase-$base.err.log")
    $a = @('-NoProfile','-ExecutionPolicy','Bypass','-File', (Join-Path $here $script)) + $phaseArgs
    $p = Start-Process -FilePath 'powershell.exe' -Credential $cred -ArgumentList $a `
                       -WorkingDirectory $WorkDir -Wait -PassThru `
                       -RedirectStandardOutput $out -RedirectStandardError $err
    foreach ($f in @($out, $err)) {
        if ((Test-Path $f) -and (Get-Item $f).Length -gt 0) {
            Get-Content $f | ForEach-Object { Note ("    [child] " + $_) }
        }
    }
    return $p.ExitCode
}

$rc = 0
Note "`n--- PHASE: preconditions ---"
if ((Invoke-AsUser 'preconditions.ps1' @('-Evidence',$Evidence)) -ne 0) { $rc = 1 }

Note "`n--- PHASE: old v0.13.0 update MUST FAIL (pre-fix bug) ---"
if ((Invoke-AsUser 'old_update_fails.ps1' @('-Hull',$oldHull,'-Repo',$RcRepo,'-PrevVersion',$PrevVersion,'-Evidence',$Evidence)) -ne 0) { $rc = 1 }

Note "`n--- PHASE: candidate self-update (force-reinstall RC) MUST SUCCEED ---"
if ((Invoke-AsUser 'self_update.ps1' @('-Hull',$candHull,'-Repo',$RcRepo,'-ExpectVersion',$ExpectVersion,'-Evidence',$Evidence)) -ne 0) { $rc = 1 }

Note "`n--- PHASE: candidate rollback via real ACL ---"
# ACL setup runs HERE (as the owner/admin): the standard user lacks WRITE_DAC on
# these runner-admin-owned files, and $env:USERNAME in the child is the inherited
# parent name, so the deny must be applied by the orchestrator against the real
# standard-user principal. Freeze the exe's ACEs so the deny cannot propagate
# onto it (it must stay renamable to <self>.old), then deny the STANDARD USER
# DELETE on any newly-created file (<self>.new) via an inherit-only ACE.
$aclDir   = Split-Path -Parent $aclHull
$fullUser = "$env:COMPUTERNAME\$user"
& icacls $aclHull /inheritance:d | Out-Null
& icacls $aclHull /grant "${fullUser}:(M)" | Out-Null
& icacls $aclDir  /deny  "${fullUser}:(OI)(IO)(DE)" | Out-Null
Note ("- ACL: froze {0} ACEs; denied {1} DELETE on new files in {2}" -f $aclHull, $fullUser, $aclDir)
if ((Invoke-AsUser 'rollback_acl.ps1' @('-Hull',$aclHull,'-Repo',$RcRepo,'-RcVersion',$ExpectVersion,'-Evidence',$Evidence)) -ne 0) { $rc = 1 }
& icacls $aclDir /remove:d "${fullUser}" | Out-Null   # lift the deny for cleanup

Note "`n--- PHASE: extras (spaces / cosmocc / ping / nested / doctor) ---"
if ((Invoke-AsUser 'extras.ps1' @('-Hull',$exHull,'-RcRepo',$RcRepo,'-ExpectVersion',$ExpectVersion,'-Evidence',$Evidence)) -ne 0) { $rc = 1 }

if ($PrevRepo -ne '') {
    Note "`n--- PHASE: downgrade candidate -> previous (staging) ---"
    if ((Invoke-AsUser 'downgrade.ps1' @('-Hull',$dgHull,'-PrevRepo',$PrevRepo,'-PrevVersion',$PrevVersion,'-Evidence',$Evidence)) -ne 0) { $rc = 1 }
} else {
    Note "`n--- PHASE: downgrade SKIPPED (no prev staging repo) ---"
}

Remove-LocalUser -Name $user -ErrorAction SilentlyContinue
Note ("`n## RESULT: {0}" -f ($(if ($rc -eq 0) {'ALL PHASES PASSED'} else {'FAILURE'})))
exit $rc
