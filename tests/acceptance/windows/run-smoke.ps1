<#
  run-smoke.ps1 - orchestrates the minimal Windows artifact smoke. Runs as the
  runner's admin account but drives verification + the cosmocc build AS a
  freshly-created STANDARD (non-admin) user with Developer Mode disabled, so the
  non-admin evidence is meaningful. It ONLY downloads the published artifact +
  its manifest + signature from the official release; it never updates, rolls
  back, mutates staging, or touches any release/asset.

  The standard-user smoke (smoke.ps1) checksums the downloaded bytes BEFORE
  executing them, then verifies the signature + reported version, and only then
  installs cosmocc and builds+serves the /ping fixtures. This orchestrator never
  executes the downloaded binary itself, so the bytes stay pristine until the
  checksum.

  Usage:
    run-smoke.ps1 -OfficialRepo artalis-io/hull -ReleaseTag v0.14.0 \
                  -ExpectVersion 0.14.0 -Pubkey <hex> -Evidence <file> -WorkDir <dir>
#>
[CmdletBinding()]
param(
    [string]$OfficialRepo  = 'artalis-io/hull',
    [string]$ReleaseTag    = '',
    [string]$ExpectVersion = '',
    [string]$Pubkey        = '',
    [Parameter(Mandatory=$true)][string]$Evidence,
    [Parameter(Mandatory=$true)][string]$WorkDir
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
function Note($m) { Add-Content -Path $Evidence -Value $m; Write-Host $m }

New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
Set-Content -Path $Evidence -Value "# Windows artifact smoke evidence`n"

# Fail closed on missing inputs (never silently target "latest").
if (-not $ReleaseTag)    { Note 'FATAL: -ReleaseTag is required (no silent latest)'; exit 2 }
if (-not $ExpectVersion) { Note 'FATAL: -ExpectVersion is required'; exit 2 }
Note ("- repo={0} tag={1} expect={2}" -f $OfficialRepo, $ReleaseTag, $ExpectVersion)

# ── Download the published artifact + manifest + signature (official only) ────
$exe = Join-Path $WorkDir 'hull-cosmo'
$man = Join-Path $WorkDir 'hull.sha256'
$sig = Join-Path $WorkDir 'hull.sha256.sig'
$base = "https://github.com/$OfficialRepo/releases/download/$ReleaseTag"
Invoke-WebRequest -Uri "$base/hull-cosmo"      -OutFile $exe
Invoke-WebRequest -Uri "$base/hull.sha256"     -OutFile $man
Invoke-WebRequest -Uri "$base/hull.sha256.sig" -OutFile $sig
Note "- downloaded hull-cosmo + hull.sha256 + hull.sha256.sig from the official release"

# ── Standard (non-admin) user + Developer-Mode-off environment ────────────────
$user = 'hullsmoke'
$pw   = ([guid]::NewGuid().ToString() + 'Aa1!')
$sec  = ConvertTo-SecureString $pw -AsPlainText -Force
if (Get-LocalUser -Name $user -ErrorAction SilentlyContinue) { Remove-LocalUser -Name $user }
New-LocalUser -Name $user -Password $sec -AccountNeverExpires -PasswordNeverExpires | Out-Null
Add-LocalGroupMember -Group 'Users' -Member $user -ErrorAction SilentlyContinue
if (Get-LocalGroupMember -Group 'Administrators' -Member $user -ErrorAction SilentlyContinue) {
    Note "FATAL: smoke user is in Administrators"; exit 1
}
$cred = New-Object System.Management.Automation.PSCredential("$env:COMPUTERNAME\$user", $sec)
Note "- created standard user $user (not in Administrators)"

$devKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock'
New-Item -Path $devKey -Force | Out-Null
Set-ItemProperty -Path $devKey -Name AllowDevelopmentWithoutDevLicense -Value 0 -Type DWord
Note "- Developer Mode disabled (AllowDevelopmentWithoutDevLicense=0)"

# The runas child loads no profile and otherwise inherits the runner-admin's
# unwritable HOME/TEMP/APPDATA; redirect them at granted subdirs of the workdir.
& icacls $WorkDir  /grant "${user}:(OI)(CI)M"  | Out-Null
& icacls $here     /grant "${user}:(OI)(CI)RX" | Out-Null
& icacls $Evidence /grant "${user}:M"          | Out-Null
$homeDir = Join-Path $WorkDir 'home'; $tmpDir = Join-Path $WorkDir 'tmp'
$appData = Join-Path $WorkDir 'appdata'; $localApp = Join-Path $WorkDir 'localappdata'
foreach ($d in @($homeDir,$tmpDir,$appData,$localApp)) { New-Item -ItemType Directory -Path $d -Force | Out-Null }
$env:HOME = $homeDir; $env:USERPROFILE = $homeDir
$env:TEMP = $tmpDir;  $env:TMP = $tmpDir
$env:APPDATA = $appData; $env:LOCALAPPDATA = $localApp
Note ("- child env: HOME/TEMP/APPDATA under {0}" -f $WorkDir)

# ── Run a phase AS the standard user; fold its output into the evidence ───────
function Invoke-AsUser([string]$script, [string[]]$phaseArgs) {
    $b = [IO.Path]::GetFileNameWithoutExtension($script)
    $o = Join-Path $WorkDir ("phase-$b.out.log"); $e = Join-Path $WorkDir ("phase-$b.err.log")
    $a = @('-NoProfile','-ExecutionPolicy','Bypass','-File', (Join-Path $here $script)) + $phaseArgs
    $p = Start-Process -FilePath 'powershell.exe' -Credential $cred -ArgumentList $a `
                       -WorkingDirectory $WorkDir -Wait -PassThru `
                       -RedirectStandardOutput $o -RedirectStandardError $e
    foreach ($f in @($o, $e)) {
        if ((Test-Path $f) -and (Get-Item $f).Length -gt 0) {
            Get-Content $f | ForEach-Object { Note ("    [child] " + $_) }
        }
    }
    return $p.ExitCode
}

$rc = 0
Note "`n--- PHASE: preconditions ---"
if ((Invoke-AsUser 'preconditions.ps1' @('-Evidence',$Evidence)) -ne 0) { $rc = 1 }

Note "`n--- PHASE: artifact smoke (verify -> cosmocc -> /ping) ---"
$smokeArgs = @('-Hull',$exe,'-Manifest',$man,'-Sig',$sig,'-Pubkey',$Pubkey,
               '-ExpectVersion',$ExpectVersion,'-Evidence',$Evidence)
if ((Invoke-AsUser 'smoke.ps1' $smokeArgs) -ne 0) { $rc = 1 }

Remove-LocalUser -Name $user -ErrorAction SilentlyContinue
Note ("`n## RESULT: {0}" -f ($(if ($rc -eq 0) {'ALL SMOKE CHECKS PASSED'} else {'FAILURE'})))
exit $rc
