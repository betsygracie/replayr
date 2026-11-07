#include "core/ClipLibrary.h"
#include "core/Log.h"
#include "core/Platform.h"
#include "core/Settings.h"
#include "core/Util.h"

#include <windows.h>
#include <propsys.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
#include <ctime>
#include <map>
#include <thread>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace cs {

namespace {

// Paths saved before the rename point into ...\Videos\Clipstream, which is now ...\Videos\Replayr.
std::wstring Renamed(std::wstring p)
{
    const std::wstring from = L"\\clipstream\\", to = L"\\replayr\\";
    const size_t at = p.find(from);
    if (at != std::wstring::npos) p.replace(at, from.size(), to);
    return p;
}


struct MediaMeta {
    double duration = 0.0;
    int width = 0, height = 0;
};

enum class OpKind { Rename, Delete, Favourite, Move };
struct Op {
    OpKind kind = OpKind::Delete;
    std::wstring path;
    std::wstring arg;
    bool flag = false;
};

std::wstring LowerPath(std::wstring p)
{
    for (auto& c : p) c = (wchar_t)towlower(c);
    return p;
}

bool IsVideoExt(const std::wstring& e)
{
    return e == L".mp4" || e == L".mkv" || e == L".mov" || e == L".webm" || e == L".m4v" || e == L".avi";
}

bool IsImageExt(const std::wstring& e)
{
    return e == L".png" || e == L".jpg" || e == L".jpeg" || e == L".bmp" || e == L".webp" || e == L".gif";
}

int64_t FileTimeToUnix(const FILETIME& ft)
{
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return (int64_t)((u.QuadPart - 116444736000000000ULL) / 10000000ULL);
}

PROPERTYKEY KeyFromName(const wchar_t* name)
{
    PROPERTYKEY k{};
    PSGetPropertyKeyFromName(name, &k);
    return k;
}

} // namespace

struct ClipLibrary::Impl {
    mutable std::mutex mutex;
    std::wstring root;
    std::wstring stateFile;
    LibraryPolicy policy;
    std::shared_ptr<const ClipList> snapshot = std::make_shared<ClipList>();
    std::atomic<uint64_t> version{ 0 };
    std::atomic<bool> scanning{ false };
    std::atomic<uint64_t> totalBytes{ 0 };
    std::set<std::wstring> favourites;   // lower-case full paths
    std::map<std::wstring, std::vector<std::string>> tags;   // lower-case full path -> tags
    std::map<std::wstring, int64_t> opened;                 // lower-case full path -> unix seconds
    void MovePathState(const std::wstring& from, const std::wstring& to)
    {
        const std::wstring f = LowerPath(from);
        if (favourites.erase(f) && !to.empty()) favourites.insert(LowerPath(to));
        auto t = tags.find(f);
        if (t != tags.end()) {
            if (!to.empty()) tags[LowerPath(to)] = t->second;
            tags.erase(f);
        }
        auto o = opened.find(f);
        if (o != opened.end()) {
            if (!to.empty()) opened[LowerPath(to)] = o->second;
            opened.erase(f);
        }
    }
    std::deque<Op> ops;
    std::deque<LibraryOpResult> results;
    std::unordered_map<std::wstring, MediaMeta> metaCache;   // key: path|size|mtime

    HANDLE wakeEvent = nullptr;
    std::atomic<bool> quit{ false };
    std::atomic<bool> rescanRequested{ true };
    std::atomic<bool> rootChanged{ true };
    std::thread worker;
    double lastPolicyRun = -1e9;

    void Run();
    void Scan(const std::wstring& rootDir);
    void Walk(const std::wstring& dir, const std::wstring& game, int depth, ClipList& out);
    MediaMeta ReadMeta(const std::wstring& path, ClipType type);
    void ApplyPolicy(const ClipList& list);
    void RunOps();
    void LoadState();
    void SaveState();
    void PushResult(LibraryOpResult r)
    {
        std::lock_guard<std::mutex> lock(mutex);
        results.push_back(std::move(r));
    }
};

ClipLibrary::ClipLibrary() : impl_(std::make_unique<Impl>()) {}
ClipLibrary::~ClipLibrary() { Stop(); }

void ClipLibrary::Start(const std::wstring& root, const std::wstring& stateFile)
{
    impl_->root = root;
    impl_->stateFile = stateFile;
    impl_->LoadState();
    impl_->wakeEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    impl_->quit = false;
    impl_->worker = std::thread([this] { impl_->Run(); });
}

void ClipLibrary::Stop()
{
    if (!impl_ || !impl_->worker.joinable()) return;
    impl_->quit = true;
    SetEvent(impl_->wakeEvent);
    impl_->worker.join();
    CloseHandle(impl_->wakeEvent);
    impl_->wakeEvent = nullptr;
}

void ClipLibrary::SetRoot(const std::wstring& root)
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (LowerPath(root) == LowerPath(impl_->root)) return;
        impl_->root = root;
    }
    impl_->rootChanged = true;
    impl_->rescanRequested = true;
    if (impl_->wakeEvent) SetEvent(impl_->wakeEvent);
}

std::wstring ClipLibrary::Root() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->root;
}

void ClipLibrary::SetPolicy(const LibraryPolicy& p)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->policy = p;
}

void ClipLibrary::Rescan()
{
    impl_->rescanRequested = true;
    if (impl_->wakeEvent) SetEvent(impl_->wakeEvent);
}

std::shared_ptr<const ClipList> ClipLibrary::Snapshot() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->snapshot;
}

uint64_t ClipLibrary::Version() const { return impl_->version.load(); }
bool ClipLibrary::Scanning() const { return impl_->scanning.load(); }
uint64_t ClipLibrary::TotalBytes() const { return impl_->totalBytes.load(); }

static void QueueOp(ClipLibrary::Impl& d, Op op)
{
    {
        std::lock_guard<std::mutex> lock(d.mutex);
        d.ops.push_back(std::move(op));
    }
    if (d.wakeEvent) SetEvent(d.wakeEvent);
}

void ClipLibrary::SetFavourite(const std::wstring& path, bool favourite)
{
    // Instant feedback: patch the current snapshot, persist on the worker.
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto copy = std::make_shared<ClipList>(*impl_->snapshot);
        for (auto& c : *copy)
            if (c.path == path) c.favourite = favourite;
        impl_->snapshot = copy;
        if (favourite) impl_->favourites.insert(LowerPath(path));
        else impl_->favourites.erase(LowerPath(path));
    }
    impl_->version++;
    QueueOp(*impl_, Op{ OpKind::Favourite, path, L"", favourite });
}

void ClipLibrary::SetTags(const std::wstring& path, const std::vector<std::string>& tags)
{
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto copy = std::make_shared<ClipList>(*impl_->snapshot);
        for (auto& c : *copy)
            if (c.path == path) c.tags = tags;
        impl_->snapshot = copy;
        if (tags.empty()) impl_->tags.erase(LowerPath(path));
        else impl_->tags[LowerPath(path)] = tags;
    }
    impl_->version++;
    QueueOp(*impl_, Op{ OpKind::Favourite, path, L"", false });   // just persists the state file
}

void ClipLibrary::MarkOpened(const std::wstring& path)
{
    const int64_t now = (int64_t)time(nullptr);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->opened[LowerPath(path)] = now;
        auto copy = std::make_shared<ClipList>(*impl_->snapshot);
        for (auto& c : *copy)
            if (c.path == path) c.lastOpened = now;
        impl_->snapshot = copy;
    }
    QueueOp(*impl_, Op{ OpKind::Favourite, path, L"", false });
}

void ClipLibrary::Rename(const std::wstring& path, const std::wstring& newStem)
{
    QueueOp(*impl_, Op{ OpKind::Rename, path, newStem, false });
}

int64_t FileModifiedUnix(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return FileTimeToUnix(fa.ftLastWriteTime);
}

void ClipLibrary::Move(const std::wstring& path, const std::wstring& destDir)
{
    QueueOp(*impl_, Op{ OpKind::Move, path, destDir, false });
}

void ClipLibrary::Delete(const std::wstring& path)
{
    QueueOp(*impl_, Op{ OpKind::Delete, path, L"", false });
}

bool ClipLibrary::PollResult(LibraryOpResult& out)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->results.empty()) return false;
    out = std::move(impl_->results.front());
    impl_->results.pop_front();
    return true;
}

// ---- worker -------------------------------------------------------------------

void ClipLibrary::Impl::LoadState()
{
    FILE* f = _wfopen(stateFile.c_str(), L"rb");
    if (!f) return;
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return;
    if (j.contains("favourites") && j["favourites"].is_array())
        for (auto& v : j["favourites"])
            if (v.is_string()) favourites.insert(Renamed(LowerPath(ToWide(v.get<std::string>()))));
    if (j.contains("tags") && j["tags"].is_object())
        for (auto& [k, v] : j["tags"].items()) {
            if (!v.is_array()) continue;
            std::vector<std::string> list;
            for (auto& t : v)
                if (t.is_string()) list.push_back(t.get<std::string>());
            if (!list.empty()) tags[Renamed(LowerPath(ToWide(k)))] = list;
        }
    if (j.contains("opened") && j["opened"].is_object())
        for (auto& [k, v] : j["opened"].items())
            if (v.is_number_integer()) opened[Renamed(LowerPath(ToWide(k)))] = v.get<int64_t>();
}

void ClipLibrary::Impl::SaveState()
{
    nlohmann::json j;
    j["favourites"] = nlohmann::json::array();
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& f : favourites) j["favourites"].push_back(ToUtf8(f));
        j["tags"] = nlohmann::json::object();
        for (auto& [k, v] : tags) j["tags"][ToUtf8(k)] = v;
        j["opened"] = nlohmann::json::object();
        for (auto& [k, v] : opened) j["opened"][ToUtf8(k)] = v;
    }
    WriteFileAtomic(stateFile, j.dump(2));
}

MediaMeta ClipLibrary::Impl::ReadMeta(const std::wstring& path, ClipType type)
{
    static const PROPERTYKEY kDuration = KeyFromName(L"System.Media.Duration");
    static const PROPERTYKEY kFrameW = KeyFromName(L"System.Video.FrameWidth");
    static const PROPERTYKEY kFrameH = KeyFromName(L"System.Video.FrameHeight");
    static const PROPERTYKEY kImageW = KeyFromName(L"System.Image.HorizontalSize");
    static const PROPERTYKEY kImageH = KeyFromName(L"System.Image.VerticalSize");

    MediaMeta m;
    ComPtr<IShellItem2> item;
    if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item)))) return m;
    ULONGLONG dur = 0;
    ULONG w = 0, h = 0;
    if (type == ClipType::Video) {
        if (SUCCEEDED(item->GetUInt64(kDuration, &dur))) m.duration = (double)dur / 1e7;   // 100 ns units
        if (SUCCEEDED(item->GetUInt32(kFrameW, &w))) m.width = (int)w;
        if (SUCCEEDED(item->GetUInt32(kFrameH, &h))) m.height = (int)h;
    } else {
        if (SUCCEEDED(item->GetUInt32(kImageW, &w))) m.width = (int)w;
        if (SUCCEEDED(item->GetUInt32(kImageH, &h))) m.height = (int)h;
    }
    return m;
}

void ClipLibrary::Impl::Walk(const std::wstring& dir, const std::wstring& game, int depth, ClipList& out)
{
    if (depth > 4 || quit) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(JoinPath(dir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        const std::wstring full = JoinPath(dir, name);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            Walk(full, depth == 0 ? name : game, depth + 1, out);
            continue;
        }
        const std::wstring ext = FileExtension(name);
        ClipType type;
        if (IsVideoExt(ext)) type = ClipType::Video;
        else if (IsImageExt(ext)) type = ClipType::Image;
        else continue;

        ClipInfo c;
        c.path = full;
        c.id = (uint64_t)std::hash<std::wstring>{}(LowerPath(full));
        c.title = ToUtf8(FileStem(name));
        c.game = ToUtf8(game);
        c.type = type;
        c.sizeBytes = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        c.modified = FileTimeToUnix(fd.ftLastWriteTime);

        std::wstring metaKey = full + L"|" + IntToW((long long)c.sizeBytes) + L"|" + IntToW(c.modified);
        auto it = metaCache.find(metaKey);
        MediaMeta meta;
        if (it != metaCache.end()) meta = it->second;
        else {
            meta = ReadMeta(full, type);
            metaCache[metaKey] = meta;
        }
        c.duration = meta.duration;
        c.width = meta.width;
        c.height = meta.height;
        out.push_back(std::move(c));
    } while (FindNextFileW(h, &fd) && !quit);
    FindClose(h);
}

void ClipLibrary::Impl::Scan(const std::wstring& rootDir)
{
    scanning = true;
    ClipList list;
    if (DirExists(rootDir)) Walk(rootDir, L"", 0, list);

    uint64_t total = 0;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (auto& c : list) {
            const std::wstring key = LowerPath(c.path);
            c.favourite = favourites.count(key) > 0;
            auto t = tags.find(key);
            if (t != tags.end()) c.tags = t->second;
            auto o = opened.find(key);
            if (o != opened.end()) c.lastOpened = o->second;
            total += c.sizeBytes;
        }
    }
    std::sort(list.begin(), list.end(), [](const ClipInfo& a, const ClipInfo& b) { return a.modified > b.modified; });
    if (metaCache.size() > 20000) metaCache.clear();

    auto snap = std::make_shared<const ClipList>(list);
    {
        std::lock_guard<std::mutex> lock(mutex);
        snapshot = snap;
    }
    totalBytes = total;
    version++;
    scanning = false;
    ApplyPolicy(*snap);
}

void ClipLibrary::Impl::ApplyPolicy(const ClipList& list)
{
    LibraryPolicy p;
    {
        std::lock_guard<std::mutex> lock(mutex);
        p = policy;
    }
    const double now = NowSeconds();
    if (now - lastPolicyRun < 600.0) return;   // at most every 10 minutes
    if (!p.autoDelete && p.maxBytes == 0) return;
    lastPolicyRun = now;

    std::vector<const ClipInfo*> victims;
    if (p.autoDelete) {
        const int64_t cutoff = NowUnix() - (int64_t)p.maxAgeDays * 86400;
        for (auto& c : list)
            if (!c.favourite && c.modified < cutoff) victims.push_back(&c);
    }
    if (p.maxBytes > 0) {
        uint64_t total = 0;
        for (auto& c : list) total += c.sizeBytes;
        for (auto* v : victims) total -= std::min(total, v->sizeBytes);
        // Oldest first (list is newest first).
        for (auto it = list.rbegin(); it != list.rend() && total > p.maxBytes; ++it) {
            if (it->favourite) continue;
            if (std::find(victims.begin(), victims.end(), &*it) != victims.end()) continue;
            victims.push_back(&*it);
            total -= std::min(total, it->sizeBytes);
        }
    }
    int removed = 0;
    for (auto* v : victims)
        if (platform::RecycleFile(v->path)) ++removed;
    if (removed > 0) {
        Log("Storage policy moved %d old clips to the Recycle Bin", removed);
        LibraryOpResult r;
        r.ok = true;
        r.message = "Moved " + std::to_string(removed) + " old clip(s) to the Recycle Bin (storage limits)";
        PushResult(r);
        rescanRequested = true;
    }
}

void ClipLibrary::Impl::RunOps()
{
    for (;;) {
        Op op;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (ops.empty()) return;
            op = std::move(ops.front());
            ops.pop_front();
        }
        LibraryOpResult r;
        r.oldPath = op.path;
        switch (op.kind) {
        case OpKind::Favourite:
            SaveState();
            continue;   // no message
        case OpKind::Rename: {
            std::wstring stem = SanitizeFileName(op.arg);
            std::wstring target = JoinPath(ParentDir(op.path), stem + FileExtension(op.path));
            if (target == op.path) continue;   // unchanged
            if (FileExists(target) && LowerPath(target) != LowerPath(op.path)) {
                r.ok = false;
                r.message = "A file called \"" + ToUtf8(stem) + "\" already exists";
            } else if (platform::RenameFile(op.path, target)) {
                r.ok = true;
                r.newPath = target;
                r.message = "Renamed to \"" + ToUtf8(stem) + "\"";
                std::lock_guard<std::mutex> lock(mutex);
                MovePathState(op.path, target);
            } else {
                r.ok = false;
                r.message = "Couldn't rename (is the clip open in another app?)";
            }
            if (r.ok) SaveState();
            break;
        }
        case OpKind::Move: {
            EnsureDir(op.arg);
            std::wstring target = UniquePath(JoinPath(op.arg, FileStem(op.path) + FileExtension(op.path)));
            if (LowerPath(ParentDir(op.path)) == LowerPath(op.arg)) continue;   // already there
            if (platform::RenameFile(op.path, target)) {
                r.ok = true;
                r.newPath = target;
                r.message = "Moved \"" + ToUtf8(FileStem(op.path)) + "\"";
                std::lock_guard<std::mutex> lock(mutex);
                MovePathState(op.path, target);
            } else {
                r.ok = false;
                r.message = "Couldn't move \"" + ToUtf8(FileStem(op.path)) + "\" (is it open in another app?)";
            }
            if (r.ok) SaveState();
            break;
        }
        case OpKind::Delete:
            if (platform::RecycleFile(op.path)) {
                r.ok = true;
                r.message = "Moved \"" + ToUtf8(FileStem(op.path)) + "\" to the Recycle Bin";
                std::lock_guard<std::mutex> lock(mutex);
                MovePathState(op.path, L"");
            } else {
                r.ok = false;
                r.message = "Couldn't delete (is the clip open in another app?)";
            }
            if (r.ok) SaveState();
            break;
        }
        PushResult(r);
        rescanRequested = true;
    }
}

void ClipLibrary::Impl::Run()
{
    HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    HANDLE change = INVALID_HANDLE_VALUE;
    std::wstring watched;
    double debounceUntil = -1.0;

    while (!quit) {
        if (rootChanged.exchange(false)) {
            if (change != INVALID_HANDLE_VALUE) FindCloseChangeNotification(change);
            change = INVALID_HANDLE_VALUE;
            {
                std::lock_guard<std::mutex> lock(mutex);
                watched = root;
            }
            EnsureDir(watched);
            change = FindFirstChangeNotificationW(watched.c_str(), TRUE,
                                                  FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                                                      FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
        }
        RunOps();
        if (rescanRequested.exchange(false)) {
            Scan(watched);
            debounceUntil = -1.0;
        }

        HANDLE handles[2] = { wakeEvent, change };
        DWORD count = (change != INVALID_HANDLE_VALUE) ? 2 : 1;
        DWORD timeout = INFINITE;
        if (debounceUntil > 0.0) {
            double ms = (debounceUntil - NowSeconds()) * 1000.0;
            timeout = ms > 0.0 ? (DWORD)ms : 0;
        }
        DWORD r = WaitForMultipleObjects(count, handles, FALSE, timeout);
        if (r == WAIT_OBJECT_0 + 1) {
            FindNextChangeNotification(change);
            debounceUntil = NowSeconds() + 0.7;   // a clip being written fires many events: wait for quiet
        } else if (r == WAIT_TIMEOUT && debounceUntil > 0.0) {
            debounceUntil = -1.0;
            rescanRequested = true;
        }
    }
    if (change != INVALID_HANDLE_VALUE) FindCloseChangeNotification(change);
    if (SUCCEEDED(coHr)) CoUninitialize();
}

} // namespace cs
