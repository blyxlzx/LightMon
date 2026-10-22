#pragma once

#include <windows.h>
#include <exdisp.h>
#include <shellapi.h>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <iterator>

// Low-frequency local lifecycle evidence. No sampler, periodic work, shared
// process handles, exception hooks, or background workers are introduced here.
namespace lightmon::lifetime {

inline constexpr wchar_t kRelayArgument[] = L"--lightmon-independent-start";
inline constexpr wchar_t kFinalRelayArgument[] = L"--lightmon-independent-final";
inline constexpr DWORD kLogLimit = 64u * 1024u;
inline constexpr DWORD kPathCapacity = 32768;
inline wchar_t logPath[kPathCapacity]{};
inline bool logInitialized = false;

struct JobState {
    bool membershipKnown = false;
    bool inJob = false;
    DWORD membershipError = ERROR_SUCCESS;
    bool limitsKnown = false;
    DWORD limitFlags = 0;
    DWORD limitsError = ERROR_SUCCESS;
};

inline JobState CurrentJob() noexcept {
    JobState state;
    BOOL inJob = FALSE;
    if (!IsProcessInJob(GetCurrentProcess(), nullptr, &inJob)) {
        state.membershipError = GetLastError();
        return state;
    }
    state.membershipKnown = true;
    state.inJob = inJob != FALSE;
    if (!state.inJob) return state;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    if (!QueryInformationJobObject(nullptr, JobObjectExtendedLimitInformation,
                                  &limits, sizeof(limits), nullptr)) {
        state.limitsError = GetLastError();
        return state;
    }
    state.limitsKnown = true;
    state.limitFlags = limits.BasicLimitInformation.LimitFlags;
    return state;
}

struct HeapPath {
    wchar_t* value = static_cast<wchar_t*>(HeapAlloc(GetProcessHeap(), 0,
        static_cast<SIZE_T>(kPathCapacity) * sizeof(wchar_t)));
    ~HeapPath() noexcept { if (value) HeapFree(GetProcessHeap(), 0, value); }
    HeapPath() noexcept = default;
    HeapPath(const HeapPath&) = delete;
    HeapPath& operator=(const HeapPath&) = delete;
};

// Keep Unicode absolute paths and use the extended Win32 form for file and
// process APIs. Return an explicit error instead of silently truncating.
inline bool ExtendedPath(wchar_t* path, DWORD capacity, DWORD& error) noexcept {
    const SIZE_T length = std::wcslen(path);
    if (length >= 4 && !std::wcsncmp(path, L"\\\\?\\", 4)) return true;
    if (length >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        if (length + 6 >= capacity) { error = ERROR_FILENAME_EXCED_RANGE; return false; }
        std::memmove(path + 8, path + 2, (length - 1) * sizeof(wchar_t));
        std::memcpy(path, L"\\\\?\\UNC\\", 8 * sizeof(wchar_t));
        return true;
    }
    if (length >= 3 && path[1] == L':' && path[2] == L'\\') {
        if (length + 4 >= capacity) { error = ERROR_FILENAME_EXCED_RANGE; return false; }
        std::memmove(path + 4, path, (length + 1) * sizeof(wchar_t));
        std::memcpy(path, L"\\\\?\\", 4 * sizeof(wchar_t));
        return true;
    }
    error = ERROR_BAD_PATHNAME;
    return false;
}

inline bool ModulePath(wchar_t* path, DWORD capacity, DWORD& error) noexcept {
    const DWORD count = GetModuleFileNameW(nullptr, path, capacity);
    if (!count) { error = GetLastError(); return false; }
    if (count >= capacity) { error = ERROR_FILENAME_EXCED_RANGE; return false; }
    return ExtendedPath(path, capacity, error);
}

inline bool Init() noexcept {
    if (logInitialized) return logPath[0] != L'\0';
    logInitialized = true;
    HeapPath module;
    DWORD error = ERROR_SUCCESS;
    if (!module.value || !ModulePath(module.value, kPathCapacity, error)) return false;
    wchar_t* slash = std::wcsrchr(module.value, L'\\');
    if (!slash) return false;
    const SIZE_T directory = static_cast<SIZE_T>(slash + 1 - module.value);
    constexpr wchar_t basename[] = L"LightMon.lifecycle.log";
    if (directory + std::size(basename) > kPathCapacity) return false;
    std::memcpy(logPath, module.value, directory * sizeof(wchar_t));
    std::memcpy(logPath + directory, basename, sizeof(basename));
    return true;
}

inline void WriteLine(const char* bytes, DWORD count) noexcept {
    if (!bytes || !count || count > 1024 || !Init()) return;
    HANDLE file = INVALID_HANDLE_VALUE;
    // Serialize the brief parent/child handoff without a named mutex or a
    // persistent handle. Readers may tail the log if they share write access.
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        file = CreateFileW(logPath, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_SHARING_VIOLATION) return;
        if (attempt < 3) Sleep(1);
    }
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{}, position{};
    bool ready = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0;
    if (ready && static_cast<ULONGLONG>(size.QuadPart) + count > kLogLimit) {
        ready = SetFilePointerEx(file, position, nullptr, FILE_BEGIN) != FALSE &&
            SetEndOfFile(file) != FALSE;
    }
    if (ready) ready = SetFilePointerEx(file, position, nullptr, FILE_END) != FALSE;
    DWORD written = 0;
    if (ready && WriteFile(file, bytes, count, &written, nullptr) && written == count)
        FlushFileBuffers(file);
    CloseHandle(file);
}

inline void SafeToken(const char* input, char* output, SIZE_T capacity) noexcept {
    if (!capacity) return;
    SIZE_T at = 0;
    if (input) for (; input[at] && at + 1 < capacity; ++at) {
        const unsigned char c = static_cast<unsigned char>(input[at]);
        output[at] = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-') ?
            static_cast<char>(c) : '_';
    }
    output[at] = '\0';
}

inline void LogEvent(const char* reason, DWORD a = 0, DWORD b = 0,
                     ULONGLONG c = 0) noexcept {
    SYSTEMTIME utc{}; GetSystemTime(&utc);
    char event[64]{}, line[384]{}; SafeToken(reason, event, sizeof(event));
    const int count = _snprintf_s(line, sizeof(line), _TRUNCATE,
        "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ pid=%lu event=%s a=%lu b=%lu c=%llu\r\n",
        utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond,
        utc.wMilliseconds, GetCurrentProcessId(), event, a, b, c);
    if (count > 0) WriteLine(line, static_cast<DWORD>(count));
}

inline void LogStartup(const char* version, bool alreadyRelayed) noexcept {
    SYSTEMTIME utc{}; GetSystemTime(&utc);
    FILETIME created{}, exit{}, kernel{}, user{};
    const bool gotTimes = GetProcessTimes(GetCurrentProcess(), &created, &exit, &kernel, &user) != FALSE;
    const ULONGLONG creation = (static_cast<ULONGLONG>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    char release[32]{}, line[512]{}; SafeToken(version, release, sizeof(release));
    const JobState job = CurrentJob();
    const int count = _snprintf_s(line, sizeof(line), _TRUNCATE,
        "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ pid=%lu event=START version=%s creation_known=%u creation=%llu relayed=%u job_known=%u in_job=%u job_error=%lu limits_known=%u limit_flags=0x%08lx limits_error=%lu\r\n",
        utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond,
        utc.wMilliseconds, GetCurrentProcessId(), release, gotTimes ? 1u : 0u,
        creation, alreadyRelayed ? 1u : 0u, job.membershipKnown ? 1u : 0u,
        job.inJob ? 1u : 0u, job.membershipError, job.limitsKnown ? 1u : 0u,
        job.limitFlags, job.limitsError);
    if (count > 0) WriteLine(line, static_cast<DWORD>(count));
}

inline bool IsRelayArgument(const wchar_t* argument) noexcept {
    return argument && (!std::wcscmp(argument, kRelayArgument) ||
                        !std::wcscmp(argument, kFinalRelayArgument));
}

enum class RelayOutcome { NotNeeded, Skipped, Failed, Launched };
struct RelayResult {
    RelayOutcome outcome = RelayOutcome::NotNeeded;
    DWORD error = ERROR_SUCCESS;
    DWORD childPid = 0;
    bool childJobKnown = false;
    bool childInJob = false;
    DWORD childJobError = ERROR_SUCCESS;
};

template<class T> struct ComReference {
    T* value = nullptr;
    ~ComReference() noexcept { if (value) value->Release(); }
    ComReference() noexcept = default;
    ComReference(const ComReference&) = delete;
    ComReference& operator=(const ComReference&) = delete;
};

struct ComApartment {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ~ComApartment() noexcept { if (SUCCEEDED(result)) CoUninitialize(); }
};

inline HRESULT DispatchProperty(IDispatch* object, const wchar_t* name,
                                IDispatch** output) noexcept {
    LPOLESTR mutableName = const_cast<LPOLESTR>(name);
    DISPID id = 0;
    HRESULT result = object->GetIDsOfNames(IID_NULL, &mutableName, 1, LOCALE_USER_DEFAULT, &id);
    if (FAILED(result)) return result;
    DISPPARAMS parameters{};
    VARIANT value{};
    result = object->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET,
        &parameters, &value, nullptr, nullptr);
    if (SUCCEEDED(result)) {
        if (value.vt == VT_DISPATCH && value.pdispVal) {
            *output = value.pdispVal;
            value.vt = VT_EMPTY;
        } else result = E_NOINTERFACE;
    }
    VariantClear(&value);
    return result;
}

// ShellExecute takes the argument tail separately. Preserve its original UTF-16
// spelling; the caller verifies that the appended marker is a separate argv.
inline const wchar_t* ArgumentTail(const wchar_t* command) noexcept {
    bool quoted = false;
    while (*command) {
        if (*command == L'"') quoted = !quoted;
        else if (!quoted && (*command == L' ' || *command == L'\t')) break;
        ++command;
    }
    return command;
}

inline HRESULT ExplorerExecute(const wchar_t* module, const wchar_t* arguments,
                               const wchar_t* directory) noexcept {
    ComApartment apartment;
    if (FAILED(apartment.result) && apartment.result != RPC_E_CHANGED_MODE) return apartment.result;
    ComReference<IShellWindows> windows;
    HRESULT result = CoCreateInstance(__uuidof(ShellWindows), nullptr, CLSCTX_LOCAL_SERVER,
        __uuidof(IShellWindows), reinterpret_cast<void**>(&windows.value));
    if (FAILED(result)) return result;
    VARIANT location{}, root{};
    location.vt = VT_I4; location.lVal = 0;
    long window = 0;
    ComReference<IDispatch> desktop, document, application;
    result = windows.value->FindWindowSW(&location, &root, SWC_DESKTOP, &window,
        SWFO_NEEDDISPATCH, &desktop.value);
    if (result != S_OK || !desktop.value) return FAILED(result) ? result : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if (FAILED(result = DispatchProperty(desktop.value, L"Document", &document.value))) return result;
    if (FAILED(result = DispatchProperty(document.value, L"Application", &application.value))) return result;
    wchar_t method[] = L"ShellExecute"; LPOLESTR methodName = method;
    DISPID id = 0;
    result = application.value->GetIDsOfNames(IID_NULL, &methodName, 1, LOCALE_USER_DEFAULT, &id);
    if (FAILED(result)) return result;
    // Automation arguments are supplied in reverse order.
    VARIANT values[5]{};
    values[0].vt = VT_I4; values[0].lVal = SW_HIDE;
    values[1].vt = VT_BSTR; values[1].bstrVal = SysAllocString(L"open");
    values[2].vt = VT_BSTR; values[2].bstrVal = SysAllocString(directory);
    values[3].vt = VT_BSTR; values[3].bstrVal = SysAllocString(arguments);
    values[4].vt = VT_BSTR; values[4].bstrVal = SysAllocString(module);
    if (!values[1].bstrVal || !values[2].bstrVal || !values[3].bstrVal || !values[4].bstrVal)
        result = E_OUTOFMEMORY;
    else {
        DISPPARAMS parameters{ values, nullptr, 5, 0 };
        result = application.value->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD,
            &parameters, nullptr, nullptr, nullptr);
    }
    for (VARIANT& value : values) VariantClear(&value);
    return result;
}

inline bool MakeMarkedCommand(wchar_t* command, const wchar_t* marker,
                              DWORD& error) noexcept {
    const wchar_t* original = GetCommandLineW();
    const SIZE_T length = original ? std::wcslen(original) : 0;
    const SIZE_T suffix = std::wcslen(marker) + 1;
    if (!length || length + 1 + suffix > 32767) {
        error = ERROR_FILENAME_EXCED_RANGE;
        return false;
    }
    std::memcpy(command, original, length * sizeof(wchar_t));
    command[length] = L' ';
    std::memcpy(command + length + 1, marker, suffix * sizeof(wchar_t));
    int count = 0;
    wchar_t** parsed = CommandLineToArgvW(command, &count);
    if (!parsed) { error = GetLastError(); return false; }
    const bool separate = count >= 2 && !std::wcscmp(parsed[count - 1], marker);
    LocalFree(parsed);
    if (!separate) error = ERROR_BAD_ARGUMENTS;
    return separate;
}

inline bool FinalMarkerPresent(DWORD& error) noexcept {
    int count = 0;
    wchar_t** parsed = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!parsed) { error = GetLastError(); return false; }
    bool present = false;
    for (int i = 1; i < count; ++i)
        if (!std::wcscmp(parsed[i], kFinalRelayArgument)) { present = true; break; }
    LocalFree(parsed);
    return present;
}

inline void DiscardSuspendedProcess(PROCESS_INFORMATION& process) noexcept {
    const BOOL stopped = TerminateProcess(process.hProcess, ERROR_CANCELLED);
    const DWORD error = stopped ? ERROR_SUCCESS : GetLastError();
    const DWORD waited = stopped ? WaitForSingleObject(process.hProcess, 5000) : WAIT_FAILED;
    LogEvent("RELAY_FINAL_DISCARDED", process.dwProcessId, error, waited);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

inline RelayResult FinalBreakaway(const JobState& job) noexcept {
    RelayResult result; result.outcome = RelayOutcome::Failed;
    HeapPath module, command;
    if (!module.value || !command.value) result.error = ERROR_NOT_ENOUGH_MEMORY;
    else {
        // Preserve the executable's original spelling. A synthetic extended-path
        // prefix becomes observable through the child's GetModuleFileNameW.
        const DWORD moduleCount = GetModuleFileNameW(nullptr, module.value, kPathCapacity);
        if (!moduleCount || moduleCount >= kPathCapacity) {
            result.error = moduleCount ? ERROR_FILENAME_EXCED_RANGE : GetLastError();
        } else if (MakeMarkedCommand(command.value, kFinalRelayArgument, result.error)) {
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESHOWWINDOW; startup.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION process{};
        LogEvent("RELAY_FINAL_ATTEMPT", job.limitFlags);
        if (CreateProcessW(module.value, command.value, nullptr, nullptr, FALSE,
                CREATE_BREAKAWAY_FROM_JOB | CREATE_SUSPENDED, nullptr, nullptr, &startup, &process)) {
            result.childPid = process.dwProcessId;
            BOOL inJob = FALSE;
            if (IsProcessInJob(process.hProcess, nullptr, &inJob)) {
                result.childJobKnown = true;
                result.childInJob = inJob != FALSE;
            } else result.childJobError = GetLastError();
            LogEvent("RELAY_FINAL_CHECK", result.childPid,
                result.childJobKnown ? (result.childInJob ? 1u : 0u) : 2u,
                result.childJobError);
            if (!result.childJobKnown || result.childInJob) {
                result.error = result.childJobKnown ? ERROR_ACCESS_DENIED : result.childJobError;
                DiscardSuspendedProcess(process);
            } else if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
                result.error = GetLastError();
                DiscardSuspendedProcess(process);
            } else {
                result.outcome = RelayOutcome::Launched;
                LogEvent("RELAY_FINAL_STARTED", result.childPid);
                CloseHandle(process.hThread); CloseHandle(process.hProcess);
                return result;
            }
        } else result.error = GetLastError();
        }
    }
    LogEvent("RELAY_FINAL_FAILED", result.error, job.limitFlags);
    return result;
}

inline RelayResult MaybeRelay(bool skipRelay, bool alreadyRelayed) noexcept {
    RelayResult result;
    if (skipRelay) {
        result.outcome = RelayOutcome::Skipped;
        return result;
    }
    const JobState job = CurrentJob();
    // An immediate job query does not reveal all ancestor jobs. Any confirmed
    // membership uses the existing Explorer desktop as a one-time launch broker.
    if (!job.membershipKnown || !job.inJob) return result;

    if (alreadyRelayed) {
        DWORD markerError = ERROR_SUCCESS;
        const bool final = FinalMarkerPresent(markerError);
        if (markerError) {
            result.outcome = RelayOutcome::Failed; result.error = markerError;
            LogEvent("RELAY_FINAL_MARKER_FAILED", markerError);
            return result;
        }
        if (final || !job.limitsKnown || !(job.limitFlags & JOB_OBJECT_LIMIT_BREAKAWAY_OK)) {
            result.outcome = RelayOutcome::Skipped;
            return result;
        }
        // Explorer may place the first relay in its own breakaway-permitted job.
        // The final child is resumed only after an actual any-job=false check.
        return FinalBreakaway(job);
    }

    result.outcome = RelayOutcome::Failed;
    HeapPath module, command, directory;
    if (!module.value || !command.value || !directory.value) {
        result.error = ERROR_NOT_ENOUGH_MEMORY;
    } else {
        const DWORD moduleCount = GetModuleFileNameW(nullptr, module.value, kPathCapacity);
        if (!moduleCount || moduleCount >= kPathCapacity) {
            result.error = moduleCount ? ERROR_FILENAME_EXCED_RANGE : GetLastError();
            LogEvent("RELAY_FAILED", result.error, job.limitFlags);
            return result;
        }
        if (MakeMarkedCommand(command.value, kRelayArgument, result.error)) {
            std::memcpy(directory.value, module.value, (moduleCount + 1) * sizeof(wchar_t));
            wchar_t* slash = std::wcsrchr(directory.value, L'\\');
            if (!slash) {
                result.error = ERROR_BAD_PATHNAME;
                LogEvent("RELAY_FAILED", result.error, job.limitFlags);
                return result;
            }
            slash[1] = L'\0';
            LogEvent("RELAY_EXPLORER_ATTEMPT", job.limitFlags);
            const HRESULT launch = ExplorerExecute(module.value, ArgumentTail(command.value), directory.value);
            if (SUCCEEDED(launch)) {
                result.outcome = RelayOutcome::Launched;
                // Shell automation returns no process handle/PID. The new
                // process records its own real job membership in START.
                LogEvent("RELAY_EXPLORER_REQUESTED", static_cast<DWORD>(launch));
                return result;
            }
            result.error = static_cast<DWORD>(launch);
        }
    }
    LogEvent("RELAY_FAILED", result.error, job.limitFlags);
    return result;
}

} // namespace lightmon::lifetime
