#include "plugin.hpp"
#include "Pattern.hpp"
#include "Engine.hpp"
#include <atomic>


using namespace gridseq;


struct GridSeq : Module {
    enum ParamId {
        STEPS_PARAM,
        RESOLUTION_PARAM,
        GATE_PARAM,
        SHUFFLE_PARAM,
        OCTAVE_PARAM,
        RUN_PARAM,
        ENUMS(BANK_PARAM, NUM_BANKS),
        ENUMS(PATTERN_PARAM, PATTERNS_PER_BANK),
        NUM_PARAMS
    };
    enum InputId {
        CLOCK_INPUT,
        RESET_INPUT,
        RUN_INPUT,
        PATTERN_CV_INPUT,
        BANK_CV_INPUT,
        NUM_INPUTS
    };
    enum OutputId {
        NOTE_OUTPUT,
        GATE_OUTPUT,
        VELOCITY_OUTPUT,
        CURVE_OUTPUT,
        NUM_OUTPUTS
    };
    enum LightId {
        RUN_LIGHT,
        ENUMS(BANK_LIGHT, NUM_BANKS),
        ENUMS(PATTERN_LIGHT, PATTERNS_PER_BANK),
        NUM_LIGHTS
    };

    enum CurveRange {
        CURVE_0_10,
        CURVE_BIPOLAR_5,
        CURVE_0_1,
        NUM_CURVE_RANGES
    };

    // Multi-cell pattern edits requested by the UI, carried out in process()
    // so the audio thread never reads a half-written pattern.
    enum Action {
        ACTION_NONE,
        ACTION_COPY,
        ACTION_PASTE,
        ACTION_CLEAR,
        ACTION_RANDOM
    };

    Pattern patterns[NUM_PATTERNS];
    Pattern clipboard;
    bool clipboardFull = false;
    gridseq::Engine engine;

    bool running = true;
    int curveRange = CURVE_0_10;
    std::atomic<int> pendingAction{ACTION_NONE};

    // Pattern whose settings the knobs currently show. -1 forces a resync.
    int syncedPattern = -1;
    int lastBankCv = -1;
    int lastPatternCv = -1;

    dsp::SchmittTrigger clockTrigger;
    dsp::SchmittTrigger resetTrigger;
    dsp::SchmittTrigger runTrigger;
    dsp::BooleanTrigger runButton;
    dsp::BooleanTrigger bankButtons[NUM_BANKS];
    dsp::BooleanTrigger patternButtons[PATTERNS_PER_BANK];
    dsp::ClockDivider lightDivider;

    GridSeq() {
        config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
        configParam(STEPS_PARAM, 1.f, MAX_STEPS, DEFAULT_LENGTH, "Pattern length", " steps")->snapEnabled = true;
        configSwitch(RESOLUTION_PARAM, 0.f, NUM_RESOLUTIONS - 1, DEFAULT_RESOLUTION, "Resolution",
            {"Clock /4", "Clock /3", "Clock /2", "Clock x1", "Clock x2", "Clock x3", "Clock x4"});
        ParamQuantity* gateQ = configParam(GATE_PARAM, MIN_GATE_LENGTH, 1.f, DEFAULT_GATE_LENGTH, "Gate length", "%", 0.f, 100.f);
        gateQ->description = "100% ties notes together";
        configParam(SHUFFLE_PARAM, 0.f, 1.f, 0.f, "Shuffle", "%", 0.f, 100.f);
        configSwitch(OCTAVE_PARAM, 0.f, NUM_OCTAVE_VIEWS - 1, 1.f, "Octave view", {"C2 to B4", "C3 to B5", "C4 to B6"});
        configButton(RUN_PARAM, "Run");
        for (int i = 0; i < NUM_BANKS; i++)
            configButton(BANK_PARAM + i, string::f("Bank %c", 'A' + i));
        for (int i = 0; i < PATTERNS_PER_BANK; i++)
            configButton(PATTERN_PARAM + i, string::f("Pattern %d", i + 1));

        configInput(CLOCK_INPUT, "Clock");
        configInput(RESET_INPUT, "Reset");
        configInput(RUN_INPUT, "Run toggle");
        configInput(PATTERN_CV_INPUT, "Pattern select (0 V to 10 V)");
        configInput(BANK_CV_INPUT, "Bank select (0 V to 10 V)");

        configOutput(NOTE_OUTPUT, "Note (1 V/oct)");
        configOutput(GATE_OUTPUT, "Gate");
        configOutput(VELOCITY_OUTPUT, "Velocity");
        configOutput(CURVE_OUTPUT, "Curve");

        lightDivider.setDivision(32);
    }

    void onReset(const ResetEvent& e) override {
        Module::onReset(e);
        for (int i = 0; i < NUM_PATTERNS; i++)
            patterns[i].reset();
        engine.resetAll();
        running = true;
        curveRange = CURVE_0_10;
        syncedPattern = -1;
    }

    // Randomize only fills the current pattern. Params are left alone on
    // purpose, so the base class is not called.
    void onRandomize(const RandomizeEvent& e) override {
        randomFill(patterns[engine.current]);
    }

    int octaveView() {
        return clamp((int) std::round(params[OCTAVE_PARAM].getValue()), 0, NUM_OCTAVE_VIEWS - 1);
    }

    void randomFill(Pattern& p) {
        int base = octaveView() * 12;
        p.clearSteps();
        for (int s = 0; s < MAX_STEPS; s++) {
            if (random::uniform() < 0.6f) {
                int note = base + std::min((int) (random::uniform() * VISIBLE_NOTES), VISIBLE_NOTES - 1);
                p.cells[s][note] = 1;
            }
            p.velocity[s] = 0.4f + 0.6f * random::uniform();
            p.curve[s] = random::uniform();
        }
    }

    void requestAction(Action action) {
        pendingAction.store(action);
    }

    void handleAction() {
        int action = pendingAction.exchange(ACTION_NONE);
        Pattern& p = patterns[engine.current];
        switch (action) {
            case ACTION_COPY:
                clipboard = p;
                clipboardFull = true;
                break;
            case ACTION_PASTE:
                if (clipboardFull) {
                    p = clipboard;
                    syncedPattern = -1;
                }
                break;
            case ACTION_CLEAR:
                p.clearSteps();
                break;
            case ACTION_RANDOM:
                randomFill(p);
                break;
            default:
                break;
        }
    }

    // The length, resolution and gate knobs belong to whichever pattern is
    // playing. On a pattern change they take the new pattern's values;
    // otherwise the knobs are written into the pattern.
    void syncPatternSettings() {
        Pattern& p = patterns[engine.current];
        if (syncedPattern != engine.current) {
            params[STEPS_PARAM].setValue(p.length);
            params[RESOLUTION_PARAM].setValue(p.resolution);
            params[GATE_PARAM].setValue(p.gateLength);
            syncedPattern = engine.current;
            return;
        }
        p.length = clamp((int) std::round(params[STEPS_PARAM].getValue()), 1, MAX_STEPS);
        p.resolution = clamp((int) std::round(params[RESOLUTION_PARAM].getValue()), 0, NUM_RESOLUTIONS - 1);
        p.gateLength = clamp(params[GATE_PARAM].getValue(), MIN_GATE_LENGTH, 1.f);
    }

    static int cvToIndex(float volts, int count) {
        return clamp((int) (volts / 10.f * count), 0, count - 1);
    }

    void selectBank(int bank) {
        engine.requestPattern(bank * PATTERNS_PER_BANK + engine.target() % PATTERNS_PER_BANK, running);
    }

    void selectPattern(int pattern) {
        engine.requestPattern(engine.target() / PATTERNS_PER_BANK * PATTERNS_PER_BANK + pattern, running);
    }

    // CV only selects when its value moves to a new slot, so the buttons
    // still work while a cable is patched.
    void handlePatternSelect() {
        for (int i = 0; i < NUM_BANKS; i++) {
            if (bankButtons[i].process(params[BANK_PARAM + i].getValue() > 0.f))
                selectBank(i);
        }
        for (int i = 0; i < PATTERNS_PER_BANK; i++) {
            if (patternButtons[i].process(params[PATTERN_PARAM + i].getValue() > 0.f))
                selectPattern(i);
        }

        if (inputs[BANK_CV_INPUT].isConnected()) {
            int bank = cvToIndex(inputs[BANK_CV_INPUT].getVoltage(), NUM_BANKS);
            if (bank != lastBankCv)
                selectBank(bank);
            lastBankCv = bank;
        }
        else {
            lastBankCv = -1;
        }

        if (inputs[PATTERN_CV_INPUT].isConnected()) {
            int pattern = cvToIndex(inputs[PATTERN_CV_INPUT].getVoltage(), PATTERNS_PER_BANK);
            if (pattern != lastPatternCv)
                selectPattern(pattern);
            lastPatternCv = pattern;
        }
        else {
            lastPatternCv = -1;
        }
    }

    float curveVolts(float value) {
        switch (curveRange) {
            case CURVE_BIPOLAR_5: return value * 10.f - 5.f;
            case CURVE_0_1: return value;
            default: return value * 10.f;
        }
    }

    void updateLights() {
        lights[RUN_LIGHT].setBrightness(running ? 1.f : 0.f);
        int current = engine.current;
        int pending = engine.pending;
        for (int i = 0; i < NUM_BANKS; i++) {
            float b = 0.f;
            if (current / PATTERNS_PER_BANK == i)
                b = 1.f;
            else if (pending >= 0 && pending / PATTERNS_PER_BANK == i)
                b = 0.25f;
            lights[BANK_LIGHT + i].setBrightness(b);
        }
        for (int i = 0; i < PATTERNS_PER_BANK; i++) {
            float b = 0.f;
            if (current % PATTERNS_PER_BANK == i)
                b = 1.f;
            else if (pending >= 0 && pending % PATTERNS_PER_BANK == i)
                b = 0.25f;
            lights[PATTERN_LIGHT + i].setBrightness(b);
        }
    }

    void process(const ProcessArgs& args) override {
        handleAction();
        syncPatternSettings();

        if (runButton.process(params[RUN_PARAM].getValue() > 0.f))
            running = !running;
        if (runTrigger.process(inputs[RUN_INPUT].getVoltage(), 0.1f, 1.f))
            running = !running;

        handlePatternSelect();

        engine.shuffle = params[SHUFFLE_PARAM].getValue();
        bool clock = clockTrigger.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 1.f);
        bool reset = resetTrigger.process(inputs[RESET_INPUT].getVoltage(), 0.1f, 1.f);
        engine.process(args.sampleTime, patterns, clock, reset, running);

        outputs[NOTE_OUTPUT].setVoltage(engine.noteVolts);
        outputs[GATE_OUTPUT].setVoltage(engine.gate ? 10.f : 0.f);
        outputs[VELOCITY_OUTPUT].setVoltage(engine.velocity * 10.f);
        outputs[CURVE_OUTPUT].setVoltage(curveVolts(engine.curve));

        if (lightDivider.process())
            updateLights();
    }

    static json_t* floatArrayToJson(const float* values, int count) {
        json_t* arrayJ = json_array();
        for (int i = 0; i < count; i++)
            json_array_append_new(arrayJ, json_real(values[i]));
        return arrayJ;
    }

    static void floatArrayFromJson(json_t* arrayJ, float* values, int count) {
        if (!json_is_array(arrayJ))
            return;
        int size = std::min((int) json_array_size(arrayJ), count);
        for (int i = 0; i < size; i++) {
            json_t* valueJ = json_array_get(arrayJ, i);
            if (json_is_number(valueJ))
                values[i] = clamp((float) json_number_value(valueJ), 0.f, 1.f);
        }
    }

    // Notes are saved per step as a list of on cells, which stays small and
    // readable compared to a full 32 x 60 table.
    static json_t* patternToJson(const Pattern& p) {
        json_t* patternJ = json_object();
        json_object_set_new(patternJ, "length", json_integer(p.length));
        json_object_set_new(patternJ, "resolution", json_integer(p.resolution));
        json_object_set_new(patternJ, "gateLength", json_real(p.gateLength));
        json_t* notesJ = json_array();
        for (int s = 0; s < MAX_STEPS; s++) {
            json_t* stepJ = json_array();
            for (int n = 0; n < NUM_NOTES; n++) {
                if (p.cells[s][n])
                    json_array_append_new(stepJ, json_integer(n));
            }
            json_array_append_new(notesJ, stepJ);
        }
        json_object_set_new(patternJ, "notes", notesJ);
        json_object_set_new(patternJ, "velocity", floatArrayToJson(p.velocity, MAX_STEPS));
        json_object_set_new(patternJ, "curve", floatArrayToJson(p.curve, MAX_STEPS));
        return patternJ;
    }

    static void patternFromJson(json_t* patternJ, Pattern& p) {
        p.reset();
        if (!json_is_object(patternJ))
            return;
        json_t* lengthJ = json_object_get(patternJ, "length");
        if (json_is_integer(lengthJ))
            p.length = clamp((int) json_integer_value(lengthJ), 1, MAX_STEPS);
        json_t* resolutionJ = json_object_get(patternJ, "resolution");
        if (json_is_integer(resolutionJ))
            p.resolution = clamp((int) json_integer_value(resolutionJ), 0, NUM_RESOLUTIONS - 1);
        json_t* gateJ = json_object_get(patternJ, "gateLength");
        if (json_is_number(gateJ))
            p.gateLength = clamp((float) json_number_value(gateJ), MIN_GATE_LENGTH, 1.f);

        json_t* notesJ = json_object_get(patternJ, "notes");
        if (json_is_array(notesJ)) {
            int steps = std::min((int) json_array_size(notesJ), MAX_STEPS);
            for (int s = 0; s < steps; s++) {
                json_t* stepJ = json_array_get(notesJ, s);
                if (!json_is_array(stepJ))
                    continue;
                for (size_t i = 0; i < json_array_size(stepJ); i++) {
                    json_t* noteJ = json_array_get(stepJ, i);
                    if (!json_is_integer(noteJ))
                        continue;
                    int n = (int) json_integer_value(noteJ);
                    if (n >= 0 && n < NUM_NOTES)
                        p.cells[s][n] = 1;
                }
            }
        }
        floatArrayFromJson(json_object_get(patternJ, "velocity"), p.velocity, MAX_STEPS);
        floatArrayFromJson(json_object_get(patternJ, "curve"), p.curve, MAX_STEPS);
    }

    json_t* dataToJson() override {
        json_t* rootJ = json_object();
        json_object_set_new(rootJ, "running", json_boolean(running));
        json_object_set_new(rootJ, "bank", json_integer(engine.current / PATTERNS_PER_BANK));
        json_object_set_new(rootJ, "pattern", json_integer(engine.current % PATTERNS_PER_BANK));
        json_object_set_new(rootJ, "curveRange", json_integer(curveRange));
        json_t* patternsJ = json_array();
        for (int i = 0; i < NUM_PATTERNS; i++)
            json_array_append_new(patternsJ, patternToJson(patterns[i]));
        json_object_set_new(rootJ, "patterns", patternsJ);
        return rootJ;
    }

    void dataFromJson(json_t* rootJ) override {
        json_t* runningJ = json_object_get(rootJ, "running");
        if (json_is_boolean(runningJ))
            running = json_boolean_value(runningJ);

        json_t* curveRangeJ = json_object_get(rootJ, "curveRange");
        if (json_is_integer(curveRangeJ))
            curveRange = clamp((int) json_integer_value(curveRangeJ), 0, NUM_CURVE_RANGES - 1);

        json_t* patternsJ = json_object_get(rootJ, "patterns");
        if (json_is_array(patternsJ)) {
            int count = std::min((int) json_array_size(patternsJ), NUM_PATTERNS);
            for (int i = 0; i < count; i++)
                patternFromJson(json_array_get(patternsJ, i), patterns[i]);
        }

        int bank = 0;
        int pattern = 0;
        json_t* bankJ = json_object_get(rootJ, "bank");
        if (json_is_integer(bankJ))
            bank = clamp((int) json_integer_value(bankJ), 0, NUM_BANKS - 1);
        json_t* patternJ = json_object_get(rootJ, "pattern");
        if (json_is_integer(patternJ))
            pattern = clamp((int) json_integer_value(patternJ), 0, PATTERNS_PER_BANK - 1);
        engine.resetAll();
        engine.current = bank * PATTERNS_PER_BANK + pattern;
        syncedPattern = -1;
    }
};


static const NVGcolor COLOR_BACKGROUND = nvgRGB(0x16, 0x18, 0x1b);
static const NVGcolor COLOR_BLACK_KEY_ROW = nvgRGB(0x1d, 0x20, 0x24);
static const NVGcolor COLOR_LINE = nvgRGB(0x2c, 0x30, 0x36);
static const NVGcolor COLOR_BEAT_LINE = nvgRGB(0x44, 0x49, 0x52);
static const NVGcolor COLOR_INACTIVE_STEPS = nvgRGBA(0x00, 0x00, 0x00, 0x80);
static const NVGcolor COLOR_CELL = nvgRGB(0xf0, 0xa0, 0x30);
static const NVGcolor COLOR_CELL_SHADOWED = nvgRGB(0x8a, 0x5c, 0x1c);
static const NVGcolor COLOR_PLAYHEAD = nvgRGBA(0xff, 0xff, 0xff, 0x30);
static const NVGcolor COLOR_VELOCITY = nvgRGB(0x50, 0xc0, 0xe0);
static const NVGcolor COLOR_CURVE = nvgRGB(0x90, 0xe0, 0x70);
static const NVGcolor COLOR_TEXT = nvgRGB(0xc8, 0xcc, 0xd2);
static const NVGcolor COLOR_TEXT_DARK = nvgRGB(0x60, 0x66, 0x70);

static const float KEY_STRIP_MM = 6.f;

// Shown in the module browser, where there is no module to read from.
// Stored note index per step for the default octave view, -1 for a rest.
static const int PREVIEW_NOTES[MAX_STEPS] = {
    24, -1, 31, 27, 24, -1, 36, 34, 22, -1, 29, 27, 22, 26, 34, 29,
    24, -1, 31, 27, 24, -1, 36, 34, 22, -1, 29, 27, 22, 26, 34, 29,
};


static bool isBlackKey(int note) {
    int pc = note % 12;
    return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
}

static float stepX(float gridX, float cellW, int step) {
    return gridX + step * cellW;
}

static void fillRect(NVGcontext* vg, float x, float y, float w, float h, NVGcolor color) {
    nvgBeginPath(vg);
    nvgRect(vg, x, y, w, h);
    nvgFillColor(vg, color);
    nvgFill(vg);
}

static void drawStepLines(NVGcontext* vg, float gridX, float cellW, float h) {
    for (int s = 1; s < MAX_STEPS; s++) {
        float x = stepX(gridX, cellW, s);
        fillRect(vg, x - 0.25f, 0.f, 0.5f, h, (s % 4 == 0) ? COLOR_BEAT_LINE : COLOR_LINE);
    }
}


struct NoteGridWidget : Widget {
    GridSeq* module = nullptr;
    int editPattern = 0;
    bool paintValue = false;
    Vec dragPos;
    int lastStep = -1;
    int lastNote = -1;

    float keyWidth() {
        return mm2px(KEY_STRIP_MM);
    }

    float cellWidth() {
        return (box.size.x - keyWidth()) / MAX_STEPS;
    }

    float cellHeight() {
        return box.size.y / VISIBLE_NOTES;
    }

    int baseNote() {
        return (module ? module->octaveView() : 1) * 12;
    }

    int length() {
        return module ? module->patterns[module->engine.current].length : DEFAULT_LENGTH;
    }

    int lowestNote(int step) {
        if (module)
            return module->patterns[module->engine.current].lowestNote(step);
        return PREVIEW_NOTES[step];
    }

    bool cellOn(int step, int note) {
        if (module)
            return module->patterns[module->engine.current].cells[step][note];
        return PREVIEW_NOTES[step] == note;
    }

    // Row 0 is the top of the grid, which is the highest visible note.
    bool cellAt(Vec pos, int* step, int* note) {
        if (pos.x < keyWidth() || pos.x >= box.size.x || pos.y < 0.f || pos.y >= box.size.y)
            return false;
        *step = clamp((int) ((pos.x - keyWidth()) / cellWidth()), 0, MAX_STEPS - 1);
        int row = clamp((int) (pos.y / cellHeight()), 0, VISIBLE_NOTES - 1);
        *note = baseNote() + VISIBLE_NOTES - 1 - row;
        return true;
    }

    void paint(Vec pos) {
        int step, note;
        if (!cellAt(pos, &step, &note))
            return;
        if (step == lastStep && note == lastNote)
            return;
        lastStep = step;
        lastNote = note;
        module->patterns[editPattern].cells[step][note] = paintValue ? 1 : 0;
    }

    void onButton(const ButtonEvent& e) override {
        if (!module || e.button != GLFW_MOUSE_BUTTON_LEFT || e.action != GLFW_PRESS)
            return;
        int step, note;
        if (!cellAt(e.pos, &step, &note))
            return;
        editPattern = module->engine.current;
        paintValue = !module->patterns[editPattern].cells[step][note];
        lastStep = -1;
        lastNote = -1;
        dragPos = e.pos;
        paint(dragPos);
        e.consume(this);
    }

    void onDragMove(const DragMoveEvent& e) override {
        if (!module || e.button != GLFW_MOUSE_BUTTON_LEFT)
            return;
        dragPos = dragPos.plus(e.mouseDelta.div(getAbsoluteZoom()));
        paint(dragPos);
    }

    void onDragEnd(const DragEndEvent& e) override {
        lastStep = -1;
        lastNote = -1;
    }

    void drawKeys(NVGcontext* vg) {
        float keyW = keyWidth();
        float cellH = cellHeight();
        int base = baseNote();
        std::shared_ptr<window::Font> font = APP->window->uiFont;
        for (int row = 0; row < VISIBLE_NOTES; row++) {
            int note = base + VISIBLE_NOTES - 1 - row;
            float y = row * cellH;
            bool black = isBlackKey(note);
            fillRect(vg, 0.f, y, keyW, cellH, black ? nvgRGB(0x20, 0x22, 0x26) : nvgRGB(0xb8, 0xbc, 0xc2));
            fillRect(vg, keyW, y, box.size.x - keyW, cellH, black ? COLOR_BLACK_KEY_ROW : COLOR_BACKGROUND);
            if (note % 12 == 0) {
                fillRect(vg, 0.f, y + cellH - 0.5f, box.size.x, 0.5f, COLOR_BEAT_LINE);
                if (font && font->handle >= 0) {
                    nvgFontFaceId(vg, font->handle);
                    nvgFontSize(vg, cellH * 1.1f);
                    nvgFillColor(vg, nvgRGB(0x20, 0x22, 0x26));
                    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
                    nvgText(vg, 1.f, y + cellH * 0.5f, string::f("C%d", (note + LOWEST_MIDI_NOTE) / 12 - 1).c_str(), NULL);
                }
            }
        }
    }

    void draw(const DrawArgs& args) override {
        NVGcontext* vg = args.vg;
        drawKeys(vg);
        drawStepLines(vg, keyWidth(), cellWidth(), box.size.y);
        float inactiveX = stepX(keyWidth(), cellWidth(), length());
        if (inactiveX < box.size.x)
            fillRect(vg, inactiveX, 0.f, box.size.x - inactiveX, box.size.y, COLOR_INACTIVE_STEPS);
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1)
            drawLit(args.vg);
        Widget::drawLayer(args, layer);
    }

    void drawLit(NVGcontext* vg) {
        float keyW = keyWidth();
        float cellW = cellWidth();
        float cellH = cellHeight();
        int base = baseNote();
        int len = length();

        if (module && module->engine.step >= 0)
            fillRect(vg, stepX(keyW, cellW, module->engine.step), 0.f, cellW, box.size.y, COLOR_PLAYHEAD);

        for (int s = 0; s < MAX_STEPS; s++) {
            int lowest = lowestNote(s);
            for (int row = 0; row < VISIBLE_NOTES; row++) {
                int note = base + VISIBLE_NOTES - 1 - row;
                if (!cellOn(s, note))
                    continue;
                // Only the lowest note in a column plays, so the others are drawn dimmer.
                NVGcolor color = (note == lowest && s < len) ? COLOR_CELL : COLOR_CELL_SHADOWED;
                fillRect(vg, stepX(keyW, cellW, s) + 0.6f, row * cellH + 0.5f, cellW - 1.2f, cellH - 1.f, color);
            }
        }
    }
};


struct LaneWidget : Widget {
    GridSeq* module = nullptr;
    bool isCurve = false;
    int editPattern = 0;
    Vec dragPos;

    float cellWidth() {
        return box.size.x / MAX_STEPS;
    }

    float value(int step) {
        if (!module)
            return isCurve ? 0.5f + 0.4f * std::sin(step * 0.4f) : (step % 4 == 0 ? 1.f : DEFAULT_VELOCITY);
        const Pattern& p = module->patterns[module->engine.current];
        return isCurve ? p.curve[step] : p.velocity[step];
    }

    void setValue(Vec pos) {
        if (pos.x < 0.f || pos.x >= box.size.x)
            return;
        int step = clamp((int) (pos.x / cellWidth()), 0, MAX_STEPS - 1);
        float v = clamp(1.f - pos.y / box.size.y, 0.f, 1.f);
        Pattern& p = module->patterns[editPattern];
        if (isCurve)
            p.curve[step] = v;
        else
            p.velocity[step] = v;
    }

    void onButton(const ButtonEvent& e) override {
        if (!module || e.button != GLFW_MOUSE_BUTTON_LEFT || e.action != GLFW_PRESS)
            return;
        editPattern = module->engine.current;
        dragPos = e.pos;
        setValue(dragPos);
        e.consume(this);
    }

    void onDragMove(const DragMoveEvent& e) override {
        if (!module || e.button != GLFW_MOUSE_BUTTON_LEFT)
            return;
        dragPos = dragPos.plus(e.mouseDelta.div(getAbsoluteZoom()));
        setValue(dragPos);
    }

    void draw(const DrawArgs& args) override {
        fillRect(args.vg, 0.f, 0.f, box.size.x, box.size.y, COLOR_BACKGROUND);
        fillRect(args.vg, 0.f, box.size.y * 0.5f - 0.25f, box.size.x, 0.5f, COLOR_LINE);
        drawStepLines(args.vg, 0.f, cellWidth(), box.size.y);
    }

    void drawLayer(const DrawArgs& args, int layer) override {
        if (layer == 1)
            drawLit(args.vg);
        Widget::drawLayer(args, layer);
    }

    void drawLit(NVGcontext* vg) {
        float cellW = cellWidth();
        int len = module ? module->patterns[module->engine.current].length : DEFAULT_LENGTH;
        NVGcolor color = isCurve ? COLOR_CURVE : COLOR_VELOCITY;
        for (int s = 0; s < MAX_STEPS; s++) {
            float h = value(s) * box.size.y;
            NVGcolor c = color;
            if (s >= len)
                c = nvgTransRGBAf(color, 0.3f);
            fillRect(vg, s * cellW + 0.6f, box.size.y - h, cellW - 1.2f, h, c);
        }
    }
};


// Panel text is drawn here rather than in the SVG, because Rack's SVG
// renderer does not draw <text> elements.
struct PanelLabels : Widget {
    void label(NVGcontext* vg, int font, float xMm, float yMm, const char* text, float size, NVGcolor color) {
        nvgFontFaceId(vg, font);
        nvgFontSize(vg, size);
        nvgFillColor(vg, color);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgText(vg, mm2px(xMm), mm2px(yMm), text, NULL);
    }

    void draw(const DrawArgs& args) override {
        std::shared_ptr<window::Font> font = APP->window->uiFont;
        if (!font || font->handle < 0)
            return;
        NVGcontext* vg = args.vg;
        int f = font->handle;

        nvgFontFaceId(vg, f);
        nvgFontSize(vg, 13.f);
        nvgFillColor(vg, COLOR_TEXT);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(vg, mm2px(4.f), mm2px(6.6f), "GRIDSEQ", NULL);

        label(vg, f, 7.f, 83.f, "VEL", 7.f, COLOR_TEXT);
        label(vg, f, 7.f, 95.f, "CRV", 7.f, COLOR_TEXT);

        label(vg, f, 26.5f, 106.f, "BANK", 7.f, COLOR_TEXT);
        label(vg, f, 83.5f, 106.f, "PATTERN", 7.f, COLOR_TEXT);
        const char* banks[NUM_BANKS] = {"A", "B", "C", "D"};
        for (int i = 0; i < NUM_BANKS; i++)
            label(vg, f, 13.f + 9.f * i, 119.f, banks[i], 7.f, COLOR_TEXT);
        const char* numbers[PATTERNS_PER_BANK] = {"1", "2", "3", "4", "5", "6", "7", "8"};
        for (int i = 0; i < PATTERNS_PER_BANK; i++)
            label(vg, f, 52.f + 9.f * i, 119.f, numbers[i], 7.f, COLOR_TEXT);

        label(vg, f, 127.f, 16.2f, "STEPS", 7.f, COLOR_TEXT);
        label(vg, f, 143.f, 16.2f, "RES", 7.f, COLOR_TEXT);
        label(vg, f, 127.f, 32.2f, "GATE", 7.f, COLOR_TEXT);
        label(vg, f, 143.f, 32.2f, "SHUF", 7.f, COLOR_TEXT);
        label(vg, f, 127.f, 46.6f, "OCT", 7.f, COLOR_TEXT);
        label(vg, f, 143.f, 46.6f, "RUN", 7.f, COLOR_TEXT);

        const char* ins[] = {"CLK", "RST", "RUN", "PAT", "BANK"};
        for (int i = 0; i < 5; i++)
            label(vg, f, 127.f, 61.f + 12.5f * i, ins[i], 7.f, COLOR_TEXT);
        const char* outs[] = {"NOTE", "GATE", "VEL", "CURVE"};
        for (int i = 0; i < 4; i++)
            label(vg, f, 143.f, 61.f + 12.5f * i, outs[i], 7.f, COLOR_TEXT);
        label(vg, f, 142.6f, 112.6f, "OUT", 6.f, COLOR_TEXT_DARK);
    }
};


struct GridSeqWidget : ModuleWidget {
    GridSeqWidget(GridSeq* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance, "res/GridSeq.svg")));

        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
        addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
        addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

        PanelLabels* labels = createWidget<PanelLabels>(Vec(0, 0));
        labels->box.size = box.size;
        addChild(labels);

        NoteGridWidget* grid = createWidget<NoteGridWidget>(mm2px(Vec(4.0, 13.0)));
        grid->box.size = mm2px(Vec(111.6, 63.0));
        grid->module = module;
        addChild(grid);

        LaneWidget* velocityLane = createWidget<LaneWidget>(mm2px(Vec(10.0, 78.0)));
        velocityLane->box.size = mm2px(Vec(105.6, 10.0));
        velocityLane->module = module;
        addChild(velocityLane);

        LaneWidget* curveLane = createWidget<LaneWidget>(mm2px(Vec(10.0, 90.0)));
        curveLane->box.size = mm2px(Vec(105.6, 10.0));
        curveLane->module = module;
        curveLane->isCurve = true;
        addChild(curveLane);

        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(127.0, 22.0)), module, GridSeq::STEPS_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(143.0, 22.0)), module, GridSeq::RESOLUTION_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(127.0, 38.0)), module, GridSeq::GATE_PARAM));
        addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(143.0, 38.0)), module, GridSeq::SHUFFLE_PARAM));
        addParam(createParamCentered<CKSSThree>(mm2px(Vec(127.0, 53.0)), module, GridSeq::OCTAVE_PARAM));
        addParam(createLightParamCentered<VCVLightBezel<GreenLight>>(mm2px(Vec(143.0, 53.0)), module, GridSeq::RUN_PARAM, GridSeq::RUN_LIGHT));

        for (int i = 0; i < NUM_BANKS; i++)
            addParam(createLightParamCentered<VCVLightBezel<YellowLight>>(mm2px(Vec(13.0 + 9.0 * i, 112.0)), module, GridSeq::BANK_PARAM + i, GridSeq::BANK_LIGHT + i));
        for (int i = 0; i < PATTERNS_PER_BANK; i++)
            addParam(createLightParamCentered<VCVLightBezel<RedLight>>(mm2px(Vec(52.0 + 9.0 * i, 112.0)), module, GridSeq::PATTERN_PARAM + i, GridSeq::PATTERN_LIGHT + i));

        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(127.0, 66.0)), module, GridSeq::CLOCK_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(127.0, 78.5)), module, GridSeq::RESET_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(127.0, 91.0)), module, GridSeq::RUN_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(127.0, 103.5)), module, GridSeq::PATTERN_CV_INPUT));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(127.0, 116.0)), module, GridSeq::BANK_CV_INPUT));

        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(143.0, 66.0)), module, GridSeq::NOTE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(143.0, 78.5)), module, GridSeq::GATE_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(143.0, 91.0)), module, GridSeq::VELOCITY_OUTPUT));
        addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(143.0, 103.5)), module, GridSeq::CURVE_OUTPUT));
    }

    void appendContextMenu(Menu* menu) override {
        GridSeq* module = getModule<GridSeq>();
        if (!module)
            return;

        menu->addChild(new MenuSeparator);
        menu->addChild(createIndexPtrSubmenuItem("Curve range",
            {"0 V to 10 V", "-5 V to 5 V", "0 V to 1 V"}, &module->curveRange));

        menu->addChild(new MenuSeparator);
        menu->addChild(createMenuLabel("Current pattern"));
        menu->addChild(createMenuItem("Copy", "", [=]() {
            module->requestAction(GridSeq::ACTION_COPY);
        }));
        menu->addChild(createMenuItem("Paste", "", [=]() {
            module->requestAction(GridSeq::ACTION_PASTE);
        }, !module->clipboardFull));
        menu->addChild(createMenuItem("Clear", "", [=]() {
            module->requestAction(GridSeq::ACTION_CLEAR);
        }));
        menu->addChild(createMenuItem("Random fill", "", [=]() {
            module->requestAction(GridSeq::ACTION_RANDOM);
        }));
    }
};


Model* modelGridSeq = createModel<GridSeq, GridSeqWidget>("GridSeq");
