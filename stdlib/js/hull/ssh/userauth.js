// hull:ssh:userauth - publickey authentication (RFC 4252).
//
// The JS twin of hull.ssh.userauth (stdlib/lua/hull/ssh/userauth.lua); see
// there for the reasoning. The important part is signedBlob(): the session
// identifier goes FIRST, which binds the signature to one connection.

import { wire } from "hull:ssh:wire";

const SSH_MSG_SERVICE_REQUEST  = 5;
const SSH_MSG_SERVICE_ACCEPT   = 6;
const SSH_MSG_USERAUTH_REQUEST = 50;
const SSH_MSG_USERAUTH_FAILURE = 51;
const SSH_MSG_USERAUTH_SUCCESS = 52;
const SSH_MSG_USERAUTH_BANNER  = 53;

const SERVICE_USERAUTH   = "ssh-userauth";
const SERVICE_CONNECTION = "ssh-connection";
const METHOD             = "publickey";
const ALGORITHM          = "ssh-ed25519";

// Service request ---------------------------------------------------------

function buildServiceRequest(service) {
    return wire.writer()
        .byte(SSH_MSG_SERVICE_REQUEST)
        .string(service || SERVICE_USERAUTH)
        .build();
}

// Returns the accepted service name, or throws.
function parseServiceAccept(payload) {
    const r = wire.reader(payload);
    const msg = r.byte();
    if (msg !== SSH_MSG_SERVICE_ACCEPT) {
        throw new Error("ssh.userauth: expected SERVICE_ACCEPT (6), got " + String(msg));
    }
    return r.string();
}

// The signed blob ----------------------------------------------------------

// Exactly the bytes signed for a publickey request (RFC 4252 section 7).
function signedBlob(sessionId, user, keyBlob, algorithm) {
    if (typeof sessionId !== "string" || sessionId === "") {
        throw new TypeError("ssh.userauth: a session identifier is required");
    }
    if (typeof user !== "string" || user === "") {
        throw new TypeError("ssh.userauth: a user name is required");
    }
    return wire.writer()
        .string(sessionId)
        .byte(SSH_MSG_USERAUTH_REQUEST)
        .string(user)
        .string(SERVICE_CONNECTION)
        .string(METHOD)
        .boolean(true)
        .string(algorithm || ALGORITHM)
        .string(keyBlob)
        .build();
}

// Request -------------------------------------------------------------------

function buildRequest(user, keyBlob, signatureBlob, algorithm) {
    return wire.writer()
        .byte(SSH_MSG_USERAUTH_REQUEST)
        .string(user)
        .string(SERVICE_CONNECTION)
        .string(METHOD)
        .boolean(true)
        .string(algorithm || ALGORITHM)
        .string(keyBlob)
        .string(signatureBlob)
        .build();
}

// A signature blob: the algorithm name around the raw signature. ECDSA r || s
// goes on the wire as mpint r, mpint s (RFC 5656 section 3.1.2).
function signatureBlob(rawSignature, algorithm) {
    algorithm = algorithm || ALGORITHM;
    if (algorithm === ALGORITHM && rawSignature.length !== 64) {
        throw new RangeError("ssh.userauth: an Ed25519 signature is 64 bytes, got "
            + String(rawSignature.length));
    }
    if (rawSignature.length === 0) {
        throw new RangeError("ssh.userauth: empty signature");
    }
    if (algorithm.startsWith("ecdsa-sha2-")) {
        if (rawSignature.length % 2 !== 0) {
            throw new RangeError("ssh.userauth: an ECDSA signature is r || s of equal widths");
        }
        const half = rawSignature.length / 2;
        rawSignature = wire.writer().mpint(rawSignature.slice(0, half))
            .mpint(rawSignature.slice(half)).build();
    }
    return wire.writer()
        .string(algorithm)
        .string(rawSignature)
        .build();
}

// Responses -----------------------------------------------------------------

// Parse an authentication response: an object with `type` of "success",
// "failure" or "banner". An unexpected type is a protocol error.
function parseResponse(payload) {
    const r = wire.reader(payload);
    const msg = r.byte();

    if (msg === SSH_MSG_USERAUTH_SUCCESS) {
        return { type: "success" };
    }

    if (msg === SSH_MSG_USERAUTH_FAILURE) {
        const methods = r.namelist();
        const partial = r.boolean();
        // partial success is NOT authentication.
        return { type: "failure", methods, partial };
    }

    if (msg === SSH_MSG_USERAUTH_BANNER) {
        const message = r.string();
        const language = r.string();
        return { type: "banner", message: wire.safeText(message),
                 rawMessage: message, language };
    }

    throw new Error("ssh.userauth: unexpected message " + String(msg)
        + " during authentication");
}

// Whether a failure response leaves publickey worth retrying with another key.
function canRetryPublickey(response) {
    if (response.type !== "failure") return false;
    for (const m of (response.methods || [])) {
        if (m === METHOD) return true;
    }
    return false;
}

export const userauth = {
    SSH_MSG_SERVICE_REQUEST, SSH_MSG_SERVICE_ACCEPT, SSH_MSG_USERAUTH_REQUEST,
    SSH_MSG_USERAUTH_FAILURE, SSH_MSG_USERAUTH_SUCCESS, SSH_MSG_USERAUTH_BANNER,
    SERVICE_USERAUTH, SERVICE_CONNECTION, METHOD, ALGORITHM,
    buildServiceRequest, parseServiceAccept, signedBlob, buildRequest,
    signatureBlob, parseResponse, canRetryPublickey,
};
export default userauth;
