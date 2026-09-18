#pragma once
// One finished game, as the unit the online leaderboard receives (see
// docs/online-and-releases.md section 3). Written as JSON to the save folder
// at game over whether or not the player is online; the upload reads the file.
#include <string>
#include <vector>

#include "core/json.h"

namespace rl::game {

constexpr int kRunKinds = 13;     // monster kinds counted (fps_mode.h kMonsterKinds)
constexpr int kRunWeapons = 4;    // shotgun, chaingun, rocket launcher, plasma rifle

struct RunRecord {
    std::string runId, playerId, installId, displayName, version, platform;
    std::string startedAt, endedAt;   // ISO 8601 UTC
    double durationS = 0;
    int score = 0, level = 1, lines = 0, redLines = 0, fights = 0, pieces = 0, tetrises = 0, bestChain = 0;
    int killsByKind[kRunKinds] = {};
    int kills = 0, highestKind = -1, blocksDestroyed = 0, pickups = 0;
    bool weaponsOwned[kRunWeapons] = {};
    int shots[kRunWeapons] = {};
    double damageTaken = 0;
    bool bfgUsed = false;
    std::string deathCause = "stack";   // stack | killed | quit
    int killedBy = -1;                  // monster kind, when killed
    unsigned seed = 0;
    bool voxels = false, brutal = false;
    int keyboardActions = 0, mouseActions = 0, padActions = 0;
    std::vector<std::string> trophies;   // ids unlocked during this run
    // What the machine is (section 6 of the notes): stored encrypted server-side, shown only in aggregate.
    std::string machineOs, machineGpu, padModel;
    int machineCores = 0, machineRamMb = 0;

    Json toJson() const;
    // Writes <runsDir>/<endedAt>-<runId>.json (creating the folder); returns the path or "".
    std::string write(const std::string& runsDir) const;
};

std::string isoNowUtc();

}  // namespace rl::game
