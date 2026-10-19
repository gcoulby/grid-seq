#pragma once
#include "Pattern.hpp"


namespace gridseq {

// Clock division for resolution indices below x1, multiplication above it.
inline int resolutionDivide(int resolution) {
    static const int table[NUM_RESOLUTIONS] = {4, 3, 2, 1, 1, 1, 1};
    return table[resolution];
}

inline int resolutionMultiply(int resolution) {
    static const int table[NUM_RESOLUTIONS] = {1, 1, 1, 1, 2, 3, 4};
    return table[resolution];
}

// Playback state for one sequencer. Works in seconds so it never depends on
// the sample rate. Holds no pattern data, only the index of the playing one.
struct Engine {
    // Used for gate length until two clocks have been seen.
    static constexpr float FALLBACK_STEP_PERIOD = 0.125f;

    float shuffle = 0.f;

    int current = 0;
    int pending = -1;
    // -1 means the next step to play is step 0.
    int step = -1;
    bool resetPending = false;

    bool clockSeen = false;
    float timeSinceClock = 0.f;
    float clockPeriod = 0.f;
    int divCounter = 0;
    int subTicksLeft = 0;
    float subTickTimer = 0.f;

    bool stepDelayed = false;
    float delayTimer = 0.f;

    float noteVolts = 0.f;
    float velocity = 0.f;
    float curve = 0.f;
    bool gate = false;
    bool gateTied = false;
    float gateTimer = 0.f;

    void resetAll() {
        *this = Engine();
    }

    int target() const {
        return pending >= 0 ? pending : current;
    }

    // While playing, a new pattern waits for the current one to finish.
    void requestPattern(int index, bool running) {
        if (index == current) {
            pending = -1;
        }
        else if (!running || step < 0) {
            current = index;
            pending = -1;
        }
        else {
            pending = index;
        }
    }

    // Reset is applied on the next clock, not immediately.
    void reset() {
        resetPending = true;
        divCounter = 0;
        subTicksLeft = 0;
        stepDelayed = false;
    }

    float stepPeriod(const Pattern& p) const {
        if (clockPeriod <= 0.f)
            return 0.f;
        return clockPeriod * resolutionDivide(p.resolution) / resolutionMultiply(p.resolution);
    }

    void process(float dt, const Pattern* patterns, bool clockEdge, bool resetEdge, bool running) {
        if (resetEdge)
            reset();

        bool tick = clockTick(dt, patterns[current], clockEdge);

        if (!running) {
            stepDelayed = false;
            gate = false;
            gateTied = false;
            gateTimer = 0.f;
            return;
        }

        if (tick) {
            if (stepDelayed) {
                stepDelayed = false;
                advance(patterns);
            }
            float period = stepPeriod(patterns[current]);
            // Shuffle delays every second step by up to half a step.
            if ((nextStep(patterns[current]) % 2) == 1 && shuffle > 0.f && period > 0.f) {
                stepDelayed = true;
                delayTimer = shuffle * 0.5f * period;
            }
            else {
                advance(patterns);
            }
        }
        else if (stepDelayed) {
            delayTimer -= dt;
            if (delayTimer <= 0.f) {
                stepDelayed = false;
                advance(patterns);
            }
        }

        if (gateTied) {
            gate = true;
        }
        else {
            gate = gateTimer > 0.f;
            gateTimer -= dt;
        }
    }

    bool clockTick(float dt, const Pattern& p, bool clockEdge) {
        int div = resolutionDivide(p.resolution);
        int mul = resolutionMultiply(p.resolution);
        if (divCounter >= div)
            divCounter = 0;

        timeSinceClock += dt;
        if (clockEdge) {
            if (clockSeen)
                clockPeriod = timeSinceClock;
            clockSeen = true;
            timeSinceClock = 0.f;

            // A real clock always resyncs, dropping any multiplied ticks left over.
            subTicksLeft = 0;
            bool tick = (divCounter == 0);
            if (tick) {
                subTicksLeft = mul - 1;
                subTickTimer = 0.f;
            }
            divCounter = (divCounter + 1) % div;
            return tick;
        }

        if (subTicksLeft > 0 && clockPeriod > 0.f) {
            subTickTimer += dt;
            float interval = clockPeriod / mul;
            if (subTickTimer >= interval) {
                subTickTimer -= interval;
                subTicksLeft--;
                return true;
            }
        }
        return false;
    }

    int nextStep(const Pattern& p) const {
        if (resetPending || step < 0 || step + 1 >= p.length)
            return 0;
        return step + 1;
    }

    void advance(const Pattern* patterns) {
        step = nextStep(patterns[current]);
        resetPending = false;
        if (step == 0 && pending >= 0) {
            current = pending;
            pending = -1;
        }
        playStep(patterns[current]);
    }

    void playStep(const Pattern& p) {
        curve = p.curve[step];
        int note = p.lowestNote(step);
        if (note < 0) {
            gateTied = false;
            gateTimer = 0.f;
            return;
        }
        noteVolts = (note + LOWEST_MIDI_NOTE - 60) / 12.f;
        velocity = p.velocity[step];
        // Full gate length ties into the next note instead of retriggering.
        gateTied = p.gateLength >= 1.f;
        float period = stepPeriod(p);
        if (period <= 0.f)
            period = FALLBACK_STEP_PERIOD;
        gateTimer = p.gateLength * period;
    }
};

} // namespace gridseq
