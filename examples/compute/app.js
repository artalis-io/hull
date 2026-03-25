// Compute example — WASM compute plugins in Hull (JavaScript)
//
// Demonstrates sync, async, buffer, and persistent instance modes.
//
// Run: hull examples/compute/app.js
// Test: hull test examples/compute

import { app } from "hull:app";
import { compute } from "hull:compute";

// Sync echo
app.get("/echo", (req, res) => {
    const input = req.query.text || "";
    const out = compute.call("echo", input);
    const bytes = new Uint8Array(out);
    let result = "";
    for (let i = 0; i < bytes.length; i++) result += String.fromCharCode(bytes[i]);
    res.json({ input, output: result, match: input === result });
});

// Async echo (yields to event loop)
app.get("/async-echo", async (req, res) => {
    const input = req.query.text || "";
    const out = await compute.async.call("echo", input);
    const bytes = new Uint8Array(out);
    let result = "";
    for (let i = 0; i < bytes.length; i++) result += String.fromCharCode(bytes[i]);
    res.json({ input, output: result, match: input === result });
});

// Buffer mode
app.get("/buffer-echo", (req, res) => {
    const input = req.query.text || "hello";
    const buf = compute.call("echo", input, { buffer: true });
    const ab = buf.bytes();
    const bytes = new Uint8Array(ab);
    let result = "";
    for (let i = 0; i < bytes.length; i++) result += String.fromCharCode(bytes[i]);
    res.json({ input, output: result, len: buf.length });
    buf.close();
});

// Persistent instance: load once, call many times
const echoInst = compute.instance("echo");

app.get("/persistent", (req, res) => {
    const input = req.query.text || "persistent";
    const out = echoInst.call(input);
    const bytes = new Uint8Array(out);
    let result = "";
    for (let i = 0; i < bytes.length; i++) result += String.fromCharCode(bytes[i]);
    res.json({ input, output: result, match: input === result });
});

// Persistent async
app.get("/persistent-async", async (req, res) => {
    const input = req.query.text || "async persistent";
    const out = await echoInst.async.call(input);
    const bytes = new Uint8Array(out);
    let result = "";
    for (let i = 0; i < bytes.length; i++) result += String.fromCharCode(bytes[i]);
    res.json({ input, output: result, match: input === result });
});

// Persistent instance info
app.get("/persistent-info", (_req, res) => {
    res.json({ name: echoInst.name, closed: echoInst.closed });
});

app.get("/health", (_req, res) => {
    res.json({ ok: true });
});
