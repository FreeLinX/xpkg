# Nux9/xpkg - the Nux9 package manager.
#
# xpkg is first-party Nux9 software (not a ported upstream project),
# so it builds directly here rather than through the Nux9/ports
# framework -- there is no upstream to fetch/patch, only this repo's own
# source. It is statically linked against the Nux9 toolchain, the same
# as everything else in the OS.
#
# Dependencies (all must already be built by Nux9/ports into the
# build/deps prefix before xpkg can link -- `make build PORT=base/<dep>`):
#   libsqlite3.a  - installed-package database   (base/sqlite)
#   libz.a        - gzip decompression (tar.c reads .xpkg archives via
#                   zlib's gzFile directly, no external gzip/tar process)
#                                                                 (base/zlib)
#   libcrypto.a   - SHA256 file hashing (from the already-ported OpenSSL)
#                                                                (base/openssl)
#
# Configure these the same way Nux9/ports does, via NUX9_* env
# vars (see Nux9/ports/config/default.conf for the same convention):
#
#   NUX9_CC           default: ../toolchain/bin/clang
#   NUX9_SYSROOT      default: ../toolchain/x86_64-linux-musl
#   NUX9_TRIPLE       default: x86_64-linux-musl
#   NUX9_PORTS_DEPS   default: ../ports-actual/build/deps  (root of the
#                          build-time dependency prefixes zlib/openssl/sqlite)

NUX9_ROOT           ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
NUX9_TOOLCHAIN_DIR  ?= $(NUX9_ROOT)/../toolchain
NUX9_TRIPLE         ?= x86_64-linux-musl
NUX9_CC             ?= $(NUX9_TOOLCHAIN_DIR)/bin/clang
NUX9_SYSROOT        ?= $(NUX9_TOOLCHAIN_DIR)/x86_64-linux-musl
NUX9_PORTS_DEPS     ?= $(shell [ -d "$(NUX9_ROOT)/../ports/build/deps" ] && echo "$(NUX9_ROOT)/../ports/build/deps" || echo "$(NUX9_ROOT)/../ports-actual/build/deps")

CC := $(NUX9_CC)

# Per-dependency include/lib prefixes (all under build/deps).
ZLIB_PREFIX    := $(NUX9_PORTS_DEPS)/zlib
OPENSSL_PREFIX := $(NUX9_PORTS_DEPS)/openssl
SQLITE_PREFIX  := $(NUX9_PORTS_DEPS)/sqlite

CFLAGS  = --target=$(NUX9_TRIPLE) --sysroot=$(NUX9_SYSROOT) \
          -I$(ZLIB_PREFIX)/include \
          -I$(OPENSSL_PREFIX)/include \
          -I$(SQLITE_PREFIX)/include \
          -Iinclude -O2 -Wall -Wextra -Wno-unused-parameter
LDFLAGS = --target=$(NUX9_TRIPLE) --sysroot=$(NUX9_SYSROOT) \
          -fuse-ld=lld -rtlib=compiler-rt -unwindlib=none -static \
          -L$(ZLIB_PREFIX)/lib \
          -L$(OPENSSL_PREFIX)/lib \
          -L$(OPENSSL_PREFIX)/lib64 \
          -L$(SQLITE_PREFIX)/lib
LDLIBS  = -lsqlite3 -lz -lssl -lcrypto -lpthread -ldl

SRCS = src/main.c src/util.c src/paths.c src/lock.c src/json.c src/sign.c \
       src/pkginfo.c src/db.c src/tar.c src/hash.c src/version.c \
       src/install.c src/cmd_install.c src/cmd_other.c \
       src/net.c src/repo.c
OBJS = $(SRCS:.c=.o)
BIN  = xpkg

.PHONY: all clean check-toolchain

all: check-toolchain $(BIN)

check-toolchain:
	@if [ ! -x "$(NUX9_CC)" ]; then \
		echo "[xpkg] error: toolchain not found at $(NUX9_CC)"; \
		echo "[xpkg] set NUX9_CC/NUX9_SYSROOT, or build Nux9/toolchain first"; \
		exit 1; \
	fi
	@for d in $(ZLIB_PREFIX) $(OPENSSL_PREFIX) $(SQLITE_PREFIX); do \
		if [ ! -d "$$d" ]; then \
			echo "[xpkg] error: dependency prefix not found: $$d"; \
			echo "[xpkg] build+stage zlib/openssl/sqlite in Nux9/ports first"; \
			exit 1; \
		fi; \
	done

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(BIN)

# --- repository index integrity -------------------------------------------
# `xpkg-create index` appends, so a rebuilt package keeps the previous build's
# size/sha256 in the index and `xpkg install` then refuses it (it verifies the
# sha256 after download).  Regenerate the index from the archives instead; see
# the "Hosting the public repo" section of README.md.
REPO_INDEX_PY   := tools/xpkg-repo-index.py
REPO_DIR        ?= ../ports/packages
PUBLISHED_BASE  ?= https://huggingface.co/datasets/Nux9/packages/resolve/main

.PHONY: repo-index repo-check repo-check-repo

repo-index:
	@python3 $(REPO_INDEX_PY) --dir "$(REPO_DIR)" --regen

repo-check:
	@python3 $(REPO_INDEX_PY) --dir "$(REPO_DIR)" --check

# Audits the live published repo: downloads every archive and re-hashes it.
repo-check-repo:
	@python3 $(REPO_INDEX_PY) --base "$(PUBLISHED_BASE)" --check -v
