<#
  scoop_test.ps1 - install Scoop (per-user, no admin), install Hull from the LOCAL
  manifest, invoke `hull version`, and uninstall, AS a standard (non-admin) user.
  The manifest URL is the immutable official release asset; read-only w.r.t.
  releases and never mutates a bucket.

  Usage: scoop_test.ps1 -ScoopManifest <hull.json> -Evidence <file>
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$ScoopManifest,
    [Parameter(Mandatory = $true)][string]$Evidence
)

$ErrorActionPreference = 'Stop'
function Note($m) { Add-Content -Path $Evidence -Value $m; Write-Host $m }
function Fail($m) { Note ("  FAIL: {0}" -f $m); $script:fail = 1 }
function Cap([scriptblock]$sb) { $ErrorActionPreference = 'Continue'; & $sb 2>&1 }
$script:fail = 0

Note "## Scoop package (as $(whoami))"

# Isolate Scoop under the granted workdir.
$env:SCOOP = Join-Path $env:USERPROFILE 'scoop'
[Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12

# Install Scoop non-interactively (per-user; no admin).
$sout = Cap {
    $installer = (New-Object Net.WebClient).DownloadString('https://get.scoop.sh')
    Invoke-Expression "& { $installer } -RunAsAdmin:$false"
}
Note (($sout | Out-String) -split "`n" | Select-Object -First 8 | ForEach-Object { "    $_" }) 2>$null

$scoopShim = Join-Path $env:SCOOP 'shims\scoop.ps1'
$scoop = Join-Path $env:SCOOP 'shims\scoop.cmd'
if (-not (Test-Path -LiteralPath $scoop)) { Fail "scoop did not install (no shim at $scoop)"; Note "SCOOP: FAIL"; exit 1 }
Note ("- scoop installed: {0}" -f (Cap { & $scoop --version } | Select-Object -First 1))

# Install Hull from the local manifest (downloads the pinned asset + verifies).
$iout = Cap { & $scoop install $ScoopManifest }
Note (($iout | Out-String) -replace '(?m)^', '    ')
if ($LASTEXITCODE -ne 0 -and (($iout | Out-String) -notmatch 'was installed successfully')) { Fail "scoop install returned $LASTEXITCODE" }

# Invoke the `hull` shim.
$hullShim = Join-Path $env:SCOOP 'shims\hull.cmd'
if (-not (Test-Path -LiteralPath $hullShim)) { $hullShim = Join-Path $env:SCOOP 'shims\hull.exe' }
if (Test-Path -LiteralPath $hullShim) {
    $ver = (Cap { & $hullShim version } | Select-Object -First 1)
    Note ("- hull (scoop shim) version: {0}" -f $ver)
    if ($ver -match '[0-9]+\.[0-9]+\.[0-9]+') { Note "- OK: scoop-installed hull runs" } else { Fail "scoop-installed hull did not report a version" }
} else { Fail "scoop did not create a hull shim" }

# Uninstall.
$uout = Cap { & $scoop uninstall hull }
Note (($uout | Out-String) -replace '(?m)^', '    ')
if (Test-Path -LiteralPath $hullShim) { Fail "hull shim remained after uninstall" }
else { Note "- OK: scoop uninstall removed the hull shim" }

if ($script:fail -ne 0) { Note "SCOOP: FAIL"; exit 1 }
Note "SCOOP: OK (install + invoke + uninstall)"
exit 0
