<#
  extras.ps1 - the remaining Windows acceptance bullets, run AS the standard
  user with the CANDIDATE hull already installed:

    - self-update works from a PATH CONTAINING SPACES;
    - non-admin `hull tools install cosmocc` succeeds (validated hardlinks, #425);
    - `hull build` + serve a minimal Lua AND JS /ping app -> "pong";
    - a nested local-module app builds and serves (extraction/built parity, #423);
    - `hull doctor`, `hull tools list`, `hull agent tools` agree on cosmocc.

  Fail-closed: any check that does not hold sets the failure flag and the script
  exits non-zero.

  Usage:
    extras.ps1 -Hull <candidate-exe> -RcRepo <org/staging-rc> \
               -ExpectVersion 0.14.0-rc1 -Evidence <file>
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory=$true)][string]$Hull,
    [Parameter(Mandatory=$true)][string]$RcRepo,
    [Parameter(Mandatory=$true)][string]$ExpectVersion,
    [Parameter(Mandatory=$true)][string]$Evidence
)

$ErrorActionPreference = 'Stop'
function Note($m) { Add-Content -Path $Evidence -Value $m; Write-Host $m }
function Fail($m) { Note ("  FAIL: {0}" -f $m); $script:fail = 1 }
$script:fail = 0
$work = Join-Path $env:TEMP ("hull-accept-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $work | Out-Null

# Serve a built APE and curl a path; returns the response body (or '').
function Serve-And-Get([string]$exe, [string]$appPath, [int]$port, [string]$route) {
    $p = Start-Process -FilePath $exe -ArgumentList @($appPath, '-p', "$port") -PassThru -WindowStyle Hidden
    try {
        for ($i = 0; $i -lt 40; $i++) {
            Start-Sleep -Milliseconds 250
            try { return (Invoke-RestMethod -Uri "http://127.0.0.1:$port$route" -TimeoutSec 2) } catch {}
        }
        return ''
    } finally { $p | Stop-Process -Force -ErrorAction SilentlyContinue }
}

# --- 1. self-update from a path with spaces ---------------------------------
# Force-reinstall the RC over the CANDIDATE placed in a spaces path: exercises
# the real deferred rename-aside swap (running .exe) with spaces in every path.
Note "## Self-update from a path with spaces (candidate deferred swap)"
$spaceDir = Join-Path $work 'hull test dir with spaces'
New-Item -ItemType Directory -Path $spaceDir | Out-Null
$spaceHull = Join-Path $spaceDir 'my hull.com'
Copy-Item $Hull $spaceHull
$sc = & $spaceHull update --force --repo $RcRepo 2>&1; $rc = $LASTEXITCODE
Note (($sc | Out-String) -replace '(?m)^','    ')
if ($rc -ne 0) { Fail "self-update from a spaces path returned non-zero" }
$sv = (& $spaceHull version 2>&1 | Select-Object -First 1)
if ($sv -notmatch [regex]::Escape($ExpectVersion)) { Fail "spaces-path candidate is not $ExpectVersion (got '$sv')" }
elseif (Test-Path "$spaceHull.new") { Fail "spaces-path <self>.new residue after a successful update" }
else { Note "- OK: updated to '$sv' from a spaces path via the deferred swap" }

# --- 2. non-admin cosmocc install -------------------------------------------
Note "## Non-admin cosmocc install"
$ci = & $Hull tools install cosmocc 2>&1; $rc = $LASTEXITCODE
Note (($ci | Out-String) -replace '(?m)^','    ')
if ($rc -ne 0) { Fail "hull tools install cosmocc returned non-zero (non-admin)" } else { Note "- OK: cosmocc installed non-admin" }

# --- 3. build + serve minimal Lua and JS /ping ------------------------------
Note "## Build + serve /ping (Lua + JS)"
$lua = @'
app.manifest({ modules = { "hull/http-server@1" } })
app.get("/ping", function(req, res) res:text("pong") end)
'@
$js = @'
import { app } from "hull:app";
app.manifest({ modules: ["hull/http-server@1"] });
app.get("/ping", (req, res) => res.text("pong"));
'@
foreach ($case in @(@{ext='lua';src=$lua;port=39701}, @{ext='js';src=$js;port=39702})) {
    $adir = Join-Path $work ("ping-" + $case.ext)
    New-Item -ItemType Directory -Path $adir | Out-Null
    Set-Content -Path (Join-Path $adir ("app." + $case.ext)) -Value $case.src
    $bout = & $Hull build $adir -o (Join-Path $adir 'out.com') 2>&1; $rc = $LASTEXITCODE
    if ($rc -ne 0) { Note (($bout | Out-String) -replace '(?m)^','    '); Fail ("hull build failed for the " + $case.ext + " /ping app"); continue }
    $body = Serve-And-Get (Join-Path $adir 'out.com') (Join-Path $adir 'out.com') $case.port '/ping'
    if ("$body".Trim() -eq 'pong') { Note ("- OK: " + $case.ext + " /ping served pong") }
    else { Fail ($case.ext + " /ping did not serve pong (got '" + $body + "')") }
}

# --- 4. nested local-module app builds + serves (extraction/built parity) ----
Note "## Nested local-module app (built parity)"
$nd = Join-Path $work 'nested'
New-Item -ItemType Directory -Path (Join-Path $nd 'routes') | Out-Null
New-Item -ItemType Directory -Path (Join-Path $nd 'lib')    | Out-Null
Set-Content (Join-Path $nd 'app.lua') @'
local users = require("./routes/users")
app.manifest({ modules = { "hull/http-server@1" } })
users.register(app)
'@
Set-Content (Join-Path $nd 'routes\users.lua') @'
local auth = require("./../lib/auth")
local M = {}
function M.register(app) app.get("/who", function(req, res) res:text(auth.who()) end) end
return M
'@
Set-Content (Join-Path $nd 'lib\auth.lua') @'
return { who = function() return "nested-ok" end }
'@
$nb = & $Hull build $nd -o (Join-Path $nd 'out.com') 2>&1; $rc = $LASTEXITCODE
if ($rc -ne 0) { Note (($nb | Out-String) -replace '(?m)^','    '); Fail "nested-module app build failed" }
else {
    $body = Serve-And-Get (Join-Path $nd 'out.com') (Join-Path $nd 'out.com') 39703 '/who'
    if ("$body".Trim() -eq 'nested-ok') { Note "- OK: built nested-module app resolved ./routes -> ./../lib and served" }
    else { Fail "nested-module built app did not serve the deep-resolved value (got '$body')" }
}

# --- 5. doctor / tools list / agent tools agree on cosmocc -------------------
Note "## doctor / tools list / agent tools agree on cosmocc"
$doc = (& $Hull doctor --json 2>$null | ConvertFrom-Json)
$tl  = (& $Hull tools list --json 2>$null | ConvertFrom-Json)
$at  = (& $Hull agent tools 2>$null | ConvertFrom-Json)
$docCosmocc = ($doc.compilers | Where-Object { $_.name -eq 'cosmocc' }).path
$tlCosmocc  = ($tl.tools     | Where-Object { $_.name -eq 'cosmocc' })
$atCosmocc  = ($at.tools     | Where-Object { $_.name -eq 'cosmocc' })
Note ("- doctor cosmocc path: {0}" -f $docCosmocc)
Note ("- tools-list cosmocc: installed={0} source={1} path={2}" -f $tlCosmocc.installed, $tlCosmocc.source, $tlCosmocc.path)
Note ("- agent-tools cosmocc: installed={0} path={1}" -f $atCosmocc.installed, $atCosmocc.path)
if (-not $docCosmocc)         { Fail "doctor did not resolve cosmocc" }
if (-not $tlCosmocc.installed){ Fail "tools list does not report cosmocc installed" }
if (-not $atCosmocc.installed){ Fail "agent tools does not report cosmocc installed" }

Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
if ($script:fail -ne 0) { Note "EXTRAS: FAIL"; exit 1 }
Note "EXTRAS: OK (spaces, cosmocc install, Lua/JS pong, nested parity, doctor/tools agreement)"
exit 0
