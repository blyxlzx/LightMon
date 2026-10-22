#pragma once
#include <winioctl.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cwchar>

namespace lightmon {
// Native read-only temperature queries. Cache sensors for five seconds, keep
// unsupported/removable devices out of the hot path, and re-enumerate each minute.
class DiskTemperatureProvider {
    struct Drive {
        HANDLE handle = INVALID_HANDLE_VALUE;
        bool supported = true;
        wchar_t model[96]{};
    };
    std::array<Drive, 32> drives_{};
    std::array<DiskTemperatureSample, kDiskTemperatureCapacity> readings_{};
    std::size_t count_ = 0;
    ULONGLONG enumerateTick_ = 0, sampleTick_ = 0;

    void Close() noexcept {
        for (auto& drive : drives_) {
            if (drive.handle != INVALID_HANDLE_VALUE) CloseHandle(drive.handle);
            drive = {};
        }
    }
    void Enumerate(ULONGLONG now) noexcept {
        Close(); enumerateTick_ = now;
        // PhysicalDrive indices may be reused after hotplug. Never carry a
        // previous device's temperatures into the newly opened handles.
        readings_ = {}; count_ = 0; sampleTick_ = 0;
        for (unsigned index = 0; index < drives_.size(); ++index) {
            wchar_t name[32]{}, target[512]{}, path[40]{};
            swprintf_s(name, L"PhysicalDrive%u", index);
            if (!QueryDosDeviceW(name, target, static_cast<DWORD>(std::size(target)))) continue;
            swprintf_s(path, L"\\\\.\\PhysicalDrive%u", index);
            auto& drive = drives_[index];
            drive.handle = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, 0, nullptr);
            if (drive.handle == INVALID_HANDLE_VALUE) continue;
            STORAGE_PROPERTY_QUERY query{};
            query.PropertyId = StorageDeviceProperty;
            query.QueryType = PropertyStandardQuery;
            std::array<DWORD, 256> buffer{}; DWORD bytes = 0;
            if (!DeviceIoControl(drive.handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                                 buffer.data(), static_cast<DWORD>(sizeof(buffer)), &bytes, nullptr) ||
                bytes < sizeof(STORAGE_DEVICE_DESCRIPTOR) || bytes > sizeof(buffer)) continue;
            const auto* descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
            if (descriptor->Size < sizeof(STORAGE_DEVICE_DESCRIPTOR)) continue;
            // A long descriptor can be truncated by this bounded query. Use
            // only the bytes actually returned and declared by the device.
            const DWORD extent = (std::min)(bytes, descriptor->Size);
            const DWORD offset = descriptor->ProductIdOffset;
            if (offset < offsetof(STORAGE_DEVICE_DESCRIPTOR, RawDeviceProperties) || offset >= extent) continue;
            const char* model = reinterpret_cast<const char*>(buffer.data()) + offset;
            const auto limit = extent - offset;
            const char* end = static_cast<const char*>(std::memchr(model, 0, limit));
            if (end) MultiByteToWideChar(CP_ACP, 0, model, static_cast<int>(end - model),
                                      drive.model, static_cast<int>(std::size(drive.model) - 1));
        }
    }
    void Refresh(ULONGLONG now) noexcept {
        sampleTick_ = now; count_ = 0;
        for (unsigned disk = 0; disk < drives_.size(); ++disk) {
            auto& drive = drives_[disk];
            if (drive.handle == INVALID_HANDLE_VALUE || !drive.supported) continue;
            STORAGE_PROPERTY_QUERY query{};
            query.PropertyId = StorageDeviceTemperatureProperty;
            query.QueryType = PropertyStandardQuery;
            std::array<DWORD, 1024> buffer{}; DWORD bytes = 0;
            if (!DeviceIoControl(drive.handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                                 buffer.data(), static_cast<DWORD>(sizeof(buffer)), &bytes, nullptr)) {
                const auto error = GetLastError();
                if (error == ERROR_INVALID_FUNCTION || error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_PARAMETER)
                    drive.supported = false;
                // A transient device error gets another attempt at the next
                // five-second refresh, instead of being hidden for a minute.
                continue;
            }
            constexpr std::size_t offset = offsetof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR, TemperatureInfo);
            if (bytes < offset || bytes > sizeof(buffer)) continue;
            const auto* descriptor = reinterpret_cast<const STORAGE_TEMPERATURE_DATA_DESCRIPTOR*>(buffer.data());
            if (descriptor->Version < sizeof(STORAGE_TEMPERATURE_DATA_DESCRIPTOR) ||
                descriptor->Size > bytes || descriptor->Size < offset ||
                descriptor->InfoCount > (descriptor->Size - offset) / sizeof(STORAGE_TEMPERATURE_INFO)) continue;
            for (unsigned i = 0; i < descriptor->InfoCount && count_ < readings_.size(); ++i) {
                const auto& sensor = descriptor->TemperatureInfo[i];
                // Reject Windows' unknown sentinels and physically implausible values.
                if (sensor.Temperature < -50 || sensor.Temperature > 200) continue;
                bool duplicate = false;
                for (std::size_t n = 0; n < count_; ++n)
                    if (readings_[n].diskIndex == disk && readings_[n].sensorIndex == sensor.Index) duplicate = true;
                if (duplicate) continue;
                auto& reading = readings_[count_++]; reading = {};
                reading.diskIndex = disk; reading.sensorIndex = sensor.Index;
                reading.temperatureC = sensor.Temperature;
                std::wmemcpy(reading.model, drive.model, std::size(reading.model));
            }
        }
    }
public:
    ~DiskTemperatureProvider() { Close(); }
    void Sample(MetricsSample& sample) noexcept {
        const auto now = GetTickCount64();
        if (!enumerateTick_ || now - enumerateTick_ >= 60000) Enumerate(now);
        if (!sampleTick_ || now - sampleTick_ >= 5000) Refresh(now);
        if (now - sampleTick_ > 15000) return;
        sample.diskTemperatureCount = count_;
        for (std::size_t i = 0; i < count_; ++i) {
            sample.diskTemperatures[i] = readings_[i];
            if (!sample.diskTemperatureAvailable || readings_[i].temperatureC > sample.diskTemperatureC) {
                sample.diskTemperatureAvailable = true;
                sample.diskTemperatureC = readings_[i].temperatureC;
                sample.hottestDiskIndex = readings_[i].diskIndex;
                sample.hottestDiskSensorIndex = readings_[i].sensorIndex;
            }
        }
    }
};
} // namespace lightmon
