// PlayerStats: identity persistence, counters per machine, formatting.
#include <cstdio>
#include <filesystem>
#include <string>

#include "game/playerstats.h"

using namespace rl::game;

namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
}

int main() {
    std::filesystem::path dir = std::filesystem::temp_directory_path() / ("redline_stats_test_" + newUuid().substr(0, 8));
    std::filesystem::create_directories(dir);
    MachineInfo a{newUuid(), "Linux", "TestOS 1", "Test GPU", 8, 16384};
    MachineInfo b{newUuid(), "Windows", "Windows 11", "Other GPU", 16, 32768};

    std::string id;
    {
        PlayerStats s;
        s.load(dir.string(), a);
        id = s.playerId();
        CHECK(id.size() == 36 && id[14] == '4');   // version-4 UUID
        CHECK(s.email().empty() && !s.emailVerified());
        CHECK(s.lifetime().sessions == 1 && s.thisMachine().sessions == 1 && s.machineCount() == 1);
        s.addTime(PlayPhase::Blocks, 90.0);
        s.addTime(PlayPhase::Fps, 30.5);
        s.addInput(InputDevice::Keyboard);
        s.addInput(InputDevice::Keyboard);
        s.addInput(InputDevice::Gamepad);
        s.setPadModel("Test Pad");
        s.setEmail("someone@example.com");
        s.save();
    }
    {
        // Same profile on a second machine: identity and lifetime totals carry over, the machine counters start fresh.
        PlayerStats s;
        s.load(dir.string(), b);
        CHECK(s.playerId() == id);
        CHECK(s.email() == "someone@example.com" && !s.emailVerified());
        CHECK(s.lifetime().sessions == 2 && s.thisMachine().sessions == 1);
        CHECK(s.machineCount() == 2);
        CHECK(s.lifetime().blocksSeconds > 89.9 && s.lifetime().fpsSeconds > 30.4 && s.thisMachine().total() == 0.0);
        CHECK(s.lifetime().keyboardActions == 2 && s.lifetime().padActions == 1 && s.thisMachine().keyboardActions == 0);
        s.addTime(PlayPhase::Menu, 5.0);
        s.save();
    }
    {
        // Back on the first machine: its own counters were kept.
        PlayerStats s;
        s.load(dir.string(), a);
        CHECK(s.thisMachine().sessions == 2 && s.thisMachine().blocksSeconds > 89.9);
        CHECK(s.lifetime().sessions == 3 && s.lifetime().menuSeconds > 4.9);
        CHECK(s.machineCount() == 2);
    }
    CHECK(PlayerStats::formatDuration(3905) == "1H 05M");
    CHECK(PlayerStats::formatDuration(723) == "12M 03S");
    CHECK(newUuid() != newUuid());
    std::filesystem::remove_all(dir);
    if (failures == 0) std::printf("stats_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
