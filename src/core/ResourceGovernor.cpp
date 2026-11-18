#include "core/ResourceGovernor.hpp"

#include "core/Log.hpp"

#include <pdhmsg.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>

namespace companion {
namespace {

constexpr wchar_t kMsgWindowClass[] = L"CompanionAI.Governor";

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

ULONGLONG ToU64(const FILETIME& ft) noexcept {
    return (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

}  // namespace

std::atomic<ResourceGovernor*> ResourceGovernor::s_instance{nullptr};

const char* ToString(SystemMode m) noexcept {
    switch (m) {
        case SystemMode::Interactive: return "Interactive";
        case SystemMode::Gaming: return "Gaming";
        case SystemMode::Studio: return "Studio";
        case SystemMode::Idle: return "Idle";
    }
    return "?";
}

const char* ToString(InterruptReason r) noexcept {
    switch (r) {
        case InterruptReason::UserInput: return "UserInput";
        case InterruptReason::GameLaunched: return "GameLaunched";
        case InterruptReason::DawLaunched: return "DawLaunched";
        case InterruptReason::VoicePause: return "VoicePause";
        case InterruptReason::Shutdown: return "Shutdown";
    }
    return "?";
}

ResourceGovernor::ResourceGovernor(GovernorConfig cfg) : cfg_(std::move(cfg)) {
    for (const auto& g : cfg_.gameProcesses) games_.insert(ToLower(g));
    for (const auto& d : cfg_.dawProcesses) daws_.insert(ToLower(d));
}

ResourceGovernor::~ResourceGovernor() { Stop(); }

void ResourceGovernor::SetModeListener(ModeListener l) {
    std::scoped_lock lock(mutex_);
    modeListener_ = std::move(l);
}

void ResourceGovernor::SetInterruptListener(InterruptListener l) {
    std::scoped_lock lock(mutex_);
    interruptListener_ = std::move(l);
}

bool ResourceGovernor::Start() {
    if (thread_.joinable()) return true;
    ResourceGovernor* expected = nullptr;
    if (!s_instance.compare_exchange_strong(expected, this)) {
        log::Info("ResourceGovernor: une seule instance autorisée");
        return false;
    }
    wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset
    if (!wakeEvent_) return false;
    thread_ = std::jthread([this](std::stop_token st) { ThreadMain(st); });
    return true;
}

void ResourceGovernor::Stop() {
    if (!thread_.joinable()) return;
    Interrupt(InterruptReason::Shutdown);
    thread_.request_stop();
    SetEvent(wakeEvent_);
    thread_.join();
    CloseHandle(wakeEvent_);
    wakeEvent_ = nullptr;
    s_instance.store(nullptr);
}

SystemSnapshot ResourceGovernor::Snapshot() const {
    SystemSnapshot s;
    s.mode = Mode();
    s.cpuLoad = cpuLoad_.load(std::memory_order_relaxed);
    s.gpuLoad = gpuLoad_.load(std::memory_order_relaxed);
    s.idleFor = std::chrono::milliseconds{idleMs_.load(std::memory_order_relaxed)};
    s.backgroundWorkActive = backgroundActive_.load(std::memory_order_relaxed);
    return s;
}

bool ResourceGovernor::CanStartAutoTraining() const {
    const auto s = Snapshot();
    return s.mode == SystemMode::Idle && s.cpuLoad < cfg_.maxCpuLoadForTraining && s.gpuLoad >= 0.f &&
           s.gpuLoad < cfg_.maxGpuLoadForTraining;
}

std::optional<std::stop_token> ResourceGovernor::BeginBackgroundWork(bool manualRequest) {
    const SystemMode m = Mode();
    if (m == SystemMode::Gaming || m == SystemMode::Studio) return std::nullopt;
    if (!manualRequest && !CanStartAutoTraining()) return std::nullopt;

    std::stop_token token;
    {
        std::scoped_lock lock(mutex_);
        if (backgroundActive_.load()) return std::nullopt;  // un seul bail à la fois
        stopSource_ = std::stop_source{};
        token = stopSource_.get_token();
        backgroundActive_.store(true, std::memory_order_release);
    }
    SetEvent(wakeEvent_);  // le thread governor enregistre Raw Input
    log::Info("Governor: bail de fond accordé ({})", manualRequest ? "manuel" : "auto");
    return token;
}

void ResourceGovernor::EndBackgroundWork() {
    {
        std::scoped_lock lock(mutex_);
        backgroundActive_.store(false, std::memory_order_release);
        stopSource_ = std::stop_source{std::nostopstate};
    }
    if (wakeEvent_) SetEvent(wakeEvent_);
}

void ResourceGovernor::Interrupt(InterruptReason reason) {
    InterruptListener listener;
    {
        std::scoped_lock lock(mutex_);
        if (!backgroundActive_.exchange(false, std::memory_order_acq_rel)) return;
        if (stopSource_.stop_possible()) stopSource_.request_stop();  // signal immédiat aux workers
        listener = interruptListener_;
    }
    log::Info("Governor: interruption ({})", ToString(reason));
    if (listener) listener(reason);
    // Désenregistrement Raw Input : immédiat si on est sur le thread governor, sinon au réveil.
    if (wakeEvent_) SetEvent(wakeEvent_);
}

void ResourceGovernor::EnterBackgroundPriority() noexcept {
    // THREAD_MODE_BACKGROUND_BEGIN abaisse aussi la priorité E/S et mémoire (pages "low priority").
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
}

void ResourceGovernor::LeaveBackgroundPriority() noexcept {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
}

// ---------------------------------------------------------------------------
// Thread governor
// ---------------------------------------------------------------------------
void ResourceGovernor::ThreadMain(std::stop_token st) {
    // Thread quasi toujours endormi : priorité haute = réaction rapide, coût nul.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    log::SetCurrentThreadName(L"Companion.Governor");

    HINSTANCE hinst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &ResourceGovernor::WndProc;
    wc.hInstance = hinst;
    wc.lpszClassName = kMsgWindowClass;
    RegisterClassExW(&wc);
    msgWindow_ = CreateWindowExW(0, kMsgWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hinst, this);

    // Changement de fenêtre au premier plan : événementiel, déclenche un scan de processus.
    foregroundHook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                      &ResourceGovernor::WinEventProc, 0, 0,
                                      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

    SampleCpuLoad();  // amorce les deltas
    Tick(true);

    while (!st.stop_requested()) {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTick_);
        const DWORD timeout =
            static_cast<DWORD>(std::max<std::int64_t>(0, (cfg_.pollInterval - elapsed).count()));

        MsgWaitForMultipleObjectsEx(1, &wakeEvent_, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (st.stop_requested()) break;

        SyncRawInputRegistration();

        const bool fgChanged = foregroundChanged_.exchange(false);
        if (fgChanged || std::chrono::steady_clock::now() - lastTick_ >= cfg_.pollInterval) Tick(fgChanged);
    }

    backgroundActive_.store(false);
    SyncRawInputRegistration();
    if (foregroundHook_) UnhookWinEvent(foregroundHook_);
    if (msgWindow_) DestroyWindow(msgWindow_);
    UnregisterClassW(kMsgWindowClass, hinst);
    if (pdhQuery_) PdhCloseQuery(pdhQuery_);
    foregroundHook_ = nullptr;
    msgWindow_ = nullptr;
    pdhQuery_ = nullptr;
}

void ResourceGovernor::Tick(bool forceProcessScan) {
    const auto now = std::chrono::steady_clock::now();
    lastTick_ = now;

    const auto idle = IdleDuration();
    idleMs_.store(idle.count(), std::memory_order_relaxed);
    cpuLoad_.store(SampleCpuLoad(), std::memory_order_relaxed);

    if (forceProcessScan || now - lastScan_ >= cfg_.processScanInterval) {
        ScanProcesses();
        lastScan_ = now;
    }

    const bool fullscreen = cfg_.treatFullscreenAsGame && IsFullscreenAppRunning();
    SystemMode m = SystemMode::Interactive;
    if (gameRunning_ || fullscreen) m = SystemMode::Gaming;
    else if (dawRunning_) m = SystemMode::Studio;
    else if (idle >= cfg_.idleThreshold) m = SystemMode::Idle;

    // Le compteur GPU PDH coûte quelques ms : on ne l'interroge que lorsqu'il sert. En jeu, toutes les 2 s :
    // il dit si le jeu sature la carte graphique (cerveau sur le processeur) ou le processeur (cerveau sur
    // la carte, en priorité basse). Moyenne glissante (~8 s) pour ignorer les pics d'un chargement.
    if (m == SystemMode::Idle) {
        gpuLoad_.store(SampleGpuLoad(), std::memory_order_relaxed);
    } else if (m == SystemMode::Gaming) {
        if (mode_.load(std::memory_order_relaxed) != SystemMode::Gaming) gpuEma_ = -1.f;
        if (++gpuTick_ % 2 == 0 || gpuEma_ < 0.f) {
            const float g = SampleGpuLoad();
            if (g >= 0.f) gpuEma_ = gpuEma_ < 0.f ? g : gpuEma_ + 0.25f * (g - gpuEma_);
        }
        gpuLoad_.store(gpuEma_, std::memory_order_relaxed);
    } else {
        gpuLoad_.store(-1.f, std::memory_order_relaxed);
    }

    SetMode(m);

    if (backgroundActive_.load(std::memory_order_acquire)) {
        if (m == SystemMode::Gaming) Interrupt(InterruptReason::GameLaunched);
        else if (m == SystemMode::Studio) Interrupt(InterruptReason::DawLaunched);
    }
}

void ResourceGovernor::SetMode(SystemMode m) {
    const SystemMode prev = mode_.exchange(m, std::memory_order_acq_rel);
    if (prev == m) return;
    log::Info("Governor: mode {} -> {}", ToString(prev), ToString(m));
    ModeListener listener;
    {
        std::scoped_lock lock(mutex_);
        listener = modeListener_;
    }
    if (listener) listener(prev, m);
}

void ResourceGovernor::SyncRawInputRegistration() {
    const bool want = backgroundActive_.load(std::memory_order_acquire) && msgWindow_ != nullptr;
    if (want == rawInputRegistered_) return;

    RAWINPUTDEVICE rid[2]{};
    rid[0].usUsagePage = 0x01; rid[0].usUsage = 0x02;  // souris
    rid[1].usUsagePage = 0x01; rid[1].usUsage = 0x06;  // clavier
    for (auto& r : rid) {
        r.dwFlags = want ? RIDEV_INPUTSINK : RIDEV_REMOVE;
        r.hwndTarget = want ? msgWindow_ : nullptr;
    }
    if (RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
        rawInputRegistered_ = want;
    } else {
        log::Info("Governor: RegisterRawInputDevices a échoué ({})", GetLastError());
        if (!want) rawInputRegistered_ = false;
    }
}

LRESULT CALLBACK ResourceGovernor::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    }
    auto* self = reinterpret_cast<ResourceGovernor*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_INPUT && self) {
        // Toute saisie pendant un bail = arrêt d'urgence. Latence : le temps de livrer un message.
        self->Interrupt(InterruptReason::UserInput);
        self->SyncRawInputRegistration();
    }
    return DefWindowProcW(hwnd, msg, wp, lp);  // obligatoire pour WM_INPUT
}

void CALLBACK ResourceGovernor::WinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    if (auto* self = s_instance.load(std::memory_order_acquire)) self->foregroundChanged_.store(true);
}

std::wstring ResourceGovernor::ActiveGame() const {
    std::scoped_lock lock(mutex_);
    return activeGame_;
}

void ResourceGovernor::ScanProcesses() {
    bool game = false, daw = false;
    std::wstring gameName;
    DWORD gamePid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    std::wstring name;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !(game && daw); ok = Process32NextW(snap, &pe)) {
        name = ToLower(pe.szExeFile);
        if (!game && games_.contains(name)) {
            game = true;
            gameName = name;
            gamePid = pe.th32ProcessID;
        }
        if (!daw && daws_.contains(name)) daw = true;
    }
    CloseHandle(snap);
    gameRunning_ = game;
    dawRunning_ = daw;
    std::scoped_lock lock(mutex_);
    activeGame_ = gameName;
    activeGamePid_ = gamePid;
}

DWORD ResourceGovernor::ActiveGamePid(std::wstring* gameName) const {
    {
        std::scoped_lock lock(mutex_);
        if (activeGamePid_) {
            if (gameName) *gameName = activeGame_;
            return activeGamePid_;
        }
    }
    if (!IsGaming()) return 0;
    DWORD pid = 0;
    if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &pid);
    if (!pid || pid == GetCurrentProcessId()) return 0;
    if (gameName) {
        gameName->clear();
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t path[MAX_PATH];
            DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, path, &n)) {
                std::wstring_view full(path, n);
                *gameName = ToLower(std::wstring(full.substr(full.find_last_of(L"\\/") + 1)));
            }
            CloseHandle(h);
        }
    }
    return pid;
}

bool ResourceGovernor::IsFullscreenAppRunning() noexcept {
    QUERY_USER_NOTIFICATION_STATE state{};
    if (FAILED(SHQueryUserNotificationState(&state))) return false;
    return state == QUNS_RUNNING_D3D_FULL_SCREEN || state == QUNS_BUSY || state == QUNS_PRESENTATION_MODE;
}

std::chrono::milliseconds ResourceGovernor::IdleDuration() noexcept {
    LASTINPUTINFO lii{};
    lii.cbSize = sizeof(lii);
    if (!GetLastInputInfo(&lii)) return std::chrono::milliseconds{0};
    return std::chrono::milliseconds{static_cast<DWORD>(GetTickCount() - lii.dwTime)};  // wrap-safe
}

float ResourceGovernor::SampleCpuLoad() {
    FILETIME idleFt, kernelFt, userFt;
    if (!GetSystemTimes(&idleFt, &kernelFt, &userFt)) return cpuLoad_.load();
    const ULONGLONG idle = ToU64(idleFt), kernel = ToU64(kernelFt), user = ToU64(userFt);
    const ULONGLONG dIdle = idle - prevIdle_, dTotal = (kernel - prevKernel_) + (user - prevUser_);
    prevIdle_ = idle; prevKernel_ = kernel; prevUser_ = user;
    if (dTotal == 0) return cpuLoad_.load();
    return std::clamp(1.f - static_cast<float>(dIdle) / static_cast<float>(dTotal), 0.f, 1.f);
}

float ResourceGovernor::SampleGpuLoad() {
    if (!pdhQuery_) {
        if (PdhOpenQueryW(nullptr, 0, &pdhQuery_) != ERROR_SUCCESS) { pdhQuery_ = nullptr; return -1.f; }
        if (PdhAddEnglishCounterW(pdhQuery_, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0,
                                  &pdhGpuCounter_) != ERROR_SUCCESS) {
            PdhCloseQuery(pdhQuery_);
            pdhQuery_ = nullptr;
            return -1.f;
        }
        PdhCollectQueryData(pdhQuery_);  // compteur de taux : il faut deux collectes
        return -1.f;
    }
    if (PdhCollectQueryData(pdhQuery_) != ERROR_SUCCESS) return -1.f;

    DWORD bytes = 0, count = 0;
    PDH_STATUS s = PdhGetFormattedCounterArrayW(pdhGpuCounter_, PDH_FMT_DOUBLE, &bytes, &count, nullptr);
    if (s != static_cast<PDH_STATUS>(PDH_MORE_DATA)) return -1.f;
    pdhBuffer_.resize(bytes);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(pdhBuffer_.data());
    if (PdhGetFormattedCounterArrayW(pdhGpuCounter_, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS)
        return -1.f;

    // Instances « pid_1234_luid_..._engtype_3D » : notre propre travail (le cerveau sur la carte) est exclu,
    // sinon il se mesurerait lui-même et se croirait chassé par le jeu.
    wchar_t self[32];
    const int selfLen = swprintf(self, 32, L"pid_%lu_", GetCurrentProcessId());
    double total = 0.0;
    for (DWORD i = 0; i < count; ++i) {
        if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA) continue;
        if (selfLen > 0 && items[i].szName && wcsncmp(items[i].szName, self, static_cast<size_t>(selfLen)) == 0) continue;
        total += items[i].FmtValue.doubleValue;
    }
    return static_cast<float>(std::clamp(total / 100.0, 0.0, 1.0));
}

}  // namespace companion
