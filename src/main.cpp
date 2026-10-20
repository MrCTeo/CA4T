#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <chrono>
#include <deque>
#include <unordered_map>

using namespace geode::prelude;

// Shown in the corner label so you can see which build is running.
static constexpr char const* kBuildTag = "tracer-v13";

// =====================================================================
// How it works
//
// Every input (press AND release) is checked by an invisible tracer: a copy
// of your player that flies ahead along the trajectory that input would give
// you. It tries the input on tick +0, +1, ... up to the search window, and
// the input is sent on the first tick where the tracer doesn't die.
// If no tick in the window is safe, the input is sent on time, unchanged.
//
// Releases are checked too. On wave a release is just as deadly as a press,
// so both directions are covered. The tracer uses orbs, pads and gravity/size
// portals like you would, and puts them back untouched after every check.
//
// The tracer never touches the real game: no snapshots, no rewinds, no wave
// trail resets, no physics changes. Safe inputs go through with zero delay.
// =====================================================================

// ---------- settings ----------

static bool assistEnabled()  { return Mod::get()->getSettingValue<bool>("enabled"); }
static int  assistWindow()   { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("window")); }
static int  releaseWindow()  { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("release-window")); }
static int  lookaheadTicks() { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("lookahead")); }
static int  waveLookahead()  { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("wave-lookahead")); }
static bool waveSpamFix()    { return Mod::get()->getSettingValue<bool>("wave-spam"); }
static int  spamGap()        { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("spam-gap")); }
static int  spamMaxDelay()   { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("spam-max-delay")); }
static bool predictMoving()  { return Mod::get()->getSettingValue<bool>("predict-moving"); }
static bool debugPanel()     { return Mod::get()->getSettingValue<bool>("debug"); }

// ---------- state ----------

struct PendingInput {
    bool down = true;
    int realTick = 0;        // tick you actually pressed/released
    int base = 0;            // earliest tick it may be sent (keeps your hold length)
    int fireTick = -1;       // -1 = not planned yet
    int firstDeath = -1;     // where the on-time input would have died
    bool foundSafe = true;
    bool skipped = false;
    bool spam = false;       // part of fast wave spam    // not checked (gamemode not assisted)
};

struct AssistState {
    int tick = 0;

    // Tracer
    PlayerObject* fake = nullptr;
    PlayLayer* owner = nullptr;
    bool tracing = false;
    bool traceDied = false;
    bool traceDone = false;
    float playerDt = 0.f;
    float collideDt = 0.f;
    // Timestep calibration: how far you really move per tick, and the factor
    // the tracer's timestep needs so it moves the same distance.
    float realDx = 0.f;
    float dtScale = 0.f;                 // 0 = not calibrated yet
    bool frozen = false;                 // tracer doesn't move at any timestep

    // Inputs
    std::deque<PendingInput> queue;
    bool heldInGame = false;   // what the game currently thinks the button is
    int lastRealInputTick = -100000;
    int pressShift = 0;        // how late the last press was sent (applied to its release)
    bool injecting = false;
    double planMs = 0.0;

    // Objects the tracer used (orbs, pads, portals): their activation is put back after each trace
    struct Touched { GameObject* obj; bool activated; };
    std::vector<Touched> touched;
    int diagOrbs = 0;                    // interactables the tracer touched
    int diagFallbackDeaths = 0;          // deaths found by our own hitbox check

    // Moving objects: positions of objects near you are tracked every real
    // tick, so the tracer can move them ahead in time while it predicts.
    struct Track { CCPoint lastPos; CCPoint vel; int lastTick = -1000; };
    std::unordered_map<GameObject*, Track> tracks;
    std::unordered_map<GameObject*, CCPoint> movedForTrace;   // original positions
    int traceAhead = 0;                  // ticks into the future the tracer is at
    int diagMovers = 0;

    // Diagnostics (last trace)
    int diagTicks = 0;
    int diagCollChecks = 0;
    int diagMaxObjs = 0;
    float diagMovedX = 0.f;
    float diagMovedY = 0.f;
    float diagStartDiff = 0.f;
    bool diagTimedOut = false;
    std::string diagReason;

    // Debug panel
    CCLabelBMFont* debugLabel = nullptr;
    std::deque<std::string> lines;

    void reset() {
        tick = 0;
        tracing = false;
        traceDied = false;
        traceDone = false;
        queue.clear();
        heldInGame = false;
        lastRealInputTick = -100000;
        pressShift = 0;
        injecting = false;
        lines.clear();
        dtScale = 0.f;
        frozen = false;
        tracks.clear();
        movedForTrace.clear();
    }
};

static AssistState s;

// ---------- debug panel ----------

static void debugLine(std::string const& line) {
    if (line.rfind("   ", 0) != 0) log::info("[CA] {}", line);
    s.lines.push_back(line);
    while (s.lines.size() > 8) s.lines.pop_front();
    if (!s.debugLabel) return;
    std::string text;
    for (auto& l : s.lines) { text += l; text += "\n"; }
    s.debugLabel->setString(text.c_str());
}

static void debugDiag() {
    if (!s.diagReason.empty()) {
        debugLine("   SKIPPED: " + s.diagReason);
        log::info("[CA]    SKIPPED: {}", s.diagReason);
        return;
    }
    log::info("[CA]    ticks {} moved {:.1f},{:.1f} start-off {:.1f} coll {} objs {} orbs {} movers {} backup {} timeout {} dt {:.5f}/{:.5f}",
              s.diagTicks, s.diagMovedX, s.diagMovedY, s.diagStartDiff,
              s.diagCollChecks, s.diagMaxObjs, s.diagOrbs, s.diagMovers, s.diagFallbackDeaths,
              s.diagTimedOut, s.playerDt, s.collideDt);
    debugLine(fmt::format("   ticks {} moved {:.3f},{:.3f} dt {:.5f}/{:.5f} x{:.2f} coll {} objs {} orbs {} movers {}{}{}",
                          s.diagTicks, s.diagMovedX, s.diagMovedY, s.playerDt, s.collideDt, s.dtScale,
                          s.diagCollChecks, s.diagMaxObjs, s.diagOrbs, s.diagMovers,
                          s.diagFallbackDeaths ? " BACKUP-DEATH" : "",
                          s.diagTimedOut ? " TIMEOUT" : ""));
}

// ---------- object types ----------

// Things that kill or block you.
static bool isSolidOrHazard(GameObjectType t) {
    switch (t) {
        case GameObjectType::Solid:
        case GameObjectType::Hazard:
        case GameObjectType::AnimatedHazard:
        case GameObjectType::Slope:
            return true;
        default:
            return false;
    }
}

// Things that only change the player who touches them. The tracer is allowed to
// use these; their activation is restored after every trace.
static bool isPlayerOnlyInteractable(GameObjectType t) {
    switch (t) {
        case GameObjectType::YellowJumpPad:
        case GameObjectType::PinkJumpPad:
        case GameObjectType::GravityPad:
        case GameObjectType::RedJumpPad:
        case GameObjectType::SpiderPad:
        case GameObjectType::YellowJumpRing:
        case GameObjectType::PinkJumpRing:
        case GameObjectType::GravityRing:
        case GameObjectType::RedJumpRing:
        case GameObjectType::GreenRing:
        case GameObjectType::DashRing:
        case GameObjectType::GravityDashRing:
        case GameObjectType::DropRing:
        case GameObjectType::CustomRing:
        case GameObjectType::InverseGravityPortal:
        case GameObjectType::NormalGravityPortal:
        case GameObjectType::GravityTogglePortal:
        case GameObjectType::MiniSizePortal:
        case GameObjectType::RegularSizePortal:
            return true;
        default:
            return false;
    }
}

// Things that change the whole level (camera, ground, both players).
// The tracer can't safely go through these, so a trace stops when it reaches one.
static bool isLevelChangingPortal(GameObjectType t) {
    switch (t) {
        case GameObjectType::ShipPortal:
        case GameObjectType::CubePortal:
        case GameObjectType::BallPortal:
        case GameObjectType::UfoPortal:
        case GameObjectType::WavePortal:
        case GameObjectType::RobotPortal:
        case GameObjectType::SpiderPortal:
        case GameObjectType::SwingPortal:
        case GameObjectType::DualPortal:
        case GameObjectType::SoloPortal:
        case GameObjectType::TeleportPortal:
        case GameObjectType::SpiderOrb:
        case GameObjectType::TeleportOrb:
            return true;
        default:
            return false;
    }
}

static void rememberTouched(GameObject* obj) {
    for (auto& t : s.touched) if (t.obj == obj) return;
    s.touched.push_back({obj, obj->m_isActivated});
}

static void restoreTouched() {
    for (auto& t : s.touched) {
        t.obj->m_isActivated = t.activated;
    }
    s.diagOrbs = static_cast<int>(s.touched.size());
    s.touched.clear();
}

// Removes anything the tracer spawned into the level (orb rings, pad effects).
static void removeNewChildren(CCNode* node, unsigned int oldCount) {
    if (!node) return;
    auto children = node->getChildren();
    if (!children) return;
    while (children->count() > oldCount) {
        auto child = static_cast<CCNode*>(children->lastObject());
        child->removeFromParentAndCleanup(true);
    }
}

// ---------- moving objects ----------

// Called with the objects near the real player every tick.
static void trackObjects(gd::vector<GameObject*>* objects, int n) {
    for (int i = 0; i < n; ++i) {
        auto obj = (*objects)[i];
        if (!obj) continue;
        auto& tr = s.tracks[obj];
        if (tr.lastTick == s.tick) continue;          // already seen this tick
        CCPoint pos = obj->getPosition();
        int gap = s.tick - tr.lastTick;
        if (gap >= 1 && gap <= 8) {
            CCPoint v = (pos - tr.lastPos) / static_cast<float>(gap);
            // Smooth a little so one odd frame doesn't throw the prediction off.
            tr.vel = gap == 1 ? (tr.vel * 0.3f + v * 0.7f) : v;
        } else {
            tr.vel = CCPointZero;
        }
        tr.lastPos = pos;
        tr.lastTick = s.tick;
    }
    // Forget objects that haven't been near you for a while.
    if ((s.tick & 255) == 0) {
        for (auto it = s.tracks.begin(); it != s.tracks.end();) {
            if (s.tick - it->second.lastTick > 240) it = s.tracks.erase(it);
            else ++it;
        }
    }
}

// Moves an object to where it will be `ahead` ticks from now (tracer only).
static void moveAhead(GameObject* obj, int ahead) {
    auto it = s.tracks.find(obj);
    if (it == s.tracks.end()) return;
    auto& tr = it->second;
    if (s.tick - tr.lastTick > 2) return;              // no fresh data
    if (std::abs(tr.vel.x) < 0.001f && std::abs(tr.vel.y) < 0.001f) return;

    auto saved = s.movedForTrace.find(obj);
    CCPoint origin;
    if (saved == s.movedForTrace.end()) {
        origin = obj->getPosition();
        s.movedForTrace[obj] = origin;
    } else {
        origin = saved->second;
    }
    obj->setPosition(origin + tr.vel * static_cast<float>(ahead));
}

// Puts every object the tracer moved back where it really is.
static void restoreMoved() {
    s.diagMovers = static_cast<int>(s.movedForTrace.size());
    for (auto& [obj, pos] : s.movedForTrace) obj->setPosition(pos);
    s.movedForTrace.clear();
}

// ---------- tracer ----------

static void injectButton(GJBaseGameLayer* layer, bool down) {
    s.injecting = true;
    layer->handleButton(down, 1, true);
    s.injecting = false;
}

static bool ensureTracer(PlayLayer* pl) {
    if (s.fake && s.owner == pl) return true;

    // Hidden layer: the tracer and anything it spawns are never drawn.
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

static void copyPlayerState(PlayerObject* dst, PlayerObject* src) {
    dst->copyAttributes(src);
    dst->m_isShip = src->m_isShip;
    dst->m_isBird = src->m_isBird;
    dst->m_isBall = src->m_isBall;
    dst->m_isDart = src->m_isDart;
    dst->m_isRobot = src->m_isRobot;
    dst->m_isSpider = src->m_isSpider;
    dst->m_isSwing = src->m_isSwing;
    dst->m_isUpsideDown = src->m_isUpsideDown;
    dst->m_vehicleSize = src->m_vehicleSize;
    dst->m_playerSpeed = src->m_playerSpeed;
    dst->m_isOnGround = src->m_isOnGround;
    dst->m_yVelocity = src->m_yVelocity;
    dst->m_position = src->m_position;
    dst->setPosition(src->getPosition());
}

// The tracer wasn't moving: the timestep it copied from the game was in the
// wrong units for it. This measures how far you really move per tick and finds
// the timestep that makes the tracer move the same distance.
static void calibrateTracer(PlayLayer* pl) {
    if (s.dtScale > 0.f || s.frozen) return;
    if (s.realDx < 0.01f || s.playerDt <= 0.f) return;   // need you moving first

    auto measure = [&](float scale) {
        copyPlayerState(s.fake, pl->m_player1);
        float x0 = s.fake->m_position.x;
        s.tracing = true;
        s.fake->update(s.playerDt * scale);
        s.tracing = false;
        return s.fake->m_position.x - x0;
    };

    // Try the copied timestep first, then the usual unit conversions.
    for (float scale : {1.f, 60.f, 240.f, 1.f / 60.f}) {
        float dx = measure(scale);
        if (std::abs(dx) < 1e-5f) continue;
        // Movement is linear in the timestep, so this lands exactly on your speed.
        s.dtScale = scale * (s.realDx / dx);
        log::info("[CA] calibrated: realDx {:.4f} tracerDx {:.6f} at x{} -> dt scale {:.4f}",
                  s.realDx, dx, scale, s.dtScale);
        return;
    }
    s.frozen = true;
    log::info("[CA] calibration failed: tracer does not move at any timestep");
}


// Flies the tracer: keeps the current button state for `delay` ticks, then
// applies the input (`down`), then keeps that state until `maxTicks`.
// Returns the tick it died on, or -1 if it survived.
static int traceInput(PlayLayer* pl, int delay, bool down, int maxTicks,
                      std::chrono::steady_clock::time_point deadline) {
    s.diagTicks = 0;
    s.diagCollChecks = 0;
    s.diagMaxObjs = 0;
    s.diagMovedX = 0.f;
    s.diagMovedY = 0.f;
    s.diagStartDiff = 0.f;
    s.diagTimedOut = false;
    s.diagOrbs = 0;
    s.diagFallbackDeaths = 0;
    s.diagMovers = 0;
    s.diagReason.clear();
    s.touched.clear();

    if (s.playerDt <= 0.f)  { s.diagReason = "no update dt"; return -1; }
    if (s.collideDt <= 0.f) { s.diagReason = "no collision dt"; return -1; }
    if (!ensureTracer(pl))  { s.diagReason = "no tracer"; return -1; }
    calibrateTracer(pl);
    if (s.frozen)           { s.diagReason = "tracer does not move"; return -1; }
    if (s.dtScale <= 0.f)   { s.diagReason = "calibrating (move first)"; return -1; }
    float stepDt = s.playerDt * s.dtScale;
    float collDt = s.collideDt * s.dtScale;

    copyPlayerState(s.fake, pl->m_player1);
    auto startPos = s.fake->m_position;
    s.diagStartDiff = ccpDistance(startPos, pl->m_player1->m_position);

    unsigned int objLayerCount = pl->m_objectLayer ? pl->m_objectLayer->getChildrenCount() : 0;
    unsigned int plCount = pl->getChildrenCount();

    s.tracing = true;
    s.traceDied = false;
    s.traceDone = false;

    // Put the tracer in the same button state the game is in right now.
    bool held = s.heldInGame;
    if (held) s.fake->pushButton(PlayerButton::Jump);
    else      s.fake->releaseButton(PlayerButton::Jump);

    // Orb fix: in the real game, a click uses whatever orb you touched on the
    // previous tick. A freshly copied tracer hasn't touched anything yet, so
    // clicking inside an orb did a normal jump instead of the orb. One collision
    // pass at your current position first lets the tracer "touch" the orb you're
    // in, exactly like you already have.
    s.traceAhead = 0;
    pl->checkCollisions(s.fake, collDt, false);
    int death = -1;
    if (s.traceDied) death = 0;   // already touching something deadly

    for (int t = 0; t < maxTicks && death < 0; ++t) {
        if (t == delay && held != down) {
            if (down) s.fake->pushButton(PlayerButton::Jump);
            else      s.fake->releaseButton(PlayerButton::Jump);
            held = down;
        }
        s.traceAhead = t + 1;
        s.fake->update(stepDt);
        pl->checkCollisions(s.fake, collDt, false);
        s.diagTicks = t + 1;

        if (s.traceDied) { death = t; break; }
        if (s.traceDone) break;
        if ((t & 3) == 3 && std::chrono::steady_clock::now() > deadline) {
            s.diagTimedOut = true;
            break;
        }
    }

    if (held) s.fake->releaseButton(PlayerButton::Jump);
    s.diagMovedX = s.fake->m_position.x - startPos.x;
    s.diagMovedY = s.fake->m_position.y - startPos.y;

    // Put the level back exactly as it was.
    restoreTouched();
    restoreMoved();
    removeNewChildren(pl->m_objectLayer, objLayerCount);
    removeNewChildren(pl, plCount);

    s.tracing = false;
    s.traceDied = false;
    s.traceDone = false;
    return death;
}

// Finds the first tick (+0 .. +window) where this input doesn't kill you.
// Returns that delay, or 0 if none is safe (input is sent on time).
// Gamemodes the assist leaves alone:
//  - ship and swing: nothing is checked
//  - robot: presses are checked, releases are not
static bool assistOff(PlayerObject* p, bool down) {
    if (p->m_isShip || p->m_isSwing) return true;
    if (p->m_isRobot && !down) return true;
    return false;
}

static int planInput(PlayLayer* pl, PendingInput& in) {
    // Not assisted in this gamemode: the input goes through on time.
    if (assistOff(pl->m_player1, in.down)) {
        in.skipped = true;
        in.foundSafe = true;
        in.firstDeath = -1;
        s.planMs = 0.0;
        return 0;
    }

    auto start = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::microseconds(4000);

    int look = pl->m_player1->m_isDart ? waveLookahead() : lookaheadTicks();
    // Presses and releases each have their own search window.
    int window = in.down ? assistWindow() : releaseWindow();
    if (in.spam) window = std::min(window, spamMaxDelay());
    int chosen = 0;
    in.foundSafe = false;
    in.firstDeath = -1;

    for (int d = 0; d <= window; ++d) {
        int death = traceInput(pl, d, in.down, d + look, deadline);
        if (d == 0) in.firstDeath = death;
        if (!s.diagReason.empty() || s.diagTimedOut) { chosen = 0; in.foundSafe = true; break; }
        if (death < 0) { chosen = d; in.foundSafe = true; break; }
    }

    s.planMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    return chosen;
}

static void logDecision(PendingInput const& in, int delay) {
    if (in.skipped) return;
    char const* what = in.spam ? (in.down ? "press* " : "release*")
                               : (in.down ? "press  " : "release");
    if (in.firstDeath < 0) {
        debugLine(fmt::format("{} +0 safe  ({:.2f}ms)", what, s.planMs));
    } else if (in.foundSafe) {
        debugLine(fmt::format("{} dies@{} -> +{}  ({:.2f}ms)", what, in.firstDeath, delay, s.planMs));
    } else {
        debugLine(fmt::format("{} dies@{}, none safe -> on time  ({:.2f}ms)",
                              what, in.firstDeath, s.planMs));
    }
    debugDiag();
}

// ---------- game layer hooks ----------

class $modify(AssistGameLayer, GJBaseGameLayer) {
    bool isActivePlayLayer() {
        auto pl = PlayLayer::get();
        return pl && static_cast<GJBaseGameLayer*>(pl) == this;
    }

    void applied(PendingInput const& in, int fireTick) {
        s.heldInGame = in.down;
        // A delayed press shifts its release by the same amount, so your hold
        // length stays exactly the same. After a release, nothing carries over.
        s.pressShift = in.down ? (fireTick - in.realTick) : 0;
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

        // Not assisted in this gamemode (ship, swing, robot releases). If nothing
        // is waiting, the input goes straight through; otherwise it queues
        // behind the waiting ones to keep the order.
        // A robot release after a delayed press still waits for its shift, so
        // the jump keeps the height you played; it just isn't checked.
        bool needsShift = !down && s.pressShift > 0;
        if (assistOff(pl->m_player1, down) && s.queue.empty() && !needsShift) {
            s.heldInGame = down;
            s.pressShift = 0;
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        // Dual mode: the assist steps aside completely. Anything still waiting is
        // sent right now, and every input goes straight to the game, so both
        // players always get your clicks exactly as you make them.
        if (m_gameState.m_isDualMode) {
            flushQueue();
            s.heldInGame = down;
            s.pressShift = 0;
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        // Wave spam fix: inputs closer together than the spam gap count as spam.
        // While spamming, nothing is allowed to pile up: anything still waiting
        // is sent right now, and each input may only move a tiny bit.
        int gap = s.tick - s.lastRealInputTick;
        s.lastRealInputTick = s.tick;
        bool spamming = waveSpamFix() && pl->m_player1->m_isDart && gap < spamGap();

        if (spamming && !s.queue.empty()) {
            int flushed = static_cast<int>(s.queue.size());
            flushQueue();
            debugLine(fmt::format("wave spam: sent {} waiting input(s) now", flushed));
        }

        PendingInput in;
        in.down = down;
        in.spam = spamming;
        in.realTick = s.tick;
        in.base = s.tick + ((down || spamming) ? 0 : s.pressShift);

        if (s.queue.empty() && in.base <= s.tick) {
            int delay = planInput(pl, in);
            logDecision(in, delay);
            if (delay == 0) {
                // Safe now (or nothing better): goes through untouched.
                applied(in, s.tick);
                return GJBaseGameLayer::handleButton(down, button, isPlayer1);
            }
            in.fireTick = s.tick + delay;
        }
        s.queue.push_back(in);
    }

    // Sends everything still waiting right now, in order.
    void flushQueue() {
        while (!s.queue.empty()) {
            PendingInput w = s.queue.front();
            s.queue.pop_front();
            injectButton(this, w.down);
            applied(w, s.tick);
        }
        s.pressShift = 0;
    }

    // Sends queued inputs in order, each on its planned tick.
    void processQueue() {
        if (m_gameState.m_isDualMode) { flushQueue(); return; }
        auto pl = PlayLayer::get();
        while (!s.queue.empty()) {
            auto& in = s.queue.front();

            if (in.fireTick < 0) {
                if (s.tick < in.base) break;
                int delay = planInput(pl, in);
                logDecision(in, delay);
                in.fireTick = s.tick + delay;
            }
            if (s.tick < in.fireTick) break;

            PendingInput done = in;
            s.queue.pop_front();
            injectButton(this, done.down);
            applied(done, s.tick);

            // A following release keeps the hold length of the press it belongs to.
            if (done.down) {
                for (auto& next : s.queue) {
                    if (!next.down && next.fireTick < 0) {
                        next.base = next.realTick + s.pressShift;
                        break;
                    }
                }
            }
        }
    }

    // Called once per physics tick (240 per second).
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (s.tracing || !assistEnabled() || !isActivePlayLayer()) {
            return GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        }
        processQueue();
        float x0 = m_player1 ? m_player1->m_position.x : 0.f;
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (m_player1) {
            float dx = m_player1->m_position.x - x0;
            if (dx > 0.01f && dx < 100.f) s.realDx = dx;   // ignore resets/teleports
        }
        s.tick++;
    }

    int checkCollisions(PlayerObject* player, float dt, bool ignoreDamage) {
        if (!s.tracing && isActivePlayLayer() && player == m_player1) s.collideDt = dt;
        return GJBaseGameLayer::checkCollisions(player, dt, ignoreDamage);
    }

    // What the tracer collides with:
    //  - blocks, spikes, slopes: normally, plus our own hitbox check as a backup
    //  - orbs, pads, gravity/size portals: used normally, activation restored afterwards
    //  - gamemode/dual/teleport portals, spider/teleport orbs: trace stops there
    //  - everything else (coins, triggers, decoration): ignored
    void collisionCheckObjects(PlayerObject* player, gd::vector<GameObject*>* objects,
                               int count, float dt) {
        if (!s.tracing || player != s.fake || !objects) {
            if (!s.tracing && objects && assistEnabled() && predictMoving()
                && isActivePlayLayer() && player == m_player1) {
                trackObjects(objects, std::min(count, static_cast<int>(objects->size())));
            }
            return GJBaseGameLayer::collisionCheckObjects(player, objects, count, dt);
        }

        static gd::vector<GameObject*> kept;
        kept.clear();
        auto playerRect = player->getObjectRect();
        int n = std::min(count, static_cast<int>(objects->size()));

        for (int i = 0; i < n; ++i) {
            auto obj = (*objects)[i];
            if (!obj) continue;
            auto type = obj->m_objectType;

            if (isSolidOrHazard(type)) {
                kept.push_back(obj);
            } else if (isPlayerOnlyInteractable(type)) {
                rememberTouched(obj);
                kept.push_back(obj);
            } else if (isLevelChangingPortal(type)) {
                if (playerRect.intersectsRect(obj->getObjectRect())) s.traceDone = true;
            }
        }

        // Moving objects: put each one where it will be at this point of the prediction.
        if (predictMoving() && s.traceAhead > 0) {
            for (auto obj : kept) moveAhead(obj, s.traceAhead);
        }

        s.diagCollChecks++;
        s.diagMaxObjs = std::max(s.diagMaxObjs, static_cast<int>(kept.size()));
        GJBaseGameLayer::collisionCheckObjects(player, &kept, static_cast<int>(kept.size()), dt);

        // Backup death check, in case the game doesn't report the tracer's death:
        // spikes kill on the full hitbox, blocks kill on the small inner hitbox.
        if (!s.traceDied) {
            auto outer = player->getObjectRect();
            auto inner = player->getObjectRect(0.3f, 0.3f);
            for (auto obj : kept) {
                auto type = obj->m_objectType;
                bool hit = false;
                if (type == GameObjectType::Hazard || type == GameObjectType::AnimatedHazard) {
                    hit = outer.intersectsRect(obj->getObjectRect());
                } else if (type == GameObjectType::Solid) {
                    hit = inner.intersectsRect(obj->getObjectRect());
                }
                if (hit) {
                    s.traceDied = true;
                    s.diagFallbackDeaths++;
                    break;
                }
            }
        }
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
            if (on) label->setString(fmt::format("CA {}p/{}r {}", assistWindow(), releaseWindow(), kBuildTag).c_str());
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
            if (player == s.fake) s.traceDied = true;
            return;
        }
        PlayLayer::destroyPlayer(player, obj);
    }

    void levelComplete() {
        if (s.tracing) { s.traceDone = true; return; }
        PlayLayer::levelComplete();
    }
};
