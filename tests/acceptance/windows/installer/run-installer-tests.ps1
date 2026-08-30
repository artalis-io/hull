<#
  run-installer-tests.ps1 - orchestrates the installer-specific Windows tests.
  Runs as the runner's admin account but drives the tests AS a freshly-created
  STANDARD (non-admin) user with Developer Mode disabled, under BOTH Windows
  PowerShell 5.1 and PowerShell 7. Read-only w.r.t. GitHub releases.

  Usage: run-installer-tests.ps1 -RepoRoot <dir> -Evidence <file> -WorkDir <dir>
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
Set-Content -Path $Evidence -Value "# Windows installer test evidence`n"

# ── Standard (non-admin) user + Developer-Mode-off ───────────────────────────
$user = 'hullinst'
$pw   = ([guid]::NewGuid().ToString() + 'Aa1!')
$sec  = ConvertTo-SecureString $pw -AsPlainText -Force
if (Get-LocalUser -Name $user -ErrorAction SilentlyContinue) { Remove-LocalUser -Name $user }
New-LocalUser -Name $user -Password $sec -AccountNeverExpires -PasswordNeverExpires | Out-Null
Add-LocalGroupMember -Group 'Users' -Member $user -ErrorAction SilentlyContinue
if (Get-LocalGroupMember -Group 'Administrators' -Member $user -ErrorAction SilentlyContinue) {
    Note "FATAL: installer test user is in Administrators"; exit 1
}
$cred = New-Object System.Management.Automation.PSCredential("$env:COMPUTERNAME\$user", $sec)
Note "- created standard user $user (not in Administrators)"

$devKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock'
New-Item -Path $devKey -Force | Out-Null
Set-ItemProperty -Path $devKey -Name AllowDevelopmentWithoutDevLicense -Value 0 -Type DWord
Note "- Developer Mode disabled"

# ── Stage the files the child reads + a user-writable environment ────────────
Copy-Item (Join-Path $RepoRoot 'install.ps1') (Join-Path $WorkDir 'install.ps1') -Force
Copy-Item (Join-Path $here 'installer_tests.ps1') (Join-Path $WorkDir 'installer_tests.ps1') -Force
Copy-Item (Join-Path (Split-Path -Parent $here) 'preconditions.ps1') (Join-Path $WorkDir 'preconditions.ps1') -Force

& icacls $WorkDir  /grant "${user}:(OI)(CI)M"  | Out-Null
& icacls $Evidence /grant "${user}:M"          | Out-Null
$homeDir = Join-Path $WorkDir 'home'; $tmpDir = Join-Path $WorkDir 'tmp'
$appData = Join-Path $WorkDir 'appdata'; $localApp = Join-Path $WorkDir 'localappdata'
foreach ($d in @($homeDir, $tmpDir, $appData, $localApp)) { New-Item -ItemType Directory -Path $d -Force | Out-Null }
$env:HOME = $homeDir; $env:USERPROFILE = $homeDir
$env:TEMP = $tmpDir;  $env:TMP = $tmpDir
$env:APPDATA = $appData; $env:LOCALAPPDATA = $localApp
Note ("- child env: HOME/TEMP/LOCALAPPDATA under {0}" -f $WorkDir)

$pwsh7 = Join-Path $PSHOME 'pwsh.exe'
if (-not (Test-Path $pwsh7)) { $pwsh7 = 'pwsh' }
$ps51  = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$ps51Modules = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\Modules'

# ── Run a phase AS the standard user under a chosen shell ────────────────────
function Invoke-AsUser([string]$ShellExe, [string]$Script, [string[]]$PhaseArgs, [string]$Label, [string]$ModulePath) {
    $o = Join-Path $WorkDir ("phase-$Label.out.log"); $e = Join-Path $WorkDir ("phase-$Label.err.log")
    $a = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $WorkDir $Script)) + $PhaseArgs
    $restore = $env:PSModulePath
    if ($ModulePath) { $env:PSModulePath = $ModulePath }   # correct WinPS 5.1 module resolution for the runas child
    try {
        $p = Start-Process -FilePath $ShellExe -Credential $cred -ArgumentList $a `
                           -WorkingDirectory $WorkDir -Wait -PassThru `
                           -RedirectStandardOutput $o -RedirectStandardError $e
    } finally { $env:PSModulePath = $restore }
    foreach ($f in @($o, $e)) {
        if ((Test-Path $f) -and (Get-Item $f).Length -gt 0) {
            Get-Content $f | ForEach-Object { Note ("    [$Label] " + $_) }
        }
    }
    return $p.ExitCode
}

$installPs1Path = Join-Path $WorkDir 'install.ps1'
$rc = 0

Note "`n--- PHASE: preconditions (non-admin, Dev-Mode off, symlink denied) ---"
if ((Invoke-AsUser $pwsh7 'preconditions.ps1' @('-Evidence', $Evidence) 'precond' $null) -ne 0) { $rc = 1 }

Note "`n--- PHASE: installer tests under PowerShell 7 (with network) ---"
if ((Invoke-AsUser $pwsh7 'installer_tests.ps1' @('-InstallPs1', $installPs1Path, '-IncludeNetwork', '-Evidence', $Evidence) 'ps7' $null) -ne 0) { $rc = 1 }

if (Test-Path $ps51) {
    Note "`n--- PHASE: installer tests under Windows PowerShell 5.1 (with network) ---"
    if ((Invoke-AsUser $ps51 'installer_tests.ps1' @('-InstallPs1', $installPs1Path, '-IncludeNetwork', '-Evidence', $Evidence) 'ps51' $ps51Modules) -ne 0) { $rc = 1 }
} else {
    Note "`n--- PHASE: Windows PowerShell 5.1 not present; SKIPPED ---"
    $rc = 1   # 5.1 must be available on windows-latest; fail if it is not
}

Remove-LocalUser -Name $user -ErrorAction SilentlyContinue
Note ("`n## RESULT: {0}" -f ($(if ($rc -eq 0) { 'ALL INSTALLER TESTS PASSED' } else { 'FAILURE' })))
exit $rc
