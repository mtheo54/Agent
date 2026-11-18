#include "core/GamePerfMonitor.hpp"

#include "core/Log.hpp"

#include <evntcons.h>
#include <pdhmsg.h>

#include <algorithm>
#include <cwchar>
#include <ctime>
#include <format>
#include <fstream>

#ifndef EVENT_FILTER_TYPE_PID
#define EVENT_FILTER_TYPE_PID 0x80000004
#endif
#ifndef INVALID_PROCESSTRACE_HANDLE
#define INVALID_PROCESSTRACE_HANDLE (static_cast<TRACEHANDLE>(INVALID_HANDLE_VALUE))
#endif

namespace companion {
namespace {

// Microsoft-Windows-DXGI : Present_Start (id 42) à chaque image présentée par un jeu D3D11 / D3D12.
constexpr GUID kDxgi = {0xCA11C036, 0x0102, 0x4A2D, {0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9}};
constexpr USHORT kPresentStart = 42;
constexpr ULONGLONG kDxgiKeywords = 0x8000000000000002ull;  // Events (0x2) | Microsoft-Windows-DXGI/Analytic : le mot-clé de Present_Start (comme PresentMon)
constexpr wchar_t kSessionName[] = L"CompanionAI-FrameTimes";

ULONGLONG CpuTime(HANDLE process) {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(process, &c, &e, &k, &u)) return 0;
    return ((static_cast<ULONGLONG>(k.dwHighDateTime) << 32) | k.dwLowDateTime) +
           ((static_cast<ULONGLONG>(u.dwHighDateTime) << 32) | u.dwLowDateTime);
}

EVENT_TRACE_PROPERTIES* FillProps(std::vector<BYTE>& buf) {
    buf.assign(sizeof(EVENT_TRACE_PROPERTIES) + sizeof(kSessionName), 0);
    auto* p = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(buf.data());
    p->Wnode.BufferSize = static_cast<ULONG>(buf.size());
    p->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    p->Wnode.ClientContext = 1;  // horodatage QueryPerformanceCounter
    p->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    p->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
    p->FlushTimer = 1;
    p->BufferSize = 16;  // Ko
    p->MinimumBuffers = 4;
    p->MaximumBuffers = 32;
    return p;
}

std::string Pct(double v) { return v < 0 ? std::string("?") : std::format("{:.1f}", v); }

}  // namespace

GamePerfMonitor::~GamePerfMonitor() { Stop(); }

bool GamePerfMonitor::Start(DWORD gamePid, std::wstring game, std::filesystem::path reportDir, BusyProbe busy,
                            PresetName preset) {
    Stop();
    if (gamePid == 0 || gamePid == GetCurrentProcessId()) return false;
    gameHandle_ = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, gamePid);
    if (!gameHandle_) return false;
    pid_ = gamePid;
    game_ = std::move(game);
    busyProbe_ = std::move(busy);
    presetName_ = std::move(preset);
    {
        std::scoped_lock lock(mutex_);
        stats_ = PerfSession{};
        stats_.game = game_;
        second_.Reset();
        window_.Reset();
        windowOn_ = false;
        sumSelfCpu_ = sumGameCpu_ = sumSelfGpu_ = sumGameGpu_ = 0.0;
        nCpu_ = nGpu_ = 0;
    }
    lastQpc_ = 0;
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    qpcToMs_ = f.QuadPart > 0 ? 1000.0 / static_cast<double>(f.QuadPart) : 0.0;
    lastGameGpu_ = lastSelfGpu_ = -1.0;
    tick_ = 0;

    // Fichiers : perf-<jeu>-AAAAMMJJ-HHMM.csv / .txt ; au-delà de 60 fichiers, les plus vieux partent.
    std::error_code ec;
    std::filesystem::create_directories(reportDir, ec);
    {
        std::vector<std::filesystem::directory_entry> old;
        for (const auto& e : std::filesystem::directory_iterator(reportDir, ec))
            if (e.is_regular_file() && e.path().filename().wstring().starts_with(L"perf-")) old.push_back(e);
        if (old.size() > 60) {
            std::ranges::sort(old, {}, [](const auto& e) { return e.last_write_time(); });
            for (size_t i = 0; i + 60 < old.size(); ++i) std::filesystem::remove(old[i].path(), ec);
        }
    }
    startedAt_ = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(startedAt_);
    std::tm tm{};
    localtime_s(&tm, &t);
    wchar_t stamp[32];
    std::wcsftime(stamp, 32, L"%Y%m%d-%H%M", &tm);
    std::wstring stem = std::filesystem::path(game_).stem().wstring();
    std::ranges::replace(stem, L' ', L'_');
    const std::wstring base = L"perf-" + stem + L"-" + stamp;
    csvFile_ = reportDir / (base + L".csv");
    summaryFile_ = reportDir / (base + L".txt");
    csv_ = _wfopen(csvFile_.c_str(), L"wb");
    if (csv_) std::fputs("seconde;fps;fps_1pc_low;jeu_cpu_pct;jeu_gpu_pct;omnis_cpu_pct;omnis_gpu_pct;omnis_travaille;preset\n", csv_);

    fpsOk_ = StartEtw();
    {
        std::scoped_lock lock(mutex_);
        stats_.fpsAvailable = fpsOk_.load();
    }
    lastGameCpu_ = CpuTime(gameHandle_);
    lastSelfCpu_ = CpuTime(GetCurrentProcess());
    lastSample_ = std::chrono::steady_clock::now();
    running_ = true;
    sampler_ = std::jthread([this](std::stop_token st) { SamplerMain(st); });
    log::Info("Perf: mesure de {} (pid {}) — FPS {}", log::Narrow(game_), pid_,
              fpsOk_ ? "via ETW" : "indisponibles : " + fpsProblem_);
    return true;
}

void GamePerfMonitor::Stop() {
    if (!running_) return;
    sampler_ = {};  // demande l'arrêt et attend le thread
    StopEtw();
    WriteSummary();
    if (csv_) {
        std::fclose(csv_);
        csv_ = nullptr;
    }
    if (pdhQuery_) {
        PdhCloseQuery(pdhQuery_);
        pdhQuery_ = nullptr;
    }
    if (gameHandle_) {
        CloseHandle(gameHandle_);
        gameHandle_ = nullptr;
    }
    running_ = false;
    log::Info("Perf: fin de la mesure, résumé dans {}", log::Narrow(summaryFile_.wstring()));
}

// ---------------------------------------------------------------------------
bool GamePerfMonitor::StartEtw() {
    fpsProblem_.clear();
    ControlTraceW(0, kSessionName, FillProps(props_), EVENT_TRACE_CONTROL_STOP);  // session orpheline (plantage)
    ULONG e = StartTraceW(&session_, kSessionName, FillProps(props_));
    if (e != ERROR_SUCCESS) {
        session_ = 0;
        fpsProblem_ = e == ERROR_ACCESS_DENIED
                          ? "Windows refuse la lecture des FPS sans le droit « Utilisateurs du journal de performances »"
                          : std::format("session ETW impossible (erreur {})", e);
        return false;
    }
    // Filtre par processus côté fournisseur : seul le jeu émet, les autres applis ne coûtent rien.
    DWORD pid = pid_;
    EVENT_FILTER_DESCRIPTOR fd{};
    fd.Ptr = reinterpret_cast<ULONGLONG>(&pid);
    fd.Size = sizeof(pid);
    fd.Type = EVENT_FILTER_TYPE_PID;
    ENABLE_TRACE_PARAMETERS params{};
    params.Version = ENABLE_TRACE_PARAMETERS_VERSION_2;
    params.EnableFilterDesc = &fd;
    params.FilterDescCount = 1;
    e = EnableTraceEx2(session_, &kDxgi, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, kDxgiKeywords, 0, 0, &params);
    if (e != ERROR_SUCCESS)  // vieux Windows : sans filtre (on trie nous-mêmes par pid)
        e = EnableTraceEx2(session_, &kDxgi, EVENT_CONTROL_CODE_ENABLE_PROVIDER, TRACE_LEVEL_INFORMATION, kDxgiKeywords, 0, 0, nullptr);
    if (e != ERROR_SUCCESS) {
        fpsProblem_ = std::format("fournisseur DXGI indisponible (erreur {})", e);
        StopEtw();
        return false;
    }
    EVENT_TRACE_LOGFILEW lf{};
    lf.LoggerName = const_cast<LPWSTR>(kSessionName);
    lf.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD | PROCESS_TRACE_MODE_RAW_TIMESTAMP;
    lf.EventRecordCallback = &GamePerfMonitor::OnEvent;
    lf.Context = this;
    trace_ = OpenTraceW(&lf);
    if (trace_ == INVALID_PROCESSTRACE_HANDLE) {
        fpsProblem_ = "lecture de la session ETW impossible";
        StopEtw();
        return false;
    }
    etwThread_ = std::thread([this] {
        log::SetCurrentThreadName(L"Companion.PerfEtw");
        TRACEHANDLE h = trace_;
        ProcessTrace(&h, 1, nullptr, nullptr);  // bloque jusqu'à l'arrêt de la session
    });
    return true;
}

void GamePerfMonitor::StopEtw() {
    if (session_) {
        ControlTraceW(session_, nullptr, FillProps(props_), EVENT_TRACE_CONTROL_STOP);
        session_ = 0;
    }
    if (trace_ != INVALID_PROCESSTRACE_HANDLE) {
        CloseTrace(trace_);
        trace_ = INVALID_PROCESSTRACE_HANDLE;
    }
    if (etwThread_.joinable()) etwThread_.join();
    fpsOk_ = false;
}

void WINAPI GamePerfMonitor::OnEvent(_EVENT_RECORD* rec) {
    auto* self = static_cast<GamePerfMonitor*>(rec->UserContext);
    if (!self || rec->EventHeader.EventDescriptor.Id != kPresentStart || rec->EventHeader.ProcessId != self->pid_ ||
        !IsEqualGUID(rec->EventHeader.ProviderId, kDxgi))
        return;
    self->OnPresent(rec->EventHeader.TimeStamp.QuadPart);
}

// Intervalle entre deux présentations = durée d'une image. Rangée selon ce que fait Omnis à cet instant.
void GamePerfMonitor::OnPresent(std::int64_t qpc) {
    if (lastQpc_ == 0) {
        lastQpc_ = qpc;
        return;
    }
    const double ms = static_cast<double>(qpc - lastQpc_) * qpcToMs_;
    lastQpc_ = qpc;
    const bool busy = busyProbe_ && busyProbe_();
    std::scoped_lock lock(mutex_);
    (busy ? stats_.busy : stats_.calm).Add(ms);
    second_.Add(ms);
    if (windowOn_) window_.Add(ms);
}

// ---------------------------------------------------------------------------
void GamePerfMonitor::SampleGpu(double& gameGpu, double& selfGpu) {
    if (!pdhQuery_) {
        if (PdhOpenQueryW(nullptr, 0, &pdhQuery_) != ERROR_SUCCESS) { pdhQuery_ = nullptr; return; }
        if (PdhAddEnglishCounterW(pdhQuery_, L"\\GPU Engine(*engtype_3D)\\Utilization Percentage", 0, &pdhCounter_) != ERROR_SUCCESS) {
            PdhCloseQuery(pdhQuery_);
            pdhQuery_ = nullptr;
            return;
        }
        PdhCollectQueryData(pdhQuery_);  // compteur de taux : la 1re collecte sert de référence
        return;
    }
    if (PdhCollectQueryData(pdhQuery_) != ERROR_SUCCESS) return;
    DWORD bytes = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(pdhCounter_, PDH_FMT_DOUBLE, &bytes, &count, nullptr) != static_cast<PDH_STATUS>(PDH_MORE_DATA))
        return;
    pdhBuffer_.resize(bytes);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(pdhBuffer_.data());
    if (PdhGetFormattedCounterArrayW(pdhCounter_, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS) return;
    const DWORD self = GetCurrentProcessId();
    double g = 0.0, s = 0.0;
    for (DWORD i = 0; i < count; ++i) {
        if (items[i].FmtValue.CStatus != PDH_CSTATUS_VALID_DATA || !items[i].szName) continue;
        if (std::wcsncmp(items[i].szName, L"pid_", 4) != 0) continue;  // « pid_1234_luid_..._engtype_3D »
        const DWORD pid = static_cast<DWORD>(std::wcstoul(items[i].szName + 4, nullptr, 10));
        if (pid == pid_) g += items[i].FmtValue.doubleValue;
        else if (pid == self) s += items[i].FmtValue.doubleValue;
    }
    gameGpu = std::min(100.0, g);
    selfGpu = std::min(100.0, s);
}

void GamePerfMonitor::SamplerMain(std::stop_token st) {
    log::SetCurrentThreadName(L"Companion.PerfSampler");
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const double cores = std::max<DWORD>(1, GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    int line = 0;
    while (!st.stop_requested()) {
        for (int i = 0; i < 10 && !st.stop_requested(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (st.stop_requested()) break;
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - lastSample_).count();
        lastSample_ = now;
        if (dt <= 0.0) continue;
        const ULONGLONG gc = CpuTime(gameHandle_), sc = CpuTime(GetCurrentProcess());
        const double gameCpu = gc >= lastGameCpu_ ? static_cast<double>(gc - lastGameCpu_) / 1e7 / dt / cores * 100.0 : -1.0;
        const double selfCpu = sc >= lastSelfCpu_ ? static_cast<double>(sc - lastSelfCpu_) / 1e7 / dt / cores * 100.0 : -1.0;
        lastGameCpu_ = gc;
        lastSelfCpu_ = sc;
        if (++tick_ % 2 == 0) SampleGpu(lastGameGpu_, lastSelfGpu_);  // PDH : quelques ms, une fois toutes les 2 s
        const bool busy = busyProbe_ && busyProbe_();

        double fps = -1, low = -1, elapsed = 0;
        {
            std::scoped_lock lock(mutex_);
            fps = second_.AvgFps();
            low = second_.Low1Fps();
            second_.Reset();
            stats_.seconds += dt;
            if (busy) stats_.busySeconds += dt;
            elapsed = stats_.seconds;
            if (selfCpu >= 0 && gameCpu >= 0) {
                sumSelfCpu_ += selfCpu;
                sumGameCpu_ += gameCpu;
                ++nCpu_;
                stats_.selfCpuPeak = std::max(stats_.selfCpuPeak, selfCpu);
            }
            if (tick_ % 2 == 0 && lastGameGpu_ >= 0) {
                sumSelfGpu_ += lastSelfGpu_;
                sumGameGpu_ += lastGameGpu_;
                ++nGpu_;
                stats_.selfGpuPeak = std::max(stats_.selfGpuPeak, lastSelfGpu_);
            }
        }
        if (csv_) {
            const std::string row = std::format("{:.0f};{};{};{};{};{};{};{};{}\n", elapsed, fps < 0 ? "" : std::format("{:.1f}", fps),
                                                low < 0 ? "" : std::format("{:.1f}", low), Pct(gameCpu), Pct(lastGameGpu_),
                                                Pct(selfCpu), Pct(lastSelfGpu_), busy ? 1 : 0, presetName_ ? presetName_() : "");
            std::fputs(row.c_str(), csv_);
            if (++line % 10 == 0) std::fflush(csv_);
        }
    }
}

// ---------------------------------------------------------------------------
void GamePerfMonitor::BeginWindow() {
    std::scoped_lock lock(mutex_);
    window_.Reset();
    windowOn_ = true;
}

FrameStats GamePerfMonitor::EndWindow() {
    std::scoped_lock lock(mutex_);
    windowOn_ = false;
    return window_;
}

PerfSession GamePerfMonitor::Snapshot() const {
    std::scoped_lock lock(mutex_);
    PerfSession s = stats_;
    s.fpsAvailable = fpsOk_.load();
    if (nCpu_ > 0) {
        s.selfCpuAvg = sumSelfCpu_ / nCpu_;
        s.gameCpuAvg = sumGameCpu_ / nCpu_;
    }
    if (nGpu_ > 0) {
        s.selfGpuAvg = sumSelfGpu_ / nGpu_;
        s.gameGpuAvg = sumGameGpu_ / nGpu_;
    } else {
        s.selfGpuAvg = s.gameGpuAvg = -1.0;
    }
    return s;
}

void GamePerfMonitor::ResetBusy() {
    std::scoped_lock lock(mutex_);
    stats_.busy.Reset();
    stats_.busySeconds = 0.0;
}

void GamePerfMonitor::WriteSummary() {
    const PerfSession s = Snapshot();
    if (s.seconds < 30.0 || summaryFile_.empty()) return;  // partie trop courte pour dire quoi que ce soit
    const PerfImpact i = Impact(s.calm, s.busy);
    const std::time_t t = std::chrono::system_clock::to_time_t(startedAt_);
    std::tm tm{};
    localtime_s(&tm, &t);
    char date[32];
    std::strftime(date, sizeof(date), "%d/%m/%Y %H:%M", &tm);
    std::string out = std::format("Omnis — performances pendant {} ({}, {:.0f} min)\n\n", log::Narrow(s.game), date, s.seconds / 60.0);
    if (!s.fpsAvailable && s.calm.frames == 0) {
        out += "FPS : non mesurés (" + (fpsProblem_.empty() ? std::string("jeu sans DXGI, ex. Vulkan") : fpsProblem_) + ").\n";
    } else {
        out += std::format("FPS au calme              : {:.0f} (1 % low {:.0f})\n", s.calm.AvgFps(), s.calm.Low1Fps());
        if (s.busy.frames > 0)
            out += std::format("FPS pendant qu'Omnis bosse : {:.0f} (1 % low {:.0f})\n", s.busy.AvgFps(), s.busy.Low1Fps());
        if (i.known)
            out += std::format("Impact d'Omnis            : -{:.1f} % de FPS, -{:.1f} % sur le 1 % low → {}\n", i.fpsDropPct, i.lowDropPct,
                               TooMuch(i, maxFpsDrop_, maxLowDrop_) ? "ÇA BOUFFE TROP" : "OK");
        else
            out += "Impact d'Omnis            : pas assez de temps de travail pendant la partie pour conclure.\n";
    }
    out += std::format("Omnis a travaillé         : {:.0f} s ({:.1f} % de la partie)\n", s.busySeconds,
                       s.seconds > 0 ? s.busySeconds / s.seconds * 100.0 : 0.0);
    out += std::format("Processeur pris par Omnis : moyenne {:.1f} %, pic {:.1f} %\n", s.selfCpuAvg, s.selfCpuPeak);
    if (s.selfGpuAvg >= 0)
        out += std::format("Carte prise par Omnis     : moyenne {:.1f} %, pic {:.1f} %\n", s.selfGpuAvg, s.selfGpuPeak);
    out += std::format("Le jeu                    : processeur {:.0f} %, carte {} %\n", s.gameCpuAvg,
                       s.gameGpuAvg >= 0 ? std::format("{:.0f}", s.gameGpuAvg) : std::string("?"));
    out += std::format("Preset en fin de partie   : {}\n", presetName_ ? presetName_() : std::string("?"));
    out += "\nDétail seconde par seconde : " + log::Narrow(csvFile_.filename().wstring()) + "\n";
    std::ofstream f(summaryFile_, std::ios::binary | std::ios::trunc);
    f << out;
    log::Info("Perf:\n{}", out);
}

}  // namespace companion
