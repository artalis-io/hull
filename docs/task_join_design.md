# Joining concurrent work: `hull.async` tasks, `hull.gather`, `hull.map`

Status: **implemented** (Lua: `runtime/lua/async.c` park/wake +
`stdlib/lua/hull/_async.lua`; JS: `hull.map` in `runtime/js/async.c`).

## 1. The gap

A fleet tool is an `app.main` program. `app.main` runs as one coroutine on the
event loop, so everything it does happens in turn: 50 hosts at two seconds each
is 100 seconds, and one unreachable host holds up the rest for its whole
connect timeout. The same holds for any CLI tool fanning out `http.fetch`,
`db.async` or `compute.async` calls.

What exists today:

- **JS has concurrency already.** Every async call returns a real Promise, so
  `await Promise.all([...])` in `app.main` runs them concurrently; `hull/jobs`
  relies on it (`stdlib/js/hull/jobs.js`).
- **Lua has spawn without join.** `hull.async(fn)` (`runtime/lua/async.c`)
  starts `fn` on its own coroutine on the event loop and returns nothing. No
  result, no way to wait, and an error is only logged. `hull/jobs` works around
  it by polling a counter with `hull.sleep(5)` (`stdlib/lua/hull/jobs.lua`).
- **A documented function that does not exist.** `docs/agent_guide.md` lists
  `hull.gather(fn1, fn2, ...)`; there is no such function.

So the work is a join for Lua, bounded fan-out for both runtimes, and fixing
the doc.

## 2. API

### Lua

```lua
-- A task: hull.async now RETURNS a handle (it returned nothing before, so
-- every existing call site keeps working).
local t = hull.async(function() return probe(host) end)
...                                  -- do other work meanwhile
local status, out = t:wait()         -- the function's return values

-- A fixed set, run concurrently; returns each function's FIRST result, in
-- argument order.
local a, b = hull.gather(
    function() return fetch(x) end,
    function() return fetch(y) end)

-- A list, run with at most `limit` in flight; returns results in input
-- order (results[i] is fn(items[i], i)'s first return value).
local results = hull.map(hosts, function(host, i)
    local conn = assert(ssh.connect{ host = host, user = "deploy", key = key })
    local r = conn:exec("uptime")
    conn:close()
    return r.stdout
end, { limit = 8 })
```

- `task:wait()` returns every value the function returned (`table.pack`
  semantics, trailing nils included). Called on a finished task it returns at
  once. Several coroutines may wait on one task.
- `task:done()` is `true` once the function has returned or raised - for
  polling without waiting.
- `hull.map`'s `limit` defaults to **16**, a cap chosen so that a list of a
  thousand hosts does not open a thousand connections by accident. `limit`
  may be any positive integer; `math.huge` means no cap.

### JS

Promises already cover tasks, and `Promise.allSettled` a gather that waits
for every one. (`Promise.all` rejects at the first failure and leaves the rest
running - the orphans §3 rejects.) JS gets only the bounded fan-out, so fleet
code reads the same in both runtimes:

```js
const results = await hull.map(hosts, async (host, i) => {
    ...
}, { limit: 8 });
```

## 3. Errors: wait for all, then raise

A task that raises records the error; `task:wait()` then raises it again, in
the waiter.

`hull.gather` and `hull.map` **let every task finish** before raising, and
then raise the first failure (by position) with all of them attached:

```lua
local ok, err = pcall(hull.map, hosts, check, { limit = 8 })
if not ok then
    -- tostring(err) is the first failure's message
    for i, e in pairs(err.errors) do report(hosts[i], e) end
end
```

- The raised value is a table `{ message = <first error's text>,
  errors = { [i] = <error value>, ... } }` with a `__tostring` that returns
  `message`, so an uncaught one still prints something readable.
- JS rejects the same way: after every item settles, with an `Error` whose
  `message` is the first failure's and whose `errors` array holds them all
  (sparse, by index).

**Why not fail fast.** A failure cannot stop the others: there is no safe way
to abandon an SSH exec or an HTTP request halfway, and a task left running
after `gather` returns would be an orphan still holding a connection. Waiting
costs at most the slowest task's time, which the operations' own timeouts
already bound (`timeout_ms` on `connect` and `exec`, `http.fetch`'s timeout).

## 4. Semantics worth stating

- **Concurrency, not parallelism.** Everything runs on the one event-loop
  thread. Tasks overlap while they WAIT (network, a worker-pool job, a sleep);
  CPU-bound Lua does not get faster. `compute.async` and `db.async` already
  run on the worker pool, so fanning those out does use more cores.
- **One task per connection.** An SSH connection (and anything else that
  parks per coroutine) belongs to the task that opened it; using it from
  another task while one is waiting is refused with `busy`, as today
  (`docs/ssh.md` section 6b). `hull.map` over hosts gives each item its own
  task and so its own connection, which is the natural shape anyway.
- **Instruction limits apply per task.** A spawned coroutine inherits the
  instruction-count hook, as timer callbacks do, so each task has its own
  budget; a runaway task raises "instruction limit exceeded", which surfaces
  through `wait` / `gather` / `map` like any other error.
- **Where it works.** Anywhere a coroutine can park: `app.main`, request
  handlers, timer callbacks, and inside other tasks (a task may spawn and wait
  on its own tasks).
- **Order.** Results come back in argument / input order, not completion
  order.

## 5. When `app.main` returns with tasks still running

Unchanged from today: the process exits and unjoined tasks are abandoned. With
a join available, joining is the app's job. What changes is that it is no
longer silent: a WARN names how many were still running -
`[hull:async] app.main returned with 3 task(s) still running; they were
abandoned (join them with task:wait, hull.gather or hull.map)`.

In a server app (routes registered) `app.main` returning 0 does not end the
process, so its tasks keep running under the serve loop and nothing is logged.

## 6. Implementation sketch

The C side gains one primitive, and the rest is Lua built on it.

1. **Park and wake** (`runtime/lua/async.c`). `hull._park()` suspends the
   current coroutine the way `hull.sleep` does - attached to the request when
   there is one, detached otherwise - but with no deadline, and returns a wake
   token; `hull._wake(token)` resumes it. The wake is always DEFERRED (a
   zero-delay timer, or completing the attached op), never an inline resume,
   so a task finishing never re-enters the coroutine that is waiting on it
   from inside its own stack. Underscore-prefixed: private to the stdlib
   helpers, not documented for apps.
2. **Tasks** (`hull.async`). The spawned body is wrapped: `pcall(fn, ...)`,
   record the results or the error on the task, mark it done, wake its
   waiters. `task:wait()` parks until done, then returns or re-raises. A count
   of running tasks feeds the section 5 warning.
3. **`gather` and `map`** are Lua over tasks: `map` keeps at most `limit`
   tasks in flight, starting the next as one finishes, and collects by index.
4. **JS `hull.map`**: plain JS over Promises (a small worker pool pulling
   indices from a shared cursor).
5. **Dogfood**: `hull/jobs`'s Lua worker drops its `hull.sleep(5)` polling
   loop for a join.

## 7. Tests

- Lua and JS unit suites: results in order; `limit` respected (a counter of
  concurrently running items never exceeds it); an error in one item waits for
  the rest and reports all of them; nested tasks; `wait` on a finished task;
  several waiters on one task; nil returns preserved.
- The section 5 warning, and its absence in a server app.
- Live, in `tests/e2e_ssh_tunnel.sh`: `hull.map` over several `ssh.connect`s
  to the test sshd, asserting the elapsed time is closer to one connection's
  than to their sum.

## 8. Docs

`docs/app_api_reference.md` (the `hull` runtime helpers), `docs/ssh.md`
section 6b (replace "`app.main` connects in turn" with the `hull.map`
pattern), `examples/ssh_fleet` (use `hull.map`), `stdlib/context`, and the
`docs/agent_guide.md` row that promised `gather`.
