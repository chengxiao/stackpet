/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace stackpet {

enum class Wave : uint8_t { Square, Sine, Triangle, Noise };

struct Tone {
    uint16_t freqHz     = 0;    // 0 = rest (silence)
    uint16_t durationMs = 100;
    uint8_t volume      = 70;   // 0-100, relative loudness inside the speaker volume set by the user
    Wave wave           = Wave::Square;
    uint16_t slideToHz  = 0;    // optional linear glide target over the tone duration (0 = no glide)
};

enum class Sfx : uint8_t {
    Click,      // short UI click
    Start,      // game start jingle
    Tick,       // countdown / wheel tick
    Go,         // dramatic "go!" sting
    Success,    // positive short jingle
    Fail,       // sad trombone
    Caught,     // alarm "caught you!"
    Win,        // victory fanfare
    Reveal,     // result reveal sparkle
    Boop,       // cute robot boop
    MuTouRen,   // 3-note children chant melody for "木头人"
    RadarPing,  // sonar ping with two fading echoes (~0.5 s)
    Alarm,      // high-low two-tone siren, exactly 3 s
};

/**
 * @brief Asynchronous sound player for mooncake (non-xiaozhi) mode.
 *
 * In mooncake mode the xiaozhi AudioService is NOT running, so Application::PlaySound() must not be used.
 * This player writes 16-bit mono PCM directly to Board::GetAudioCodec() (AUDIO_OUTPUT_SAMPLE_RATE, 24 kHz)
 * from its own FreeRTOS task. All methods are thread-safe and return immediately (except waitIdle() and
 * shutdown()).
 *
 * Sounds are queued and played one after another. The queue holds at most kMaxQueuedItems sounds; when it is
 * full the oldest queued (not yet started) sound is dropped, so rapid repeated requests can never build up a
 * long backlog.
 */
class SoundPlayer {
public:
    static constexpr size_t kMaxQueuedItems = 8;

    static SoundPlayer& instance();

    void playTones(const std::vector<Tone>& tones);
    void playSfx(Sfx sfx);
    // stopAll() followed by playSfx(), atomically: the new effect starts right away instead of queueing up.
    void playSfxExclusive(Sfx sfx);

    // Play an embedded Ogg/Opus clip, e.g. Lang::Sounds::OGG_1 (zh-CN digit "一") from <assets/lang_config.h>.
    // Silence below about -35 dBFS at the start is skipped, and a silent tail longer than 60 ms is cut right
    // after the voice with a 30 ms fade-out (the zh-CN digit clips are ~0.97 s, of which ~0.3 s is voice).
    // The first play of a clip decodes it (tens of ms); the trimmed audio is cached, later plays start at once.
    void playOgg(std::string_view ogg);

    // Drop all queued sounds and stop the current one as soon as possible (within ~10 ms).
    void stopAll();

    // True while a sound is queued or its PCM is still being output, including the last ~60 ms that are
    // already in the I2S DMA buffers. False only when the queue is empty and the speaker has played every
    // sample, so "!isBusy()" means "the previous sound has really finished" (use it to pace a sequence).
    // Keeping the amplifier powered for 2 s after the last sound (to avoid pops) does NOT count as busy.
    bool isBusy();
    // Block until isBusy() becomes false or timeout. Returns true if idle.
    bool waitIdle(uint32_t timeoutMs);

    // Stop all sounds and power the speaker output down. Call from App::onClose(). Safe to call multiple
    // times; the next play*() powers the output up again. (The player task itself stays parked, idle.)
    void shutdown();
};

}  // namespace stackpet
