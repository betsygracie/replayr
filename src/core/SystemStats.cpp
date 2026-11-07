#include "core/SystemStats.h"
#include "core/Platform.h"

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

namespace cs {

struct SystemStats::Impl {
    mutable std::mutex mutex;
    SystemSnapshot snap;
    std::wstring diskPath;
    std::thread thread;
    HANDLE stopEvent = nullptr;

    void Run();
};

static uint64_t FtToU64(const FILETIME& ft) { return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime; }

void SystemStats::Impl::Run()
{
    FILETIME idle0{}, kernel0{}, user0{};
    GetSystemTimes(&idle0, &kernel0, &user0);

    PDH_HQUERY query = nullptr;
    PDH_HCOUNTER gpuCounter = nullptr;
    bool gpuOk = PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS &&
                 PdhAddEnglishCounterW(query, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &gpuCounter) == ERROR_SUCCESS &&
                 PdhCollectQueryData(query) == ERROR_SUCCESS;
    std::vector<unsigned char> buffer;

    while (WaitForSingleObject(stopEvent, 1000) == WAIT_TIMEOUT) {
        SystemSnapshot s;

        FILETIME idle{}, kernel{}, user{};
        if (GetSystemTimes(&idle, &kernel, &user)) {
            uint64_t di = FtToU64(idle) - FtToU64(idle0);
            uint64_t dk = FtToU64(kernel) - FtToU64(kernel0);   // kernel time includes idle time
            uint64_t du = FtToU64(user) - FtToU64(user0);
            if (dk + du > 0) s.cpu = 100.0f * (1.0f - (float)di / (float)(dk + du));
            idle0 = idle;
            kernel0 = kernel;
            user0 = user;
        }

        if (gpuOk && PdhCollectQueryData(query) == ERROR_SUCCESS) {
            DWORD size = 0, count = 0;
            PDH_STATUS st = PdhGetFormattedCounterArrayW(gpuCounter, PDH_FMT_DOUBLE, &size, &count, nullptr);
            if (st == (PDH_STATUS)PDH_MORE_DATA && size > 0) {
                buffer.resize(size);
                auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
                if (PdhGetFormattedCounterArrayW(gpuCounter, PDH_FMT_DOUBLE, &size, &count, items) == ERROR_SUCCESS) {
                    double total = 0.0;
                    for (DWORD i = 0; i < count; ++i)
                        if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA) total += items[i].FmtValue.doubleValue;
                    s.gpu = (float)std::min(100.0, std::max(0.0, total));
                }
            }
        }

        std::wstring path;
        {
            std::lock_guard<std::mutex> lock(mutex);
            path = diskPath;
        }
        s.diskFree = platform::DiskFreeBytes(path);

        std::lock_guard<std::mutex> lock(mutex);
        snap = s;
    }
    if (query) PdhCloseQuery(query);
}

SystemStats::SystemStats() : impl_(std::make_unique<Impl>()) {}
SystemStats::~SystemStats() { Stop(); }

void SystemStats::Start(const std::wstring& diskPath)
{
    impl_->diskPath = diskPath;
    impl_->snap.diskFree = platform::DiskFreeBytes(diskPath);
    impl_->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl_->thread = std::thread([this] { impl_->Run(); });
}

void SystemStats::Stop()
{
    if (!impl_ || !impl_->thread.joinable()) return;
    SetEvent(impl_->stopEvent);
    impl_->thread.join();
    CloseHandle(impl_->stopEvent);
    impl_->stopEvent = nullptr;
}

void SystemStats::SetDiskPath(const std::wstring& path)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->diskPath = path;
}

SystemSnapshot SystemStats::Get() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->snap;
}

} // namespace cs
