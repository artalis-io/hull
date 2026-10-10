# Lua patches

Hull vendors Lua 5.4 directly in `vendor/lua/` (edited in tree, like QuickJS;
see [quickjs_patches.md](quickjs_patches.md)). Every local change is marked in
the source with a `HULL PATCH` comment and listed here.

**On a Lua upgrade:** grep the new tree for `HULL PATCH`, confirm each entry
below is still needed against upstream, and re-apply the ones that are.

## Patch 0001 - keep the count hook on inside finalizers

**File:** `vendor/lua/lgc.c`, `GCTM`
**Found by:** round-5 C audit (H4)
**Upstream:** Hull-specific; not for upstream (the stock behaviour is deliberate).

`GCTM` runs each `__gc` finalizer with `L->allowhook = 0`, so no debug hook
fires inside it. Hull's instruction budget (`src/hull/runtime/lua/budget.c`) is
a COUNT hook, so an app finalizer was unmetered:

```lua
setmetatable({}, { __gc = function() while true do end end })
collectgarbage()   -- pins the event loop for good
```

In a `worker.dispatch` VM the same body ran at `lua_close` and pinned a pool
thread, and a finalizer could do anything a handler can outside any budget.

The patch turns hooks ON for the finalizer when the thread has a count hook
installed (`L->hookmask & LUA_MASKCOUNT`), and off as before otherwise. It
used to leave `allowhook` as it was, which missed one case (round-6 audit
H1): a GC step taken while a hook runs - the budget's own raise allocated,
or a C message handler it reached did - has `allowhook == 0`, and the
finalizer then ran with no metering at all. (budget.c also raises the trip
without allocating now, and skips xpcall's message handler on a trip, since
Lua calls the handler before the error leaves the hook.) What makes that
safe:

- The finalizer already runs under `luaD_pcall`, with GC steps stopped
  (`GCSTPGC`). A budget trip raised from the hook is caught there like any
  other `__gc` error (`luaE_warnerror`), so the finalizer ends and the GC
  continues normally.
- The budget's trip is sticky: once it fires, the next instruction of the code
  that triggered the collection raises too, so the request stops instead of
  carrying on after its finalizer was cut short.
- Line, call and return hooks are unaffected: they stay off inside finalizers,
  as upstream intends.

## Patch 0002 - charge pattern matching to the instruction budget

**File:** `vendor/lua/lstrlib.c`, `MatchState` / `prepstate` / `match`
**Found by:** round-5 C audit (M2)
**Upstream:** Hull-specific.

`string.find` / `match` / `gmatch` / `gsub` match in C (`match`,
`max_expand`, `min_expand`), so the count hook never fires inside them, and a
pattern that backtracks - `string.find(string.rep("a", 1e5),
string.rep("a-", 30) .. "b")` is polynomial of degree ~30 - ran for as long as
it liked on the event loop, untouched by the limit (or `pcall`).

The patch counts work in the `MatchState`: one unit per `match()` call and
per subject byte a single step scans - a single-char or class test (a
`[set]` costs its length), a `%b` balance scan, a `%1` back-reference
compare, and, for a plain `string.find` (no specials, or `plain = true`),
the bytes each `lmemfind` candidate costs, and the bytes of the final scan
that finds nothing (round-7 audit M1: a miss on a 32 MB subject cost ~6
instructions). What is left below one hook period when the call returns is
charged to the thread through Patch 0004's `lua_hlcharge` (dropped, it made
every call free up to 10000 units). Counting `match()` calls alone
(round-5) charged `string.find(s, "%b()")` n units for O(n^2) bytes
scanned, and a plain find nothing (round-6 audit M1). Every hook period of
it (the count hook's own `lua_gethookcount`, captured when the state is
prepared; nothing is counted on a thread with no count hook), it calls the
thread's count hook as if that many instructions had run. Hull's budget hook
charges the stride and, once over the limit, raises - out of the matcher, the
same way the matcher's own "pattern too complex" error leaves it. Nothing the
matcher holds needs cleanup on that path (its state is on the C stack; `gsub`'s
buffer is on the Lua stack).

The hook is reached through `lua_gethook`, so the vendored file depends on no
Hull symbol, and a VM without a count hook (the tool VM, tests) behaves exactly
as upstream.

## Patch 0003 - hooks allowed again when a thread is reset

**File:** `vendor/lua/lstate.c`, `luaE_resetthread`
**Found by:** round-6 C audit (H1, same family)
**Upstream:** Hull-specific (arguably an upstream gap, but harmless there).

A coroutine that dies by an error raised from inside a hook - Hull's budget
trip - keeps `L->allowhook == 0`: `luaD_hook` turned it off, and nothing on
`lua_resume`'s error path turns it back on. Its pending `__close` handlers run
later, from `coroutine.close` (or `lua_closethread`), through
`luaE_resetthread` - with every hook off, so unmetered:

```lua
CO = coroutine.create(function()
  local x <close> = setmetatable({}, { __close = function() while true do end end })
  while true do end           -- trips the budget; CO is dead
end)
coroutine.resume(CO)
-- next request:
coroutine.close(CO)           -- ran the loop with no limit
```

The patch sets `L->allowhook = 1` before the `__close` handlers run, the state
a new thread starts in. The handlers already run under `luaD_closeprotected`,
so a trip there is an ordinary error, and `coroutine.close`'s guard
(runtime/lua/async.c) re-raises it.

## Patch 0004 - charge work done inside one instruction

**Files:** `vendor/lua/lstate.h` / `lstate.c` (`luaE_hlcharge`, `luaE_hlbytes`,
`luaE_hltransfer`, the `hlbytes` / `hlowed` thread fields, the `hlgcwork`
global field, `lua_closethread`), `lapi.c` + `lua.h` (`lua_hlcharge`,
`lua_hlwork`, `lua_hltakeowed`, `lua_rawequal`, `lua_gettable` /
`lua_settable` / `lua_rawget`, `lua_stringtonumber`), `ldebug.c`
(`lua_sethook`), `ldo.c` (`moveresults`, `lua_resume`), `lgc.c`, `lmem.c`,
`ltable.h` / `ltable.c` (`luaH_getL`, `luaH_getintL`, `luaH_next`, `luaH_getn`), `ltm.c`
(`luaT_getvarargs`), `lvm.h` / `lvm.c`, `lauxlib.c` (`resizebox`),
`lstrlib.c`, `ltablib.c`, `lutf8lib.c`, `lbaselib.c`
**Found by:** round-7 C audit (M1, M2); extended by round-8 (M1-M3, L1) and
round-9 (H2)
**Upstream:** Hull-specific.

The count hook counts VM instructions, and some instructions do work
proportional to their operands' size: a long-string `==` is a `memcmp`, a
`..` / `string.rep` / `string.format` copies, `table.insert(t, 1, v)` shifts
the whole array, `collectgarbage()` traverses the heap. Each cost one
instruction, so `max_instructions` did not bound a run's wall time:

```lua
local a = string.rep("x", 3e7); local b = a:sub(1, -2) .. "x"
while a == b do end               -- 2 instructions per 30 MB memcmp: ~a day
table.move({}, 1, 1e15, 2)        -- 1e15 iterations in one instruction
```

The patch charges such work to the thread's count hook, in instruction
equivalents:

- **Deferred** (`luaE_hlcharge` / `luaE_hlbytes`, public `lua_hlcharge`):
  the units come off `L->hookcount`, so the hook runs that much sooner - at
  the very next instruction once they reach it - and what goes past it is
  kept in `L->hlowed`, which the hook collects with `lua_hltakeowed` (Hull's
  budget hook adds it to the run's count). No allocation, no error, so it is
  safe anywhere, including inside the allocator and the VM. Bulk bytes
  (`luaE_hlbytes`) cost one unit per 64 bytes. Charged:
  - every block Lua allocates or grows (`luaM_malloc_`, a growing
    `luaM_realloc_` - strings, tables, buffers' results), and the box a
    `luaL_Buffer` grows (`resizebox`, which allocates outside the core: a
    buffer filled and then dropped by an error, `pcall(table.concat, {big,
    true})`, was free);
  - the bytes a string compare reads: a long-string equality
    (`luaV_equalobj`, `OP_EQK`, `lua_rawequal`); a string `<` / `<=`, for
    the prefix it compared (`l_strcmp`: each equal segment, then the
    deciding one up to its first differing 64-byte block - charging the
    left operand's length made `big < "a"`, one byte compared, cost
    len/64, and a `table.sort` of large distinct strings trip the limit);
    and a long-string table key that equals a key in the table without
    being the same object (`luaH_getL` / `equalkey`, used for every key a
    script chooses: `t[k]`, `t[k] = v`, `t:m()`, `rawget`, `next(t, k)`,
    the `__index` / `__newindex` chain, `luaH_set`);
  - the empty slots a `next` steps over (`luaH_next`; a table whose
    entries were removed keeps its size, and each `next` scanned all of it);
  - the hash chain a lookup or insert walks, a unit per node past the
    first 8 (`HL_FREE_CHAIN`; `getgeneric`, `luaH_getint` through
    `luaH_getintL` / `luaH_getL`, the previous-node search in
    `luaH_newkey`, `luaH_getn`'s `hash_search`, which now take `L`). An
    integer key hashes to `k % ((sizenode - 1) | 1)` and a float key by
    `frexp`, with no seed, so a script can put every key on one chain:
    `local m = (1 << 17) - 1; for k = 1, 1e5 do t[k * m] = true end`, and
    each `t[k * m]` (and each insert, and the rehash that reinserts them
    all) then walked 1e5 nodes inside one instruction (round-9 H2). Used
    by `luaV_fastgeti` (`t[i]`, `OP_GETI` / `OP_SETI`), `lua_geti` /
    `lua_seti` / `lua_rawgeti` / `lua_rawseti` / `lua_rawgetp`,
    `luaH_setint`, `luaH_set` (and so `reinsert`), `next`, and `#t` /
    `lua_rawlen`. Short strings keep upstream's per-state seed and are not
    charged; internal callers with no thread (the parser's constant table)
    charge nothing. A per-state seed for integer / float keys was the
    alternative; charging keeps iteration order deterministic and bounds
    the run whatever the keys;
  - values copied in bulk: results moved by a return (`moveresults`) and
    varargs fetched by `...` (`luaT_getvarargs`), a unit each past the first
    16 (`HL_FREE_COPIES`);
  - a string-to-number coercion, a unit per byte (`l_strton` - so
    `luaV_tonumber_` / `luaV_tointeger` take `L` - and
    `lua_stringtonumber`, `tonumber(s, base)`): spaces and digits are
    scanned, and `strtod` reads the whole string;
  - the collector's work (`lgc.c`, `g->hlgcwork`): every unit of work a
    step reports, what `atomic` marks, each object `sweepgen` / `sweep2old`
    / `whitelist` / `markold` visits, each table `convergeephemerons`
    revisits, each slot `clearbykeys` / `clearbyvalues` scans - charged to
    the running thread when `luaC_step`, `luaC_fullgc` or
    `luaC_changemode` returns. A collection runs inside one instruction,
    and app code chose when: a failing allocation runs an emergency full
    collection before its (catchable) memory error, so `pcall(function()
    return a .. a end)` bought a full traversal of a large heap for ~6
    instructions; `collectgarbage("generational")` / `("incremental")` and
    the tuning forms (`setpause`, `setstepmul`, mode arguments) did the
    same, or raised the work done per byte allocated. (This replaces the
    heap-size charge `collectgarbage("collect"/"step")` had: the work is
    now charged where it is done, whoever triggers it. Making a memory
    error sticky was the alternative; charging keeps a caught memory error
    an ordinary error, and covers the implicit collections too.)
  - `utf8.len` (one unit per byte decoded, as a match step), and the bytes
    `utf8.offset` and a `utf8.codes` step skip.
- **Checked** (`lua_hlwork`, from a C function only): the same charge, then
  the count hook runs at once if it came due - for a library loop whose
  length an argument or `__len` decides: `table.insert` / `table.remove`
  shifts, `table.move` and `table.concat`, charged per 1024 elements;
  `table.sort`, per partition; `table.unpack`, `string.byte` and
  `utf8.codepoint`, a unit per value pushed, charged before the loop;
  `string.pack` / `unpack` / `packsize`, a unit per format byte (an option
  such as `' '` pushes nothing, so a long format looped for free); and
  `string.rep`, a unit per copy. `string.rep` of an empty result (`s` and
  `sep` both empty) now returns at once: `string.rep("", 1e18)` ran its copy
  loop 1e18 times inside one instruction.

**Coroutines** (`luaE_hltransfer`, from `lua_resume` and `lua_closethread`):
the count hook reports a whole period only (Hull's stride, 10000), and every
new coroutine starts on a full one. A coroutine that ran less than a period
and returned was never charged, so `for j = 1, n do coroutine.wrap(f)() end`
with a 9900-instruction `f` ran ~400x past the limit (round-4 H1, splitting
work across coroutines, was closed only per period). When a resume returns
(or yields), the part of the period the coroutine used, plus its owed units
and sub-unit bytes, is charged to the thread that resumed it, and the
coroutine starts its next run on a full period. `coroutine.close` does the
same for its `__close` handlers.

`lua_sethook` clears both fields, so a re-armed hook starts owing nothing.
A VM with no count hook (the tool VM, tests) behaves exactly as upstream.

**Not covered:** a `lua_getfield` / `lua_setfield` from C with a long C-string
key (Hull's bindings use short literal names); and Hull's own C bindings
charge what they allocate, plus, in `hull.crypto`, the bytes a digest / MAC /
signature / cipher / `constant_time_eq` reads (`crypto_charge`, one unit per 8
bytes) and a key derivation's rounds before it runs (`crypto_charge_kdf`:
PBKDF2 at two SHA-256 blocks per iteration, so `verify_password` with the 10M
iterations a stored string may name trips the limit at once; `bcrypt_pbkdf` at
2^17 units per bcrypt hash - round-9 H3), `res:json` / `html` / `text` the
body they copy or gzip, `res:header` the bytes it adds (round-9 M1 / M2), and
`part:read()` the bytes it accumulates - a binding that reads a large input and
allocates little is otherwise charged one instruction.

## Patch 0005 - an embedder stop flag polled at every hooked instruction

**Files:** `vendor/lua/lstate.h` / `lstate.c` (the `hlstop` global field),
`lapi.c` + `lua.h` (`lua_hlsetstop`), `ldebug.c` (`luaG_traceexec`)
**Found by:** round-12 audit (G2: the run watchdog)
**Upstream:** Hull-specific.

Audits 9-12 kept finding work one instruction does that patch 0004 does not
charge (a rehash, `luaC_checkfinalizer` walking the finalizer list on every
`setmetatable` with `__gc`, `luaH_next` over live entries, ...). Charging each
one never converges, so Hull bounds every run by wall-clock time as well
(`src/hull/cap/run_watchdog.c`): a watchdog thread raises a per-VM stop flag at
the run's deadline. The hook can only see that flag when it runs, and between
two count-hook calls a thread runs a whole hook period (10000 instructions) -
each of which may be one of those uncharged operations.

The patch gives the global state a pointer to an `int` the embedder owns
(`lua_hlsetstop(L, &flag)`, NULL = none). `luaG_traceexec` - which a hooked
thread already enters at every instruction to decrement `hookcount` - reads the
flag (an atomic relaxed load: another thread writes it) and, when it is set,
sets `hookcount` to 1 so the count hook runs at this instruction. Hull's hook
(`runtime/lua/budget.c`) then trips the run's budget with its own message
(`run exceeded its time limit`); the trip is the same sticky, uncatchable one
the instruction limit raises.

Cost: one pointer load and one compare per hooked instruction, in a function
the hooked VM already calls per instruction. A VM with no count hook never
enters `luaG_traceexec`, so the flag is not polled there - Hull installs the
hook whenever a deadline is armed, even with no instruction limit. A VM with
no flag set (the tool VM) behaves exactly as upstream.

**Limit:** the flag is seen between instructions. One instruction that runs
long (one huge rehash, a C function such as `table.sort` with a C
comparator) runs to completion; the watchdog bounds loops of them.

**Guard:** `tests/hull/runtime/lua/test_lua.c`, `lua_run_watchdog.*` (loops of
round-12 triggers under a 200 ms deadline and no instruction limit are stopped;
the trip survives pcall / xpcall / a coroutine; a worker.dispatch job is
stopped).
