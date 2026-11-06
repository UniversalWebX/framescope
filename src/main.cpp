#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>

#include <Geode/binding/ButtonSprite.hpp>
#include <Geode/binding/CCMenuItemSpriteExtra.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/cocos/draw_nodes/CCDrawNode.h>
#include <Geode/cocos/label_nodes/CCLabelBMFont.h>
#include <Geode/cocos/layers_scenes_transitions_nodes/CCLayer.h>
#include <Geode/cocos/menu_nodes/CCMenu.h>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

using namespace geode::prelude;

namespace imadamnduck {

constexpr int kSearchRadius = 12;
constexpr int kBoundaryCapacity = 1024;
constexpr int kRecentCapacity = 64;
constexpr int kJumpButton = 1;

// Buckets are based on the closest sampled state change.
enum class Bucket : std::uint8_t {
    One, Two, Three, Four, FiveSix, SevenEight, NineTen, ElevenPlus, Count
};
constexpr std::size_t kBucketCount = static_cast<std::size_t>(Bucket::Count);

const char* bucketName(Bucket b) {
    switch (b) {
        case Bucket::One: return "1";
        case Bucket::Two: return "2";
        case Bucket::Three: return "3";
        case Bucket::Four: return "4";
        case Bucket::FiveSix: return "5-6";
        case Bucket::SevenEight: return "7-8";
        case Bucket::NineTen: return "9-10";
        case Bucket::ElevenPlus: return "11+";
        default: return "?";
    }
}

Bucket bucketFor(int frames) {
    if (frames <= 1) return Bucket::One;
    if (frames == 2) return Bucket::Two;
    if (frames == 3) return Bucket::Three;
    if (frames == 4) return Bucket::Four;
    if (frames <= 6) return Bucket::FiveSix;
    if (frames <= 8) return Bucket::SevenEight;
    if (frames <= 10) return Bucket::NineTen;
    return Bucket::ElevenPlus;
}

ccColor4F colorFor(Bucket bucket) {
        switch (bucket) {
        case Bucket::One: return {1.00f, 0.20f, 0.27f, 0.95f};
        case Bucket::Two: return {1.00f, 0.48f, 0.18f, 0.95f};
        case Bucket::Three: return {1.00f, 0.84f, 0.20f, 0.95f};
        case Bucket::Four: return {0.95f, 0.97f, 1.00f, 0.95f};
        case Bucket::FiveSix: return {0.30f, 0.92f, 0.47f, 0.95f};
        case Bucket::SevenEight: return {0.20f, 0.88f, 0.95f, 0.95f};
        case Bucket::NineTen: return {0.72f, 0.45f, 1.00f, 0.95f};
        case Bucket::ElevenPlus: return {0.34f, 0.53f, 1.00f, 0.95f};
        default: return {0.75f, 0.78f, 0.85f, 0.9f};
    }
}

struct Snapshot {
    bool ground1 = false;
    bool ground2 = false;
    bool ground3 = false;
    bool ground4 = false;
    bool slope = false;
    bool sliding = false;
    bool rotating = false;
    bool dead = false;
    bool jumpBuffered = false;
    int groundObject = -1;
    int slopeObject = -1;
    int ringContacts = 0;
    double gravity = 1.0;
};

struct Boundary {
    std::uint64_t tick = 0;
    float x = 0.f;
    float y = 0.f;
    bool groundRelated = false;
};

struct PendingInput {
    std::uint64_t tick = 0;
    float x = 0.f;
    float y = 0.f;
    bool down = true;
};

struct InputResult {
    std::uint64_t tick = 0;
    float x = 0.f;
    float y = 0.f;
    int frames = 0;
    bool down = true;
    bool known = false;
};

struct Counters {
    std::array<std::uint64_t, kBucketCount> bucket{};
    std::uint64_t total = 0;
    std::uint64_t unknown = 0;

    void clear() {
        bucket.fill(0);
        total = 0;
        unknown = 0;
    }

    void add(const InputResult& result) {
        if (!result.known) {
            ++unknown;
            return;
        }
        ++bucket[static_cast<std::size_t>(bucketFor(result.frames))];
        ++total;
    }

    std::uint64_t oneFrame() const { return bucket[static_cast<std::size_t>(Bucket::One)]; }
};

int objectID(GameObject const* object) {
    return object ? object->m_objectID : -1;
}

bool sameSnapshot(const Snapshot& a, const Snapshot& b) {
    return a.ground1 == b.ground1 && a.ground2 == b.ground2 &&
        a.ground3 == b.ground3 && a.ground4 == b.ground4 &&
        a.slope == b.slope && a.sliding == b.sliding &&
        a.rotating == b.rotating && a.dead == b.dead &&
        a.jumpBuffered == b.jumpBuffered &&
        a.groundObject == b.groundObject && a.slopeObject == b.slopeObject &&
        a.ringContacts == b.ringContacts &&
        std::fabs(a.gravity - b.gravity) < 1e-9;
}

bool groundChange(const Snapshot& a, const Snapshot& b) {
    return a.ground1 != b.ground1 || a.ground2 != b.ground2 ||
        a.ground3 != b.ground3 || a.ground4 != b.ground4 ||
        a.slope != b.slope || a.sliding != b.sliding ||
        a.groundObject != b.groundObject || a.slopeObject != b.slopeObject;
}

class Tracker {
public:
    static Tracker& get() {
        static Tracker instance;
        return instance;
    }

    void attach(PlayLayer* layer) {
        if (!layer) return;
        detach();
        m_layer = layer;
        resetAttempt();
        buildHud();
    }

    void detach() {
        if (m_layer) {
            if (m_draw) m_draw->removeFromParentAndCleanup(true);
            if (m_panel) m_panel->removeFromParentAndCleanup(true);
        }
        m_draw = nullptr;
        m_panel = nullptr;
        m_hud = nullptr;
        m_layer = nullptr;
        m_lastPlayer = nullptr;
        m_boundaries.clear();
        m_pending.clear();
        m_recent.clear();
        m_hasPrevious = false;
        m_tick = 0;
        m_counters.clear();
        m_lastHudText.clear();
    }

    void onButton(PlayLayer* layer, bool down, int button, bool isPlayer1) {
        if (!layer || layer != m_layer || !isPlayer1 || button != kJumpButton) return;
        if (!down && !setting<bool>("count-releases", false)) return;

        auto* player = layer->m_player1;
        if (!player || player->m_isDead) return;

        PendingInput event;
        event.tick = m_tick;
        event.x = player->m_position.x;
        event.y = player->m_position.y;
        event.down = down;
        m_pending.push_back(event);

        if (m_pending.size() > 512) m_pending.erase(m_pending.begin(), m_pending.begin() + 128);
    }

    void afterTick(PlayLayer* layer) {
        if (!layer || layer != m_layer) return;
        ++m_tick;

        auto* player = layer->m_player1;
        if (!player) {
            m_hasPrevious = false;
            m_lastPlayer = nullptr;
            return;
        }

        if (m_lastPlayer && player != m_lastPlayer) {
            // Respawn can replace the player object.
            m_hasPrevious = false;
            m_boundaries.clear();
            m_pending.clear();
        }
        m_lastPlayer = player;

        Snapshot current;
        current.ground1 = player->m_isOnGround;
        current.ground2 = player->m_isOnGround2;
        current.ground3 = player->m_isOnGround3;
        current.ground4 = player->m_isOnGround4;
        current.slope = player->m_isCollidingWithSlope;
        current.sliding = player->m_isSliding;
        current.rotating = player->m_isRotating;
        current.dead = player->m_isDead;
        current.jumpBuffered = player->m_jumpBuffered;
        current.groundObject = objectID(player->m_lastGroundObject);
        current.slopeObject = objectID(player->m_currentSlope);
        current.ringContacts = static_cast<int>(std::min<std::size_t>(player->m_touchedRings.size(), 64));
        current.gravity = std::isfinite(player->m_gravity) ? player->m_gravity : 1.0;

        const float x = player->m_position.x;
        const float y = player->m_position.y;

        if (!m_hasPrevious) {
            m_previous = current;
            m_hasPrevious = true;
            pushBoundary({m_tick, x, y, false});
        } else if (!sameSnapshot(m_previous, current)) {
            const bool isGround = groundChange(m_previous, current);
            m_previous = current;
            pushBoundary({m_tick, x, y, isGround});
        }

        resolvePending();
    }

    void postFrame() {
        updateHud();
        drawMarkers();
    }

    void clearCounters() {
        m_counters.clear();
        m_recent.clear();
        m_pending.clear();
        m_lastHudText.clear();
        updateHud();
        drawMarkers();
    }

    const Counters& counters() const { return m_counters; }
    const std::deque<InputResult>& recent() const { return m_recent; }
    std::uint64_t tick() const { return m_tick; }

    template <class T>
    static T setting(const char* key, T fallback) {
        auto* mod = Mod::get();
        if (!mod || !mod->hasSetting(key)) return fallback;
        return mod->getSettingValue<T>(key);
    }

private:
    void resetAttempt() {
        m_tick = 0;
        m_counters.clear();
        m_pending.clear();
        m_recent.clear();
        m_boundaries.clear();
        m_lastPlayer = nullptr;
        m_hasPrevious = false;
        m_lastHudText.clear();
    }

    void pushBoundary(const Boundary& boundary) {
        m_boundaries.push_back(boundary);
        while (m_boundaries.size() > kBoundaryCapacity) m_boundaries.pop_front();
    }

    void resolvePending() {
        for (auto it = m_pending.begin(); it != m_pending.end();) {
            if (m_tick < it->tick + kSearchRadius) {
                ++it;
                continue;
            }

            InputResult result;
            result.tick = it->tick;
            result.x = it->x;
            result.y = it->y;
            result.down = it->down;

            std::uint64_t bestDistance = static_cast<std::uint64_t>(kSearchRadius) + 1;
            for (const auto& boundary : m_boundaries) {
                const auto distance = boundary.tick > it->tick
                    ? boundary.tick - it->tick : it->tick - boundary.tick;
                if (distance <= static_cast<std::uint64_t>(kSearchRadius) && distance < bestDistance) {
                    bestDistance = distance;
                }
            }

            if (bestDistance <= static_cast<std::uint64_t>(kSearchRadius)) {
                result.known = true;
                result.frames = static_cast<int>(std::max<std::uint64_t>(1, bestDistance));
            }

            m_counters.add(result);
            m_recent.push_back(result);
            while (m_recent.size() > kRecentCapacity) m_recent.pop_front();
            it = m_pending.erase(it);
        }
    }

    void buildHud() {
        if (!m_layer) return;
        auto win = CCDirector::sharedDirector()->getWinSize();

        m_draw = CCDrawNode::create();
        if (m_draw) {
            m_layer->addChild(m_draw, 999999);
        }

        m_panel = CCLayerColor::create(ccColor4B{12, 17, 30, 218}, 250.f, 82.f);
        if (!m_panel) return;
        m_panel->setAnchorPoint({0.f, 1.f});
        m_panel->setPosition({12.f, win.height - 12.f});
        m_panel->ignoreAnchorPointForPosition(false);
        m_panel->setID("imadamnduck.framescope/hud-panel");
        auto* hudParent = CCDirector::sharedDirector()->getRunningScene();
        (hudParent ? hudParent : static_cast<CCNode*>(m_layer))->addChild(m_panel, 1000000);

        m_hud = CCLabelBMFont::create("IMADAMNDUCK  /  ESTIMATED", "bigFont.fnt");
        if (m_hud) {
            m_hud->setAnchorPoint({0.f, 1.f});
            m_hud->setScale(0.42f);
            m_hud->setPosition({10.f, 68.f});
            m_hud->setColor(ccColor3B{190, 215, 255});
            m_panel->addChild(m_hud);
        }
        updateHud();
        applyHudVisibility();
    }

    void applyHudVisibility() {
        const bool visible = setting<bool>("show-hud", true);
        if (m_panel) m_panel->setVisible(visible);
    }

    std::string hudText() const {
        std::ostringstream out;
        out << "IMADAMNDUCK  /  ESTIMATE\n";
        out << "INPUTS " << m_counters.total << "   1F " << m_counters.oneFrame()
            << "   UNKNOWN " << m_counters.unknown << "\n";
        out << "1:" << m_counters.bucket[0]
            << "  2:" << m_counters.bucket[1]
            << "  3:" << m_counters.bucket[2]
            << "  4:" << m_counters.bucket[3] << "\n";
        out << "5-6:" << m_counters.bucket[4]
            << "  7-8:" << m_counters.bucket[5]
            << "  9-10:" << m_counters.bucket[6]
            << "  11+:" << m_counters.bucket[7];
        return out.str();
    }

    void updateHud() {
        applyHudVisibility();
        if (!m_hud) return;
        const auto text = hudText();
        if (text != m_lastHudText) {
            m_hud->setString(text.c_str());
            m_lastHudText = text;
        }
    }

    void drawMarkers() {
        if (!m_draw) return;
        m_draw->clear();
        if (!setting<bool>("show-markers", true)) return;

        for (const auto& result : m_recent) {
            if (m_tick > result.tick + 96) continue;
            if (!result.known) continue;
            const auto bucket = bucketFor(result.frames);
            const float alpha = std::max(0.12f, 1.f - static_cast<float>(m_tick - result.tick) / 110.f);
            auto color = colorFor(bucket);
            color.a *= alpha;
            m_draw->drawDot({result.x, result.y}, 5.5f, color);
        }
    }

    PlayLayer* m_layer = nullptr;
    const PlayerObject* m_lastPlayer = nullptr;
    std::uint64_t m_tick = 0;
    Snapshot m_previous{};
    bool m_hasPrevious = false;

    std::deque<Boundary> m_boundaries;
    std::vector<PendingInput> m_pending;
    std::deque<InputResult> m_recent;
    Counters m_counters;

    CCDrawNode* m_draw = nullptr;
    CCLayerColor* m_panel = nullptr;
    CCLabelBMFont* m_hud = nullptr;
    std::string m_lastHudText;
};

class DuckPopup : public geode::Popup {
public:
    static DuckPopup* create() {
        auto* ret = new DuckPopup();
        if (ret && ret->init(370.f, 260.f, "GJ_square01.png")) {
            ret->autorelease();
            return ret;
        }
        CC_SAFE_DELETE(ret);
        return nullptr;
    }

    bool init(float width, float height, geode::ZStringView bg = "GJ_square01.png",
              cocos2d::CCRect bgRect = {}) {
        if (!geode::Popup::init(width, height, bg, bgRect)) return false;

        setTitle("IMADAMNDUCK", "goldFont.fnt", 0.72f, 20.f);
        const auto size = m_mainLayer->getContentSize();

        auto* subtitle = CCLabelBMFont::create("LIVE FRAME WINDOW TRACKER", "bigFont.fnt");
        if (subtitle) {
            subtitle->setScale(0.38f);
            subtitle->setColor(ccColor3B{112, 202, 255});
            subtitle->setPosition({size.width / 2.f, size.height - 48.f});
            m_mainLayer->addChild(subtitle, 3);
        }

        auto* surface = CCLayerColor::create(ccColor4B{10, 15, 28, 205}, 326.f, 138.f);
        if (surface) {
            surface->setPosition({(size.width - 326.f) / 2.f, 57.f});
            m_mainLayer->addChild(surface, 1);
        }

        m_content = CCLabelBMFont::create("", "bigFont.fnt");
        if (m_content) {
            m_content->setAnchorPoint({0.5f, 0.5f});
            m_content->setScale(0.43f);
            m_content->setPosition({size.width / 2.f, 126.f});
            m_content->setColor(ccColor3B{231, 238, 255});
            m_mainLayer->addChild(m_content, 2);
        }

        auto* menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        m_mainLayer->addChild(menu, 4);

        auto addButton = [&](const char* label, float x, SEL_MenuHandler handler) {
            auto* sprite = ButtonSprite::create(label, 72, 30, 0.55f, false, "goldFont.fnt", "GJ_button_02.png");
            auto* item = CCMenuItemSpriteExtra::create(sprite, this, handler);
            item->setPosition({x, 31.f});
            menu->addChild(item);
        };

        addButton("LIVE", 75.f, menu_selector(DuckPopup::onLive));
        addButton("RANGES", size.width / 2.f, menu_selector(DuckPopup::onRanges));
        addButton("HISTORY", size.width - 75.f, menu_selector(DuckPopup::onHistory));

        auto* clearSprite = ButtonSprite::create("CLEAR", 58, 25, 0.48f, false, "goldFont.fnt", "GJ_button_04.png");
        auto* clearItem = CCMenuItemSpriteExtra::create(
            clearSprite, this, menu_selector(DuckPopup::onClear)
        );
        clearItem->setPosition({size.width - 45.f, size.height - 49.f});
        menu->addChild(clearItem);

        auto* foot = CCLabelBMFont::create("240 TICK BASELINE  |  ESTIMATED WINDOWS", "bigFont.fnt");
        if (foot) {
            foot->setScale(0.30f);
            foot->setColor(ccColor3B{142, 156, 184});
            foot->setPosition({size.width / 2.f, 48.f});
            m_mainLayer->addChild(foot, 3);
        }

        refresh();
        return true;
    }

    void onLive(CCObject*) { m_tab = 0; refresh(); }
    void onRanges(CCObject*) { m_tab = 1; refresh(); }
    void onHistory(CCObject*) { m_tab = 2; refresh(); }
    void onClear(CCObject*) {
        Tracker::get().clearCounters();
        refresh();
    }

private:
    void refresh() {
        if (!m_content) return;
        const auto& counters = Tracker::get().counters();
        std::ostringstream out;

        if (m_tab == 0) {
            out << "SESSION OVERVIEW\n\n";
            out << "CLASSIFIED INPUTS       " << counters.total << "\n";
            out << "ONE-FRAME ESTIMATES     " << counters.oneFrame() << "\n";
            out << "NO NEARBY EDGE          " << counters.unknown << "\n\n";
            out << "LOGIC TICK              " << Tracker::get().tick() << "\n";
            out << "TRACKED BUTTON          PLAYER 1 / JUMP";
        } else if (m_tab == 1) {
            out << "WINDOW DISTRIBUTION\n\n";
            for (std::size_t i = 0; i < kBucketCount; ++i) {
                out << std::setw(5) << bucketName(static_cast<Bucket>(i))
                    << " frames       " << counters.bucket[i] << "\n";
            }
            out << "\nUNCLASSIFIED             " << counters.unknown;
        } else {
            out << "RECENT INPUTS  /  NEWEST FIRST\n\n";
            const auto& recent = Tracker::get().recent();
            int shown = 0;
            for (auto it = recent.rbegin(); it != recent.rend() && shown < 8; ++it, ++shown) {
                out << (it->down ? "PRESS     " : "RELEASE   ");
                if (it->known) {
                    out << it->frames << "F     " << std::fixed << std::setprecision(1)
                        << (240.0 / std::max(1, it->frames)) << " FPS";
                } else {
                    out << "NO EDGE";
                }
                out << "\n";
            }
            if (shown == 0) out << "No inputs classified yet.\n";
        }

        m_content->setString(out.str().c_str());
    }

    int m_tab = 0;
    CCLabelBMFont* m_content = nullptr;
};
} // namespace imadamnduck

// Take a sample after each physics step.
class $modify(DuckPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool dontCreateObjects) {
        if (!PlayLayer::init(level, dontCreateObjects)) return false;
        imadamnduck::Tracker::get().attach(this);
        return true;
    }

    void postUpdate(float dt) {
        imadamnduck::Tracker::get().postFrame();
        PlayLayer::postUpdate(dt);
    }

    void onExit() {
        imadamnduck::Tracker::get().detach();
        PlayLayer::onExit();
    }
};

// Watch jump input. Nothing is changed or blocked here.
class $modify(DuckInputHook, GJBaseGameLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("GJBaseGameLayer::handleButton", Priority::Early);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (auto* layer = PlayLayer::get()) {
            imadamnduck::Tracker::get().onButton(layer, down, button, isPlayer1);
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    // update() can run at render FPS, so sample here instead.
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
        if (auto* layer = PlayLayer::get()) {
            imadamnduck::Tracker::get().afterTick(layer);
        }
    }
};

class $modify(DuckPauseHook, PauseLayer) {
    $override
    void customSetup() {
        PauseLayer::customSetup();

        auto* menu = static_cast<CCMenu*>(this->getChildByID("right-button-menu"));
        if (!menu || menu->getChildByID("imadamnduck.framescope/open-button")) return;

        auto* button = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("DUCK"), this,
            menu_selector(DuckPauseHook::openDuckPanel)
        );
        button->setID("imadamnduck.framescope/open-button");
        menu->addChild(button);
        menu->updateLayout();
    }

    void openDuckPanel(CCObject*) {
        if (auto* popup = imadamnduck::DuckPopup::create()) popup->show();
    }
};
