#include "game/runrecord.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>

namespace rl::game {

std::string isoNowUtc() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

Json RunRecord::toJson() const {
    Json j = Json::object();
    j.set("run_id", runId).set("player_id", playerId).set("install_id", installId).set("display_name", displayName);
    j.set("version", version).set("platform", platform).set("started_at", startedAt).set("ended_at", endedAt);
    j.set("duration_s", durationS).set("score", score).set("level", level).set("lines", lines).set("red_lines", redLines);
    j.set("fights", fights).set("pieces", pieces).set("tetrises", tetrises).set("best_chain", bestChain);
    Json kk = Json::array();
    for (int k : killsByKind) kk.push(k);
    j.set("kills_by_kind", kk).set("kills", kills).set("highest_kind", highestKind);
    j.set("blocks_destroyed", blocksDestroyed).set("pickups", pickups);
    static const char* weaponNames[kRunWeapons] = {"shotgun", "chaingun", "rocket_launcher", "plasma_rifle"};
    Json wo = Json::array();
    for (int w = 0; w < kRunWeapons; ++w) if (weaponsOwned[w]) wo.push(weaponNames[w]);
    Json sh = Json::object();
    for (int w = 0; w < kRunWeapons; ++w) sh.set(weaponNames[w], shots[w]);
    j.set("weapons_owned", wo).set("shots", sh).set("damage_taken", damageTaken).set("bfg_used", bfgUsed);
    j.set("death_cause", deathCause).set("killed_by", killedBy).set("seed", static_cast<double>(seed));
    j.set("voxels", voxels).set("brutal", brutal);
    j.set("input", Json::object().set("keyboard", keyboardActions).set("mouse", mouseActions).set("gamepad", padActions));
    Json tr = Json::array();
    for (const std::string& t : trophies) tr.push(t);
    j.set("trophies", tr);
    j.set("machine", Json::object().set("platform", platform).set("os", machineOs).set("gpu", machineGpu).set("cores", machineCores).set("ram", machineRamMb).set("pad", padModel));
    return j;
}

std::string RunRecord::write(const std::string& runsDir) const {
    std::error_code ec;
    std::filesystem::create_directories(runsDir, ec);
    std::string stamp = endedAt.empty() ? isoNowUtc() : endedAt;
    for (char& c : stamp) if (c == ':') c = '-';
    const std::string path = (std::filesystem::path(runsDir) / (stamp + "-" + runId.substr(0, 8) + ".json")).string();
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return "";
    const std::string text = toJson().dump(2);
    std::fwrite(text.data(), 1, text.size(), f);
    std::fputc('\n', f);
    std::fclose(f);
    return path;
}

}  // namespace rl::game
