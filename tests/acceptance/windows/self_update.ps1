<#
  self_update.ps1 - the "after" half: the CANDIDATE (which HAS the deferred-swap
  fix) self-updates successfully on Windows. Force-reinstalls the RC over itself
  (a running .exe), exercising the exact deferred rename-aside swap. Run AS the
  standard user.

  Fail-closed assertions:
    - `hull update --force --repo=<rc-staging>` returns success;
    - the updating process exits normally (exit 0, no crash);
    - the next invocation reports the candidate version;
    - the residue is cleaned: no <self>.new, and <self>.old is swept on the next
      startup (the candidate carries the startup sweeper).

  Usage: self_update.ps1 -Hull <candidate-exe> -Repo <org/staging-rc> -ExpectVersion 0.14.0-rc1 -Evidence <file>
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Hull,
    [Parameter(Mandatory=$true)][string]$Repo,
    [Parameter(Mandatory=$true)][string]$ExpectVersion,
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

Note "## Candidate self-update via the deferred swap (force-reinstall RC)"
$pre = (Cap { & $Hull version } | Select-Object -First 1)
Note ("- pre-update version: {0}" -f $pre)
if ($pre -notmatch [regex]::Escape($ExpectVersion)) { Fail "candidate does not report $ExpectVersion pre-update" }

$out  = Cap { & $Hull update --force --repo $Repo }
$code = $LASTEXITCODE
Note (($out | Out-String) -replace '(?m)^','    ')
Note ("- exit code: {0}" -f $code)
if ($code -ne 0) { Fail "candidate ``hull update --force`` returned non-zero" }

$post = (Cap { & $Hull version } | Select-Object -First 1)
Note ("- post-update version: {0}" -f $post)
if ($post -notmatch [regex]::Escape($ExpectVersion)) { Fail "post-update version is not $ExpectVersion" }

if (Test-Path $new) { Fail "<self>.new residue remains after a successful update" }
if (Test-Path $old) {
    Cap { & $Hull version } | Out-Null   # next startup: the sweeper (present in the candidate) removes .old
    if (Test-Path $old) { Fail "<self>.old was not swept on the next startup" }
    else { Note "- <self>.old swept on the next startup (candidate sweeper)" }
} else {
    Note "- no <self>.old residue"
}

if ($script:fail -ne 0) { Note "SELF-UPDATE: FAIL"; exit 1 }
Note "SELF-UPDATE: OK (candidate force-reinstalled $ExpectVersion via the deferred swap; residue clean)"
exit 0
