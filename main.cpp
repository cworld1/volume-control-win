#include <windows.h>
#include <tlhelp32.h>

#include <mmdeviceapi.h>
#include <audiopolicy.h>

#include <cerrno>
#include <cwctype>
#include <cstdlib>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "ole32.lib")

template <typename T>
class ComPtr {
private:
    T* ptr_ = nullptr;

public:
    ComPtr() = default;

    explicit ComPtr(T* ptr)
        : ptr_(ptr) {
    }

    ~ComPtr() {
        reset();
    }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept
        : ptr_(other.ptr_) {
        other.ptr_ = nullptr;
    }

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset();

            ptr_ = other.ptr_;
            other.ptr_ = nullptr;
        }

        return *this;
    }

    T* get() const {
        return ptr_;
    }

    T** put() {
        reset();
        return &ptr_;
    }

    T* operator->() const {
        return ptr_;
    }

    explicit operator bool() const {
        return ptr_ != nullptr;
    }

    void reset() {
        if (ptr_) {
            ptr_->Release();
            ptr_ = nullptr;
        }
    }
};

bool EqualIgnoreCase(
    const wchar_t* a,
    const wchar_t* b
) {
    if (!a || !b)
        return false;

    while (*a && *b) {
        if (std::towlower(*a) != std::towlower(*b))
            return false;

        ++a;
        ++b;
    }

    return *a == *b;
}

void PrintHresult(
    const wchar_t* operation,
    HRESULT hr
) {
    std::wcerr
        << operation
        << L" failed: 0x"
        << std::hex
        << static_cast<unsigned long>(hr)
        << std::dec
        << L"\n";
}

struct ProcessInfo {
    DWORD pid;
    DWORD parentPid;
    std::wstring name;
};

std::vector<ProcessInfo> GetAllProcesses() {
    std::vector<ProcessInfo> processes;

    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPPROCESS,
        0
    );

    if (snapshot == INVALID_HANDLE_VALUE)
        return processes;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            processes.push_back({
                entry.th32ProcessID,
                entry.th32ParentProcessID,
                entry.szExeFile
            });
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return processes;
}

/*
 * Find the specified process and all of its child processes.
 */
std::unordered_set<DWORD> FindProcessTree(
    const wchar_t* processName
) {
    const std::vector<ProcessInfo> processes =
        GetAllProcesses();

    std::unordered_set<DWORD> targetPids;

    for (const auto& process : processes) {
        if (EqualIgnoreCase(
                process.name.c_str(),
                processName)) {
            targetPids.insert(process.pid);
        }
    }

    bool changed;

    do {
        changed = false;

        for (const auto& process : processes) {
            if (targetPids.count(process.pid))
                continue;

            if (targetPids.count(process.parentPid)) {
                targetPids.insert(process.pid);
                changed = true;
            }
        }
    } while (changed);

    return targetPids;
}

/*
 * Parse a volume value from 0 to 100.

 * The returned value is normalized to 0.0f-1.0f,
 * which is the range required by SetMasterVolume().
 */
bool ParseVolume(
    const wchar_t* text,
    float& volume
) {
    if (!text || !*text)
        return false;

    errno = 0;

    wchar_t* end = nullptr;

    const unsigned long value =
        std::wcstoul(text, &end, 10);

    if (errno == ERANGE ||
        end == text ||
        *end != L'\0' ||
        value > 100) {
        return false;
    }

    volume = static_cast<float>(value) / 100.0f;
    return true;
}

struct AudioSession {
    ComPtr<ISimpleAudioVolume> volume;
};

HRESULT FindMatchingSessions(
    IMMDevice* device,
    const std::unordered_set<DWORD>& targetPids,
    std::vector<AudioSession>& sessions
) {
    if (!device)
        return E_INVALIDARG;

    ComPtr<IAudioSessionManager2> manager;

    HRESULT hr = device->Activate(
        __uuidof(IAudioSessionManager2),
        CLSCTX_ALL,
        nullptr,
        reinterpret_cast<void**>(manager.put())
    );

    if (FAILED(hr))
        return hr;

    ComPtr<IAudioSessionEnumerator> enumerator;

    hr = manager->GetSessionEnumerator(
        enumerator.put()
    );

    if (FAILED(hr))
        return hr;

    int sessionCount = 0;

    hr = enumerator->GetCount(&sessionCount);

    if (FAILED(hr))
        return hr;

    for (int i = 0; i < sessionCount; ++i) {
        ComPtr<IAudioSessionControl> control;

        hr = enumerator->GetSession(
            i,
            control.put()
        );

        if (FAILED(hr) || !control)
            continue;

        ComPtr<IAudioSessionControl2> control2;

        hr = control->QueryInterface(
            __uuidof(IAudioSessionControl2),
            reinterpret_cast<void**>(control2.put())
        );

        if (FAILED(hr) || !control2)
            continue;

        DWORD processId = 0;

        hr = control2->GetProcessId(&processId);

        if (FAILED(hr) || processId == 0)
            continue;

        if (!targetPids.count(processId))
            continue;

        ComPtr<ISimpleAudioVolume> volume;

        hr = control->QueryInterface(
            __uuidof(ISimpleAudioVolume),
            reinterpret_cast<void**>(volume.put())
        );

        if (FAILED(hr) || !volume)
            continue;

        sessions.push_back({
            std::move(volume)
        });
    }

    return S_OK;
}

int wmain(
    int argc,
    wchar_t** argv
) {
    if (argc != 3) {
        std::wcerr
            << L"Usage:\n"
            << L"  VolumeControl.exe <process.exe> mute | unmute | 0-100\n";

        return 1;
    }

    const wchar_t* processName = argv[1];
    const wchar_t* command = argv[2];

    const bool mute =
        EqualIgnoreCase(command, L"mute");

    const bool unmute =
        EqualIgnoreCase(command, L"unmute");

    float targetVolume = 0.0f;

    const bool setVolume =
        ParseVolume(command, targetVolume);

    if (!mute && !unmute && !setVolume) {
        std::wcerr
            << L"Invalid command.\n"
            << L"Use mute, unmute, or a volume from 0 to 100.\n";

        return 1;
    }

    HRESULT hr = CoInitializeEx(
        nullptr,
        COINIT_MULTITHREADED
    );

    if (FAILED(hr)) {
        PrintHresult(L"CoInitializeEx", hr);
        return 2;
    }

    struct ComGuard {
        ~ComGuard() {
            CoUninitialize();
        }
    } comGuard;

    const std::unordered_set<DWORD> targetPids =
        FindProcessTree(processName);

    if (targetPids.empty()) {
        std::wcerr
            << L"Process not found: "
            << processName
            << L"\n";

        return 3;
    }

    std::wcout << L"Target process PIDs:\n";

    for (DWORD pid : targetPids) {
        std::wcout
            << L"  "
            << pid
            << L"\n";
    }

    ComPtr<IMMDeviceEnumerator> deviceEnumerator;

    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(
            deviceEnumerator.put()
        )
    );

    if (FAILED(hr)) {
        PrintHresult(
            L"Create device enumerator",
            hr
        );

        return 4;
    }

    ComPtr<IMMDeviceCollection> devices;

    /*
     * Enumerate all active render devices instead of
     * using only the default audio device.
     */
    hr = deviceEnumerator->EnumAudioEndpoints(
        eRender,
        DEVICE_STATE_ACTIVE,
        devices.put()
    );

    if (FAILED(hr)) {
        PrintHresult(
            L"Enumerate audio devices",
            hr
        );

        return 5;
    }

    UINT deviceCount = 0;

    hr = devices->GetCount(&deviceCount);

    if (FAILED(hr)) {
        PrintHresult(
            L"Get audio device count",
            hr
        );

        return 6;
    }

    std::vector<AudioSession> sessions;

    for (UINT i = 0; i < deviceCount; ++i) {
        ComPtr<IMMDevice> device;

        hr = devices->Item(
            i,
            device.put()
        );

        if (FAILED(hr) || !device)
            continue;

        hr = FindMatchingSessions(
            device.get(),
            targetPids,
            sessions
        );

        if (FAILED(hr)) {
            PrintHresult(
                L"Find audio sessions",
                hr
            );
        }
    }

    if (sessions.empty()) {
        std::wcerr
            << L"No matching audio session found.\n"
            << L"Make sure the process is currently playing audio.\n";

        return 7;
    }

    int changed = 0;

    for (auto& session : sessions) {
        if (mute) {
            hr = session.volume->SetMute(
                TRUE,
                nullptr
            );
        } else if (unmute) {
            hr = session.volume->SetMute(
                FALSE,
                nullptr
            );
        } else {
            /*
             * Set the volume and unmute the session.
             */
            hr = session.volume->SetMasterVolume(
                targetVolume,
                nullptr
            );

            if (SUCCEEDED(hr)) {
                session.volume->SetMute(
                    FALSE,
                    nullptr
                );
            }
        }

        if (SUCCEEDED(hr)) {
            ++changed;
        } else {
            PrintHresult(
                L"Set audio session state",
                hr
            );
        }
    }

    if (setVolume) {
        std::wcout
            << L"Volume set to "
            << static_cast<int>(
                targetVolume * 100.0f
            )
            << L"%";

    } else {
        std::wcout
            << L"Result: "
            << (mute ? L"muted" : L"unmuted");
    }

    std::wcout
        << L"; sessions changed: "
        << changed
        << L"/"
        << sessions.size()
        << L"\n";

    return changed > 0 ? 0 : 8;
}
