-- TOTP fixture (Lua) for tests/e2e_totp.sh.
--
-- Exposes a minimal HTTP surface that the orchestrator drives:
--
--   POST /enroll                            -> { secret_base32, otpauth_url,
--                                                recovery_codes }
--   GET  /qr?text=<otpauth>                 -> binary PBM (P4) image
--   POST /confirm  { user_id, code }        -> { ok }
--   POST /verify   { user_id, code }        -> { ok, kind }
--
-- The PBM render is inlined here (~20 LOC) rather than added to
-- hull/qrcode so the public API stays focused on SVG. zbarimg
-- decodes PBM natively, which avoids a separate SVG→PNG conversion
-- dependency in CI.

app.manifest({
    name = "totp-test-client",
    modules = {
        "hull/web/middleware/totp@1",
        "hull/qrcode@1",
        "hull/http-server@1",
        "hull/json@1",
    },
})

local totp   = require("hull.web.middleware.totp")
local qrcode = require("hull.qrcode")
local json   = require("hull.json")

totp.init({
    issuer = "TestApp",
    -- Recovery codes default of 10 is fine for e2e.
})

-- P1 ASCII PBM. Larger on the wire than P4 (one ASCII char per
-- bit) but text-only, so it survives the JS-side res.html → C
-- string boundary that would otherwise truncate a P4 binary at
-- the first NUL byte (every all-light QR module row begins with
-- zeros). Lua doesn't have that issue but using the same format
-- across runtimes simplifies the test orchestrator.
local function pbm(text, scale)
    scale = scale or 8
    local q = qrcode.encode(text, { ec_level = "M" })
    local margin = 4
    local total_modules = q.size + 2 * margin
    local width_px = total_modules * scale
    local height_px = width_px
    local rows = { string.format("P1\n%d %d\n", width_px, height_px) }
    for r = 0, total_modules - 1 do
        local row_bits = {}
        for c = 0, total_modules - 1 do
            local inside_r = r >= margin and r < total_modules - margin
            local inside_c = c >= margin and c < total_modules - margin
            local dark = inside_r and inside_c
                and q.matrix[r - margin + 1][c - margin + 1] == 1
            local ch = dark and "1" or "0"
            for _ = 1, scale do row_bits[#row_bits + 1] = ch end
        end
        local line = table.concat(row_bits, " ") .. "\n"
        for _ = 1, scale do rows[#rows + 1] = line end
    end
    return table.concat(rows)
end

local function read_json_body(req)
    local ok, obj = pcall(json.decode, req.body or "")
    if not ok then return nil end
    return obj
end

app.post("/enroll", function(req, res)
    local body = read_json_body(req) or {}
    local user_id = body.user_id or "alice"
    local r = totp.enroll(user_id)
    res:json({
        user_id        = user_id,
        secret_base32  = r.secret_base32,
        otpauth_url    = r.otpauth_url,
        recovery_codes = r.recovery_codes,
    })
end)

app.get("/qr", function(req, res)
    local text = req.query and req.query.text
    if not text or text == "" then
        return res:status(400):json({ error = "text required" })
    end
    res:header("Content-Type", "image/x-portable-bitmap")
    res:html(pbm(text))
end)

app.post("/confirm", function(req, res)
    local body = read_json_body(req)
    if not body then return res:status(400):json({ error = "bad json" }) end
    local ok = totp.confirm(body.user_id, body.code)
    res:json({ ok = ok })
end)

app.post("/verify", function(req, res)
    local body = read_json_body(req)
    if not body then return res:status(400):json({ error = "bad json" }) end
    -- verify_with_kind preserves the (ok, kind) tuple this fixture
    -- surfaces for the e2e. App code that only needs the bool can
    -- use the simpler totp.verify (returns bare bool, no second).
    local ok, kind = totp.verify_with_kind(body.user_id, body.code)
    res:json({ ok = ok, kind = kind })
end)
