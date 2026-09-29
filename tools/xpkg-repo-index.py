#!/usr/bin/env python3
"""xpkg-repo-index - verify and repair an xpkg repository index.

WHY THIS EXISTS
`xpkg-create index` adds entries for packages it has just built, but it does
not refresh the `size`/`sha256` of entries that already exist.  The documented
publish flow (xpkg/README.md) is "build -> xpkg-create -> upload .xpkg and
index.json to the Hugging Face dataset", so re-uploading a rebuilt package
leaves the published index pointing at the *previous* build's size and hash.

That is not cosmetic.  `xpkg install` downloads the archive and verifies the
index sha256 (src/repo.c, repo_pkg_cached); on mismatch it refuses the package.
The published FreeLinX repo is in exactly that state: measured against the
archives actually being served, 184 of 289 entries (63.7%) carry a stale
size/sha256, so nearly two thirds of `xpkg install <name>` fail even though the
file downloads fine with HTTP 200.

The fix is to make the index a function of the files rather than an append-only
log: regenerate it from the archives right before they are uploaded, and gate
the upload on the check.

MODES
  --check    exit 1 if any entry disagrees with the archive on disk
  --regen    write a corrected index (same key order, same metadata, refreshed
             size/sha256), adding archives that were missing and dropping
             entries whose archive is gone

A local directory is the normal case: you already have every .xpkg you are
about to upload, so nothing is downloaded.  With --base the same logic works
against a published HTTP(S) repo, which is how the current published index was
audited and repaired.

EXAMPLES
  # gate a release
  xpkg-repo-index.py --dir ports/packages --check

  # repair before upload
  xpkg-repo-index.py --dir ports/packages --regen --output /tmp/index.json

  # audit / repair the live repo
  xpkg-repo-index.py --base https://.../resolve/main --regen -o fixed.json
"""

import argparse
import hashlib
import json
import os
import sys
import time
from concurrent.futures import ThreadPoolExecutor

CHUNK = 1 << 20
DEFAULT_BASE = "https://huggingface.co/datasets/FreeLinX/packages/resolve/main"


def sha256_file(path):
    h = hashlib.sha256()
    n = 0
    with open(path, "rb") as f:
        while True:
            b = f.read(CHUNK)
            if not b:
                break
            h.update(b)
            n += len(b)
    return n, h.hexdigest()


def fetch_remote(base, relpath):
    """Download base/relpath, return (size, sha256) or (None, error)."""
    import urllib.request
    url = "%s/%s" % (base.rstrip("/"), relpath)
    h = hashlib.sha256()
    n = 0
    last = None
    for attempt in range(3):
        try:
            with urllib.request.urlopen(url, timeout=120) as r:
                if getattr(r, "status", 200) != 200:
                    return None, "HTTP %s" % getattr(r, "status", "?")
                while True:
                    b = r.read(CHUNK)
                    if not b:
                        break
                    h.update(b)
                    n += len(b)
            return (n, h.hexdigest()), None
        except Exception as e:                     # transient network fault
            last = "%s: %s" % (type(e).__name__, e)
            time.sleep(2 * (attempt + 1))
    return None, last


def read_pkginfo(path):
    """Read NAME/VERSION/DESCRIPTION/ARCH out of an .xpkg's pkg-info member."""
    try:
        import tarfile
        with tarfile.open(path) as t:
            for cand in ("pkg-info", "./pkg-info", "PKGINFO", "./PKGINFO"):
                try:
                    data = t.extractfile(cand).read().decode("utf-8", "replace")
                except (KeyError, AttributeError, TypeError):
                    continue
                fields = {}
                for line in data.splitlines():
                    if "=" in line:
                        k, _, v = line.partition("=")
                        fields[k.strip()] = v.strip()
                if fields.get("NAME"):
                    return fields
            return None
    except Exception:
        return None


def vtuple(v):
    """Version -> comparable tuple. Numeric parts compare numerically, text
    parts lexically, so '0.35.1' < '0.39.0' but '1.10' > '1.9' (string
    comparison would get that backwards, which is exactly the doom case)."""
    out = []
    for part in str(v).replace("-", ".").split("."):
        out.append((0, int(part), "") if part.isdigit() else (1, 0, part))
    return out


def unindexed_to_entries(archives, pk):
    """Decide, for archives the index does not reference, which ones the index
    SHOULD reference.

    Three things make this less trivial than "append one entry per file":

      * the package name is not always the file name (`doom-1.10.xpkg` carries
        NAME=doom), so entries must be keyed by pkg-info NAME;
      * a newer build of an already-indexed package must SUPERSEDE the old
        entry, because a JSON object cannot hold two `doom` keys and leaving
        the old row is the actual defect - the repo then advertises
        doom-0.1.xpkg forever while doom-1.10.xpkg is invisible to clients;
      * and the reverse: an archive that is OLDER than what the index already
        serves must NOT be promoted.  Repos legitimately keep previous
        archives for rollback, and a naive "one entry per file" pass silently
        downgrades the repo to the oldest build on disk.

    Returns (promote, superseded, reject) where promote maps name -> entry for
    names the index does not serve at all, superseded maps name ->
    (old_entry, new_entry) for downgrades-in-reverse (newer on disk), and
    reject is a list of (filename, name, indexed_version, disk_version).
    """
    promote, superseded, reject = {}, {}, []
    for fname, path in sorted(archives.items()):
        info = read_pkginfo(path)
        if not info:
            reject.append((fname, None, None, "no pkg-info"))
            continue
        name = info["NAME"]
        ver = info.get("VERSION", "")
        if "%s-%s.xpkg" % (name, ver) != fname:
            # Not the canonical <name>-<version>.xpkg name; leave it alone
            # rather than guessing at an identity for it.
            reject.append((fname, name, None, "non-canonical file name"))
            continue

        if name in pk:
            have = str(pk[name].get("version", ""))
            if fname == pk[name].get("file"):
                reject.append((fname, name, have, "already indexed"))
            elif vtuple(ver) > vtuple(have):
                n, sha = sha256_file(path)
                superseded[name] = (dict(pk[name]), {
                    "version": ver,
                    "description": info.get("DESCRIPTION", ""),
                    "arch": info.get("ARCH", ""),
                    "file": fname,
                    "size": n,
                    "sha256": sha,
                })
            else:
                reject.append((fname, name, have, "older than indexed v%s" % have))
            continue

        n, sha = sha256_file(path)
        promote[name] = {
            "version": ver,
            "description": info.get("DESCRIPTION", ""),
            "arch": info.get("ARCH", ""),
            "file": fname,
            "size": n,
            "sha256": sha,
        }
    return promote, superseded, reject


def load_index(path_or_url):
    src = path_or_url
    if src.startswith("http://") or src.startswith("https://"):
        import urllib.request
        with urllib.request.urlopen(src, timeout=120) as r:
            return json.load(r)
    with open(src) as f:
        return json.load(f)


def split_index(doc):
    """Return (packages_dict, other_top_level_keys)."""
    if isinstance(doc, dict) and "packages" in doc and isinstance(doc["packages"], dict):
        return doc["packages"], {k: v for k, v in doc.items() if k != "packages"}
    return doc, {}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--dir", help="local directory holding *.xpkg and index.json")
    g.add_argument("--base", help="base URL of a published repo (implies %s)" % DEFAULT_BASE)

    ap.add_argument("--index", help="index.json to read (default <dir>/index.json or <base>/index.json)")
    ap.add_argument("-o", "--output", help="where --regen writes (default: --index, or <dir>/index.json)")
    ap.add_argument("--regen", action="store_true", help="write a corrected index")
    ap.add_argument("--check", action="store_true", help="report only; exit 1 on any disagreement")
    ap.add_argument("--jobs", type=int, default=6, help="parallel downloads for --base (default 6)")
    ap.add_argument("--limit", type=int, default=0, help="only process the first N entries (debugging)")
    ap.add_argument("-v", "--verbose", action="store_true", help="list every disagreement")
    a = ap.parse_args()

    if not a.regen and not a.check:
        a.check = True

    if a.dir:
        index_path = a.index or os.path.join(a.dir, "index.json")
        if not os.path.isfile(index_path):
            sys.exit("error: no index at %s" % index_path)
        local = {}
        for name in os.listdir(a.dir):
            if name.endswith(".xpkg") and os.path.isfile(os.path.join(a.dir, name)):
                local[name] = os.path.join(a.dir, name)
    else:
        base = a.base if "://" in a.base else DEFAULT_BASE
        index_path = a.index or (base.rstrip("/") + "/index.json")
        local = None

    doc = load_index(index_path)
    pk, extra = split_index(doc)
    items = sorted(pk.items())
    if a.limit:
        items = items[:a.limit]

    print("index : %s" % index_path)
    print("entries: %d%s" % (len(items), "  (limited to %d)" % a.limit if a.limit else ""))
    if local is not None:
        print("archives on disk: %d" % len(local))
    print()

    actual = {}
    if local is not None:
        for name, path in local.items():
            actual[name] = sha256_file(path)
    else:
        t0 = time.time()
        with ThreadPoolExecutor(max_workers=a.jobs) as ex:
            futs = {ex.submit(fetch_remote, base, v.get("file", "")): (k, v) for k, v in items}
            done = 0
            for fut, (k, v) in futs.items():
                got, err = fut.result()
                done += 1
                if got is None:
                    print("[%d/%d] UNREACHABLE %s: %s" % (done, len(items), k, err))
                    continue
                actual[v["file"]] = got
                if done % 50 == 0:
                    print("[%d/%d] verified  %.0fs" % (done, len(items), time.time() - t0))
        print("verified %d archives in %.0fs\n" % (len(actual), time.time() - t0))

    good, wrong_size, wrong_sha, missing_file, unreachable, unindexed = [], [], [], [], [], []
    for k, v in items:
        fname = v.get("file")
        got = actual.get(fname)
        if got is None:
            unreachable.append((k, fname))
            continue
        n, sha = got
        bad_n = (v.get("size") != n)
        bad_h = (str(v.get("sha256", "")).lower() != sha)
        if bad_n and bad_h:
            wrong_size.append((k, v.get("size"), n, v.get("sha256"), sha))
        elif bad_h:
            wrong_sha.append((k, v.get("size"), n, v.get("sha256"), sha))
        elif bad_n:
            wrong_size.append((k, v.get("size"), n, v.get("sha256"), sha))
        else:
            good.append(k)

    promote, superseded, reject = {}, {}, []
    if local is not None:
        indexed_files = {v.get("file") for _, v in items}
        unindexed = sorted(f for f in local if f not in indexed_files)
        promote, superseded, reject = unindexed_to_entries(
            {f: local[f] for f in unindexed}, {k: v for k, v in items})
        # Only archives that the index ought to reference but does not are a
        # problem.  Older leftovers are fine: repos keep previous archives for
        # rollback, and promoting one of those would downgrade the repo.
        pending_files = {e["file"] for e in promote.values()}
        pending_files |= {e["file"] for _, e in superseded.values()}
        unindexed = sorted(pending_files)
    for k, fname in unreachable:
        if local is not None and fname not in local:
            missing_file.append((k, fname))

    total = len(items)
    print("=" * 68)
    print("  correct                       %4d  (%.1f%%)" % (len(good), 100.0 * len(good) / total))
    print("  wrong size + sha256           %4d  (%.1f%%)" % (len(wrong_size), 100.0 * len(wrong_size) / total))
    print("  wrong sha256 only             %4d  (%.1f%%)" % (len(wrong_sha), 100.0 * len(wrong_sha) / total))
    if missing_file:
        print("  archive missing locally       %4d" % len(missing_file))
    if unreachable:
        print("  unreachable (not verified)    %4d" % len(unreachable))
    if unindexed:
        print("  archive on disk not indexed   %4d  %s" % (len(unindexed), unindexed[:6]))
    if reject:
        print("  archives correctly skipped   %4d" % len(reject))
    print("=" * 68)

    bad_total = len(wrong_size) + len(wrong_sha) + len(missing_file)
    if bad_total:
        print("\n`xpkg install` REFUSES these %d packages (sha256 verified after"
              % bad_total)
        print("download, src/repo.c). The archive downloads fine; the index is stale.")
        if a.verbose or bad_total <= 20:
            for k, os_, oa, osha, nsha in (wrong_size + wrong_sha)[:60]:
                print("    %-22s size %-10s -> %-10s  sha %s -> %s"
                      % (k, os_, oa, str(osha)[:10], nsha[:10]))
        else:
            print("    (first 60 shown; -v for all)")
    if unindexed:
        print("\nThese %d archives are on disk but unreachable through the index;" % len(unindexed))
        print("`xpkg install <name>` cannot find them until the index is regenerated:")
        for f in unindexed[:20]:
            kind = ""
            for name, (old, new) in superseded.items():
                if new["file"] == f:
                    kind = "   SUPERSEDES %s (v%s)" % (old["file"], old.get("version", "?"))
            print("    %-28s%s" % (f, kind))
        if superseded:
            print("\nThe SUPERSEDES lines matter: a newer build of an already-indexed")
            print("package cannot be added as a second JSON key, so without --regen")
            print("the repo advertises the old version forever.")
    if not bad_total and not unindexed:
        print("\nindex and archives agree.")
        stale = [r for r in reject if r[3].startswith("older")]
        if stale:
            print("%d superseded archive(s) kept for rollback and correctly not"
                  % len(stale))
            print("referenced by the index: %s" % ", ".join(r[0] for r in stale[:6]))

    if not a.regen:
        return 1 if bad_total or unindexed else 0

    out = {"packages": {}}
    for k, v in items:
        nv = dict(v)
        got = actual.get(v.get("file"))
        if got is not None:
            nv["size"], nv["sha256"] = got
        out["packages"][k] = nv
    for k, v in extra.items():
        out[k] = v

    # A newer build of an already-indexed package replaces the old entry; a
    # package the index does not serve at all is added.  Both are needed: the
    # index is a JSON object keyed by name, so a superseding build can only be
    # expressed by overwriting the row, never by appending a second one.
    for name, (old, new) in sorted(superseded.items()):
        out["packages"][name] = new
    for name, entry in sorted(promote.items()):
        out["packages"][name] = entry

    out_path = a.output or index_path
    tmp = out_path + ".new"
    with open(tmp, "w") as f:
        json.dump(out, f, indent=2, sort_keys=True)
        f.write("\n")
    os.replace(tmp, out_path)
    fixed = len(wrong_size) + len(wrong_sha)
    print("\n--regen wrote %s (%d bytes, %d entries)"
          % (out_path, os.path.getsize(out_path), len(out["packages"])))
    print("        refreshed %d stale entries, kept %d already-correct" % (fixed, len(good)))
    for name, (old, new) in sorted(superseded.items()):
        print("        superseded %s: %s (v%s) -> %s (v%s)"
              % (name, old.get("file"), old.get("version", "?"),
                 new["file"], new.get("version", "?")))
    for name, entry in sorted(promote.items()):
        print("        added       %s: %s (v%s)" % (name, entry["file"], entry["version"]))
    unresolved = [r for r in reject if not r[3].startswith(("older", "already", "non-canonical"))]
    if unresolved:
        print("        WARNING: %d archive(s) could not be indexed:" % len(unresolved))
        for f, nm, ver, why in unresolved[:10]:
            print("          %-28s (%s)" % (f, why))
        for f in unresolved[:10]:
            print("          %s" % f)
    return 0


if __name__ == "__main__":
    sys.exit(main())
