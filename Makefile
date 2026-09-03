# FreeLinX/xpkg - the FreeLinX package manager.
#
# xpkg is first-party FreeLinX software (not a ported upstream project),
# so it builds directly here rather than through the FreeLinX/ports
# framework -- there is no upstream to fetch/patch, only this repo's own
# source. It is statically linked against the FreeLinX toolchain, the same
# as everything else in the OS.
#
# Dependencies (all must already be built by FreeLinX/ports into the
# build/deps prefix before xpkg can link -- `make build PORT=base/<dep>`):
#   libsqlite3.a  - installed-package database   (base/sqlite)
#   libz.a        - gzip decompression (tar.c reads .xpkg archives via
#                   zlib's gzFile directly, no external gzip/tar process)
#                                                                 (base/zlib)
#   libcrypto.a   - SHA256 file hashing (from the already-ported OpenSSL)
#                                                                (base/openssl)
#
# Configure these the same way FreeLinX/ports does, via FREELINX_* env
# vars (see FreeLinX/ports/config/default.conf for the same convention):
#
#   FREELINX_CC           default: ../toolchain/bin/clang
#   FREELINX_SYSROOT      default: ../toolchain/x86_64-linux-musl
#   FREELINX_TRIPLE       default: x86_64-linux-musl
#   FREELINX_PORTS_DEPS   default: ../ports-actual/build/deps  (root of the
#                          build-time dependency prefixes zlib/openssl/sqlite)

FREELINX_ROOT           ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
FREELINX_TOOLCHAIN_DIR  ?= $(FREELINX_ROOT)/../toolchain
FREELINX_TRIPLE         ?= x86_64-linux-musl
FREELINX_CC             ?= $(FREELINX_TOOLCHAIN_DIR)/bin/clang
FREELINX_SYSROOT        ?= $(FREELINX_TOOLCHAIN_DIR)/x86_64-linux-musl
FREELINX_PORTS_DEPS     ?= $(FREELINX_ROOT)/../ports-actual/build/deps

CC := $(FREELINX_CC)

# Per-dependency include/lib prefixes (all under build/deps).
ZLIB_PREFIX    := $(FREELINX_PORTS_DEPS)/zlib
OPENSSL_PREFIX := $(FREELINX_PORTS_DEPS)/openssl
SQLITE_PREFIX  := $(FREELINX_PORTS_DEPS)/sqlite

CFLAGS  = --target=$(FREELINX_TRIPLE) --sysroot=$(FREELINX_SYSROOT) \
          -I$(ZLIB_PREFIX)/include \
          -I$(OPENSSL_PREFIX)/include \
          -I$(SQLITE_PREFIX)/include \
          -Iinclude -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS = --target=$(FREELINX_TRIPLE) --sysroot=$(FREELINX_SYSROOT) \
          -fuse-ld=lld -rtlib=compiler-rt -unwindlib=none -static \
          -L$(ZLIB_PREFIX)/lib \
          -L$(OPENSSL_PREFIX)/lib \
          -L$(OPENSSL_PREFIX)/lib64 \
          -L$(SQLITE_PREFIX)/lib
LDLIBS  = -lsqlite3 -lz -lssl -lcrypto -lpthread -ldl

SRCS = src/main.c src/pkginfo.c src/db.c src/tar.c src/hash.c \
       src/cmd_install.c src/cmd_other.c \
       src/net.c src/repo.c
OBJS = $(SRCS:.c=.o)
BIN  = xpkg

.PHONY: all clean check-toolchain

all: check-toolchain $(BIN)

check-toolchain:
	@if [ ! -x "$(FREELINX_CC)" ]; then \
		echo "[xpkg] error: toolchain not found at $(FREELINX_CC)"; \
		echo "[xpkg] set FREELINX_CC/FREELINX_SYSROOT, or build FreeLinX/toolchain first"; \
		exit 1; \
	fi
	@for d in $(ZLIB_PREFIX) $(OPENSSL_PREFIX) $(SQLITE_PREFIX); do \
		if [ ! -d "$$d" ]; then \
			echo "[xpkg] error: dependency prefix not found: $$d"; \
			echo "[xpkg] build+stage zlib/openssl/sqlite in FreeLinX/ports first"; \
			exit 1; \
		fi; \
	done

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(BIN)
