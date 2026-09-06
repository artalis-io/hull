# mk/vendor/lua.mk - Lua 5.4 vendored runtime config.
# Extracted verbatim (build modularization), included at the original position.

# ── Lua 5.4 ──────────────────────────────────────────────────────────

LUA_DIR  := $(VENDDIR)/lua
LUA_SRCS := $(filter-out $(LUA_DIR)/lua.c $(LUA_DIR)/luac.c, \
             $(wildcard $(LUA_DIR)/*.c))
LUA_OBJS := $(patsubst $(LUA_DIR)/%.c,$(BUILDDIR)/lua_%.o,$(LUA_SRCS))

# Lua compiled with relaxed warnings (vendored code)
LUA_CFLAGS := -std=c11 $(HL_OPT) -w -DLUA_USE_POSIX
