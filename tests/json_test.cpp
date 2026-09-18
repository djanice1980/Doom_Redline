// JSON round trips, escapes, unicode, error handling, and the run record shape.
#include <cstdio>
#include <string>

#include "core/json.h"
#include "game/runrecord.h"

using rl::Json;

namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
}

int main() {
    Json j;
    std::string err;
    CHECK(Json::parse(R"({"a": 1, "b": [true, null, "x\ny", 2.5], "c": {"d": "é😀"}, "e": -3e2})", j, &err));
    CHECK(err.empty());
    CHECK(j["a"].asInt() == 1);
    CHECK(j["b"].size() == 4);
    CHECK(j["b"][0].asBool());
    CHECK(j["b"][1].isNull());
    CHECK(j["b"][2].asString() == "x\ny");
    CHECK(j["b"][3].asNumber() == 2.5);
    CHECK(j["c"]["d"].asString() == "\xC3\xA9\xF0\x9F\x98\x80");
    CHECK(j["e"].asNumber() == -300.0);
    CHECK(j["missing"].isNull());
    CHECK(j["missing"]["deeper"].asString("dflt") == "dflt");
    CHECK(j["b"][99].isNull());

    // Round trip through the writer.
    Json back;
    CHECK(Json::parse(j.dump(), back, &err));
    CHECK(back.dump() == j.dump());
    CHECK(Json::parse(j.dump(2), back, &err));
    CHECK(back.dump() == j.dump());

    // Writer escapes.
    Json s = Json::object().set("k", "q\"b\\s\n\t\x01");
    CHECK(s.dump() == "{\"k\":\"q\\\"b\\\\s\\n\\t\\u0001\"}");
    CHECK(Json(42).dump() == "42");
    CHECK(Json(2.5).dump() == "2.5");
    CHECK(Json(true).dump() == "true");
    CHECK(Json().dump() == "null");

    // Errors do not throw.
    CHECK(!Json::parse("{\"a\": }", j, &err));
    CHECK(!Json::parse("[1, 2", j, &err));
    CHECK(!Json::parse("\"unterminated", j, &err));
    CHECK(!Json::parse("{} trailing", j, &err));
    CHECK(!Json::parse("", j, &err));

    // The run record serialises every field.
    rl::game::RunRecord r;
    r.runId = "0123456789abcdef"; r.playerId = "p"; r.installId = "i"; r.displayName = "MARINE"; r.version = "0.1.0"; r.platform = "Linux";
    r.startedAt = "2026-09-18T10:00:00Z"; r.endedAt = "2026-09-18T10:05:00Z"; r.durationS = 300; r.score = 12345; r.level = 3;
    r.killsByKind[1] = 4; r.killsByKind[12] = 1; r.kills = 5; r.highestKind = 12; r.weaponsOwned[0] = r.weaponsOwned[2] = true; r.shots[0] = 20;
    r.trophies = {"first_blood", "red_line"};
    Json rj = r.toJson();
    CHECK(rj["score"].asInt() == 12345);
    CHECK(rj["kills_by_kind"].size() == rl::game::kRunKinds);
    CHECK(rj["kills_by_kind"][12].asInt() == 1);
    CHECK(rj["weapons_owned"].size() == 2);
    CHECK(rj["weapons_owned"][1].asString() == "rocket_launcher");
    CHECK(rj["shots"]["shotgun"].asInt() == 20);
    CHECK(rj["trophies"][1].asString() == "red_line");
    CHECK(rj["input"]["keyboard"].asInt() == 0);
    CHECK(rl::game::isoNowUtc().size() == 20);

    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("json_test: all passed");
    return 0;
}
