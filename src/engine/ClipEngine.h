#pragma once
#include "core/ClipLibrary.h"
#include "media/Banner.h"
#include "engine/Transcoder.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// The only header the UI uses to talk to capture/encoding. Everything behind it runs on its own
// threads; the UI reads Status() snapshots and drains events once per frame, so it never blocks.
// Swap the backend by returning a different implementation from CreateClipEngine().
namespace cs::engine {

enum class EncoderKind { Auto, NVENC, AMF, QuickSync, X264 };
enum class Codec { H264, HEVC, AV1 };
enum class BufferState { Off, Starting, Filling, Ready, Error };

const char* EncoderLabel(EncoderKind k);      // "NVIDIA NVENC"
const char* EncoderShortLabel(EncoderKind k); // "NVENC"
const char* CodecLabel(Codec c);              // "H.264"
EncoderKind ParseEncoder(const std::string& s);
Codec ParseCodec(const std::string& s);
const char* EncoderKey(EncoderKind k);        // "nvenc" (settings value)
const char* CodecKey(Codec c);                // "h264"

struct EncoderInfo {
    EncoderKind kind = EncoderKind::X264;
    std::string gpuName;      // adapter that provides it ("" for x264)
    bool hardware = false;
    bool detected = false;    // a GPU from this vendor is present
    int probe = -1;           // FFmpeg trial encode: -1 not run, 0 failed, 1 works
};

struct AudioTrackConfig {
    std::string name;
    bool enabled = true;
    float volume = 1.0f;
};

struct CaptureConfig {
    // "monitor:<index>" (whole screen), "window:foreground" (the screen the active window is on),
    // "game" (the game's window only) or "both:<index>" (game + whole screen, two files per clip).
    std::string source = "monitor:0";
    bool hideCaptureBorder = true;      // game capture: no yellow border (Windows 11)
    int outputHeight = 0;               // 0 = native, else scale to this height
    int fps = 60;
    int bitrateKbps = 40000;
    EncoderKind encoder = EncoderKind::Auto;
    Codec codec = Codec::H264;
    int qualityPreset = 1;              // 0 performance, 1 balanced, 2 quality
    bool captureCursor = true;
    int replaySeconds = 60;
    int sampleRate = 48000;
    bool separateTracks = true;
    bool captureSystem = true;
    float systemVolume = 1.0f;
    bool captureMic = true;
    float micVolume = 1.0f;
    bool pushToTalk = false;
    uint32_t pushToTalkKey = 0;
    std::vector<AudioTrackConfig> apps;
    std::wstring outputRoot;
    bool perGameFolders = true;
    std::wstring ffmpegPath;            // "" = auto-detect
    std::wstring outputDeviceId;        // "" = default playback device (loopback)
    std::wstring inputDeviceId;         // "" = default microphone
    std::wstring fileSuffix;            // added to saved file names ("_game", "_screen" in Both mode)
};

struct EngineStatus {
    bool simulated = false;
    BufferState buffer = BufferState::Off;
    double bufferedSeconds = 0.0;
    int bufferTargetSeconds = 0;
    uint64_t bufferBytes = 0;
    bool recording = false;
    double recordingSeconds = 0.0;
    float captureFps = 0.0f;
    uint32_t droppedFrames = 0;
    std::string encoderName;            // "NVENC H.264"
    std::string sourceName;             // "Display 1"
    bool ffmpegAvailable = false;
    std::wstring ffmpegPath;
    int activeJobs = 0;
    std::string lastError;
};

enum class EngineEventType {
    ClipSaved,          // path (may be empty in simulation), seconds
    RecordingStarted,
    RecordingStopped,   // seconds
    ExportProgress,     // job, progress 0..1
    ExportDone,         // job, path
    ExportFailed,       // job, message
    EncoderProbeDone,
    Info,
    Error,
};

struct EngineEvent {
    EngineEventType type = EngineEventType::Info;
    std::wstring path;
    std::string message;
    double seconds = 0.0;
    float progress = 0.0f;
    uint64_t job = 0;
};

struct ExportRequest {
    // Either one range of one clip (source/start/end), or `segments` (cuts, speed, montages).
    std::wstring source;
    std::wstring destination;
    double start = 0.0;
    double end = 0.0;
    std::vector<Segment> segments;
    std::vector<Caption> captions;
    double targetMB = 0.0;              // > 0: fit under this size
    int quality = 0;                    // 0 Original, 1 High, 2 Small file
    bool banner = false;                // the animated "Clipped using Replayr" banner
    BannerStyle bannerStyle;
    bool hardware = true;
    Look look;
    std::vector<std::vector<float>> mix;   // audio mix matrix (see TranscodeRequest); empty = tracks as they are
};

class IClipEngine {
public:
    virtual ~IClipEngine() = default;

    virtual void Configure(const CaptureConfig& cfg) = 0;
    virtual void StartBuffer() = 0;
    virtual void StopBuffer() = 0;
    virtual void SaveClip(int seconds, const std::wstring& gameName) = 0;   // seconds <= 0: whole buffer
    virtual void StartRecording(const std::wstring& gameName) = 0;
    virtual void StopRecording() = 0;

    virtual uint64_t Export(const ExportRequest& req) = 0;
    virtual void CancelExport(uint64_t job) = 0;
    virtual void ProbeEncoders() = 0;
    virtual std::vector<EncoderInfo> Encoders() const = 0;

    virtual EngineStatus Status() const = 0;
    virtual bool PollEvent(EngineEvent& out) = 0;
    virtual std::shared_ptr<const ClipList> ListClips() const = 0;
};

// Desktop Duplication + Media Foundation backend (RealClipEngine.cpp).
// `bufferName` is the folder under %LOCALAPPDATA%\Replayr for the replay segments; a second engine
// running at the same time needs its own.
std::unique_ptr<IClipEngine> CreateClipEngine(ClipLibrary& library, const std::wstring& bufferName = L"Buffer");

// "Both" capture: wraps two engines (game window + whole screen) behind one IClipEngine
// (DualClipEngine.cpp). With any other source it behaves exactly like the single engine.
std::unique_ptr<IClipEngine> CreateCaptureEngine(ClipLibrary& library);

} // namespace cs::engine
