<#
  rollback_acl.ps1 - prove the candidate's self-update ROLLBACK on a REAL Windows
  ACL scenario (no test hooks, no helper processes, no timing races), run AS the
  standard user.

  Setup: the running CANDIDATE exe sits in a dedicated directory. First we
  DISCONNECT inheritance on the existing exe (`icacls /inheritance:d` converts
  its current permissions to explicit ACEs), so the deny we add next cannot
  propagate onto it. Then we deny DELETE to the user on NEWLY-created files in
  the directory via an inherit-only ACE. Result: the existing exe (and its
  rename to <self>.old) keeps a permissive, DELETE-allowing ACL, while any file
  created afterwards (<self>.new) inherits the DELETE deny. During `hull update`:
    - the downloaded <self>.new can be WRITTEN but not renamed (rename needs
      DELETE on the source);
    - the running exe still moves to <self>.old (its ACL permits delete);
    - installing <self>.new at the original path fails via real ACL enforcement;
    - rollback renames <self>.old back to <self> (it kept the permissive ACL).

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
$script:fail = 0

$dir = Split-Path -Parent $Hull
$new = "$Hull.new"
$me  = "$env:USERDOMAIN\$env:USERNAME"

Note "## Rollback via real Windows ACL (from the candidate)"
$pre = (& $Hull version 2>&1 | Select-Object -First 1)
Note ("- pre-update version: {0}" -f $pre)
if ($pre -notmatch [regex]::Escape($RcVersion)) { Fail "candidate is not $RcVersion pre-update" }

# Protect the existing exe's ACL from the deny we are about to add: convert its
# inherited permissions to explicit ACEs and disconnect inheritance, so the
# directory-level deny cannot propagate onto it (it must stay renamable to .old).
& icacls $Hull /inheritance:d | Out-Null
Note ("- disconnected inheritance on the running exe (keeps its permissive ACL)")

# Deny DELETE to the user on NEWLY-created files (inherit-only). Only files
# created AFTER this - i.e. <self>.new - inherit it; the existing exe does not.
& icacls $dir /deny "${me}:(OI)(IO)(DE)" | Out-Null
Note ("- applied inherit-only DELETE deny for {0} on new files in {1}" -f $me, $dir)

# Force-reinstall the RC; the ACL makes the install rename fail -> rollback.
$out = & $Hull update --force --repo $Repo 2>&1
$code = $LASTEXITCODE
$text = ($out | Out-String)
Note ($text -replace '(?m)^','    ')
Note ("- exit code: {0}" -f $code)

# Remove the deny so post-run cleanup can delete residue.
& icacls $dir /remove:d "${me}" | Out-Null

if ($code -eq 0) { Fail "hull update unexpectedly SUCCEEDED under the ACL lock" }
if ($text -match 'rolled back to the previous binary') {
    Note "- evidence: reached the mid-swap install failure -> rolled back"
} else {
    Fail "did not reach the mid-swap install/rollback path (no rollback message)"
}
if ($text -match 'cannot move .* aside')       { Fail "failed at the INITIAL rename, not the install step" }
if ($text -match 'failed to download|returned HTTP') { Fail "failed during DOWNLOAD, not the install step" }
if (Test-Path $new) { Note "- evidence: <self>.new exists (download+write succeeded past the ACL create/write allowance)" }
else { Fail "<self>.new absent - the ACL install failure was not exercised" }

if (-not (Test-Path $Hull)) { Fail "original candidate path missing after rollback" }
$post = (& $Hull version 2>&1 | Select-Object -First 1)
Note ("- post-rollback version: {0}" -f $post)
if ($post -notmatch [regex]::Escape($RcVersion)) { Fail "post-rollback version is not $RcVersion (a partial install landed)" }

if ($script:fail -ne 0) { Note "ROLLBACK-ACL: FAIL"; exit 1 }
Note "ROLLBACK-ACL: OK (real ACL mid-swap install failure -> clean rollback to $RcVersion)"
exit 0
