/*
 * refusal.h - stable, machine-readable identity for the runtime's
 * refusals to start or wait on an async op (audit 35).
 *
 * The stdlib used to recognise these refusals by their message text, so a
 * new refusal (audit 31's per-VM fetch cap) read as a remote failure: pwned
 * failed open on it, outbox counted it as a failed delivery. Every refusal
 * now carries a code:
 *
 *   HL_REFUSAL_WAIT  "wait_refused"  the op cannot wait HERE (a transaction
 *                    is open, middleware, a parked multipart read, not in a
 *                    handler / task, the runtime is shutting down) - a bug
 *                    or a misplaced call in the caller, not a remote answer.
 *   HL_REFUSAL_BUSY  "busy"          an admission cap of this VM is full (the
 *                    in-flight fetch cap, the off-heap copy total, the live
 *                    WebSocket / blob handle caps) - transient and local:
 *                    nothing was sent, a later call may be admitted.
 *
 * JS: the thrown Error gets an own `code` property (hl_js_refusal_tag,
 * runtime/js/refusal.h). Lua: the error string starts with
 * "[hull:<code>] " (the HL_LUA_REFUSAL_* literals, concatenated into the
 * luaL_error format). The stdlib tests them through hull._refusal /
 * hull:_refusal, never by message text. A NEW refusal must carry one.
 */
#ifndef HULL_SHARED_REFUSAL_H
#define HULL_SHARED_REFUSAL_H

#define HL_REFUSAL_WAIT "wait_refused"
#define HL_REFUSAL_BUSY "busy"

#define HL_LUA_REFUSAL_WAIT "[hull:wait_refused] "
#define HL_LUA_REFUSAL_BUSY "[hull:busy] "

#endif /* HULL_SHARED_REFUSAL_H */
