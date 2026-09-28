-- test_ssh_known_hosts.lua - Tests for hull.ssh.known_hosts and ssh.file_store
--
-- The codec against lines in OpenSSH's own format, and the store over a fake
-- hull/fs, so the file round trip is exercised without a filesystem.

local kh     = require('hull.ssh.known_hosts')
local wire   = require('hull.ssh.wire')
local enc = require('hull.encoding')
local base64 = enc.base64
local ssh    = require('hull.ssh')

local pass = 0
local fail = 0

local function test(name, fn)
    local ok, err = pcall(fn)
    if ok then
        pass = pass + 1
    else
        fail = fail + 1
        print("FAIL: " .. name .. ": " .. tostring(err))
    end
end

local function assert_eq(a, b, msg)
    if a ~= b then
        error((msg or "") .. " expected " .. tostring(b) .. ", got " .. tostring(a))
    end
end

-- A wire-format key blob of the given type.
local function blob(keytype, byte)
    return wire.writer():string(keytype):string(string.rep(byte or "\1", 32)):build()
end

local ED_A, ED_B = blob("ssh-ed25519", "\1"), blob("ssh-ed25519", "\2")
local RSA = blob("ssh-rsa", "\3")

local function line(hosts, b)
    return hosts .. " " .. kh.blob_type(b) .. " " .. base64.encode(b)
end

-- A deterministic stand-in for HMAC-SHA1: 20 bytes depending on key and msg.
-- What is under test is the known_hosts plumbing; the real HMAC is
-- hull.crypto's, tested with it.
local function fake_hmac(key, msg)
    local s = key .. "|" .. msg
    local out = {}
    for i = 1, 20 do
        local acc = i
        for j = 1, #s do acc = (acc * 31 + s:byte(j) + i) % 251 end
        out[i] = string.char(acc)
    end
    return table.concat(out)
end

-- codec -------------------------------------------------------------------------

test("a plain line lists its names, lower-cased", function()
    local e = kh.parse_line(line("Web1,[web1]:2222,10.0.0.7", ED_A) .. " a comment")
    assert_eq(#e.names, 3)
    assert_eq(e.names[1], "web1")
    assert_eq(e.names[2], "[web1]:2222")
    assert_eq(e.keytype, "ssh-ed25519")
    assert_eq(e.blob, ED_A)
    assert_eq(kh.matches(e, "[web1]:2222", fake_hmac), true)
    assert_eq(kh.matches(e, "web2", fake_hmac), false)
end)

test("comments, markers, blanks and malformed lines are skipped", function()
    for _, l in ipairs({ "", "   ", "# a comment",
                         "@cert-authority *.example.com " .. line("x", ED_A):match(" (.*)"),
                         "@revoked " .. line("web1", ED_A),
                         "web1 ssh-ed25519 !!!not-base64!!!",
                         "web1 ssh-ed25519",
                         -- the blob says rsa, the line says ed25519
                         "web1 ssh-ed25519 " .. base64.encode(RSA) }) do
        assert_eq(kh.parse_line(l), nil, l)
    end
end)

test("patterns are skipped, so a host covered only by one is unknown", function()
    -- Never wrongly trusted: a name matches only itself.
    assert_eq(kh.parse_line(line("*.example.com", ED_A)), nil)
    local e = kh.parse_line(line("web1,*.internal,!bastion", ED_A))
    assert_eq(#e.names, 1)
    assert_eq(kh.matches(e, "db.internal", fake_hmac), false)
end)

test("a hashed name matches only the name it hashes", function()
    local salt = string.rep("s", 20)
    local l = kh.render("web1", ED_A, salt, fake_hmac)
    assert_eq(l:sub(1, 3), "|1|")
    local e = kh.parse_line(l)
    assert_eq(e.names, nil)
    assert_eq(kh.matches(e, "web1", fake_hmac), true)
    assert_eq(kh.matches(e, "web2", fake_hmac), false)
end)

test("a plain line renders the way OpenSSH writes it", function()
    local l = kh.render("[web1]:2222", ED_A)
    assert_eq(l, "[web1]:2222 ssh-ed25519 " .. base64.encode(ED_A))
    assert_eq(kh.parse_line(l).blob, ED_A)
end)

test("without drops a name, keeps its neighbours and every other line", function()
    local text = "# keep me\n"
              .. line("web1,web2", ED_A) .. "\n"
              .. line("web3", ED_B) .. "\n"
              .. kh.render("web1", ED_B, string.rep("s", 20), fake_hmac) .. "\n"
    local out = kh.without(text, "web1", fake_hmac)
    assert_eq(out, "# keep me\n" .. line("web2", ED_A) .. "\n"
                   .. line("web3", ED_B) .. "\n")
end)

-- file_store ---------------------------------------------------------------------

local function fake_fs(initial)
    local fs = { files = { kh = initial }, writes = 0 }
    fs.read = function(p) return fs.files[p] end
    fs.write = function(p, text)
        if fs.deny then return nil, "fs.write: not permitted" end
        fs.writes = fs.writes + 1
        fs.files[p] = text
        return true
    end
    return fs
end

local fake_crypto = {
    hmac_sha1 = function(msg, key_hex)
        return enc.hex.encode(fake_hmac(enc.hex.decode(key_hex), msg))
    end,
    random = function(n) return string.rep("r", n) end,
}

local function store(fs, opts)
    opts = opts or {}
    opts.fs, opts.crypto = fs, fake_crypto
    return ssh.file_store("kh", opts)
end

test("a missing file is an empty store, created by the first accept", function()
    local fs = fake_fs(nil)
    local st = store(fs)
    assert_eq(st.get("web1"), nil)
    assert_eq(ssh.accept_host(st, "web1", ED_A), true)
    assert_eq(fs.files.kh, line("web1", ED_A) .. "\n")
    assert_eq(st.get("web1"), ED_A)
end)

test("a key accepted off port 22 is filed as [host]:port", function()
    local fs = fake_fs("")
    local st = store(fs)
    ssh.accept_host(st, "Web1", ED_A, 2222)
    assert_eq(fs.files.kh, line("[web1]:2222", ED_A) .. "\n")
    assert_eq(st.get("web1"), nil, "port 22 is a different host:")
end)

test("an existing known_hosts is read, other key types ignored", function()
    local fs = fake_fs(line("web1", RSA) .. "\n" .. line("web1,web2", ED_B) .. "\n")
    local st = store(fs)
    assert_eq(st.get("web1"), ED_B)
    local e = st.entries()
    assert_eq(e.web1, ED_B)
    assert_eq(e.web2, ED_B)
end)

test("a file that does not end in a newline is appended to cleanly", function()
    local fs = fake_fs(line("web1", ED_A))
    store(fs).put("web2", ED_B)
    assert_eq(fs.files.kh, line("web1", ED_A) .. "\n" .. line("web2", ED_B) .. "\n")
end)

test("hashed mode writes a hashed entry that it can find again", function()
    local fs = fake_fs("")
    local st = store(fs, { hash = true })
    st.put("web1", ED_A)
    assert_eq(fs.files.kh:sub(1, 3), "|1|")
    assert_eq(fs.files.kh:find("web1", 1, true), nil, "the name is not in the file:")
    assert_eq(st.get("web1"), ED_A)
    assert_eq(next(st.entries()), nil, "a hashed name cannot be listed:")
end)

test("forget_host removes the entry and leaves the rest", function()
    local fs = fake_fs(line("web1,web2", ED_A) .. "\n")
    local st = store(fs)
    assert_eq(ssh.forget_host(st, "web1"), true)
    assert_eq(st.get("web1"), nil)
    assert_eq(st.get("web2"), ED_A)
end)

test("a store that cannot write says store_failed, not a raise", function()
    local fs = fake_fs("")
    fs.deny = true
    local ok, err = ssh.accept_host(store(fs), "web1", ED_A)
    assert_eq(ok, nil)
    assert_eq(err.code, "store_failed")
end)

-- kv_store -------------------------------------------------------------------------
--
-- Over a stand-in with hull.kv's semantics (cas(k, nil, v) is set-if-absent and
-- returns false when k exists; scan(prefix) lists keys). hull.kv itself needs
-- the runtime; test_lua.c exercises the real one.

local function fake_kv(caps)
    local data = {}
    local kv = { data = data,
                 caps = caps or { compare_exchange = true, scan = true } }
    function kv:get(k) return data[k] end
    function kv:delete(k) data[k] = nil; return true end
    function kv:cas(k, expected, new)
        if data[k] ~= expected then return false end
        data[k] = new
        return true
    end
    function kv:scan(prefix)
        local out = {}
        for k in pairs(data) do
            if k:sub(1, #prefix) == prefix then out[#out + 1] = k end
        end
        return out
    end
    return kv
end

test("kv_store keeps keys under its prefix, by known_hosts name", function()
    local kv = fake_kv()
    local st = ssh.kv_store(kv)
    assert_eq(ssh.accept_host(st, "Web1", ED_A, 2222), true)
    assert_eq(kv.data["hostkey:[web1]:2222"], ED_A)
    assert_eq(st.get("[web1]:2222"), ED_A)
end)

test("kv_store will not overwrite a key, even under a race", function()
    -- The ordinary case, and the race: a worker whose get() saw nothing, but
    -- another recorded a key before its write. The write is set-if-absent, so
    -- the first key stands and the second worker is told.
    local kv = fake_kv()
    local st = ssh.kv_store(kv)
    ssh.accept_host(st, "web1", ED_A)
    local ok, err = ssh.accept_host(st, "web1", ED_B)
    assert_eq(ok, nil)
    assert_eq(err.code, "already_trusted")

    local racing = ssh.kv_store(kv)
    local stale_get = racing.get
    racing.get = function() return nil end            -- saw nothing...
    ok, err = ssh.accept_host(racing, "web1", ED_B)   -- ...but lost the write
    racing.get = stale_get
    assert_eq(ok, nil)
    assert_eq(err.code, "already_trusted")
    assert_eq(kv.data["hostkey:web1"], ED_A, "the first key stands:")
end)

test("kv_store shares a namespace: entries lists only its own keys", function()
    local kv = fake_kv()
    kv.data["session:abc"] = "not a host key"
    local st = ssh.kv_store(kv)
    ssh.accept_host(st, "web1", ED_A)
    ssh.accept_host(st, "web2", ED_B)
    local e = st.entries()
    assert_eq(e.web1, ED_A)
    assert_eq(e.web2, ED_B)
    assert_eq(e["session:abc"], nil)
    assert_eq(ssh.forget_host(st, "web1"), true)
    assert_eq(st.get("web1"), nil)
    assert_eq(kv.data["session:abc"], "not a host key", "untouched:")
end)

test("kv_store refuses a backend without compare-and-swap", function()
    local ok = pcall(ssh.kv_store, fake_kv({ scan = true }))
    assert_eq(ok, false)
end)

test("kv_store takes its own prefix", function()
    local kv = fake_kv()
    local st = ssh.kv_store(kv, { prefix = "fleet-a/" })
    ssh.accept_host(st, "web1", ED_A)
    assert_eq(kv.data["fleet-a/web1"], ED_A)
end)

return {pass = pass, fail = fail}
