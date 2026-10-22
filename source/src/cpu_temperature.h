#pragma once
#include "cpu_temperature_protocol.h"
#include "cpu_temperature_helper_hash.h"
#include "cpu_temperature_task.h"
#include <shellapi.h>
#include <objbase.h>
#include <sddl.h>
#include <wincrypt.h>
#include <intrin.h>
#include <array>
#include <cwchar>

namespace lightmon {
// This reader stays in the ordinary taskbar process. Start() is the manual UAC
// entry point; StartAutomatic() uses only an already-authorized fixed task.
class CpuTemperatureProvider {
    HANDLE mapping_ = nullptr, helper_ = nullptr;
    HANDLE protectedHelperFile_ = INVALID_HANDLE_VALUE;
    CpuTemperatureSharedPage* page_ = nullptr;
    wchar_t mappingName_[128]{};
    CpuTemperatureStatus status_ = CpuTemperatureStatus::NeedsPermission;
    HRESULT lastError_ = S_OK;
    CpuTemperatureSnapshot latest_{};
    ULONGLONG launchTick_ = 0;
    std::uint64_t launchFileTime_ = 0;
    bool taskPending_ = false;

    static bool SupportedCpu() noexcept {
        int value[4]{}; __cpuid(value, 0);
        char vendor[13]{};
        std::memcpy(vendor, &value[1], 4); std::memcpy(vendor + 4, &value[3], 4);
        std::memcpy(vendor + 8, &value[2], 4);
        if (std::strcmp(vendor, "AuthenticAMD") != 0) return false;
        __cpuid(value, 1); const unsigned fms = static_cast<unsigned>(value[0]);
        unsigned family = (fms >> 8) & 15u;
        if (family == 15u) family += (fms >> 20) & 255u;
        const unsigned model = ((fms >> 4) & 15u) | ((fms >> 12) & 240u);
        return family == 0x19 && model == 0x61;
    }

    static HANDLE OpenVerifiedHelperFile(const wchar_t* path, wchar_t* canonical, DWORD capacity) noexcept {
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
        BY_HANDLE_FILE_INFORMATION info{};
        bool valid = GetFileInformationByHandle(file, &info) &&
                     (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0 &&
                     info.nFileSizeHigh == 0 && info.nFileSizeLow >= 16384 && info.nFileSizeLow <= 1048576;
        HCRYPTPROV provider = 0; HCRYPTHASH hash = 0;
        if (valid) valid = CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) != 0;
        if (valid) valid = CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) != 0;
        std::array<BYTE, 4096> buffer{}; DWORD read = 0;
        while (valid) {
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) { valid = false; break; }
            if (read == 0) break;
            if (!CryptHashData(hash, buffer.data(), read, 0)) valid = false;
        }
        BYTE digest[32]{}; DWORD size = sizeof(digest);
        if (valid) valid = CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0) != 0 && size == sizeof(digest) &&
                           std::memcmp(digest, kCpuTemperatureHelperSha256, sizeof(digest)) == 0;
        if (hash) CryptDestroyHash(hash);
        if (provider) CryptReleaseContext(provider, 0);
        const DWORD characters = valid ? GetFinalPathNameByHandleW(file, canonical, capacity, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS) : 0;
        valid = valid && characters > 7 && characters < capacity && std::wcsncmp(canonical, L"\\\\?\\", 4) == 0 &&
                canonical[5] == L':' && canonical[6] == L'\\';
        if (valid) {
            // ShellExecute receives the canonical drive path, and the open
            // handle prohibits overwriting/deleting it throughout UAC.
            std::memmove(canonical, canonical + 4, (characters - 3) * sizeof(wchar_t));
            return file;
        }
        CloseHandle(file); return INVALID_HANDLE_VALUE;
    }

    bool CreateMapping() noexcept {
        HANDLE token = nullptr; DWORD length = 0;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
        GetTokenInformation(token, TokenUser, nullptr, 0, &length);
        std::array<BYTE, 512> tokenData{};
        const bool gotUser = length > 0 && length <= tokenData.size() &&
            GetTokenInformation(token, TokenUser, tokenData.data(), static_cast<DWORD>(tokenData.size()), &length);
        CloseHandle(token);
        if (!gotUser) return false;
        LPWSTR sid = nullptr;
        if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(tokenData.data())->User.Sid, &sid)) return false;
        wchar_t acl[512]{};
        const int formatted = swprintf_s(acl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;%s)", sid);
        LocalFree(sid);
        if (formatted < 0) return false;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(acl, SDDL_REVISION_1, &descriptor, nullptr)) return false;
        GUID nonce{}; const HRESULT guidHr = CoCreateGuid(&nonce);
        if (FAILED(guidHr)) { LocalFree(descriptor); SetLastError(ERROR_GEN_FAILURE); return false; }
        swprintf_s(mappingName_, L"Local\\LightMon.Cpu.%lu.%08lX%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X",
            GetCurrentProcessId(), nonce.Data1, nonce.Data2, nonce.Data3, nonce.Data4[0], nonce.Data4[1],
            nonce.Data4[2], nonce.Data4[3], nonce.Data4[4], nonce.Data4[5], nonce.Data4[6], nonce.Data4[7]);
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, 0, 4096, mappingName_);
        const DWORD createError = GetLastError(); LocalFree(descriptor);
        if (!mapping_ || createError == ERROR_ALREADY_EXISTS) {
            if (mapping_) { CloseHandle(mapping_); mapping_ = nullptr; }
            SetLastError(createError == ERROR_ALREADY_EXISTS ? ERROR_ALREADY_EXISTS : createError);
            return false;
        }
        page_ = static_cast<CpuTemperatureSharedPage*>(MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 4096));
        if (!page_) return false;
        FILETIME creation{}, exit{}, kernel{}, user{};
        if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user)) return false;
        *page_ = {}; page_->parentPid = GetCurrentProcessId();
        page_->parentCreationTime = CpuFileTimeValue(creation);
        return true;
    }

    bool FailAutomaticLaunch(HRESULT error) noexcept {
        Stop(); status_ = CpuTemperatureStatus::LaunchFailed;
        lastError_ = FAILED(error) ? error : E_FAIL; return false;
    }

    bool CompleteAutomaticHandshake(DWORD pid) noexcept {
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return FailAutomaticLaunch(HRESULT_FROM_WIN32(GetLastError()));
        struct ProcessGuard { HANDLE value; ~ProcessGuard() { if (value) CloseHandle(value); } } guard{process};
        wchar_t image[32768]{}; DWORD imageSize = static_cast<DWORD>(std::size(image));
        FILETIME creation{}, exit{}, kernel{}, user{};
        DWORD processSession = 0, ownSession = 0;
        if (WaitForSingleObject(process, 0) != WAIT_TIMEOUT ||
            !QueryFullProcessImageNameW(process, 0, image, &imageSize) ||
            _wcsicmp(image, kCpuAutomaticHelperPath) != 0 ||
            !GetProcessTimes(process, &creation, &exit, &kernel, &user) ||
            CpuFileTimeValue(creation) < launchFileTime_ ||
            !ProcessIdToSessionId(pid, &processSession) ||
            !ProcessIdToSessionId(GetCurrentProcessId(), &ownSession) ||
            processSession == 0 || processSession != ownSession)
            return FailAutomaticLaunch(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        HANDLE processToken = nullptr, ownToken = nullptr;
        if (!OpenProcessToken(process, TOKEN_QUERY, &processToken))
            return FailAutomaticLaunch(HRESULT_FROM_WIN32(GetLastError()));
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &ownToken)) {
            const auto error = GetLastError(); CloseHandle(processToken);
            return FailAutomaticLaunch(HRESULT_FROM_WIN32(error));
        }
        std::array<BYTE, 512> processData{}, ownData{}; DWORD processLength = 0, ownLength = 0;
        TOKEN_ELEVATION elevation{}; DWORD elevationLength = 0;
        const bool matching = GetTokenInformation(processToken, TokenUser, processData.data(),
            static_cast<DWORD>(processData.size()), &processLength) &&
            GetTokenInformation(ownToken, TokenUser, ownData.data(), static_cast<DWORD>(ownData.size()), &ownLength) &&
            EqualSid(reinterpret_cast<TOKEN_USER*>(processData.data())->User.Sid,
                     reinterpret_cast<TOKEN_USER*>(ownData.data())->User.Sid) &&
            GetTokenInformation(processToken, TokenElevation, &elevation, sizeof(elevation), &elevationLength) &&
            elevation.TokenIsElevated != 0;
        CloseHandle(ownToken); CloseHandle(processToken);
        if (!matching) return FailAutomaticLaunch(E_ACCESSDENIED);
        // The expected hash was checked and this image file has remained open
        // without write/delete sharing since before the task was launched.
        helper_ = process; guard.value = nullptr; taskPending_ = false; return true;
    }

public:
    CpuTemperatureProvider() noexcept { if (!SupportedCpu()) status_ = CpuTemperatureStatus::UnsupportedCpu; }
    ~CpuTemperatureProvider() { Stop(); }
    CpuTemperatureProvider(const CpuTemperatureProvider&) = delete;
    CpuTemperatureProvider& operator=(const CpuTemperatureProvider&) = delete;

    bool Start(HWND owner) noexcept {
        if (!SupportedCpu()) { status_ = CpuTemperatureStatus::UnsupportedCpu; return false; }
        if (Active()) return true;
        Stop();
        wchar_t path[32768]{}, canonical[32768]{};
        const DWORD count = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
        wchar_t* last = count > 0 && count < std::size(path) ? std::wcsrchr(path, L'\\') : nullptr;
        if (!last || wcscpy_s(last + 1, std::size(path) - static_cast<std::size_t>(last + 1 - path), L"LightMonCpu.exe") != 0) {
            status_ = CpuTemperatureStatus::LaunchFailed; lastError_ = HRESULT_FROM_WIN32(ERROR_INVALID_IMAGE_HASH); return false;
        }
        const HANDLE verified = OpenVerifiedHelperFile(path, canonical, static_cast<DWORD>(std::size(canonical)));
        if (verified == INVALID_HANDLE_VALUE) {
            status_ = CpuTemperatureStatus::LaunchFailed; lastError_ = HRESULT_FROM_WIN32(ERROR_INVALID_IMAGE_HASH); return false;
        }
        struct FileGuard { HANDLE value; ~FileGuard() { CloseHandle(value); } } guard{verified};
        if (!CreateMapping()) {
            const DWORD error = GetLastError(); Stop(); status_ = CpuTemperatureStatus::LaunchFailed;
            lastError_ = HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE); return false;
        }
        wchar_t arguments[384]{};
        swprintf_s(arguments, L"--parent %lu --parent-start %llu --mapping \"%s\"",
                   page_->parentPid, page_->parentCreationTime, mappingName_);
        SHELLEXECUTEINFOW execute{sizeof(execute)};
        execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        execute.hwnd = owner; execute.lpVerb = L"runas"; execute.lpFile = canonical;
        execute.lpParameters = arguments; execute.nShow = SW_HIDE;
        if (!ShellExecuteExW(&execute) || !execute.hProcess) {
            const DWORD error = GetLastError(); Stop();
            status_ = error == ERROR_CANCELLED ? CpuTemperatureStatus::LaunchCancelled : CpuTemperatureStatus::LaunchFailed;
            lastError_ = HRESULT_FROM_WIN32(error ? error : ERROR_GEN_FAILURE); return false;
        }
        helper_ = execute.hProcess; launchTick_ = GetTickCount64();
        status_ = CpuTemperatureStatus::Starting; lastError_ = S_OK; return true;
    }

    bool StartAutomatic(HWND owner) noexcept {
        (void)owner;
        if (!SupportedCpu()) { status_ = CpuTemperatureStatus::UnsupportedCpu; return false; }
        if (Active()) return true;
        Stop();
        wchar_t canonical[32768]{};
        protectedHelperFile_ = OpenVerifiedHelperFile(kCpuAutomaticHelperPath, canonical,
                                                      static_cast<DWORD>(std::size(canonical)));
        if (protectedHelperFile_ == INVALID_HANDLE_VALUE || _wcsicmp(canonical, kCpuAutomaticHelperPath) != 0)
            return FailAutomaticLaunch(HRESULT_FROM_WIN32(ERROR_INVALID_IMAGE_HASH));
        if (!CreateMapping()) return FailAutomaticLaunch(HRESULT_FROM_WIN32(GetLastError()));
        FILETIME beforeLaunch{}; GetSystemTimeAsFileTime(&beforeLaunch);
        launchFileTime_ = CpuFileTimeValue(beforeLaunch); launchTick_ = GetTickCount64();
        const HRESULT error = RunAutomaticCpuTemperatureTask(page_->parentPid, page_->parentCreationTime, mappingName_);
        if (FAILED(error)) return FailAutomaticLaunch(error);
        taskPending_ = true; status_ = CpuTemperatureStatus::Starting; lastError_ = S_OK; return true;
    }

    void Stop() noexcept {
        if (page_) InterlockedExchange(&page_->stop, 1);
        if (helper_) { CloseHandle(helper_); helper_ = nullptr; }
        if (page_) { UnmapViewOfFile(page_); page_ = nullptr; }
        if (mapping_) { CloseHandle(mapping_); mapping_ = nullptr; }
        if (protectedHelperFile_ != INVALID_HANDLE_VALUE) { CloseHandle(protectedHelperFile_); protectedHelperFile_ = INVALID_HANDLE_VALUE; }
        latest_ = {}; launchTick_ = 0; launchFileTime_ = 0; taskPending_ = false;
        status_ = SupportedCpu() ? CpuTemperatureStatus::NeedsPermission : CpuTemperatureStatus::UnsupportedCpu;
        lastError_ = S_OK;
    }

    bool Active() const noexcept {
        return (helper_ && WaitForSingleObject(helper_, 0) == WAIT_TIMEOUT) ||
               (taskPending_ && page_ && GetTickCount64() - launchTick_ <= 20000);
    }
    void Sample(MetricsSample& output) noexcept {
        output.cpuTemperatureAvailable = false;
        if (!page_) return;
        if (taskPending_) {
            if (GetTickCount64() - launchTick_ > 20000) {
                FailAutomaticLaunch(HRESULT_FROM_WIN32(ERROR_TIMEOUT)); return;
            }
            const DWORD pid = static_cast<DWORD>(InterlockedCompareExchange(&page_->reserved, 0, 0));
            if (pid == 0) return;
            if (!CompleteAutomaticHandshake(pid)) return;
        }
        if (!Active()) { status_ = CpuTemperatureStatus::HelperExited; return; }
        CpuTemperatureSnapshot current{};
        if (!ReadCpuTemperatureSnapshot(page_, current)) { status_ = CpuTemperatureStatus::ReadFailed; return; }
        latest_ = current; status_ = current.status; lastError_ = static_cast<HRESULT>(current.lastError);
        const auto now = GetTickCount64();
        if (current.status == CpuTemperatureStatus::Available) {
            if (!IsFreshCpuTemperature(current, now)) { status_ = CpuTemperatureStatus::Stale; return; }
            output.cpuTemperatureC = (current.temperatureMilliC + 500) / 1000;
            output.cpuTemperatureAvailable = true;
        } else if (current.status == CpuTemperatureStatus::Starting && now - launchTick_ > 20000) {
            status_ = CpuTemperatureStatus::Stale;
        }
    }
    CpuTemperatureStatus Status() const noexcept { return status_; }
    HRESULT LastError() const noexcept { return lastError_; }
    std::uint32_t Raw() const noexcept { return latest_.raw; }
    std::uint64_t LastTick() const noexcept { return latest_.sampledAtTickMs; }
};
} // namespace lightmon
