<#
  preconditions.ps1 - FAIL-CLOSED verification that this process runs in a
  genuinely restricted Windows environment, run AS the standard (non-admin)
  acceptance user. Records evidence for each check and exits non-zero unless ALL
  of the following hold:

    1. the current token is NOT an administrator and is NOT elevated;
    2. Developer Mode is disabled (machine policy);
    3. creating a symbolic link is DENIED (the runtime signal that this account
       lacks SeCreateSymbolicLinkPrivilege, i.e. neither elevated nor Dev-Mode).

  This is the gate that makes the non-admin cosmocc-extraction + self-update
  evidence meaningful: if any precondition is not as expected, the run fails here
  rather than silently proving something on a too-permissive environment.

  Usage: preconditions.ps1 -Evidence <path-to-evidence-file>
#>
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Evidence)

$ErrorActionPreference = 'Stop'
function Note($m) { Add-Content -Path $Evidence -Value $m; Write-Host $m }

$fail = 0
Note "## Windows preconditions (as $(whoami))"

# 1. Non-admin + not elevated -------------------------------------------------
$id  = [Security.Principal.WindowsIdentity]::GetCurrent()
$pri = New-Object Security.Principal.WindowsPrincipal($id)
$isAdmin = $pri.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
# Token elevation: a full (elevated) token has integrity level High (S-1-16-12288).
$groups  = whoami /groups
$isHigh  = ($groups -match 'S-1-16-12288')      # High mandatory level
$isMed   = ($groups -match 'S-1-16-8192')       # Medium mandatory level
Note ("- token user: {0}" -f $id.Name)
Note ("- IsInRole(Administrator): {0}" -f $isAdmin)
Note ("- integrity: High={0} Medium={1}" -f [bool]$isHigh, [bool]$isMed)
if ($isAdmin) { Note "  FAIL: token is in the Administrators role"; $fail = 1 }
if ($isHigh)  { Note "  FAIL: token is elevated (High integrity)";  $fail = 1 }
if (-not $isMed) { Note "  FAIL: token is not Medium integrity (expected for a standard user)"; $fail = 1 }

# 2. Developer Mode disabled --------------------------------------------------
$devKey = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock'
$dev = 0
try {
    $v = Get-ItemProperty -Path $devKey -Name AllowDevelopmentWithoutDevLicense -ErrorAction Stop
    $dev = [int]$v.AllowDevelopmentWithoutDevLicense
} catch { $dev = 0 }   # absent == disabled
Note ("- Developer Mode (AllowDevelopmentWithoutDevLicense): {0}" -f $dev)
if ($dev -ne 0) { Note "  FAIL: Developer Mode is enabled"; $fail = 1 }

# 3. Symlink creation DENIED (specifically - not a target/dir write failure) --
# First prove the target + directory ARE writable (write the target file); only
# THEN attempt the symlink, so a failure is unambiguously the missing
# SeCreateSymbolicLinkPrivilege (no elevation / Developer Mode), not an
# unwritable path.
$tmp    = Join-Path $env:TEMP ("hull-symcheck-{0}" -f ([guid]::NewGuid()))
$target = Join-Path $env:TEMP ("hull-symtarget-{0}.txt" -f ([guid]::NewGuid()))
$targetWritable = $false
try { Set-Content -Path $target -Value 'x' -ErrorAction Stop; $targetWritable = $true }
catch { Note ("  FAIL(harness): symlink target/dir is not writable ({0}) - cannot isolate the symlink-privilege check" -f $_.Exception.Message); $fail = 1 }
if ($targetWritable) {
    Note "- symlink target is writable (so a symlink failure is privilege-specific)"
    $symDenied = $false
    try {
        New-Item -ItemType SymbolicLink -Path $tmp -Target $target -ErrorAction Stop | Out-Null
        Note "- control symlink creation: SUCCEEDED (unexpected - env is not restricted)"
        Remove-Item $tmp -Force -ErrorAction SilentlyContinue
    } catch {
        $symDenied = $true
        Note ("- control symlink creation: DENIED as expected ({0})" -f $_.Exception.Message)
    }
    if (-not $symDenied) { Note "  FAIL: symlink creation was allowed - environment is not restricted"; $fail = 1 }
    Remove-Item $target -Force -ErrorAction SilentlyContinue
}

if ($fail -ne 0) { Note "PRECONDITIONS: FAIL"; exit 1 }
Note "PRECONDITIONS: OK (non-admin, Dev-Mode off, symlink denied)"
exit 0
