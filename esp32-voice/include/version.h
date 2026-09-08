// =============================================================================
// version.h - what this firmware is, and which tree it came from.
//
// The version string is semantic version + git provenance:
//
//     1.0.1-c938c17            clean tree, commit c938c17
//     1.0.1-c938c17-dirty      uncommitted work in the tree
//     1.0.1-nogit              built outside a git checkout
//
// The suffix is not decoration. A field report that says "it does X" is
// worthless without knowing which build did X, and the single most common way
// to lose an afternoon is comparing two boards that turn out to be running
// different code. The -dirty flag matters just as much: it says the hash does
// NOT identify the source, because there were uncommitted changes.
//
// The git parts come from include/build_info.h, generated at build time by
// scripts/build_info.py. See docs/01-version.md.
// =============================================================================
#pragma once

#include <stdint.h>

// -----------------------------------------------------------------------------
// The hand-maintained part. Bump these deliberately.
//
//   MAJOR  the over-the-air protocol changed incompatibly, or the config
//          layout changed in a way older firmware cannot read.
//   MINOR  new functionality, still interoperable.
//   PATCH  fixes only.
//
// VOICE_PROTO_VERSION in link.h moves independently and on its own rules -
// two handsets care about that, not about this.
// -----------------------------------------------------------------------------
#define VOICE_VERSION_MAJOR 1
#define VOICE_VERSION_MINOR 0
#define VOICE_VERSION_PATCH 1

// build_info.h is a build artefact and is gitignored, so a fresh checkout that
// has not been built yet does not have it. Tolerate that rather than failing
// the compile with something that looks like a missing dependency: the editor
// and its index will hit this long before the compiler does.
#if defined(__has_include)
#  if __has_include("build_info.h")
#    include "build_info.h"
#  endif
#endif

#ifndef BUILD_GIT_REV
#define BUILD_GIT_REV "unknown"
#endif
#ifndef BUILD_GIT_DIRTY
#define BUILD_GIT_DIRTY 0
#endif
#ifndef BUILD_GIT_BRANCH
#define BUILD_GIT_BRANCH "?"
#endif
#ifndef BUILD_TIMESTAMP
#define BUILD_TIMESTAMP "unknown"
#endif

// "1.0.1-c938c17-dirty". Points at a static buffer built once at first call.
const char* versionString();

// "1.0.1" alone, for anywhere the provenance is noise.
const char* versionSemver();

// The pieces, for the display and for anything that wants to format its own.
const char* versionGitRev();
bool        versionGitDirty();
const char* versionGitBranch();
const char* versionBuildTimestamp();

// Packed as MAJOR<<16 | MINOR<<8 | PATCH, for comparisons and for stamping
// into the config blob so a downgrade is detectable.
uint32_t    versionNumeric();
