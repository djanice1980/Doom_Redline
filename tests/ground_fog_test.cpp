// Ground fog field: bodies plough it, a fast body leaves an unbroken furrow, blasts
// throw it and it flows back in, eddies curl on in a wake, walls hold it, it keeps
// moving by itself, it carries over when the area grows, the texture carries the
// flow, and a full-size step stays cheap.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "game/ground_fog.h"

using rl::game::GroundFog;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static void run(GroundFog& f, float seconds) {
    for (int s = 0; s < static_cast<int>(seconds * 30.f + 0.5f); ++s) f.update(GroundFog::kStep);
}

int main() {
    GroundFog f;
    f.setBounds(-16.f, -3.f, 16.f, 24.f);
    f.setRest([](float, float) { return 1.f; }, true);
    f.setSwell(0.f);   // still air for the measurements; the swell gets its own check
    CHECK(f.width() > 0 && f.width() <= GroundFog::kMaxCells);
    CHECK(f.densityAt(0.f, 10.f) > 0.99f);

    // A runner crossing the room leaves a thinner trail behind it and shoulders a
    // ridge up beside it.
    glm::vec3 p(-10.f, 0.f, 10.f), v(6.f, 0.f, 0.f);
    float ridge = 0.f;
    for (int s = 0; s < 90; ++s) {
        f.clearBodies(); f.addBody(p, v, 0.7f); f.update(GroundFog::kStep); p += v * GroundFog::kStep;
        for (float dz = 0.6f; dz < 2.f; dz += 0.1f) ridge = std::max(ridge, f.densityAt(p.x - 1.f, 10.f + dz));
    }
    const float trail = f.densityAt(2.f, 10.f), aside = f.densityAt(2.f, 16.f);
    std::printf("trail %.3f, ridge %.3f, untouched %.3f\n", trail, ridge, aside);
    CHECK(trail < 0.85f);
    CHECK(ridge > 1.03f);
    CHECK(std::fabs(aside - 1.f) < 0.03f);

    // It settles back.
    f.clearBodies();
    run(f, 12.f);
    std::printf("after 12 s: %.3f\n", f.densityAt(2.f, 10.f));
    CHECK(std::fabs(f.densityAt(2.f, 10.f) - 1.f) < 0.05f);

    // A rocket at 25 m/s crosses more than a cell per step: its furrow is unbroken.
    {
        GroundFog g;
        g.setBounds(-16.f, -3.f, 16.f, 24.f);
        g.setRest([](float, float) { return 1.f; }, true);
        g.setSwell(0.f);
        glm::vec3 r(-12.f, 0.5f, 5.f), rv(25.f, 0.f, 0.f);
        for (int s = 0; s < 30; ++s) { g.clearBodies(); g.addBody(r, rv, 1.3f); g.update(GroundFog::kStep); r += rv * GroundFog::kStep; }
        g.clearBodies();
        float worst = 0.f, wind = 0.f;
        for (float x = -8.f; x < 8.f; x += 0.1f) { worst = std::max(worst, g.densityAt(x, 5.f)); wind = std::max(wind, g.speedAt(x, 5.f)); }
        std::printf("rocket furrow: thickest point %.3f, wind %.2f m/s\n", worst, wind);
        CHECK(worst < 0.9f);
        CHECK(wind > 2.f);
        run(g, 15.f);
        std::printf("rocket furrow after 15 s: %.3f\n", g.densityAt(0.f, 5.f));
        CHECK(std::fabs(g.densityAt(0.f, 5.f) - 1.f) < 0.06f);
    }

    // A wake keeps curling: with the spin fed back, the eddies a runner leaves are
    // several times stronger a second and two seconds on than with it off, they die
    // away in the end, and still air stays still.
    {
        auto peakSpin = [](const GroundFog& g) {
            float peak = 0.f;
            const float h = 0.2f;
            for (float x = -9.f; x < 5.f; x += 0.2f)
                for (float z = 7.f; z < 13.f; z += 0.2f) {
                    const float w = (g.flowAt(x + h, z).y - g.flowAt(x - h, z).y - g.flowAt(x, z + h).x + g.flowAt(x, z - h).x) / (2.f * h);
                    peak = std::max(peak, std::fabs(w));
                }
            return peak;
        };
        float at1[2], at2[2], late = 0.f;
        for (int on = 0; on < 2; ++on) {
            GroundFog g;
            g.setBounds(-16.f, -3.f, 16.f, 24.f);
            g.setRest([](float, float) { return 1.f; }, true);
            g.setSwell(0.f);
            if (!on) g.setSpinKeep(0.f);
            glm::vec3 q(-10.f, 0.f, 10.f), qv(5.f, 0.f, 0.f);
            for (int s = 0; s < 90; ++s) { g.clearBodies(); g.addBody(q, qv, 0.7f); g.update(GroundFog::kStep); q += qv * GroundFog::kStep; }
            g.clearBodies();
            run(g, 1.f); at1[on] = peakSpin(g);
            run(g, 1.f); at2[on] = peakSpin(g);
            if (on) { run(g, 30.f); late = g.spin(); }
        }
        std::printf("wake spin: 1 s on %.2f/s (off %.2f), 2 s on %.2f/s (off %.2f), 32 s %.4f\n", at1[1], at1[0], at2[1], at2[0], late);
        CHECK(at1[1] > 3.f * at1[0] && at1[1] > 1.5f);
        CHECK(at2[1] > 5.f * at2[0]);
        CHECK(late < 0.005f);
    }

    // The texture carries the flow: still fog is exactly 128 in both velocity bytes, and
    // a runner's wake shows up there.
    {
        GroundFog g;
        g.setBounds(-8.f, -8.f, 8.f, 8.f);
        g.setRest([](float, float) { return 1.f; }, true);
        g.setSwell(0.f);
        g.update(GroundFog::kStep);
        CHECK(g.pixels().size() == static_cast<size_t>(g.width()) * g.height() * 4);
        CHECK(g.pixels()[2] == 128 && g.pixels()[3] == 128);
        glm::vec3 q(-5.f, 0.f, 0.f), qv(5.f, 0.f, 0.f);
        for (int s = 0; s < 30; ++s) { g.clearBodies(); g.addBody(q, qv, 0.7f); g.update(GroundFog::kStep); q += qv * GroundFog::kStep; }
        int moved = 0;
        for (size_t k = 0; k < g.pixels().size(); k += 4) moved += std::abs(static_cast<int>(g.pixels()[k + 2]) - 128) > 10;
        std::printf("texture: %d cells show the wake\n", moved);
        CHECK(moved > 20);
    }

    // The wisp pattern tiles: the last column runs on into the first as smoothly as
    // any two neighbouring columns do, and so do the rows; and it has wisps and gaps.
    {
        const int n = 256;
        const std::vector<uint8_t> w = GroundFog::wispPattern(n);
        CHECK(w.size() == static_cast<size_t>(n) * n);
        double inside = 0.0, seamX = 0.0, seamY = 0.0, mean = 0.0;
        int lo = 255, hi = 0;
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i) {
                const int v = w[static_cast<size_t>(j * n + i)];
                lo = std::min(lo, v); hi = std::max(hi, v); mean += v;
                if (i + 1 < n) inside += std::abs(v - w[static_cast<size_t>(j * n + i + 1)]);
            }
        for (int j = 0; j < n; ++j) seamX += std::abs(w[static_cast<size_t>(j * n + n - 1)] - w[static_cast<size_t>(j * n)]);
        for (int i = 0; i < n; ++i) seamY += std::abs(w[static_cast<size_t>((n - 1) * n + i)] - w[static_cast<size_t>(i)]);
        inside /= static_cast<double>(n) * (n - 1);
        seamX /= n; seamY /= n; mean /= static_cast<double>(n) * n;
        std::printf("wisp pattern: step between columns %.2f, across the seams %.2f / %.2f, range %d..%d, mean %.0f\n", inside, seamX, seamY, lo, hi, mean);
        CHECK(seamX < 2.5 * inside && seamY < 2.5 * inside);
        CHECK(lo < 10 && hi > 245);
    }

    // A blast empties its middle and throws a ring up around it, and the fog flows
    // back in: the hole fills from its edges far faster than the slow refill alone.
    f.blast(glm::vec3(0.f, 0.f, 5.f), 3.f);
    f.update(GroundFog::kStep);
    std::printf("blast centre %.3f\n", f.densityAt(0.f, 5.f));
    CHECK(f.densityAt(0.f, 5.f) < 0.6f);
    float ring = 0.f, low = 1.f;
    for (int s = 0; s < 45; ++s) {
        f.update(GroundFog::kStep);
        low = std::min(low, f.densityAt(0.f, 5.f));
        for (float a = 0.f; a < 6.28f; a += 0.3f) ring = std::max(ring, f.densityAt(3.f * std::cos(a), 5.f + 3.f * std::sin(a)));
    }
    run(f, 2.5f);
    const float back = f.densityAt(0.f, 5.f);
    std::printf("blast: lowest %.3f, ring %.3f, centre after 4 s %.3f\n", low, ring, back);
    CHECK(ring > 1.1f && ring < GroundFog::kMaxDepth - 0.05f);
    CHECK(back > 0.8f);

    // Walls hold it: a blast against one side of a wall leaves the other side alone.
    {
        GroundFog g;
        g.setBounds(-8.f, -8.f, 8.f, 8.f);
        g.setRest([](float, float) { return 1.f; }, true);
        g.setSwell(0.f);
        g.setSolid([](float x, float) { return std::fabs(x) < 0.5f; });
        g.blast(glm::vec3(-2.f, 0.f, 0.f), 3.f);
        float far = 0.f;
        for (int s = 0; s < 60; ++s) { g.update(GroundFog::kStep); far = std::max(far, std::fabs(g.densityAt(1.5f, 0.f) - 1.f)); }
        std::printf("wall: far side moved by %.4f\n", far);
        CHECK(far < 0.01f);
    }

    // Left alone with the swell on, it keeps moving and stays about as deep as its rest.
    {
        GroundFog g;
        g.setBounds(-16.f, -3.f, 16.f, 24.f);
        g.setRest([](float, float) { return 1.f; }, true);
        g.setSwell(1.f);
        run(g, 20.f);
        const float a = g.densityAt(3.f, 7.f);
        run(g, 3.f);
        const float b = g.densityAt(3.f, 7.f);
        float lo = 9.f, hi = 0.f, speed = 0.f;
        for (float z = 0.f; z < 20.f; z += 0.5f)
            for (float x = -12.f; x < 12.f; x += 0.5f) { const float d = g.densityAt(x, z); lo = std::min(lo, d); hi = std::max(hi, d); speed += g.speedAt(x, z); }
        speed /= 40.f * 48.f;
        std::printf("swell: mean %.3f, range %.3f..%.3f, drift %.2f m/s, change over 3 s %.3f\n", g.meanDensity(), lo, hi, speed, std::fabs(b - a));
        CHECK(std::fabs(g.meanDensity() - 1.f) < 0.1f);
        CHECK(hi - lo > 0.1f);
        CHECK(speed > 0.05f && speed < 1.f);
    }

    // Growing the area keeps what was there; the new part starts at its rest value.
    f.blast(glm::vec3(0.f, 0.f, 5.f), 3.f);
    f.update(GroundFog::kStep);
    f.setBounds(-34.f, -90.f, 34.f, 24.f);
    f.setRest([](float, float z) { return z < -3.f ? 1.f : 0.3f; }, false);
    std::printf("after grow: blast %.3f, crypt %.3f\n", f.densityAt(0.f, 5.f), f.densityAt(0.f, -40.f));
    CHECK(f.densityAt(0.f, 5.f) < 0.7f);
    CHECK(f.densityAt(0.f, -40.f) > 0.99f);

    // Cost of a full-size step, swell on.
    f.setSwell(1.f);
    const int steps = 300;
    auto t0 = std::chrono::steady_clock::now();
    for (int s = 0; s < steps; ++s) {
        f.clearBodies();
        for (int m = 0; m < 16; ++m) f.addBody(glm::vec3(-20.f + 2.5f * m, 0.f, -30.f), glm::vec3(3.f, 0.f, 0.f), 1.f);
        f.update(GroundFog::kStep);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / steps;
    std::printf("%dx%d cells (%.2f m): %.3f ms per step\n", f.width(), f.height(), f.cell(), ms);
    CHECK(ms < 4.0);
    if (failures == 0) std::printf("ground_fog_test: ok\n");
    return failures == 0 ? 0 : 1;
}
