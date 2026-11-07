#include "core/Bookmarks.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace cs {

namespace {

FILE* OpenFile(const std::wstring& path, const wchar_t* mode)
{
#ifdef _WIN32
    return _wfopen(path.c_str(), mode);
#else
    std::string p(path.begin(), path.end()), m;
    for (const wchar_t* c = mode; *c; ++c) m += (char)*c;
    return fopen(p.c_str(), m.c_str());
#endif
}

bool RemoveFile(const std::wstring& path)
{
#ifdef _WIN32
    return DeleteFileW(path.c_str()) != 0;
#else
    return std::remove(std::string(path.begin(), path.end()).c_str()) == 0;
#endif
}

bool ReplaceFile(const std::wstring& from, const std::wstring& to)
{
#ifdef _WIN32
    return MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    return std::rename(std::string(from.begin(), from.end()).c_str(), std::string(to.begin(), to.end()).c_str()) == 0;
#endif
}

std::string ReadAll(const std::wstring& path)
{
    std::string s;
    FILE* f = OpenFile(path, L"rb");
    if (!f) return s;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && s.size() < (1u << 20)) s.append(buf, n);
    fclose(f);
    return s;
}

int64_t Tell(FILE* f)
{
#ifdef _WIN32
    return _ftelli64(f);
#else
    return ftello(f);
#endif
}
void SeekTo(FILE* f, int64_t pos)
{
#ifdef _WIN32
    _fseeki64(f, pos, SEEK_SET);
#else
    fseeko(f, pos, SEEK_SET);
#endif
}

uint32_t Be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint64_t Be64(const uint8_t* p) { return (uint64_t)Be32(p) << 32 | Be32(p + 4); }
void Put32(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
void Put64(uint8_t* p, uint64_t v)
{
    Put32(p, (uint32_t)(v >> 32));
    Put32(p + 4, (uint32_t)v);
}
void Append32(std::vector<uint8_t>& v, uint32_t x)
{
    uint8_t b[4];
    Put32(b, x);
    v.insert(v.end(), b, b + 4);
}

// A box inside a buffer: [pos, pos+size), header length hdr.
struct Box {
    size_t pos = 0, size = 0, hdr = 8;
    char type[5] = {};
};

bool ReadBox(const std::vector<uint8_t>& d, size_t pos, size_t end, Box& b)
{
    if (pos + 8 > end) return false;
    uint64_t size = Be32(&d[pos]);
    b.hdr = 8;
    if (size == 1) {
        if (pos + 16 > end) return false;
        size = Be64(&d[pos + 8]);
        b.hdr = 16;
    } else if (size == 0) {
        size = end - pos;
    }
    if (size < b.hdr || pos + size > end) return false;
    b.pos = pos;
    b.size = (size_t)size;
    memcpy(b.type, &d[pos + 4], 4);
    b.type[4] = 0;
    return true;
}

// Adds `delta` to every chunk offset (stco / co64) under a container box.
bool ShiftChunkOffsets(std::vector<uint8_t>& d, size_t pos, size_t end, int64_t delta)
{
    static const char* containers[] = { "moov", "trak", "mdia", "minf", "stbl", "edts", "dinf" };
    while (pos < end) {
        Box b;
        if (!ReadBox(d, pos, end, b)) return false;
        const size_t body = b.pos + b.hdr;
        if (!strcmp(b.type, "stco") && b.size >= b.hdr + 8) {
            const uint32_t n = Be32(&d[body + 4]);
            if (body + 8 + (size_t)n * 4 > b.pos + b.size) return false;
            for (uint32_t i = 0; i < n; ++i) {
                const int64_t v = (int64_t)Be32(&d[body + 8 + i * 4]) + delta;
                if (v < 0 || v > 0xFFFFFFFFLL) return false;   // would need co64: give up safely
                Put32(&d[body + 8 + i * 4], (uint32_t)v);
            }
        } else if (!strcmp(b.type, "co64") && b.size >= b.hdr + 8) {
            const uint32_t n = Be32(&d[body + 4]);
            if (body + 8 + (size_t)n * 8 > b.pos + b.size) return false;
            for (uint32_t i = 0; i < n; ++i) Put64(&d[body + 8 + i * 8], (uint64_t)((int64_t)Be64(&d[body + 8 + i * 8]) + delta));
        } else {
            for (const char* c : containers)
                if (!strcmp(b.type, c)) {
                    if (!ShiftChunkOffsets(d, body, b.pos + b.size, delta)) return false;
                    break;
                }
        }
        pos = b.pos + b.size;
    }
    return true;
}

} // namespace

std::wstring SidecarPath(const std::wstring& clipPath)
{
    const size_t slash = clipPath.find_last_of(L"\\/");
    const size_t dot = clipPath.find_last_of(L'.');
    const std::wstring stem = dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash) ? clipPath.substr(0, dot) : clipPath;
    return stem + L".replayr.json";
}

std::vector<Bookmark> LoadBookmarks(const std::wstring& clipPath)
{
    std::vector<Bookmark> out;
    const std::string text = ReadAll(SidecarPath(clipPath));
    if (text.empty()) return out;
    auto j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.contains("bookmarks") || !j["bookmarks"].is_array()) return out;
    for (const auto& e : j["bookmarks"]) {
        if (!e.is_object() || !e.contains("t") || !e["t"].is_number()) continue;
        Bookmark b;
        b.t = e["t"].get<double>();
        if (e.contains("label") && e["label"].is_string()) b.label = e["label"].get<std::string>();
        if (e.contains("game") && e["game"].is_string()) b.game = e["game"].get<std::string>();
        if (e.contains("source") && e["source"].is_string()) b.source = e["source"].get<std::string>();
        if (e.contains("score") && e["score"].is_number()) b.score = e["score"].get<int>();
        out.push_back(b);
    }
    std::sort(out.begin(), out.end(), [](const Bookmark& a, const Bookmark& b) { return a.t < b.t; });
    return out;
}

bool SaveBookmarks(const std::wstring& clipPath, const std::vector<Bookmark>& marks)
{
    const std::wstring path = SidecarPath(clipPath);
    if (marks.empty()) {
        RemoveFile(path);
        return true;
    }
    nlohmann::json arr = nlohmann::json::array();
    for (const Bookmark& b : marks) {
        nlohmann::json e = { { "t", std::round(b.t * 1000.0) / 1000.0 }, { "source", b.source } };
        if (!b.label.empty()) e["label"] = b.label;
        if (!b.game.empty()) e["game"] = b.game;
        if (b.score) e["score"] = b.score;
        arr.push_back(e);
    }
    const std::string text = nlohmann::json({ { "version", 1 }, { "bookmarks", arr } }).dump(2);
    const std::wstring tmp = path + L".tmp";
    FILE* f = OpenFile(tmp, L"wb");
    if (!f) return false;
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    return ok && ReplaceFile(tmp, path);
}

int CountBookmarks(const std::wstring& clipPath)
{
    const std::string text = ReadAll(SidecarPath(clipPath));
    int n = 0;
    for (size_t p = text.find("\"t\""); p != std::string::npos; p = text.find("\"t\"", p + 3)) ++n;
    return n;
}

// ---- live log --------------------------------------------------------------------------------

void BookmarkLog::Add(double wallTime, const std::string& game, const std::string& source, int score, const std::string& label)
{
    std::lock_guard<std::mutex> lock(m_);
    Entry e;
    e.wall = wallTime;
    e.b.game = game;
    e.b.source = source;
    e.b.score = score;
    e.b.label = label;
    marks_.push_back(e);
    // Keep the last 3 hours (long recordings), and never more than a few thousand.
    marks_.erase(std::remove_if(marks_.begin(), marks_.end(), [&](const Entry& x) { return x.wall < wallTime - 3 * 3600.0; }), marks_.end());
    if (marks_.size() > 5000) marks_.erase(marks_.begin(), marks_.begin() + (ptrdiff_t)(marks_.size() - 5000));
}

std::vector<Bookmark> BookmarkLog::Range(double start, double end) const
{
    std::lock_guard<std::mutex> lock(m_);
    std::vector<Bookmark> out;
    for (const Entry& e : marks_)
        if (e.wall >= start && e.wall <= end) {
            Bookmark b = e.b;
            b.t = e.wall - start;
            out.push_back(b);
        }
    return out;
}

size_t BookmarkLog::Count() const
{
    std::lock_guard<std::mutex> lock(m_);
    return marks_.size();
}

// ---- MP4 chapters ----------------------------------------------------------------------------

bool WriteMp4Chapters(const std::wstring& path, std::vector<Chapter> chapters, std::string& error)
{
    std::sort(chapters.begin(), chapters.end(), [](const Chapter& a, const Chapter& b) { return a.start < b.start; });
    if (chapters.size() > 255) chapters.resize(255);
    FILE* f = OpenFile(path, L"rb");
    if (!f) {
        error = "Couldn't open the video to add chapters.";
        return false;
    }
    // Find the top-level boxes (sizes only; media data isn't read).
    struct Top {
        int64_t pos, size;
        char type[5];
    };
    std::vector<Top> tops;
    int64_t pos = 0;
    for (;;) {
        SeekTo(f, pos);
        uint8_t h[16];
        if (fread(h, 1, 8, f) != 8) break;
        int64_t size = Be32(h);
        if (size == 1) {
            if (fread(h + 8, 1, 8, f) != 8) break;
            size = (int64_t)Be64(h + 8);
        } else if (size == 0) {
            SeekTo(f, 0);
#ifdef _WIN32
            _fseeki64(f, 0, SEEK_END);
#else
            fseeko(f, 0, SEEK_END);
#endif
            size = Tell(f) - pos;
        }
        if (size < 8) break;
        Top t{ pos, size, {} };
        memcpy(t.type, h + 4, 4);
        tops.push_back(t);
        pos += size;
    }
    const Top* moovTop = nullptr;
    bool moovBeforeData = false, seenData = false;
    for (const Top& t : tops) {
        if (!strcmp(t.type, "mdat")) seenData = true;
        if (!strcmp(t.type, "moov")) {
            moovTop = &t;
            moovBeforeData = !seenData;
        }
    }
    if (!moovTop || moovTop->size > (64 << 20)) {
        fclose(f);
        error = "That file isn't an MP4 Replayr can add chapters to.";
        return false;
    }
    std::vector<uint8_t> moov((size_t)moovTop->size);
    SeekTo(f, moovTop->pos);
    if (fread(moov.data(), 1, moov.size(), f) != moov.size()) {
        fclose(f);
        error = "Couldn't read the video's header.";
        return false;
    }
    // Rebuild moov: drop any old chpl from udta, then add the new one.
    Box mb;
    if (!ReadBox(moov, 0, moov.size(), mb)) {
        fclose(f);
        error = "The video's header is damaged.";
        return false;
    }
    std::vector<uint8_t> chpl;
    Append32(chpl, 0);
    chpl.insert(chpl.end(), { 'c', 'h', 'p', 'l' });
    Append32(chpl, 0x01000000);   // version 1, no flags
    Append32(chpl, 0);
    chpl.push_back((uint8_t)chapters.size());
    for (const Chapter& c : chapters) {
        uint8_t t[8];
        Put64(t, (uint64_t)std::llround(std::max(0.0, c.start) * 1e7));   // 100 ns units
        chpl.insert(chpl.end(), t, t + 8);
        const std::string title = c.title.substr(0, 255);
        chpl.push_back((uint8_t)title.size());
        chpl.insert(chpl.end(), title.begin(), title.end());
    }
    Put32(chpl.data(), (uint32_t)chpl.size());

    std::vector<uint8_t> body;   // moov's children, rebuilt
    bool hadUdta = false;
    for (size_t p = mb.hdr; p < moov.size();) {
        Box c;
        if (!ReadBox(moov, p, moov.size(), c)) break;
        if (!strcmp(c.type, "udta")) {
            hadUdta = true;
            std::vector<uint8_t> u;
            Append32(u, 0);
            u.insert(u.end(), { 'u', 'd', 't', 'a' });
            for (size_t q = c.pos + c.hdr; q < c.pos + c.size;) {
                Box g;
                if (!ReadBox(moov, q, c.pos + c.size, g)) break;
                if (strcmp(g.type, "chpl") != 0) u.insert(u.end(), moov.begin() + (ptrdiff_t)g.pos, moov.begin() + (ptrdiff_t)(g.pos + g.size));
                q = g.pos + g.size;
            }
            if (!chapters.empty()) u.insert(u.end(), chpl.begin(), chpl.end());
            Put32(u.data(), (uint32_t)u.size());
            if (u.size() > 8) body.insert(body.end(), u.begin(), u.end());
        } else {
            body.insert(body.end(), moov.begin() + (ptrdiff_t)c.pos, moov.begin() + (ptrdiff_t)(c.pos + c.size));
        }
        p = c.pos + c.size;
    }
    if (!hadUdta && !chapters.empty()) {
        std::vector<uint8_t> u;
        Append32(u, (uint32_t)(8 + chpl.size()));
        u.insert(u.end(), { 'u', 'd', 't', 'a' });
        u.insert(u.end(), chpl.begin(), chpl.end());
        body.insert(body.end(), u.begin(), u.end());
    }
    std::vector<uint8_t> newMoov;
    Append32(newMoov, (uint32_t)(8 + body.size()));
    newMoov.insert(newMoov.end(), { 'm', 'o', 'o', 'v' });
    newMoov.insert(newMoov.end(), body.begin(), body.end());
    const int64_t delta = (int64_t)newMoov.size() - moovTop->size;
    if (moovBeforeData && delta != 0 && !ShiftChunkOffsets(newMoov, 8, newMoov.size(), delta)) {
        fclose(f);
        error = "Couldn't update the video's sample table for chapters.";
        return false;
    }

    // Write a new file: everything before moov, the new moov, everything after it.
    const std::wstring tmp = path + L".chapters.tmp";
    FILE* o = OpenFile(tmp, L"wb");
    if (!o) {
        fclose(f);
        error = "Couldn't write the video with chapters.";
        return false;
    }
    std::vector<char> buf(1 << 20);
    auto copy = [&](int64_t from, int64_t len) -> bool {
        SeekTo(f, from);
        while (len > 0) {
            const size_t n = fread(buf.data(), 1, (size_t)std::min<int64_t>(len, (int64_t)buf.size()), f);
            if (n == 0 || fwrite(buf.data(), 1, n, o) != n) return false;
            len -= (int64_t)n;
        }
        return true;
    };
    bool ok = copy(0, moovTop->pos) && fwrite(newMoov.data(), 1, newMoov.size(), o) == newMoov.size();
    const int64_t after = moovTop->pos + moovTop->size, end = tops.back().pos + tops.back().size;
    if (ok && end > after) ok = copy(after, end - after);
    fclose(o);
    fclose(f);
    if (!ok || !ReplaceFile(tmp, path)) {
        RemoveFile(tmp);
        error = "Couldn't write the video with chapters.";
        return false;
    }
    return true;
}

} // namespace cs
