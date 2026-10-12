/*
 * SPDX-FileCopyrightText: 2026 StackPet
 *
 * SPDX-License-Identifier: MIT
 */
#include "sound_player.h"
#include "pcm_trim.h"
#include "tone_synth.h"
#include <hal/hal.h>
#include <board.h>
#include <audio_codec.h>
#include <ogg_demuxer.h>
#include <esp_opus_dec.h>
#include <esp_ae_rate_cvt.h>
#include <esp_audio_types.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mooncake_log.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>

namespace stackpet {

namespace {

const std::string_view _tag = "SoundPlayer";
static const char* TAG      = "SoundPlayer";

// Opus decoding needs a big stack (xiaozhi gives its opus task 24 KB). The stack lives in PSRAM, like the
// xiaozhi wake word encoder task, so it does not eat the ~130 KB of internal SRAM that is left.
constexpr uint32_t kTaskStackSize = 24 * 1024;
// Core 1, priority 4: the LVGL task also runs on core 1 at priority 3 (stackchan_display.cc), the mooncake
// main loop and Wi-Fi are on core 0. Being one step above LVGL means a long UI render can never starve the
// I2S writes (no underrun crackle), and it costs LVGL almost nothing: the task renders 10 ms of audio in well
// under a millisecond and then blocks in the I2S write until the DMA has room again.
constexpr UBaseType_t kTaskPriority = 4;
constexpr BaseType_t kTaskCore      = 1;
// Decoding a whole Ogg clip (first play only, tens of ms of CPU) is done below LVGL so the UI never hitches
constexpr UBaseType_t kDecodePriority = 2;

constexpr size_t kChunkSamples      = 240;   // 10 ms at 24 kHz: stop latency and write granularity
constexpr uint32_t kIdleOutputOffMs = 2000;  // power the amplifier down after this much silence
constexpr float kVoiceGain          = 0.85f; // embedded voice clips are mastered loud, leave some headroom
constexpr size_t kOggFeedBytes      = 1024;  // demux in small pieces so stopAll() is honoured quickly
constexpr uint32_t kMaxOpusFrameMs  = 120;
constexpr size_t kMaxCachedClips    = 8;
constexpr size_t kMaxCacheBytes     = 384 * 1024;  // PSRAM; a trimmed zh-CN digit is ~17 KB


struct Item {
    enum class Kind : uint8_t { Tones, Ogg };
    Kind kind = Kind::Tones;
    std::vector<Tone> tones;
    std::string_view ogg;
    uint32_t generation = 0;
};

struct State {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Item> queue;
    bool playing          = false;  // an item is being played or its tail is still draining from the DMA
    bool output_enabled   = false;  // mirror of the codec output state, owned by the task
    bool power_down       = false;  // request: disable the output as soon as the task is idle
    bool task_started     = false;
    uint32_t dropped      = 0;      // items dropped because the queue was full
    std::atomic<uint32_t> generation{0};  // bumped by stopAll()/shutdown(): queued and playing items are dropped
};

State& state()
{
    static State s;
    return s;
}

/* -------------------------------------------------------------------------- */
/*                              Ogg/Opus playback                             */
/* -------------------------------------------------------------------------- */
class OggPlayer {
public:
    explicit OggPlayer(int outputRate) : _output_rate(outputRate)
    {
    }

    ~OggPlayer()
    {
        close_decoder();
    }

    // Decoded, silence-trimmed PCM at the output rate, cached by clip. nullptr if the decode was aborted.
    // The returned buffer stays valid until the next prepare() call.
    template <typename Abort>
    const std::vector<int16_t>* prepare(std::string_view ogg, Abort aborted)
    {
        for (auto it = _cache.begin(); it != _cache.end(); ++it) {
            if (it->data == ogg.data() && it->size == ogg.size()) {
                if (it != _cache.begin()) {
                    // Keep the most recently used clip at the front
                    std::rotate(_cache.begin(), it, std::next(it));
                }
                return &_cache.front().pcm;
            }
        }

        std::vector<int16_t> pcm;
        vTaskPrioritySet(nullptr, kDecodePriority);
        bool complete = decode_all(ogg, aborted, pcm);
        vTaskPrioritySet(nullptr, kTaskPriority);
        if (!complete) {
            return nullptr;
        }

        auto range = pcm::findVoicedRange(pcm.data(), pcm.size(), _output_rate);
        std::vector<int16_t> trimmed;
        if (!range.silent) {
            trimmed.assign(pcm.begin() + range.begin, pcm.begin() + range.end);
            pcm::applyFadeOut(trimmed.data(), trimmed.size(), range.fadeOutSamples);
        }
        mclog::tagInfo(_tag, "ogg clip decoded: {} ms, playing {} ms after trimming", pcm.size() * 1000 / _output_rate,
                       trimmed.size() * 1000 / _output_rate);

        cache_insert(ogg, std::move(trimmed));
        return &_cache.front().pcm;
    }

private:
    struct CachedClip {
        const char* data = nullptr;
        size_t size      = 0;
        std::vector<int16_t> pcm;
    };

    void cache_insert(std::string_view ogg, std::vector<int16_t>&& pcm)
    {
        size_t bytes = pcm.size() * sizeof(int16_t);
        // Evict least recently used clips (at the back) until the new one fits
        while (!_cache.empty() && (_cache.size() >= kMaxCachedClips || _cache_bytes + bytes > kMaxCacheBytes)) {
            _cache_bytes -= _cache.back().pcm.size() * sizeof(int16_t);
            _cache.pop_back();
        }
        _cache.push_front(CachedClip{ogg.data(), ogg.size(), std::move(pcm)});
        _cache_bytes += bytes;
    }

    template <typename Abort>
    bool decode_all(std::string_view ogg, Abort aborted, std::vector<int16_t>& out)
    {
        if (ogg.empty()) {
            return true;
        }
        if (!_demuxer) {
            // ~8.5 KB of parser state, keep it out of internal RAM
            _demuxer = std::make_unique<OggDemuxer>();
        }

        _demuxer->Reset();
        _demuxer->OnDemuxerFinished([&](const uint8_t* data, int sampleRate, size_t len) {
            if (!aborted()) {
                decode_packet(data, len, sampleRate, out);
            }
        });

        const auto* buf = reinterpret_cast<const uint8_t*>(ogg.data());
        for (size_t offset = 0; offset < ogg.size() && !aborted(); offset += kOggFeedBytes) {
            _demuxer->Process(buf + offset, std::min(kOggFeedBytes, ogg.size() - offset));
        }
        _demuxer->OnDemuxerFinished(nullptr);
        return !aborted();
    }

    bool open_decoder(int sampleRate)
    {
        if (_decoder != nullptr && _decoder_rate == sampleRate) {
            return true;
        }
        close_decoder();

        esp_opus_dec_cfg_t cfg = {
            .sample_rate    = static_cast<uint32_t>(sampleRate),
            .channel        = ESP_AUDIO_MONO,
            .frame_duration = ESP_OPUS_DEC_FRAME_DURATION_60_MS,
            .self_delimited = false,
        };
        auto ret = esp_opus_dec_open(&cfg, sizeof(cfg), &_decoder);
        if (_decoder == nullptr) {
            mclog::tagError(_tag, "opus decoder open failed: {}", static_cast<int>(ret));
            return false;
        }
        _decoder_rate = sampleRate;
        _pcm.resize(static_cast<size_t>(sampleRate) * kMaxOpusFrameMs / 1000);

        if (_decoder_rate != _output_rate) {
            esp_ae_rate_cvt_cfg_t rate_cfg = {
                .src_rate        = static_cast<uint32_t>(_decoder_rate),
                .dest_rate       = static_cast<uint32_t>(_output_rate),
                .channel         = ESP_AUDIO_MONO,
                .bits_per_sample = ESP_AUDIO_BIT16,
                .complexity      = 2,
                .perf_type       = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
            };
            esp_ae_rate_cvt_open(&rate_cfg, &_resampler);
            if (_resampler == nullptr) {
                mclog::tagError(_tag, "resampler {} -> {} open failed", _decoder_rate, _output_rate);
            }
        }
        return true;
    }

    void close_decoder()
    {
        if (_decoder != nullptr) {
            esp_opus_dec_close(_decoder);
            _decoder = nullptr;
        }
        if (_resampler != nullptr) {
            esp_ae_rate_cvt_close(_resampler);
            _resampler = nullptr;
        }
        _decoder_rate = 0;
    }

    void decode_packet(const uint8_t* data, size_t len, int sampleRate, std::vector<int16_t>& out)
    {
        if (!open_decoder(sampleRate)) {
            return;
        }

        esp_audio_dec_in_raw_t raw = {
            .buffer        = const_cast<uint8_t*>(data),
            .len           = static_cast<uint32_t>(len),
            .consumed      = 0,
            .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
        };
        esp_audio_dec_out_frame_t frame = {
            .buffer       = reinterpret_cast<uint8_t*>(_pcm.data()),
            .len          = static_cast<uint32_t>(_pcm.size() * sizeof(int16_t)),
            .needed_size  = 0,
            .decoded_size = 0,
        };
        esp_audio_dec_info_t info = {};
        auto ret = esp_opus_dec_decode(_decoder, &raw, &frame, &info);
        if (ret != ESP_AUDIO_ERR_OK) {
            mclog::tagWarn(_tag, "opus decode failed: {}", static_cast<int>(ret));
            return;
        }

        size_t samples = frame.decoded_size / sizeof(int16_t);
        for (size_t i = 0; i < samples; i++) {
            _pcm[i] = static_cast<int16_t>(_pcm[i] * kVoiceGain);
        }

        if (_resampler == nullptr) {
            out.insert(out.end(), _pcm.begin(), _pcm.begin() + samples);
            return;
        }

        uint32_t max_out = 0;
        esp_ae_rate_cvt_get_max_out_sample_num(_resampler, samples, &max_out);
        _resampled.resize(max_out);
        uint32_t out_samples = max_out;
        esp_ae_rate_cvt_process(_resampler, reinterpret_cast<esp_ae_sample_t>(_pcm.data()), samples,
                                reinterpret_cast<esp_ae_sample_t>(_resampled.data()), &out_samples);
        out.insert(out.end(), _resampled.begin(), _resampled.begin() + out_samples);
    }

    int _output_rate                    = 24000;
    void* _decoder                      = nullptr;
    int _decoder_rate                   = 0;
    esp_ae_rate_cvt_handle_t _resampler = nullptr;
    std::unique_ptr<OggDemuxer> _demuxer;
    std::vector<int16_t> _pcm;
    std::vector<int16_t> _resampled;
    std::deque<CachedClip> _cache;  // most recently used first
    size_t _cache_bytes = 0;
};

/* -------------------------------------------------------------------------- */
/*                                 Player task                                */
/* -------------------------------------------------------------------------- */
void set_output(AudioCodec* codec, bool enabled)
{
    if (codec->output_enabled() != enabled) {
        codec->EnableOutput(enabled);
    }
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.output_enabled = enabled;
    s.cv.notify_all();
}

void player_task(void* arg)
{
    auto& s    = state();
    auto codec = Board::GetInstance().GetAudioCodec();
    const int rate = codec->output_sample_rate();

    synth::ToneRenderer renderer(rate);
    OggPlayer ogg_player(rate);
    std::vector<int16_t> chunk;
    chunk.reserve(kChunkSamples);
    bool wrote_audio = false;  // PCM was handed to the codec since the last time the DMA was drained
    // Audio already handed to the codec keeps playing for up to one full I2S DMA ring (6 x 240 frames,
    // 60 ms at 24 kHz) after the last write
    const uint32_t dma_drain_ms = AUDIO_CODEC_DMA_DESC_NUM * AUDIO_CODEC_DMA_FRAME_NUM * 1000 / rate + 10;

    mclog::tagInfo(_tag, "player task started, output {} Hz, dma drain {} ms", rate, dma_drain_ms);

    while (true) {
        Item item;
        bool has_item   = false;
        bool power_down = false;
        {
            std::unique_lock<std::mutex> lock(s.mutex);
            auto wake = [&]() { return !s.queue.empty() || s.power_down; };

            if (s.queue.empty() && wrote_audio) {
                // The end of the last sound is still in the I2S DMA ring: stay busy until it has been played,
                // unless the next sound is already waiting
                s.cv.wait_for(lock, std::chrono::milliseconds(dma_drain_ms), wake);
                wrote_audio = false;
            }

            if (s.queue.empty()) {
                s.playing = false;
                s.cv.notify_all();

                if (s.output_enabled) {
                    // Keep the amplifier on for a while so back-to-back sounds start without a pop
                    s.cv.wait_for(lock, std::chrono::milliseconds(kIdleOutputOffMs), wake);
                } else {
                    s.cv.wait(lock, wake);
                }
            }

            if (!s.queue.empty()) {
                item = std::move(s.queue.front());
                s.queue.pop_front();
                s.playing = true;
                has_item  = true;
            } else {
                // Idle timeout or explicit power down request
                power_down   = s.output_enabled || s.power_down;
                s.power_down = false;
            }
        }

        if (!has_item) {
            if (power_down) {
                set_output(codec, false);
            }
            continue;
        }

        if (item.generation != s.generation.load()) {
            continue;  // dropped by stopAll() while it was queued
        }
        if (!codec->output_enabled()) {
            set_output(codec, true);
        }

        auto aborted = [&]() { return item.generation != s.generation.load(); };
        auto output  = [&](const int16_t* pcm, size_t samples) {
            for (size_t offset = 0; offset < samples && !aborted(); offset += kChunkSamples) {
                size_t n = std::min(kChunkSamples, samples - offset);
                chunk.assign(pcm + offset, pcm + offset + n);
                codec->OutputData(chunk);
                wrote_audio = true;
            }
        };

        if (item.kind == Item::Kind::Tones) {
            int16_t buf[kChunkSamples];
            for (const auto& tone : item.tones) {
                renderer.begin(tone);
                size_t n = 0;
                while (!aborted() && (n = renderer.render(buf, kChunkSamples)) > 0) {
                    output(buf, n);
                }
                if (aborted()) {
                    break;
                }
            }
        } else {
            const auto* pcm = ogg_player.prepare(item.ogg, aborted);
            if (pcm != nullptr && !pcm->empty()) {
                output(pcm->data(), pcm->size());
            }
        }
    }

    (void)arg;
}

void ensure_started()
{
    auto& s = state();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.task_started) {
            return;
        }
        s.task_started = true;
    }

    // The xiaozhi Application (which normally restores the speaker volume) is not running in mooncake mode,
    // so push the user's saved volume to the codec before the first sound. Done here, in the caller's task:
    // it writes NVS, which must not happen from a task whose stack lives in PSRAM.
    GetHAL().setSpeakerVolume(GetHAL().getSpeakerVolume(), false);

    auto* stack = static_cast<StackType_t*>(heap_caps_malloc(kTaskStackSize, MALLOC_CAP_SPIRAM));
    auto* tcb   = static_cast<StaticTask_t*>(heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL));
    if (stack == nullptr || tcb == nullptr) {
        mclog::tagError(_tag, "failed to allocate player task memory");
        heap_caps_free(stack);
        heap_caps_free(tcb);
        std::lock_guard<std::mutex> lock(s.mutex);
        s.task_started = false;
        return;
    }

    // The task is never deleted: it parks on the condition variable when idle. That keeps the statically
    // allocated stack/TCB valid for the whole uptime (apps warm-reboot on close anyway).
    xTaskCreateStaticPinnedToCore(player_task, "stackpet_sound", kTaskStackSize, nullptr, kTaskPriority, stack, tcb,
                                  kTaskCore);
}

void enqueue(Item&& item, bool exclusive = false)
{
    ensure_started();
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.task_started) {
        return;
    }
    if (exclusive) {
        // Same as stopAll(), but in the same critical section so nothing can slip in between
        s.generation++;
        s.queue.clear();
    }
    while (s.queue.size() >= SoundPlayer::kMaxQueuedItems) {
        s.queue.pop_front();
        s.dropped++;
        ESP_LOGD(TAG, "queue full, dropped the oldest sound (%lu dropped so far)", (unsigned long)s.dropped);
    }
    item.generation = s.generation.load();
    s.power_down    = false;
    s.queue.push_back(std::move(item));
    s.cv.notify_all();
}

Item tones_item(std::vector<Tone> tones)
{
    Item item;
    item.kind  = Item::Kind::Tones;
    item.tones = std::move(tones);
    return item;
}

}  // namespace

SoundPlayer& SoundPlayer::instance()
{
    static SoundPlayer player;
    return player;
}

void SoundPlayer::playTones(const std::vector<Tone>& tones)
{
    if (tones.empty()) {
        return;
    }
    enqueue(tones_item(tones));
}

void SoundPlayer::playSfx(Sfx sfx)
{
    playTones(synth::sfxTones(sfx));
}

void SoundPlayer::playSfxExclusive(Sfx sfx)
{
    enqueue(tones_item(synth::sfxTones(sfx)), true);
}

void SoundPlayer::playOgg(std::string_view ogg)
{
    if (ogg.empty()) {
        return;
    }
    Item item;
    item.kind = Item::Kind::Ogg;
    item.ogg  = ogg;
    enqueue(std::move(item));
}

void SoundPlayer::stopAll()
{
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.generation++;
    s.queue.clear();
    s.cv.notify_all();
}

bool SoundPlayer::isBusy()
{
    auto& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.playing || !s.queue.empty();
}

bool SoundPlayer::waitIdle(uint32_t timeoutMs)
{
    auto& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    return s.cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&]() { return !s.playing && s.queue.empty(); });
}

void SoundPlayer::shutdown()
{
    auto& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    if (!s.task_started) {
        return;
    }
    s.generation++;
    s.queue.clear();
    s.power_down = true;
    s.cv.notify_all();

    // Wait until the current sound is aborted and the amplifier is off (bounded, never hang an onClose())
    bool done = s.cv.wait_for(lock, std::chrono::milliseconds(500), [&]() { return !s.playing && !s.output_enabled; });
    if (!done) {
        mclog::tagWarn(_tag, "shutdown timed out waiting for the player to go idle");
    }
}

}  // namespace stackpet
