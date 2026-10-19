#include "../../src/Pattern.hpp"
#include "../../src/Engine.hpp"
#include <cstdio>
#include <cmath>

using namespace gridseq;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static Pattern pats[NUM_PATTERNS];
static const float DT = 1.f / 1000.f;   // 1 kHz "sample rate" keeps maths simple

// Run n samples with a clock edge on the first one.
static void clockAndRun(Engine& e, int samples, bool running = true, bool reset = false) {
    e.process(DT, pats, true, reset, running);
    for (int i = 1; i < samples; i++)
        e.process(DT, pats, false, false, running);
}

int main() {
    for (int i = 0; i < NUM_PATTERNS; i++) pats[i].reset();
    // pattern 0: note n at step n, plus a higher note at step 0 to test lowest-wins
    for (int s = 0; s < MAX_STEPS; s++) pats[0].cells[s][s] = 1;
    pats[0].cells[0][40] = 1;
    pats[0].length = 4;
    pats[1].cells[0][24] = 1;
    pats[1].length = 4;

    Engine e;
    clockAndRun(e, 100);
    CHECK(e.step == 0);
    CHECK(std::fabs(e.noteVolts - (0 + 36 - 60) / 12.f) < 1e-6);  // lowest note (0) wins over 40
    clockAndRun(e, 100); CHECK(e.step == 1);
    clockAndRun(e, 100); CHECK(e.step == 2);
    clockAndRun(e, 100); CHECK(e.step == 3);
    clockAndRun(e, 100); CHECK(e.step == 0);   // wraps at length 4

    // gate: 50% of 100 ms step = 50 ms
    CHECK(!e.gate);  // already past 50 ms after 100 samples
    e.process(DT, pats, true, false, true);
    CHECK(e.gate);
    for (int i = 0; i < 48; i++) e.process(DT, pats, false, false, true);
    CHECK(e.gate);
    for (int i = 0; i < 3; i++) e.process(DT, pats, false, false, true);
    CHECK(!e.gate);
    CHECK(e.step == 1);
    for (int i = 0; i < 48; i++) e.process(DT, pats, false, false, true);

    // reset: takes effect on next clock
    e.process(DT, pats, false, true, true);
    CHECK(e.step == 1);
    clockAndRun(e, 100); CHECK(e.step == 0);

    // pattern switch waits for end of pattern
    e.requestPattern(1, true);
    CHECK(e.current == 0 && e.pending == 1);
    clockAndRun(e, 100); CHECK(e.step == 1 && e.current == 0);
    clockAndRun(e, 100); clockAndRun(e, 100); CHECK(e.step == 3 && e.current == 0);
    clockAndRun(e, 100); CHECK(e.step == 0 && e.current == 1 && e.pending == -1);
    CHECK(std::fabs(e.noteVolts - (24 + 36 - 60) / 12.f) < 1e-6);

    // stopped: immediate switch, gate low, outputs hold
    float held = e.noteVolts;
    clockAndRun(e, 10, false);
    CHECK(!e.gate && e.step == 0 && e.noteVolts == held);
    e.requestPattern(0, false);
    CHECK(e.current == 0 && e.pending == -1);

    // x2 resolution: two steps per clock
    Engine m;
    pats[0].resolution = 4;
    pats[0].length = 32;
    clockAndRun(m, 100); clockAndRun(m, 100);   // period known after 2nd clock
    int before = m.step;
    clockAndRun(m, 100);
    CHECK(m.step == before + 2);

    // /2 resolution: one step per two clocks
    Engine d;
    pats[0].resolution = 2;
    clockAndRun(d, 100); CHECK(d.step == 0);
    clockAndRun(d, 100); CHECK(d.step == 0);
    clockAndRun(d, 100); CHECK(d.step == 1);

    // shuffle: odd steps delayed by shuffle * half a step
    Engine sh;
    pats[0].resolution = DEFAULT_RESOLUTION;
    sh.shuffle = 1.f;
    clockAndRun(sh, 100); CHECK(sh.step == 0);
    sh.process(DT, pats, true, false, true);   // tick for step 1, should be delayed 50 ms
    CHECK(sh.step == 0);
    for (int i = 0; i < 48; i++) sh.process(DT, pats, false, false, true);
    CHECK(sh.step == 0);
    for (int i = 0; i < 3; i++) sh.process(DT, pats, false, false, true);
    CHECK(sh.step == 1);
    for (int i = 0; i < 48; i++) sh.process(DT, pats, false, false, true);
    sh.process(DT, pats, true, false, true);   // step 2 is even: not delayed
    CHECK(sh.step == 2);

    // tie: 100% gate stays high across consecutive notes, drops on a rest
    Engine t;
    pats[2].reset();
    pats[2].cells[0][10] = 1; pats[2].cells[1][12] = 1;
    pats[2].length = 3;
    pats[2].gateLength = 1.f;
    t.current = 2;
    clockAndRun(t, 100); CHECK(t.gate);
    clockAndRun(t, 100); CHECK(t.gate);
    clockAndRun(t, 100); CHECK(!t.gate);

    printf(failures ? "%d FAILURES\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
