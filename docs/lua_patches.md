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
the `hlbytes` / `hlowed` thread fields), `lapi.c` + `lua.h` (`lua_hlcharge`,
`lua_hlwork`, `lua_hltakeowed`, `lua_rawequal`), `ldebug.c` (`lua_sethook`),
`lmem.c`, `lvm.h` / `lvm.c`, `lstrlib.c`, `ltablib.c`, `lutf8lib.c`,
`lbaselib.c`
**Found by:** round-7 C audit (M1, M2)
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
  (`luaE_hlbytes`) cost one unit per 64 bytes. Charged: every block Lua
  allocates or grows (`luaM_malloc_`, a growing `luaM_realloc_` - strings,
  tables, buffers' results), the `memcmp` of a long-string equality
  (`luaV_equalobj`, `OP_EQK`, `lua_rawequal`), a string `<` / `<=`, a
  `collectgarbage("collect")` (the heap) or `("step", n)` (n KB), and a
  `utf8.len` (one unit per byte decoded, as a match step).
- **Checked** (`lua_hlwork`, from a C function only): the same charge, then
  the count hook runs at once if it came due - for a library loop whose
  length an argument or `__len` decides (`table.insert` / `table.remove`
  shifts and `table.move`, charged per 1024 elements; `table.sort`, per
  partition), which would otherwise only be stopped after it finished.

`lua_sethook` clears both fields, so a re-armed hook starts owing nothing.
A VM with no count hook (the tool VM, tests) behaves exactly as upstream.

**Not covered:** a lookup of a long-string table key that equals the key in
the table without being the same string object `memcmp`s in `ltable.c`,
where no `lua_State` is at hand; and Hull's own C bindings charge what they
allocate, plus, in `hull.crypto`, the bytes a digest / MAC / signature reads
(`crypto_charge`, one unit per 8 bytes) - a binding that reads a large input
and allocates little is otherwise charged one instruction.
