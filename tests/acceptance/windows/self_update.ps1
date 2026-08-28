<#
  self_update.ps1 - Prove a REAL Windows self-update of the old (previous
  release) APE to the candidate, via a maintainer-prepopulated staging repo
  whose /releases/latest mirrors the RC. Run AS the standard user.

  Fail-closed assertions:
    - `hull update --repo=<staging>` returns success;
    - the updating process exits normally (exit 0, no crash);
    - the NEXT invocation reports the expected candidate version;
    - the deferred-swap residue is cleaned: no <self>.new remains, and <self>.old
      is swept by the next startup (the running process holds it until exit).

  Usage: self_update.ps1 -Hull <exe> -Repo <org/staging> -ExpectVersion 0.14.0-rc1 -Evidence <file>
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
$script:fail = 0

$new = "$Hull.new"
$old = "$Hull.old"

Note "## Real self-update (upgrade)"
$pre = (& $Hull version 2>&1 | Select-Object -First 1)
Note ("- pre-update version: {0}" -f $pre)

$out  = & $Hull update --repo $Repo 2>&1
$code = $LASTEXITCODE
Note "- hull update output:"
Note (($out | Out-String) -replace '(?m)^', '    ')
Note ("- hull update exit code: {0}" -f $code)
if ($code -ne 0) { Fail "hull update returned non-zero" }

# The NEXT invocation (a fresh process; the updated binary is on disk) must
# report the candidate version, and its startup sweep removes <self>.old.
$post = (& $Hull version 2>&1 | Select-Object -First 1)
Note ("- post-update version: {0}" -f $post)
if ($post -notmatch [regex]::Escape($ExpectVersion)) { Fail "post-update version is not $ExpectVersion" }

if (Test-Path $new) { Fail "<self>.new residue remains after a successful update" }
if (Test-Path $old) {
    # One more invocation to give the startup sweep a clean shot (the update
    # process held the lock; it has since exited).
    & $Hull version *> $null
    if (Test-Path $old) { Fail "<self>.old was not swept on the next startup" }
    else { Note "- <self>.old swept on the next startup" }
} else {
    Note "- no <self>.old residue"
}

if ($script:fail -ne 0) { Note "SELF-UPDATE: FAIL"; exit 1 }
Note "SELF-UPDATE: OK ($pre -> $post via deferred swap; residue clean)"
exit 0
