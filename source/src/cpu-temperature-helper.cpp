#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <sddl.h>
#include <wincrypt.h>
#include <intrin.h>
#include <array>
#include <cwchar>
#include <cstring>
#include "cpu_temperature_protocol.h"

namespace {
using lightmon::CpuTemperatureStatus;
using lightmon::CpuTemperatureSnapshot;
using lightmon::CpuTemperatureSharedPage;

constexpr BYTE kModuleSha256[32] = {
    0xDA, 0xE7, 0x46, 0x15, 0x76, 0x1B, 0x78, 0xBD, 0xF0, 0x64, 0xDF, 0xB3, 0xE1, 0x36, 0x25, 0x2D, 0xDC, 0xC6, 0xFC, 0x72, 0x7D, 0x88, 0xF1, 0x47, 0x38, 0xD0, 0xE5, 0x80, 0x0D, 0x42, 0x7A, 0x91
};

bool NumericArgument(const wchar_t*& text, std::uint64_t& value) noexcept {
    value = 0; if (!text || *text < L'0' || *text > L'9') return false;
    for (; *text >= L'0' && *text <= L'9'; ++text) {
        const unsigned digit = static_cast<unsigned>(*text - L'0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    return value != 0;
}
bool ConsumeArgument(const wchar_t*& text, const wchar_t* expected) noexcept {
    const auto size = std::wcslen(expected);
    if (std::wcsncmp(text, expected, size) != 0) return false;
    text += size; return true;
}
bool MappingArgument(const wchar_t* text, DWORD parent) noexcept {
    wchar_t prefix[64]{}; swprintf_s(prefix, L"Local\\LightMon.Cpu.%lu.", parent);
    const auto size = std::wcslen(prefix);
    if (!text || std::wcsncmp(text, prefix, size) != 0 || std::wcslen(text) != size + 32) return false;
    for (const wchar_t* at = text + size; *at; ++at)
        if (!((*at >= L'0' && *at <= L'9') || (*at >= L'A' && *at <= L'F') || (*at >= L'a' && *at <= L'f'))) return false;
    return true;
}
bool ParentHasSameUserAndSession(HANDLE parent, DWORD parentPid) noexcept {
    DWORD parentSession = 0, ownSession = 0;
    if (!ProcessIdToSessionId(parentPid, &parentSession) ||
        !ProcessIdToSessionId(GetCurrentProcessId(), &ownSession) ||
        parentSession == 0 || parentSession != ownSession) return false;
    HANDLE parentToken = nullptr, ownToken = nullptr;
    if (!OpenProcessToken(parent, TOKEN_QUERY, &parentToken)) return false;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &ownToken)) { CloseHandle(parentToken); return false; }
    std::array<BYTE, 512> parentData{}, ownData{}; DWORD parentLength = 0, ownLength = 0;
    const bool gotTokens = GetTokenInformation(parentToken, TokenUser, parentData.data(),
        static_cast<DWORD>(parentData.size()), &parentLength) &&
        GetTokenInformation(ownToken, TokenUser, ownData.data(), static_cast<DWORD>(ownData.size()), &ownLength);
    CloseHandle(ownToken); CloseHandle(parentToken);
    return gotTokens && EqualSid(reinterpret_cast<TOKEN_USER*>(parentData.data())->User.Sid,
                                 reinterpret_cast<TOKEN_USER*>(ownData.data())->User.Sid);
}
bool SupportedCpu() noexcept {
    int value[4]{}; __cpuid(value, 0); char vendor[13]{};
    std::memcpy(vendor, &value[1], 4); std::memcpy(vendor + 4, &value[3], 4);
    std::memcpy(vendor + 8, &value[2], 4);
    if (std::strcmp(vendor, "AuthenticAMD") != 0) return false;
    __cpuid(value, 1); const unsigned fms = static_cast<unsigned>(value[0]);
    unsigned family = (fms >> 8) & 15u;
    if (family == 15u) family += (fms >> 20) & 255u;
    const unsigned model = ((fms >> 4) & 15u) | ((fms >> 12) & 240u);
    return family == 0x19 && model == 0x61;
}
bool HashModule(const BYTE* data, DWORD bytes) noexcept {
    HCRYPTPROV provider = 0; HCRYPTHASH hash = 0;
    bool valid = CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) != 0;
    if (valid) valid = CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) != 0;
    if (valid) valid = CryptHashData(hash, data, bytes, 0) != 0;
    BYTE digest[32]{}; DWORD size = sizeof(digest);
    if (valid) valid = CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0) != 0 && size == sizeof(digest) &&
                       std::memcmp(digest, kModuleSha256, sizeof(digest)) == 0;
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    return valid;
}

class PawnIoCpuSensor {
    using Version = HRESULT(WINAPI*)(PULONG);
    using Open = HRESULT(WINAPI*)(PHANDLE);
    using Load = HRESULT(WINAPI*)(HANDLE, const UCHAR*, SIZE_T);
    using Execute = HRESULT(WINAPI*)(HANDLE, PCSTR, const ULONG64*, SIZE_T, PULONG64, SIZE_T, PSIZE_T);
    using Close = HRESULT(WINAPI*)(HANDLE);
    HMODULE dll_ = nullptr;
    HANDLE executor_ = nullptr, pciMutex_ = nullptr;
    Execute execute_ = nullptr; Close close_ = nullptr;
    CpuTemperatureStatus status_ = CpuTemperatureStatus::Starting;
    HRESULT error_ = S_OK;
public:
    ~PawnIoCpuSensor() {
        if (executor_ && close_) close_(executor_);
        if (pciMutex_) CloseHandle(pciMutex_);
        if (dll_) FreeLibrary(dll_);
    }
    bool Initialize() noexcept {
        if (!SupportedCpu()) { status_ = CpuTemperatureStatus::UnsupportedCpu; return false; }
        // Protected HKLM configuration, not the inheritable ProgramFiles
        // environment variable or writable user preferences. Avoid loading
        // the Shell/COM runtime in this small background helper.
        wchar_t programFiles[1024]{}, path[1100]{}; DWORD folderBytes = sizeof(programFiles);
        const LSTATUS folderStatus = RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion",
            L"ProgramFilesDir", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, programFiles, &folderBytes);
        const int formatted = folderStatus == ERROR_SUCCESS && programFiles[0] &&
            programFiles[1] == L':' && programFiles[2] == L'\\' ? swprintf_s(path, L"%s\\PawnIO\\PawnIOLib.dll", programFiles) : -1;
        if (formatted < 0) {
            status_ = CpuTemperatureStatus::DllMissing;
            error_ = folderStatus == ERROR_SUCCESS ? E_INVALIDARG : HRESULT_FROM_WIN32(folderStatus); return false;
        }
        dll_ = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!dll_) { status_ = CpuTemperatureStatus::DllMissing; error_ = HRESULT_FROM_WIN32(GetLastError()); return false; }
        const auto version = reinterpret_cast<Version>(GetProcAddress(dll_, "pawnio_version"));
        const auto open = reinterpret_cast<Open>(GetProcAddress(dll_, "pawnio_open"));
        const auto load = reinterpret_cast<Load>(GetProcAddress(dll_, "pawnio_load"));
        execute_ = reinterpret_cast<Execute>(GetProcAddress(dll_, "pawnio_execute"));
        close_ = reinterpret_cast<Close>(GetProcAddress(dll_, "pawnio_close"));
        ULONG ver = 0;
        if (!version || !open || !load || !execute_ || !close_ || FAILED(error_ = version(&ver)) || (ver >> 16) != 2) {
            status_ = CpuTemperatureStatus::DllInvalid; if (SUCCEEDED(error_)) error_ = E_NOINTERFACE; return false;
        }
        error_ = open(&executor_);
        if (FAILED(error_) || !executor_ || executor_ == INVALID_HANDLE_VALUE) {
            executor_ = nullptr;
            status_ = error_ == E_ACCESSDENIED ? CpuTemperatureStatus::AccessDenied : CpuTemperatureStatus::DriverMissing;
            if (SUCCEEDED(error_)) error_ = E_HANDLE; return false;
        }
        HMODULE self = GetModuleHandleW(nullptr);
        HRSRC resource = FindResourceW(self, MAKEINTRESOURCEW(101), RT_RCDATA);
        const DWORD bytes = resource ? SizeofResource(self, resource) : 0;
        HGLOBAL loaded = resource ? LoadResource(self, resource) : nullptr;
        const BYTE* blob = loaded ? static_cast<const BYTE*>(LockResource(loaded)) : nullptr;
        if (!blob || bytes != 10652 || !HashModule(blob, bytes)) {
            status_ = CpuTemperatureStatus::ModuleRejected; error_ = HRESULT_FROM_WIN32(ERROR_INVALID_IMAGE_HASH); return false;
        }
        // Reviewed official module main only verifies x64/AMD/supported family.
        // Its signature is checked by the already-installed PawnIO driver.
        error_ = load(executor_, blob, bytes);
        if (FAILED(error_)) { status_ = CpuTemperatureStatus::ModuleRejected; return false; }

        // Match the documented global Access_PCI coordination object. New
        // objects permit all sensor readers to wait/release; existing ACLs are
        // never modified. A failed mutex is a hard error, never an unlocked read.
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;WD)", SDDL_REVISION_1, &descriptor, nullptr)) {
            status_ = CpuTemperatureStatus::MutexUnavailable; error_ = HRESULT_FROM_WIN32(GetLastError()); return false;
        }
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
        pciMutex_ = CreateMutexExW(&attributes, L"Global\\Access_PCI", 0, SYNCHRONIZE | MUTEX_MODIFY_STATE);
        const DWORD mutexError = GetLastError(); LocalFree(descriptor);
        if (!pciMutex_) {
            pciMutex_ = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, L"Global\\Access_PCI");
            if (!pciMutex_) { status_ = CpuTemperatureStatus::MutexUnavailable; error_ = HRESULT_FROM_WIN32(mutexError); return false; }
        }
        status_ = CpuTemperatureStatus::Starting; error_ = S_OK; return true;
    }
    CpuTemperatureSnapshot Read() noexcept {
        CpuTemperatureSnapshot value{};
        value.sampledAtTickMs = GetTickCount64(); value.status = status_; value.lastError = error_;
        if (!executor_ || !execute_ || !pciMutex_) return value;
        const DWORD wait = WaitForSingleObject(pciMutex_, 10);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            value.status = CpuTemperatureStatus::MutexUnavailable;
            value.lastError = HRESULT_FROM_WIN32(wait == WAIT_TIMEOUT ? WAIT_TIMEOUT : GetLastError()); return value;
        }
        const ULONG64 input = 0x59800; ULONG64 output = 0; SIZE_T returned = 0;
        // This is the only module function executed. The SMN read writes its
        // PCI address selector (0x60) before reading data (0x64); it does not
        // write thermal values, MSRs, clocks, voltages, fan or power controls.
        const HRESULT hr = execute_(executor_, "ioctl_read_smn", &input, 1, &output, 1, &returned);
        const BOOL released = ReleaseMutex(pciMutex_);
        if (FAILED(hr) || returned != 1 || output > UINT32_MAX || !released) {
            value.status = CpuTemperatureStatus::ReadFailed;
            value.lastError = FAILED(hr) ? hr : (!released ? HRESULT_FROM_WIN32(GetLastError()) : E_UNEXPECTED);
            return value;
        }
        value.raw = static_cast<std::uint32_t>(output);
        if (!lightmon::DecodeCpuTemperatureRaw(value.raw, value.temperatureMilliC)) {
            value.status = CpuTemperatureStatus::InvalidSensor; value.lastError = E_UNEXPECTED; return value;
        }
        value.status = CpuTemperatureStatus::Available; value.lastError = S_OK; return value;
    }
    CpuTemperatureStatus Status() const noexcept { return status_; }
    HRESULT LastError() const noexcept { return error_; }
};
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR commandLine, int) {
    const wchar_t* argument = commandLine;
    if (!argument || !ConsumeArgument(argument, L"--parent ")) return 2;
    std::uint64_t parentValue = 0, startValue = 0;
    if (!NumericArgument(argument, parentValue) || parentValue > UINT32_MAX ||
        !ConsumeArgument(argument, L" --parent-start ") || !NumericArgument(argument, startValue) ||
        !ConsumeArgument(argument, L" --mapping \"")) return 3;
    const wchar_t* closing = std::wcschr(argument, L'"');
    wchar_t mappingName[128]{};
    if (!closing || closing[1] != 0 || closing == argument || closing - argument >= static_cast<std::ptrdiff_t>(std::size(mappingName))) return 3;
    std::memcpy(mappingName, argument, static_cast<std::size_t>(closing - argument) * sizeof(wchar_t));
    if (!MappingArgument(mappingName, static_cast<DWORD>(parentValue))) return 3;
    const DWORD parentPid = static_cast<DWORD>(parentValue);
    HANDLE parent = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, parentPid);
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!parent || !GetProcessTimes(parent, &creation, &exit, &kernel, &user) ||
        lightmon::CpuFileTimeValue(creation) != startValue || WaitForSingleObject(parent, 0) != WAIT_TIMEOUT ||
        !ParentHasSameUserAndSession(parent, parentPid)) {
        if (parent) CloseHandle(parent); return 4;
    }
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, mappingName);
    auto* page = mapping ? static_cast<CpuTemperatureSharedPage*>(MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 4096)) : nullptr;
    if (!page || page->magic != lightmon::kCpuTemperatureProtocolMagic || page->version != lightmon::kCpuTemperatureProtocolVersion ||
        page->parentPid != parentPid || page->parentCreationTime != startValue ||
        InterlockedCompareExchange(&page->stop, 0, 0) != 0 || InterlockedCompareExchange(&page->sequence, 0, 0) != 0 ||
        InterlockedCompareExchange(&page->reserved, 0, 0) != 0) {
        if (page) UnmapViewOfFile(page); if (mapping) CloseHandle(mapping); CloseHandle(parent); return 5;
    }
    // Parent verifies this real process's protected image, creation time, SID,
    // and session. Task Scheduler's EnginePID is intentionally never used.
    InterlockedExchange(&page->reserved, static_cast<LONG>(GetCurrentProcessId()));
    {
        PawnIoCpuSensor sensor;
        if (!sensor.Initialize()) {
            CpuTemperatureSnapshot failure{}; failure.status = sensor.Status(); failure.lastError = sensor.LastError();
            failure.sampledAtTickMs = GetTickCount64(); lightmon::PublishCpuTemperatureSnapshot(page, failure);
            // Keep the explicit failure available while parent is running so
            // its reader can distinguish a hardware error from helper exit.
            while (WaitForSingleObject(parent, lightmon::kCpuTemperaturePollMs) == WAIT_TIMEOUT &&
                   InterlockedCompareExchange(&page->stop, 0, 0) == 0) {}
        } else {
            do {
                if (InterlockedCompareExchange(&page->stop, 0, 0) != 0) break;
                lightmon::PublishCpuTemperatureSnapshot(page, sensor.Read());
            } while (WaitForSingleObject(parent, lightmon::kCpuTemperaturePollMs) == WAIT_TIMEOUT);
        }
    }
    UnmapViewOfFile(page); CloseHandle(mapping); CloseHandle(parent); return 0;
}
