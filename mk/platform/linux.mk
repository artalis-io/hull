# mk/platform/linux.mk - native Linux platform policy.
#
# The jart/pledge polyfill: real pledge()/unveil() via seccomp + landlock.
# (macOS uses seatbelt, applied in sandbox.c with no compiled polyfill; cosmo
# has pledge/unveil built in.) Included only when PLATFORM=linux, so the
# original ifeq(Linux)/ifndef(COSMO) guard is no longer needed here.

PLEDGE_DIR := $(VENDDIR)/pledge
PLEDGE_CFLAGS := -std=c11 -O2 -w -D_GNU_SOURCE -I$(PLEDGE_DIR) $(DEPFLAGS)

# musl (Alpine) omits the glibc-only __O_TMPFILE macro that pledge-linux.c
# references - purely as the numeric seccomp-mask constant 020000000 (the
# vendored polyfill's own comments spell out that value; it is the Linux ABI
# flag, identical on x86_64/aarch64). glibc defines it via <bits/fcntl-linux.h>,
# so scope the define to a musl target (a glibc -D would just warn on redefine)
# and to PLEDGE_CFLAGS (pledge-linux.c is its sole user tree-wide). Detected from
# the compiler's target triple. This is the one shim a musl build needs; the
# rest of the vendored stack is musl-clean. See docs/musl_build.md.
ifneq ($(findstring musl,$(shell $(CC) -dumpmachine 2>/dev/null)),)
PLEDGE_CFLAGS += -D__O_TMPFILE=020000000
endif

PLEDGE_SRCS := \
	$(PLEDGE_DIR)/libc/calls/pledge.c \
	$(PLEDGE_DIR)/libc/calls/pledge-linux.c \
	$(PLEDGE_DIR)/libc/calls/unveil.c \
	$(PLEDGE_DIR)/libc/calls/parsepromises.c \
	$(PLEDGE_DIR)/libc/calls/landlock_add_rule.c \
	$(PLEDGE_DIR)/libc/calls/landlock_create_ruleset.c \
	$(PLEDGE_DIR)/libc/calls/landlock_restrict_self.c \
	$(PLEDGE_DIR)/libc/calls/commandv.c \
	$(PLEDGE_DIR)/libc/calls/getcpucount.c \
	$(PLEDGE_DIR)/libc/calls/islinux.c \
	$(PLEDGE_DIR)/libc/intrin/promises.c \
	$(PLEDGE_DIR)/libc/intrin/pthread_setcancelstate.c \
	$(PLEDGE_DIR)/libc/elf/checkelfaddress.c \
	$(PLEDGE_DIR)/libc/elf/getelfsegmentheaderaddress.c \
	$(PLEDGE_DIR)/libc/str/classifypath.c \
	$(PLEDGE_DIR)/libc/str/endswith.c \
	$(PLEDGE_DIR)/libc/str/isabspath.c \
	$(PLEDGE_DIR)/libc/fmt/joinpaths.c \
	$(PLEDGE_DIR)/libc/fmt/sizetol.c \
	$(PLEDGE_DIR)/libc/runtime/isdynamicexecutable.c \
	$(PLEDGE_DIR)/libc/sysv/calls/ioprio_set.c \
	$(PLEDGE_DIR)/libc/x/xdie.c \
	$(PLEDGE_DIR)/libc/x/xjoinpaths.c \
	$(PLEDGE_DIR)/libc/x/xmalloc.c \
	$(PLEDGE_DIR)/libc/x/xrealloc.c \
	$(PLEDGE_DIR)/libc/x/xstrcat.c \
	$(PLEDGE_DIR)/libc/x/xstrdup.c
PLEDGE_OBJS := $(patsubst $(PLEDGE_DIR)/%.c,$(BUILDDIR)/pledge_%.o,$(PLEDGE_SRCS))

# Flatten libc/calls/pledge.c -> build/pledge_libc_calls_pledge.o
$(BUILDDIR)/pledge_%.o: $(PLEDGE_DIR)/%.c | $(BUILDDIR)
	@mkdir -p $(dir $@)
	$(CC) $(PLEDGE_CFLAGS) -c -o $@ $<
