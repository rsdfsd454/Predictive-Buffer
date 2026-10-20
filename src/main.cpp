// Predictive Input Buffer (Geode mod sketch)
//
// Idea: remember the exact player X position of every click that led to
// survival ("known-good targets"). If you press up to N physics steps BEFORE
// a known-good target, the press is held back and injected on the step where
// the player reaches that X. Release is deferred so the tap keeps its order.
//
// Matching by X position (not a frame counter) keeps it correct across
// checkpoints, practice-mode respawns and speed portals.
//
// NOTE: hook signatures follow the 2.2074 bindings. Check them against the
// Geode bindings for your GD version if the build complains.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

using namespace geode::prelude;

namespace buf {
    struct Press { float x; int steps = 0; };

    std::vector<float> good;     // committed target X positions
    std::vector<Press> pending;  // presses waiting to prove they were safe
    std::string levelKey;

    bool  queued = false;        // a press is being held back
    float queuedX = 0.f;
    bool  releaseWanted = false; // user released while press was held back
    bool  releaseNext = false;   // inject the release on the next step
    bool  injecting = false;     // true while WE call handleButton

    float lastX = 0.f;
    float dx = 1.3f;             // X distance per physics step (measured live)

    constexpr int COMMIT_AFTER_STEPS = 120; // ~0.5 s at 240 steps/s

    bool enabled()  { return Mod::get()->getSettingValue<bool>("enabled"); }
    bool learning() { return Mod::get()->getSettingValue<bool>("learn"); }
    int  maxEarly() { return (int)Mod::get()->getSettingValue<int64_t>("max-early-steps"); }

    void clearTransient() {
        pending.clear();
        queued = releaseWanted = releaseNext = false;
        lastX = 0.f;
    }

    void commit(float x) {
        for (float g : good) {
            if (std::fabs(g - x) < dx * 0.5f) return; // already known
        }
        good.push_back(x);
        std::sort(good.begin(), good.end());
    }
}

class $modify(BufferGJBL, GJBaseGameLayer) {
    void processCommands(float dt) {
        if (buf::enabled() && PlayLayer::get() && m_player1) {
            float x = m_player1->getPositionX();

            // measure per-step distance, ignore respawn jumps
            float d = x - buf::lastX;
            if (d > 0.01f && d < 10.f) buf::dx = d;
            buf::lastX = x;

            // age pending presses, commit the ones that survived
            for (auto& p : buf::pending) p.steps++;
            while (!buf::pending.empty() &&
                   buf::pending.front().steps >= buf::COMMIT_AFTER_STEPS) {
                if (buf::learning()) buf::commit(buf::pending.front().x);
                buf::pending.erase(buf::pending.begin());
            }

            // deferred release from last step
            if (buf::releaseNext) {
                buf::releaseNext = false;
                buf::injecting = true;
                GJBaseGameLayer::handleButton(false, 1, true);
                buf::injecting = false;
            }

            // fire the held-back press when the player reaches the target
            if (buf::queued && x >= buf::queuedX - buf::dx * 0.5f) {
                buf::queued = false;
                buf::injecting = true;
                GJBaseGameLayer::handleButton(true, 1, true);
                buf::injecting = false;
                buf::pending.push_back({x});
                if (buf::releaseWanted) {
                    buf::releaseWanted = false;
                    buf::releaseNext = true;
                }
            }
        }
        GJBaseGameLayer::processCommands(dt);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (buf::injecting || !buf::enabled() || !isPlayer1 || button != 1 ||
            !PlayLayer::get() || !m_player1) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        float x = m_player1->getPositionX();

        if (down) {
            if (buf::queued) return; // already holding one back

            float window = buf::dx * (float)buf::maxEarly();
            float best = -1.f;
            for (float g : buf::good) {
                float ahead = g - x;
                if (ahead >= buf::dx * 0.5f && ahead <= window) {
                    best = g; // good is sorted: first hit is the nearest
                    break;
                }
            }

            if (best >= 0.f) {
                buf::queued = true;
                buf::queuedX = best;
                buf::releaseWanted = false;
                return; // swallow the early press
            }
            buf::pending.push_back({x});
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }

        // release
        if (buf::queued) {
            buf::releaseWanted = true; // replay it right after the press
            return;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(BufferPL, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        buf::clearTransient();
        buf::levelKey = fmt::format("targets-{}", level->m_levelID.value());
        buf::good = Mod::get()->getSavedValue<std::vector<float>>(buf::levelKey);
        std::sort(buf::good.begin(), buf::good.end());
        return true;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        buf::clearTransient(); // presses before a death are never committed
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        PlayLayer::destroyPlayer(player, obj);
        buf::clearTransient();
    }

    void onQuit() {
        Mod::get()->setSavedValue(buf::levelKey, buf::good);
        PlayLayer::onQuit();
    }
};
