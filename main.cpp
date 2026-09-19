#include <windows.h>
#include <tlhelp32.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>

#include <cerrno>
#include <cwchar>
#include <cwctype>
#include <iostream>
#include <string>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "ole32.lib")

template<class T>
class ComPtr {
    T* p_ = nullptr;

public:
    ComPtr() = default;
    explicit ComPtr(T* p) : p_(p) {}
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept : p_(other.p_) {
        other.p_ = nullptr;
    }

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset();
            p_ = other.p_;
            other.p_ = nullptr;
        }
        return *this;
    }

    T* get() const { return p_; }
    T** put() {
        reset();
        return &p_;
    }

    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

    void reset() {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }
};

bool EqualIgnoreCase(const wchar_t* a, const wchar_t* b) {
    if (!a || !b)
        return false;

    while (*a && *b) {
        if (std::towlower(*a++) != std::towlower(*b++))
            return false;
    }

    return *a == *b;
}

void PrintError(const wchar_t* action, HRESULT hr) {
    std::wcerr << action
               << L" failed: 0x"
               << std::hex
               << static_cast<unsigned long>(hr)
               << std::dec
               << L'\n';
}

struct ProcessInfo {
    DWORD pid;
    DWORD parentPid;
    std::wstring name;
};

std::vector<ProcessInfo> GetProcesses() {
    std::vector<ProcessInfo> result;

    HANDLE snapshot = CreateToolhelp32Snapshot(
        TH32CS_SNAPPROCESS,
        0
    );

    if (snapshot == INVALID_HANDLE_VALUE)
        return result;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Process32FirstW(snapshot, &entry)) {
        do {
            result.push_back({
                entry.th32ProcessID,
                entry.th32ParentProcessID,
                entry.szExeFile
            });
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return result;
}

std::unordered_set<DWORD> FindProcessTree(
    const wchar_t* processName
) {
    const auto processes = GetProcesses();
    std::unordered_set<DWORD> pids;

    // Find the requested process.
    for (const auto& process : processes) {
        if (EqualIgnoreCase(process.name.c_str(), processName))
            pids.insert(process.pid);
    }

    // Add all descendants.
    bool changed;

    do {
        changed = false;

        for (const auto& process : processes) {
            if (!pids.count(process.pid) &&
                pids.count(process.parentPid)) {
                pids.insert(process.pid);
                changed = true;
            }
        }
    } while (changed);

    return pids;
}

bool ParseVolume(const wchar_t* text, float& volume) {
    if (!text || !*text)
        return false;

    errno = 0;

    wchar_t* end = nullptr;
    unsigned long value = std::wcstoul(text, &end, 10);

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
    BOOL muted;
};

HRESULT GetMatchingSessions(
    IMMDevice* device,
    const std::unordered_set<DWORD>& pids,
    std::vector<AudioSession>& result
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

    if (FAILED(hr = manager->GetSessionEnumerator(
        enumerator.put()
    ))) {
        return hr;
    }

    int count = 0;

    if (FAILED(hr = enumerator->GetCount(&count)))
        return hr;

    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> control;

        if (FAILED(enumerator->GetSession(i, control.put())))
            continue;

        ComPtr<IAudioSessionControl2> control2;

        if (FAILED(control->QueryInterface(
            __uuidof(IAudioSessionControl2),
            reinterpret_cast<void**>(control2.put())
        ))) {
            continue;
        }

        DWORD pid = 0;

        if (FAILED(control2->GetProcessId(&pid)) ||
            !pids.count(pid)) {
            continue;
        }

        ComPtr<ISimpleAudioVolume> volume;

        if (FAILED(control->QueryInterface(
            __uuidof(ISimpleAudioVolume),
            reinterpret_cast<void**>(volume.put())
        ))) {
            continue;
        }

        BOOL muted = FALSE;

        if (FAILED(volume->GetMute(&muted)))
            continue;

        result.push_back({
            std::move(volume),
            muted
        });
    }

    return S_OK;
}

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 3) {
        std::wcerr
            << L"Usage:\n"
            << L"  VolumeControl.exe <process.exe> mute\n"
            << L"  VolumeControl.exe <process.exe> unmute\n"
            << L"  VolumeControl.exe <process.exe> toggle\n"
            << L"  VolumeControl.exe <process.exe> 0-100\n";

        return 1;
    }

    const wchar_t* processName = argv[1];
    const wchar_t* command = argv[2];

    const bool mute = EqualIgnoreCase(command, L"mute");
    const bool unmute = EqualIgnoreCase(command, L"unmute");
    const bool toggle = EqualIgnoreCase(command, L"toggle");

    float volumeValue = 0.0f;
    const bool setVolume = ParseVolume(command, volumeValue);

    if (!mute && !unmute && !toggle && !setVolume) {
        std::wcerr
            << L"Invalid command. Use mute, unmute, toggle, or 0-100.\n";
        return 1;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    if (FAILED(hr)) {
        PrintError(L"CoInitializeEx", hr);
        return 2;
    }

    struct ComGuard {
        ~ComGuard() {
            CoUninitialize();
        }
    } comGuard;

    const auto pids = FindProcessTree(processName);

    if (pids.empty()) {
        std::wcerr
            << L"Process not found: "
            << processName
            << L'\n';

        return 3;
    }

    ComPtr<IMMDeviceEnumerator> deviceEnumerator;

    hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator),
        nullptr,
        CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator),
        reinterpret_cast<void**>(deviceEnumerator.put())
    );

    if (FAILED(hr)) {
        PrintError(L"Create device enumerator", hr);
        return 4;
    }

    ComPtr<IMMDeviceCollection> devices;

    hr = deviceEnumerator->EnumAudioEndpoints(
        eRender,
        DEVICE_STATE_ACTIVE,
        devices.put()
    );

    if (FAILED(hr)) {
        PrintError(L"Enumerate audio devices", hr);
        return 5;
    }

    UINT deviceCount = 0;

    if (FAILED(hr = devices->GetCount(&deviceCount))) {
        PrintError(L"Get audio device count", hr);
        return 6;
    }

    std::vector<AudioSession> sessions;

    for (UINT i = 0; i < deviceCount; ++i) {
        ComPtr<IMMDevice> device;

        if (FAILED(devices->Item(i, device.put())))
            continue;

        hr = GetMatchingSessions(
            device.get(),
            pids,
            sessions
        );

        if (FAILED(hr))
            PrintError(L"Find audio sessions", hr);
    }

    if (sessions.empty()) {
        std::wcerr
            << L"No matching audio session found.\n"
            << L"Make sure the process is currently playing audio.\n";

        return 7;
    }

    bool targetMute = false;

    if (mute) {
        targetMute = true;
    } else if (unmute) {
        targetMute = false;
    } else if (toggle) {
        // Toggle all sessions together.
        targetMute = false;

        for (const auto& session : sessions) {
            if (!session.muted) {
                targetMute = true;
                break;
            }
        }
    }

    int changed = 0;

    for (auto& session : sessions) {
        if (setVolume) {
            // SetMasterVolume uses a range from 0.0 to 1.0.
            hr = session.volume->SetMasterVolume(
                volumeValue,
                nullptr
            );

            // Numeric volume commands also unmute the session.
            if (SUCCEEDED(hr)) {
                hr = session.volume->SetMute(
                    FALSE,
                    nullptr
                );
            }
        } else {
            hr = session.volume->SetMute(
                targetMute ? TRUE : FALSE,
                nullptr
            );
        }

        if (SUCCEEDED(hr)) {
            ++changed;
        } else {
            PrintError(L"Set audio session state", hr);
        }
    }

    if (setVolume) {
        std::wcout
            << L"Volume set to "
            << static_cast<int>(volumeValue * 100.0f)
            << L"%";
    } else {
        std::wcout
            << L"Result: "
            << (targetMute ? L"muted" : L"unmuted");
    }

    std::wcout
        << L"; sessions changed: "
        << changed
        << L'/'
        << sessions.size()
        << L'\n';

    return changed > 0 ? 0 : 8;
}
