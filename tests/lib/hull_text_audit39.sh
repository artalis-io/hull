# shellcheck shell=sh
# hull_text_audit39.sh - the audit 39 apps shared by e2e_text_storage.sh
# (SQLite), e2e_postgres.sh and e2e_mysql.sh: request / app text the server
# refuses (a NUL, bytes that are not UTF-8, a lone surrogate) must not fail a
# stdlib write after the step it cannot undo.
#
#   idempotency - a response body echoing such bytes is stored (base64) and
#                 replayed byte-exact; the handler runs once; a key that is
#                 not printable ASCII is a 400; the middleware works on MySQL
#                 (`key` is a reserved word there).
#   session /   - a login whose User-Agent holds obs-text bytes (or is cut
#   audit-log     through a multibyte character) succeeds and records the
#                 device once.
#   attachment  - an upload whose filename is not UTF-8 is stored.
#
# text_audit39_apps DIR   writes DIR/l/app.lua and DIR/j/app.js.
# text_audit39_run HULL APP PORT TAG [hull args...]
#   starts APP on PORT, drives it with curl, stops it and prints one line:
#     A39 idem=201/201 same=1 count=1 badkey=400 login=200/200 new=1/0 att=200
#   With A39_INJECT set (a shell command, eval'ed, that makes the database refuse the
#   stored response of a /payfail request), it also checks that a store that
#   fails leaves the key processed - the retry is a 409, the handler ran once:
#     ... fail=201/409 failcount=1
#   TAG keeps the rows of each run apart (keys, tables, users).

text_audit39_apps() {
    mkdir -p "$1/l" "$1/j"
    cat > "$1/l/app.lua" <<'LUA'
app.manifest({
    modules = { "hull/db@1", "hull/http-server@1", "hull/env@1",
                "hull/web/middleware/idempotency@1", "hull/web/middleware/session@1",
                "hull/web/middleware/audit-log@1", "hull/attachment@1", "hull/blob@1" },
    env = { "A39_TAG" },
    fs = { write = { "data/" } },
})
local db = require("hull.db")
local env = require("hull.env")
local idem = require("hull.web.middleware.idempotency")
local session = require("hull.web.middleware.session")
local audit_log = require("hull.web.middleware.audit-log")
local attachment = require("hull.attachment")
local blob = require("hull.blob")
local TAG = "lua"
local T

app.main(function()
    TAG = env.get("A39_TAG") or TAG
    T = "a39_pay_" .. TAG
    idem.init()
    session.init()
    audit_log.init({ fingerprint_salt = "a39-e2e-salt" })
    blob.init({ dir = "data/blobs" })
    attachment.init()
    db.default().exec("CREATE TABLE IF NOT EXISTS " .. T .. " (note TEXT)")
end)

app.get("/health", function(_, res) res:text("ok") end)
local idem_mw = idem.middleware({ get_principal = function() return "u-" .. TAG end })
app.use_post("POST", "/pay", idem_mw)
app.use_post("POST", "/payfail", idem_mw)
local function pay(req, res)
    db.default().exec("INSERT INTO " .. T .. " (note) VALUES (?)", { req.path })
    -- Echo request bytes verbatim (a Latin-1 name in an HTML fragment).
    idem.respond_html(req, res, 201, "<p>paid " .. (req.body or "") .. "</p>")
end
app.post("/pay", pay)
app.post("/payfail", pay)
app.get("/count", function(req, res)
    local where = req.query.path and " WHERE note = ?" or ""
    local rows = db.default().query("SELECT COUNT(*) AS n FROM " .. T .. where,
                                    req.query.path and { req.query.path } or nil)
    res:text(tostring(rows[1].n))
end)
app.post("/login", function(req, res)
    local uid = "u-" .. TAG
    local new = audit_log.is_new_device(uid, req)
    session.create({ user_id = uid }, { req = req })
    audit_log.record(uid, "login", req)
    res:text((new and "1" or "0") .. " " .. #audit_log.list_devices(uid))
end)
app.post("/upload", function(req, res)
    for part in req:multipart() do
        if part.filename then attachment.store(part) end
    end
    res:text("ok")
end, { multipart = { max_part_size = 32 * 1024 } })
LUA
    cat > "$1/j/app.js" <<'JS'
import { app } from "hull:app";
import { db } from "hull:db";
import { env } from "hull:env";
import { idempotency } from "hull:web:middleware:idempotency";
import { session } from "hull:web:middleware:session";
import { auditLog } from "hull:web:middleware:audit-log";
import { attachment } from "hull:attachment";
import { blob } from "hull:blob";
app.manifest({
    modules: ["hull/db@1", "hull/http-server@1", "hull/env@1",
              "hull/web/middleware/idempotency@1", "hull/web/middleware/session@1",
              "hull/web/middleware/audit-log@1", "hull/attachment@1", "hull/blob@1"],
    env: ["A39_TAG"],
    fs: { write: ["data/"] },
});
let TAG = "js", T = "";
app.main(() => {
    TAG = env.get("A39_TAG") || TAG;
    T = "a39_pay_" + TAG;
    idempotency.init();
    session.init();
    auditLog.init({ fingerprintSalt: "a39-e2e-salt" });
    blob.init({ dir: "data/blobs" });
    attachment.init();
    db.default().exec("CREATE TABLE IF NOT EXISTS " + T + " (note TEXT)");
    return 0;
});
app.get("/health", (_req, res) => res.text("ok"));
const idemMw = idempotency.middleware({ getPrincipal: () => "u-" + TAG });
app.usePost("POST", "/pay", idemMw);
app.usePost("POST", "/payfail", idemMw);
function pay(req, res) {
    db.default().exec("INSERT INTO " + T + " (note) VALUES (?)", [req.path]);
    // A display name cut by code units: half of an emoji, and a NUL.
    const name = "\u{1F600}name".slice(0, 1) + "\u0000";
    idempotency.respondHtml(req, res, 201, "<p>paid " + name + " café</p>");
}
app.post("/pay", pay);
app.post("/payfail", pay);
app.get("/count", (req, res) => {
    const p = req.query.path;
    const rows = p ? db.default().query("SELECT COUNT(*) AS n FROM " + T + " WHERE note = ?", [p])
                   : db.default().query("SELECT COUNT(*) AS n FROM " + T);
    res.text(String(rows[0].n));
});
app.post("/login", (req, res) => {
    const uid = "u-" + TAG;
    const isNew = auditLog.isNewDevice(uid, req);
    session.create({ user_id: uid }, { req });
    auditLog.record(uid, "login", req);
    res.text((isNew ? "1" : "0") + " " + auditLog.listDevices(uid).length);
});
app.post("/upload", async (req, res) => {
    for await (const part of req.multipart()) {
        if (part.filename) await attachment.store(part);
    }
    res.text("ok");
}, { multipart: { maxPartSize: 32 * 1024 } });
JS
}

# _a39_code URL [curl args...] - the HTTP status of one request.
_a39_code() {
    _u=$1; shift
    curl -s -o /dev/null -w "%{http_code}" "$@" "$_u" 2>/dev/null || true
}

text_audit39_run() {
    _hull=$1; _app=$2; _port=$3; _tag=$4; shift 4
    _w=$(mktemp -d)
    A39_TAG=$_tag "$_hull" -p "$_port" "$@" "$_app" > "$_w/log" 2>&1 &
    _pid=$!
    _i=0
    while [ "$_i" -lt 60 ]; do
        curl -fsS "http://127.0.0.1:$_port/health" >/dev/null 2>&1 && break
        if ! kill -0 "$_pid" 2>/dev/null; then break; fi
        _i=$((_i + 1)); sleep 0.5
    done
    _b="http://127.0.0.1:$_port"
    printf 'caf\351 \000 \377' > "$_w/body"
    _k="a39-$_tag-1"
    _c1=$(curl -s -o "$_w/r1" -w '%{http_code}' -X POST "$_b/pay" \
          -H "Idempotency-Key: $_k" --data-binary @"$_w/body" 2>/dev/null || true)
    _c2=$(curl -s -o "$_w/r2" -D "$_w/h2" -w '%{http_code}' -X POST "$_b/pay" \
          -H "Idempotency-Key: $_k" --data-binary @"$_w/body" 2>/dev/null || true)
    _same=0
    if [ -s "$_w/r1" ] && cmp -s "$_w/r1" "$_w/r2" && grep -qi '^x-idempotency-replay: true' "$_w/h2"; then
        _same=1
    fi
    _count=$(curl -s "$_b/count?path=/pay" 2>/dev/null)
    _bad=$(_a39_code "$_b/pay" -X POST -H "$(printf 'Idempotency-Key: caf\351')" --data-binary x)
    _long=$(printf 'A%.0s' $(seq 1 511))
    _ua=$(printf 'Moz\377illa %s\303\251tail' "$_long")
    _l1=$(curl -s -o "$_w/l1" -w '%{http_code}' -X POST "$_b/login" -H "User-Agent: $_ua" 2>/dev/null || true)
    _l2=$(curl -s -o "$_w/l2" -w '%{http_code}' -X POST "$_b/login" -H "User-Agent: $_ua" 2>/dev/null || true)
    _n1=$(cut -d' ' -f1 < "$_w/l1" 2>/dev/null); _n2=$(cut -d' ' -f1 < "$_w/l2" 2>/dev/null)
    _d2=$(cut -d' ' -f2 < "$_w/l2" 2>/dev/null)
    printf 'attachment bytes' > "$_w/f.txt"
    _att=$(_a39_code "$_b/upload" -X POST -F "$(printf 'file=@%s;filename=caf\351\377.txt' "$_w/f.txt")")
    _line="A39 idem=$_c1/$_c2 same=$_same count=$_count badkey=$_bad login=$_l1/$_l2 new=$_n1/$_n2 dev=$_d2 att=$_att"
    if [ -n "${A39_INJECT:-}" ]; then
        eval "$A39_INJECT" >/dev/null 2>&1 || true
        _fk="a39-$_tag-fail"
        _f1=$(_a39_code "$_b/payfail" -X POST -H "Idempotency-Key: $_fk" --data-binary x)
        _f2=$(_a39_code "$_b/payfail" -X POST -H "Idempotency-Key: $_fk" --data-binary x)
        _fc=$(curl -s "$_b/count?path=/payfail" 2>/dev/null)
        _line="$_line fail=$_f1/$_f2 failcount=$_fc"
    fi
    kill "$_pid" 2>/dev/null; wait "$_pid" 2>/dev/null
    echo "$_line"
    if ! case "$_line" in
        "A39 idem=201/201 same=1 count=1 badkey=400 login=200/200 new=1/0 dev=1 att=200"*) true ;;
        *) false ;; esac; then
        echo "--- $_app log ---"; cat "$_w/log"
        rm -rf "$_w"; return 1
    fi
    case "$_line" in
        *"fail="*)
            case "$_line" in
                *"fail=201/409 failcount=1") ;;
                *) echo "--- $_app log ---"; cat "$_w/log"; rm -rf "$_w"; return 1 ;;
            esac ;;
    esac
    rm -rf "$_w"
    return 0
}
