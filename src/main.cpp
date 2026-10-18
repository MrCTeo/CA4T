// Click Assist (Practice) - Geode mod for Geometry Dash
//
// How it works:
//  1. When you press jump, the mod saves a snapshot of the game state
//     (the same kind of checkpoint practice mode uses) right before the press.
//  2. If you die within the "lookahead" window after that press, the death is
//     skipped and the game is rolled back to the snapshot.
//  3. The press is then replayed 1 tick later. If that still dies, it tries
//     2 ticks later, and so on, up to the "window" setting.
//  4. If every delay up to the window still dies, you die normally.
//
// Only early clicks can be fixed, since the press can only be moved later.
// Only player 1's jump button is handled.

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
    int tick = 0;                         // our own physics tick counter

    Ref<CheckpointObject> snapshot = nullptr; // game state right before the press
    int pressTick = -1;                   // tick of the original press
    int holdTicks = -1;                   // how long the press was held (-1 = still held)
    int delay = 0;                        // delay currently being tried

    int fireTick = -1;                    // tick to inject the delayed press
    int releaseTick = -1;                 // tick to inject the release
    bool rollbackRequested = false;
    bool injecting = false;               // true while we call handleButton ourselves

    void clearPress() {
        snapshot = nullptr;
        pressTick = -1;
        holdTicks = -1;
        delay = 0;
        fireTick = -1;
        releaseTick = -1;
        rollbackRequested = false;
    }

    void reset() {
        clearPress();
        tick = 0;
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
        if (s.injecting || !assistEnabled() || !isActivePlayLayer() || button != 1 || !isPlayer1) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        auto pl = PlayLayer::get();

        if (down) {
            // A new real press replaces any assist still in progress.
            s.clearPress();
            s.snapshot = pl->createCheckpoint();
            s.pressTick = s.tick;
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        // Release
        if (s.pressTick >= 0 && s.holdTicks < 0) {
            if (s.fireTick >= 0) {
                // The delayed press hasn't fired yet: release right after it does.
                s.holdTicks = 1;
                s.releaseTick = s.fireTick + 1;
                return; // swallow the real release for now
            }
            s.holdTicks = std::max(1, s.tick - (s.pressTick + s.delay));
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    // Called once per physics tick (240 per second).
    void processCommands(float dt) {
        if (!assistEnabled() || !isActivePlayLayer()) {
            return GJBaseGameLayer::processCommands(dt);
        }

        auto pl = PlayLayer::get();

        // Do the rollback here rather than inside destroyPlayer,
        // because restoring state in the middle of collision checks is unsafe.
        if (s.rollbackRequested && s.snapshot) {
            s.rollbackRequested = false;
            pl->loadFromCheckpoint(s.snapshot);
            s.tick = s.pressTick;
            s.fireTick = s.pressTick + s.delay;
            s.releaseTick = s.holdTicks >= 0 ? s.fireTick + s.holdTicks : -1;
        }

        if (s.fireTick >= 0 && s.tick >= s.fireTick) {
            s.fireTick = -1;
            injectButton(this, true);
        }
        if (s.fireTick < 0 && s.releaseTick >= 0 && s.tick >= s.releaseTick) {
            s.releaseTick = -1;
            injectButton(this, false);
        }

        GJBaseGameLayer::processCommands(dt);
        s.tick++;

        // Survived long enough after the press: stop tracking it.
        if (s.pressTick >= 0 && !s.rollbackRequested
            && s.tick - (s.pressTick + s.delay) > lookaheadTicks()) {
            s.clearPress();
        }
    }
};

// ---------- death, label and safe mode ----------

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
        // m_anticheatSpike is a fake death GD triggers on purpose; always let it through.
        if (!assistEnabled() || obj == m_anticheatSpike || !s.snapshot
            || s.pressTick < 0 || s.delay >= assistWindow()) {
            return PlayLayer::destroyPlayer(player, obj);
        }

        if (!s.rollbackRequested) {
            s.delay++;
            s.rollbackRequested = true;
        }
        // Skip this death; the rollback happens at the start of the next tick.
    }
};
