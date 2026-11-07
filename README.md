# Replayr

A game clipping and screenshot app for Windows 10/11, built with C++20, Dear ImGui (docking branch)
and DirectX 11. It has a glass UI, a borderless window, global hotkeys, a tray icon, toasts, a clip
library and a trimmer/editor.

The UI talks only to the `IClipEngine` interface (`src/engine/ClipEngine.h`). The engine behind it
(`RealClipEngine.cpp`) needs nothing extra installed:

- **Capture:** DXGI Desktop Duplication on the GPU that drives the monitor, with the cursor drawn in.
  It falls back to GDI capture where duplication isn't available (Remote Desktop, some VMs).
- **Convert:** BGRA to NV12, scaled to the output size, with the D3D11 video processor.
- **Encode:** the Media Foundation sink writer, with H.264 or HEVC on the GPU's hardware encoder when
  there is one (NVENC/AMF/Quick Sync MFTs), else Microsoft's software encoder. Audio is AAC.
- **Audio:** WASAPI loopback (system sound) and the microphone, on separate tracks or mixed.
- **Replay buffer:** a ring of 4-second MP4 segments in `%LOCALAPPDATA%\Replayr\Buffer`. Saving a
  clip or stopping a recording joins segments with no re-encode, starting on a keyframe.
- **Exports:** *Original* with no banner is a stream copy (no quality loss, near instant). Anything
  else (High, Small file, or with the banner) is decoded and re-encoded on the GPU with Media
  Foundation (`engine/Transcoder.cpp`), so FFmpeg is not needed for anything.

---

## Build

You need:

- Windows 10 or 11
- **Visual Studio 2022 or newer** with the *Desktop development with C++* workload. This includes CMake and the Windows SDK.
- Internet access for the first configure. CMake downloads Dear ImGui `v1.92.2b-docking` and the nlohmann/json single header.

You can build in any of these ways:

- **Double-click `build.bat`**, or run it from the *x64 Native Tools Command Prompt*. The exe ends up in `build\Release\Replayr.exe`.
- **Visual Studio:** *File > Open > Folder...*, pick this folder, choose the `Replayr.exe` target and press F5.
- **Command line:**
  ```
  cmake -S . -B build
  cmake --build build --config Release
  ```

Optional extras:

- `vendor\ffmpeg\ffmpeg.exe`: copied next to the exe on every build. You can also put it next to the exe yourself or on PATH. Get it from https://www.gyan.dev/ffmpeg/builds/ (the "essentials" build is enough). Not needed for exports any more; only used for test clips and the encoder test.
- `assets\fonts\Inter-Regular.ttf` + `Inter-SemiBold.ttf` switch the UI font from Segoe UI to Inter.
- Offline builds: put an ImGui checkout in `vendor\imgui` and `json.hpp` in `vendor\nlohmann`.

The exe uses a static CRT, so it runs without the VC++ redistributable. Settings, the log and favourites
live in `%APPDATA%\Replayr`. Clips go to `Videos\Replayr\<Game>\` by default.

---

## Project layout

```
CMakeLists.txt, build.bat
assets/      app.ico, app.rc (icon + version info), Replayr.manifest (PerMonitorV2 DPI), fonts/
src/
  main.cpp               DPI awareness, single instance, COM, App
  AppInfo.h              app name/version: change the name here
  ui/                    everything ImGui (no <windows.h>)
    App.cpp              owns the services, message loop, layout, actions, hotkeys/tray/engine events
    AppContext.h         page/viewer/library state + the Action list views post
    Theme.h/.cpp         design tokens (colours, radii, spacing, type, motion), fonts, glass layers
    Anim.h/.cpp          per-ID easing, springs (stiffness + damping) and timers shared by every widget
    Widgets.h/.cpp       buttons, toggles, sliders, checkboxes, dropdowns, text fields, menus, modals,
                         tooltips, hotkey binder, meters, panels (no stock ImGui widgets on screen)
    ClipCard.cpp         card: thumbnail, duration, title, game, time; preview + Edit/"..." on hover
    TopBar.cpp           search, recording pill, window controls
    Sidebar.cpp          logo, Clips / Screenshots / Editor / Settings, storage + Save clip
    Library.cpp          shared helpers: card grid, "..." menu, rename/delete dialogs, snackbar
    LibraryView.cpp      Clips and Screenshots: game filter, sort, Today / Yesterday / Earlier
    Editor.cpp           player, filmstrip trim, export popup, live banner preview
    ImageViewer.cpp      screenshots: zoom, pan, previous/next, copy
    SettingsView.cpp     General, Recording, Audio, Hotkeys, Storage (+ "Advanced" sections)
    Icons.h              Segoe Fluent Icons / MDL2 code points (ships with Windows)
  media/
    MediaCache.cpp       filmstrips (Media Foundation, cached on disk as JPEG) and full-size images (WIC)
    Banner.cpp           the "Clipped using Replayr" banner: layout, timing, GDI+ drawing, blending
  core/                  platform services
    Window.cpp           borderless window, D3D11 device, DirectComposition swap chain, Mica/Acrylic
    GlassRenderer.cpp    gradient-mesh background shader + dual-Kawase blur for the glass panels
    Settings.cpp         settings struct <-> JSON, background saver
    Hotkeys.cpp Tray.cpp Toast.cpp
    Thumbnailer.cpp      async shell thumbnails -> GPU textures, LRU cache
    ClipLibrary.cpp      folder scan + change watcher, metadata, favourites, rename/delete
    MediaPlayer.cpp      editor preview (IMFMediaEngine, hardware decode)
    Screenshot.cpp       DXGI Desktop Duplication / GDI capture, region overlay, WIC PNG/JPG
    SystemStats.cpp Platform.cpp Util.cpp Log.cpp
  engine/
    ClipEngine.h         the interface the UI uses
    RealClipEngine.cpp   session thread: pacing, segments, replay saves, recording, exports
    Transcoder.cpp       re-encoding exports: decode, trim, banner, H.264 + AAC on the GPU
    DesktopCapture.cpp   Desktop Duplication / GDI capture, cursor, GPU BGRA->NV12 + scaling
    MediaWriter.cpp      Media Foundation MP4 writer (H.264/HEVC + AAC) and the no-re-encode joiner
    AudioInput.cpp       WASAPI loopback / mic streams, resampled to 48 kHz, kept in sync
    Encoder.cpp          GPU vendor -> NVENC/AMF/QuickSync/x264 selection
    AudioCapture.cpp     WASAPI devices + live peak meters (system, mic, per-app sessions)
    FFmpegTools.cpp      optional: runs ffmpeg.exe hidden (encoder probes, test clips)
vendor/                  optional offline/extra files (see vendor/README.txt)
```

---

## How to test each part

### 1. Window and look (glass)
- The background is a slowly drifting gradient mesh drawn by a pixel shader (`core/GlassRenderer.cpp`),
  with film grain. It leans a little towards the cursor and loops every 45 s. It only redraws at
  30 fps while the window is focused, and freezes when it isn't (so it costs nothing while you game).
- Every panel shows a blurred, slightly more saturated copy of the background behind it (blurred once
  per frame at 1/4 and 1/8 size and shared by all panels). Popups and menus use the stronger blur.
- *Settings > General > Appearance*: Background (Animated / Static / Solid), Reduce motion,
  accent colour. *Advanced* has Windows Mica/Acrylic (Windows 11) and interface size.
- If the GPU shaders can't be created, the log says so and the old CPU-drawn backdrop is used.

### 1b. Interactions
- Buttons: hover lifts and grows them on a spring, a light follows the cursor across the glass, the
  border and glow pick up the accent, icons nudge. Press pushes them down; release springs back with
  a ripple from where you clicked. Primary buttons get a shine sweep every few seconds while hovered.
- Toggles spring and stretch, sliders pop a value bubble, text fields fill their border from the left
  and float their label up, menus/dropdowns/tooltips scale in from where they opened, cards tilt
  towards the cursor with a glare, the sidebar pill springs between pages, toasts spring in.
- *Interface sounds* (off by default) adds a very quiet tick on hover and click.

### 2. Library
- The sidebar has Clips, Screenshots and Settings; Editor appears while a clip is open.
- Clips and Screenshots are one grid grouped Today / Yesterday / Earlier. Top right: clean-up button,
  **Filters** (recorded today / this week / this month, length, tag), game filter (with Favourites)
  and sort. Ctrl+F jumps to search, which also matches tags.
- Right-click or "..." on a card: Edit, Rename, Favourite, **Tags...**, **Add to montage**, Copy,
  Show in folder, Open externally, Delete.
- **Clean up** suggests near-duplicates (Save clip pressed twice) and month-old clips you never
  opened, with the space they free. Ticked ones go to the Recycle Bin; favourites are never listed.

### 3. Editor
- Click a clip (or Edit). Click the video to play/pause, double-click for fullscreen.
- **Cut out parts:** press S (or Split) at the playhead to cut the clip in two, click a section and
  press Delete. Removed parts are dimmed and hatched and are skipped while playing. Drag any
  section's edges to trim it (they snap to the playhead and can't overlap).
- **Speed:** click a section and pick 0.25x to 2x. It previews at that speed; sound follows.
- **Text:** press T (or Add text). Edit the words, size (S/M/L) and place (Top/Middle/Bottom) in the
  row that appears; drag the caption on the text lane to move it, or its ends to change how long it
  shows. It's drawn on the video exactly where the export puts it.
- Ctrl+Z undoes, Esc clears the selection and then goes back.
- Export: quality, **Fit under a size** (10 MB for Discord, 25/50/100 MB), banner, name. With a size
  limit Replayr picks the bitrate, and drops resolution or frame rate only if it has to.

### 3b. Montage
- Right-click clips > *Add to montage*. A tray appears at the bottom; *Make video* lets you reorder
  them and export one video (same quality, size and banner options).

### 4. Banner
- With the toggle on, the banner plays over the video in the editor exactly as it will in the export:
  in at 1 s, a shine sweep, out at 5.4 s (or always, see Settings).
- *Settings > General > Replayr banner*: default on/off, when it shows, corner, screenshots, preview.

### 5. Screenshots
- Click one: wheel zooms around the cursor, drag pans, double-click toggles fit/100%, arrows go to the
  previous/next one, Esc or a click outside closes it. Ctrl+C copies the image.

### 6. Settings
- Five categories; technical options are in each category's *Advanced* section. Changes save
  automatically and a "Saved" tick confirms it.

### 6b. First run and updates
- A fresh install opens a three-step welcome: Save clip hotkey and length, quality (Light /
  Balanced / Sharp), clips folder with mic and start-with-Windows. "Use defaults" skips it.
- Updates: set `CS_UPDATE_REPO` in `src/AppInfo.h` to your GitHub "owner/repo". Publish a release
  tagged like `v0.2.0` with `Replayr.exe` attached. The app checks 12 s after starting, downloads
  it in the background and shows "Restart to update" in the top bar (also in Settings > General).
- *Settings > Recording > Performance* shows capture fps, dropped frames, the encoder and PC load.

### 6c. Accounts (Discord / Steam) and sharing
- Set up the free server once: see `server/README.md` (Cloudflare Workers + D1 + R2).
- Point the app at it: set `CS_ACCOUNT_SERVER` in `src/AppInfo.h`, or without rebuilding add
  `"accountServer": "https://replayr.<you>.workers.dev"` inside `"general"` in
  `%APPDATA%\Replayr\settings.json` (close Replayr first).
- The card at the bottom of the sidebar then shows "Share your clips" with Discord and Steam
  buttons. Signing in opens your browser; the app never sees your password.
- Signed in: right-click a clip > *Upload & copy link*. The card shows progress, and the link is
  copied when it's done. *Uploads* (sidebar) lists your clips: Public / Unlisted / Private, copy
  link, open, delete. Click your name on the card to sign out.
- Without a server address the card shows local storage as before.

### 6d. Keyboard & mouse overlay
- *Settings > Recording > Keyboard & mouse overlay*: a small glass keyboard (gaming keys) and mouse
  that light up as you press them; other keys (Enter, F-keys, arrows...) pop up as pills.
- Where it shows: *Screen + clips* (a click-through window, captured with the game), *Screen only*
  (hidden from captures, Windows 10 2004+), or *Clips only* (drawn into the recording, not on screen).
- *Only during games* (on by default) hides it unless a fullscreen/borderless app is in front, so it
  never shows typing in chats or browsers. Input is read with Raw Input only while it's on, and is
  never saved.

### 7. Capture engine
- The replay buffer is on by default (30 s, 1080p60, GPU encoder, mic off).
- **Save replay** (Alt+F10) writes the last N seconds to your clips folder in about a second.
- **Record** (Alt+F9) records until you press it again. It works with the buffer on or off.
- If capture can't start, a toast says why, and `replayr.log` has the details.

### Screenshots
- **Full screen** captures the monitor of the focused window.
- **Window** crops to the active window.
- **Region** freezes the screen: drag a box, click for the whole monitor, or press Esc/right-click to cancel.
- PNG or JPG (*Settings > Storage*). The image is also copied to the clipboard, so you can paste it into Discord.
- When you trigger a shot from Replayr's own buttons, the window hides first so it isn't in the picture.

---

## Non-obvious choices

- **Composition swap chain:** a DirectComposition swap chain with premultiplied alpha, plus `WS_EX_NOREDIRECTIONBITMAP`, is the documented way to let DWM's Mica/Acrylic show through a D3D11 window. If DirectComposition isn't available, it falls back to a normal flip-model swap chain.
- **Borderless but still a normal window:** it keeps `WS_OVERLAPPEDWINDOW`, and `WM_NCCALCSIZE` removes the frame. That keeps Aero Snap, the min/max animations and the DWM shadow and rounded corners. `WM_NCHITTEST` uses rectangles the title bar reports every frame. The maximise button returns `HTMAXBUTTON` so Windows 11 shows Snap Layouts.
- **No downloaded fonts or icons:** it uses Segoe UI and Segoe Fluent Icons/MDL2, which ship with Windows, so there's nothing extra to download.
- **Thumbnails** come from Windows' own thumbnail providers (`IShellItemImageFactory`), so there's no FFmpeg dependency and HEVC/AV1 work if the Store codec extensions are installed. They're decoded on a worker thread into `IMMUTABLE` textures (`ID3D11Device` is free-threaded).
- **Editor preview** uses `IMFMediaEngine` in frame-server mode on the app's own D3D11 device. That gives hardware decode, audio, seeking and frame stepping for about 300 lines. The device has `VIDEO_SUPPORT` and multithread protection on, because Media Foundation uses the context from its own threads.
- **Hotkeys** use `RegisterHotKey`, not a keyboard hook. That costs nothing while idle and works over games. A registered combo is swallowed for other apps, which is why the defaults all use modifiers. While binding, hotkeys are unregistered so you can rebind a combo to itself.
- **Toasts** are separate layered, click-through, `WS_EX_NOACTIVATE` windows drawn once with GDI+ and then only moved and faded. They never steal focus, and the main window doesn't have to be visible.
- **Microphone meter:** the meter's own mic stream only opens while a meter is on screen. The engine opens its own stream while capturing.
- **Replay buffer on disk:** each 4 s segment is a complete MP4 that starts on a keyframe. The next segment's encoder is created in the background before it's needed, and the previous one is finalized in the background, so rotating segments doesn't drop frames. 60 s at 40 Mbit/s is about 300 MB of disk, not RAM.

## Performance pitfalls handled

- **The UI thread never blocks:**
  - file scans, metadata reads, thumbnail decodes, WIC encodes, settings writes, FFmpeg jobs, audio and stats all run on worker threads
  - the UI reads snapshots (`shared_ptr` / mutex-guarded structs) and drains event queues once per frame
- **Texture uploads:**
  - thumbnails are created off-thread as immutable textures
  - the video preview is a single GPU-to-GPU `TransferVideoFrame` per new frame
  - an LRU cache (about 360 thumbnails) caps VRAM
- **Big libraries:** the grid uses `ImGuiListClipper`, so only visible rows are drawn. Thumbnail requests are LIFO and dropped once they scroll away, and filter/sort results are cached until the library or the filters change.
- **Idle cost:**
  - vsync when focused, about 20 fps unfocused, nothing when hidden
  - no XInput polling (`IMGUI_IMPL_WIN32_DISABLE_GAMEPAD`)
  - engine reconfiguration is debounced while you drag sliders
- **Scrubbing:** approximate seeks are throttled to about 20 per second while dragging, then one exact seek on release.

## Troubleshooting

- Log file: `%APPDATA%\Replayr\replayr.log` (also via *Settings > General > Advanced > Open log folder*).
- **"No video encoder accepts this input" on Windows N/KN editions:** install the *Media Feature Pack* (Settings > Apps > Optional features).
- **Black thumbnails or "can't play this format" for HEVC/AV1:** install the *HEVC Video Extensions* / *AV1 Video Extension* from the Microsoft Store.
- **Toasts don't appear over a game:** the game is probably in *exclusive* fullscreen. Use borderless/windowed fullscreen (most modern games default to it).
- **A hotkey shows "Another app already uses this combo":** pick another one, or quit the app that owns it (GeForce/NVIDIA overlay, Discord, Xbox Game Bar...).

## Not done yet

- Per-app audio tracks (the app list and meters work; capture records system sound as a whole).
- Windows Graphics Capture for single-window capture (needs C++/WinRT; Desktop Duplication captures the whole monitor).
- AV1 recording (Media Foundation has no AV1 encoder on most PCs, so AV1 falls back to H.264).
