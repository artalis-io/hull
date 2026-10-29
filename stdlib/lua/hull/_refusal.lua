-- hull._refusal - the runtime's refusals to start or wait on an async op,
-- recognised by their code (internal: the underscore keeps it out of app
-- code; apps test the same prefix, docs/app_api_reference.md).
--
-- A refusal is raised as a string that starts with "[hull:<code>] "
-- (include/hull/shared/refusal.h):
--   "wait_refused"  the op cannot wait here (a transaction is open, not in a
--                   handler / task / app.main, the runtime is shutting down):
--                   a bug or a misplaced call in the caller.
--   "busy"          an admission cap of this VM is full (64 fetches in
--                   flight, the off-heap copy total, the live WebSocket /
--                   blob handle caps): local and transient, nothing was sent.
-- Neither says anything about the remote side, so a caller must not read one
-- as a remote failure (audit 35: pwned failed open on the fetch cap, outbox
-- counted it as a failed delivery). The stdlib tests the code, never the
-- message text.

local M = {}

M.WAIT = "wait_refused"
M.BUSY = "busy"

--- The refusal code of a caught error, or nil when it is not a refusal.
function M.code(err)
    if type(err) ~= "string" then return nil end
    return string.match(err, "^%[hull:([%w_]+)%] ")
end

--- True when err is the runtime refusing to wait here.
function M.is_wait(err) return M.code(err) == M.WAIT end

--- True when err is an admission cap of this VM refusing the op.
function M.is_busy(err) return M.code(err) == M.BUSY end

return M
