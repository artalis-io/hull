// Fixture: hull:http-client, hull:email, hull:smtp, hull:ssh, hull:compute,
// hull:gpu, hull:image, hull:tui used as an app would. Never executed.
import { httpClient } from "hull:http-client";
import type { HttpResponse } from "hull:http-client";
import { email } from "hull:email";
import { smtp } from "hull:smtp";
import { ssh } from "hull:ssh";
import sshDefault from "hull:ssh";
import type { SshConnection, SshError } from "hull:ssh";
import { compute } from "hull:compute";
import type { WasmInstance } from "hull:compute";
import { gpu } from "hull:gpu";
import { image } from "hull:image";
import { tui } from "hull:tui";
import type { TuiEvent } from "hull:tui";

export async function httpUsage(): Promise<number> {
    const r: HttpResponse = httpClient.get("https://api.example.com/x", {
        headers: { Accept: "application/json" }, timeoutMs: 5000,
    });
    const ct: string | undefined = r.headers["content-type"];
    const posted = httpClient.post("https://api.example.com/y", JSON.stringify({ a: 1 }));
    httpClient.request("PUT", "https://api.example.com/z", { body: new Uint8Array(4) });
    httpClient.delete("https://api.example.com/z");
    const a = await httpClient.async.get("https://api.example.com/x");
    const b = await httpClient.async.post("https://api.example.com/y", "body", { headers: { "X-K": "v" } });
    await httpClient.async.request("PATCH", "https://api.example.com/y", { body: "x", timeoutMs: 100 });
    // @ts-expect-error a Promise has no status: await it
    const bad: number = httpClient.async.get("https://api.example.com/x").status;
    // @ts-expect-error timeoutMs is a number
    httpClient.get("https://x", { timeoutMs: "5s" });
    // @ts-expect-error misspelled option
    httpClient.get("https://x", { header: {} });
    // @ts-expect-error url is required
    httpClient.get();
    // @ts-expect-error a body is a string or bytes
    httpClient.post("https://x", { a: 1 });
    void ct; void bad;
    return r.status + posted.status + a.body.length + b.status;
}

export async function mailUsage(): Promise<void> {
    const ok: true = await email.send({
        provider: "postmark", from: "a@example.com", to: "b@example.com",
        subject: "Hi", body: "Hello", api_key: "k", cc: ["c@example.com"],
    });
    try {
        await email.send({ from: "a@example.com", to: "b@example.com", subject: "s", body: "b",
                           smtp_host: "smtp.example.com", smtp_port: 465 });
    } catch (e) {
        const code = (e as { code?: string }).code;
        void code;
    }
    // @ts-expect-error unknown provider
    await email.send({ provider: "mailgun", from: "a", to: "b", subject: "s", body: "b" });
    // @ts-expect-error subject is required
    await email.send({ from: "a", to: "b", body: "b" });

    const res = await smtp.send({ host: "smtp.example.com", port: 587, tls: true,
                                  from: "a@example.com", to: "b@example.com",
                                  subject: "s", body: "b", cc: ["c@example.com"] });
    if (!res.ok) { const why: string = res.error; void why; }
    // @ts-expect-error cc is a list
    await smtp.send({ host: "h", from: "a", to: "b", subject: "s", body: "b", cc: "c@example.com" });
    void ok;
}

export async function sshUsage(keyText: string): Promise<number | null> {
    const trust = ssh.memoryStore();
    const opts = { host: "build.internal", user: "deploy", key: keyText, trust };
    let conn: SshConnection;
    try {
        conn = await ssh.connect(opts);
    } catch (e) {
        const err = e as SshError;
        if (err.code !== "host_unknown" || err.keyBlob === undefined) throw e;
        await ssh.acceptHost(trust, opts.host, err.keyBlob);
        conn = await sshDefault.connect(opts);
    }
    const r = await conn.exec("uname -a", { onStdout: (c) => { void c.length; }, timeoutMs: 30000 });
    const sftp = await conn.sftp({ replyTimeoutMs: 10000 });
    const listing = await sftp.list("/var/log");
    for (const e of listing.entries) { const d: boolean | undefined = e.attrs.isDir; void d; }
    await sftp.write("/tmp/x", "data");
    const f = await sftp.open("/tmp/x", "r");
    const chunk: string = await f.read(1024);
    await f.close();
    await sftp.close();
    const fp: string = conn.hostKey().fingerprint;
    const st = conn.stats();
    const key = ssh.loadKey(keyText, { passphraseEnv: "KEY_PASS" });
    const alg: string = key.algorithm;
    const pending = ssh.pendingTrust();
    void ssh.CONNECT_TIMEOUT_MS; void pending; void chunk; void fp; void st.bytesSent; void alg;
    conn.close();
    // @ts-expect-error host is required
    await ssh.connect({ user: "deploy", key: keyText });
    // @ts-expect-error open mode is one of r, r+, w, a, wx
    await sftp.open("/tmp/x", "rw");
    // @ts-expect-error exec resolves asynchronously
    const s: number | null = conn.exec("true").status;
    void s;
    return r.status;
}

export async function computeUsage(mapped: Hull.MappedBuffer): Promise<number> {
    const out: ArrayBuffer = compute.call("scorer", "input", { gas: 1000000, timeoutMs: 500 });
    const wb: Hull.WasmBuffer = compute.call("scorer", new Uint8Array(8), { buffer: true });
    const n: number = wb.length;
    const copy: ArrayBuffer | null = wb.bytes();
    wb.close();
    const spanned = compute.call("scan", "q", { spans: [{ name: "data", buffer: mapped }] });
    const asyncOut: ArrayBuffer = await compute.async.call("scorer", mapped);
    const inst: WasmInstance = compute.instance("counter", { heap: 1 << 20 });
    inst.call("x");
    await inst.async.call("y");
    const nm: string | null = inst.name;
    inst.close();
    compute.load("scorer");
    compute.segment("lookup", "table", new ArrayBuffer(4096));
    compute.segment("lookup", null);
    const streamed: ArrayBuffer = compute.stream("upper", { file: "data/in.txt" }, { chunkSize: 65536 });
    compute.stream("upper", "abc", (chunk, i, last) => { void chunk.byteLength; void i; void last; });
    compute.stream("upper", mapped, { file: "data/out.txt" });
    const ok: boolean = compute.available();
    // @ts-expect-error input is required
    compute.call("scorer");
    // @ts-expect-error misspelled option
    compute.call("scorer", "x", { timeout: 5 });
    // @ts-expect-error async.call resolves to a buffer, not a WasmBuffer field
    const len: number = compute.async.call("scorer", "x").byteLength;
    void copy; void spanned; void nm; void ok; void len;
    return out.byteLength + n + asyncOut.byteLength + streamed.byteLength;
}

export async function gpuUsage(): Promise<void> {
    if (!gpu.available()) return;
    const devs = gpu.devices();
    gpu.load("blur", { device: devs[0]?.id ?? 0 });
    gpu.compile("add", "@compute @workgroup_size(64) fn main() {}");
    const res = gpu.dispatch("add", {
        workgroups: { x: 4 },
        buffers: [{ name: "a", data: new Float32Array(256), usage: "read" },
                  { name: "out", size: 1024, usage: "readwrite" }],
        output: 1,
    });
    void res.byteLength;
    const none: true = gpu.dispatch("add", { output: false });
    const texBack: ArrayBuffer | true = gpu.dispatch("add", { output: false, outputTexture: 0 });
    void texBack;
    // @ts-expect-error -- outputTexture with output: false reads the texture back, not true
    const notTrue: true = gpu.dispatch("add", { output: false, outputTexture: 0 });
    const piped = await gpu.async.pipeline([{ shader: "a" }, { shader: "b", workgroups: { x: 2, y: 2 } }],
                                           { outputs: [{ stage: 1, buffer: 0 }] });
    void piped;
    await gpu.async.load("blur");
    const ab: ArrayBuffer = await gpu.async.dispatch("add", { uniforms: new Uint32Array([1, 2]) });
    gpu.buffer("persist", new Uint8Array(64));
    gpu.bufferCopy("persist", "other", { size: 64 });
    const back: ArrayBuffer = gpu.bufferRead("persist");
    gpu.texture("tex", image.decode(new Uint8Array(10)), { filter: "linear", wrap: "repeat" });
    const img: Hull.Image = gpu.textureRead("tex");
    void none; void ab; void back; void img.width;
    // @ts-expect-error usage is read | write | readwrite
    gpu.dispatch("add", { buffers: [{ name: "a", usage: "rw" }] });
    // @ts-expect-error the async dispatch takes no textures
    await gpu.async.dispatch("add", { textures: [] });
    // @ts-expect-error wgsl source is required
    gpu.compile("add");
}

export function imageUsage(png: ArrayBuffer): ArrayBuffer {
    const img = image.decode(png, "png");
    const w: number = img.width;
    const fmt: "rgba8" | "r8" | "rgba16float" | "r32float" = img.format;
    const px: ArrayBuffer = img.pixels();
    const raw = image.new(2, 2, "rgba8", new Uint8Array(16));
    const view = image.fromBuffer(new Uint8Array(16), 2, 2, "r8");
    const wire = image.toWasm(raw);
    const back = image.fromWasm(wire);
    const jpg = image.encode(img, "jpeg", { quality: 80 });
    img.close();
    void w; void fmt; void px; void view; void back;
    // @ts-expect-error BMP cannot be encoded
    image.encode(raw, "bmp");
    // @ts-expect-error unknown pixel format
    image.new(1, 1, "rgb8", new Uint8Array(3));
    return jpg;
}

export function tuiUsage(): number | null {
    const picked = tui.list(["one", "two", "three"], { title: "Pick" });
    const yes: boolean = tui.confirm("Continue?");
    const name: string | null = tui.input("Name: ", { maxLen: 40 });
    const [frame, next] = tui.spinner(0);
    const bar: string = tui.progress(0.5, { width: 10 });
    const result = tui.run<string>({
        draw(t) {
            t.clear();
            t.style({ bold: true, fg: 0xff8800 });
            t.print(1, 1, `${t.cols}x${t.rows} ${frame} ${bar}`);
            tui.frame({ x: 1, y: 2, w: 20, h: 5, border: "round", title: "box" }, (inner) => {
                inner.print(1, 1, "inside");
            });
        },
        onEvent(ev: TuiEvent) {
            if (ev.kind === "key" && ev.key === "q") return "quit";
            if (ev.kind === "resize") void ev.cols;
            return null;
        },
        tickMs: 100,
        mouse: true,
    });
    const caps = tui.caps();
    if (caps.clipboard) tui.clipboardSet("copied");
    const theme: "dark" | "light" | "unknown" = tui.theme();
    const ev = tui.poll(0);
    if (ev && ev.kind === "mouse") void (ev.x + ev.y + ev.button);
    tui.async(async () => { await httpClient.async.get("https://x"); });
    const ctrl: number = tui.MOD_CTRL;
    void yes; void name; void next; void result; void theme; void ctrl;
    // @ts-expect-error draw is required
    tui.run({ onEvent: () => null });
    // @ts-expect-error unknown border style
    tui.frame({ border: "dotted" });
    // @ts-expect-error print takes coordinates first
    tui.print("hello", 1, 1);
    return picked;
}
