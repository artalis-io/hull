<#
  downgrade.ps1 - prove a real Windows DOWNGRADE via a staging repo, run AS the
  standard user. `hull update` only reads /releases/latest and only advances by
  default, so downgrade uses `--force` against a staging repo whose latest
  mirrors the PREVIOUS signed release.

  Starts from a fresh previous-release APE, updates UP to the candidate (via the
  RC staging repo), then FORCE-updates back DOWN to the previous release (via the
  previous staging repo). Fail-closed assertions:
    - the up-update reports the candidate version;
    - the forced down-update returns success and reports the previous version;
    - no <self>.new residue; <self>.old is swept on the next startup.

  Usage: downgrade.ps1 -Hull <exe> -RcRepo <org/staging-rc> -PrevRepo <org/staging-prev> \
                       -RcVersion 0.14.0-rc1 -PrevVersion 0.13.0 -Evidence <file>
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Hull,
    [Parameter(Mandatory=$true)][string]$RcRepo,
    [Parameter(Mandatory=$true)][string]$PrevRepo,
    [Parameter(Mandatory=$true)][string]$RcVersion,
    [Parameter(Mandatory=$true)][string]$PrevVersion,
    [Parameter(Mandatory=$true)][string]$Evidence
)

$ErrorActionPreference = 'Stop'
function Note($m) { Add-Content -Path $Evidence -Value $m; Write-Host $m }
function Fail($m) { Note ("  FAIL: {0}" -f $m); $script:fail = 1 }
$script:fail = 0
$new = "$Hull.new"; $old = "$Hull.old"

Note "## Downgrade (staging-repo based)"
# Up to the candidate first.
& $Hull update --repo $RcRepo *> $null
$up = (& $Hull version 2>&1 | Select-Object -First 1)
Note ("- after up-update: {0}" -f $up)
if ($up -notmatch [regex]::Escape($RcVersion)) { Fail "up-update did not reach $RcVersion"; }

# Forced downgrade to the previous release.
$out = & $Hull update --force --repo $PrevRepo 2>&1; $code = $LASTEXITCODE
Note (($out | Out-String) -replace '(?m)^','    ')
if ($code -ne 0) { Fail "forced downgrade returned non-zero" }
$down = (& $Hull version 2>&1 | Select-Object -First 1)
Note ("- after forced down-update: {0}" -f $down)
if ($down -notmatch [regex]::Escape($PrevVersion)) { Fail "downgrade did not reach $PrevVersion" }

if (Test-Path $new) { Fail "<self>.new residue after downgrade" }
& $Hull version *> $null
if (Test-Path $old) { Fail "<self>.old not swept after downgrade" }

if ($script:fail -ne 0) { Note "DOWNGRADE: FAIL"; exit 1 }
Note "DOWNGRADE: OK ($RcVersion -> $PrevVersion via forced staging update)"
exit 0
