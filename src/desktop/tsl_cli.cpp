// Desktop harness for the shared core: talks to Steam on the LAN the same way the TV app
// does, and writes the stream to files instead of a screen.
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <opus.h>

#include "core/client.h"
#include "core/identity.h"
#include "core/log.h"
#include "core/session.h"

namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

std::atomic<bool> g_interrupted{false};

void OnSignal(int) { g_interrupted = true; }

std::string ConfigDir() {
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    std::string base = xdg != nullptr && *xdg ? xdg : std::string(std::getenv("HOME")) + "/.config";
    return base + "/tizen-steam-link";
}

bool EnsureDir(const std::string &path) {
    std::string partial;
    std::stringstream ss(path);
    std::string part;
    if (!path.empty() && path[0] == '/') partial = "/";
    while (std::getline(ss, part, '/')) {
        if (part.empty()) continue;
        partial += part + "/";
        if (mkdir(partial.c_str(), 0700) != 0 && errno != EEXIST) return false;
    }
    return true;
}

std::optional<tsl::Identity> LoadOrCreateIdentity(const std::string &dir) {
    std::string path = dir + "/identity";
    std::ifstream in(path);
    if (in) {
        std::string line;
        std::getline(in, line);
        if (auto id = tsl::Identity::Parse(line)) return id;
        std::fprintf(stderr, "Ignoring unreadable identity file %s\n", path.c_str());
    }
    uint8_t random[tsl::Identity::kRandomBytes];
    if (getrandom(random, sizeof(random), 0) != static_cast<ssize_t>(sizeof(random))) {
        std::perror("getrandom");
        return std::nullopt;
    }
    char hostname[64] = "Linux";
    gethostname(hostname, sizeof(hostname) - 1);
    tsl::Identity id = tsl::Identity::FromRandom(random, std::string("tsl-cli on ") + hostname);
    if (!EnsureDir(dir)) {
        std::perror("mkdir");
        return std::nullopt;
    }
    std::ofstream out(path, std::ios::trunc);
    out << id.Serialize() << "\n";
    if (!out) {
        std::fprintf(stderr, "Can't write %s\n", path.c_str());
        return std::nullopt;
    }
    chmod(path.c_str(), 0600);
    return id;
}

struct Paired {
    uint64_t steamId = 0;
    tsl::HostKey key{};
};

// ip -> steam id and the key negotiated when pairing. One "ip steamid keyhex" line each.
std::map<std::string, Paired> LoadPaired(const std::string &dir) {
    std::map<std::string, Paired> paired;
    std::ifstream in(dir + "/paired");
    std::string ip, keyHex;
    uint64_t steamId;
    while (in >> ip >> steamId >> keyHex) {
        Paired p;
        p.steamId = steamId;
        if (tsl::HexDecode(keyHex, p.key.data(), p.key.size())) paired[ip] = p;
    }
    return paired;
}

void SavePaired(const std::string &dir, const std::map<std::string, Paired> &paired) {
    std::string path = dir + "/paired";
    std::ofstream out(path, std::ios::trunc);
    for (const auto &[ip, p] : paired) {
        out << ip << " " << p.steamId << " " << tsl::HexEncode(p.key.data(), p.key.size()) << "\n";
    }
    out.close();
    chmod(path.c_str(), 0600);
}

// Collects client events for a main thread that waits on them.
class Events : public tsl::Client::Listener {
public:
    struct Pairing {
        bool done = false;
        bool ok = false;
        uint64_t steamId = 0;
        tsl::HostKey key{};
        IHS_AuthorizationResult result = IHS_AuthorizationFailed;
    };
    struct Streaming {
        bool done = false;
        bool ok = false;
        IHS_SessionInfo info{};
        IHS_StreamingResult result = IHS_StreamingFailed;
    };

    void OnHostFound(const tsl::Host &host) override {
        std::lock_guard<std::mutex> lock(mutex_);
        auto [it, inserted] = hosts_.insert_or_assign(host.address(), host);
        if (inserted && printHosts_) {
            const IHS_HostInfo &h = it->second.info;
            std::printf("%-15s  %-24s  os=%d  universe=%d  games_running=%s\n", host.address().c_str(), h.hostname,
                        h.ostype, h.universe, h.gamesRunning ? "yes" : "no");
            std::fflush(stdout);
        }
        cond_.notify_all();
    }
    void OnPairProgress(const tsl::Host &) override {
        std::printf("Waiting for the PIN to be entered on the host...\n");
        std::fflush(stdout);
    }
    void OnPairSucceeded(const tsl::Host &, uint64_t steamId, const tsl::HostKey &key) override {
        std::lock_guard<std::mutex> lock(mutex_);
        pairing_ = {true, true, steamId, key, IHS_AuthorizationSuccess};
        cond_.notify_all();
    }
    void OnPairFailed(const tsl::Host &, IHS_AuthorizationResult result) override {
        std::lock_guard<std::mutex> lock(mutex_);
        pairing_ = {true, false, 0, {}, result};
        cond_.notify_all();
    }
    void OnStreamProgress(const tsl::Host &) override {
        std::printf("Host is preparing the stream...\n");
        std::fflush(stdout);
    }
    void OnStreamReady(const tsl::Host &, const IHS_SessionInfo &info) override {
        std::lock_guard<std::mutex> lock(mutex_);
        streaming_.done = true;
        streaming_.ok = true;
        streaming_.info = info;
        cond_.notify_all();
    }
    void OnStreamFailed(const tsl::Host &, IHS_StreamingResult result) override {
        std::lock_guard<std::mutex> lock(mutex_);
        streaming_.done = true;
        streaming_.ok = false;
        streaming_.result = result;
        cond_.notify_all();
    }

    void set_print_hosts(bool print) { printHosts_ = print; }

    std::optional<tsl::Host> WaitForHost(const std::string &ip, Clock::duration timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        bool found = cond_.wait_for(lock, timeout, [&] { return g_interrupted || hosts_.count(ip) > 0; });
        if (!found || g_interrupted) return std::nullopt;
        return hosts_.at(ip);
    }

    Pairing WaitForPairing(Clock::duration timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait_for(lock, timeout, [&] { return g_interrupted || pairing_.done; });
        return pairing_;
    }

    Streaming WaitForStream(Clock::duration timeout) {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait_for(lock, timeout, [&] { return g_interrupted || streaming_.done; });
        return streaming_;
    }

    size_t host_count() {
        std::lock_guard<std::mutex> lock(mutex_);
        return hosts_.size();
    }

private:
    std::mutex mutex_;
    std::condition_variable cond_;
    std::map<std::string, tsl::Host> hosts_;
    Pairing pairing_;
    Streaming streaming_;
    bool printHosts_ = false;
};

// Writes the raw video elementary stream and decodes Opus into a WAV file.
class FileSink : public tsl::MediaSink, public tsl::Session::Listener {
public:
    FileSink(std::string dir, bool hevc) : dir_(std::move(dir)), hevc_(hevc) {}
    ~FileSink() override { CloseAudio(); }

    bool SupportsHevc() override { return hevc_; }

    int VideoStart(const IHS_StreamVideoConfig &config) override {
        const char *ext = config.codec == IHS_StreamVideoCodecHEVC ? "hevc" : "h264";
        if (config.codec != IHS_StreamVideoCodecH264 && config.codec != IHS_StreamVideoCodecHEVC) {
            std::fprintf(stderr, "Unsupported video codec %d\n", config.codec);
            return -1;
        }
        videoPath_ = dir_ + "/video." + ext;
        video_ = std::fopen(videoPath_.c_str(), "wb");
        return video_ != nullptr ? 0 : -1;
    }

    IHS_StreamVideoSubmitResult VideoFrame(const uint8_t *data, size_t len, bool keyframe) override {
        if (video_ == nullptr) return IHS_StreamVideoSubmitError;
        // A decoder can only start at a keyframe; drop what comes before the first one.
        if (!sawKeyframe_ && !keyframe) return IHS_StreamVideoSubmitOK;
        sawKeyframe_ = true;
        std::fwrite(data, 1, len, video_);
        return IHS_StreamVideoSubmitOK;
    }

    void VideoStop() override {
        if (video_ != nullptr) std::fclose(video_);
        video_ = nullptr;
    }

    int AudioStart(const IHS_StreamAudioConfig &config) override {
        if (config.codec != IHS_StreamAudioCodecOpus || config.channels < 1 || config.channels > 2) {
            std::fprintf(stderr, "Unsupported audio: codec=%d channels=%u\n", config.codec, config.channels);
            return -1;
        }
        int err = 0;
        opus_ = opus_decoder_create(static_cast<opus_int32>(config.frequency), static_cast<int>(config.channels),
                                    &err);
        if (err != OPUS_OK) return -1;
        channels_ = config.channels;
        rate_ = config.frequency;
        pcm_.resize(5760 * channels_);
        audioPath_ = dir_ + "/audio.wav";
        audio_ = std::fopen(audioPath_.c_str(), "wb");
        if (audio_ == nullptr) return -1;
        WriteWavHeader(0);
        return 0;
    }

    int AudioPacket(const uint8_t *data, size_t len) override {
        if (opus_ == nullptr || audio_ == nullptr) return -1;
        int samples = opus_decode(opus_, data, static_cast<opus_int32>(len), pcm_.data(), 5760, 0);
        if (samples < 0) {
            decodeErrors_++;
            return 0;
        }
        std::fwrite(pcm_.data(), sizeof(opus_int16), static_cast<size_t>(samples) * channels_, audio_);
        pcmBytes_ += static_cast<uint32_t>(samples) * channels_ * sizeof(opus_int16);
        return 0;
    }

    void AudioStop() override { CloseAudio(); }

    void OnSessionConnected() override {
        std::printf("Connected. Streaming...\n");
        std::fflush(stdout);
    }
    void OnSessionDisconnected() override {
        std::printf("Disconnected.\n");
        disconnected_ = true;
    }
    void OnSessionFinalized() override { finalized_ = true; }

    bool disconnected() const { return disconnected_; }
    const std::string &video_path() const { return videoPath_; }
    const std::string &audio_path() const { return audioPath_; }
    uint64_t decode_errors() const { return decodeErrors_; }

private:
    void WriteWavHeader(uint32_t dataBytes) {
        auto le32 = [](uint8_t *p, uint32_t v) {
            for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (8 * i));
        };
        auto le16 = [](uint8_t *p, uint16_t v) {
            p[0] = static_cast<uint8_t>(v);
            p[1] = static_cast<uint8_t>(v >> 8);
        };
        uint8_t h[44];
        std::memcpy(h, "RIFF", 4);
        le32(h + 4, 36 + dataBytes);
        std::memcpy(h + 8, "WAVEfmt ", 8);
        le32(h + 16, 16);
        le16(h + 20, 1);
        le16(h + 22, static_cast<uint16_t>(channels_));
        le32(h + 24, rate_);
        le32(h + 28, rate_ * channels_ * 2);
        le16(h + 32, static_cast<uint16_t>(channels_ * 2));
        le16(h + 34, 16);
        std::memcpy(h + 36, "data", 4);
        le32(h + 40, dataBytes);
        std::fseek(audio_, 0, SEEK_SET);
        std::fwrite(h, 1, sizeof(h), audio_);
        std::fseek(audio_, 0, SEEK_END);
    }

    void CloseAudio() {
        if (audio_ != nullptr) {
            WriteWavHeader(pcmBytes_);
            std::fclose(audio_);
            audio_ = nullptr;
        }
        if (opus_ != nullptr) {
            opus_decoder_destroy(opus_);
            opus_ = nullptr;
        }
    }

    std::string dir_;
    bool hevc_;
    FILE *video_ = nullptr;
    FILE *audio_ = nullptr;
    std::string videoPath_, audioPath_;
    bool sawKeyframe_ = false;
    OpusDecoder *opus_ = nullptr;
    uint32_t channels_ = 2, rate_ = 48000, pcmBytes_ = 0;
    std::vector<opus_int16> pcm_;
    uint64_t decodeErrors_ = 0;
    std::atomic<bool> disconnected_{false}, finalized_{false};
};

void Usage() {
    std::fprintf(stderr,
                 "Usage: tsl-cli [-v] <command> [args]\n"
                 "\n"
                 "  discover [--seconds N]           list Steam hosts that answer a broadcast\n"
                 "  probe <ip> [--seconds N]         ask one host directly (no broadcast)\n"
                 "  pair <ip> [--pin NNNN] [--wait S] pair this device; type the PIN into Steam on the host\n"
                 "  stream <ip> [--seconds N] [--out DIR] [--hevc] [--no-audio] [--max WxH] [--input-test]\n"
                 "              [--quality auto|fast|balanced|beautiful] [--bitrate KBPS] [--no-fec]\n"
                 "                                   stream and write video.h264/.hevc and audio.wav;\n"
                 "                                   --input-test plugs in a virtual pad and presses buttons\n"
                 "\n"
                 "State (device identity, paired hosts) lives in %s\n",
                 ConfigDir().c_str());
}

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> options;
    bool Has(const std::string &k) const { return options.count(k) > 0; }
    std::string Get(const std::string &k, const std::string &def) const {
        auto it = options.find(k);
        return it == options.end() ? def : it->second;
    }
};

Args ParseArgs(int argc, char **argv) {
    static const char *flags[] = {"--hevc", "--no-audio", "--input-test", "--no-fec", "-v"};
    Args args;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        bool isFlag = false;
        for (const char *f : flags) isFlag = isFlag || a == f;
        if (isFlag) {
            args.options[a] = "1";
        } else if (a.rfind("--", 0) == 0 && i + 1 < argc) {
            args.options[a] = argv[++i];
        } else {
            args.positional.push_back(a);
        }
    }
    return args;
}

std::optional<tsl::Host> FindHost(tsl::Client &client, Events &events, const std::string &ip, int seconds) {
    auto deadline = Clock::now() + std::chrono::seconds(seconds);
    while (Clock::now() < deadline && !g_interrupted) {
        client.Probe(ip);
        if (auto host = events.WaitForHost(ip, 1s)) return host;
    }
    return std::nullopt;
}

int CmdDiscover(const tsl::Identity &identity, const Args &args) {
    Events events;
    events.set_print_hosts(true);
    tsl::Client client(identity, &events);
    client.StartDiscovery(1000);
    int seconds = std::atoi(args.Get("--seconds", "3").c_str());
    for (int i = 0; i < seconds * 10 && !g_interrupted; i++) {
        if (i % 10 == 0) client.ProbeLocalBroadcasts();
        std::this_thread::sleep_for(100ms);
    }
    client.StopDiscovery();
    if (events.host_count() == 0) {
        std::fprintf(stderr, "No hosts answered. Is Steam running with Remote Play enabled?\n");
        return 1;
    }
    return 0;
}

int CmdProbe(const tsl::Identity &identity, const Args &args) {
    if (args.positional.size() < 2) return Usage(), 2;
    Events events;
    tsl::Client client(identity, &events);
    const std::string &ip = args.positional[1];
    auto host = FindHost(client, events, ip, std::atoi(args.Get("--seconds", "5").c_str()));
    if (!host) {
        std::fprintf(stderr, "No answer from %s\n", ip.c_str());
        return 1;
    }
    std::printf("%s answered: %s (os=%d universe=%d games_running=%s)\n", ip.c_str(), host->info.hostname,
                host->info.ostype, host->info.universe, host->info.gamesRunning ? "yes" : "no");
    return 0;
}

int CmdPair(const tsl::Identity &identity, const std::string &dir, const Args &args) {
    if (args.positional.size() < 2) return Usage(), 2;
    Events events;
    tsl::Client client(identity, &events);
    const std::string &ip = args.positional[1];
    auto host = FindHost(client, events, ip, 5);
    if (!host) {
        std::fprintf(stderr, "No answer from %s\n", ip.c_str());
        return 1;
    }
    std::string pin = args.Get("--pin", "");
    if (pin.empty()) {
        uint32_t r = 0;
        getrandom(&r, sizeof(r), 0);
        pin = tsl::FormatPin(r);
    }
    std::printf("Pairing with %s. Enter PIN %s in the dialog Steam shows on that computer.\n",
                host->info.hostname, pin.c_str());
    std::fflush(stdout);
    if (!client.Pair(*host, pin)) {
        std::fprintf(stderr, "Could not start pairing\n");
        return 1;
    }
    int wait = std::atoi(args.Get("--wait", "120").c_str());
    Events::Pairing result = events.WaitForPairing(std::chrono::seconds(wait > 0 ? wait : 120));
    if (!result.done) {
        client.CancelPair();
        std::fprintf(stderr, "Pairing timed out\n");
        return 1;
    }
    if (!result.ok) {
        std::fprintf(stderr, "Pairing failed: %s\n", tsl::AuthorizationResultName(result.result));
        return 1;
    }
    auto paired = LoadPaired(dir);
    paired[ip] = Paired{result.steamId, result.key};
    SavePaired(dir, paired);
    std::printf("Paired. Steam ID %llu\n", static_cast<unsigned long long>(result.steamId));
    return 0;
}

int CmdStream(const tsl::Identity &identity, const std::string &dir, const Args &args) {
    if (args.positional.size() < 2) return Usage(), 2;
    const std::string &ip = args.positional[1];
    std::string out = args.Get("--out", ".");
    if (!EnsureDir(out)) {
        std::perror("mkdir");
        return 1;
    }
    int seconds = std::atoi(args.Get("--seconds", "10").c_str());
    tsl::StreamRequest request;
    std::string max = args.Get("--max", "1920x1080");
    if (std::sscanf(max.c_str(), "%dx%d", &request.maxWidth, &request.maxHeight) != 2) {
        std::fprintf(stderr, "Bad --max %s\n", max.c_str());
        return 2;
    }

    auto paired = LoadPaired(dir);
    auto pairedIt = paired.find(ip);
    if (pairedIt == paired.end()) {
        std::fprintf(stderr, "%s is not paired with this device. Run: tsl-cli pair %s\n", ip.c_str(), ip.c_str());
        return 1;
    }
    const Paired &pairing = pairedIt->second;

    Events events;
    tsl::Client client(identity, &events);
    auto host = FindHost(client, events, ip, 5);
    if (!host) {
        std::fprintf(stderr, "No answer from %s\n", ip.c_str());
        return 1;
    }
    if (!client.RequestStream(*host, request, pairing.key)) {
        std::fprintf(stderr, "Could not send the streaming request\n");
        return 1;
    }
    Events::Streaming streaming = events.WaitForStream(30s);
    if (!streaming.done) {
        std::fprintf(stderr, "Streaming request timed out\n");
        return 1;
    }
    if (!streaming.ok) {
        std::fprintf(stderr, "Streaming request refused: %s\n", tsl::StreamingResultName(streaming.result));
        return 1;
    }

    IHS_SessionInfo info = streaming.info;
    info.steamId = pairing.steamId;

    tsl::SessionOptions options;
    options.audio = !args.Has("--no-audio");
    options.hevc = args.Has("--hevc");
    options.fec = !args.Has("--no-fec");
    static const std::map<std::string, int> qualities = {
            {"auto", -1}, {"fast", 1}, {"balanced", 2}, {"beautiful", 3}};
    std::string quality = args.Get("--quality", "balanced");
    if (!qualities.count(quality)) {
        std::fprintf(stderr, "Bad --quality %s\n", quality.c_str());
        return 2;
    }
    options.quality = qualities.at(quality);
    options.bitrateKbps = std::atoi(args.Get("--bitrate", "0").c_str());
    options.maxWidth = request.maxWidth;
    options.maxHeight = request.maxHeight;
    FileSink sink(out, options.hevc);
    tsl::Session session(identity, pairing.key, info, options, &sink, &sink);
    if (!session.Connect()) {
        std::fprintf(stderr, "Could not start the session\n");
        return 1;
    }
    // --input-test: plug in a virtual gamepad, press A, then D-pad down, so the effect shows
    // up in the recording (and in Steam's controller log).
    struct Step {
        Clock::duration at;
        const char *what;
        std::function<void()> run;
    };
    TSL_GamepadInfo padInfo{};
    std::snprintf(padInfo.name, sizeof(padInfo.name), "tsl-cli test pad");
    auto press = [&session](uint16_t buttons) {
        TSL_GamepadState state{};
        state.buttons = buttons;
        session.UpdateGamepad(0, state);
    };
    std::vector<Step> steps;
    if (args.Has("--input-test")) {
        steps = {
                {3s, "connect pad 0", [&] { session.SetGamepadConnected(0, true, &padInfo); }},
                {6s, "press A", [&] { press(1u << TSL_PAD_A); }},
                {6200ms, "release A", [&] { press(0); }},
                {10s, "press D-pad down", [&] { press(1u << TSL_PAD_DPAD_DOWN); }},
                {10200ms, "release D-pad down", [&] { press(0); }},
                {13s, "key Escape", [&] { session.SendKey(41, true); session.SendKey(41, false); }},
        };
    }
    size_t nextStep = 0;
    auto start = Clock::now();
    while (!g_interrupted && !sink.disconnected() && Clock::now() - start < std::chrono::seconds(seconds)) {
        auto elapsed = Clock::now() - start;
        while (nextStep < steps.size() && session.connected() && elapsed >= steps[nextStep].at) {
            std::printf("[%5.1fs] %s\n", std::chrono::duration<double>(elapsed).count(), steps[nextStep].what);
            std::fflush(stdout);
            steps[nextStep++].run();
        }
        std::this_thread::sleep_for(20ms);
    }
    session.Disconnect();
    session.Join();

    tsl::SessionStats s = session.Stats();
    double elapsed = std::chrono::duration<double>(Clock::now() - start).count();
    std::printf("Video: %llu frames (%llu keyframes), %.1f MB, %ux%u, ~%.1f Mbit/s\n",
                static_cast<unsigned long long>(s.videoFrames), static_cast<unsigned long long>(s.keyFrames),
                s.videoBytes / 1e6, s.width, s.height, elapsed > 0 ? s.videoBytes * 8 / elapsed / 1e6 : 0.0);
    std::printf("Audio: %llu packets, %llu bytes, %llu decode errors\n",
                static_cast<unsigned long long>(s.audioPackets), static_cast<unsigned long long>(s.audioBytes),
                static_cast<unsigned long long>(sink.decode_errors()));
    uint32_t sent = s.videoPackets + s.videoPacketsLost;
    std::printf("Network: %u video packets, %u lost (%.2f%%), FEC %s (%u rebuilt, %u lost), "
                "%u keyframe requests, round trip %.1f ms\n",
                s.videoPackets, s.videoPacketsLost, sent > 0 ? 100.0 * s.videoPacketsLost / sent : 0.0,
                s.fec ? "on" : "off", s.fecRecovered, s.fecUnrecoverable, s.lostReports, s.roundTripMs);
    if (!sink.video_path().empty()) std::printf("Wrote %s\n", sink.video_path().c_str());
    if (!sink.audio_path().empty()) std::printf("Wrote %s\n", sink.audio_path().c_str());
    return s.videoFrames > 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char **argv) {
    Args args = ParseArgs(argc, argv);
    if (args.positional.empty()) {
        Usage();
        return 2;
    }
    std::signal(SIGINT, OnSignal);
    std::signal(SIGTERM, OnSignal);
    tsl::SetLogLevel(args.Has("-v") ? IHS_LogLevelDebug : IHS_LogLevelWarn);

    std::string dir = ConfigDir();
    auto identity = LoadOrCreateIdentity(dir);
    if (!identity) return 1;

    IHS_Init();
    int rc;
    const std::string &cmd = args.positional[0];
    if (cmd == "discover") {
        rc = CmdDiscover(*identity, args);
    } else if (cmd == "probe") {
        rc = CmdProbe(*identity, args);
    } else if (cmd == "pair") {
        rc = CmdPair(*identity, dir, args);
    } else if (cmd == "stream") {
        rc = CmdStream(*identity, dir, args);
    } else {
        Usage();
        rc = 2;
    }
    IHS_Quit();
    return rc;
}
