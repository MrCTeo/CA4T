#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/ui/GeodeUI.hpp>
#include <chrono>
#include <deque>
#include <unordered_map>

using namespace geode::prelude;

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
static bool releaseAssist()  { return Mod::get()->getSettingValue<bool>("release-assist"); }
static int  releaseAssistAfter() { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("release-assist-after")); }
static int  forceDelay()     { return static_cast<int>(Mod::get()->getSettingValue<int64_t>("force-delay")); }
static bool debugPanel()     { return Mod::get()->getSettingValue<bool>("debug"); }

// ---------- state ----------

struct PendingInput {
    bool down = true;
    int realTick = 0;        // tick you actually pressed/released
    int base = 0;            // earliest tick it may be sent (keeps your hold length)
    int fireTick = -1;       // -1 = not planned yet
    int firstDeath = -1;     // where the on-time input would have died
    bool foundSafe = true;
    bool skipped = false;    // not checked (gamemode not assisted)
    bool spam = false;       // part of fast wave spam
    bool autoRelease = false; // made by Release Assist, not by you
    bool checked = false;    // the tracer actually verified the chosen tick
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
    // How far you really move forward per tick (measured every tick).
    float realDx = 0.f;
    float prevTickX = 0.f;
    int prevTickNum = -1000;
    // Offset between your drawn position and your physics position.
    CCPoint nodeOffset = CCPointZero;
    bool builtinActive = false;          // current trace uses built-in wave physics
    std::vector<GameObject*> lastKept;   // objects near the tracer this step

    // Inputs
    std::deque<PendingInput> queue;
    bool heldInGame = false;   // what the game currently thinks the button is
    int lastRealInputTick = -100000;
    int pressShift = 0;        // how late the last press was sent (applied to its release)
    bool injecting = false;
    double planMs = 0.0;

    // Release Assist
    bool physHeld = false;       // you are physically holding the button
    int holdStartTick = 0;       // tick the current press reached the game
    bool autoReleased = false;   // Release Assist let go for you; your own release is skipped
    int nextAutoCheck = 0;       // earliest tick to look for a release again

    // Objects the tracer used (orbs, pads, portals): their activation is put back after each trace
    struct Touched { GameObject* obj; bool activated; };
    std::vector<Touched> touched;
    int diagOrbs = 0;                    // interactables the tracer touched
    int diagFallbackDeaths = 0;          // deaths found by our own hitbox check

    // Moving objects: positions of objects near you are tracked every real
    // tick, so the tracer can move them ahead in time while it predicts.
    struct Track {
        CCPoint lastPos;
        CCPoint vel;                 // units per tick
        CCPoint acc;                 // change of vel per tick (easing, speeding up, slowing down)
        float lastRot = 0.f;
        float rotVel = 0.f;          // degrees per tick
        int lastTick = -1000;
        int samples = 0;
    };
    std::unordered_map<GameObject*, Track> tracks;
    struct Saved { CCPoint pos; float rot; };
    std::unordered_map<GameObject*, Saved> movedForTrace;     // original positions/rotations
    bool scouting = false;               // looking ahead for objects, not predicting
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
        physHeld = false;
        holdStartTick = 0;
        autoReleased = false;
        nextAutoCheck = 0;
        lastRealInputTick = -100000;
        pressShift = 0;
        injecting = false;
        lines.clear();
        prevTickNum = -1000;
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
    debugLine(fmt::format("   ticks {} moved {:.3f},{:.3f} dt {:.5f}/{:.5f} speed {:.3f} coll {} objs {} orbs {} movers {}{}{}",
                          s.diagTicks, s.diagMovedX, s.diagMovedY, s.playerDt, s.collideDt, s.realDx,
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

// Records where objects are right now and works out how they move:
// speed, acceleration (for eased moves) and rotation speed.
static void trackObjects(gd::vector<GameObject*>* objects, int n) {
    for (int i = 0; i < n; ++i) {
        auto obj = (*objects)[i];
        if (!obj) continue;
        auto& tr = s.tracks[obj];
        if (tr.lastTick == s.tick) continue;          // already seen this tick
        CCPoint pos = obj->getPosition();
        float rot = obj->getRotation();
        int gap = s.tick - tr.lastTick;
        if (gap >= 1 && gap <= 8 && tr.samples > 0) {
            float g = static_cast<float>(gap);
            CCPoint v = (pos - tr.lastPos) / g;
            float rv = (rot - tr.lastRot) / g;
            if (tr.samples >= 2) {
                CCPoint a = (v - tr.vel) / g;
                tr.acc = tr.acc * 0.5f + a * 0.5f;
            }
            // Light smoothing so one odd frame doesn't throw the prediction off.
            tr.vel = tr.vel * 0.25f + v * 0.75f;
            tr.rotVel = tr.rotVel * 0.25f + rv * 0.75f;
            tr.samples++;
        } else {
            tr.vel = CCPointZero;
            tr.acc = CCPointZero;
            tr.rotVel = 0.f;
            tr.samples = 1;
        }
        tr.lastPos = pos;
        tr.lastRot = rot;
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

// Puts an object where it will be `ahead` ticks from now (tracer only):
// position from speed + acceleration, angle from rotation speed.
static void moveAhead(GameObject* obj, int ahead) {
    auto it = s.tracks.find(obj);
    if (it == s.tracks.end()) return;
    auto& tr = it->second;
    if (tr.samples < 2 || s.tick - tr.lastTick > 6) return;    // no fresh data
    bool moving = std::abs(tr.vel.x) > 0.001f || std::abs(tr.vel.y) > 0.001f
               || std::abs(tr.acc.x) > 0.0005f || std::abs(tr.acc.y) > 0.0005f;
    bool rotating = std::abs(tr.rotVel) > 0.01f;
    if (!moving && !rotating) return;

    auto saved = s.movedForTrace.find(obj);
    if (saved == s.movedForTrace.end()) {
        // Remember the real position/angle once, to put it back after the trace.
        s.movedForTrace[obj] = AssistState::Saved{obj->getPosition(), obj->getRotation()};
    }

    // Data is from tr.lastTick; predict from there to `ahead` ticks after now.
    float t = static_cast<float>(ahead + (s.tick - tr.lastTick));
    CCPoint base = tr.lastPos;
    CCPoint byVel = tr.vel * t;
    CCPoint byAcc = tr.acc * (0.5f * t * t);
    // Acceleration is only trusted up to the size of the move itself, so a noisy
    // reading can't fling an object across the screen.
    float velLen = ccpLength(byVel) + 2.f;
    float accLen = ccpLength(byAcc);
    if (accLen > velLen) byAcc = byAcc * (velLen / accLen);

    if (moving) obj->setPosition(base + byVel + byAcc);
    if (rotating) obj->setRotation(tr.lastRot + tr.rotVel * t);
}

// Puts every object the tracer moved back exactly where it really is.
static void restoreMoved() {
    s.diagMovers = static_cast<int>(s.movedForTrace.size());
    for (auto& [obj, sv] : s.movedForTrace) {
        obj->setPosition(sv.pos);
        obj->setRotation(sv.rot);
    }
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

    // The last argument tells GD this player belongs to a real level. Players
    // created with `false` (like the ones in the main menu) don't run their own
    // physics, which is why the tracer never moved.
    auto fake = PlayerObject::create(1, 1, pl, hidden, true);
    if (!fake) return false;
    // Not hidden itself: GD may skip updating a hidden player. Its parent layer
    // is hidden, so it's still never drawn.
    hidden->addChild(fake);

    s.fake = fake;
    s.owner = pl;
    return true;
}

// Copies a field only if it exists in your GD version's bindings, so a guessed
// field name can never break the build.
#define CA_COPY_IF_EXISTS(dst, src, field) \
    [](auto* d, auto* s2) { if constexpr (requires { d->field = s2->field; }) d->field = s2->field; }(dst, src)
#define CA_HAS_FIELD(type, field) \
    ([](auto* d) { return requires { d->field; }; }(static_cast<type*>(nullptr)))

// True if the tracer can copy your exact button/orb-buffer state, instead of
// pretending you just pressed.
static constexpr bool kCopiesHoldState = CA_HAS_FIELD(PlayerObject, m_holdingButtons);

static void copyPlayerState(PlayerObject* dst, PlayerObject* src) {
    dst->copyAttributes(src);

    // Button and orb state: whether you're holding, whether a click is buffered,
    // which orbs/pads you're touching or already used. This is what decides if
    // an orb fires, so the tracer has to match you exactly.
    CA_COPY_IF_EXISTS(dst, src, m_holdingButtons);
    CA_COPY_IF_EXISTS(dst, src, m_jumpBuffered);
    CA_COPY_IF_EXISTS(dst, src, m_stateJumpBuffered);
    CA_COPY_IF_EXISTS(dst, src, m_wasJumpBuffered);
    CA_COPY_IF_EXISTS(dst, src, m_hasJustHeld);
    CA_COPY_IF_EXISTS(dst, src, m_hasJustHeld2);
    CA_COPY_IF_EXISTS(dst, src, m_isHolding);
    CA_COPY_IF_EXISTS(dst, src, m_isHolding2);
    CA_COPY_IF_EXISTS(dst, src, m_stateRingJump);
    CA_COPY_IF_EXISTS(dst, src, m_stateRingJump2);
    CA_COPY_IF_EXISTS(dst, src, m_touchedRings);
    CA_COPY_IF_EXISTS(dst, src, m_ringRelatedSet);
    CA_COPY_IF_EXISTS(dst, src, m_touchedPad);

    // Movement state used by jumps, slopes, dashes and gravity.
    CA_COPY_IF_EXISTS(dst, src, m_isDashing);
    CA_COPY_IF_EXISTS(dst, src, m_dashX);
    CA_COPY_IF_EXISTS(dst, src, m_dashY);
    CA_COPY_IF_EXISTS(dst, src, m_dashAngle);
    CA_COPY_IF_EXISTS(dst, src, m_isSliding);
    CA_COPY_IF_EXISTS(dst, src, m_isRising);
    CA_COPY_IF_EXISTS(dst, src, m_isOnSlope);
    CA_COPY_IF_EXISTS(dst, src, m_wasOnSlope);
    CA_COPY_IF_EXISTS(dst, src, m_gravityMod);
    CA_COPY_IF_EXISTS(dst, src, m_gravity);
    CA_COPY_IF_EXISTS(dst, src, m_isLocked);
    CA_COPY_IF_EXISTS(dst, src, m_lastGroundedPos);
    CA_COPY_IF_EXISTS(dst, src, m_isSideways);
    CA_COPY_IF_EXISTS(dst, src, m_isGoingLeft);

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
    // Where the player was last tick. GD checks collisions along the path from
    // here to the current position; a fresh tracer had (0,0) here, so its first
    // collision check swept across half the level and "died" instantly.
    dst->m_lastPosition = src->m_lastPosition;
    dst->setPosition(src->getPosition());
}

// ---------- tracer movement helpers ----------

// Moves the tracer: physics position and drawn position together, keeping the
// same offset between them that your real player has.
static void setTracerPos(CCPoint p) {
    s.fake->m_position = p;
    s.fake->setPosition(p + s.nodeOffset);
}

// The tracer's hitbox: your real hitbox, moved to where the tracer is. That way
// it's always exactly your size (mini, wave hitbox) whatever the tracer copied.
static PlayerObject* s_realPlayer = nullptr;
static CCRect tracerRect() {
    auto r = s_realPlayer->getObjectRect();
    CCPoint off = s.fake->m_position - s_realPlayer->m_position;
    r.origin = r.origin + off;
    return r;
}

// A smaller box in the middle of a hitbox (GD's "inner hitbox" for blocks).
static CCRect innerRect(CCRect const& r, float scale) {
    float w = r.size.width * scale, h = r.size.height * scale;
    return CCRect(r.getMidX() - w / 2.f, r.getMidY() - h / 2.f, w, h);
}

// After GD has run its code on the tracer, its new height can be in either the
// physics position or the drawn position, depending on what GD changed. This
// takes whichever one moved away from `y`, and keeps both in sync.
static float pickNewY(float y) {
    float my = s.fake->m_position.y;
    float ny = s.fake->getPositionY() - s.nodeOffset.y;
    if (std::abs(my - y) > 1e-4f) return my;
    if (std::abs(ny - y) > 1e-4f) return ny;
    return y;
}

// ---------- built-in wave physics ----------
//
// Wave is simple enough to simulate exactly: it moves forward at your speed and
// goes diagonally up while held, down while released (twice as steep in mini).
// Floors and ceilings are slid along; hitting a block side or a spike kills.

static void waveStep(bool held) {
    CCPoint p = s.fake->m_position;
    // GD checks collisions along the path from last tick's position, so that
    // has to be where the tracer really was one tick ago.
    s.fake->m_lastPosition = p;
    float slope = s.fake->m_vehicleSize < 0.9f ? 2.f : 1.f;
    float dir = (held ? 1.f : -1.f) * (s.fake->m_isUpsideDown ? -1.f : 1.f);
    p.x += s.realDx;
    p.y += dir * slope * s.realDx;
    setTracerPos(p);
}

// Returns true if the tracer dies against the objects near it.
static bool waveCollide(CCRect const& before) {
    for (auto obj : s.lastKept) {
        auto type = obj->m_objectType;
        auto r = obj->getObjectRect();
        auto outer = tracerRect();
        if (!outer.intersectsRect(r)) continue;

        if (type == GameObjectType::Hazard || type == GameObjectType::AnimatedHazard) {
            return true;
        }
        if (type == GameObjectType::Solid) {
            float top = r.getMaxY(), bottom = r.getMinY();
            if (before.getMinY() >= top - 0.5f) {
                // Came from above: slide along the top.
                CCPoint p = s.fake->m_position;
                p.y += top - outer.getMinY();
                setTracerPos(p);
                continue;
            }
            if (before.getMaxY() <= bottom + 0.5f) {
                // Came from below: slide along the bottom.
                CCPoint p = s.fake->m_position;
                p.y -= outer.getMaxY() - bottom;
                setTracerPos(p);
                continue;
            }
            return true;   // hit the side of a block
        }
    }
    return false;
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
    if (s.realDx < 0.01f)   { s.diagReason = "measuring your speed"; return -1; }

    // Wave: built-in wave physics, exact and independent of GD's player code.
    // Other modes: GD's own physics for height (jumps, gravity, orbs, pads) at
    // the game's real timestep, and your measured speed for forward movement.
    bool builtinWave = pl->m_player1->m_isDart;
    s.builtinActive = builtinWave;
    s_realPlayer = pl->m_player1;
    s.nodeOffset = pl->m_player1->getPosition() - pl->m_player1->m_position;
    float stepDt = s.playerDt;
    float collDt = s.collideDt;

    copyPlayerState(s.fake, pl->m_player1);
    auto startPos = s.fake->m_position;
    s.diagStartDiff = ccpDistance(startPos, pl->m_player1->m_position);

    unsigned int objLayerCount = pl->m_objectLayer ? pl->m_objectLayer->getChildrenCount() : 0;
    unsigned int plCount = pl->getChildrenCount();

    s.tracing = true;
    s.traceDied = false;
    s.traceDone = false;

    // Put the tracer in the same button state the game is in right now.
    // If your exact hold/buffer state was copied, it already matches. Otherwise
    // it's recreated with a press/release (less exact for orbs).
    bool held = s.heldInGame;
    if (!kCopiesHoldState) {
        if (held) s.fake->pushButton(PlayerButton::Jump);
        else      s.fake->releaseButton(PlayerButton::Jump);
    }

    // Orb fix: in the real game, a click uses whatever orb you touched on the
    // previous tick. A freshly copied tracer hasn't touched anything yet, so
    // clicking inside an orb did a normal jump instead of the orb. One collision
    // pass at your current position first lets the tracer "touch" the orb you're
    // in, exactly like you already have.
    s.traceAhead = 0;
    pl->checkCollisions(s.fake, collDt, false);
    int death = -1;
    // A death in this setup pass isn't a real prediction (you're alive right
    // now), so it's ignored. Only deaths after the tracer actually moves count.
    s.traceDied = false;

    for (int t = 0; t < maxTicks && death < 0; ++t) {
        if (t == delay && held != down) {
            if (down) s.fake->pushButton(PlayerButton::Jump);
            else      s.fake->releaseButton(PlayerButton::Jump);
            held = down;
        }
        s.traceAhead = t + 1;
        s.lastKept.clear();
        if (builtinWave) {
            CCRect before = tracerRect();
            // GD's player code applies orb/portal effects (gravity flips, size
            // changes) to the tracer; the position it computes is thrown away,
            // because the built-in wave movement is exact.
            CCPoint keep = s.fake->m_position;
            s.fake->update(stepDt);
            setTracerPos(keep);
            waveStep(held);
            pl->checkCollisions(s.fake, collDt, false);   // orbs, portals, moving objects
            if (!s.traceDied && waveCollide(before)) {
                s.traceDied = true;
                s.diagFallbackDeaths++;
            }
        } else {
            CCPoint p = s.fake->m_position;
            s.fake->m_lastPosition = p;
            setTracerPos(p);                      // both positions in sync before GD runs
            s.fake->update(stepDt);               // GD: jumps, gravity, orbs, pads
            CCPoint np(p.x + s.realDx, pickNewY(p.y));
            setTracerPos(np);                     // forward at your real speed
            pl->checkCollisions(s.fake, collDt, false);
            // Collisions may have moved it (landing on a block): keep that height.
            setTracerPos(CCPoint(np.x, pickNewY(np.y)));
        }
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
    s.builtinActive = false;

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

    // Test setting: delay every press by a fixed amount, no prediction at all.
    // Used to check that holding back inputs works by itself.
    if (forceDelay() > 0 && in.down) {
        in.firstDeath = 0;
        in.foundSafe = true;
        s.planMs = 0.0;
        s.diagReason = "forced delay test";
        return forceDelay();
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
        // Couldn't check (see SKIPPED reason): send on time.
        if (!s.diagReason.empty()) { chosen = 0; in.foundSafe = true; break; }
        // Ran out of time before finding a safe tick: send on time.
        if (s.diagTimedOut) { chosen = 0; in.foundSafe = false; break; }
        if (death < 0) { chosen = d; in.foundSafe = true; in.checked = true; break; }
    }

    s.planMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    return chosen;
}

static void logDecision(PendingInput const& in, int delay) {
    if (in.skipped) return;
    char const* what = in.autoRelease ? "auto-release"
                     : in.spam ? (in.down ? "press* " : "release*")
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
        if (in.down) s.holdStartTick = fireTick;
        if (in.autoRelease) s.autoReleased = true;
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

        // Release Assist bookkeeping.
        s.physHeld = down;
        if (down) {
            s.autoReleased = false;
            s.nextAutoCheck = 0;
        } else {
            if (s.autoReleased) {
                // Release Assist already let go for you: the game isn't holding,
                // so your own release has nothing left to do.
                s.autoReleased = false;
                return;
            }
            // You let go before Release Assist's planned release: drop that plan
            // and handle your own release normally.
            for (auto it = s.queue.begin(); it != s.queue.end();) {
                if (it->autoRelease) it = s.queue.erase(it);
                else ++it;
            }
        }

        // Not assisted in this gamemode (ship, swing, robot releases). If nothing
        // is waiting, the input goes straight through; otherwise it queues
        // behind the waiting ones to keep the order.
        // A robot release after a delayed press still waits for its shift, so
        // the jump keeps the height you played; it just isn't checked.
        bool needsShift = !down && s.pressShift > 0;
        if (assistOff(pl->m_player1, down) && s.queue.empty() && !needsShift) {
            s.heldInGame = down;
            if (down) s.holdStartTick = s.tick;
            s.pressShift = 0;
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        // Dual mode: the assist steps aside completely. Anything still waiting is
        // sent right now, and every input goes straight to the game, so both
        // players always get your clicks exactly as you make them.
        if (m_gameState.m_isDualMode) {
            flushQueue();
            s.heldInGame = down;
            if (down) s.holdStartTick = s.tick;
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

    // Release Assist: once you've held longer than the limit, find the first tick
    // where letting go doesn't kill you (same check as normal inputs) and let go
    // for you there. If no tick is safe yet, it keeps you holding and looks again.
    void releaseAssistCheck() {
        if (!releaseAssist()) return;
        if (!s.physHeld || !s.heldInGame || s.autoReleased || !s.queue.empty()) return;
        if (m_gameState.m_isDualMode) return;
        auto pl = PlayLayer::get();
        auto p = pl->m_player1;
        if (p->m_isPlatformer || assistOff(p, false)) return;      // ship, swing, robot
        if (s.tick - s.holdStartTick < releaseAssistAfter()) return;
        if (s.tick < s.nextAutoCheck) return;

        PendingInput in;
        in.down = false;
        in.autoRelease = true;
        in.realTick = s.tick;
        in.base = s.tick;
        int delay = planInput(pl, in);

        if (in.checked) {
            logDecision(in, delay);
            in.fireTick = s.tick + delay;
            s.queue.push_back(in);
        } else {
            // Nothing safe (or couldn't check): keep holding, look again soon.
            s.nextAutoCheck = s.tick + std::max(1, releaseWindow());
        }
    }

    // Moving objects: watches the area AHEAD of you every tick, so objects already
    // have a known speed by the time you click near them. One cheap lookup per
    // tick, cycling through four spots in front of / above / below you.
    void scoutAhead() {
        if (!predictMoving() || s.realDx < 0.01f || s.collideDt <= 0.f || !m_player1) return;
        auto pl = PlayLayer::get();
        if (!ensureTracer(pl)) return;

        static const float aheadTicks[4] = {10.f, 25.f, 18.f, 18.f};
        static const float yOffset[4]    = {0.f, 0.f, 50.f, -50.f};
        int k = s.tick & 3;

        copyPlayerState(s.fake, m_player1);
        s.nodeOffset = m_player1->getPosition() - m_player1->m_position;
        CCPoint p = m_player1->m_position;
        p.x += s.realDx * aheadTicks[k];
        p.y += yOffset[k];
        s.fake->m_lastPosition = p;
        setTracerPos(p);

        s.tracing = true;
        s.scouting = true;
        pl->checkCollisions(s.fake, s.collideDt, true);
        s.scouting = false;
        s.tracing = false;
        s.traceDied = false;
        s.traceDone = false;
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
        // Measure how far you really move per tick, from one tick's start to the
        // next. This works no matter where in the tick GD moves the player.
        if (m_player1) {
            float x = m_player1->m_position.x;
            if (s.prevTickNum == s.tick - 1) {
                float dx = x - s.prevTickX;
                if (dx > 0.01f && dx < 100.f) s.realDx = dx;   // ignore resets/teleports
            }
            s.prevTickX = x;
            s.prevTickNum = s.tick;
        }

        releaseAssistCheck();
        processQueue();
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        scoutAhead();
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
        // Scouting: just remember where the objects ahead are, nothing else runs.
        if (s.scouting && player == s.fake && objects) {
            trackObjects(objects, std::min(count, static_cast<int>(objects->size())));
            return;
        }
        if (!s.tracing || player != s.fake || !objects) {
            if (!s.tracing && objects && assistEnabled() && predictMoving()
                && isActivePlayLayer() && player == m_player1) {
                trackObjects(objects, std::min(count, static_cast<int>(objects->size())));
            }
            return GJBaseGameLayer::collisionCheckObjects(player, objects, count, dt);
        }

        static gd::vector<GameObject*> kept;
        kept.clear();
        auto playerRect = s_realPlayer ? tracerRect() : player->getObjectRect();
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

        // GD can check a step in several batches, so collect all of them.
        s.lastKept.insert(s.lastKept.end(), kept.begin(), kept.end());
        if (s.builtinActive) return;   // built-in wave physics does its own collision

        // Backup death check, in case the game doesn't report the tracer's death:
        // spikes kill on the full hitbox, blocks kill on the small inner hitbox.
        // Uses your real hitbox size moved to the tracer, so the size is always right.
        if (!s.traceDied && s_realPlayer) {
            auto outer = tracerRect();
            auto inner = innerRect(outer, 0.3f);
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

// ---------- play layer: tracer deaths, debug panel ----------

class $modify(AssistPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        s.reset();
        s.fake = nullptr;
        s.owner = nullptr;
        s.playerDt = 0.f;
        s.collideDt = 0.f;
        s.debugLabel = nullptr;

        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        auto win = CCDirector::get()->getWinSize();

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
        if (s.debugLabel) s.debugLabel->setVisible(on && debugPanel());
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        s.reset();
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

// ---------- pause menu: settings button ----------

class $modify(AssistPause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto spr = CCSprite::createWithSpriteFrameName("GJ_optionsBtn_001.png");
        spr->setScale(0.6f);
        auto btn = CCMenuItemSpriteExtra::create(spr, this, menu_selector(AssistPause::onAssistSettings));

        auto menu = CCMenu::create();
        menu->addChild(btn);
        menu->setPosition({35.f, 35.f});
        this->addChild(menu, 10);
    }

    void onAssistSettings(CCObject*) {
        geode::openSettingsPopup(Mod::get());
    }
};
