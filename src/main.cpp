#include <Geode/Geode.hpp>
#include <chrono>
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

    // Runs one off-screen trial with the press delayed by `d` ticks.
    // Returns the tick the player died on, or -1 if it survived `maxTicks`.
    // Always restores the snapshot afterwards.
    int runTrial(PlayLayer* pl, CheckpointObject* snap, float dt, int d, int hold,
                 int maxTicks, std::chrono::steady_clock::time_point deadline, bool& outOfTime) {
        s.simDied = false;
        s.simDone = false;
        bool held = false;
        int deathTick = -1;

        for (int t = 0; t < maxTicks; ++t) {
            if (t == d) { injectButton(this, true); held = true; }
            if (held && t == d + hold) { injectButton(this, false); held = false; }
            // isLastTick = false: skips per-frame visual work the trial doesn't need.
            GJBaseGameLayer::processCommands(dt, false, false);
            if (s.simDied) { deathTick = t; break; }
            if (s.simDone) break;
            if ((t & 7) == 7 && std::chrono::steady_clock::now() > deadline) {
                outOfTime = true;
                break;
            }
        }

        if (held) injectButton(this, false);
        pl->loadFromCheckpoint(snap);
        return deathTick;
    }

    // Picks when to actually send the press.
    //
    // Tries the click on tick 0, 1, 2, ... up to the window. For each one it simulates
    // `lookahead` ticks after the click. The first tick where the click doesn't kill
    // you is where the real click gets sent.
    // If every tick dies, it sends the click on whichever tick survived longest.
    // Nothing is drawn between trials, so none of this is visible.
    int predictDelay(PlayLayer* pl, float dt) {
        Ref<CheckpointObject> snap = pl->createCheckpoint();
        if (!snap) return 0;

        int window = assistWindow();
        int look = lookaheadTicks();
        int hold = s.releaseHold > 0 ? s.releaseHold : s.lastHold;

        auto startTime = std::chrono::steady_clock::now();
        auto deadline = startTime + std::chrono::milliseconds(8);
        bool outOfTime = false;

        int chosen = -1;
        int bestDelay = 0;
        int bestDeath = -1;
        int tried = 0;

        s.simulating = true;

        for (int d = 0; d <= window; ++d) {
            ++tried;
            int death = runTrial(pl, snap, dt, d, hold, d + look, deadline, outOfTime);
            if (outOfTime) break;

            if (death < 0) {           // this tick is safe
                chosen = d;
                break;
            }
            if (death > bestDeath) {   // remember the one that lived longest
                bestDeath = death;
                bestDelay = d;
            }
        }

        s.simulating = false;
        s.simDied = false;
        s.simDone = false;

        bool foundSafe = chosen >= 0;
        if (!foundSafe) chosen = bestDelay;

        auto ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - startTime).count();
        log::info("CA predict: sendOnTick=+{} trials={} foundSafe={} outOfTime={} time={:.2f}ms",
                  chosen, tried, foundSafe, outOfTime, ms);

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
