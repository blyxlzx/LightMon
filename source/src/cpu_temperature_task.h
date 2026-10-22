#pragma once
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <taskschd.h>
#include <msxml6.h>
#include <sddl.h>
#include <array>
#include <cwchar>
#include <cstdint>

namespace lightmon {
inline constexpr wchar_t kCpuAutomaticHelperPath[] = L"C:\\Program Files\\LightMon\\LightMonCpu.exe";
inline constexpr wchar_t kCpuAutomaticHelperDirectory[] = L"C:\\Program Files\\LightMon";
inline constexpr wchar_t kCpuAutomaticTaskArguments[] = L"--parent $(Arg0) --parent-start $(Arg1) --mapping \"$(Arg2)\"";

template<class T> class CpuTaskComPtr {
    T* value_ = nullptr;
public:
    CpuTaskComPtr() noexcept = default;
    CpuTaskComPtr(const CpuTaskComPtr&) = delete;
    CpuTaskComPtr& operator=(const CpuTaskComPtr&) = delete;
    ~CpuTaskComPtr() { if (value_) value_->Release(); }
    T** Put() noexcept { return &value_; }
    T* operator->() const noexcept { return value_; }
    T* Get() const noexcept { return value_; }
};
class CpuTaskBstr {
    BSTR value_ = nullptr;
public:
    explicit CpuTaskBstr(const wchar_t* value = nullptr) noexcept : value_(value ? SysAllocString(value) : nullptr) {}
    CpuTaskBstr(const CpuTaskBstr&) = delete;
    CpuTaskBstr& operator=(const CpuTaskBstr&) = delete;
    ~CpuTaskBstr() { SysFreeString(value_); }
    BSTR* Put() noexcept { return &value_; }
    BSTR Get() const noexcept { return value_; }
};

inline HRESULT CpuTaskCurrentUserSid(wchar_t* value, size_t capacity) noexcept {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return HRESULT_FROM_WIN32(GetLastError());
    std::array<BYTE, 512> bytes{}; DWORD length = 0;
    const BOOL read = GetTokenInformation(token, TokenUser, bytes.data(), static_cast<DWORD>(bytes.size()), &length);
    const DWORD error = read ? ERROR_SUCCESS : GetLastError(); CloseHandle(token);
    if (!read) return HRESULT_FROM_WIN32(error);
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid))
        return HRESULT_FROM_WIN32(GetLastError());
    const auto copied = wcscpy_s(value, capacity, sid); LocalFree(sid);
    return copied == 0 ? S_OK : E_INVALIDARG;
}

inline bool CpuTaskPrincipalIsCurrentUser(BSTR account, bool& unresolved) noexcept {
    unresolved = false;
    if (!account || !*account || SysStringLen(account) != std::wcslen(account)) return false;
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    std::array<BYTE, 512> tokenData{}; DWORD tokenLength = 0;
    const BOOL read = GetTokenInformation(token, TokenUser, tokenData.data(),
                                          static_cast<DWORD>(tokenData.size()), &tokenLength);
    CloseHandle(token);
    if (!read) return false;
    PSID currentSid = reinterpret_cast<TOKEN_USER*>(tokenData.data())->User.Sid;
    PSID parsedSid = nullptr;
    if (ConvertStringSidToSidW(account, &parsedSid)) {
        const bool same = IsValidSid(parsedSid) && EqualSid(parsedSid, currentSid);
        LocalFree(parsedSid); return same;
    }
    // Task Scheduler can return a normalized account name even when the task
    // XML uses a SID. Resolve that name and compare identities, never names.
    std::array<BYTE, SECURITY_MAX_SID_SIZE> accountSid{};
    std::array<wchar_t, 256> domain{};
    DWORD sidLength = static_cast<DWORD>(accountSid.size());
    DWORD domainLength = static_cast<DWORD>(domain.size()); SID_NAME_USE use{};
    if (!LookupAccountNameW(nullptr, account, accountSid.data(), &sidLength,
                            domain.data(), &domainLength, &use)) {
        unresolved = GetLastError() == ERROR_NONE_MAPPED;
        return false;
    }
    // A bare username can collide with the computer/domain name. Such a result
    // identifies no user; require the strict SID from the protected task XML.
    if (use == SidTypeDomain && !std::wcschr(account, L'\\') && !std::wcschr(account, L'@')) {
        unresolved = true;
        return false;
    }
    return use == SidTypeUser && IsValidSid(accountSid.data()) && EqualSid(accountSid.data(), currentSid);
}

// The service may normalize a SID to an unresolvable short account name in
// IPrincipal::UserId. Its protected XML retains the original SID. Require that
// authoritative SID as well, using a namespace-aware parser with DTDs disabled.
inline HRESULT CpuTaskVerifyXmlPrincipal(IRegisteredTask* task, const wchar_t* currentSid) noexcept {
    CpuTaskBstr xml;
    HRESULT hr = task->get_Xml(xml.Put());
    if (FAILED(hr)) return hr;
    if (!xml.Get() || !*xml.Get() || SysStringLen(xml.Get()) > 1024 * 1024 ||
        SysStringLen(xml.Get()) != std::wcslen(xml.Get())) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    CpuTaskComPtr<IXMLDOMDocument2> document;
    hr = CoCreateInstance(__uuidof(DOMDocument60), nullptr, CLSCTX_INPROC_SERVER,
                          __uuidof(IXMLDOMDocument2), reinterpret_cast<void**>(document.Put()));
    if (FAILED(hr)) return hr;
    hr = document->put_async(VARIANT_FALSE);
    if (FAILED(hr)) return hr;
    hr = document->put_validateOnParse(VARIANT_FALSE);
    if (FAILED(hr)) return hr;
    hr = document->put_resolveExternals(VARIANT_FALSE);
    if (FAILED(hr)) return hr;
    CpuTaskBstr prohibitDtd(L"ProhibitDTD"), languageName(L"SelectionLanguage"), languageValue(L"XPath"),
        namespaceName(L"SelectionNamespaces"),
        namespaceValue(L"xmlns:t='http://schemas.microsoft.com/windows/2004/02/mit/task'"),
        principalQuery(L"/t:Task/t:Principals/t:Principal"), userQuery(L"t:UserId");
    if (!prohibitDtd.Get() || !languageName.Get() || !languageValue.Get() || !namespaceName.Get() ||
        !namespaceValue.Get() || !principalQuery.Get() || !userQuery.Get()) return E_OUTOFMEMORY;
    VARIANT property{}; property.vt = VT_BOOL; property.boolVal = VARIANT_TRUE;
    hr = document->setProperty(prohibitDtd.Get(), property);
    if (FAILED(hr)) return hr;
    property.vt = VT_BSTR; property.bstrVal = languageValue.Get();
    hr = document->setProperty(languageName.Get(), property);
    if (FAILED(hr)) return hr;
    property.bstrVal = namespaceValue.Get();
    hr = document->setProperty(namespaceName.Get(), property);
    if (FAILED(hr)) return hr;
    VARIANT_BOOL parsed = VARIANT_FALSE;
    hr = document->loadXML(xml.Get(), &parsed);
    if (FAILED(hr)) return hr;
    if (parsed != VARIANT_TRUE) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    CpuTaskComPtr<IXMLDOMNodeList> principals;
    hr = document->selectNodes(principalQuery.Get(), principals.Put());
    if (FAILED(hr)) return hr;
    LONG count = 0;
    hr = principals->get_length(&count);
    if (FAILED(hr)) return hr;
    if (count != 1) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    CpuTaskComPtr<IXMLDOMNode> principal;
    hr = principals->get_item(0, principal.Put());
    if (FAILED(hr)) return hr;
    CpuTaskComPtr<IXMLDOMNodeList> users;
    hr = principal->selectNodes(userQuery.Get(), users.Put());
    if (FAILED(hr)) return hr;
    hr = users->get_length(&count);
    if (FAILED(hr)) return hr;
    if (count != 1) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    CpuTaskComPtr<IXMLDOMNode> user;
    hr = users->get_item(0, user.Put());
    if (FAILED(hr)) return hr;
    CpuTaskBstr xmlUser;
    hr = user->get_text(xmlUser.Put());
    if (FAILED(hr)) return hr;
    if (!xmlUser.Get() || SysStringLen(xmlUser.Get()) != std::wcslen(xmlUser.Get()))
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    PSID xmlSid = nullptr, expectedSid = nullptr;
    if (!ConvertStringSidToSidW(xmlUser.Get(), &xmlSid)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    const bool validExpected = ConvertStringSidToSidW(currentSid, &expectedSid) != FALSE;
    const bool same = validExpected && IsValidSid(xmlSid) && IsValidSid(expectedSid) && EqualSid(xmlSid, expectedSid);
    if (expectedSid) LocalFree(expectedSid);
    LocalFree(xmlSid);
    return same ? S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
}

// This uses only an already-provisioned, administrator-protected task. It never
// registers a task, changes its security descriptor, or falls back to runas.
inline HRESULT RunAutomaticCpuTemperatureTask(DWORD parentPid, std::uint64_t parentStart,
                                               const wchar_t* mappingName) noexcept {
    struct Apartment {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ~Apartment() { if (SUCCEEDED(hr)) CoUninitialize(); }
    } apartment;
    if (FAILED(apartment.hr) && apartment.hr != RPC_E_CHANGED_MODE) return apartment.hr;
    wchar_t sid[256]{}, name[320]{};
    HRESULT hr = CpuTaskCurrentUserSid(sid, std::size(sid));
    if (FAILED(hr)) return hr;
    if (swprintf_s(name, L"LightMon CPU Temperature %s", sid) < 0) return E_INVALIDARG;
    CpuTaskComPtr<ITaskService> service;
    hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_ITaskService,
                          reinterpret_cast<void**>(service.Put()));
    if (FAILED(hr)) return hr;
    VARIANT empty{}; VariantInit(&empty);
    hr = service->Connect(empty, empty, empty, empty);
    if (FAILED(hr)) return hr;
    CpuTaskBstr rootName(L"\\"), taskName(name);
    if (!rootName.Get() || !taskName.Get()) return E_OUTOFMEMORY;
    CpuTaskComPtr<ITaskFolder> folder;
    hr = service->GetFolder(rootName.Get(), folder.Put());
    if (FAILED(hr)) return hr;
    CpuTaskComPtr<IRegisteredTask> task;
    hr = folder->GetTask(taskName.Get(), task.Put());
    if (FAILED(hr)) return hr;
    VARIANT_BOOL enabled = VARIANT_FALSE;
    hr = task->get_Enabled(&enabled);
    if (FAILED(hr)) return hr;
    if (enabled != VARIANT_TRUE) return HRESULT_FROM_WIN32(ERROR_SERVICE_DISABLED);
    CpuTaskComPtr<ITaskDefinition> definition;
    hr = task->get_Definition(definition.Put());
    if (FAILED(hr)) return hr;
    CpuTaskComPtr<IPrincipal> principal;
    hr = definition->get_Principal(principal.Put());
    if (FAILED(hr)) return hr;
    CpuTaskBstr principalUser;
    hr = principal->get_UserId(principalUser.Put());
    if (FAILED(hr)) return hr;
    TASK_RUNLEVEL_TYPE runLevel{}; TASK_LOGON_TYPE logonType{};
    hr = principal->get_RunLevel(&runLevel);
    if (FAILED(hr)) return hr;
    hr = principal->get_LogonType(&logonType);
    if (FAILED(hr)) return hr;
    bool unresolvedPrincipal = false;
    const bool samePrincipal = CpuTaskPrincipalIsCurrentUser(principalUser.Get(), unresolvedPrincipal);
    if ((!samePrincipal && !unresolvedPrincipal) ||
        runLevel != TASK_RUNLEVEL_HIGHEST || logonType != TASK_LOGON_INTERACTIVE_TOKEN)
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    hr = CpuTaskVerifyXmlPrincipal(task.Get(), sid);
    if (FAILED(hr)) return hr;
    CpuTaskComPtr<ITaskSettings> settings;
    hr = definition->get_Settings(settings.Put());
    if (FAILED(hr)) return hr;
    VARIANT_BOOL demand = VARIANT_FALSE;
    hr = settings->get_AllowDemandStart(&demand);
    if (FAILED(hr)) return hr;
    hr = settings->get_Enabled(&enabled);
    if (FAILED(hr)) return hr;
    if (demand != VARIANT_TRUE || enabled != VARIANT_TRUE) return HRESULT_FROM_WIN32(ERROR_SERVICE_DISABLED);
    CpuTaskComPtr<IActionCollection> actions;
    hr = definition->get_Actions(actions.Put());
    if (FAILED(hr)) return hr;
    LONG actionCount = 0;
    hr = actions->get_Count(&actionCount);
    if (FAILED(hr)) return hr;
    if (actionCount != 1) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    CpuTaskComPtr<IAction> action;
    hr = actions->get_Item(1, action.Put());
    if (FAILED(hr)) return hr;
    TASK_ACTION_TYPE actionType{};
    hr = action->get_Type(&actionType);
    if (FAILED(hr)) return hr;
    if (actionType != TASK_ACTION_EXEC) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    CpuTaskComPtr<IExecAction> exec;
    hr = action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(exec.Put()));
    if (FAILED(hr)) return hr;
    CpuTaskBstr path, arguments, directory;
    hr = exec->get_Path(path.Put());
    if (FAILED(hr)) return hr;
    hr = exec->get_Arguments(arguments.Put());
    if (FAILED(hr)) return hr;
    hr = exec->get_WorkingDirectory(directory.Put());
    if (FAILED(hr)) return hr;
    if (!path.Get() || _wcsicmp(path.Get(), kCpuAutomaticHelperPath) != 0 ||
        !arguments.Get() || std::wcscmp(arguments.Get(), kCpuAutomaticTaskArguments) != 0 ||
        !directory.Get() || _wcsicmp(directory.Get(), kCpuAutomaticHelperDirectory) != 0)
        return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    DWORD session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &session)) return HRESULT_FROM_WIN32(GetLastError());
    if (session == 0 || session > MAXLONG) return E_INVALIDARG;
    wchar_t pid[32]{}, start[32]{};
    swprintf_s(pid, L"%lu", parentPid); swprintf_s(start, L"%llu", parentStart);
    CpuTaskBstr values[] = {CpuTaskBstr(pid), CpuTaskBstr(start), CpuTaskBstr(mappingName)};
    struct Parameters {
        VARIANT value{};
        Parameters() { VariantInit(&value); }
        ~Parameters() { VariantClear(&value); }
    } parameters;
    parameters.value.vt = VT_ARRAY | VT_BSTR;
    parameters.value.parray = SafeArrayCreateVector(VT_BSTR, 0, 3);
    if (!parameters.value.parray) return E_OUTOFMEMORY;
    for (LONG index = 0; index < 3; ++index) {
        if (!values[index].Get()) return E_OUTOFMEMORY;
        hr = SafeArrayPutElement(parameters.value.parray, &index, values[index].Get());
        if (FAILED(hr)) return hr;
    }
    CpuTaskComPtr<IRunningTask> running;
    hr = task->RunEx(parameters.value, TASK_RUN_USE_SESSION_ID, static_cast<LONG>(session), nullptr, running.Put());
    if (FAILED(hr)) return hr;
    return running.Get() ? S_OK : E_FAIL;
}
} // namespace lightmon
