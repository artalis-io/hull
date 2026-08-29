<#
  rollback_acl.ps1 - prove the candidate's self-update ROLLBACK on a REAL Windows
  ACL scenario (no test hooks, no helper processes, no timing races), run AS the
  standard user.

  The ACL is prepared by the orchestrator (run.ps1) BEFORE this phase, because
  changing a DACL needs WRITE_DAC/ownership that the standard user lacks. Setup:
    - inheritance on the running exe is disconnected and it keeps an explicit
      permissive (DELETE-allowing) ACE, so it can be renamed to <self>.old;
    - the directory carries an inherit-only DELETE deny for THIS user, so any
      file created afterwards (<self>.new) cannot be deleted/renamed by us.
  During `hull update --force`:
    - the downloaded <self>.new is written but cannot be renamed (needs DELETE
      on the source, which is denied);
    - the running exe still moves to <self>.old;
    - installing <self>.new at the original path fails via real ACL enforcement;
    - rollback renames <self>.old back to <self>.

  Fail-closed assertions:
    - `hull update` returns FAILURE;
    - it REACHED the mid-swap install failure: the self-update layer printed
      "install failed; rolled back to the previous binary" (NOT an earlier
      download error and NOT "cannot move ... aside", the initial-rename path);
    - <self>.new was created (the download + write step succeeded);
    - the original candidate is restored, runs, and still reports the candidate
      version; no partial candidate/other version was installed.

  Usage: rollback_acl.ps1 -Hull <candidate-exe> -Repo <org/staging-rc> -RcVersion 0.14.0-rc1 -Evidence <file>
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Hull,
    [Parameter(Mandatory=$true)][string]$Repo,
    [Parameter(Mandatory=$true)][string]$RcVersion,
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
$new = "$Hull.new"

Note "## Rollback via real Windows ACL (from the candidate, as $(whoami))"
$pre = (Cap { & $Hull version } | Select-Object -First 1)
Note ("- pre-update version: {0}" -f $pre)
if ($pre -notmatch [regex]::Escape($RcVersion)) { Fail "candidate is not $RcVersion pre-update" }

# The DELETE deny on new files is already in place (prepared by run.ps1).
$out = Cap { & $Hull update --force --repo $Repo }
$code = $LASTEXITCODE
$text = ($out | Out-String)
Note ($text -replace '(?m)^','    ')
Note ("- exit code: {0}" -f $code)

if ($code -eq 0) { Fail "hull update unexpectedly SUCCEEDED under the ACL lock" }
if ($text -match 'rolled back to the previous binary') {
    Note "- evidence: reached the mid-swap install failure -> rolled back"
} else {
    Fail "did not reach the mid-swap install/rollback path (no rollback message)"
}
if ($text -match 'cannot move .* aside')            { Fail "failed at the INITIAL rename, not the install step" }
if ($text -match 'failed to download|returned HTTP'){ Fail "failed during DOWNLOAD, not the install step" }
if (Test-Path $new) { Note "- evidence: <self>.new exists (download+write succeeded past the ACL create/write allowance)" }
else { Fail "<self>.new absent - the ACL install failure was not exercised" }

if (-not (Test-Path $Hull)) { Fail "original candidate path missing after rollback" }
$post = (Cap { & $Hull version } | Select-Object -First 1)
Note ("- post-rollback version: {0}" -f $post)
if ($post -notmatch [regex]::Escape($RcVersion)) { Fail "post-rollback version is not $RcVersion (a partial install landed)" }

if ($script:fail -ne 0) { Note "ROLLBACK-ACL: FAIL"; exit 1 }
Note "ROLLBACK-ACL: OK (real ACL mid-swap install failure -> clean rollback to $RcVersion)"
exit 0
