<#
  old_update_fails.ps1 - the honest "before" half of the self-update contract:
  the OLD v0.13.0 APE cannot self-update on Windows, because its update code
  predates the deferred-swap fix (it renames the new binary over the RUNNING
  .exe, which Windows refuses, with no fallback). Run AS the standard user.

  Fail-closed assertions:
    - `hull update` returns FAILURE;
    - it fails at the atomic replace of the running binary (the pre-fix path):
      the output contains "atomic_write: rename ... failed" and NOT the
      deferred-swap "rolled back" / "self_replace" messages;
    - the original v0.13.0 binary is untouched, runs, and still reports 0.13.0.

  Usage: old_update_fails.ps1 -Hull <exe> -Repo <org/staging-rc> -PrevVersion 0.13.0 -Evidence <file>
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Hull,
    [Parameter(Mandatory=$true)][string]$Repo,
    [Parameter(Mandatory=$true)][string]$PrevVersion,
    [Parameter(Mandatory=$true)][string]$Evidence
)

$ErrorActionPreference = 'Stop'
function Note($m) { Add-Content -Path $Evidence -Value $m; Write-Host $m }
function Fail($m) { Note ("  FAIL: {0}" -f $m); $script:fail = 1 }
# hull writes progress to stdout and errors/logs to stderr; under -EA Stop a
# 2>&1 merge of native stderr becomes a terminating error. The 0.13.0 failure we
# assert here IS printed to stderr, so capture through a helper that locally
# relaxes the preference and merges both streams (else the script would die
# before it can assert the expected failure).
function Cap([scriptblock]$sb) { $ErrorActionPreference = 'Continue'; & $sb 2>&1 }
$script:fail = 0

Note "## Old v0.13.0 self-update MUST FAIL (pre-fix Windows bug)"
$pre = (Cap { & $Hull version } | Select-Object -First 1)
Note ("- pre-update version: {0}" -f $pre)

$out  = Cap { & $Hull update --repo $Repo }
$code = $LASTEXITCODE
$text = ($out | Out-String)
Note (($text) -replace '(?m)^','    ')
Note ("- exit code: {0}" -f $code)

if ($code -eq 0) { Fail "old v0.13.0 hull update unexpectedly SUCCEEDED on Windows" }
if ($text -match 'atomic_write: rename.*failed') {
    Note "- confirmed: failed at the atomic replace of the running binary (the pre-fix path)"
} else {
    Fail "did not fail via the expected atomic-rename path (unexpected failure mode)"
}
if ($text -match 'rolled back|self_replace') { Fail "the old binary appears to have the deferred-swap code (unexpected)" }

# Original untouched.
$post = (Cap { & $Hull version } | Select-Object -First 1)
Note ("- post-attempt version: {0}" -f $post)
if ($post -notmatch [regex]::Escape($PrevVersion)) { Fail "old binary changed / not $PrevVersion after the failed update" }

# Any half-written sidecar must not have replaced the binary.
Remove-Item "$Hull.new" -Force -ErrorAction SilentlyContinue

if ($script:fail -ne 0) { Note "OLD-UPDATE-FAILS: FAIL"; exit 1 }
Note "OLD-UPDATE-FAILS: OK (v0.13.0 cannot self-update on Windows; binary intact - the bug #429 fixes going forward)"
exit 0
