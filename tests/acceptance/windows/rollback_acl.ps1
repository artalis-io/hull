<#
  rollback_acl.ps1 - Prove the self-update ROLLBACK on a REAL Windows ACL
  scenario (no test hooks, no helper processes, no timing races), run AS the
  standard user.

  Setup: the running hull exe sits in a dedicated directory. We deny DELETE to
  the user on NEWLY-created files via inherit-only ACEs, while leaving the
  existing exe's own ACL permissive. Consequences during `hull update`:

    - the downloaded <self>.new can be WRITTEN (create/write still allowed)
      but cannot be renamed (a rename needs DELETE on the source);
    - the running exe still moves to <self>.old (its own ACL permits delete);
    - installing <self>.new at the original path fails via real ACL enforcement;
    - rollback renames <self>.old back to <self> (it kept the permissive ACL).

  Fail-closed assertions:
    - `hull update` returns FAILURE;
    - it reached the MID-SWAP install failure - the self-update layer printed
      "install failed; rolled back to the previous binary" (NOT an earlier
      download error and NOT "cannot move ... aside", the initial-rename path);
    - <self>.new was created (evidence the download + write step succeeded);
    - the original path still exists, runs, and reports the previous version;
    - no usable candidate was installed (version did not advance).

  Usage: rollback_acl.ps1 -Hull <exe> -Repo <org/staging-rc> -PrevVersion 0.13.0 -Evidence <file>
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
$script:fail = 0

$dir  = Split-Path -Parent $Hull
$new  = "$Hull.new"
$old  = "$Hull.old"
$me   = "$env:USERDOMAIN\$env:USERNAME"

Note "## Rollback via real Windows ACL"
Note ("- hull: {0}" -f $Hull)

# Pre-state: the candidate binary reports the previous version.
$pre = (& $Hull version 2>&1 | Select-Object -First 1)
Note ("- pre-update version: {0}" -f $pre)
if ($pre -notmatch [regex]::Escape($PrevVersion)) { Fail "pre-update version is not $PrevVersion" }

# Deny DELETE to the user on NEWLY-created files (inherit-only): (OI) object
# inherit, (IO) inherit-only so the dir itself is unaffected, (DE) delete. The
# existing exe predates this ACE and keeps its permissive ACL.
& icacls $dir /deny "${me}:(OI)(IO)(DE)" | Out-Null
Note ("- applied inherit-only DELETE deny for {0} on new files in {1}" -f $me, $dir)

# Attempt the self-update. It should fail and roll back.
$out = & $Hull update --repo $Repo 2>&1
$code = $LASTEXITCODE
$text = ($out | Out-String)
Note "- hull update output:"
Note ($text -replace '(?m)^', '    ')
Note ("- hull update exit code: {0}" -f $code)

# Remove the deny so post-run cleanup can delete any residue.
& icacls $dir /remove:d "${me}" | Out-Null

if ($code -eq 0) { Fail "hull update unexpectedly SUCCEEDED under the ACL lock" }

# Evidence: reached the mid-swap install failure (rollback), not an earlier stage.
if ($text -match 'rolled back to the previous binary') {
    Note "- evidence: reached mid-swap install failure -> rolled back"
} else {
    Fail "did not reach the mid-swap install/rollback path (no rollback message)"
}
if ($text -match 'cannot move .* aside') { Fail "failed at the INITIAL rename, not the install step" }
if ($text -match 'failed to download|returned HTTP') { Fail "failed during DOWNLOAD, not the install step" }

# Evidence the download + write step succeeded: <self>.new was created.
if (Test-Path $new) { Note "- evidence: <self>.new exists (download+write succeeded past the ACL create/write allowance)" }
else { Fail "<self>.new absent - download/write did not complete, so the ACL install failure was not exercised" }

# Original restored, runnable, still the previous version.
if (-not (Test-Path $Hull)) { Fail "original hull path is missing after rollback" }
$post = (& $Hull version 2>&1 | Select-Object -First 1)
Note ("- post-rollback version: {0}" -f $post)
if ($post -notmatch [regex]::Escape($PrevVersion)) { Fail "post-rollback version is not $PrevVersion (a candidate was partially installed)" }

if ($script:fail -ne 0) { Note "ROLLBACK-ACL: FAIL"; exit 1 }
Note "ROLLBACK-ACL: OK (real ACL mid-swap install failure -> clean rollback to $PrevVersion)"
exit 0
