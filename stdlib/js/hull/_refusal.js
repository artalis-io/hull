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

// The refusal code of a caught error, or null when it is not a refusal.
function code(e) {
    if (e === null || typeof e !== "object" || !hasOwn.call(e, "code")) return null;
    const c = e.code;
    return c === WAIT || c === BUSY ? c : null;
}

function isWait(e) { return code(e) === WAIT; }
function isBusy(e) { return code(e) === BUSY; }

export const _refusal = { WAIT, BUSY, code, isWait, isBusy };
export default _refusal;
