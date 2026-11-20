// hull:_refusal - the runtime's refusals to start or wait on an async op,
// recognised by their code (internal: the underscore keeps it out of app
// code; apps read the same `e.code`, docs/app_api_reference.md).
//
// A refusal is an Error with an own `code` (include/hull/shared/refusal.h):
//   "wait_refused"  the op cannot wait here (a transaction is open,
//                   middleware, a parked req.multipart() read): a bug or a
//                   misplaced call in the caller.
//   "busy"          an admission cap of this VM is full (64 fetches in
//                   flight, the off-heap copy total, the live WebSocket /
//                   blob handle caps): local and transient, nothing was sent.
// Neither says anything about the remote side, so a caller must not read one
// as a remote failure (audit 35: pwned failed open on the fetch cap, outbox
// counted it as a failed delivery). The stdlib tests the code, never the
// message text.

const WAIT = "wait_refused";
const BUSY = "busy";

const hasOwn = Object.prototype.hasOwnProperty;

// The refusal code of a caught error, or null when it is not a refusal. An
// error hull:ssh raised for its own connection (an own `ssh` marker) is not
// one, whatever its code: its "busy" says THIS connection is in use by another
// task, after the command may already have run, and a caller that took it for
// an admission refusal (hull:jobs hands those back uncounted) re-ran it.
function code(e) {
    if (e === null || typeof e !== "object" || !hasOwn.call(e, "code")) return null;
    if (hasOwn.call(e, "ssh")) return null;
    const c = e.code;
    return c === WAIT || c === BUSY ? c : null;
}

function isWait(e) { return code(e) === WAIT; }
function isBusy(e) { return code(e) === BUSY; }

const DENIED = "denied";

// True when e is the runtime refusing a host manifest.hosts does not admit
// (an own `code` "denied", gap 3). Not a code() answer: the same call is
// denied again, so a caller that hands refusals back must count it. A caller
// that fails closed on a misconfiguration (pwned) or must not repeat a
// deterministic failure (retry.run) tests it here (audit 50).
function isDenied(e) {
    if (e === null || typeof e !== "object" || !hasOwn.call(e, "code")) return false;
    if (hasOwn.call(e, "ssh")) return false;
    return e.code === DENIED;
}

export const _refusal = { WAIT, BUSY, DENIED, code, isWait, isBusy, isDenied };
export default _refusal;
