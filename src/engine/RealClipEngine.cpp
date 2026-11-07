// The real capture backend.
//
// Pipeline (one session thread, paced to the frame rate):
//   Desktop Duplication (GPU of the monitor) -> cursor -> D3D11 video processor (BGRA -> NV12,
//   scaled) -> Media Foundation sink writer (hardware H.264/HEVC when available) + AAC audio from
//   WASAPI loopback and microphone.
//
// The replay buffer is a ring of short MP4 segments on disk (%LOCALAPPDATA%\Replayr\Buffer).
// Each segment is a complete file starting with a keyframe, so saving a clip or finishing a
// recording only joins segments (no re-encode), which takes well under a second for a minute of
// footage. The next segment's encoder is prepared in the background and the previous one is
// finalized in the background, so rotating segments doesn't stall capture.

#include "engine/AudioInput.h"
#include "engine/ClipEngine.h"
#include "engine/DesktopCapture.h"
#include "engine/Encoder.h"
#include "engine/FFmpegTools.h"
#include "engine/MediaWriter.h"
#include "engine/Transcoder.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "core/Util.h"

#include <windows.h>
#include <d3d11.h>
#include <mmsystem.h>
#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace cs::engine {

namespace {

constexpr double kSegmentSeconds = 4.0;

struct JobThread {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> finished = std::make_shared<std::atomic<bool>>(false);
};

struct ComScope {
    ComScope() { CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComScope() { CoUninitialize(); }
};

// Runs jobs one after another on its own thread (encoder preparation, finalizing segments).
class Worker {
public:
    Worker()
    {
        thread_ = std::thread([this] {
            ComScope com;
            for (;;) {
                std::function<void()> job;
                {
                    std::unique_lock<std::mutex> lock(m_);
                    cv_.wait(lock, [this] { return quit_ || !jobs_.empty(); });
                    if (jobs_.empty()) return;
                    job = std::move(jobs_.front());
                    jobs_.pop_front();
                    busy_ = true;
                }
                job();
                {
                    std::lock_guard<std::mutex> lock(m_);
                    busy_ = false;
                }
                idle_.notify_all();
            }
        });
    }
    ~Worker()
    {
        {
            std::lock_guard<std::mutex> lock(m_);
            quit_ = true;   // remaining jobs still run (they finalize files)
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    void Post(std::function<void()> fn)
    {
        {
            std::lock_guard<std::mutex> lock(m_);
            jobs_.push_back(std::move(fn));
        }
        cv_.notify_one();
    }
    void WaitIdle()
    {
        std::unique_lock<std::mutex> lock(m_);
        idle_.wait(lock, [this] { return jobs_.empty() && !busy_; });
    }

private:
    std::thread thread_;
    std::mutex m_;
    std::condition_variable cv_, idle_;
    std::deque<std::function<void()>> jobs_;
    bool quit_ = false, busy_ = false;
};

struct Segment {
    int id = 0;
    std::wstring path;
    double start = 0.0;       // session seconds
    double duration = 0.0;    // set when the segment is closed
    bool closed = false;
    bool finalized = false;
    bool failed = false;
    bool recording = false;   // part of the running recording
    int pins = 0;             // save jobs using it
    uint64_t bytes = 0;
};

struct Track {
    std::vector<std::pair<AudioInput*, int>> inputs;   // input, gain slot (0 system, 1 mic)
    int64_t written = 0;                               // frames since session start
};

enum class CmdType { StartBuffer, StopBuffer, Save, RecStart, RecStop, Reconfigure };
struct Cmd {
    CmdType type;
    int seconds = 0;
    std::wstring game;
};

struct PendingSave {
    int seconds = 0;
    std::wstring game;
};

class RealClipEngine final : public IClipEngine {
public:
    explicit RealClipEngine(ClipLibrary& lib) : lib_(lib)
    {
        MediaStartup();
        encoders_ = DetectEncoders();
        bufferDir_ = JoinPath(LocalDataDir(), L"Buffer");
        EnsureDir(bufferDir_);
        CleanBufferDir();
        thread_ = std::thread([this] { Loop(); });
    }

    ~RealClipEngine() override
    {
        quit_ = true;
        cmdCv_.notify_all();
        if (thread_.joinable()) thread_.join();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& kv : cancels_) *kv.second = true;
        }
        {
            std::lock_guard<std::mutex> lock(jobsMutex_);
            for (auto& j : jobs_)
                if (j.thread.joinable()) j.thread.join();
        }
        worker_.reset();
        CleanBufferDir();
        MediaShutdown();
    }

    // --- IClipEngine (UI thread) ------------------------------------------------------------

    void Configure(const CaptureConfig& cfg) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool restart = CaptureChanged(cfg_, cfg);
        cfg_ = cfg;
        ffmpeg_ = FindFFmpeg(cfg.ffmpegPath);
        st_.bufferTargetSeconds = cfg.replaySeconds;
        st_.ffmpegAvailable = !ffmpeg_.empty();
        st_.ffmpegPath = ffmpeg_;
        if (st_.encoderName.empty()) st_.encoderName = std::string("Media Foundation ") + CodecLabel(cfg.codec == Codec::AV1 ? Codec::H264 : cfg.codec);
        if (st_.sourceName.empty() || restart) {
            if (cfg.source.rfind("monitor:", 0) == 0) st_.sourceName = "Display " + std::to_string(atoi(cfg.source.c_str() + 8) + 1);
            else st_.sourceName = "Foreground window's display";
        }
        if (restart && configured_) PushCmdLocked({ CmdType::Reconfigure });
        configured_ = true;
    }

    void StartBuffer() override { PushCmd({ CmdType::StartBuffer }); }
    void StopBuffer() override { PushCmd({ CmdType::StopBuffer }); }
    void SaveClip(int seconds, const std::wstring& game) override { PushCmd({ CmdType::Save, seconds, game }); }
    void StartRecording(const std::wstring& game) override
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (st_.recording) return;
            st_.recording = true;   // immediate feedback; the session thread confirms or reports an error
            st_.recordingSeconds = 0.0;
        }
        PushCmd({ CmdType::RecStart, 0, game });
    }
    void StopRecording() override { PushCmd({ CmdType::RecStop }); }

    uint64_t Export(const ExportRequest& req) override
    {
        const uint64_t id = nextJob_++;
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancels_[id] = cancel;
        }
        Spawn([this, req, id, cancel] {
            ComScope com;
            EngineEvent done;
            done.job = id;
            EnsureDir(ParentDir(req.destination));
            double lastSent = 0.0;
            auto progress = [&](float p) {
                double now = NowSeconds();
                if (now - lastSent < 0.08 && p < 1.0f) return;
                lastSent = now;
                EngineEvent e{ EngineEventType::ExportProgress };
                e.job = id;
                e.progress = p;
                Push(e);
            };
            bool ok = false;
            std::string message;
            // "Original" without the banner is a stream copy when the in-point is on a keyframe (Replayr
            // records one per second); anything else is decoded and re-encoded so the length is exact.
            std::vector<engine::Segment> segs = req.segments;
            if (segs.empty()) segs.push_back(engine::Segment{ req.source, req.start, req.end, 1.0 });
            const bool simple = segs.size() == 1 && segs[0].speed == 1.0 && req.captions.empty() && req.targetMB <= 0.0 && req.quality == 0 &&
                                !req.banner && !req.look.Any();
            const double key = !simple ? -1.0 : segs[0].start < 0.05 ? 0.0 : KeyframeAtOrBefore(segs[0].source, segs[0].start);
            const bool copy = simple && key >= 0.0 && segs[0].start - key < 0.06;
            std::string err;
            if (copy) {
                ok = RemuxMp4({ { segs[0].source, 0.0 } }, req.destination, segs[0].start, segs[0].end, progress, cancel.get(), nullptr, err);
            } else {
                TranscodeRequest t;
                t.segments = segs;
                t.destination = req.destination;
                t.quality = (ExportQuality)Clamp(req.quality, 0, 2);
                t.targetMB = req.targetMB;
                t.banner = req.banner;
                t.bannerStyle = req.bannerStyle;
                t.captions = req.captions;
                t.hardware = req.hardware;
                t.look = req.look;
                ok = Transcode(t, progress, cancel.get(), err);
            }
            if (!ok) message = *cancel ? "Export cancelled." : err;
            if (!ok) platform::DeleteFileNow(req.destination);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                cancels_.erase(id);
            }
            done.type = ok ? EngineEventType::ExportDone : EngineEventType::ExportFailed;
            done.path = ok ? req.destination : L"";
            done.message = message;
            done.seconds = engine::OutputLength(segs);
            Push(done);
            if (ok) lib_.Rescan();
        });
        return id;
    }

    void CancelExport(uint64_t job) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cancels_.find(job);
        if (it != cancels_.end()) *it->second = true;
    }

    void ProbeEncoders() override
    {
        std::wstring exe;
        Codec codec;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            exe = ffmpeg_;
            codec = cfg_.codec;
        }
        Spawn([this, exe, codec] {
            std::vector<EncoderInfo> list;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                list = encoders_;
            }
            for (auto& e : list) {
                if (exe.empty()) break;
                FFmpegResult r = RunFFmpeg(exe, BuildProbeArgs(FfmpegEncoderName(e.kind, codec)), 0.0, nullptr, &quit_);
                e.probe = (r.started && r.exitCode == 0) ? 1 : 0;
                if (quit_) return;
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                encoders_ = list;
            }
            EngineEvent e{ EngineEventType::EncoderProbeDone };
            e.message = exe.empty() ? "Recording uses Windows' built-in encoders. FFmpeg (optional) is only needed for re-encoded exports."
                                    : "Encoder test finished.";
            Push(e);
        });
    }

    std::vector<EncoderInfo> Encoders() const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return encoders_;
    }

    EngineStatus Status() const override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        EngineStatus s = st_;
        s.activeJobs = activeJobs_.load();
        return s;
    }

    bool PollEvent(EngineEvent& out) override
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (events_.empty()) return false;
        out = std::move(events_.front());
        events_.pop_front();
        return true;
    }

    std::shared_ptr<const ClipList> ListClips() const override { return lib_.Snapshot(); }

private:
    // --- helpers ----------------------------------------------------------------------------

    static bool CaptureChanged(const CaptureConfig& a, const CaptureConfig& b)
    {
        return a.source != b.source || a.outputHeight != b.outputHeight || a.fps != b.fps || a.bitrateKbps != b.bitrateKbps ||
               a.encoder != b.encoder || a.codec != b.codec || a.captureCursor != b.captureCursor || a.separateTracks != b.separateTracks ||
               a.captureSystem != b.captureSystem || a.captureMic != b.captureMic || a.outputDeviceId != b.outputDeviceId ||
               a.inputDeviceId != b.inputDeviceId;
    }

    static std::string FirstLine(const std::string& s)
    {
        size_t nl = s.find('\n');
        return nl == std::string::npos ? s : s.substr(0, nl);
    }

    void PushCmdLocked(Cmd c) { cmds_.push_back(std::move(c)); }
    void PushCmd(Cmd c)
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cmds_.push_back(std::move(c));
        }
        cmdCv_.notify_all();
    }
    void Push(EngineEvent e)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        events_.push_back(std::move(e));
    }
    void PushError(const std::string& msg)
    {
        // The same failure repeating (e.g. capture retrying) shows once a minute at most.
        const double now = NowSeconds();
        if (msg == lastErrorMsg_ && now - lastErrorAt_ < 60.0) return;
        lastErrorMsg_ = msg;
        lastErrorAt_ = now;
        EngineEvent e{ EngineEventType::Error };
        e.message = msg;
        Push(e);
    }

    void CleanBufferDir()
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(JoinPath(bufferDir_, L"*.mp4").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return;
        do {
            DeleteFileW(JoinPath(bufferDir_, fd.cFileName).c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    std::wstring ClipPath(const CaptureConfig& cfg, const std::wstring& game) const
    {
        std::wstring folder = cfg.outputRoot;
        const std::wstring name = SanitizeFileName(game.empty() ? L"Clip" : game);
        if (cfg.perGameFolders && !game.empty()) folder = JoinPath(folder, name);
        EnsureDir(folder);
        return UniquePath(JoinPath(folder, name + L" " + TimestampForFile() + L".mp4"));
    }

    void Spawn(std::function<void()> fn)
    {
        std::lock_guard<std::mutex> lock(jobsMutex_);
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            if (it->finished->load()) {
                if (it->thread.joinable()) it->thread.join();
                it = jobs_.erase(it);
            } else {
                ++it;
            }
        }
        JobThread j;
        auto finished = j.finished;
        activeJobs_++;
        j.thread = std::thread([this, fn = std::move(fn), finished] {
            fn();
            activeJobs_--;
            *finished = true;
        });
        jobs_.push_back(std::move(j));
    }

    // --- session thread ---------------------------------------------------------------------

    void Loop()
    {
        ComScope com;
        timeBeginPeriod(1);
        worker_ = std::make_unique<Worker>();
        while (!quit_) {
            ProcessCommands();
            const bool want = wantBuffer_ || recActive_ || recStartPending_;
            if (want && !session_ && NowSeconds() >= retryAt_) StartSession();
            if (session_ && (!want || restartPending_)) {
                StopSession();
                restartPending_ = false;
            }
            if (session_) {
                Tick();
            } else {
                if (pruneRequested_.exchange(false)) PruneSegments(true);
                PublishIdle();
                std::unique_lock<std::mutex> lock(mutex_);
                cmdCv_.wait_for(lock, std::chrono::milliseconds(50), [this] { return quit_ || !cmds_.empty(); });
            }
        }
        if (session_) StopSession();
        worker_->WaitIdle();
        timeEndPeriod(1);
    }

    void ProcessCommands()
    {
        std::deque<Cmd> cmds;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cmds.swap(cmds_);
        }
        for (Cmd& c : cmds) {
            switch (c.type) {
            case CmdType::StartBuffer:
                if (!wantBuffer_) {
                    wantBuffer_ = true;
                    bufferSince_ = session_ ? SessionNow() : 0.0;
                    std::lock_guard<std::mutex> lock(mutex_);
                    if (failures_ == 0 || session_) {   // after failures, keep showing the error until a retry works
                        st_.buffer = BufferState::Starting;
                        st_.lastError.clear();
                    }
                }
                break;
            case CmdType::StopBuffer:
                wantBuffer_ = false;
                pendingSaves_.clear();
                break;
            case CmdType::Save: {
                if (!wantBuffer_ || !session_) {
                    PushError("The replay buffer is off. Turn it on first.");
                    break;
                }
                if (SessionNow() - bufferSince_ < 1.0) {
                    PushError("The replay buffer is still filling up.");
                    break;
                }
                pendingSaves_.push_back({ c.seconds, c.game });
                forceCut_ = true;
                break;
            }
            case CmdType::RecStart:
                if (!recActive_ && !recStartPending_) {
                    recStartPending_ = true;
                    recGame_ = c.game;
                    recWall_ = NowSeconds();
                    if (session_) forceCut_ = true;
                    retryAt_ = 0.0;
                }
                break;
            case CmdType::RecStop:
                if (recActive_) {
                    recStopPending_ = true;
                    forceCut_ = true;
                } else if (recStartPending_) {
                    recStartPending_ = false;
                    SetRecordingFlag(false, 0.0);
                }
                break;
            case CmdType::Reconfigure:
                if (session_) {
                    if (recActive_ || recStartPending_) reconfigAfterRec_ = true;
                    else restartPending_ = true;
                }
                break;
            }
        }
    }

    double SessionNow() const { return NowSeconds() - sessionStart_; }

    void SetRecordingFlag(bool on, double seconds)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        st_.recording = on;
        st_.recordingSeconds = seconds;
    }

    bool StartSession()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            scfg_ = cfg_;
        }
        std::string error;
        DesktopCaptureParams dp;
        dp.monitorIndex = scfg_.source.rfind("monitor:", 0) == 0 ? atoi(scfg_.source.c_str() + 8) : -1;
        dp.outputHeight = scfg_.outputHeight;
        dp.fps = std::clamp(scfg_.fps, 10, 240);
        dp.cursor = scfg_.captureCursor;
        fps_ = dp.fps;
        capture_ = std::make_unique<DesktopCapture>();
        bool ok = capture_->Open(dp, error);

        // Audio tracks: system (with the mic mixed in unless tracks are separate) and/or mic.
        sysIn_.reset();
        micIn_.reset();
        tracks_.clear();
        if (ok && scfg_.captureSystem) {
            sysIn_ = std::make_unique<AudioInput>();
            std::string aerr;
            if (!sysIn_->Open(true, scfg_.outputDeviceId, aerr)) {
                Log("System audio: %s", aerr.c_str());
                sysIn_.reset();
                PushError("System audio won't be recorded: " + aerr);
            }
        }
        if (ok && scfg_.captureMic) {
            micIn_ = std::make_unique<AudioInput>();
            std::string aerr;
            if (!micIn_->Open(false, scfg_.inputDeviceId, aerr)) {
                Log("Microphone: %s", aerr.c_str());
                micIn_.reset();
            }
        }
        if (sysIn_) {
            Track t;
            t.inputs.push_back({ sysIn_.get(), 0 });
            if (micIn_ && !scfg_.separateTracks) t.inputs.push_back({ micIn_.get(), 1 });
            tracks_.push_back(t);
        }
        if (micIn_ && (scfg_.separateTracks || !sysIn_)) {
            Track t;
            t.inputs.push_back({ micIn_.get(), 1 });
            tracks_.push_back(t);
        }

        sessionId_++;
        segNum_ = 0;
        sessionStart_ = NowSeconds();
        frameIdx_ = 0;
        segStartFrame_ = 0;
        segFrames_ = (int64_t)std::llround(kSegmentSeconds * fps_);

        if (ok) {
            // Find a frame path the encoder accepts, best first.
            const FramePath order[] = { FramePath::GpuSurfaces, FramePath::GpuReadback, FramePath::CpuRgb };
            ok = false;
            for (FramePath p : order) {
                if (p < capture_->Path()) continue;
                if (!capture_->SetPath(p)) continue;
                std::wstring path;
                std::string werr;
                writer_ = MakeWriter(path, werr);
                if (writer_) {
                    curPath_ = path;
                    ok = true;
                    break;
                }
                error = werr;
                Log("Writer with %s frames failed: %s", FramePathLabel(p), werr.c_str());
            }
        }
        if (!ok) {
            Log("Capture failed to start: %s", error.c_str());
            capture_.reset();
            sysIn_.reset();
            micIn_.reset();
            tracks_.clear();
            failures_++;
            retryAt_ = NowSeconds() + std::min(60.0, 5.0 * failures_);
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (wantBuffer_) st_.buffer = BufferState::Error;
                st_.lastError = error;
                st_.recording = false;
            }
            PushError(error.empty() ? "Capture couldn't start." : error);
            wantBuffer_ = false;
            recStartPending_ = false;
            return false;
        }
        frameBuf_.resize((size_t)capture_->OutWidth() * capture_->OutHeight() * 4);
        failures_ = 0;
        session_ = true;
        bufferSince_ = 0.0;
        forceCut_ = false;
        gpuFailures_ = 0;
        fpsCount_ = 0;
        fpsWindowStart_ = 0.0;
        lastStats_ = -1.0;
        dropped_ = 0;
        OpenSegment(0);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            st_.encoderName = writer_->EncoderName() + "  \xC2\xB7  " +
                              std::to_string(capture_->OutWidth()) + "x" + std::to_string(capture_->OutHeight());
            st_.sourceName = capture_->MonitorName();
            st_.lastError.clear();
            if (wantBuffer_) st_.buffer = BufferState::Filling;
        }
        Log("Session %d started: %s, %s frames, %d audio track(s)", sessionId_, writer_->EncoderName().c_str(),
            FramePathLabel(capture_->Path()), (int)tracks_.size());
        if (recStartPending_) BeginRecording();
        return true;
    }

    std::unique_ptr<MediaWriter> MakeWriter(std::wstring& path, std::string& error)
    {
        wchar_t name[64];
        swprintf(name, 64, L"seg_%d_%05d.mp4", sessionId_, segNum_++);
        path = JoinPath(bufferDir_, name);
        WriterParams wp;
        wp.path = path;
        wp.width = capture_->OutWidth();
        wp.height = capture_->OutHeight();
        wp.fps = fps_;
        wp.bitrateKbps = scfg_.bitrateKbps;
        wp.codec = scfg_.codec;
        wp.hardware = scfg_.encoder != EncoderKind::X264;
        wp.inputKind = capture_->Path() == FramePath::GpuSurfaces ? 0 : (capture_->Path() == FramePath::GpuReadback ? 1 : 2);
        wp.device = capture_->Device();
        wp.audioTracks = (int)tracks_.size();
        auto w = std::make_unique<MediaWriter>();
        if (!w->Open(wp, error)) {
            w.reset();
            DeleteFileW(path.c_str());
            return nullptr;
        }
        return w;
    }

    void OpenSegment(int64_t frame)
    {
        Segment s;
        s.id = ++segId_;
        s.path = curPath_;
        s.start = frame / (double)fps_;
        s.recording = recActive_;
        curSegId_ = s.id;
        segStartFrame_ = frame;
        std::lock_guard<std::mutex> lock(segMutex_);
        segments_.push_back(s);
    }

    void BeginRecording()
    {
        recStartPending_ = false;
        recActive_ = true;
        recFirstSeg_ = curSegId_;
        recSessionStart_ = segStartFrame_ / (double)fps_;
        {
            std::lock_guard<std::mutex> lock(segMutex_);
            for (Segment& s : segments_)
                if (s.id == curSegId_) s.recording = true;
        }
        SetRecordingFlag(true, 0.0);
        Push({ EngineEventType::RecordingStarted });
    }

    // Closes the current segment at `frame` and (optionally) starts the next one.
    void Cut(int64_t frame, bool openNext)
    {
        FlushPrev(true);   // the previous segment must be fully written before this one closes
        const double cutTime = frame / (double)fps_;
        const int closedId = curSegId_;
        {
            std::lock_guard<std::mutex> lock(segMutex_);
            for (Segment& s : segments_)
                if (s.id == closedId) {
                    s.duration = cutTime - s.start;
                    s.closed = true;
                }
        }
        prev_ = std::move(writer_);
        prevSegId_ = closedId;
        prevBoundary_ = (int64_t)std::llround(cutTime * AudioInput::Rate);
        prevSegStart_ = segStartFrame_ / (double)fps_;
        prevSince_ = NowSeconds();

        // Saves and recording-stops that asked for this cut.
        for (const PendingSave& ps : pendingSaves_) QueueSave(ps, cutTime, closedId);
        pendingSaves_.clear();
        if (recStopPending_) {
            recStopPending_ = false;
            recActive_ = false;
            QueueRecordingSave(recFirstSeg_, closedId, cutTime);
            if (reconfigAfterRec_) {
                reconfigAfterRec_ = false;
                restartPending_ = true;
            }
        }

        if (openNext) {
            std::unique_ptr<MediaWriter> next;
            std::wstring nextPath;
            {
                std::lock_guard<std::mutex> lock(prepMutex_);
                if (prepared_ && preparedPathKind_ == capture_->Path()) {
                    next = std::move(prepared_);
                    nextPath = preparedPath_;
                }
            }
            if (!next) {
                std::string err;
                next = MakeWriter(nextPath, err);
                if (!next) {
                    Log("Segment writer failed: %s", err.c_str());
                    PushError("The encoder stopped: " + err);
                    restartPending_ = true;
                    retryAt_ = NowSeconds() + 2.0;
                    return;
                }
            }
            writer_ = std::move(next);
            curPath_ = nextPath;
            OpenSegment(frame);
            if (recStartPending_) BeginRecording();
        }
        PruneSegments();
    }

    // Hands the previous writer to the worker once its audio has caught up with the cut.
    void FlushPrev(bool force)
    {
        if (!prev_) return;
        bool caughtUp = true;
        for (const Track& t : tracks_)
            if (t.written < prevBoundary_) caughtUp = false;
        if (!caughtUp && !force && NowSeconds() - prevSince_ < 1.0) return;
        std::shared_ptr<MediaWriter> w(std::move(prev_));
        const int id = prevSegId_;
        worker_->Post([this, w, id] {
            const bool ok = w->Finalize();
            std::lock_guard<std::mutex> lock(segMutex_);
            for (Segment& s : segments_)
                if (s.id == id) {
                    s.finalized = ok;
                    s.failed = !ok;
                    WIN32_FILE_ATTRIBUTE_DATA fa{};
                    if (GetFileAttributesExW(s.path.c_str(), GetFileExInfoStandard, &fa))
                        s.bytes = ((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
                }
            segCv_.notify_all();
        });
    }

    void PrepareNext()
    {
        {
            std::lock_guard<std::mutex> lock(prepMutex_);
            if (prepared_ || preparing_) return;
            preparing_ = true;
        }
        // Built on the worker so creating the encoder doesn't stall capture.
        std::wstring path;
        wchar_t name[64];
        swprintf(name, 64, L"seg_%d_%05d.mp4", sessionId_, segNum_++);
        path = JoinPath(bufferDir_, name);
        WriterParams wp;
        wp.path = path;
        wp.width = capture_->OutWidth();
        wp.height = capture_->OutHeight();
        wp.fps = fps_;
        wp.bitrateKbps = scfg_.bitrateKbps;
        wp.codec = scfg_.codec;
        wp.hardware = scfg_.encoder != EncoderKind::X264;
        const FramePath kind = capture_->Path();
        wp.inputKind = kind == FramePath::GpuSurfaces ? 0 : (kind == FramePath::GpuReadback ? 1 : 2);
        wp.device = capture_->Device();
        wp.audioTracks = (int)tracks_.size();
        ComPtr<ID3D11Device> keep(capture_->Device());
        worker_->Post([this, wp, kind, keep] {
            auto w = std::make_unique<MediaWriter>();
            std::string err;
            const bool ok = w->Open(wp, err);
            std::lock_guard<std::mutex> lock(prepMutex_);
            preparing_ = false;
            if (ok) {
                prepared_ = std::move(w);
                preparedPath_ = wp.path;
                preparedPathKind_ = kind;
            } else {
                DeleteFileW(wp.path.c_str());
            }
        });
    }

    void DropPrepared()
    {
        worker_->WaitIdle();
        std::lock_guard<std::mutex> lock(prepMutex_);
        if (prepared_) {
            prepared_.reset();
            DeleteFileW(preparedPath_.c_str());
        }
        preparing_ = false;
    }

    void Tick()
    {
        const double now = SessionNow();
        const double frameDur = 1.0 / fps_;
        double due = frameIdx_ * frameDur;
        if (now < due) {
            // Poll audio while waiting so its buffers never overflow, then sleep until the frame is due.
            PumpAudio(now);
            const double wait = due - SessionNow();
            if (wait > 0.0015) Sleep((DWORD)std::max(1.0, std::floor((wait - 0.0005) * 1000.0)));
            return;
        }
        if (now - due > 4 * frameDur) {   // fell behind (system stall): skip ahead
            const int64_t skip = (int64_t)((now - due) * fps_);
            frameIdx_ += skip;
            dropped_ += (uint32_t)skip;
        }
        capture_->Update();

        if (forceCut_ || frameIdx_ - segStartFrame_ >= segFrames_) {
            if (frameIdx_ > segStartFrame_ || !forceCut_) Cut(frameIdx_, true);
            forceCut_ = false;
            if (!writer_) return;   // encoder failed: the session restarts
        }

        const double t = (frameIdx_ - segStartFrame_) * frameDur;
        bool ok = false;
        if (capture_->Path() == FramePath::GpuSurfaces) {
            ID3D11Texture2D* tex = nullptr;
            if (capture_->ComposeGpu(&tex) && tex) {
                ok = writer_->WriteVideoSurface(tex, t, frameDur);
                tex->Release();
            }
            if (!ok && ++gpuFailures_ >= 3) {
                Log("GPU surfaces rejected by the encoder; switching to copied frames");
                capture_->SetPath(FramePath::GpuReadback);
                DropPrepared();
                forceCut_ = true;
            }
        } else {
            const size_t size = capture_->Path() == FramePath::CpuRgb ? (size_t)capture_->OutWidth() * capture_->OutHeight() * 4
                                                                       : (size_t)capture_->OutWidth() * capture_->OutHeight() * 3 / 2;
            if (capture_->ComposeCpu(frameBuf_.data(), frameBuf_.size())) ok = writer_->WriteVideoMemory(frameBuf_.data(), size, t, frameDur);
        }
        if (ok) {
            gpuFailures_ = 0;
            fpsCount_++;
        } else {
            dropped_++;
        }
        frameIdx_++;

        if (frameIdx_ - segStartFrame_ >= segFrames_ - (int64_t)(fps_ * 1.5)) PrepareNext();
        PumpAudio(SessionNow());
        FlushPrev(false);
        UpdateStats();
    }

    void PumpAudio(double now)
    {
        if (tracks_.empty()) return;
        const int64_t expected = (int64_t)(now * AudioInput::Rate);
        if (sysIn_) sysIn_->Poll(expected);
        if (micIn_) micIn_->Poll(expected);
        float gains[2];
        {
            std::lock_guard<std::mutex> lock(mutex_);
            gains[0] = cfg_.captureSystem ? cfg_.systemVolume : 0.0f;
            gains[1] = cfg_.captureMic ? cfg_.micVolume : 0.0f;
            if (cfg_.pushToTalk && cfg_.pushToTalkKey && !platform::IsKeyDown(cfg_.pushToTalkKey)) gains[1] = 0.0f;
        }
        for (size_t ti = 0; ti < tracks_.size(); ++ti) {
            Track& tr = tracks_[ti];
            int64_t n = INT64_MAX;
            for (auto& in : tr.inputs) n = std::min(n, in.first->Available());
            if (n <= 0 || n == INT64_MAX) continue;
            n = std::min<int64_t>(n, AudioInput::Rate);   // at most a second per call
            mix_.assign((size_t)n * 2, 0.0f);
            for (size_t i = 0; i < tr.inputs.size(); ++i) tr.inputs[i].first->Take(mix_.data(), n, gains[tr.inputs[i].second], i > 0);
            // Part before the last cut goes to the previous segment.
            int64_t pos = tr.written;
            int64_t offset = 0;
            if (prev_ && pos < prevBoundary_) {
                const int64_t part = std::min(n, prevBoundary_ - pos);
                prev_->WriteAudio((int)ti, mix_.data(), (int)part, pos / (double)AudioInput::Rate - prevSegStart_);
                offset = part;
                pos += part;
            }
            if (offset < n && writer_) {
                const double segStart = segStartFrame_ / (double)fps_;
                const int64_t segStartSample = (int64_t)std::llround(segStart * AudioInput::Rate);
                if (pos < segStartSample) {   // only happens if the previous writer was already flushed
                    const int64_t skip = std::min(n - offset, segStartSample - pos);
                    offset += skip;
                    pos += skip;
                }
                if (offset < n)
                    writer_->WriteAudio((int)ti, mix_.data() + offset * 2, (int)(n - offset), pos / (double)AudioInput::Rate - segStart);
            }
            tr.written += n;
        }
    }

    void StopSession()
    {
        if (!session_) return;
        Cut(frameIdx_, false);
        // Let audio catch up for the last segment, then finalize it.
        FlushPrev(true);
        DropPrepared();
        worker_->WaitIdle();
        capture_.reset();
        sysIn_.reset();
        micIn_.reset();
        tracks_.clear();
        session_ = false;
        if (recActive_) {   // only on shutdown/restart paths
            recActive_ = false;
            SetRecordingFlag(false, 0.0);
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            st_.captureFps = 0.0f;
            if (!wantBuffer_) st_.buffer = BufferState::Off;
        }
        PruneSegments(true);
        Log("Session stopped");
    }

    // Deletes segments that are outside the replay window and not used by a save or recording.
    void PruneSegments(bool all = false)
    {
        const double keep = wantBuffer_ && !all ? scfg_.replaySeconds + kSegmentSeconds : 0.0;
        const double horizon = session_ ? SessionNow() - keep : 1e18;
        std::lock_guard<std::mutex> lock(segMutex_);
        for (auto it = segments_.begin(); it != segments_.end();) {
            const Segment& s = *it;
            const bool done = s.finalized || s.failed;
            const bool oldEnough = !session_ || (s.closed && s.start + s.duration < horizon);
            if (done && s.pins == 0 && !s.recording && oldEnough) {
                DeleteFileW(s.path.c_str());
                it = segments_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void QueueSave(const PendingSave& ps, double cutTime, int closedId)
    {
        const double from = std::max(bufferSince_, cutTime - (ps.seconds > 0 ? ps.seconds : scfg_.replaySeconds));
        std::vector<int> ids;
        {
            std::lock_guard<std::mutex> lock(segMutex_);
            for (Segment& s : segments_) {
                if (s.id > closedId || s.failed) continue;
                if (s.closed && s.start + s.duration <= from) continue;
                s.pins++;
                ids.push_back(s.id);
            }
        }
        const std::wstring out = ClipPath(scfg_, ps.game);
        SpawnJoin(ids, out, from, false, cutTime - from);
    }

    void QueueRecordingSave(int firstId, int lastId, double cutTime)
    {
        std::vector<int> ids;
        {
            std::lock_guard<std::mutex> lock(segMutex_);
            for (Segment& s : segments_) {
                if (s.id < firstId || s.id > lastId || s.failed) continue;
                s.pins++;
                s.recording = false;
                ids.push_back(s.id);
            }
        }
        const double length = cutTime - recSessionStart_;
        SetRecordingFlag(false, 0.0);
        EngineEvent e{ EngineEventType::RecordingStopped };
        e.seconds = length;
        Push(e);
        const std::wstring out = ClipPath(scfg_, recGame_);
        SpawnJoin(ids, out, recSessionStart_, true, length);
    }

    // Waits for the segments to be finalized, then joins them into one MP4.
    void SpawnJoin(std::vector<int> ids, std::wstring out, double from, bool recording, double expected)
    {
        if (ids.empty()) {
            PushError("Nothing was captured yet.");
            return;
        }
        Spawn([this, ids, out, from, recording, expected] {
            ComScope com;
            std::vector<RemuxInput> inputs;
            double firstStart = 0.0;
            {
                std::unique_lock<std::mutex> lock(segMutex_);
                segCv_.wait_for(lock, std::chrono::seconds(15), [&] {
                    for (const Segment& s : segments_)
                        if (std::find(ids.begin(), ids.end(), s.id) != ids.end() && !s.finalized && !s.failed) return false;
                    return true;
                });
                bool firstSet = false;
                for (const Segment& s : segments_) {
                    if (std::find(ids.begin(), ids.end(), s.id) == ids.end() || !s.finalized) continue;
                    if (!firstSet) {
                        firstStart = s.start;
                        firstSet = true;
                    }
                    inputs.push_back({ s.path, s.start - firstStart });
                }
            }
            std::string err;
            double length = 0.0;
            bool ok = !inputs.empty();
            if (ok) ok = RemuxMp4(inputs, out, std::max(0.0, from - firstStart), 0.0, nullptr, &quit_, &length, err);
            else err = "The buffered video couldn't be read.";
            {
                std::lock_guard<std::mutex> lock(segMutex_);
                for (Segment& s : segments_)
                    if (std::find(ids.begin(), ids.end(), s.id) != ids.end()) s.pins--;
            }
            pruneRequested_ = true;
            if (ok && FileExists(out)) {
                EngineEvent e{ EngineEventType::ClipSaved };
                e.path = out;
                e.seconds = length > 0.0 ? length : expected;
                e.message = recording ? "Recording" : "";
                Push(e);
                lib_.Rescan();
            } else {
                platform::DeleteFileNow(out);
                Log("Saving failed: %s", err.c_str());
                PushError((recording ? "Couldn't save the recording: " : "Couldn't save the clip: ") + err);
            }
        });
    }

    void UpdateStats()
    {
        const double now = SessionNow();
        if (pruneRequested_.exchange(false)) PruneSegments();
        if (now - lastStats_ < 0.25) return;
        lastStats_ = now;
        float fps = 0.0f;
        if (now - fpsWindowStart_ >= 1.0) {
            measuredFps_ = (float)(fpsCount_ / (now - fpsWindowStart_));
            fpsCount_ = 0;
            fpsWindowStart_ = now;
        }
        fps = measuredFps_;
        uint64_t bytes = 0;
        {
            std::lock_guard<std::mutex> lock(segMutex_);
            for (const Segment& s : segments_) bytes += s.bytes;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        st_.captureFps = fps;
        st_.droppedFrames = dropped_;
        if (wantBuffer_) {
            st_.bufferedSeconds = std::min<double>(now - bufferSince_, scfg_.replaySeconds);
            st_.bufferBytes = bytes;
            st_.buffer = st_.bufferedSeconds >= scfg_.replaySeconds - 0.25 ? BufferState::Ready : BufferState::Filling;
        } else {
            st_.bufferedSeconds = 0.0;
            st_.bufferBytes = 0;
            st_.buffer = BufferState::Off;
        }
        if (recActive_) st_.recordingSeconds = now - recSessionStart_;
    }

    void PublishIdle()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        st_.captureFps = 0.0f;
        if (!wantBuffer_ && st_.buffer != BufferState::Error) {
            st_.buffer = BufferState::Off;
            st_.bufferedSeconds = 0.0;
            st_.bufferBytes = 0;
        }
    }

    ClipLibrary& lib_;

    // Shared with the UI thread (mutex_)
    mutable std::mutex mutex_;
    std::condition_variable cmdCv_;
    std::deque<Cmd> cmds_;
    CaptureConfig cfg_;
    bool configured_ = false;
    EngineStatus st_;
    std::deque<EngineEvent> events_;
    std::vector<EncoderInfo> encoders_;
    std::wstring ffmpeg_;
    std::map<uint64_t, std::shared_ptr<std::atomic<bool>>> cancels_;

    // Segments (segMutex_), shared with the worker and save jobs
    std::mutex segMutex_;
    std::condition_variable segCv_;
    std::deque<Segment> segments_;
    std::atomic<bool> pruneRequested_{ false };

    // Prepared writer (prepMutex_)
    std::mutex prepMutex_;
    std::unique_ptr<MediaWriter> prepared_;
    std::wstring preparedPath_;
    FramePath preparedPathKind_ = FramePath::GpuSurfaces;
    bool preparing_ = false;

    // Session thread only
    std::wstring bufferDir_;
    CaptureConfig scfg_;
    std::unique_ptr<Worker> worker_;
    std::unique_ptr<DesktopCapture> capture_;
    std::unique_ptr<AudioInput> sysIn_, micIn_;
    std::vector<Track> tracks_;
    std::unique_ptr<MediaWriter> writer_, prev_;
    std::wstring curPath_;
    std::vector<uint8_t> frameBuf_;
    std::vector<float> mix_;
    bool session_ = false, wantBuffer_ = false, forceCut_ = false, restartPending_ = false;
    bool recActive_ = false, recStartPending_ = false, recStopPending_ = false, reconfigAfterRec_ = false;
    std::wstring recGame_;
    double recWall_ = 0.0, recSessionStart_ = 0.0;
    int recFirstSeg_ = 0;
    std::vector<PendingSave> pendingSaves_;
    int sessionId_ = 0, segNum_ = 0, segId_ = 0, curSegId_ = 0, prevSegId_ = 0, fps_ = 60, gpuFailures_ = 0;
    double sessionStart_ = 0.0, bufferSince_ = 0.0, retryAt_ = 0.0, prevSegStart_ = 0.0, prevSince_ = 0.0;
    int64_t frameIdx_ = 0, segStartFrame_ = 0, segFrames_ = 240, prevBoundary_ = 0;
    uint32_t dropped_ = 0;
    int failures_ = 0;
    std::string lastErrorMsg_;
    double lastErrorAt_ = -1e9;
    int fpsCount_ = 0;
    double fpsWindowStart_ = 0.0, lastStats_ = -1.0;
    float measuredFps_ = 0.0f;

    std::thread thread_;
    std::atomic<bool> quit_{ false };
    std::atomic<uint64_t> nextJob_{ 1 };
    std::atomic<int> activeJobs_{ 0 };
    std::mutex jobsMutex_;
    std::vector<JobThread> jobs_;
};

} // namespace

std::unique_ptr<IClipEngine> CreateClipEngine(ClipLibrary& library)
{
    return std::make_unique<RealClipEngine>(library);
}

} // namespace cs::engine
