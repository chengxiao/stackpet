/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "stackpet_games_check.h"
#include <stackpet/games/game_mood.h>
#include <cstring>
#include <vector>

using namespace stackpet::games;

namespace {

const FreezePhase kFreezePhases[] = {FreezePhase::Idle,     FreezePhase::Ready,      FreezePhase::Chanting,
                                     FreezePhase::TurningBack, FreezePhase::Settling, FreezePhase::Watching,
                                     FreezePhase::Survived, FreezePhase::Caught,     FreezePhase::PlayerWin,
                                     FreezePhase::CaughtWait};
const SpinPhase kSpinPhases[]   = {SpinPhase::Idle,     SpinPhase::SpinUp, SpinPhase::Cruise,
                                   SpinPhase::SlowDown, SpinPhase::Settle, SpinPhase::Reveal};
const SpinTone kSpinTones[]     = {SpinTone::Pick, SpinTone::Food, SpinTone::Truth, SpinTone::Dare,
                                   SpinTone::Yes,  SpinTone::No,   SpinTone::Joke};

// Rules every script must follow: valid emotions, only the last beat may hold, no beat too short to see
void checkScript(const std::vector<MoodBeat>& beats)
{
    for (size_t i = 0; i < beats.size(); i++) {
        int value = static_cast<int>(beats[i].emotion);
        CHECK(value >= 0 && value < stackchan::avatar::kEmotionCount);
        bool last = i + 1 == beats.size();
        if (last) {
            CHECK(beats[i].durationMs == 0);
        } else {
            CHECK(beats[i].durationMs >= kMinBeatMs);
        }
    }
}

Emotion holdOf(const std::vector<MoodBeat>& beats)
{
    return beats.back().emotion;
}

void testMoodScriptPlaysBeatsInOrder()
{
    MoodScript script;
    Emotion e = Emotion::Neutral;
    CHECK(!script.active());
    CHECK(!script.update(0, e));

    script.play({{Emotion::Surprised, 600}, {Emotion::Angry, 1400}, {Emotion::Laughing, 0}}, 1000);
    CHECK(script.active());
    CHECK(script.update(1000, e));
    CHECK(e == Emotion::Surprised);
    CHECK(!script.update(1300, e));  // no change, no repeated setEmotion
    CHECK(script.update(1600, e));
    CHECK(e == Emotion::Angry);
    CHECK(!script.update(2999, e));
    CHECK(script.update(3000, e));
    CHECK(e == Emotion::Laughing);
    CHECK(!script.update(60000, e));  // the last beat holds

    // A new script that starts with the shown emotion does not report a change (no needless blink)
    script.play({{Emotion::Laughing, 500}, {Emotion::Love, 0}}, 70000);
    CHECK(!script.update(70000, e));
    CHECK(script.update(70500, e));
    CHECK(e == Emotion::Love);

    // An empty script keeps the current one
    script.play({}, 80000);
    CHECK(!script.update(80000, e));
    CHECK(script.active());

    // A script made only of timed beats holds its last beat
    script.play({{Emotion::Sad, 400}, {Emotion::Shy, 400}}, 90000);
    CHECK(script.update(90000, e) && e == Emotion::Sad);
    CHECK(script.update(90400, e) && e == Emotion::Shy);
    CHECK(!script.update(99999, e));
}

void testFreezeMoods()
{
    for (auto phase : kFreezePhases) {
        auto beats = freezeMood(phase);
        CHECK(!beats.empty());
        checkScript(beats);
    }
    CHECK(holdOf(freezeMood(FreezePhase::Idle)) == Emotion::Neutral);
    CHECK(holdOf(freezeMood(FreezePhase::Chanting)) == Emotion::Laughing);  // cheeky, eyes shut while counting
    CHECK(holdOf(freezeMood(FreezePhase::Watching)) == Emotion::Doubt);
    CHECK(holdOf(freezeMood(FreezePhase::Survived)) == Emotion::Love);
    CHECK(holdOf(freezeMood(FreezePhase::CaughtWait)) == Emotion::Doubt);

    // Caught: startled first, then angry, ending with a taunt
    auto caught = freezeMood(FreezePhase::Caught);
    CHECK(caught.size() == 3);
    CHECK(caught[0].emotion == Emotion::Surprised);
    CHECK(caught[1].emotion == Emotion::Angry);
    CHECK(caught[2].emotion == Emotion::Laughing);
    // The taunt starts before the caught screen ends (caughtMs), so it is actually seen
    CHECK(caught[0].durationMs + caught[1].durationMs + kMinBeatMs <= FreezeConfig{}.caughtMs);

    auto win = freezeMood(FreezePhase::PlayerWin);
    CHECK(win.front().emotion == Emotion::Surprised);
    CHECK(holdOf(win) == Emotion::Love);
}

void testWatchMoodHysteresis()
{
    WatchMood mood;
    mood.reset(0);
    CHECK(mood.current() == Emotion::Doubt);
    CHECK(mood.update(0.9f, 100) == Emotion::Doubt);  // dwell after reset
    CHECK(mood.update(0.5f, 600) == Emotion::Doubt);  // below the alert level
    CHECK(mood.update(0.65f, 700) == Emotion::Surprised);
    CHECK(mood.update(0.1f, 900) == Emotion::Surprised);  // dwell: too soon to calm down
    CHECK(mood.current() == Emotion::Surprised);
    CHECK(mood.update(0.5f, 1300) == Emotion::Surprised);  // hysteresis: 0.5 does not calm down
    CHECK(mood.update(0.3f, 1400) == Emotion::Doubt);
    CHECK(mood.update(0.59f, 2000) == Emotion::Doubt);

    // A noisy level around the threshold changes the face at most once per dwell time
    mood.reset(10000);
    int changes  = 0;
    Emotion last = mood.current();
    for (uint32_t t = 10000; t < 15000; t += 66) {
        float level = (t / 66) % 2 ? 0.9f : 0.0f;
        Emotion now = mood.update(level, t);
        if (now != last) {
            changes++;
            last = now;
        }
    }
    CHECK(changes <= static_cast<int>(5000 / WatchMood::kDwellMs) + 1);
    CHECK(changes > 0);
}

void testSpinMoods()
{
    for (auto phase : kSpinPhases) {
        checkScript(spinMood(phase));
    }
    CHECK(holdOf(spinMood(SpinPhase::Idle)) == Emotion::Neutral);
    auto up = spinMood(SpinPhase::SpinUp);
    CHECK(up.front().emotion == Emotion::Surprised);
    CHECK(holdOf(up) == Emotion::Laughing);
    CHECK(holdOf(spinMood(SpinPhase::SlowDown)) == Emotion::Doubt);
    // These keep whatever is playing (the "whoa" of SpinUp must not be cut by Cruise)
    CHECK(spinMood(SpinPhase::Cruise).empty());
    CHECK(spinMood(SpinPhase::Settle).empty());
    CHECK(spinMood(SpinPhase::Reveal).empty());

    for (auto tone : kSpinTones) {
        auto beats = spinRevealMood(tone);
        CHECK(!beats.empty());
        checkScript(beats);
        CHECK(beats.front().emotion == Emotion::Surprised);  // ta-da
    }
    CHECK(holdOf(spinRevealMood(SpinTone::Pick)) == Emotion::Laughing);
    CHECK(holdOf(spinRevealMood(SpinTone::Food)) == Emotion::Love);
    CHECK(holdOf(spinRevealMood(SpinTone::Truth)) == Emotion::Shy);
    CHECK(holdOf(spinRevealMood(SpinTone::Dare)) == Emotion::Laughing);
    CHECK(holdOf(spinRevealMood(SpinTone::Yes)) == Emotion::Love);
    CHECK(holdOf(spinRevealMood(SpinTone::No)) == Emotion::Doubt);
    CHECK(holdOf(spinRevealMood(SpinTone::Joke)) == Emotion::Laughing);
}

void testSpinResultTones()
{
    const auto& cats = spinCategories();
    CHECK(spinResultTone(0, -1) == SpinTone::Pick);
    CHECK(spinResultTone(1, 3) == SpinTone::Food);
    CHECK(spinResultTone(2, 0) == SpinTone::Truth);
    CHECK(spinResultTone(3, 5) == SpinTone::Dare);
    CHECK(spinResultTone(99, 0) == SpinTone::Pick);  // out of range category

    // 做决定: every answer has its own tone, about yes 6 : no 5 : joke 3
    const auto& decide = cats[4];
    CHECK(std::strcmp(decide.name, "做决定") == 0);
    CHECK(decide.itemTones.size() == decide.items.size());
    int yes = 0, no = 0, joke = 0;
    for (size_t i = 0; i < decide.items.size(); i++) {
        auto tone = spinResultTone(4, static_cast<int>(i));
        yes += tone == SpinTone::Yes;
        no += tone == SpinTone::No;
        joke += tone == SpinTone::Joke;
    }
    CHECK(yes == 6 && no == 5 && joke == 3);
    CHECK(spinResultTone(4, 0) == SpinTone::Yes);    // 冲就完了！
    CHECK(spinResultTone(4, 7) == SpinTone::No);     // 算了吧
    CHECK(spinResultTone(4, 13) == SpinTone::Joke);  // 先吃饭再说
    CHECK(std::strcmp(decide.items[7], "算了吧") == 0);
    CHECK(std::strcmp(decide.items[13], "先吃饭再说") == 0);

    // Categories without per-item tones use one tone for everything
    for (size_t c = 0; c < 4; c++) {
        CHECK(cats[c].itemTones.empty());
    }
}

}  // namespace

int main()
{
    RUN_TEST(testMoodScriptPlaysBeatsInOrder);
    RUN_TEST(testFreezeMoods);
    RUN_TEST(testWatchMoodHysteresis);
    RUN_TEST(testSpinMoods);
    RUN_TEST(testSpinResultTones);
    std::cout << stackpet_test::checks() << " checks passed\n";
    return 0;
}
