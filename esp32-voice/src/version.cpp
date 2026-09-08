#include "version.h"

#include <stdio.h>

// Composed once, on first use, into a static buffer. It is asked for by the
// console, the display and the power-on self test, and none of those wants to
// own the storage.
//
// Worst case is "255.255.255-" + 7 hex + "-dirty" = 30 characters, so 48 is
// comfortable without being a guess.
static char full[48] = "";

const char* versionSemver() {
  static char semver[16] = "";
  if (semver[0] == '\0') {
    snprintf(semver, sizeof(semver), "%d.%d.%d",
             VOICE_VERSION_MAJOR, VOICE_VERSION_MINOR, VOICE_VERSION_PATCH);
  }
  return semver;
}

const char* versionString() {
  if (full[0] == '\0') {
    snprintf(full, sizeof(full), "%s-%s%s",
             versionSemver(), BUILD_GIT_REV,
             BUILD_GIT_DIRTY ? "-dirty" : "");
  }
  return full;
}

const char* versionGitRev()          { return BUILD_GIT_REV; }
bool        versionGitDirty()        { return BUILD_GIT_DIRTY != 0; }
const char* versionGitBranch()       { return BUILD_GIT_BRANCH; }
const char* versionBuildTimestamp()  { return BUILD_TIMESTAMP; }

uint32_t versionNumeric() {
  return ((uint32_t)VOICE_VERSION_MAJOR << 16) |
         ((uint32_t)VOICE_VERSION_MINOR << 8) |
          (uint32_t)VOICE_VERSION_PATCH;
}
