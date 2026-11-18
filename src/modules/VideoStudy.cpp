#include "modules/VideoStudy.hpp"

#include "core/CallPrompt.hpp"
#include "core/LocalBrain.hpp"
#include "core/Log.hpp"
#include "core/MemoryDB.hpp"
#include "core/RageBait.hpp"
#include "network/HttpClient.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#if COMPANION_WITH_WHISPER
#include <whisper.h>
#endif

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

using Microsoft::WRL::ComPtr;

namespace companion {
namespace {

constexpr const char* kYtdlpUrl = "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe";
constexpr const char* kYtdlpSums = "https://github.com/yt-dlp/yt-dlp/releases/latest/download/SHA2-256SUMS";
constexpr const char* kDenoUrl = "https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip";
constexpr const char* kDenoSum = "https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip.sha256sum";

std::string Sha256File(const std::filesystem::path& file) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return {};
    std::string hex;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0) {
        std::ifstream f(file, std::ios::binary);
        std::vector<char> buf(1 << 16);
        while (f) {
            f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
            if (const auto n = f.gcount(); n > 0) BCryptHashData(h, reinterpret_cast<PUCHAR>(buf.data()), static_cast<ULONG>(n), 0);
        }
        UCHAR digest[32];
        if (BCryptFinishHash(h, digest, 32, 0) == 0) {
            static const char* d = "0123456789abcdef";
            for (UCHAR b : digest) {
                hex += d[b >> 4];
                hex += d[b & 15];
            }
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

// Téléchargé, puis vérifié : un fichier dont l'empreinte ne correspond pas n'est JAMAIS lancé.
bool DownloadVerified(HttpClient& http, const char* url, const char* sumsUrl, const std::string& sumsName,
                      const std::filesystem::path& dest, std::string& error) {
    const auto sums = http.Get(sumsUrl, 20000);
    const std::string expected = sums.Ok() ? ExpectedSha256(sums.body, sumsName) : std::string{};
    if (expected.empty()) {
        error = "empreinte officielle introuvable pour " + sumsName;
        return false;
    }
    auto tmp = dest;
    tmp += L".part";
    if (!http.DownloadToFile(url, tmp, {}, &error)) return false;
    if (Sha256File(tmp) != expected) {
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        error = "empreinte SHA-256 différente de l'officielle pour " + sumsName + " : fichier supprimé";
        return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, dest, ec);
    if (ec) {
        error = "écriture impossible";
        return false;
    }
    std::ofstream(std::filesystem::path(dest.wstring() + L".ok")) << expected;
    return true;
}

bool FreshTool(const std::filesystem::path& exe) {
    std::error_code ec;
    const auto ok = exe.wstring() + L".ok";
    if (!std::filesystem::exists(exe, ec) || !std::filesystem::exists(ok, ec)) return false;
    // YouTube change souvent : yt-dlp est repris tous les 30 jours.
    const auto age = std::filesystem::file_time_type::clock::now() - std::filesystem::last_write_time(ok, ec);
    return !ec && age < std::chrono::hours(24 * 30);
}

std::wstring Quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    for (wchar_t c : a) {
        if (c == L'"') q += L'\\';
        q += c;
    }
    return q + L"\"";
}

std::string ReadAll(const std::filesystem::path& f) {
    std::ifstream in(f, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Audio d'une vidéo (m4a / mp4) -> 16 kHz mono, décodé par Windows (Media Foundation) : pas de ffmpeg à installer.
std::vector<float> DecodeAudio16k(const std::filesystem::path& file, int maxSeconds) {
    std::vector<float> mono;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return mono;
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> want, got;
    UINT32 rate = 0, ch = 0;
    if (SUCCEEDED(MFCreateSourceReaderFromURL(file.c_str(), nullptr, &reader)) &&
        SUCCEEDED(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE)) &&
        SUCCEEDED(reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE)) && SUCCEEDED(MFCreateMediaType(&want)) &&
        SUCCEEDED(want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio)) && SUCCEEDED(want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float)) &&
        SUCCEEDED(reader->SetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), nullptr, want.Get())) &&
        SUCCEEDED(reader->GetCurrentMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), &got))) {
        rate = MFGetAttributeUINT32(got.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
        ch = MFGetAttributeUINT32(got.Get(), MF_MT_AUDIO_NUM_CHANNELS, 1);
    }
    std::vector<float> native;
    if (rate && ch) {
        for (;;) {
            DWORD flags = 0;
            ComPtr<IMFSample> sample;
            if (FAILED(reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, nullptr, &flags, nullptr, &sample)) ||
                (flags & MF_SOURCE_READERF_ENDOFSTREAM))
                break;
            if (!sample) continue;
            ComPtr<IMFMediaBuffer> buf;
            if (FAILED(sample->ConvertToContiguousBuffer(&buf))) continue;
            BYTE* data = nullptr;
            DWORD len = 0;
            if (SUCCEEDED(buf->Lock(&data, nullptr, &len))) {
                const auto* f = reinterpret_cast<const float*>(data);
                for (DWORD i = 0; i + ch <= len / 4; i += ch) {
                    float s = 0;
                    for (UINT32 c = 0; c < ch; ++c) s += f[i + c];
                    native.push_back(s / static_cast<float>(ch));
                }
                buf->Unlock();
            }
            if (native.size() > static_cast<std::size_t>(rate) * static_cast<std::size_t>(maxSeconds)) break;
        }
    }
    reader.Reset();
    MFShutdown();
    if (native.empty() || !rate) return mono;
    const double step = static_cast<double>(rate) / 16000.0;
    mono.resize(static_cast<std::size_t>(static_cast<double>(native.size()) / step));
    for (std::size_t i = 0; i < mono.size(); ++i) {
        const double x = static_cast<double>(i) * step;
        const std::size_t k = static_cast<std::size_t>(x);
        const float fr = static_cast<float>(x - static_cast<double>(k));
        mono[i] = native[k] * (1.f - fr) + (k + 1 < native.size() ? native[k + 1] : native[k]) * fr;
    }
    return mono;
}

}  // namespace

VideoStudy::VideoStudy(HttpClient& http, std::filesystem::path dataDir, VideoStudyConfig cfg)
    : http_(http), dir_(std::move(dataDir)), cfg_(std::move(cfg)) {
    tools_ = dir_ / L"tools";
    ytdlp_ = tools_ / L"yt-dlp.exe";
    deno_ = tools_ / L"deno.exe";
}

bool VideoStudy::EnsureTools(std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(tools_, ec);
    if (!FreshTool(ytdlp_) && !DownloadVerified(http_, kYtdlpUrl, kYtdlpSums, "yt-dlp.exe", ytdlp_, error)) return false;
    if (!FreshTool(deno_)) {
        const auto zip = tools_ / L"deno.zip";
        if (!DownloadVerified(http_, kDenoUrl, kDenoSum, "deno-x86_64-pc-windows-msvc.zip", zip, error)) return false;
        // Windows 10+ fournit tar.exe (bsdtar), qui sait ouvrir les .zip.
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        std::string out;
        Run({std::wstring(sys) + L"\\tar.exe", L"-xf", zip.wstring(), L"-C", tools_.wstring()}, out, 120);
        std::filesystem::remove(zip, ec);
        std::filesystem::remove(zip.wstring() + L".ok", ec);
        if (!std::filesystem::exists(deno_, ec)) {
            error = "décompression de deno impossible";
            return false;
        }
        std::ofstream(std::filesystem::path(deno_.wstring() + L".ok")) << "zip vérifié";
    }
    return true;
}

int VideoStudy::Run(const std::vector<std::wstring>& args, std::string& out, int timeoutSec, std::stop_token stop) {
    out.clear();
    std::wstring cmd;
    for (const auto& a : args) cmd += (cmd.empty() ? L"" : L" ") + Quote(a);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmd;
    const BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS,
                                   nullptr, tools_.c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        return -1;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    char buf[4096];
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
            DWORD n = 0;
            if (ReadFile(rd, buf, std::min<DWORD>(avail, sizeof(buf)), &n, nullptr) && n) out.append(buf, n);
            if (out.size() > (8u << 20)) out.erase(0, out.size() - (4u << 20));
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
            DWORD n = 0;
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail && ReadFile(rd, buf, std::min<DWORD>(avail, sizeof(buf)), &n, nullptr) && n)
                out.append(buf, n);
            break;
        }
        if (std::chrono::steady_clock::now() > deadline || stop.stop_requested()) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            break;
        }
    }
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return static_cast<int>(code);
}

std::vector<VideoItem> VideoStudy::Search(const std::string& query, int n) {
    std::string out;
    Run({ytdlp_.wstring(), L"--js-runtimes", L"deno:" + deno_.wstring(), L"--flat-playlist", L"--no-warnings", L"--print",
         L"%(id)s\t%(title)s\t%(duration)s\t%(view_count)s", L"ytsearch" + std::to_wstring(n) + L":" + log::Widen(query)},
        out, 90);
    return ParseSearchLines(out);
}

std::string VideoStudy::Transcribe(const std::filesystem::path& audio) {
#if COMPANION_WITH_WHISPER
    if (cfg_.whisperModel.empty() || !std::filesystem::exists(cfg_.whisperModel)) return {};
    const std::vector<float> pcm = DecodeAudio16k(audio, cfg_.maxSeconds);
    if (pcm.size() < 16000 * 2) return {};
    whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = false;
    whisper_context* w = whisper_init_from_file_with_params(log::Narrow(cfg_.whisperModel.wstring()).c_str(), cp);
    if (!w) return {};
    whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wp.language = "fr";
    wp.n_threads = 4;
    wp.print_progress = wp.print_realtime = wp.print_timestamps = wp.print_special = false;
    std::string text;
    if (whisper_full(w, wp, pcm.data(), static_cast<int>(pcm.size())) == 0)
        for (int i = 0; i < whisper_full_n_segments(w); ++i) text += whisper_full_get_segment_text(w, i);
    whisper_free(w);
    return text;
#else
    (void)audio;
    return {};
#endif
}

bool VideoStudy::Fetch(const std::string& url, std::string& title, std::string& transcript, std::string& comments, std::string& error) {
    const auto tmp = dir_ / L"video-tmp";
    std::error_code ec;
    std::filesystem::remove_all(tmp, ec);
    std::filesystem::create_directories(tmp, ec);
    const std::wstring out = (tmp / L"v.%(ext)s").wstring();
    std::string log;
    // Infos + sous-titres (manuels, sinon automatiques) + commentaires les plus likés, sans télécharger la vidéo.
    Run({ytdlp_.wstring(), L"--js-runtimes", L"deno:" + deno_.wstring(), L"--no-warnings", L"--no-playlist", L"--skip-download",
         L"--write-info-json", L"--write-subs", L"--write-auto-subs", L"--sub-langs", L"fr.*,fr,en.*", L"--sub-format", L"json3",
         L"--write-comments", L"--extractor-args", L"youtube:max_comments=40,all,0,0;comment_sort=top", L"-o", out, log::Widen(url)},
        log, 150);
    const std::string info = ReadAll(tmp / L"v.info.json");
    if (info.empty()) {
        error = log.size() > 300 ? log.substr(log.size() - 300) : log;
        return false;
    }
    try {
        const auto j = nlohmann::json::parse(info);
        title = j.value("title", std::string{});
        if (j.value("duration", 0.0) > cfg_.maxSeconds) {
            error = "vidéo trop longue";
            return false;
        }
        if (const auto d = j.value("description", std::string{}); !d.empty()) transcript = "[Description] " + d.substr(0, 400) + "\n";
    } catch (const std::exception&) {
    }
    comments = TopComments(info, 15);
    // Sous-titres : le français d'abord.
    std::filesystem::path subs;
    for (const auto& e : std::filesystem::directory_iterator(tmp, ec))
        if (e.path().extension() == L".json3" && (subs.empty() || e.path().filename().wstring().find(L".fr") != std::wstring::npos)) subs = e.path();
    std::string text = subs.empty() ? std::string{} : Json3ToText(ReadAll(subs));
    // Pas de sous-titres (TikTok, Shorts…) : l'audio, transcrit en local par Whisper.
    if (text.size() < 80) {
        Run({ytdlp_.wstring(), L"--js-runtimes", L"deno:" + deno_.wstring(), L"--no-warnings", L"--no-playlist", L"-f",
             L"bestaudio[ext=m4a]/bestaudio[ext=mp4]/best[ext=mp4]/best", L"-o", (tmp / L"a.%(ext)s").wstring(), log::Widen(url)},
            log, 240);
        for (const auto& e : std::filesystem::directory_iterator(tmp, ec))
            if (e.path().filename().wstring().rfind(L"a.", 0) == 0) {
                text = Transcribe(e.path());
                break;
            }
    }
    std::filesystem::remove_all(tmp, ec);  // rien n'est gardé
    transcript += text.substr(0, 6000);
    if (transcript.size() < 80 && comments.empty()) {
        error = "ni sous-titres, ni audio exploitable";
        return false;
    }
    return true;
}

bool VideoStudy::QueueLink(const std::filesystem::path& dataDir, const std::string& url) {
    std::string u = url;
    while (!u.empty() && (u.back() == ' ' || u.back() == '\n' || u.back() == '\r')) u.pop_back();
    if (u.rfind("https://", 0) != 0) return false;
    const std::string host = u.substr(8, u.find('/', 8) - 8);
    static const char* ok[] = {"www.youtube.com", "youtube.com", "m.youtube.com", "youtu.be", "www.tiktok.com", "tiktok.com", "vm.tiktok.com"};
    if (std::none_of(std::begin(ok), std::end(ok), [&](const char* h) { return host == h; })) return false;
    if (u.find_first_of(" \"'<>") != std::string::npos || u.size() > 300) return false;
    std::ofstream(dataDir / L"to_watch.txt", std::ios::app) << u << "\n";
    return true;
}

std::string VideoStudy::RunSession(LocalBrain& brain, MemoryDB& db, BaitBank& bank, std::stop_token stop) {
    std::string err;
    if (!EnsureTools(err)) {
        log::Info("VideoStudy: outils indisponibles ({})", err);
        return {};
    }
    // Déjà vues (500 dernières) : on ne regarde pas deux fois la même.
    std::vector<std::string> seen;
    try {
        seen = nlohmann::json::parse(db.GetProfile("videostudy.seen").value_or("[]")).get<std::vector<std::string>>();
    } catch (const std::exception&) {
    }
    std::vector<std::string> todo;
    {  // 1) La file : liens donnés par toi (TikTok, YouTube)
        std::ifstream q(dir_ / L"to_watch.txt");
        for (std::string l; std::getline(q, l);)
            if (!l.empty() && std::ranges::find(seen, l) == seen.end()) todo.push_back(l);
    }
    // 2) Des recherches, à tour de rôle
    int qi = std::atoi(db.GetProfile("videostudy.q").value_or("0").c_str());
    for (int tries = 0; static_cast<int>(todo.size()) < cfg_.perSession + 2 && tries < 3 && !cfg_.queries.empty(); ++tries) {
        const std::string& query = cfg_.queries[static_cast<std::size_t>(qi++) % cfg_.queries.size()];
        for (const auto& v : Search(query, 10))
            if (v.seconds >= 15 && v.seconds <= cfg_.maxSeconds && std::ranges::find(seen, v.url) == seen.end() &&
                std::ranges::find(todo, v.url) == todo.end())
                todo.push_back(v.url);
    }
    db.SetProfile("videostudy.q", std::to_string(qi));

    int studied = 0, forms = 0;
    std::vector<std::string> titles;
    for (const auto& url : todo) {
        if (studied >= cfg_.perSession || stop.stop_requested()) break;
        seen.push_back(url);
        std::string title, transcript, comments;
        if (!Fetch(url, title, transcript, comments, err)) {
            log::Info("VideoStudy: {} ignorée ({})", url, err);
            continue;
        }
        const std::string user = StudyPrompt(title, transcript, comments);
        GenerateOptions o;
        o.maxTokens = 380;
        o.temperature = 0.4f;
        o.grammar = kStudyGrammar;
        o.stop = stop;
        const auto out = brain.Generate({{"system", kStudySystem}, {"user", user}}, o);
        if (!out) continue;
        StudyResult r = ParseStudy(*out, url);
        if (!r.humour) {
            log::Info("VideoStudy: « {} » : pas d'humour exploitable", title);
            continue;
        }
        std::erase_if(r.forms, [](const BaitForm& f) { return !CrossesLine(f.form, {}).empty(); });
        forms += bank.AddLearned(r.forms);
        db.AddLesson("rage bait : " + title, r.technique, r.technique + " — " + r.why);
        ++studied;
        titles.push_back(title);
        log::Info("VideoStudy: « {} » -> {} ({} formes)", title, r.technique, r.forms.size());
    }
    if (seen.size() > 500) seen.erase(seen.begin(), seen.end() - 500);
    db.SetProfile("videostudy.seen", nlohmann::json(seen).dump());
    if (studied >= 2)
        for (const char* t : kRageBaitTopics) db.MarkTrained(t, "appris");  // les sujets « rage bait » sont travaillés
    if (!studied) return {};
    return "J'ai regardé " + std::to_string(studied) + " vidéo" + (studied > 1 ? "s" : "") + " pour travailler mes vannes, et j'en ai tiré " +
           std::to_string(forms) + " nouvelle" + (forms > 1 ? "s" : "") + " forme" + (forms > 1 ? "s" : "") + ".";
}

}  // namespace companion
