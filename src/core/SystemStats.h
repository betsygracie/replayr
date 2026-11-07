#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace cs {

struct SystemSnapshot {
    float cpu = -1.0f;        // percent, -1 = unknown
    float gpu = -1.0f;        // percent (3D engine), -1 = unavailable
    uint64_t diskFree = 0;    // bytes free on the clips drive
};

// CPU (GetSystemTimes), GPU (the same PDH counters Task Manager uses) and free disk space,
// sampled once a second on a background thread.
class SystemStats {
public:
    SystemStats();
    ~SystemStats();
    void Start(const std::wstring& diskPath);
    void Stop();
    void SetDiskPath(const std::wstring& path);
    SystemSnapshot Get() const;

    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
};

} // namespace cs
