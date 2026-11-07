#include "ui/App.h"
#include "ui/AppContext.h"
#include "ui/Anim.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/Widgets.h"
#include "core/ClipLibrary.h"
#include "core/Hotkeys.h"
#include "core/Log.h"
#include "core/MediaPlayer.h"
#include "core/Platform.h"
#include "core/Screenshot.h"
#include "core/Settings.h"
#include "core/SystemStats.h"
#include "core/Thumbnailer.h"
#include "core/Toast.h"
#include "core/Bookmarks.h"
#include "core/Tray.h"
#include "core/Util.h"
#include "core/Window.h"
#include "core/GlassRenderer.h"
#include "engine/AudioCapture.h"
#include "engine/ClipEngine.h"
#include "engine/Encoder.h"
#include "media/MediaCache.h"
#include "AppInfo.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdio>
#include <string>

namespace cs::ui {

namespace {
bool g_uiSoundsOn = false;
void UiSound(int kind)
{
    if (g_uiSoundsOn) platform::PlayUiSound(kind);
}
} // namespace

struct App::Impl {
    void* hinst = nullptr;
    std::wstring dataDir;
    std::wstring settingsPath;
    Settings settings;
    SettingsSaver saver;

    Window window;
    GlassRenderer glass;
    Updater updater;
    Account account;
    InputOverlay inputOverlay;
    double updateCheckAt = 12.0;   // seconds after start
    Hotkeys hotkeys;
    Tray tray;
    Toasts toasts;
    // Bookmarks: marks in wall-clock time, matched to clips when they're saved.
    BookmarkLog marks;
    double lastMarkAt = -10.0, lastSaveAt = 0.0, recStartAt = 0.0, recStopAt = 0.0;
    bool lastMarkSaved = false;
    void OnBookmarkHotkey();
    Thumbnailer thumbs;
    Filmstrips strips;
    ImageCache images;
    ClipLibrary library;
    MediaPlayer player;
    Screenshotter shots;
    SystemStats stats;
    engine::AudioCapture audio;
    std::unique_ptr<engine::IClipEngine> engine;

    UiState ui;
    bool imguiReady = false;
    bool running = true;
    uint64_t frame = 0;
    uint64_t captureStartFrame = 0;

    bool savePending = false;
    double saveAt = 0.0;
    double openedAt = -1.0;               // window-open fade/scale start
    bool engineConfigPending = false;
    double engineConfigAt = 0.0;
    double lastTray = 0.0;
    double lastMonitors = -1e9;
    double lastBufferStart = -1e9;
    std::vector<platform::MonitorDesc> monitors;

    struct PendingShot {
        bool active = false;
        ShotMode mode = ShotMode::FullScreen;
        double at = 0.0;
    } pendingShot;
    // "Clip saved" toasts wait (briefly) for the clip's poster frame so they can show a thumbnail.
    struct PendingToast {
        std::wstring path;
        int64_t modified = 0;
        std::string title, body;
        double deadline = 0.0;
    };
    std::vector<PendingToast> pendingToasts;
    bool restoreAfterShot = false;
    bool shownOnce = false;
    bool startMaximized = false;

    bool Init(void* hinstance, bool startHidden);
    void Shutdown();
    void Frame();
    void Tick();
    void DrawRoot(AppContext& ctx);
    void ApplyOpenTransition(ImDrawData* dd);

    engine::CaptureConfig BuildConfig() const;
    LibraryPolicy Policy() const;
    void ApplyThemeNow();
    void ApplyAudio();
    void UpdateMicUse();
    void ApplyChanges();
    void ProcessActions();
    void ProcessKeyCapture();
    void RequestSave();
    void SaveNow();
    void Toast(ToastKind kind, const std::string& title, const std::string& body, const char* icon = nullptr, const std::wstring& image = L"");
    void Sound(platform::Sound s);
    std::wstring CurrentGame();
    std::string GameFromPath(const std::wstring& path) const;

    void OnHotkey(HotkeyAction a);
    void OnTray(uint32_t msg);
    void OnEngineEvent(const engine::EngineEvent& e);
    void OnShot(const ShotResult& r);
    void OnLibraryResult(const LibraryOpResult& r);
    void SaveReplay();
    void ToggleRecording();
    void ToggleBuffer(bool announce);
    void TakeScreenshot(ShotMode mode, bool fromUi);
    void DoScreenshot(ShotMode mode);
    void ShowMainWindow();
    void HideToTray();
    void CloseRequested();
    void Quit();
    AppContext MakeContext();
};

App::App() : impl_(std::make_unique<Impl>()) {}
App::~App() = default;
bool App::Init(void* hinstance, bool startHidden) { return impl_->Init(hinstance, startHidden); }
void App::Shutdown() { impl_->Shutdown(); }

int App::Run()
{
    Impl& d = *impl_;
    while (d.running) {
        if (!Window::PumpMessages()) break;
        if (!d.running) break;
        d.Tick();
        const bool visible = d.window.IsVisible() && !d.window.Minimized();
        if (!visible) {
            Window::WaitForMessages(100);   // hidden: no rendering at all
            continue;
        }
        d.Frame();
        if (!d.window.IsFocused()) Window::WaitForMessages(45);   // background (e.g. while gaming): ~20 fps
    }
    return 0;
}

// ---------------------------------------------------------------------------------------------
// Init / shutdown
// ---------------------------------------------------------------------------------------------

bool App::Impl::Init(void* hinstance, bool startHidden)
{
    hinst = hinstance;
    dataDir = AppDataDir();
    const std::wstring logDir = JoinPath(dataDir, L"logs");
    EnsureDir(logDir);
    LogInit(JoinPath(logDir, L"replayr.log"));
    Log("%s %s starting (Windows build %u)", CS_APP_NAME, CS_APP_VERSION, platform::WindowsBuildNumber());

    settingsPath = JoinPath(dataDir, L"settings.json");
    std::string err;
    if (!LoadSettings(settingsPath, settings, &err)) Log("Settings: %s - using defaults", err.c_str());
    EnsureDir(settings.ClipsFolder());
    saver.Start(settingsPath);
    if (settings.general.startWithWindows) platform::SetStartWithWindows(true);   // refresh the exe path

    // Last page and sort order.
    const int lp = Clamp(settings.ui.lastPage, 0, (int)Page::Count - 1);
    ui.page = (Page)lp == Page::Screenshots ? Page::Screenshots : Page::Clips;
    ui.library.clipSort = Clamp(settings.ui.clipSort, 0, 4);
    ui.library.shotSort = Clamp(settings.ui.shotSort, 0, 4);

    // Window: restore the last position if it is still on a monitor, else centre on the primary one.
    const bool hidden = startHidden || settings.general.startMinimized;
    int x, y, w, h;
    const WindowState& ws = settings.window;
    if (ws.w > 0 && ws.h > 0 && ws.x != INT_MIN && platform::RectOnAnyMonitor(ws.x, ws.y, ws.w, ws.h)) {
        x = ws.x;
        y = ws.y;
        w = ws.w;
        h = ws.h;
    } else {
        int ax, ay, aw, ah;
        platform::PrimaryWorkArea(ax, ay, aw, ah);
        const float s = platform::SystemDpiScale();
        w = std::min((int)(1380 * s), aw - 40);
        h = std::min((int)(880 * s), ah - 40);
        x = ax + (aw - w) / 2;
        y = ay + (ah - h) / 2;
    }
    startMaximized = ws.maximized;
    if (!window.Create(hinst, CS_APP_NAME_W, x, y, w, h, false, false)) {
        platform::ShowErrorBox(L"Replayr couldn't create its window or start DirectX 11.\nSee the log in %APPDATA%\\Replayr\\logs.");
        return false;
    }
    window.callbacks.onHotkey = [this](int id) {
        HotkeyAction a;
        if (Hotkeys::ActionFromId(id, a)) OnHotkey(a);
    };
    window.callbacks.onTrayMessage = [this](uint32_t msg) { OnTray(msg); };
    window.callbacks.onTaskbarCreated = [this] { tray.Recreate(); };
    window.callbacks.onCloseRequested = [this] { CloseRequested(); };
    window.callbacks.onShowRequest = [this] { ShowMainWindow(); };
    window.callbacks.onRenderWhileSizing = [this] {
        if (imguiReady) Frame();
    };
    window.callbacks.onEndSession = [this] { WriteFileAtomic(settingsPath, SerializeSettings(settings)); };

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;              // layout is code-driven
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    ImGui_ImplWin32_Init(window.Hwnd());
    ImGui_ImplDX11_Init(window.Device(), window.Context());
    if (!glass.Init(window.Device(), window.Context())) Log("Glass renderer unavailable; using the simple backdrop");
    updater.CleanupOld();
    LoadFonts();
    toasts.Init(hinst);
    ApplyThemeNow();
    imguiReady = true;

    thumbs.Init(window.Device());
    strips.Init(window.Device(), JoinPath(LocalDataDir(), L"Cache"));
    images.Init(window.Device());
    if (!settings.general.accountServer.empty()) Account::SetServer(settings.general.accountServer);
    account.Start(JoinPath(LocalDataDir(), L"Cache"));
    inputOverlay.Start();
    player.Init(window.Device());
    library.Start(settings.ClipsFolder(), JoinPath(dataDir, L"library.json"));
    library.SetPolicy(Policy());
    stats.Start(settings.ClipsFolder());
    shots.Init(hinst, window.Hwnd());
    audio.Start();
    ApplyAudio();

    engine = engine::CreateCaptureEngine(library);
    engine->Configure(BuildConfig());
    if (settings.replay.enabled) engine->StartBuffer();

    hotkeys.Apply(window.Hwnd(), settings.hotkeys);
    for (int i = 0; i < kHotkeyCount; ++i)
        if (hotkeys.Status((HotkeyAction)i) == HotkeyStatus::InUse) {
            Toast(ToastKind::Error, "A hotkey is taken", std::string(HotkeyActionLabel((HotkeyAction)i)) + " \xE2\x80\x94 change it in Settings > Hotkeys");
            break;
        }

    tray.Create(window.Hwnd(), hinst, CS_APP_NAME_W);
    monitors = platform::EnumerateMonitors();
    lastMonitors = NowSeconds();

    if (hidden) {
        Toast(ToastKind::Info, "Replayr is running", HotkeyText(MakeContext(), HotkeyAction::SaveReplay) + " saves a clip");
    } else {
        Frame();
        ShowMainWindow();
    }
    Log("Ready");
    return true;
}

void App::Impl::Shutdown()
{
    if (window.Hwnd()) {
        int x, y, w, h;
        bool maximized;
        window.GetNormalRect(x, y, w, h, maximized);
        settings.window = WindowState{ x, y, w, h, maximized };
    }
    engine.reset();   // joins capture and export jobs (they reference the library)
    audio.Stop();
    shots.Shutdown();
    player.Shutdown();
    images.Shutdown();
    strips.Shutdown();
    thumbs.Shutdown();
    library.Stop();
    stats.Stop();
    hotkeys.UnregisterAll();
    tray.Remove();
    toasts.Shutdown();
    if (imguiReady) {
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        imguiReady = false;
    }
    window.Destroy();
    saver.Save(settings);
    saver.Stop();
    Log("Bye");
    LogShutdown();
}

// ---------------------------------------------------------------------------------------------
// Per-frame work
// ---------------------------------------------------------------------------------------------

AppContext App::Impl::MakeContext()
{
    AppContext ctx{ settings, window, *engine, audio, library, thumbs, strips, images, player, hotkeys, ui };
    ctx.status = engine->Status();
    ctx.levels = audio.Levels();
    ctx.system = stats.Get();
    ctx.monitors = monitors;
    ctx.updater = &updater;
    ctx.account = Account::Enabled() ? &account : nullptr;
    ctx.inputOverlay = &inputOverlay;
    return ctx;
}

void App::Impl::Tick()
{
    const double now = NowSeconds();
    engine::EngineEvent ev;
    while (engine->PollEvent(ev)) OnEngineEvent(ev);
    ShotResult shot;
    while (shots.PollResult(shot)) OnShot(shot);
    AccountEvent ae;
    while (account.PollEvent(ae)) {
        switch (ae.type) {
        case AccountEventType::Uploaded: {
            const bool copied = platform::CopyTextToClipboard(window.Hwnd(), ae.message);
            Snack(ui, copied ? "Uploaded \"" + ae.title + "\" \xC2\xB7 link copied" : "Uploaded \"" + ae.title + "\"");
            Sound(platform::Sound::ClipSaved);
            break;
        }
        case AccountEventType::UploadFailed: Snack(ui, "Couldn't upload \"" + ae.title + "\": " + ae.message, L"", true); break;
        case AccountEventType::SignInFailed:
        case AccountEventType::Error: Snack(ui, ae.message, L"", true); break;
        default: Snack(ui, ae.message); break;
        }
    }
    LibraryOpResult lr;
    while (library.PollResult(lr)) OnLibraryResult(lr);

    if (pendingShot.active && now >= pendingShot.at) {
        pendingShot.active = false;
        DoScreenshot(pendingShot.mode);
    }
    for (auto it = pendingToasts.begin(); it != pendingToasts.end();) {
        const std::wstring poster = strips.PosterPath(it->path, it->modified);
        if (!poster.empty() || now >= it->deadline) {
            Toast(ToastKind::Success, it->title, it->body, ICON_SAVE, poster);
            it = pendingToasts.erase(it);
        } else {
            ++it;
        }
    }

    // The replay buffer follows the setting (with a retry delay if the engine reports an error).
    const engine::EngineStatus st = engine->Status();
    const bool on = st.buffer != engine::BufferState::Off && st.buffer != engine::BufferState::Error;
    if (settings.replay.enabled && !on && now - lastBufferStart > 5.0) {
        engine->StartBuffer();
        lastBufferStart = now;
    } else if (!settings.replay.enabled && on) {
        engine->StopBuffer();
    }
    if (engineConfigPending && now >= engineConfigAt) {
        engineConfigPending = false;
        engine->Configure(BuildConfig());
    }
    if (savePending && now >= saveAt) SaveNow();

    if (now - lastTray > 1.0) {
        lastTray = now;
        std::wstring tip = CS_APP_NAME_W;
        if (st.recording) tip += L" · Recording " + ToWide(FormatDuration(st.recordingSeconds));
        else if (st.buffer == engine::BufferState::Ready) tip += L" · Replay ready (" + ToWide(FormatDuration(st.bufferedSeconds)) + L")";
        else if (on) tip += L" · Replay buffering";
        else tip += L" · Replay off";
        tray.SetTooltip(tip);
    }
    {
        const InputDisplaySettings& d = settings.inputDisplay;
        InputOverlayConfig c;
        c.enabled = d.enabled;
        c.mode = d.mode;
        c.corner = d.corner;
        c.size = d.size;
        c.show = d.show;
        c.opacity = d.opacity;
        c.gamesOnly = d.gamesOnly;
        for (int i = 0; i < 3; ++i) c.accent[i] = settings.general.accent[i];
        inputOverlay.SetConfig(c);
    }
    static const double started = now;
    if (updateCheckAt > 0.0 && now - started > updateCheckAt) {
        updateCheckAt = 0.0;
        if (Updater::Enabled() && settings.general.autoUpdate) updater.Check(true);
    }
    if (now - lastMonitors > 5.0) {
        lastMonitors = now;
        monitors = platform::EnumerateMonitors();
    }
    UpdateMicUse();
}

void App::Impl::Frame()
{
    if (window.TakeDpiChanged()) ui.themeDirty = true;
    if (ui.themeDirty) {
        ApplyThemeNow();
        ui.themeDirty = false;
    }
    // Background + blur textures for this window size; the UI samples them while it builds the frame.
    glass.Prepare(window.Width(), window.Height());
    {
        GlassSource src;
        src.background = (ImTextureID)glass.Background();
        src.blurred = (ImTextureID)glass.Blurred();
        src.strong = (ImTextureID)glass.BlurredStrong();
        src.origin = ImVec2(0, 0);
        src.size = ImVec2((float)std::max(1, window.Width()), (float)std::max(1, window.Height()));
        SetGlassSource(src);
    }
    g_uiSoundsOn = settings.general.uiSounds;
    SetUiSoundHook(&UiSound);
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    window.inFrame = true;
    ++frame;
    AnimNewFrame();
    thumbs.NewFrame();
    strips.NewFrame();
    player.Update();
    ProcessKeyCapture();

    AppContext ctx = MakeContext();
    DrawRoot(ctx);

    // A click anywhere cancels an in-progress key capture.
    KeyCapture& kc = window.Capture();
    if (ui.settings.capturing >= 0 && kc.active && frame > captureStartFrame + 1 &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        kc = KeyCapture{};
        ui.settings.capturing = -1;
        hotkeys.Suspend(false);
    }

    ImGui::Render();
    ApplyOpenTransition(ImGui::GetDrawData());
    {
        const GeneralSettings& g = settings.general;
        GlassFrameParams gp;
        gp.time = ImGui::GetTime();
        const ImVec2 mp = ImGui::GetIO().MousePos, ds = ImGui::GetIO().DisplaySize;
        static float mouse[2] = { 0.5f, 0.5f };
        if (ImGui::IsMousePosValid(&mp) && ds.x > 0 && ds.y > 0) {
            // Ease the cursor so the blobs drift rather than jump.
            const float k = 1.0f - std::exp(-2.0f * ImGui::GetIO().DeltaTime);
            mouse[0] += (std::clamp(mp.x / ds.x, 0.0f, 1.0f) - mouse[0]) * k;
            mouse[1] += (std::clamp(mp.y / ds.y, 0.0f, 1.0f) - mouse[1]) * k;
        }
        gp.mouse[0] = mouse[0];
        gp.mouse[1] = mouse[1];
        for (int i = 0; i < 3; ++i) {
            gp.accent[i] = g.accent[i];
            gp.accent2[i] = g.accent2[i];
        }
        gp.style = std::clamp(g.background, 0, 2);
        if (!g.animations && gp.style == 0) gp.style = 1;   // reduce motion: hold the gradient still
        gp.alpha = window.SupportsBackdrop() && g.backdrop != 0 ? 0.70f : 1.0f;
        gp.moving = window.IsFocused() && !window.Minimized();
        glass.Render(gp);
    }
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    window.BeginFrame(clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    window.Present(true);
    window.inFrame = false;

    ProcessActions();
    ApplyChanges();
}

void App::Impl::DrawRoot(AppContext& ctx)
{
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    DrawBackdrop(ImGui::GetBackgroundDrawList(), vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), (float)ImGui::GetTime());
    {   // Editor fullscreen covers the whole monitor (taskbar included), and is undone whenever we leave it.
        const bool wantFull = ui.page == Page::Editor && ui.editor.fullscreen;
        if (wantFull != window.IsFullscreen()) window.SetFullscreen(wantFull);
        if (ui.page != Page::Editor) ui.editor.fullscreen = false;
    }
    if (ui.page == Page::Editor && ui.editor.fullscreen) {
        // Fullscreen editor: the video fills the window; the caption strip is only the very top edge.
        CaptionLayout& cap = window.Caption();
        cap.interactive.clear();
        cap.maxButton = HitRect{};
        cap.height = 0.0f;
        ImGui::SetNextWindowPos(vp->Pos);
        ImGui::SetNextWindowSize(vp->Size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::Begin("##fullscreen", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                  ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                                  ImGuiWindowFlags_NoDocking);
        ImGui::PopStyleVar(3);
        DrawEditor(ctx);
        ImGui::End();
        LibraryOverlays(ctx);
        DrawSnack(ctx);
        return;
    }

    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##root", nullptr, flags);
    ImGui::PopStyleVar(3);

    const float W = vp->Size.x, H = vp->Size.y;
    const float m = S(12.0f);
    const float sideW = W < S(Tok::SidebarCollapseBelow) ? S(Tok::SidebarCollapsed) : S(Tok::SidebarWidth);
    const float animSide = Anim(ImGui::GetID("##sidew"), sideW, Motion::Slide);
    const float topH = S(Tok::TopBarHeight);
    const float colX = m + animSide + m;
    const float colW = W - colX - m;

    CaptionLayout& cap = window.Caption();
    cap.interactive.clear();
    cap.maxButton = HitRect{};
    cap.height = m + topH;

    ImGui::SetCursorPos(ImVec2(m, m));
    DrawSidebar(ctx, animSide, H - m * 2.0f);
    // The sidebar's widgets sit below the caption strip except the header, which has none.

    ImGui::SetCursorPos(ImVec2(colX, m));
    ImGui::BeginChild("##topbar", ImVec2(colW, topH), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    DrawTopBar(ctx, topH);
    ImGui::EndChild();

    // Content: a big glass sheet; the page scrolls inside it and fades/slides in on page changes.
    const float cy = m + topH + m;
    const ImVec2 ca(vp->Pos.x + colX, vp->Pos.y + cy), cb(ca.x + colW, vp->Pos.y + H - m);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    Shadow(dl, ca, cb, S(Tok::RadiusPanel), 0.8f);
    Glass(dl, ca, cb, S(Tok::RadiusPanel), 0.0f, Tok::GlassBorder);
    const float t = (float)(ImGui::GetTime() - ui.pageChangedAt);
    const float e = Animations() ? EaseOutCubic(t / 0.22f) : 1.0f;
    // The scrolling area is inset from the glass (top/bottom past the rounded corners, a little from the
    // right edge) so the scrollbar stays inside the panel instead of running over its border.
    const float insetY = S(Tok::RadiusPanel) * 0.7f, insetX = S(6.0f);
    ImGui::SetCursorPos(ImVec2(colX, cy + insetY));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(28.0f), std::max(0.0f, S(24.0f) - insetY)));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(Tok::ScrollbarSize));
    ImGui::BeginChild("##page", ImVec2(colW - insetX, H - cy - m - insetY * 2.0f), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar(2);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.0f + e);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + S(10.0f) * (1.0f - e));
    ImGui::PushID((int)ui.page);
    switch (ui.page) {
    case Page::Clips: DrawLibrary(ctx, false); break;
    case Page::Screenshots: DrawLibrary(ctx, true); break;
    case Page::Editor: DrawEditor(ctx); break;
    case Page::Uploads: DrawUploads(ctx); break;
    case Page::Settings: DrawSettings(ctx); break;
    default: break;
    }
    ImGui::PopID();
    ImGui::PopStyleVar();
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::EndChild();
    ImGui::End();

    DrawImageViewer(ctx);
    LibraryOverlays(ctx);
    DrawWelcome(ctx);
    DrawSnack(ctx);
}

// Fades and scales the whole UI in (0.98 -> 1.0) when the window opens, by transforming the
// final vertices: costs nothing outside the 220 ms after the window opens.
void App::Impl::ApplyOpenTransition(ImDrawData* dd)
{
    if (openedAt < 0.0 || !dd || !settings.general.animations) return;
    float t = (float)((NowSeconds() - openedAt) / 0.22);
    if (t >= 1.0f) {
        openedAt = -1.0;
        return;
    }
    const float e = EaseOutCubic(Clamp(t, 0.0f, 1.0f));
    const float scale = 0.98f + 0.02f * e;
    const ImVec2 c(dd->DisplayPos.x + dd->DisplaySize.x * 0.5f, dd->DisplayPos.y + dd->DisplaySize.y * 0.5f);
    for (ImDrawList* dl : dd->CmdLists) {
        for (ImDrawVert& v : dl->VtxBuffer) {
            v.pos.x = c.x + (v.pos.x - c.x) * scale;
            v.pos.y = c.y + (v.pos.y - c.y) * scale;
            const ImU32 a = (v.col >> IM_COL32_A_SHIFT) & 0xFF;
            v.col = (v.col & ~IM_COL32_A_MASK) | ((ImU32)(a * e) << IM_COL32_A_SHIFT);
        }
        for (ImDrawCmd& cmd : dl->CmdBuffer) {
            cmd.ClipRect.x = c.x + (cmd.ClipRect.x - c.x) * scale;
            cmd.ClipRect.y = c.y + (cmd.ClipRect.y - c.y) * scale;
            cmd.ClipRect.z = c.x + (cmd.ClipRect.z - c.x) * scale;
            cmd.ClipRect.w = c.y + (cmd.ClipRect.w - c.y) * scale;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Settings plumbing
// ---------------------------------------------------------------------------------------------

engine::CaptureConfig App::Impl::BuildConfig() const
{
    const Settings& s = settings;
    engine::CaptureConfig c;
    static const int heights[] = { 0, 2160, 1440, 1080, 720 };
    c.source = s.video.source;
    c.outputHeight = heights[Clamp(s.video.resolution, 0, 4)];
    c.fps = s.video.fps;
    c.bitrateKbps = s.video.bitrateMbps * 1000;
    c.encoder = engine::ParseEncoder(s.video.encoder);
    c.codec = engine::ParseCodec(s.video.codec);
    c.qualityPreset = s.video.quality;
    c.captureCursor = s.video.captureCursor;
    c.hideCaptureBorder = s.video.hideBorder;
    c.replaySeconds = s.replay.seconds;
    c.sampleRate = 48000;
    c.separateTracks = s.audio.separateTracks;
    c.captureSystem = s.audio.captureSystem;
    c.systemVolume = s.audio.systemMuted ? 0.0f : s.audio.systemVolume;
    c.captureMic = s.audio.captureMic;
    c.micVolume = s.audio.micMuted ? 0.0f : s.audio.micVolume;
    c.micFx.suppress = s.audio.noiseSuppression;
    c.micFx.strength = s.audio.suppressionStrength;
    c.micFx.gate = s.audio.noiseGate;
    c.micFx.gateDb = s.audio.gateDb;
    c.micFx.highPass = s.audio.highPass;
    c.keepRawMic = s.audio.keepRawMic;
    c.pushToTalk = s.audio.pushToTalk;
    c.pushToTalkKey = s.audio.pushToTalkKey;
    c.outputRoot = s.ClipsFolder();
    c.perGameFolders = s.storage.perGameFolders;
    c.ffmpegPath = s.general.ffmpegPath;
    c.outputDeviceId = s.audio.outputDeviceId;
    c.inputDeviceId = s.audio.inputDeviceId;
    return c;
}

LibraryPolicy App::Impl::Policy() const
{
    LibraryPolicy p;
    p.autoDelete = settings.storage.autoDelete;
    p.maxAgeDays = settings.storage.autoDeleteDays;
    p.maxBytes = (uint64_t)settings.storage.maxLibraryGB * 1024ull * 1024ull * 1024ull;
    return p;
}

void App::Impl::ApplyThemeNow()
{
    const GeneralSettings& g = settings.general;
    ThemeParams p;
    for (int i = 0; i < 3; ++i) {
        p.accent[i] = g.accent[i];
        p.accent2[i] = g.accent2[i];
    }
    p.animations = g.animations;
    p.background = g.background;
    const bool backdrop = window.SupportsBackdrop() && g.backdrop != 0;
    p.backdrop = backdrop;
    p.dpiScale = window.DpiScale();
    p.uiScale = g.uiScale;
    ApplyTheme(p);
    window.SetBackdrop(backdrop ? (Backdrop)g.backdrop : Backdrop::Gradient);
    ToastStyle ts;
    for (int i = 0; i < 3; ++i) {
        ts.accent[i] = g.accent[i];
        ts.accent2[i] = g.accent2[i];
    }
    ts.corner = g.toastCorner;
    toasts.SetStyle(ts);
}

void App::Impl::ApplyAudio()
{
    audio.SetDevices(settings.audio.outputDeviceId, settings.audio.inputDeviceId);
    audio.SetPushToTalk(settings.audio.pushToTalk, settings.audio.pushToTalkKey);
    UpdateMicUse();
}

void App::Impl::UpdateMicUse()
{
    // The meter's own mic stream only opens while the Audio settings (with the meters) are on screen,
    // so Windows doesn't show "microphone in use" the whole time. The engine opens its own stream.
    const bool metersVisible = window.IsVisible() && !window.Minimized() && ui.page == Page::Settings && ui.settings.category == 2;
    audio.SetMicEnabled(settings.audio.captureMic && metersVisible);
}

void App::Impl::ApplyChanges()
{
    const double now = NowSeconds();
    // Sort orders are remembered between launches.
    if (settings.ui.clipSort != ui.library.clipSort || settings.ui.shotSort != ui.library.shotSort) {
        settings.ui.clipSort = ui.library.clipSort;
        settings.ui.shotSort = ui.library.shotSort;
        ui.settingsDirty = true;
    }
    if (ui.hotkeysDirty) {
        ui.hotkeysDirty = false;
        if (!hotkeys.Suspended()) hotkeys.Apply(window.Hwnd(), settings.hotkeys);
    }
    if (ui.audioDirty) {
        ui.audioDirty = false;
        ApplyAudio();
    }
    if (ui.storageDirty) {
        ui.storageDirty = false;
        EnsureDir(settings.ClipsFolder());
        library.SetRoot(settings.ClipsFolder());
        library.SetPolicy(Policy());
        stats.SetDiskPath(settings.ClipsFolder());
    }
    {   // Keep Replayr's window out of its own recordings (Windows 10 2004+; ignored before).
        static int affinity = -1;
        const int want = settings.video.hideApp ? 0x11 /* WDA_EXCLUDEFROMCAPTURE */ : 0;
        if (want != affinity && window.Hwnd()) {
            platform::ExcludeFromCapture(window.Hwnd(), want != 0);
            affinity = want;
        }
    }
    if (ui.engineDirty) {
        ui.engineDirty = false;
        engineConfigPending = true;   // debounced: sliders fire every frame while dragged
        engineConfigAt = now + 0.25;
    }
    if (ui.startupDirty) {
        ui.startupDirty = false;
        if (!platform::SetStartWithWindows(settings.general.startWithWindows)) Toast(ToastKind::Error, "Couldn't change the startup setting", "");
    }
    if (ui.settingsDirty) {
        ui.settingsDirty = false;
        if (ui.page == Page::Settings) ui.settings.savedAt = ImGui::GetTime();
        RequestSave();
    }
}

void App::Impl::RequestSave()
{
    savePending = true;
    saveAt = NowSeconds() + 0.6;
}

void App::Impl::SaveNow()
{
    savePending = false;
    saver.Save(settings);
}

void App::Impl::ProcessKeyCapture()
{
    KeyCapture& kc = window.Capture();
    int& slot = ui.settings.capturing;
    if (slot < 0 || !kc.done) return;
    if (!kc.cancelled) {
        if (slot == kPushToTalkSlot) {
            if (!kc.cleared) settings.audio.pushToTalkKey = kc.vk;
            ui.audioDirty = true;
            ui.engineDirty = true;
        } else if (slot < kHotkeyCount) {
            const Hotkey k = kc.cleared ? Hotkey{} : Hotkey{ kc.vk, kc.mods };
            settings.hotkeys[(size_t)slot] = k;
            // Conflicts: a key can only do one thing. Take it off the other action and say so.
            if (k.vk)
                for (int j = 0; j < kHotkeyCount; ++j)
                    if (j != slot && settings.hotkeys[(size_t)j] == k) {
                        settings.hotkeys[(size_t)j] = Hotkey{};
                        Snack(ui, std::string("That key was used for \"") + HotkeyActionLabel((HotkeyAction)j) + "\", so it's been removed from there.");
                    }
            if (k.vk && settings.audio.pushToTalk && settings.audio.pushToTalkKey == k.vk && !k.mods) {
                Snack(ui, "That key is also your push-to-talk key: both will happen when you press it.");
            }
        }
        ui.settingsDirty = true;
    }
    kc = KeyCapture{};
    slot = -1;
    hotkeys.Suspend(false);
    hotkeys.Apply(window.Hwnd(), settings.hotkeys);
}

void App::Impl::ProcessActions()
{
    std::vector<Action> actions;
    actions.swap(ui.actions);
    for (Action a : actions) {
        switch (a) {
        case Action::SaveReplay: SaveReplay(); break;
        case Action::ToggleRecording: ToggleRecording(); break;
        case Action::ToggleBuffer: ToggleBuffer(false); break;
        case Action::ScreenshotFull: TakeScreenshot(ShotMode::FullScreen, true); break;
        case Action::ScreenshotWindow: TakeScreenshot(ShotMode::ActiveWindow, true); break;
        case Action::ScreenshotRegion: TakeScreenshot(ShotMode::Region, true); break;
        case Action::OpenClipsFolder:
            EnsureDir(settings.ClipsFolder());
            platform::OpenPath(settings.ClipsFolder());
            break;
        case Action::BrowseClipsFolder: {
            std::wstring folder = settings.ClipsFolder();
            if (platform::PickFolder(window.Hwnd(), folder)) {
                settings.storage.outputFolder = folder;
                ui.storageDirty = ui.engineDirty = ui.settingsDirty = true;
            }
            break;
        }
        case Action::CheckUpdates: updater.Check(true); break;
        case Action::ApplyUpdate:
            if (updater.Apply()) {
                SaveNow();
                window.Close();
            } else {
                Snack(ui, "Couldn't install the update. Try running Replayr from a folder you can write to.", L"", true);
            }
            break;
        case Action::FinishSetup:
            settings.general.setupDone = true;
            RequestSave();
            break;
        case Action::OpenLogFolder: platform::OpenPath(JoinPath(dataDir, L"logs")); break;
        case Action::TestToast: {
            // Use the newest screenshot or clip poster, if any, so the preview looks real.
            std::wstring image;
            if (auto snap = library.Snapshot())
                for (const ClipInfo& c : *snap)
                    if (c.type == ClipType::Image) {
                        image = c.path;
                        break;
                    }
            Toast(ToastKind::Success, "Clip saved \xC2\xB7 0:30", "This is how notifications look", ICON_SAVE, image);
            Sound(platform::Sound::ClipSaved);
            break;
        }
        case Action::StartKeyCapture: {
            KeyCapture& kc = window.Capture();
            kc = KeyCapture{};
            kc.active = true;
            hotkeys.Suspend(true);   // otherwise pressing an existing combo would fire it instead
            captureStartFrame = frame;
            break;
        }
        case Action::Minimize:
            if (settings.general.minimizeToTray) HideToTray();
            else window.Minimize();
            break;
        case Action::ToggleMaximize: window.ToggleMaximize(); break;
        case Action::Close: CloseRequested(); break;
        case Action::Quit: Quit(); break;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------------------------

void App::Impl::Toast(ToastKind kind, const std::string& title, const std::string& body, const char* icon, const std::wstring& image)
{
    if (settings.general.toasts) toasts.Show(kind, title, body, icon, image);
}

void App::Impl::Sound(platform::Sound s)
{
    if (settings.general.sounds) platform::PlaySoundCue(s);
}

std::wstring App::Impl::CurrentGame() { return platform::ForegroundAppName(window.Hwnd()); }

std::string App::Impl::GameFromPath(const std::wstring& path) const
{
    const std::wstring parent = ParentDir(path);
    if (_wcsicmp(parent.c_str(), settings.ClipsFolder().c_str()) == 0) return "Desktop";
    return ToUtf8(FileStem(parent + L".x"));   // last folder name
}

void App::Impl::OnHotkey(HotkeyAction a)
{
    switch (a) {
    case HotkeyAction::SaveReplay: SaveReplay(); break;
    case HotkeyAction::ToggleRecording: ToggleRecording(); break;
    case HotkeyAction::ToggleReplayBuffer: ToggleBuffer(true); break;
    case HotkeyAction::Screenshot: TakeScreenshot(ShotMode::FullScreen, false); break;
    case HotkeyAction::ScreenshotRegion: TakeScreenshot(ShotMode::Region, false); break;
    case HotkeyAction::ScreenshotWindow: TakeScreenshot(ShotMode::ActiveWindow, false); break;
    case HotkeyAction::Bookmark: OnBookmarkHotkey(); break;
    default: break;
    }
}

// Tap: mark the moment. Double tap (within 400 ms): mark and save a clip. Repeats within a
// second are ignored, so a held or bouncing key doesn't flood the timeline.
void App::Impl::OnBookmarkHotkey()
{
    const double now = NowSeconds();
    const engine::EngineStatus st = engine->Status();
    const bool live = st.recording || st.buffer == engine::BufferState::Filling || st.buffer == engine::BufferState::Ready;
    if (!live) {
        Toast(ToastKind::Info, "Nothing to bookmark", "Turn on the replay buffer or start a recording first", ICON_INFO);
        return;
    }
    const double since = now - lastMarkAt;
    if (since < 0.4 && !lastMarkSaved) {
        lastMarkSaved = true;
        SaveReplay();
        return;   // the "Clip saved" toast follows
    }
    if (since < 1.0) return;
    lastMarkAt = now;
    lastMarkSaved = false;
    marks.Add(now, ToUtf8(CurrentGame()));
    Sound(platform::Sound::Bookmark);
    const std::string when = st.recording ? FormatDuration(st.recordingSeconds) + " into the recording" : TimeOfDayText();
    Toast(ToastKind::Info, "Marked \xC2\xB7 " + when, "Tap twice to save a clip", ICON_BOOKMARK);
}

void App::Impl::SaveReplay()
{
    lastSaveAt = NowSeconds();
    engine->SaveClip(0, CurrentGame());
}

void App::Impl::ToggleRecording()
{
    if (engine->Status().recording) {
        recStopAt = NowSeconds();
        engine->StopRecording();
    } else {
        engine->StartRecording(CurrentGame());
    }
}

void App::Impl::ToggleBuffer(bool announce)
{
    settings.replay.enabled = !settings.replay.enabled;
    RequestSave();
    if (announce) Toast(ToastKind::Info, settings.replay.enabled ? "Replay buffer on" : "Replay buffer off", "", ICON_HISTORY);
}

void App::Impl::TakeScreenshot(ShotMode mode, bool fromUi)
{
    if (shots.SelectingRegion() || pendingShot.active) return;
    if (fromUi && window.IsVisible() && !window.Minimized()) {
        // Get our own window out of the picture first.
        window.Hide();
        restoreAfterShot = true;
        pendingShot.active = true;
        pendingShot.mode = mode;
        pendingShot.at = NowSeconds() + 0.25;
        return;
    }
    DoScreenshot(mode);
}

void App::Impl::DoScreenshot(ShotMode mode)
{
    const std::wstring game = CurrentGame();
    ShotRequest r;
    r.mode = mode;
    r.folder = settings.storage.perGameFolders ? JoinPath(settings.ClipsFolder(), SanitizeFileName(game)) : settings.ClipsFolder();
    r.baseName = game;
    r.copyToClipboard = settings.storage.screenshotToClipboard;
    r.jpeg = settings.storage.screenshotFormat == 1;
    for (int i = 0; i < 3; ++i) r.accent[i] = settings.general.accent[i];
    r.banner = settings.banner.screenshots;
    r.bannerStyle = BannerStyleFor(settings);
    shots.Capture(r);
}

void App::Impl::OnShot(const ShotResult& r)
{
    if (restoreAfterShot) {
        restoreAfterShot = false;
        ShowMainWindow();
    }
    if (r.cancelled) return;
    if (!r.ok) {
        Sound(platform::Sound::Error);
        Toast(ToastKind::Error, "Screenshot failed", r.error);
        return;
    }
    Sound(platform::Sound::Screenshot);
    char body[160];
    snprintf(body, sizeof(body), "%s  \xC2\xB7  %d\xC3\x97%d%s", GameFromPath(r.path).c_str(), r.width, r.height, r.copied ? "  \xC2\xB7  copied" : "");
    Toast(ToastKind::Success, "Screenshot saved", body, ICON_CAMERA, r.path);
    library.Rescan();
}

void App::Impl::OnLibraryResult(const LibraryOpResult& r)
{
    EditorState& ed = ui.editor;
    const bool editorClip = ed.hasClip && r.oldPath == ed.clip.path;
    if (editorClip) ed.holdPlayer = false;
    if (!r.ok) {
        Snack(ui, r.message, L"", true);
        if (editorClip) snprintf(ed.title, sizeof(ed.title), "%s", ed.clip.title.c_str());
        return;
    }
    Snack(ui, r.message);
    // Keep the editor pointing at a renamed clip (the player reopens it from the new path).
    if (editorClip && !r.newPath.empty()) {
        ed.clip.path = r.newPath;
        ed.clip.title = ToUtf8(FileStem(r.newPath));
        snprintf(ed.title, sizeof(ed.title), "%s", ed.clip.title.c_str());
    }
    if (editorClip && r.newPath.empty() && !FileExists(ed.clip.path)) ed = EditorState{};   // deleted
    library.Rescan();
}

void App::Impl::OnEngineEvent(const engine::EngineEvent& e)
{
    using T = engine::EngineEventType;
    EditorState& v = ui.editor;
    switch (e.type) {
    case T::ClipSaved: {
        Sound(platform::Sound::ClipSaved);
        // Bookmarks inside the clip go into its sidecar file. A saved clip ends when Save was
        // pressed; a recording runs from its start to its stop.
        if (!e.path.empty()) {
            const bool rec = e.message == "Recording";
            const double end = rec ? (recStopAt > recStartAt ? recStopAt : NowSeconds()) : lastSaveAt;
            const double start = rec ? recStartAt : end - e.seconds;
            std::vector<Bookmark> inside = marks.Range(start, end + 0.25);
            if (!inside.empty()) SaveBookmarks(e.path, inside);
        }
        const std::string title = std::string(e.message == "Recording" ? "Recording saved" : "Clip saved") + " \xC2\xB7 " + FormatDuration(e.seconds);
        const std::string game = e.path.empty() ? std::string() : GameFromPath(e.path);
        if (!e.path.empty()) {
            library.Rescan();
            // Decode the poster frame, then show the toast with it (or without, after 1.5 s).
            PendingToast pt;
            pt.path = e.path;
            pt.modified = FileModifiedUnix(e.path);
            pt.title = title;
            pt.body = game;
            pt.deadline = NowSeconds() + 1.5;
            strips.Request(pt.path, pt.modified);
            pendingToasts.push_back(pt);
        } else {
            Toast(ToastKind::Success, title, game, ICON_SAVE);
        }
        break;
    }
    case T::RecordingStarted:
        recStartAt = NowSeconds();
        Sound(platform::Sound::RecordStart);
        Toast(ToastKind::Recording, "Recording", "Press " + HotkeyText(MakeContext(), HotkeyAction::ToggleRecording) + " to stop", ICON_RECORD);
        break;
    case T::RecordingStopped:
        Sound(platform::Sound::RecordStop);
        Toast(ToastKind::Info, "Recording stopped", FormatDuration(e.seconds) + "  \xC2\xB7  saving\xE2\x80\xA6", ICON_STOP);
        break;
    case T::ExportProgress:
        if (e.job == v.exportJob) v.exportProgress = e.progress;
        if (e.job == ui.montage.exportJob) ui.montage.exportProgress = e.progress;
        break;
    case T::ExportDone:
        if (e.job == v.exportJob) {
            v.exporting = false;
            v.exportJob = 0;
            v.exportFinished = true;
        }
        if (e.job == ui.montage.exportJob) {
            ui.montage.exporting = false;
            ui.montage.exportJob = 0;
            ui.montage.exportFinished = true;
        }
        Sound(platform::Sound::ClipSaved);
        Snack(ui, "Exported", e.path);
        library.Rescan();
        break;
    case T::ExportFailed:
        if (e.job == v.exportJob) {
            v.exporting = false;
            v.exportJob = 0;
            if (e.message == "Export cancelled.") v.exportError.clear();
            else v.exportError = e.message.rfind("Couldn't", 0) == 0 ? e.message : "Couldn't export: " + e.message;
        }
        if (e.job == ui.montage.exportJob) {
            MontageState& m = ui.montage;
            m.exporting = false;
            m.exportJob = 0;
            if (e.message == "Export cancelled.") m.exportError.clear();
            else m.exportError = e.message.rfind("Couldn't", 0) == 0 ? e.message : "Couldn't export: " + e.message;
        }
        break;
    case T::EncoderProbeDone: break;
    case T::Info:
        Toast(ToastKind::Info, e.message, "");
        break;
    case T::Error:
        Sound(platform::Sound::Error);
        Toast(ToastKind::Error, e.message, "");
        break;
    }
}

void App::Impl::OnTray(uint32_t msg)
{
    if (msg == kTrayLeftUp || msg == kTrayLeftDouble) {
        ShowMainWindow();
        return;
    }
    if (msg != kTrayRightUp && msg != kTrayContextMenu) return;
    const engine::EngineStatus st = engine->Status();
    TrayMenuState ms;
    ms.recording = st.recording;
    ms.bufferOn = st.buffer != engine::BufferState::Off && st.buffer != engine::BufferState::Error;
    switch (tray.ShowMenu(ms)) {
    case TrayCommand::SaveReplay: SaveReplay(); break;
    case TrayCommand::ToggleRecording: ToggleRecording(); break;
    case TrayCommand::ToggleReplayBuffer: ToggleBuffer(true); break;
    case TrayCommand::Screenshot: TakeScreenshot(ShotMode::FullScreen, false); break;
    case TrayCommand::ScreenshotRegion: TakeScreenshot(ShotMode::Region, false); break;
    case TrayCommand::OpenApp: ShowMainWindow(); break;
    case TrayCommand::OpenFolder:
        EnsureDir(settings.ClipsFolder());
        platform::OpenPath(settings.ClipsFolder());
        break;
    case TrayCommand::Quit: Quit(); break;
    default: break;
    }
}

void App::Impl::ShowMainWindow()
{
    openedAt = NowSeconds();
    if (!shownOnce) {
        shownOnce = true;
        window.ShowInitial(startMaximized);
    } else {
        window.Show();
    }
}

void App::Impl::HideToTray()
{
    window.Hide();
    if (player.IsOpen()) player.Pause();
    if (!settings.general.trayHintShown) {
        Toast(ToastKind::Info, "Still running in the tray", "Hotkeys keep working. Right-click the tray icon to quit.");
        settings.general.trayHintShown = true;
        RequestSave();
    }
}

void App::Impl::CloseRequested()
{
    if (settings.general.closeToTray) HideToTray();
    else Quit();
}

void App::Impl::Quit()
{
    running = false;
    int x, y, w, h;
    bool maximized;
    window.GetNormalRect(x, y, w, h, maximized);
    settings.window = WindowState{ x, y, w, h, maximized };
    SaveNow();
}

} // namespace cs::ui
