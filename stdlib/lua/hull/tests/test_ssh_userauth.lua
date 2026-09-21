-- test_ssh_userauth.lua - Tests for hull.ssh.userauth
--
-- RFC 4252. The signed-blob cases are the point: the session identifier binds
-- a signature to one connection, and the user name and service bind it to one
-- identity. Lose any of that and a captured signature becomes reusable.

local userauth = require('hull.ssh.userauth')
local wire = require('hull.ssh.wire')

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

local function assert_raises(fn, msg)
    local ok = pcall(fn)
    if ok then error((msg or "should have raised") .. " but did not") end
end

local KEY_BLOB = wire.writer():string("ssh-ed25519")
                              :string(string.rep("\1", 32)):build()
local SIG = string.rep("\9", 64)

-- service request ------------------------------------------------------------

test("service request names ssh-userauth", function()
    local r = wire.reader(userauth.build_service_request())
    assert_eq(r:byte(), userauth.SSH_MSG_SERVICE_REQUEST)
    assert_eq(r:string(), "ssh-userauth")
end)

test("service accept round trips", function()
    local p = wire.writer():byte(6):string("ssh-userauth"):build()
    assert_eq(userauth.parse_service_accept(p), "ssh-userauth")
end)

test("service accept rejects another message", function()
    local p = wire.writer():byte(7):string("ssh-userauth"):build()
    assert_raises(function() userauth.parse_service_accept(p) end, "wrong type")
end)

-- the signed blob --------------------------------------------------------------

test("signed blob is the RFC 4252 section 7 order", function()
    local blob = userauth.signed_blob("SESSION", "operator", KEY_BLOB)
    local r = wire.reader(blob)
    assert_eq(r:string(), "SESSION")
    assert_eq(r:byte(), userauth.SSH_MSG_USERAUTH_REQUEST)
    assert_eq(r:string(), "operator")
    assert_eq(r:string(), "ssh-connection")
    assert_eq(r:string(), "publickey")
    assert_eq(r:boolean(), true)
    assert_eq(r:string(), "ssh-ed25519")
    assert_eq(r:string(), KEY_BLOB)
    assert_eq(r:remaining(), 0)
end)

test("the session identifier comes first", function()
    -- This is the replay defence: it is what makes the signature specific to
    -- one connection.
    local blob = userauth.signed_blob("SESSION", "operator", KEY_BLOB)
    assert_eq(wire.reader(blob):string(), "SESSION")
end)

test("a different session gives a different blob to sign", function()
    -- A signature captured from one connection must not verify on another.
    local a = userauth.signed_blob("SESSION-A", "operator", KEY_BLOB)
    local b = userauth.signed_blob("SESSION-B", "operator", KEY_BLOB)
    assert_eq(a ~= b, true)
end)

test("a different user gives a different blob to sign", function()
    -- So a captured signature cannot be replayed as somebody else.
    local a = userauth.signed_blob("SESSION", "operator", KEY_BLOB)
    local b = userauth.signed_blob("SESSION", "root", KEY_BLOB)
    assert_eq(a ~= b, true)
end)

test("a different key gives a different blob to sign", function()
    local other = wire.writer():string("ssh-ed25519")
                               :string(string.rep("\2", 32)):build()
    local a = userauth.signed_blob("SESSION", "operator", KEY_BLOB)
    local b = userauth.signed_blob("SESSION", "operator", other)
    assert_eq(a ~= b, true)
end)

test("signed blob refuses a missing session or user", function()
    assert_raises(function()
        userauth.signed_blob(nil, "operator", KEY_BLOB)
    end, "no session")
    assert_raises(function()
        userauth.signed_blob("", "operator", KEY_BLOB)
    end, "empty session")
    assert_raises(function()
        userauth.signed_blob("SESSION", "", KEY_BLOB)
    end, "empty user")
end)

test("the signed boolean is TRUE, not the query form", function()
    -- FALSE would be the "would this key be acceptable" probe, which Hull
    -- does not send: it costs a round trip to learn nothing when there is
    -- exactly one key.
    local r = wire.reader(userauth.signed_blob("S", "u", KEY_BLOB))
    r:string(); r:byte(); r:string(); r:string(); r:string()
    assert_eq(r:boolean(), true)
end)

-- the request -------------------------------------------------------------------

test("request mirrors the signed blob field for field", function()
    -- The server rebuilds what we signed from the request plus its own
    -- session id, so the two have to agree exactly.
    local sig_blob = userauth.signature_blob(SIG)
    local req = userauth.build_request("operator", KEY_BLOB, sig_blob)
    local signed = userauth.signed_blob("SESSION", "operator", KEY_BLOB)

    local rq = wire.reader(req)
    local sg = wire.reader(signed)
    assert_eq(sg:string(), "SESSION")            -- only in the signed blob

    assert_eq(rq:byte(), sg:byte())              -- message number
    assert_eq(rq:string(), sg:string())          -- user
    assert_eq(rq:string(), sg:string())          -- service
    assert_eq(rq:string(), sg:string())          -- method
    assert_eq(rq:boolean(), sg:boolean())        -- TRUE
    assert_eq(rq:string(), sg:string())          -- algorithm
    assert_eq(rq:string(), sg:string())          -- key blob
    assert_eq(rq:string(), sig_blob)             -- and then the signature
    assert_eq(rq:remaining(), 0)
    assert_eq(sg:remaining(), 0)
end)

test("signature blob wraps the raw signature", function()
    local b = userauth.signature_blob(SIG)
    local r = wire.reader(b)
    assert_eq(r:string(), "ssh-ed25519")
    assert_eq(r:string(), SIG)
end)

test("signature blob refuses the wrong length", function()
    assert_raises(function()
        userauth.signature_blob(string.rep("\9", 63))
    end, "63 bytes")
    assert_raises(function()
        userauth.signature_blob(string.rep("\9", 65))
    end, "65 bytes")
end)

-- responses ----------------------------------------------------------------------

test("success is recognised", function()
    local p = wire.writer():byte(52):build()
    assert_eq(userauth.parse_response(p).type, "success")
end)

test("failure carries the methods that can continue", function()
    local p = wire.writer():byte(51)
        :namelist({ "publickey", "password" }):boolean(false):build()
    local got = userauth.parse_response(p)
    assert_eq(got.type, "failure")
    assert_eq(#got.methods, 2)
    assert_eq(got.partial, false)
end)

test("partial success is a failure, not a success", function()
    -- The server accepted this method but wants another as well. Treating it
    -- as authentication is how a second factor gets silently skipped.
    local p = wire.writer():byte(51):namelist({ "publickey" }):boolean(true):build()
    local got = userauth.parse_response(p)
    assert_eq(got.type, "failure")
    assert_eq(got.partial, true)
end)

test("can_retry_publickey reads the continue list", function()
    local again = userauth.parse_response(
        wire.writer():byte(51):namelist({ "publickey" }):boolean(false):build())
    assert_eq(userauth.can_retry_publickey(again), true)

    local done = userauth.parse_response(
        wire.writer():byte(51):namelist({ "password" }):boolean(false):build())
    assert_eq(userauth.can_retry_publickey(done), false)

    local ok = userauth.parse_response(wire.writer():byte(52):build())
    assert_eq(userauth.can_retry_publickey(ok), false)
end)

test("an unexpected message is a protocol error", function()
    local p = wire.writer():byte(99):build()
    assert_raises(function() userauth.parse_response(p) end, "message 99")
end)

test("a truncated failure is refused", function()
    local p = wire.writer():byte(51):build()
    assert_raises(function() userauth.parse_response(p) end, "truncated")
end)

-- banners -------------------------------------------------------------------------

test("banner text is returned", function()
    local p = wire.writer():byte(53):string("Authorised use only"):string("en"):build()
    local got = userauth.parse_response(p)
    assert_eq(got.type, "banner")
    assert_eq(got.message, "Authorised use only")
    assert_eq(got.language, "en")
end)

test("banner escapes are stripped", function()
    -- The banner is attacker-controlled and headed for a terminal. ANSI
    -- escapes there let a hostile host forge what looks like another host
    -- result in a fleet tool output.
    local nasty = "ok\27[2J\27[Hall clear\7"
    local p = wire.writer():byte(53):string(nasty):string(""):build()
    local got = userauth.parse_response(p)
    assert_eq(got.message:find("\27", 1, true), nil, "escape survived")
    assert_eq(got.message:find("\7", 1, true), nil, "bell survived")
    assert_eq(got.message, "ok[2J[Hall clear")
    -- the original is still available for a caller that wants to log bytes
    assert_eq(got.raw_message, nasty)
end)

test("banner keeps newline and tab", function()
    -- Stripping those would mangle legitimate multi-line banners.
    local p = wire.writer():byte(53):string("line one\nline\ttwo"):string(""):build()
    assert_eq(userauth.parse_response(p).message, "line one\nline\ttwo")
end)

test("sanitize removes NUL and DEL", function()
    assert_eq(userauth.sanitize_text("a\0b\127c"), "abc")
end)

-- Return results for C test harness
return {pass = pass, fail = fail}
