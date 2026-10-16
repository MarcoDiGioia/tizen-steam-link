#include "tizen/player.h"

#include <cstring>

#include "core/log.h"

#include "samsung/wasm/elementary_audio_track_config.h"
#include "samsung/wasm/elementary_media_packet.h"
#include "samsung/wasm/elementary_video_track_config.h"
#include "samsung/wasm/operation_result.h"

namespace tsl {
namespace {

using namespace std::chrono_literals;

constexpr uint32_t kSampleRate = 48000;
// Largest Opus frame: 120 ms at 48 kHz.
constexpr int kMaxOpusSamples = 5760;
// If no audio arrived for this long while video plays, feed silence so the media
// element doesn't stall video waiting for audio.
constexpr auto kAudioStarvation = 100ms;
// How far audio timestamps may drift from the wall clock before they are pulled back (behind)
// or packets dropped (ahead). In the low latency modes the player's clock follows the
// timestamps appended, so audio running ahead made video frames count as late: with 150 ms
// ahead allowed, an HEVC stream on a Samsung monitor dropped about 100 frames at the start,
// with 40 ms about 20, and nothing audible changed.
const Player::Seconds kAudioMaxBehind(0.04);
const Player::Seconds kAudioMaxAhead(0.04);
constexpr auto kOpenTimeout = 5000ms;
// Most lost audio frames in a row to conceal: 80 ms of 10 ms frames.
constexpr uint32_t kMaxConcealedFrames = 8;

// The level follows the picture size times the frame rate.
const char *VideoMime(const IHS_StreamVideoConfig &config, uint32_t fps) {
    uint64_t samples = static_cast<uint64_t>(config.width) * config.height * fps;
    switch (config.codec) {
        case IHS_StreamVideoCodecH264: {
            // Level 4.2 allows 522240 macroblocks a second (1080p at 64 fps); 5.2 allows 2073600
            // (4K60, 1080p120). Steam's 4K H.264 is level 5.1.
            uint64_t macroblocks = ((config.width + 15) / 16) * ((config.height + 15) / 16) * static_cast<uint64_t>(fps);
            if (macroblocks > 522240) {
                return "video/mp4; codecs=\"avc1.640034\"";  // High, level 5.2
            }
            return "video/mp4; codecs=\"avc1.64002A\"";  // High, level 4.2
        }
        case IHS_StreamVideoCodecHEVC:
            // Level 5.1 allows 534773760 luma samples a second (4K60, 1080p120); 5.2 twice that.
            if (samples > 534773760ull) {
                return "video/mp4; codecs=\"hev1.1.6.L156.B0\"";  // Main, level 5.2 (4K120)
            }
            return "video/mp4; codecs=\"hev1.1.6.L153.B0\"";  // Main, level 5.1
        default:
            return nullptr;
    }
}

}  // namespace

Player::Player(const char *videoElementId, samsung::wasm::EmssLatencyMode latency)
    : element_(videoElementId),
      source_(latency, samsung::wasm::ElementaryMediaStreamSource::RenderingMode::kMediaElement),
      sourceListener_(this),
      videoListener_(this, true),
      audioListener_(this, false) {
    source_.SetListener(&sourceListener_);
}

Player::~Player() {
    if (opus_ != nullptr) opus_decoder_destroy(opus_);
}

void Player::SourceListener::OnSourceClosed() {
    std::lock_guard<std::mutex> lock(player_->mutex_);
    player_->sourceState_ = ReadyState::kClosed;
    player_->changed_.notify_all();
}

void Player::SourceListener::OnSourceOpenPending() {
    std::lock_guard<std::mutex> lock(player_->mutex_);
    player_->sourceState_ = ReadyState::kOpenPending;
    player_->changed_.notify_all();
}

void Player::SourceListener::OnSourceOpen() {
    std::lock_guard<std::mutex> lock(player_->mutex_);
    player_->sourceState_ = ReadyState::kOpen;
    player_->changed_.notify_all();
}

void Player::SourceListener::OnPipelineError(samsung::wasm::MediaPipelineError error, const char *message) {
    player_->pipelineErrors_++;
    player_->lastPipelineError_ = static_cast<int>(error);
    Log(IHS_LogLevelError, "Player", "Media pipeline error %d: %s", static_cast<int>(error),
        message != nullptr ? message : "");
}

void Player::TrackListener::OnAppendError(samsung::wasm::OperationResult result) {
    if (video_) player_->lastAppendError_ = static_cast<int>(result);
    Log(IHS_LogLevelWarn, "Player", "%s append error %d", video_ ? "Video" : "Audio", static_cast<int>(result));
}

void Player::TrackListener::OnTrackOpen() {
    std::lock_guard<std::mutex> lock(player_->mutex_);
    (video_ ? player_->videoOpen_ : player_->audioOpen_) = true;
    player_->changed_.notify_all();
}

void Player::TrackListener::OnTrackClosed(samsung::wasm::ElementaryMediaTrack::CloseReason reason) {
    Log(IHS_LogLevelInfo, "Player", "%s track closed, reason %d", video_ ? "Video" : "Audio", static_cast<int>(reason));
    if (video_) {
        player_->videoTrackCloses_++;
        player_->lastCloseReason_ = static_cast<int>(reason);
    }
    std::lock_guard<std::mutex> lock(player_->mutex_);
    (video_ ? player_->videoOpen_ : player_->audioOpen_) = false;
    if (video_) {
        player_->waitKeyframe_ = true;
        player_->lostReported_ = false;
    }
    player_->changed_.notify_all();
}

void Player::TrackListener::OnSessionIdChanged(samsung::wasm::SessionId sessionId) {
    (video_ ? player_->videoSession_ : player_->audioSession_) = sessionId;
    Log(IHS_LogLevelInfo, "Player", "%s session id %u", video_ ? "Video" : "Audio", static_cast<unsigned>(sessionId));
    if (video_) player_->videoSessionChanges_++;
    // A new track session starts over: the decoder needs a keyframe first.
    if (video_) {
        std::lock_guard<std::mutex> lock(player_->mutex_);
        player_->waitKeyframe_ = true;
        player_->lostReported_ = false;
    }
}

bool Player::WaitFor(std::chrono::milliseconds timeout, const std::function<bool()> &condition) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, condition);
}

Player::Seconds Player::Now() const { return std::chrono::steady_clock::now() - start_; }

bool Player::OpenSource(const IHS_StreamVideoConfig &config) {
    const char *mime = VideoMime(config, fps_);
    if (mime == nullptr) {
        Log(IHS_LogLevelError, "Player", "Unsupported video codec %d", config.codec);
        return false;
    }
    element_.SetSrc(&source_);
    if (!WaitFor(kOpenTimeout, [this] { return sourceState_ == ReadyState::kClosed; })) {
        Log(IHS_LogLevelError, "Player", "Media source did not attach");
        return false;
    }

    if (audioEnabled_) {
        auto audio = source_.AddTrack(samsung::wasm::ElementaryAudioTrackConfig{
                "audio/webm; codecs=\"pcm\"",
                {},
                samsung::wasm::SampleFormat::kS16,
                samsung::wasm::ChannelLayout::kStereo,
                kSampleRate,
        });
        if (audio) {
            audioTrack_ = std::move(*audio);
            audioTrack_.SetListener(&audioListener_);
        } else {
            Log(IHS_LogLevelWarn, "Player", "Could not add the audio track, continuing without audio");
            audioEnabled_ = false;
        }
    }

    auto video = source_.AddTrack(samsung::wasm::ElementaryVideoTrackConfig{
            mime,
            {},
            config.width,
            config.height,
            fps_,
            1,
    });
    if (!video) {
        Log(IHS_LogLevelError, "Player", "Could not add the video track (%s %ux%u)", mime, config.width,
            config.height);
        return false;
    }
    videoTrack_ = std::move(*video);
    videoTrack_.SetListener(&videoListener_);

    source_.Open([](samsung::wasm::OperationResult) {});
    if (!WaitFor(kOpenTimeout, [this] { return sourceState_ == ReadyState::kOpenPending ||
                                               sourceState_ == ReadyState::kOpen; })) {
        Log(IHS_LogLevelError, "Player", "Media source did not open");
        return false;
    }
    element_.Play([](samsung::wasm::OperationResult result) {
        if (result != samsung::wasm::OperationResult::kSuccess) {
            Log(IHS_LogLevelError, "Player", "Play() failed: %d", static_cast<int>(result));
        }
    });
    bool opened = WaitFor(kOpenTimeout, [this] { return videoOpen_ && (!audioEnabled_ || audioOpen_); });
    if (!opened) {
        Log(IHS_LogLevelError, "Player", "Tracks did not open (video=%d audio=%d)", videoOpen_.load(),
            audioOpen_.load());
        return false;
    }
    Log(IHS_LogLevelInfo, "Player", "Playing %s %ux%u, audio %s", mime, config.width, config.height,
        audioEnabled_ ? "on" : "off");
    return true;
}

int Player::VideoStart(const IHS_StreamVideoConfig &config) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        width_ = config.width;
        height_ = config.height;
        waitKeyframe_ = true;
        lostReported_ = false;
    }
    if (sourceOpened_) {
        // Same source, same tracks; the frames carry the new size in their SPS.
        return 0;
    }
    start_ = std::chrono::steady_clock::now();
    lastAudio_ = start_;
    if (!OpenSource(config)) {
        return -1;
    }
    sourceOpened_ = true;
    return 0;
}

IHS_StreamVideoSubmitResult Player::VideoFrame(const uint8_t *data, size_t len, bool keyframe) {
    if (!videoOpen_) {
        return IHS_StreamVideoSubmitOK;
    }
    uint32_t width, height;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (waitKeyframe_) {
            if (!keyframe) {
                // Report once: ihslib then resets and asks the host for a keyframe. Reporting
                // every frame would repeat that request for each frame until it arrives.
                if (lostReported_) return IHS_StreamVideoSubmitOK;
                lostReported_ = true;
                return IHS_StreamVideoSubmitReportLost;
            }
            waitKeyframe_ = false;
            lostReported_ = false;
        }
        width = width_;
        height = height_;
    }
    Seconds pts = Now();
    samsung::wasm::ElementaryMediaPacket packet{
            pts,
            pts,
            Seconds(1.0 / fps_),
            keyframe,
            len,
            data,
            width,
            height,
            fps_,
            1,
            videoSession_.load(),
    };
    auto appended = videoTrack_.AppendPacket(packet);
    if (!appended) {
        uint64_t failed = ++videoFailed_;
        lastAppendError_ = static_cast<int>(appended.operation_result);
        if (failed <= 5) {
            Log(IHS_LogLevelWarn, "Player", "Video append failed: result %d (%s frame of %zu bytes)",
                static_cast<int>(appended.operation_result), keyframe ? "key" : "delta", len);
        }
        std::lock_guard<std::mutex> lock(mutex_);
        waitKeyframe_ = true;
        lostReported_ = true;
        return IHS_StreamVideoSubmitReportLost;
    }
    videoAppended_++;
    FillSilenceIfStarved();
    return IHS_StreamVideoSubmitOK;
}

void Player::VideoStop() {
    std::lock_guard<std::mutex> lock(mutex_);
    waitKeyframe_ = true;
    lostReported_ = false;
}

int Player::AudioStart(const IHS_StreamAudioConfig &config) {
    if (config.codec != IHS_StreamAudioCodecOpus || config.channels < 1 || config.channels > 2) {
        Log(IHS_LogLevelWarn, "Player", "Unsupported audio codec=%d channels=%u", config.codec, config.channels);
        return -1;
    }
    std::lock_guard<std::mutex> lock(audioMutex_);
    if (opus_ != nullptr) opus_decoder_destroy(opus_);
    int err = OPUS_OK;
    opus_ = opus_decoder_create(static_cast<opus_int32>(config.frequency), static_cast<int>(config.channels), &err);
    if (err != OPUS_OK) {
        opus_ = nullptr;
        return -1;
    }
    channels_ = static_cast<int>(config.channels);
    pcm_.assign(kMaxOpusSamples * 2, 0);
    return 0;
}

void Player::AppendPcm(const int16_t *pcm, int samplesPerChannel) {
    Seconds duration(static_cast<double>(samplesPerChannel) / kSampleRate);
    // Keep audio timestamps near the wall clock. Behind: jump forward (a gap). Ahead, after
    // a burst of late packets: drop, so the backlog and the latency can't grow.
    Seconds now = Now();
    if (audioPts_ < now - kAudioMaxBehind) audioPts_ = now;
    if (audioPts_ > now + kAudioMaxAhead) {
        audioDropped_++;
        return;
    }
    samsung::wasm::ElementaryMediaPacket packet{
            audioPts_,
            audioPts_,
            duration,
            true,
            static_cast<size_t>(samplesPerChannel) * 2 * sizeof(int16_t),
            pcm,
            0,
            0,
            0,
            1,
            audioSession_.load(),
    };
    if (audioTrack_.AppendPacket(packet)) {
        audioPts_ += duration;
        audioAppended_++;
    } else {
        audioFailed_++;
    }
}

int Player::AudioPacket(const uint8_t *data, size_t len) {
    if (!audioOpen_) return 0;
    std::lock_guard<std::mutex> lock(audioMutex_);
    if (opus_ == nullptr) return 0;
    int samples = opus_decode(opus_, data, static_cast<opus_int32>(len), pcm_.data(), kMaxOpusSamples, 0);
    if (samples <= 0) return 0;
    lastSamples_ = samples;
    AppendDecoded(samples);
    lastAudio_ = std::chrono::steady_clock::now();
    return 0;
}

void Player::AudioLost(uint32_t frames) {
    if (!audioOpen_) return;
    std::lock_guard<std::mutex> lock(audioMutex_);
    if (opus_ == nullptr) return;
    // Opus conceals a lost frame when decoding without data. Past a few frames that sounds worse
    // than a gap, so longer losses stay gaps (AppendPcm moves the timestamps on).
    for (uint32_t i = 0; i < frames && i < kMaxConcealedFrames; i++) {
        int samples = opus_decode(opus_, nullptr, 0, pcm_.data(), lastSamples_, 0);
        if (samples <= 0) break;
        AppendDecoded(samples);
        audioConcealed_++;
    }
}

void Player::AppendDecoded(int samples) {
    if (channels_ == 1) {
        // The track is stereo; duplicate mono into both channels, back to front in place.
        for (int i = samples - 1; i >= 0; i--) {
            pcm_[i * 2 + 1] = pcm_[i];
            pcm_[i * 2] = pcm_[i];
        }
    }
    AppendPcm(pcm_.data(), samples);
}

void Player::FillSilenceIfStarved() {
    if (!audioOpen_) return;
    std::lock_guard<std::mutex> lock(audioMutex_);
    if (std::chrono::steady_clock::now() - lastAudio_ < kAudioStarvation) return;
    // Fill up to the wall clock, measured from where the audio timestamps are, not from
    // when the last packet arrived.
    static const std::vector<int16_t> silence(480 * 2, 0);  // 10 ms
    for (int i = 0; i < 20 && audioPts_ < Now(); i++) {
        AppendPcm(silence.data(), 480);
        silence_++;
    }
}

void Player::AudioStop() {
    std::lock_guard<std::mutex> lock(audioMutex_);
    if (opus_ != nullptr) {
        opus_decoder_destroy(opus_);
        opus_ = nullptr;
    }
}

Player::Counters Player::counters() const {
    Counters c;
    c.videoAppended = videoAppended_.load();
    c.videoFailed = videoFailed_.load();
    c.audioAppended = audioAppended_.load();
    c.audioFailed = audioFailed_.load();
    c.silenceFilled = silence_.load();
    c.audioDropped = audioDropped_.load();
    c.audioConcealed = audioConcealed_.load();
    c.videoSessionChanges = videoSessionChanges_.load();
    c.videoTrackCloses = videoTrackCloses_.load();
    c.lastCloseReason = lastCloseReason_.load();
    c.lastAppendError = lastAppendError_.load();
    c.pipelineErrors = pipelineErrors_.load();
    c.lastPipelineError = lastPipelineError_.load();
    return c;
}

}  // namespace tsl
