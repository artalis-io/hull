<#
  winget_validate.ps1 - validate the local Winget manifest AS the fresh standard
  (non-admin) user. `winget validate` parses the manifest schema locally; it is
  the check that runs under a true standard user (the install/invoke/uninstall
  path needs App Installer package identity, which a runas standard user lacks on
  the GitHub image - see winget_test.ps1 + packaging/windows/README.md).

  Usage: winget_validate.ps1 -ManifestDir <dir> -Evidence <file>
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ManifestDir,
    [Parameter(Mandatory = $true)][string]$Evidence
)

$ErrorActionPreference = 'Stop'
function Note($m) { Add-Content -Path $Evidence -Value $m; Write-Host $m }
function Fail($m) { Note ("  FAIL: {0}" -f $m); $script:fail = 1 }
# File-redirected native invocation (avoids the PS7 `& native 2>&1`
# "StandardOutputEncoding" error for a packaged binary run outside its alias).
function Invoke-Native([string]$Exe, [string[]]$Args, [int]$TimeoutSec = 300) {
    $o = [System.IO.Path]::GetTempFileName(); $e = [System.IO.Path]::GetTempFileName()
    try {
        $p = Start-Process -FilePath $Exe -ArgumentList $Args -NoNewWindow -PassThru -RedirectStandardOutput $o -RedirectStandardError $e
        if (-not $p.WaitForExit($TimeoutSec * 1000)) { try { $p.Kill() } catch { }; return @{ Code = $null; Out = ''; TimedOut = $true } }
        return @{ Code = $p.ExitCode; Out = ((Get-Content -LiteralPath $o -Raw -EA SilentlyContinue) + "`n" + (Get-Content -LiteralPath $e -Raw -EA SilentlyContinue)); TimedOut = $false }
    } finally { Remove-Item -LiteralPath $o, $e -Force -EA SilentlyContinue }
}
$script:fail = 0

Note "## Winget manifest validation (as $(whoami), a true standard user)"

# Register App Installer for this user so winget.exe is present, then resolve the
# raw package binary (a standard user has no `winget` alias on PATH).
try { Get-AppxPackage -Name Microsoft.DesktopAppInstaller | ForEach-Object { Add-AppxPackage -DisableDevelopmentMode -Register (Join-Path $_.InstallLocation 'AppXManifest.xml') -EA SilentlyContinue } } catch { }
$pkg = Get-AppxPackage -Name Microsoft.DesktopAppInstaller | Select-Object -First 1
$winget = $null
if ($pkg) {
    foreach ($n in @('winget.exe', 'AppInstallerCLI.exe')) {
        $cand = Join-Path $pkg.InstallLocation $n
        if (Test-Path -LiteralPath $cand) { $winget = $cand; break }
    }
}
if (-not $winget) { $cmd = Get-Command winget -ErrorAction SilentlyContinue; if ($cmd) { $winget = $cmd.Source } }
if (-not $winget) { Fail "winget.exe not resolvable for the standard user"; Note "WINGET-VALIDATE: FAIL"; exit 1 }
Note ("- winget.exe: {0}" -f $winget)

$v = Invoke-Native $winget @('validate', '--manifest', $ManifestDir)
Note (($v.Out) -replace '(?m)^', '    ')
if ($v.TimedOut) { Fail "winget validate timed out" }
elseif ($v.Code -eq 0 -or $v.Out -match 'Manifest validation succeeded') { Note "- OK: winget validate succeeded (standard user)" }
else { Fail "winget validate failed (code $($v.Code))" }

if ($script:fail -ne 0) { Note "WINGET-VALIDATE: FAIL"; exit 1 }
Note "WINGET-VALIDATE: OK (manifest validated as a standard user)"
exit 0
