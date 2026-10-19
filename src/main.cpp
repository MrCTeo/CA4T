#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <chrono>
#include <deque>

using namespace geode::prelude;

// =====================================================================
// How it works
//
// When you click, an invisible copy of your player (the "tracer") is sent
// forward along the trajectory that click would give you. If it survives the
// safety check, the click goes through immediately and untouched.
// If it dies, the click is held, and on each following real tick the tracer
// checks again. The click is sent on the first tick where it doesn't die
// (or at the end of the search window if none is safe).
//
// The real game is never rewound or copied, so there are no snapshots,
// no wave trail resets and no physics changes. The only thing that ever
// changes is the tick your click is sent on.
// =====================================================================

// ---------- settings ----------

static bool assistEnabled() { return Mod::get()->getSettingValue<bool>("enabled"); }
static int  assistWindow()  { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("window")); }
static int  lookaheadTicks(){ return static_cast<int>(Mod::get()->getSettingValue<int64_t>("lookahead")); }
static bool debugPanel()    { return Mod::get()->getSettingValue<bool>("debug"); }

// ---------- state ----------

struct AssistState {
    int tick = 0;                        // real physics ticks this attempt

    // Tracer (invisible fake player)
    PlayerObject* fake = nullptr;
    PlayLayer* owner = nullptr;          // which PlayLayer the tracer belongs to
    bool tracing = false;
    bool traceDied = false;
    bool traceDone = false;
    float playerDt = 0.f;                // dt the game gives PlayerObject::update
    float collideDt = 0.f;               // dt the game gives checkCollisions

    // Click handling
    bool waiting = false;                // a click is being held for a safe tick
    int waited = 0;                      // ticks waited so far
    int firstDeath = -1;                 // where the on-time click would have died
    int realPressTick = -1;
    int releaseHold = -1;                // known hold length if released while waiting
    int lastHold = 6;                    // last hold length, used to guess the next one
    bool pressDown = false;              // our delayed press is down in-game
    int pressFireTick = -1;
    int releaseTick = -1;
    bool injecting = false;
    double clickMs = 0.0;                // tracer time spent on the current click

    // Debug panel
    CCLabelBMFont* debugLabel = nullptr;
    std::deque<std::string> lines;

    void clearPress() {
        waiting = false;
        waited = 0;
        firstDeath = -1;
        realPressTick = -1;
        releaseHold = -1;
        pressDown = false;
        pressFireTick = -1;
        releaseTick = -1;
        clickMs = 0.0;
    }

    void reset() {
        clearPress();
        tick = 0;
        tracing = false;
        traceDied = false;
        traceDone = false;
        injecting = false;
        lines.clear();
    }
};

static AssistState s;

// ---------- helpers ----------

static void debugLine(std::string const& line) {
    s.lines.push_back(line);
    while (s.lines.size() > 6) s.lines.pop_front();
    if (!s.debugLabel) return;
    std::string text;
    for (auto& l : s.lines) { text += l; text += "\n"; }
    s.debugLabel->setString(text.c_str());
}

static void injectButton(GJBaseGameLayer* layer, bool down) {
    s.injecting = true;
    layer->handleButton(down, 1, true);
    s.injecting = false;
}

static bool ensureTracer(PlayLayer* pl) {
    if (s.fake && s.owner == pl) return true;

    // Hidden layer: the tracer and anything it spawns (particles, trails)
    // live in here and are never drawn.
    auto hidden = CCLayer::create();
    hidden->setVisible(false);
    pl->addChild(hidden, -1000);

    auto fake = PlayerObject::create(1, 1, pl, hidden, false);
    if (!fake) return false;
    fake->setVisible(false);
    hidden->addChild(fake);

    s.fake = fake;
    s.owner = pl;
    return true;
}

// Sends the tracer along the trajectory of "click now, hold for holdTicks".
// Returns the tick it died on, or -1 if it survived the safety check.
static int traceClick(PlayLayer* pl, int holdTicks) {
    if (s.playerDt <= 0.f || s.collideDt <= 0.f) return -1;
    if (!ensureTracer(pl)) return -1;

    auto start = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::microseconds(1500);

    s.fake->copyAttributes(pl->m_player1);

    s.tracing = true;
    s.traceDied = false;
    s.traceDone = false;

    s.fake->pushButton(PlayerButton::Jump);
    bool held = true;
    int death = -1;
    int look = lookaheadTicks();

    for (int t = 0; t < look; ++t) {
        if (held && t >= holdTicks) {
            s.fake->releaseButton(PlayerButton::Jump);
            held = false;
        }
        s.fake->update(s.playerDt);
        pl->checkCollisions(s.fake, s.collideDt, false);

        if (s.traceDied) { death = t; break; }
        if (s.traceDone) break;
        if ((t & 3) == 3 && std::chrono::steady_clock::now() > deadline) break; // treat as safe
    }

    if (held) s.fake->releaseButton(PlayerButton::Jump);
    s.tracing = false;
    s.traceDied = false;
    s.traceDone = false;

    s.clickMs += std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    return death;
}

// ---------- game layer hooks ----------

class $modify(AssistGameLayer, GJBaseGameLayer) {
    bool isActivePlayLayer() {
        auto pl = PlayLayer::get();
        return pl && static_cast<GJBaseGameLayer*>(pl) == this;
    }

    void firePress() {
        injectButton(this, true);
        s.waiting = false;
        s.pressDown = true;
        s.pressFireTick = s.tick;
        if (s.releaseHold > 0) s.releaseTick = s.tick + s.releaseHold;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (s.injecting || s.tracing || !assistEnabled() || !isActivePlayLayer()
            || button != 1 || !isPlayer1) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        auto pl = PlayLayer::get();
        if (pl->m_player1->m_isPlatformer) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        if (down) {
            if (s.waiting) return;                        // already holding a click
            if (s.pressDown) { injectButton(this, false); s.clearPress(); }

            s.clickMs = 0.0;
            s.realPressTick = s.tick;
            int death = traceClick(pl, s.lastHold);

            if (death < 0) {
                // Safe right now: the click goes through untouched.
                debugLine(fmt::format("+0 safe  ({:.2f}ms)", s.clickMs));
                return GJBaseGameLayer::handleButton(down, button, isPlayer1);
            }

            // Not safe: hold it and keep checking on the next ticks.
            s.waiting = true;
            s.waited = 0;
            s.firstDeath = death;
            return;
        }

        // ----- release -----
        int hold = s.realPressTick >= 0 ? std::max(1, s.tick - s.realPressTick) : 1;

        if (s.waiting) {
            // Click hasn't been sent yet: release it `hold` ticks after it is.
            s.releaseHold = hold;
            s.lastHold = std::clamp(hold, 1, lookaheadTicks());
            return;
        }

        if (s.pressDown) {
            // Delayed click: keep your hold length, shifted by the delay.
            s.lastHold = std::clamp(hold, 1, lookaheadTicks());
            int rt = s.pressFireTick + hold;
            if (rt <= s.tick) {
                s.clearPress();
                return GJBaseGameLayer::handleButton(down, button, isPlayer1);
            }
            s.releaseTick = rt;
            return;
        }

        // Normal click that went through on time.
        if (s.realPressTick >= 0) {
            s.lastHold = std::clamp(hold, 1, lookaheadTicks());
            s.realPressTick = -1;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    // Called once per physics tick (240 per second).
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (s.tracing || !assistEnabled() || !isActivePlayLayer()) {
            return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        }

        auto pl = PlayLayer::get();

        if (s.waiting) {
            s.waited++;
            int hold = s.releaseHold > 0 ? s.releaseHold : s.lastHold;
            int death = traceClick(pl, hold);

            if (death < 0) {
                debugLine(fmt::format("+0 dies@{} -> sent +{}  ({:.2f}ms)",
                                      s.firstDeath, s.waited, s.clickMs));
                firePress();
            }
            else if (s.waited >= assistWindow()) {
                debugLine(fmt::format("no safe tick -> sent +{}  ({:.2f}ms)",
                                      s.waited, s.clickMs));
                firePress();
            }
        }

        if (s.pressDown && s.releaseTick >= 0 && s.tick >= s.releaseTick) {
            injectButton(this, false);
            s.clearPress();
        }

        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        s.tick++;
    }

    // Records the dt the game really uses, so the tracer moves exactly like you.
    int checkCollisions(PlayerObject* player, float dt, bool ignoreDamage) {
        if (!s.tracing && isActivePlayLayer() && player == m_player1) s.collideDt = dt;
        return GJBaseGameLayer::checkCollisions(player, dt, ignoreDamage);
    }

    // The tracer only collides with blocks, spikes and slopes. It can't use orbs,
    // pads, portals, coins or triggers, so it never changes anything in the level.
    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects,
                               int count, float dt) {
        if (!s.tracing || player != s.fake || !objects) {
            return GJBaseGameLayer::collisionCheckObjects(player, objects, count, dt);
        }

        static gd::vector<GameObject*> kept;
        kept.clear();
        int n = std::min(count, static_cast<int>(objects->size()));
        for (int i = 0; i < n; ++i) {
            auto obj = (*objects)[i];
            if (!obj) continue;
            switch (obj->m_objectType) {
                case GameObjectType::Solid:
                case GameObjectType::Hazard:
                case GameObjectType::AnimatedHazard:
                case GameObjectType::Slope:
                    kept.push_back(obj);
                    break;
                default:
                    break;
            }
        }
        GJBaseGameLayer::collisionCheckObjects(player, &kept, static_cast<int>(kept.size()), dt);
    }
};

// ---------- player hook (records the real update dt) ----------

class $modify(AssistPlayer, PlayerObject) {
    void update(float dt) {
        if (!s.tracing) {
            auto pl = PlayLayer::get();
            if (pl && pl->m_player1 == this) s.playerDt = dt;
        }
        PlayerObject::update(dt);
    }
};

// ---------- play layer: tracer deaths, label, debug panel, safe mode ----------

class $modify(AssistPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* label = nullptr;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        s.reset();
        s.fake = nullptr;
        s.owner = nullptr;
        s.playerDt = 0.f;
        s.collideDt = 0.f;
        s.debugLabel = nullptr;

        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        if (assistEnabled()) m_isTestMode = true;

        auto win = CCDirector::get()->getWinSize();

        auto label = CCLabelBMFont::create("", "bigFont.fnt");
        label->setScale(0.35f);
        label->setOpacity(180);
        label->setAnchorPoint({1.f, 0.f});
        label->setPosition({win.width - 5.f, 5.f});
        m_uiLayer->addChild(label, 1000);
        m_fields->label = label;

        auto dbg = CCLabelBMFont::create("", "chatFont.fnt");
        dbg->setScale(0.5f);
        dbg->setAnchorPoint({0.f, 1.f});
        dbg->setAlignment(kCCTextAlignmentLeft);
        dbg->setPosition({5.f, win.height - 5.f});
        m_uiLayer->addChild(dbg, 1000);
        s.debugLabel = dbg;

        updateAssistLabel();
        return true;
    }

    void updateAssistLabel() {
        bool on = assistEnabled();
        if (auto label = m_fields->label) {
            label->setVisible(on);
            if (on) label->setString(fmt::format("CA {}t", assistWindow()).c_str());
        }
        if (s.debugLabel) s.debugLabel->setVisible(on && debugPanel());
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        s.reset();
        // Safe mode: test mode stops progress and completions from saving.
        if (assistEnabled()) m_isTestMode = true;
        if (s.debugLabel) s.debugLabel->setString("");
        updateAssistLabel();
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        if (s.tracing) {
            // The tracer hit something: record it, nothing actually dies.
            if (player == s.fake) s.traceDied = true;
            return;
        }
        // Real deaths always go through.
        PlayLayer::destroyPlayer(player, obj);
    }

    void levelComplete() {
        if (s.tracing) { s.traceDone = true; return; }
        PlayLayer::levelComplete();
    }
};
