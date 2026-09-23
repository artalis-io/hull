# mk/vendor/bcrypt.mk - OpenBSD bcrypt_pbkdf + Blowfish, for OpenSSH private
# keys that carry a passphrase.
#
# Vendored rather than reimplemented: bcrypt_pbkdf is a KDF whose whole value
# is agreeing byte-for-byte with what ssh-keygen wrote, and Blowfish is 4 KiB
# of constant tables that cannot be derived from the algorithm. Getting either
# subtly wrong yields a key that decrypts to garbage, or worse, a KDF that is
# cheaper than it looks.
#
# Compiled -w like every other vendored tree. The two .c files are byte-for-
# byte upstream; everything Hull needs to supply them lives in the two shim
# headers beside them (see vendor/bcrypt/includes.h).

BCRYPT_DIR    := $(VENDDIR)/bcrypt
BCRYPT_OBJS   := $(BUILDDIR)/bcrypt_pbkdf.o $(BUILDDIR)/blowfish.o
BCRYPT_CFLAGS := -std=c11 $(HL_OPT) -w -I$(BCRYPT_DIR)
