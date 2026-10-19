#pragma once
#include <cstdint>


namespace gridseq {

static const int MAX_STEPS = 32;
static const int DEFAULT_LENGTH = 16;

// Five octaves are stored; the grid shows three of them at a time.
static const int NUM_NOTES = 60;
static const int VISIBLE_NOTES = 36;
static const int NUM_OCTAVE_VIEWS = (NUM_NOTES - VISIBLE_NOTES) / 12 + 1;
// MIDI number of stored note 0 (C2). Note CV is 0 V at MIDI 60 (C4).
static const int LOWEST_MIDI_NOTE = 36;

static const int NUM_BANKS = 4;
static const int PATTERNS_PER_BANK = 8;
static const int NUM_PATTERNS = NUM_BANKS * PATTERNS_PER_BANK;

// Index into the resolution table in Engine.hpp: /4 /3 /2 x1 x2 x3 x4
static const int NUM_RESOLUTIONS = 7;
static const int DEFAULT_RESOLUTION = 3;

static const float DEFAULT_VELOCITY = 0.8f;
static const float DEFAULT_CURVE = 0.f;
static const float DEFAULT_GATE_LENGTH = 0.5f;
static const float MIN_GATE_LENGTH = 0.05f;

// Cells are one byte each so the UI thread can toggle a cell with a single
// write while the audio thread reads the pattern, without a lock.
struct Pattern {
    uint8_t cells[MAX_STEPS][NUM_NOTES];
    float velocity[MAX_STEPS];
    float curve[MAX_STEPS];
    int length;
    int resolution;
    float gateLength;

    Pattern() {
        reset();
    }

    void clearSteps() {
        for (int s = 0; s < MAX_STEPS; s++) {
            for (int n = 0; n < NUM_NOTES; n++)
                cells[s][n] = 0;
            velocity[s] = DEFAULT_VELOCITY;
            curve[s] = DEFAULT_CURVE;
        }
    }

    void reset() {
        clearSteps();
        length = DEFAULT_LENGTH;
        resolution = DEFAULT_RESOLUTION;
        gateLength = DEFAULT_GATE_LENGTH;
    }

    // The sequencer is monophonic. When several cells in a column are on,
    // the lowest pitch is the one that plays. Returns -1 for a rest.
    int lowestNote(int step) const {
        for (int n = 0; n < NUM_NOTES; n++) {
            if (cells[step][n])
                return n;
        }
        return -1;
    }
};

} // namespace gridseq
