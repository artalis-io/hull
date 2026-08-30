<#
  winget_test.ps1 - validate + install + invoke + uninstall the Hull Winget
  portable package from the LOCAL manifest, AS a standard (non-admin) user. The
  installer URL in the manifest is the immutable official release asset; the test
  is read-only with respect to releases and never submits to any index.

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
function Cap([scriptblock]$sb) { $ErrorActionPreference = 'Continue'; & $sb 2>&1 }
$script:fail = 0

Note "## Winget portable package (as $(whoami))"

# App Installer (winget) is a per-user MSIX; register the machine-provisioned
# package for this fresh standard user so `winget` is available.
try { Add-AppxPackage -RegisterByFamilyName -MainPackage Microsoft.DesktopAppInstaller_8wekyb3d8bbwe -ErrorAction Stop; Note "- registered Microsoft.DesktopAppInstaller for the user" }
catch { Note ("- App Installer register note: {0}" -f $_.Exception.Message) }

$winget = $null
$cmd = Get-Command winget -ErrorAction SilentlyContinue
if ($cmd) { $winget = $cmd.Source }
if (-not $winget) {
    $pkg = Get-AppxPackage -Name Microsoft.DesktopAppInstaller -ErrorAction SilentlyContinue
    Note ("- DesktopAppInstaller: version={0} loc={1}" -f $pkg.Version, $pkg.InstallLocation)
    if ($pkg -and $pkg.InstallLocation) {
        foreach ($n in @('winget.exe', 'AppInstallerCLI.exe')) {
            $c = Join-Path $pkg.InstallLocation $n
            if (Test-Path -LiteralPath $c) { $winget = $c; break }
        }
    }
}
if (-not $winget -and (Test-Path "$env:LOCALAPPDATA\Microsoft\WindowsApps\winget.exe")) { $winget = "$env:LOCALAPPDATA\Microsoft\WindowsApps\winget.exe" }
if (-not $winget) { Fail "winget is not available for this user (App Installer MSIX not reachable in a runas session)"; Note "WINGET: FAIL"; exit 1 }
Note ("- winget: {0} ({1})" -f $winget, (Cap { & $winget --version } | Select-Object -First 1))

$common = @('--accept-source-agreements', '--accept-package-agreements', '--disable-interactivity')

# 1. validate the manifest
$vout = Cap { & $winget validate --manifest $ManifestDir }
Note (($vout | Out-String) -replace '(?m)^', '    ')
if ($LASTEXITCODE -eq 0 -or (($vout | Out-String) -match 'Manifest validation succeeded')) { Note "- OK: winget validate succeeded" }
else { Fail "winget validate failed" }

# 2. install the portable from the local manifest (downloads the pinned asset)
$iout = Cap { & $winget install --manifest $ManifestDir @common }
Note (($iout | Out-String) -replace '(?m)^', '    ')
if ($LASTEXITCODE -ne 0) { Fail "winget install returned $LASTEXITCODE" }

# 3. invoke the `hull` alias (the WinGet Links dir is on the user PATH; invoke
#    the alias directly since this process's PATH may predate the install).
$alias = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links\hull.exe'
if (-not (Test-Path -LiteralPath $alias)) { $alias = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links\hull.cmd' }
if (Test-Path -LiteralPath $alias) {
    $ver = (Cap { & $alias version } | Select-Object -First 1)
    Note ("- hull (winget alias) version: {0}" -f $ver)
    if ($ver -match '[0-9]+\.[0-9]+\.[0-9]+') { Note "- OK: winget-installed hull runs" } else { Fail "winget-installed hull did not report a version" }
} else { Fail "winget did not create a hull alias in the Links dir" }

# 4. uninstall
$uout = Cap { & $winget uninstall Artalis.Hull --disable-interactivity }
Note (($uout | Out-String) -replace '(?m)^', '    ')
if ($LASTEXITCODE -ne 0) { Fail "winget uninstall returned $LASTEXITCODE" }
elseif (Test-Path -LiteralPath $alias) { Fail "hull alias remained after uninstall" }
else { Note "- OK: winget uninstall removed the hull alias" }

if ($script:fail -ne 0) { Note "WINGET: FAIL"; exit 1 }
Note "WINGET: OK (validate + install + invoke + uninstall)"
exit 0
