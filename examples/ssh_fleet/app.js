// ssh_fleet - run one command across a list of hosts, over hull/ssh.
//
// The JS twin of app.lua; read that file's header for why each piece is
// there. In short: the manifest is the allowlist, host keys are never trusted
// automatically, the trust store is an OpenSSH known_hosts file, and a
// non-zero remote exit is a status, not an error.
//
//   hull run app.js -- web1.internal web2.internal
//   HULL_SSH_COMMAND="systemctl is-active app" hull run app.js -- web1.internal
//   hull run app.js -- --accept-new web1.internal
//   HULL_SSH_TUNNEL=relay.example.com:443 hull run app.js -- web1.internal
//
// The JS API differs from the Lua one only where JS does: names are
// camelCase, a failure is a rejection carrying `.code` (Lua returns nil, err),
// and command output is a byte string (one character per byte).

import { app } from "hull:app";
import { fs } from "hull:fs";
import { ssh } from "hull:ssh";

app.manifest({
    modules: ["hull/ssh@1", "hull/fs@1"],
    env: ["HULL_SSH_KEY", "HULL_SSH_USER", "HULL_SSH_COMMAND",
          "HULL_SSH_TUNNEL", "HULL_SSH_KEY_PASSPHRASE"],
    fs: {
        read: ["id_ed25519", "known_hosts"],
        write: ["known_hosts"],
    },
    ssh: {
        connect: {
            hosts: ["*.internal", "127.0.0.1"],
            ports: [22],
            users: ["deploy", "root"],
        },
        tunnel: { hosts: ["*.example.com"], ports: [443] },
    },
});

const TRUST_FILE = "known_hosts";

// "host:port", TLS always on (see app.lua).
function parseTunnel(spec) {
    if (!spec) return undefined;
    const m = /^([^:]+):(\d+)$/.exec(spec);
    if (!m) throw new Error("HULL_SSH_TUNNEL must look like relay.example.com:443");
    return { host: m[1], port: Number(m[2]), tls: true, headers: [] };
}

async function connect(host, opts) {
    return ssh.connect({
        host, port: 22, user: opts.user, key: opts.key,
        trust: opts.trust, tunnel: opts.tunnel,
    });
}

async function runOne(ctx, host, opts) {
    let conn;
    try {
        conn = await connect(host, opts);
    } catch (first) {
        let err = first;
        if (err.code === "host_unknown") {
            ctx.stderr.write(`${host}: unknown host key ${err.fingerprint}\n`);
            if (!opts.acceptNew) {
                ctx.stderr.write("  re-run with --accept-new to trust it\n");
                return 1;
            }
            try {
                await ssh.acceptHost(opts.trust, host, err.keyBlob, 22);
            } catch (aerr) {
                ctx.stderr.write(`${host}: could not record its key: ${String(aerr.detail || aerr.code)}\n`);
                return 1;
            }
            try {
                conn = await connect(host, opts);
            } catch (err2) {
                err = err2;
            }
        }
        if (!conn) {
            // host_changed is the one that means STOP.
            if (err.code === "host_changed") {
                ctx.stderr.write(`${host}: HOST KEY CHANGED - was ${String(err.storedFingerprint)}, now ${String(err.fingerprint)}\n`);
                return 2;
            }
            ctx.stderr.write(`${host}: ${String(err.code)}${err.detail ? ` (${String(err.detail)})` : ""}\n`);
            return 1;
        }
    }

    let r;
    try {
        r = await conn.exec(opts.command);
    } catch (rerr) {
        conn.close();
        ctx.stderr.write(`${host}: ${String(rerr.code)}\n`);
        return 1;
    }

    for (const line of (r.stdout || "").split("\n")) {
        if (line) ctx.stdout.write(`${host}: ${line}\n`);
    }
    for (const line of (r.stderr || "").split("\n")) {
        if (line) ctx.stderr.write(`${host}: ${line}\n`);
    }

    const stats = conn.stats();
    ctx.stderr.write(`${host}: exit ${r.status}, ${conn.negotiated().cipherC2s}, ${stats.bytesSent + stats.bytesReceived} bytes, ${stats.rekeys} rekey(s)\n`);

    conn.close();
    return r.status;
}

function usage(stderr) {
    stderr.write("usage: hull run app.js -- [--accept-new] HOST...\n");
    stderr.write("  HULL_SSH_KEY      private key path (default id_ed25519)\n");
    stderr.write("  HULL_SSH_USER     login (default deploy)\n");
    stderr.write("  HULL_SSH_COMMAND  command (default uname -a)\n");
    stderr.write("  HULL_SSH_TUNNEL   relay host:port for a WebSocket tunnel\n");
}

app.main(async (ctx) => {
    const hosts = [];
    let acceptNew = false;
    for (const a of ctx.args) {
        if (a === "--accept-new") {
            acceptNew = true;
        } else if (a === "-h" || a === "--help") {
            usage(ctx.stderr);
            return 0;
        } else if (a.startsWith("-")) {
            ctx.stderr.write(`unknown flag: ${a}\n`);
            return 2;
        } else {
            hosts.push(a);
        }
    }
    if (hosts.length === 0) {
        usage(ctx.stderr);
        return 2;
    }

    const keyFile = ctx.env.HULL_SSH_KEY || "id_ed25519";
    let keyText;
    try {
        keyText = fs.read(keyFile);
    } catch (_e) {
        ctx.stderr.write(`cannot read ${keyFile} (add it to manifest.fs.read)\n`);
        return 2;
    }

    // The passphrase is NAMED, not carried: passphraseEnv hands the variable
    // NAME to C, which reads it, derives the key and scrubs the buffer.
    let key;
    try {
        key = ssh.loadKey(keyText, {
            passphraseEnv: ctx.env.HULL_SSH_KEY_PASSPHRASE ? "HULL_SSH_KEY_PASSPHRASE" : undefined,
        });
    } catch (e) {
        ctx.stderr.write(`cannot load ${keyFile}: ${String(e?.message)}\n`);
        return 2;
    }

    const opts = {
        key,
        user: ctx.env.HULL_SSH_USER || "deploy",
        command: ctx.env.HULL_SSH_COMMAND || "uname -a",
        tunnel: parseTunnel(ctx.env.HULL_SSH_TUNNEL),
        trust: ssh.fileStore(TRUST_FILE),
        acceptNew,
    };

    // Concurrently, at most 8 at a time; the worst status wins.
    const statuses = await hull.map(hosts, (host) => runOne(ctx, host, opts), { limit: 8 });
    return statuses.reduce((w, s) => (s > w ? s : w), 0);
});
