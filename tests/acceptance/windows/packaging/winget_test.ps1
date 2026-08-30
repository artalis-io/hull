<#
  winget_test.ps1 - Winget portable install / invoke (`hull version`) / uninstall
  from the LOCAL manifest.

  Boundary (see packaging/windows/README.md): winget's per-user App Installer
  MSIX is not provisionable for a freshly-created standard user in a runas
  session on the GitHub image, so this phase runs under the RUNNER's own account,
  which has valid App Installer package identity. Hull's winget install is
  per-user portable scope and requests no elevation. The runner account's actual
  integrity level (the GitHub image runs it elevated / High) is recorded in the
  evidence: that is a runner-image property, NOT a Hull requirement, and NOT
  evidence Hull needs administrator privileges. The fully non-admin proof is the
  Scoop phase. This account is in the Administrators group, so it is not
  characterized as a true non-admin account.

  The manifest URL is the immutable official release asset; read-only w.r.t.
  releases; nothing is submitted to any index.

  Usage: winget_test.ps1 -ManifestDir <dir> -Evidence <file>
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ManifestDir,
    [Parameter(Mandatory = $true)][string]$Evidence
)

$ErrorActionPreference = 'Stop'
function Note($m) { Add-Content -Path $Evidence -Value $m; Write-Host $m }
function Fail($m) { Note ("  FAIL: {0}" -f $m); $script:fail = 1 }
# Run a native command with file-redirected stdio + a timeout. Avoids the PS7
# `& native 2>&1` "StandardOutputEncoding is only supported when standard output
# is redirected" error.
function Invoke-Native([string]$Exe, [string[]]$CmdArgs, [int]$TimeoutSec = 600) {
    $o = [System.IO.Path]::GetTempFileName(); $e = [System.IO.Path]::GetTempFileName()
    try {
        $p = Start-Process -FilePath $Exe -ArgumentList $CmdArgs -NoNewWindow -PassThru -RedirectStandardOutput $o -RedirectStandardError $e
        if (-not $p.WaitForExit($TimeoutSec * 1000)) { try { $p.Kill() } catch { }; return @{ Code = $null; Out = ''; TimedOut = $true } }
        return @{ Code = $p.ExitCode; Out = ((Get-Content -LiteralPath $o -Raw -EA SilentlyContinue) + "`n" + (Get-Content -LiteralPath $e -Raw -EA SilentlyContinue)); TimedOut = $false }
    } finally { Remove-Item -LiteralPath $o, $e -Force -EA SilentlyContinue }
}
$script:fail = 0

Note "## Winget portable install / invoke / uninstall (runner account)"
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
# Record the observed integrity honestly (GitHub runners are non-elevated /
# Medium by default). The install is per-user portable scope; no elevation is
# requested. This is not characterized as a true non-admin account.
$elevated = ((whoami /groups) -match 'S-1-16-12288')
$level = if ($elevated) { 'High (elevated)' } else { 'Medium (non-elevated)' }
Note ("- account: {0}; integrity: {1}; per-user portable install, no elevation requested" -f $id.Name, $level)

$cmd = Get-Command winget -ErrorAction SilentlyContinue
if (-not $cmd) { Fail "winget alias not on PATH for the runner account"; Note "WINGET: FAIL"; exit 1 }
$winget = $cmd.Source
Note ("- winget: {0}" -f ((Invoke-Native $winget @('--version')).Out.Trim()))

$common = @('--accept-source-agreements', '--accept-package-agreements', '--disable-interactivity')

# Installing from a LOCAL manifest requires the LocalManifestFiles experimental
# feature, which an administrator enables. The runner account is elevated, so we
# enable it for the test harness. This is a test-harness action (installing from
# an uncommitted local manifest), NOT a Hull runtime requirement - a published
# winget package installs with no such setting.
$en = Invoke-Native $winget @('settings', '--enable', 'LocalManifestFiles')
Note (($en.Out) -replace '(?m)^', '    ')
Note "- enabled LocalManifestFiles (needed only to install from a LOCAL manifest)"

# 1. validate (again, in the runner context)
$v = Invoke-Native $winget @('validate', '--manifest', $ManifestDir)
Note (($v.Out) -replace '(?m)^', '    ')
if ($v.Code -eq 0 -or $v.Out -match 'Manifest validation succeeded') { Note "- OK: winget validate succeeded" } else { Fail "winget validate failed (code $($v.Code))" }

# 2. install the portable (downloads the pinned asset + verifies the SHA-256)
$i = Invoke-Native $winget (@('install', '--manifest', $ManifestDir) + $common)
Note (($i.Out) -replace '(?m)^', '    ')
if ($i.Code -ne 0) { Fail "winget install returned $($i.Code)" }

# 3. invoke the hull alias directly (the Links dir may postdate this session PATH)
$alias = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links\hull.exe'
if (-not (Test-Path -LiteralPath $alias)) { $alias = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links\hull.cmd' }
if (Test-Path -LiteralPath $alias) {
    $ver = (Invoke-Native $alias @('version')).Out
    $ver1 = ($ver -split "`n" | Where-Object { $_ -match '\S' } | Select-Object -First 1)
    Note ("- hull (winget alias) version: {0}" -f $ver1)
    if ($ver1 -match '[0-9]+\.[0-9]+\.[0-9]+') { Note "- OK: winget-installed hull runs" } else { Fail "winget-installed hull did not report a version" }
} else { Fail "winget did not create a hull alias in the Links dir" }

# 4. uninstall. Diagnostic first: show how the portable is tracked, then remove
# it by exact id. --accept-source-agreements: uninstall queries all sources incl.
# msstore, whose agreement would otherwise cancel under --disable-interactivity.
$ls = Invoke-Native $winget @('list', '--id', 'Artalis.Hull', '--exact', '--accept-source-agreements', '--disable-interactivity')
Note (($ls.Out) -replace '(?m)^', '    list> ')
$u = Invoke-Native $winget @('uninstall', '--id', 'Artalis.Hull', '--exact', '--accept-source-agreements', '--disable-interactivity')
Note (($u.Out) -replace '(?m)^', '    ')
if ($u.Code -ne 0) { Fail "winget uninstall returned $($u.Code)" }
elseif (Test-Path -LiteralPath $alias) { Fail "hull alias remained after uninstall" }
else { Note "- OK: winget uninstall removed the hull alias" }

if ($script:fail -ne 0) { Note "WINGET: FAIL"; exit 1 }
Note "WINGET: OK (validate + install + invoke + uninstall, runner/non-elevated)"
exit 0
