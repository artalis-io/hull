-- hull.ssh.userauth - publickey authentication (RFC 4252).
--
-- Ed25519 public keys only, matching the host key side: one algorithm Hull
-- can do properly beats several it can half-do.
--
-- The important part of this file is signed_blob(). The session identifier
-- goes FIRST, and that placement is the whole replay defence: it binds the
-- signature to one connection, so a signature captured from an earlier
-- session cannot be presented on a new one. The blob also covers the user
-- name and the service, so a captured signature cannot be replayed as a
-- different user either. Get the contents or the order of that blob wrong and
-- authentication still works against nothing - the server simply rejects it -
-- which is why it is a pure function with its bytes asserted in tests.

local wire = require('hull.ssh.wire')

local M = {}

M.SSH_MSG_SERVICE_REQUEST  = 5
M.SSH_MSG_SERVICE_ACCEPT   = 6
M.SSH_MSG_USERAUTH_REQUEST = 50
M.SSH_MSG_USERAUTH_FAILURE = 51
M.SSH_MSG_USERAUTH_SUCCESS = 52
M.SSH_MSG_USERAUTH_BANNER  = 53

M.SERVICE_USERAUTH   = "ssh-userauth"
M.SERVICE_CONNECTION = "ssh-connection"
M.METHOD             = "publickey"
M.ALGORITHM          = "ssh-ed25519"

-- Service request ---------------------------------------------------------

function M.build_service_request(service)
    return wire.writer()
        :byte(M.SSH_MSG_SERVICE_REQUEST)
        :string(service or M.SERVICE_USERAUTH)
        :build()
end

-- Returns the accepted service name, or raises.
function M.parse_service_accept(payload)
    local r = wire.reader(payload)
    local msg = r:byte()
    if msg ~= M.SSH_MSG_SERVICE_ACCEPT then
        error("ssh.userauth: expected SERVICE_ACCEPT (6), got " .. tostring(msg))
    end
    return r:string()
end

-- The signed blob -----------------------------------------------------------

-- Exactly the bytes signed for a publickey request (RFC 4252 section 7):
--
--   string    session identifier
--   byte      SSH_MSG_USERAUTH_REQUEST
--   string    user name
--   string    service name
--   string    "publickey"
--   boolean   TRUE
--   string    public key algorithm
--   string    public key blob
--
-- The session identifier leads, which is what stops a signature from one
-- connection being replayed on another; the user name and service are in
-- there too, so one cannot be replayed as a different user or against a
-- different service.
--
-- The boolean is TRUE because this is a real attempt. RFC 4252 also defines a
-- FALSE form that asks whether a key WOULD be acceptable without signing.
-- Hull does not send it: that exists to avoid pointless signatures when an
-- agent holds many keys, and a fleet tool connects with the one key it was
-- given, so the probe would cost a round trip to learn nothing.
function M.signed_blob(session_id, user, key_blob, algorithm)
    if type(session_id) ~= "string" or session_id == "" then
        error("ssh.userauth: a session identifier is required", 2)
    end
    if type(user) ~= "string" or user == "" then
        error("ssh.userauth: a user name is required", 2)
    end
    return wire.writer()
        :string(session_id)
        :byte(M.SSH_MSG_USERAUTH_REQUEST)
        :string(user)
        :string(M.SERVICE_CONNECTION)
        :string(M.METHOD)
        :boolean(true)
        :string(algorithm or M.ALGORITHM)
        :string(key_blob)
        :build()
end

-- Request -------------------------------------------------------------------

-- The request itself. Everything after the message byte is the signed blob
-- minus its leading session identifier, plus the signature: the server
-- reconstructs what we signed from the request and its own session id, which
-- is why the two must agree field for field.
function M.build_request(user, key_blob, signature_blob, algorithm)
    return wire.writer()
        :byte(M.SSH_MSG_USERAUTH_REQUEST)
        :string(user)
        :string(M.SERVICE_CONNECTION)
        :string(M.METHOD)
        :boolean(true)
        :string(algorithm or M.ALGORITHM)
        :string(key_blob)
        :string(signature_blob)
        :build()
end

-- An ssh-ed25519 signature blob, the wrapper around the raw 64 bytes.
function M.signature_blob(raw_signature, algorithm)
    if #raw_signature ~= 64 then
        error("ssh.userauth: an Ed25519 signature is 64 bytes, got "
              .. tostring(#raw_signature), 2)
    end
    return wire.writer()
        :string(algorithm or M.ALGORITHM)
        :string(raw_signature)
        :build()
end

-- Responses -------------------------------------------------------------------

-- Strip control characters from server-supplied text.
--
-- The banner is attacker-controlled and headed for a terminal. Left alone it
-- can carry ANSI escapes that reposition the cursor or recolour output, which
-- for a fleet tool means a hostile host can forge what looks like another
-- host's result. Newline and tab survive; nothing else below 0x20 does, and
-- neither does DEL.
function M.sanitize_text(s)
    return (s:gsub("[%z\1-\8\11\12\14-\31\127]", ""))
end

-- Parse an authentication response.
--
-- Returns a table with `type` of "success", "failure" or "banner". The
-- transport filters the transport-layer messages (DISCONNECT, IGNORE, DEBUG,
-- UNIMPLEMENTED) before this sees them, so an unexpected type here is a
-- protocol error rather than something to skip.
function M.parse_response(payload)
    local r = wire.reader(payload)
    local msg = r:byte()

    if msg == M.SSH_MSG_USERAUTH_SUCCESS then
        return { type = "success" }
    end

    if msg == M.SSH_MSG_USERAUTH_FAILURE then
        local methods = r:namelist()
        local partial = r:boolean()
        -- partial success means the server accepted this method but wants
        -- another as well; it is NOT authentication, and treating it as such
        -- is how a multi-factor requirement gets silently skipped.
        return { type = "failure", methods = methods, partial = partial }
    end

    if msg == M.SSH_MSG_USERAUTH_BANNER then
        local message = r:string()
        local language = r:string()
        return { type = "banner", message = M.sanitize_text(message),
                 raw_message = message, language = language }
    end

    error("ssh.userauth: unexpected message " .. tostring(msg)
          .. " during authentication")
end

-- Whether a failure response leaves publickey worth retrying with another key.
function M.can_retry_publickey(response)
    if response.type ~= "failure" then return false end
    for _, m in ipairs(response.methods or {}) do
        if m == M.METHOD then return true end
    end
    return false
end

return M
