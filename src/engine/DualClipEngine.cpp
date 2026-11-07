// "Both" capture mode: two independent engines run side by side, one on the game's window and
// one on the whole screen, each with its own replay buffer. Saving a clip saves both, as
// "<name>_game.mp4" and "<name>_screen.mp4". Everything else (exports, encoders, the clip list)
// goes to the main engine.
//
// If the second engine can't run (most often: the GPU encoder refuses a second session), it's
// stopped and the user is told that only the game is being recorded.
#include "engine/ClipEngine.h"
#include "core/Log.h"
#include "core/Util.h"

#include <memory>
#include <string>

namespace cs::engine {

namespace {

class DualClipEngine final : public IClipEngine {
public:
    explicit DualClipEngine(ClipLibrary& lib) : lib_(lib), main_(CreateClipEngine(lib, L"Buffer")) {}

    void Configure(const CaptureConfig& cfg) override
    {
        cfg_ = cfg;
        const bool both = cfg.source.rfind("both:", 0) == 0;
        CaptureConfig m = cfg;
        if (both) {
            m.source = "game";
            m.fileSuffix = L"_game";
        }
        main_->Configure(m);
        if (both && !failed_) {
            CaptureConfig s = cfg;
            s.source = "monitor:" + cfg.source.substr(5);
            s.fileSuffix = L"_screen";
            if (!second_) {
                second_ = CreateClipEngine(lib_, L"Buffer2");
                second_->Configure(s);
                if (buffer_) {
                    second_->StartBuffer();
                    secondStarted_ = NowSeconds();
                }
            } else {
                second_->Configure(s);
            }
        } else if (!both) {
            DropSecond();
            failed_ = false;   // leaving Both mode clears a past failure, so it can be retried
        }
    }

    void StartBuffer() override
    {
        buffer_ = true;
        main_->StartBuffer();
        if (second_) {
            second_->StartBuffer();
            secondStarted_ = NowSeconds();
        }
    }
    void StopBuffer() override
    {
        buffer_ = false;
        main_->StopBuffer();
        if (second_) second_->StopBuffer();
    }
    void SaveClip(int seconds, const std::wstring& game, const std::wstring& folder) override
    {
        main_->SaveClip(seconds, game, folder);
        if (second_ && folder.empty()) second_->SaveClip(seconds, game, folder);   // suggestions: the game view only
    }
    void StartRecording(const std::wstring& game) override
    {
        main_->StartRecording(game);
        if (second_) second_->StartRecording(game);
    }
    void StopRecording() override
    {
        main_->StopRecording();
        if (second_) second_->StopRecording();
    }

    uint64_t Export(const ExportRequest& req) override { return main_->Export(req); }
    void CancelExport(uint64_t job) override { main_->CancelExport(job); }
    void ProbeEncoders() override { main_->ProbeEncoders(); }
    std::vector<EncoderInfo> Encoders() const override { return main_->Encoders(); }
    std::shared_ptr<const ClipList> ListClips() const override { return main_->ListClips(); }

    EngineStatus Status() const override
    {
        EngineStatus s = main_->Status();
        if (second_) {
            const EngineStatus b = second_->Status();
            s.bufferBytes += b.bufferBytes;
            s.sourceName += " + Screen";
        }
        return s;
    }

    bool PollEvent(EngineEvent& out) override
    {
        if (!pending_.message.empty()) {   // the "fell back to one stream" notice
            out = pending_;
            pending_ = EngineEvent{};
            return true;
        }
        if (main_->PollEvent(out)) return true;
        if (!second_) return false;
        EngineEvent e;
        while (second_->PollEvent(e)) {
            switch (e.type) {
            case EngineEventType::ClipSaved:   // the screen copy of the clip: the library picks it up
                out = e;
                return true;
            case EngineEventType::Error:
                // A failure soon after starting means the GPU couldn't run two recordings.
                Log("Both mode, screen engine: %s", e.message.c_str());
                if (NowSeconds() - secondStarted_ < 8.0 || second_->Status().buffer == BufferState::Error) {
                    Fallback();
                    out = pending_;
                    pending_ = EngineEvent{};
                    return true;
                }
                out = e;
                out.message = "Screen recording: " + e.message;
                return true;
            default:   // recording started/stopped, probes, info: the main engine reports those
                break;
            }
        }
        return false;
    }

private:
    void DropSecond()
    {
        if (!second_) return;
        second_->StopBuffer();
        second_.reset();   // joins its thread and clears its buffer folder
    }

    void Fallback()
    {
        DropSecond();
        failed_ = true;
        pending_.type = EngineEventType::Error;
        pending_.message =
            "Both mode needs two recordings at once and your GPU encoder couldn't start the second one, so only the game is "
            "being recorded. Pick Game only or Whole screen in Settings > Recording to stop seeing this.";
    }

    ClipLibrary& lib_;
    std::unique_ptr<IClipEngine> main_, second_;
    CaptureConfig cfg_;
    bool buffer_ = false, failed_ = false;
    double secondStarted_ = 0.0;
    EngineEvent pending_;
};

} // namespace

std::unique_ptr<IClipEngine> CreateCaptureEngine(ClipLibrary& library) { return std::make_unique<DualClipEngine>(library); }

} // namespace cs::engine
