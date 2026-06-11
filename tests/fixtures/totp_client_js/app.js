// TOTP fixture (JS) for tests/e2e_totp.sh.
//
// Mirrors tests/fixtures/totp_client_lua/app.lua — same endpoints,
// same PBM inline render. See that file for the full design notes.

import { app }    from "hull:app";
import { totp }   from "hull:web:middleware:totp";
import { qrcode } from "hull:qrcode";

app.manifest({
    name: "totp-test-client-js",
    modules: [
        "hull/web/middleware/totp@1",
        "hull/qrcode@1",
        "hull/http-server@1",
        "hull/json@1",
    ],
});

totp.init({ issuer: "TestApp" });

// P1 ASCII PBM. P4 (binary) would truncate at the first NUL byte
// when res.html does its C-string conversion (every all-light QR
// row begins with zeros). P1 is text-only, safe through res.html
// on both runtimes. zbarimg decodes it natively.
function pbm(text, scale) {
    scale = scale || 8;
    const q = qrcode.encode(text, { ecLevel: "M" });
    const margin = 4;
    const totalModules = q.size + 2 * margin;
    const widthPx = totalModules * scale;
    const heightPx = widthPx;
    const lines = [`P1\n${widthPx} ${heightPx}\n`];
    for (let r = 0; r < totalModules; r++) {
        const rowBits = [];
        for (let c = 0; c < totalModules; c++) {
            const insideR = r >= margin && r < totalModules - margin;
            const insideC = c >= margin && c < totalModules - margin;
            const dark = insideR && insideC
                && q.matrix[r - margin + 1][c - margin + 1] === 1;
            const ch = dark ? "1" : "0";
            for (let k = 0; k < scale; k++) rowBits.push(ch);
        }
        const line = rowBits.join(" ") + "\n";
        for (let k = 0; k < scale; k++) lines.push(line);
    }
    return lines.join("");
}

function readJsonBody(req) {
    try { return JSON.parse(req.body || "{}"); }
    catch (_e) { return null; }
}

app.post("/enroll", (req, res) => {
    const body = readJsonBody(req) || {};
    const userId = body.user_id || "alice";
    const r = totp.enroll(userId);
    res.json({
        user_id:        userId,
        secret_base32:  r.secretBase32,
        otpauth_url:    r.otpauthUrl,
        recovery_codes: r.recoveryCodes,
    });
});

app.get("/qr", (req, res) => {
    const text = req.query && req.query.text;
    if (!text) return res.status(400).json({ error: "text required" });
    res.header("Content-Type", "image/x-portable-bitmap");
    res.html(pbm(text));
});

app.post("/confirm", (req, res) => {
    const body = readJsonBody(req);
    if (!body) return res.status(400).json({ error: "bad json" });
    const ok = totp.confirm(body.user_id, body.code);
    res.json({ ok });
});

app.post("/verify", (req, res) => {
    const body = readJsonBody(req);
    if (!body) return res.status(400).json({ error: "bad json" });
    const result = totp.verify(body.user_id, body.code);
    res.json({ ok: result[0], kind: result[1] });
});
