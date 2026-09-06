# mk/vendor/stb.mk - stb_image vendored config (extracted verbatim; DEPFLAGS applied by
# the shared block in the root Makefile after all vendor includes).

# ── stb_image (image decode/encode) ──────────────────────────────────

STB_DIR     := $(VENDDIR)/stb
STB_CFLAGS  := -std=c11 $(HL_OPT) -w

ifeq ($(HL_ENABLE_IMAGE),1)
STB_OBJ     := $(BUILDDIR)/stb_impl.o

$(STB_OBJ): $(STB_DIR)/stb_impl.c | $(BUILDDIR)
	$(CC) $(STB_CFLAGS) -I$(STB_DIR) -c -o $@ $<
else
# Image codecs disabled: no stb object linked (image's sole consumer).
STB_OBJ     :=
endif
