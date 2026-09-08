# 01 — Firmware version and build provenance

**Status:** implemented · **Files:** [`scripts/build_info.py`](../scripts/build_info.py), [`include/version.h`](../include/version.h), [`src/version.cpp`](../src/version.cpp)

## What it is

Every build reports a version string that identifies both *what* it is and
*where it came from*:

```
1.0.1-c938c17            semver + commit, clean tree
1.0.1-c938c17-dirty      uncommitted changes were present
1.0.1-nogit              built outside a git checkout
```

The semver part is hand-maintained in `version.h`. The git part is generated at
build time.

## Why the suffix matters

A field report that says "it does X" is worthless without knowing which build
did X, and the single most reliable way to lose an afternoon is comparing two
boards that turn out to be running different code.

The `-dirty` flag carries its own information, and it is the part people skip:
it says the hash does **not** identify the source, because the tree had
uncommitted changes when the image was built. A dirty build is not
reproducible. The console prints that in words rather than as a symbol,
precisely because a symbol is easy to read past.

## How to see it

| Where | How |
| --- | --- |
| Boot log | first line after the banner, before anything can fail |
| Serial | `version` (alias `ver`) |
| Display | VERSION screen — long-press MODE to cycle, or `screen 4` |

```
firmware  1.0.1-c938c17-dirty
semver    1.0.1
git       c938c17 on feature/lora_radio  (TREE WAS DIRTY - the hash does not identify the source)
built     2026-09-04T14:03:39Z
protocol  v1
```

The version is printed **before** any initialisation runs. A boot log that dies
half way through is still useful if it named the build first.

## Three version numbers, deliberately separate

| Number | Lives in | Changes when |
| --- | --- | --- |
| Firmware semver | `version.h` | this codebase changes |
| Protocol version | `link.h` → `VOICE_PROTO_VERSION` | the bytes on the air change |
| Config version | `configstore.h` | the NVS layout changes |

They move on their own rules. Two handsets only care about the protocol
version; the config version only matters to a board reading its own NVS. Fusing
them would mean a cosmetic display fix forced a protocol mismatch.

Semver bumping rules for the firmware number:

- **MAJOR** — the over-the-air protocol changed incompatibly, or the config
  layout changed such that older firmware cannot read it
- **MINOR** — new functionality, still interoperable
- **PATCH** — fixes only

## How the git info gets in

`scripts/build_info.py` runs as a PlatformIO `pre:` script and writes
`include/build_info.h`:

```c
#define BUILD_GIT_REV    "c938c17"
#define BUILD_GIT_DIRTY  1
#define BUILD_GIT_BRANCH "feature/lora_radio"
#define BUILD_TIMESTAMP  "2026-09-04T14:03:39Z"
```

Two decisions in that script are worth knowing about, because the obvious
implementations of both are wrong:

**It generates a header, not `-D` flags.** Project-wide defines are part of
every translation unit's command line. The git hash changes on every commit, so
`-DGIT_REV=...` would invalidate the command line of every file and rebuild the
entire project — including all of Codec2 — on every single commit. A header is
a dependency of exactly the one file that includes it.

**It only rewrites the file when the content changes.** The timestamp differs on
every invocation, so writing unconditionally would make the header permanently
newer than its object file, and we would be back to rebuilding constantly. The
comparison deliberately ignores the timestamp line: a build where nothing else
changed does not churn. The console output says which happened:

```
build_info: c938c17-dirty (unchanged)
```

**Dirty means dirty.** `git status --porcelain` reports modified tracked files
*and* untracked ones, and both count — a build with new uncommitted files in the
tree is no more reproducible from the hash than one with modified files.

## Absent git

Not an error. A firmware built from a tarball is still a firmware; it just
cannot say which commit it came from, and reports `nogit`. The script catches
every failure mode — git missing, not a repository, git refusing — and carries
on.

## Fresh checkout

`include/build_info.h` is **gitignored**: it is a build artefact, and committing
it would mean every commit dirties the tree that the next commit then records.

That means a fresh checkout does not have it until the first build. `version.h`
tolerates this with `__has_include` and falls back to `"unknown"`, so an editor
indexing the tree before anything has been built does not show a wall of errors
that look like missing dependencies.
