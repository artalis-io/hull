<#
  downgrade.ps1 - the running CANDIDATE downgrades to the PREVIOUS release,
  proving target-version independence of the (fixed) updater. Run AS the standard
  user. `hull update` only advances by default, so downgrade uses `--force`
  against a staging repo whose latest mirrors the previous signed release.

  Fail-closed assertions:
    - the forced down-update returns success;
    - the installed previous binary RUNS and reports the previous version.

  Expected pre-fix limitation (recorded, NOT failed): the candidate's deferred
  swap installs the previous binary and leaves <self>.old (the aside candidate,
  which was locked while the updating process ran). The NEXT startup would
  normally sweep it - but the newly-installed PREVIOUS binary predates the
  startup sweeper, so <self>.old persists. We verify the previous binary runs,
  then clean the residue explicitly and record this as expected.

  Usage: downgrade.ps1 -Hull <candidate-exe> -PrevRepo <org/staging-prev> -PrevVersion 0.13.0 -Evidence <file>
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Hull,
    [Parameter(Mandatory=$true)][string]$PrevRepo,
    [Parameter(Mandatory=$true)][string]$PrevVersion,
    [Parameter(Mandatory=$true)][string]$Evidence
)

$ErrorActionPreference = 'Stop'
function Note($m) { Add-Content -Path $Evidence -Value $m; Write-Host $m }
function Fail($m) { Note ("  FAIL: {0}" -f $m); $script:fail = 1 }
# hull writes progress to stdout and errors/logs to stderr; under -EA Stop a
# 2>&1 merge of native stderr becomes a terminating error. Capture through a
# helper that locally relaxes the preference and merges both streams.
function Cap([scriptblock]$sb) { $ErrorActionPreference = 'Continue'; & $sb 2>&1 }
$script:fail = 0
$new = "$Hull.new"; $old = "$Hull.old"

Note "## Downgrade: candidate -> previous (staging, forced)"
$out = Cap { & $Hull update --force --repo $PrevRepo }
$code = $LASTEXITCODE
Note (($out | Out-String) -replace '(?m)^','    ')
Note ("- exit code: {0}" -f $code)
if ($code -ne 0) { Fail "forced downgrade returned non-zero" }

$down = (Cap { & $Hull version } | Select-Object -First 1)
Note ("- installed version now runs as: {0}" -f $down)
if ($down -notmatch [regex]::Escape($PrevVersion)) { Fail "downgrade did not install a runnable $PrevVersion" }

if (Test-Path $new) { Fail "<self>.new residue after downgrade" }

# Expected pre-fix limitation: the installed previous binary has no startup
# sweeper, so <self>.old is NOT auto-cleaned. Record it and clean explicitly.
if (Test-Path $old) {
    Note ("- expected pre-fix limitation: <self>.old remains ({0} has no startup sweeper); cleaning explicitly" -f $PrevVersion)
    Remove-Item $old -Force -ErrorAction SilentlyContinue
    if (Test-Path $old) { Note "  (note: explicit cleanup could not remove it either)" }
} else {
    Note "- no <self>.old residue"
}

if ($script:fail -ne 0) { Note "DOWNGRADE: FAIL"; exit 1 }
Note "DOWNGRADE: OK (candidate installed a runnable $PrevVersion; .old residue is an expected pre-fix limitation)"
exit 0
