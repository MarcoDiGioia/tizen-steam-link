#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <cstdint>
#include <mutex>
#include <vector>

#include <opus.h>

#include "core/session.h"

#include "samsung/html/html_media_element.h"
#include "samsung/wasm/elementary_media_stream_source.h"
#include "samsung/wasm/elementary_media_stream_source_listener.h"
#include "samsung/wasm/elementary_media_track.h"
#include "samsung/wasm/elementary_media_track_listener.h"
#include "samsung/wasm/media_pipeline_error.h"
#include "samsung/wasm/operation_result.h"

namespace tsl {

// Hardware playback through Samsung's ElementaryMediaStreamSource (Tizen WASM Player) on a
// <video> element. Video goes in as Annex-B access units; audio is decoded from Opus to
// PCM here, as Moonlight for Tizen does.
//
// The source is opened once per page load, on the first VideoStart. The app reloads the
// page after a stream ends instead of tearing the source down.
class Player : public MediaSink {
public:
    struct Counters {
        uint64_t videoAppended = 0;
        uint64_t videoFailed = 0;
        uint64_t audioAppended = 0;
        uint64_t audioFailed = 0;
        uint64_t silenceFilled = 0;
        uint64_t audioDropped = 0;
    };

    // Must be constructed on the browser main thread. videoElementId names a <video>.
    explicit Player(const char *videoElementId);
    ~Player() override;

    void SetHevcSupported(bool supported) { hevc_ = supported; }
    void SetAudioEnabled(bool enabled) { audioEnabled_ = enabled; }

    bool SupportsHevc() override { return hevc_; }
    int VideoStart(const IHS_StreamVideoConfig &config) override;
    IHS_StreamVideoSubmitResult VideoFrame(const uint8_t *data, size_t len, bool keyframe) override;
    void VideoStop() override;
    int AudioStart(const IHS_StreamAudioConfig &config) override;
    int AudioPacket(const uint8_t *data, size_t len) override;
    void AudioStop() override;

    Counters counters() const;

    using Seconds = samsung::wasm::Seconds;

private:
    using ReadyState = samsung::wasm::ElementaryMediaStreamSource::ReadyState;

    class SourceListener : public samsung::wasm::ElementaryMediaStreamSourceListener {
    public:
        explicit SourceListener(Player *player) : player_(player) {}
        void OnSourceClosed() override;
        void OnSourceOpenPending() override;
        void OnSourceOpen() override;
        void OnPipelineError(samsung::wasm::MediaPipelineError error, const char *message) override;

    private:
        Player *player_;
    };

    class TrackListener : public samsung::wasm::ElementaryMediaTrackListener {
    public:
        TrackListener(Player *player, bool video) : player_(player), video_(video) {}
        void OnTrackOpen() override;
        void OnTrackClosed(samsung::wasm::ElementaryMediaTrack::CloseReason reason) override;
        void OnSessionIdChanged(samsung::wasm::SessionId sessionId) override;
        void OnAppendError(samsung::wasm::OperationResult result) override;

    private:
        Player *player_;
        bool video_;
    };

    bool OpenSource(const IHS_StreamVideoConfig &config);
    bool WaitFor(std::chrono::milliseconds timeout, const std::function<bool()> &condition);
    Seconds Now() const;
    void AppendPcm(const int16_t *pcm, int samplesPerChannel);
    void FillSilenceIfStarved();

    std::atomic<bool> hevc_{false};
    std::atomic<bool> audioEnabled_{true};

    std::mutex mutex_;
    std::condition_variable changed_;
    ReadyState sourceState_ = ReadyState::kDetached;
    bool sourceOpened_ = false;  // once per page
    std::atomic<bool> videoOpen_{false};
    std::atomic<bool> audioOpen_{false};
    std::atomic<samsung::wasm::SessionId> videoSession_{0};
    std::atomic<samsung::wasm::SessionId> audioSession_{0};

    samsung::html::HTMLMediaElement element_;
    samsung::wasm::ElementaryMediaStreamSource source_;
    SourceListener sourceListener_;
    TrackListener videoListener_;
    TrackListener audioListener_;
    samsung::wasm::ElementaryMediaTrack videoTrack_;
    samsung::wasm::ElementaryMediaTrack audioTrack_;

    std::chrono::steady_clock::time_point start_;
    uint32_t width_ = 0, height_ = 0, fps_ = 60;
    bool waitKeyframe_ = true;
    bool lostReported_ = false;

    std::mutex audioMutex_;
    OpusDecoder *opus_ = nullptr;
    int channels_ = 2;
    std::vector<int16_t> pcm_;
    Seconds audioPts_{0};
    std::chrono::steady_clock::time_point lastAudio_;

    std::atomic<uint64_t> videoAppended_{0}, videoFailed_{0}, audioAppended_{0}, audioFailed_{0}, silence_{0},
            audioDropped_{0};
};

}  // namespace tsl
