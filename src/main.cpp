#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

// ---------- settings ----------

static bool assistEnabled() {
    return Mod::get()->getSettingValue<bool>("enabled");
}

static int assistWindow() {
    return static_cast<int>(Mod::get()->getSettingValue<int64_t>("window"));
}

static int lookaheadTicks() {
    return static_cast<int>(Mod::get()->getSettingValue<int64_t>("lookahead"));
}

// ---------- state ----------

struct AssistState {
    int tick = 0;                 // real physics ticks completed this attempt

    // Real input
    bool pendingPress = false;    // a real press is waiting to be predicted
    int realPressTick = -1;       // tick the real press arrived
    int releaseHold = -1;         // hold length of the real press, once released
    int lastHold = 6;             // last observed hold length, used to guess the next one

    // Scheduled (possibly delayed) press/release in the real run
    int delay = 0;
    int fireTick = -1;            // tick to inject the press
    int pressFireTick = -1;       // tick the press was actually injected
    int releaseTick = -1;         // tick to inject the release

    // Prediction
    bool simulating = false;      // true while running off-screen trial ticks
    bool simDied = false;
    bool simDone = false;         // level completed during a trial
    bool injecting = false;       // true while we call handleButton ourselves

    void clearPress() {
        pendingPress = false;
        realPressTick = -1;
        releaseHold = -1;
        delay = 0;
        fireTick = -1;
        pressFireTick = -1;
        releaseTick = -1;
    }

    void reset() {
        clearPress();
        tick = 0;
        simulating = false;
        simDied = false;
        simDone = false;
        injecting = false;
    }
};

static AssistState s;

static void injectButton(GJBaseGameLayer* layer, bool down) {
    s.injecting = true;
    layer->handleButton(down, 1, true);
    s.injecting = false;
}

// ---------- input + tick hooks ----------

class $modify(AssistGameLayer, GJBaseGameLayer) {
    bool isActivePlayLayer() {
        auto pl = PlayLayer::get();
        return pl && static_cast<GJBaseGameLayer*>(pl) == this;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (s.injecting || s.simulating || !assistEnabled() || !isActivePlayLayer()
            || button != 1 || !isPlayer1) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        if (down) {
            // Hold the real press back; the next tick predicts when to fire it.
            // A new press replaces anything still scheduled from an older one.
            if (s.pressFireTick >= 0 || s.releaseTick >= 0) {
                // Older press is still held in-game: release it first.
                injectButton(this, false);
            }
            s.clearPress();
            s.pendingPress = true;
            s.realPressTick = s.tick;
            return;
        }

        // Release of a press we never tracked: pass it through.
        if (s.realPressTick < 0) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        int hold = std::max(1, s.tick - s.realPressTick);
        s.lastHold = std::clamp(hold, 1, lookaheadTicks());

        if (s.pressFireTick >= 0) {
            // Press already fired: keep the same hold length, shifted by the delay.
            int rt = s.pressFireTick + hold;
            if (rt <= s.tick) {
                s.clearPress();
                return GJBaseGameLayer::handleButton(down, button, isPlayer1);
            }
            s.releaseTick = rt;
            return;
        }

        // Press hasn't fired yet: release `hold` ticks after it does.
        s.releaseHold = hold;
    }

    // Runs trial timelines off-screen and returns the first delay that survives.
    // Nothing is drawn between these ticks, so the player never sees the trials.
    int predictDelay(PlayLayer* pl, float dt) {
        Ref<CheckpointObject> snap = pl->createCheckpoint();
        if (!snap) return 0;

        int window = assistWindow();
        int look = lookaheadTicks();
        int hold = s.releaseHold > 0 ? s.releaseHold : s.lastHold;

        int chosen = 0;
        s.simulating = true;

        for (int d = 0; d <= window; ++d) {
            s.simDied = false;
            s.simDone = false;
            bool held = false;

            for (int t = 0; t < d + look && !s.simDied && !s.simDone; ++t) {
                if (t == d) { injectButton(this, true); held = true; }
                if (held && t == d + hold) { injectButton(this, false); held = false; }
                GJBaseGameLayer::processCommands(dt, false, true);
            }

            if (held) injectButton(this, false);
            pl->loadFromCheckpoint(snap);

            if (!s.simDied) {
                chosen = d;
                break;
            }
        }

        s.simulating = false;
        s.simDied = false;
        s.simDone = false;
        // If every timing dies, fire on time and let the death happen normally.
        return chosen;
    }

    // Called once per physics tick (240 per second).
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (s.simulating || !assistEnabled() || !isActivePlayLayer()) {
            return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        }

        auto pl = PlayLayer::get();

        if (s.pendingPress) {
            s.pendingPress = false;
            s.delay = predictDelay(pl, dt);
            s.fireTick = s.tick + s.delay;
        }

        if (s.fireTick >= 0 && s.tick >= s.fireTick) {
            s.fireTick = -1;
            injectButton(this, true);
            s.pressFireTick = s.tick;
            if (s.releaseHold > 0) s.releaseTick = s.tick + s.releaseHold;
        }

        if (s.fireTick < 0 && s.releaseTick >= 0 && s.tick >= s.releaseTick) {
            injectButton(this, false);
            s.clearPress();
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        s.tick++;
    }
};

// ---------- death, completion, label and safe mode ----------

class $modify(AssistPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* label = nullptr;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        s.reset();
        if (assistEnabled()) m_isTestMode = true;

        auto label = CCLabelBMFont::create("", "bigFont.fnt");
        label->setScale(0.35f);
        label->setOpacity(180);
        label->setAnchorPoint({1.f, 0.f});
        auto win = CCDirector::get()->getWinSize();
        label->setPosition({win.width - 5.f, 5.f});
        m_uiLayer->addChild(label, 1000);
        m_fields->label = label;

        updateAssistLabel();
        return true;
    }

    void updateAssistLabel() {
        auto label = m_fields->label;
        if (!label) return;
        bool on = assistEnabled();
        label->setVisible(on);
        if (on) {
            label->setString(fmt::format("CA {}t", assistWindow()).c_str());
        }
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        s.reset();
        // Safe mode: test mode stops progress and completions from saving.
        if (assistEnabled()) m_isTestMode = true;
        updateAssistLabel();
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        if (s.simulating) {
            // Death inside a trial timeline: record it, don't actually die.
            s.simDied = true;
            return;
        }
        // Real deaths always go through. The assist only picks click timing.
        PlayLayer::destroyPlayer(player, obj);
    }

    void levelComplete() {
        if (s.simulating) {
            // Reaching the end during a trial counts as surviving.
            s.simDone = true;
            return;
        }
        PlayLayer::levelComplete();
    }
};
