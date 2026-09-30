-- hull._async - joinable tasks, and gather / map over them.
--
-- Reached as hull.async, hull.gather and hull.map (the hull global loads this
-- module the first time one of them is touched). Built on three private
-- primitives from runtime/lua/async.c:
--
--   hull._spawn(fn)   run fn on its own coroutine on the event loop
--   hull._token()     a wake token
--   hull._park(tok)   suspend until tok is woken; hull._wake(tok) wakes it
--
-- Concurrency, not parallelism: everything runs on the event-loop thread, and
-- tasks overlap while they WAIT (network, the worker pool, a sleep). Design:
-- docs/task_join_design.md.

local M = {}

local H = hull

-- How many tasks are running: read by the runtime when app.main returns, to
-- say how many it is abandoning.
H._running = H._running or 0

local Task = {}
Task.__index = Task

--- Run fn(...) concurrently. Returns a task; task:wait() returns what fn
--- returned, or raises what it raised.
function M.async(fn, ...)
    if type(fn) ~= "function" then
        error("hull.async: expected a function, got " .. type(fn), 2)
    end
    local task = setmetatable({ _done = false, _waiters = {} }, Task)
    local args = table.pack(...)
    -- Counted inside the body, which _spawn runs at once: a spawn that fails
    -- before running it leaves the count alone.
    H._spawn(function()
        H._running = H._running + 1
        local r = table.pack(pcall(fn, table.unpack(args, 1, args.n)))
        if r[1] then
            task._results = table.pack(table.unpack(r, 2, r.n))
        else
            task._failed, task._error = true, r[2]
        end
        task._done = true
        H._running = H._running - 1
        local waiters = task._waiters
        task._waiters = nil
        -- One wake that cannot be scheduled must not keep the rest asleep.
        for _, tok in ipairs(waiters) do pcall(H._wake, tok) end
    end)
    return task
end

--- Whether the task has finished (returned or raised).
function Task:done()
    return self._done
end

--- Wait for the task; its return values, or its error raised again.
function Task:wait()
    if not self._done then
        local tok = H._token()
        self._waiters[#self._waiters + 1] = tok
        H._park(tok)
    end
    if self._failed then error(self._error, 0) end
    return table.unpack(self._results, 1, self._results.n)
end

-- Every failure, raised after the rest have finished. Its message is the
-- first failure's (by position), so an uncaught one reads like a plain error.
local Failure = {
    __tostring = function(e) return e.message end,
}

local function message_of(v)
    if type(v) == "string" then return v end
    local ok, s = pcall(tostring, v)
    return ok and s or "(an error)"
end

-- What errors[i] holds for a function that raised nil (error() with no
-- value): a nil entry would read as "did not fail".
local NIL_ERROR = "(error with no value)"

local function failed_with(v)
    if v == nil then return NIL_ERROR end
    return v
end

local function raise(errors)
    local first
    for i in pairs(errors) do
        if not first or i < first then first = i end
    end
    error(setmetatable({ message = message_of(errors[first]), errors = errors },
                       Failure), 0)
end

--- Run each function concurrently; return their first results in argument
--- order. If any raised, every one still finishes first, and then the first
--- failure is raised with `errors` holding all of them by position.
function M.gather(...)
    local fns = table.pack(...)
    for i = 1, fns.n do
        if type(fns[i]) ~= "function" then
            error("hull.gather: argument " .. i .. " is not a function", 2)
        end
    end
    local tasks = {}
    for i = 1, fns.n do tasks[i] = M.async(fns[i]) end
    local results, errors = {}, nil
    for i = 1, fns.n do
        local ok, v = pcall(tasks[i].wait, tasks[i])
        if ok then results[i] = v
        else errors = errors or {}; errors[i] = failed_with(v) end
    end
    if errors then raise(errors) end
    return table.unpack(results, 1, fns.n)
end

M.DEFAULT_LIMIT = 16

--- fn(item, i) for every item, at most opts.limit (default 16) at once;
--- results[i] is fn's first result for items[i], and results.n is the item
--- count (results may hold nils). The items are items[1..items.n] when the
--- list carries an n (table.pack), else items[1..#items]. Failures as for
--- gather.
function M.map(items, fn, opts)
    if type(items) ~= "table" then
        error("hull.map: expected a list, got " .. type(items), 2)
    end
    if type(fn) ~= "function" then
        error("hull.map: expected a function, got " .. type(fn), 2)
    end
    if opts ~= nil and type(opts) ~= "table" then
        error("hull.map: opts must be a table, got " .. type(opts), 2)
    end
    local limit = opts and opts.limit
    if limit == nil then limit = M.DEFAULT_LIMIT end
    if limit ~= math.huge then
        limit = math.tointeger(limit)   -- 4.0 is 4; 4.5 and "4" are not
        if not limit or limit < 1 then
            error("hull.map: limit must be a positive integer (or math.huge)", 2)
        end
    end

    local n = items.n
    if n == nil then
        n = #items
    elseif math.type(n) ~= "integer" or n < 0 then
        error("hull.map: items.n must be a non-negative integer", 2)
    end
    local results, errors = { n = n }, nil
    local next_i = 1

    -- A fixed number of workers each take the next index until none are
    -- left: at most `limit` items are ever in flight, and a slow item holds
    -- up only its own worker.
    local function worker()
        while next_i <= n do
            local i = next_i
            next_i = i + 1
            local ok, v = pcall(fn, items[i], i)
            if ok then results[i] = v
            else errors = errors or {}; errors[i] = failed_with(v) end
        end
    end

    local workers = {}
    for w = 1, math.min(limit, n) do workers[w] = M.async(worker) end
    for w = 1, #workers do workers[w]:wait() end

    if errors then raise(errors) end
    return results
end

return M
