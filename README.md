# xpkg — the FreeLinX package manager

xpkg installs, upgrades and removes FreeLinX packages from signed
repositories. It is written in C against musl, links only zlib, OpenSSL and
SQLite, and never runs an external `tar`, `gzip` or `curl`.

```
xpkg search editor          find packages by name or description
xpkg show vim               details from the repository
xpkg install vim git        install, with everything they need
xpkg remove vim             remove (refused while something needs it)
xpkg autoremove             drop dependencies nothing needs any more
xpkg upgrade                refresh the indexes and upgrade everything
xpkg outdated               what upgrade would change
xpkg list [-e]              installed packages (-e: only ones you asked for)
xpkg info | files <name>    an installed package, its files
xpkg owns /usr/bin/Xorg     which package a file belongs to
xpkg verify [name...]       check installed files against the database
xpkg clean                  delete downloaded archives
xpkg repo list|add|remove   repositories
```

Options: `-n/--dry-run`, `-f/--force`, `-q`, `-v`, `--root <dir>` (manage a
system mounted elsewhere, e.g. from the installer), `--allow-unsigned`,
`--no-scripts`. On the desktop, **Packages** (`flxpkg`) is the graphical front
end.

## Trust

- **Signed indexes.** A repository serves `index.json` and `index.json.sig`,
  an Ed25519 signature of the exact index bytes. Keys trusted by a system live
  in `/etc/xpkg/keys/*.pub`; an unsigned or wrongly signed index is refused,
  and so is every index when no key is installed. The index pins every archive's size and
  SHA-256, so a verified index vouches for every package it lists.
- **Rollback protection.** `generated` in the index may not go backwards
  between updates, so a mirror cannot replay an old signed index.
- **TLS** with certificate and host name verification (system CA bundle,
  `XPKG_CA_FILE` to override); no redirects from HTTPS to HTTP.
- **Archives are checked every time**, including ones already in the cache.
- **Safe extraction:** absolute paths, `..` and hardlinks leaving the archive
  are refused.

## Installing a package

1. The transaction is planned first: targets and missing dependencies are
   resolved from the index into dependency order, and the plan and download
   size are shown.
2. Every archive is downloaded (with retries on network hiccups) and verified.
3. Each package is extracted to a private scratch directory, checked for
   conflicts (a path owned by another package, or a directory where the
   package has a file), then written file by file to a temporary name and
   `rename()`d into place, inside one SQLite transaction. A running program,
   xpkg itself included, is never overwritten in place.
4. Files the previous version had and the new one does not are removed.
5. `post-install` runs (with `--root`, inside a chroot).

Configuration under `/etc` that you edited is never overwritten: the packaged
version is saved as `<file>.xpkgnew`. Removing a package leaves edited
configuration behind. Hardlinked files stay hardlinked (Mesa ships one driver
under nine names). Setuid bits are kept.

## Package format

A `.xpkg` is a gzip'd ustar archive:

```
pkg-info        NAME, VERSION, DESCRIPTION, ARCH, DEPENDS (key=value)
post-install    optional sh script (install and upgrade)
pre-remove      optional sh script
files/...       installed relative to /
```

Long paths and link targets use pax extended headers. Archives are
reproducible: entries are sorted and carry no timestamps or owners.

```
xpkg-create create --name foo --version 1.0-1 --description "..." \
    --depends bar,baz --stage DIR --output foo-1.0-1.xpkg
xpkg-create index --dir REPO --output REPO/index.json
xpkg-create keygen --out NAME            # NAME.key (secret), NAME.pub
xpkg-create sign --key NAME.key REPO/index.json
```

## The public repository

`https://huggingface.co/datasets/FreeLinX/packages/resolve/main` — about
390 packages: the NetBSD 10.1 userland, the tools from FreeLinX/ports, and the
whole desktop stack (Xorg, Mesa, GTK, FreeLinX Web, Bluetooth, ...).

`tools/publish-repo.sh [--upload]` builds it from `../ports/packages` and
`../Desktop-test/stack/work/pkgs` (made by `stack/package-stack.sh`, which
derives every dependency from the ELF files). A publish stops if any archive
fails `check-nognu.sh` (GCC or glibc code), then writes and signs the index,
checks the signature against `keys/freelinx.pub` and, with `--upload`, mirrors
the directory to Hugging Face (removing archives no longer published). The
signing key stays with the release manager (`~/.config/xpkg/freelinx.key`).

## Building and testing

The desktop stack builds xpkg (`Desktop-test/stack/build-stack.sh`, step
`xpkg`). A static build is available through the `Makefile` against
FreeLinX/ports dependencies.

```
tests/run-tests.sh <xpkg> <xpkg-create> [runner]
```

runs 49 end-to-end checks against a signed repository served on localhost:
dependency resolution, conflicts, config protection, upgrades and stale files,
hardlinks, pax paths, setuid bits, tampered and unsigned indexes, a missing key
directory, oversized downloads, corrupted archives, archives that plant a symlink to write outside
the extraction directory, removal guards and autoremove.

## State

`/var/lib/xpkg/xpkg.db` (packages, files with SHA-256, dependencies; older
databases are migrated on first use), `/var/cache/xpkg`, `/etc/xpkg`. The
FreeLinX image registers its desktop stack at build time, so `xpkg list`
shows what the system actually runs.
