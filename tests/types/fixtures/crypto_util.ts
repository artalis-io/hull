// Fixture: hull:crypto and the utility modules, used as an app would.
import { crypto } from "hull:crypto";
import type { CryptoKey, Keypair } from "hull:crypto";
import { envelope } from "hull:crypto:envelope";
import hkdfDefault, { hkdf } from "hull:crypto:hkdf";
import { otp } from "hull:crypto:otp";
import sealboxDefault, { sealbox } from "hull:crypto:sealbox";
import encodingDefault, { encoding } from "hull:encoding";
import { jwt } from "hull:jwt";
import { uuid } from "hull:uuid";
import { path } from "hull:path";
import { mime } from "hull:mime";
import { csv } from "hull:csv";
import { qrcode } from "hull:qrcode";
import { tar } from "hull:archive:tar";
import { validate } from "hull:validate";
import { i18n } from "hull:i18n";
import { template } from "hull:template";

export function cryptoUsage(): void {
    const digest: ArrayBuffer = crypto.sha256("hello");
    const hex: string = encoding.hex.encode(digest);
    const mac = crypto.hmacSha256(new Uint8Array([1, 2, 3]), encoding.bytes.toU8("key"));
    const ok: boolean = crypto.hmacSha256Verify("data", "key", mac);
    const token: string = crypto.randomToken(32, "hex");
    const stored = crypto.hashPassword("hunter2");
    const valid: boolean = crypto.verifyPassword("hunter2", stored);
    const kp: Keypair = crypto.ed25519Keypair();
    const sig = crypto.ed25519Sign("msg", kp.secretKey);
    const sigOk: boolean = crypto.ed25519Verify("msg", sig, kp.publicKey);
    const nonce = crypto.random(24);
    const key = crypto.random(32);
    const ct = crypto.secretbox("secret", nonce, key);
    const pt = crypto.secretboxOpen(ct, nonce, key);
    if (pt !== null) encoding.utf8.decode(pt);
    const shared = crypto.x25519(crypto.x25519Keypair().secretKey, crypto.boxKeypair().publicKey);
    if (shared === null) return;
    const h = crypto.createSha256().update("a").update(new Uint8Array(4));
    const d2: ArrayBuffer = h.digest();
    const envKey: CryptoKey = crypto.keyFromEnv("APP_KEY");
    const sealed = envKey.secretbox("x", nonce);
    const opened: ArrayBuffer | null = envKey.secretboxOpen(sealed, nonce);
    envKey.destroy();
    const g = crypto.gcmSeal(key, crypto.random(12), null, "plain");
    const back = crypto.gcmOpen(key, crypto.random(12), null, g.ciphertext, g.tag);
    const ctr = crypto.aes256ctr(key, crypto.random(16), "data");
    const cc = crypto.chacha20(key, crypto.random(12), 1, "data");
    const tag = crypto.poly1305(key, "m");
    const derived = crypto.bcryptPbkdf("pass", "salt", 16, 48);
    const rsaOk: boolean = crypto.verify("RS256", "-----BEGIN PUBLIC KEY-----", "data", sig);
    const pem: string | null = crypto.x509PubkeyPem(new Uint8Array(10));
    const eq: boolean = crypto.constantTimeEq("a", "b");
    const max: number = crypto.BCRYPT_MAX_ROUNDS;
    void [hex, ok, token, valid, sigOk, d2, opened, back, ctr, cc, tag, derived, rsaOk, pem, eq, max];

    // @ts-expect-error -- not an algorithm crypto.sign knows
    crypto.sign("HS256", "pem", "data");
    // @ts-expect-error -- randomToken formats are base64url and hex
    crypto.randomToken(16, "base32");
    // @ts-expect-error -- digests are bytes, not hex strings
    const wrong: string = crypto.sha256("x");
    // @ts-expect-error -- secretboxOpen may return null
    const notNull: ArrayBuffer = crypto.secretboxOpen(ct, nonce, key);
    // @ts-expect-error -- a number is not bytes
    crypto.sha512(42);
    void [wrong, notNull];
}

export function tokensUsage(): void {
    const t = envelope.sign({ action: "reset", uid: 7 }, "secret");
    const [payload, err] = envelope.verify(t, "secret");
    if (err === null) {
        const p: { [key: string]: unknown } | unknown[] = payload;
        void p;
    } else {
        const reason: "missing" | "malformed" | "bad tag" | "bad encoding" | "bad json" = err;
        void reason;
    }

    const jwtToken = jwt.sign({ sub: "u1", exp: 3600 }, "a-long-random-secret");
    const [claims, jerr] = jwt.verify(jwtToken, "a-long-random-secret", { algs: ["HS256"], leeway: 5, aud: ["web"] });
    if (jerr === null) {
        const sub = claims.sub;
        void sub;
    }
    jwt.verify(jwtToken, (kid, alg) => (alg === "RS256" && kid === "k1" ? "-----BEGIN PUBLIC KEY-----" : null), { algs: ["RS256"] });
    const decoded = jwt.decode(jwtToken);
    if (decoded) void decoded.iss;
    // @ts-expect-error -- "none" is never an accepted alg
    jwt.verify(jwtToken, "s", { algs: ["none"] });
    // @ts-expect-error -- misspelled option
    jwt.verify(jwtToken, "s", { requireExpiry: true });
    // @ts-expect-error -- sign takes an object payload
    jwt.sign("sub", "s");
}

export function kdfUsage(): void {
    const master = crypto.random(32);
    const enc: ArrayBuffer = hkdf.derive(master, 32, { info: "app v1 encryption" });
    const prk = hkdfDefault.extract(null, master);
    const okm = hkdf.expand(prk, "label", 64);
    const n: number = hkdf.HASH_LEN + hkdf.MAX_LENGTH;
    void [enc, okm, n];

    const code: string = otp.hotp(encoding.bytes.fromBuffer(crypto.random(20)), otp.step(1_700_000_000), 6);
    void code;
    // @ts-expect-error -- 4 digits is not a valid HOTP length
    otp.hotp("key", 1, 4);

    const ring = sealbox.keyring({ keys: { 1: encoding.bytes.fromBuffer(crypto.random(32)) }, current: 1 });
    const blob = sealbox.seal(ring, "value", ["kv", "user:1"]);
    const [value, serr, version] = sealboxDefault.open(ring, blob, ["kv", "user:1"]);
    if (serr === null) {
        const v: string = value;
        const ver: number = version;
        void [v, ver];
    } else {
        const r: "unknown_version" | "open_failed" = serr;
        void r;
    }
    const old: string | null = sealbox.openUnversioned(ring, 1, blob);
    const envRing = sealbox.keyringFromEnv({ keys: { 2: "KV_KEY_2" }, current: 2 });
    void [old, envRing, sealbox.MIN_LEN];
    // @ts-expect-error -- the context is an array of strings
    sealbox.seal(ring, "v", "ctx");
}

export function encodingUsage(): void {
    const b64: string = encoding.base64.encode("bytes", { url: true });
    const raw = encoding.base64.decode(b64, { url: true });
    if (raw === null) {
        const why = encoding.base64.why(b64, { url: true });
        void why;
    }
    const b32 = encodingDefault.base32.encode(new Uint8Array([1, 2]));
    const b32back: string | null = encoding.base32.decode(b32, { lenient: true });
    const u: Uint8Array = encoding.bytes.toU8("abc");
    const text: string | null = encoding.utf8.decode(u);
    const esc: string = encoding.url.encode("a b/é", { keep: "/" });
    const unesc: string = encoding.url.decode(esc, { form: true });
    void [b32back, text, unesc];
    // @ts-expect-error -- decode returns null on bad input
    const s: string = encoding.hex.decode("zz");
    // @ts-expect-error -- fromBuffer takes a buffer, not a string
    encoding.bytes.fromBuffer("abc");
    // @ts-expect-error -- misspelled option
    encoding.base64.encode("x", { urlsafe: true });
    void s;
}

export function utilUsage(): void {
    const id: string = uuid.v7() + uuid.v4();
    const p: string = path.join("a", "b", "../c.txt");
    const ext = path.extension(p);
    const inside: boolean = path.isWithin("/srv", "/srv/x");
    const type: string | null = mime.sniff(new Uint8Array([0x25, 0x50, 0x44, 0x46]));
    void [id, ext, inside, type, path.relative("/a", "/a/b"), path.stem(p)];
    // @ts-expect-error -- join takes strings
    path.join("a", 1);

    const rows: string[][] = csv.parse("a,b\n1,2\n");
    const objs = csv.parse("name,age\nann,3\n", { headers: true, maxRows: 10 });
    const name: string = objs[0].name;
    const out1: string = csv.encode([["a", 1, null]]);
    const out2: string = csv.encode([{ name: "ann", age: 3 }], { headers: true, columns: ["name", "age"] });
    void [rows, name, out1, out2];
    // @ts-expect-error -- with headers the result is objects, not string arrays
    const bad: string[][] = csv.parse("a\n", { headers: true });
    void bad;

    const q = qrcode.encode("https://example.com", { ecLevel: "Q" });
    const dark: boolean = q.matrix[1][1] === 1;
    const svg: string = qrcode.svg("hi", { scale: 6, margin: 2, light: "none" });
    void [dark, svg, q.size, q.version, q.mask];
    // @ts-expect-error -- not an error-correction level
    qrcode.encode("x", { ecLevel: "X" });

    const archive: ArrayBuffer = tar.create([{ name: "a.txt", data: "hi" }, { name: "d", isDir: true, mode: 0o755 }]);
    for (const e of tar.parse(archive)) {
        const n: string = e.name;
        const size: number = e.size;
        const dir: boolean = e.isDir;
        void [n, size, dir, e.data.byteLength];
    }
    const extracted: true = tar.extract(archive, "out");
    const packed = tar.pack(["data/a.txt", { path: "data/b.txt", name: "b.txt" }]);
    void [extracted, packed];
    // @ts-expect-error -- an entry needs a name
    tar.create([{ data: "x" }]);
}

export function formsUsage(form: Record<string, unknown>): void {
    const [ok, errors] = validate.check(form, {
        email: { required: true, email: true },
        name: { required: true, trim: true, min: 1, max: 100 },
        age: { type: "integer", min: 0, max: 150 },
        code: { pattern: "^[A-Z]{3}$", message: "bad code" },
        role: { oneof: ["admin", "user"] },
        nick: { fn: (v) => (typeof v === "string" && v.length < 20 ? true : "too long") },
    });
    if (!ok) {
        const msg: string | undefined = errors.email;
        void msg;
    }
    // @ts-expect-error -- not a supported type
    validate.check(form, { a: { type: "array" } });
    // @ts-expect-error -- pattern is a source string, not a RegExp
    validate.check(form, { a: { pattern: /x/ } });

    i18n.load("en", {
        hello: "Hello, ${name}!",
        nav: { home: "Home" },
        format: { thousandsSep: ",", currency: { USD: { symbol: "$", decimalDigits: 2 } } },
    });
    const loc = i18n.detect("en-GB,en;q=0.8") ?? "en";
    i18n.locale(loc);
    const greet: string = i18n.tIn(loc, "hello", { name: "Ann" });
    const money: string = i18n.currencyIn(loc, 12.5, "USD");
    void [greet, money, i18n.t("nav.home"), i18n.number(1234.5), i18n.date(0), i18n.dateIn(null, 0)];
    // @ts-expect-error -- currency needs a code
    i18n.currency(5);

    const html: string = template.render("pages/home.html", { user: { name: "ann" } });
    const render = template.compile("partials/row.html");
    const row: string = render({ item: 1 });
    void [html, row, template.renderString("{{ x }}", { x: 1 })];
    template.clearCache();
    // @ts-expect-error -- data is an object
    template.render("x.html", "data");
}
