#include "core/LocalBrain.hpp"

#include "core/Log.hpp"

#if COMPANION_WITH_LLAMA
#include <llama.h>
#endif
#if COMPANION_WITH_LLAMA && COMPANION_WITH_MTMD
#include <mtmd-helper.h>
#include <mtmd.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <thread>

namespace companion {

#if COMPANION_WITH_LLAMA

namespace {
void QuietLog(ggml_log_level level, const char* text, void*) {
    if (level >= GGML_LOG_LEVEL_WARN) {
        OutputDebugStringA(text);
        log::Raw(std::string("[llama] ") + text);  // erreurs Vulkan, mémoire… dans omnis.log
    }
}
}  // namespace

LocalBrain::LocalBrain() {
    static const bool init = [] {
        llama_log_set(&QuietLog, nullptr);
        llama_backend_init();
        return true;
    }();
    (void)init;
}

LocalBrain::~LocalBrain() { Unload(); }

bool LocalBrain::Available() const { return std::filesystem::exists(cfg_.model); }

bool LocalBrain::Load(const LocalBrainConfig& cfg) {
    Unload();
    cfg_ = cfg;
    if (!std::filesystem::exists(cfg.model)) {
        log::Info("LocalBrain: modèle introuvable ({})", log::Narrow(cfg.model.wstring()));
        return false;
    }
    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = useGpu_ ? cfg.gpuLayers : 0;
    // MoE : experts des premières couches en RAM (comme --n-cpu-moe de llama.cpp).
    std::vector<llama_model_tensor_buft_override> overrides;
    overridePatterns_.clear();
    if (useGpu_ && cfg.cpuMoeLayers > 0) {
        overridePatterns_.reserve(static_cast<size_t>(cfg.cpuMoeLayers));
        for (int i = 0; i < cfg.cpuMoeLayers; ++i)
            overridePatterns_.push_back("blk\\." + std::to_string(i) + "\\.ffn_(up|down|gate|gate_up)_(ch|)exps");
        for (const auto& pat : overridePatterns_) overrides.push_back({pat.c_str(), ggml_backend_cpu_buffer_type()});
        overrides.push_back({nullptr, nullptr});
        mp.tensor_buft_overrides = overrides.data();
        log::Info("LocalBrain: experts de {} couches gardés en RAM", cfg.cpuMoeLayers);
    }
    // load_mode par défaut = mmap : un rechargement GPU <-> CPU relit le cache disque de Windows.
    model_ = llama_model_load_from_file(log::Narrow(cfg.model.wstring()).c_str(), mp);
    if (!model_) {
        log::Info("LocalBrain: chargement du modèle impossible");
        return false;
    }
    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = static_cast<uint32_t>(cfg.contextTokens);
    const bool vision = !cfg.mmproj.empty();
    cp.n_batch = vision ? static_cast<uint32_t>(cfg.mediaBatch) : 512;  // une image fait plusieurs centaines de tokens
    cp.n_ubatch = vision ? static_cast<uint32_t>(cfg.mediaBatch) : static_cast<uint32_t>(std::clamp(cfg.ubatch, 32, 512));
    cp.n_threads = cfg.threads;
    cp.n_threads_batch = cfg.threads;
    cp.no_perf = true;
    ctx_ = llama_init_from_model(model_, cp);
    if (!ctx_) {
        log::Info("LocalBrain: création du contexte impossible");
        llama_model_free(model_);
        model_ = nullptr;
        return false;
    }
    cachedTokens_.clear();
    snapTokens_.clear();
    snapState_.clear();
    recurrent_ = llama_model_is_recurrent(model_) || llama_model_is_hybrid(model_);
    if (!cfg.lora.empty() && std::filesystem::exists(cfg.lora)) {
        // Libéré automatiquement avec le modèle.
        if (llama_adapter_lora* adapter = llama_adapter_lora_init(model_, log::Narrow(cfg.lora.wstring()).c_str())) {
            float scale = 1.0f;
            llama_set_adapters_lora(ctx_, &adapter, 1, &scale);
            log::Info("LocalBrain: adaptateur LoRA appliqué");
        } else {
            log::Info("LocalBrain: lora.gguf incompatible avec ce modèle, ignoré");
        }
    }
    log::Info("LocalBrain: modèle chargé ({}, ctx {})", useGpu_ ? "GPU" : "CPU", cfg.contextTokens);
    return true;
}

void LocalBrain::Unload() {
#if COMPANION_WITH_MTMD
    if (mtmd_) mtmd_free(mtmd_);
#endif
    mtmd_ = nullptr;
    if (ctx_) llama_free(ctx_);
    if (model_) llama_model_free(model_);
    ctx_ = nullptr;
    model_ = nullptr;
    cachedTokens_.clear();
    snapTokens_.clear();
    snapState_.clear();
}

bool LocalBrain::Configure(bool useGpu, int threads) {
    cfg_.threads = threads;
    if (useGpu != useGpu_) {
        useGpu_ = useGpu;
        // Changement de périphérique : on libère tout de suite (la VRAM revient au jeu) et on ne recharge
        // que si on parle au compagnon. Pas de lecture de 5 Go sur le disque pendant que le jeu se lance.
        if (IsLoaded()) Unload();
    } else if (ctx_) {
        llama_set_n_threads(ctx_, threads, threads);
    }
    return true;
}

std::string LocalBrain::ApplyTemplate(const std::vector<ChatTurn>& messages) const {
    std::vector<llama_chat_message> chat;
    chat.reserve(messages.size());
    size_t total = 0;
    for (const auto& m : messages) {
        chat.push_back({m.role.c_str(), m.content.c_str()});
        total += m.content.size();
    }
    const char* tmpl = llama_model_chat_template(model_, nullptr);
    std::string buf(total * 2 + 1024, '\0');
    int32_t n = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, buf.data(), static_cast<int32_t>(buf.size()));
    if (n > static_cast<int32_t>(buf.size())) {
        buf.resize(static_cast<size_t>(n));
        n = llama_chat_apply_template(tmpl, chat.data(), chat.size(), true, buf.data(), n);
    }
    if (n < 0) return {};
    buf.resize(static_cast<size_t>(n));
    // Modèles « à réflexion » (Qwen3 / Qwen3.5…) : bloc de réflexion vide d'office, comme leur template avec
    // enable_thinking=false. À l'oral on veut la réponse tout de suite, pas un monologue interne.
    if (tmpl && std::string_view(tmpl).find("<think>") != std::string_view::npos) buf += "<think>\n\n</think>\n\n";
    return buf;
}

std::optional<std::string> LocalBrain::Generate(const std::vector<ChatTurn>& messages, const GenerateOptions& opt) {
    const BusyScope busy(busy_);
    if (!IsLoaded() && !Load(cfg_)) return std::nullopt;
    const llama_vocab* vocab = llama_model_get_vocab(model_);

    const std::string prompt = ApplyTemplate(messages);
    if (prompt.empty()) return std::nullopt;

    // Tokenisation (le template inclut déjà les tokens spéciaux du modèle).
    int n = -llama_tokenize(vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()), nullptr, 0, true, true);
    std::vector<llama_token> tokens(static_cast<size_t>(std::max(n, 0)));
    if (n <= 0 || llama_tokenize(vocab, prompt.c_str(), static_cast<int32_t>(prompt.size()), tokens.data(), n, true,
                                 true) < 0)
        return std::nullopt;

    const int nCtx = static_cast<int>(llama_n_ctx(ctx_));
    if (static_cast<int>(tokens.size()) + opt.maxTokens > nCtx) {
        log::Info("LocalBrain: prompt trop long ({} tokens)", tokens.size());
        return std::nullopt;
    }

    // Longueur du prompt système (personnalité) : frontière de la photo d'état. Utile aux modèles récurrents
    // (leur cache ne se rembobine pas) et à tout modèle sur CPU (en jeu : préremplir 800 tokens = 15 s).
    size_t sysLen = 0;
    const bool useSnapshots = recurrent_ || !useGpu_;
    if (useSnapshots && !messages.empty() && messages.front().role == "system") {
        const llama_chat_message sys{messages.front().role.c_str(), messages.front().content.c_str()};
        const char* tmpl = llama_model_chat_template(model_, nullptr);
        std::string s(messages.front().content.size() * 2 + 256, '\0');
        int32_t k = llama_chat_apply_template(tmpl, &sys, 1, false, s.data(), static_cast<int32_t>(s.size()));
        if (k > static_cast<int32_t>(s.size())) { s.resize(static_cast<size_t>(k)); k = llama_chat_apply_template(tmpl, &sys, 1, false, s.data(), k); }
        if (k > 0) {
            s.resize(static_cast<size_t>(k));
            const int m = -llama_tokenize(vocab, s.c_str(), k, nullptr, 0, true, true);
            std::vector<llama_token> st(static_cast<size_t>(std::max(m, 0)));
            if (m > 0 && llama_tokenize(vocab, s.c_str(), k, st.data(), m, true, true) > 0 && st.size() < tokens.size() &&
                std::equal(st.begin(), st.end(), tokens.begin()))
                sysLen = st.size();
        }
    }

    // Réutilisation du cache KV : on garde le plus long préfixe commun avec l'appel précédent.
    size_t common = 0;
    while (common < cachedTokens_.size() && common < tokens.size() && cachedTokens_[common] == tokens[common]) ++common;
    if (common == tokens.size()) --common;  // il faut au moins un token à évaluer pour obtenir des logits
    llama_memory_t mem = llama_get_memory(ctx_);
    if (!llama_memory_seq_rm(mem, 0, static_cast<llama_pos>(common), -1)) {
        llama_memory_clear(mem, true);
        common = 0;
    }
    // Personnalité pas (ou plus) dans le cache : on repart de la photo prise juste après elle — en mémoire,
    // sinon sur le disque (d'une session à l'autre : même la 1re question d'une partie part tout de suite).
    if (sysLen > 0 && common < sysLen) {
        const std::vector<int> sysTokens(tokens.begin(), tokens.begin() + static_cast<std::ptrdiff_t>(sysLen));
        if (snapTokens_ != sysTokens) LoadSnapshot(sysTokens);
        if (snapTokens_ == sysTokens && !snapState_.empty()) {
            llama_memory_clear(mem, true);
            if (llama_state_seq_set_data(ctx_, snapState_.data(), snapState_.size(), 0) == snapState_.size()) {
                common = sysLen;
            } else {
                llama_memory_clear(mem, true);
                common = 0;
                snapState_.clear();
                snapTokens_.clear();
            }
        }
    }
    cachedTokens_.assign(tokens.begin(), tokens.begin() + static_cast<std::ptrdiff_t>(common));

    // En jeu (débit limité) : le préremplissage avance par morceaux d'un micro-lot, avec une courte pause
    // entre deux, pour ne jamais occuper la carte ou la mémoire plus de quelques millisecondes d'affilée.
    const size_t step = pacingTps_ > 0 ? static_cast<size_t>(llama_n_ubatch(ctx_)) : 512;
    for (size_t i = common; i < tokens.size();) {
        if (opt.stop.stop_requested()) return std::nullopt;
        if (pacingTps_ > 0 && i > common) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        // On s'arrête pile à la fin de la personnalité pour en prendre la photo (une seule fois par personnalité).
        size_t end = std::min(tokens.size(), i + step);
        const bool snapHere = sysLen > i && sysLen < end && (snapTokens_.size() != sysLen ||
                                                             !std::equal(snapTokens_.begin(), snapTokens_.end(), tokens.begin()));
        if (snapHere) end = sysLen;
        const int32_t chunk = static_cast<int32_t>(end - i);
        if (llama_decode(ctx_, llama_batch_get_one(tokens.data() + i, chunk)) != 0) {
            llama_memory_clear(mem, true);
            cachedTokens_.clear();
            return std::nullopt;
        }
        cachedTokens_.insert(cachedTokens_.end(), tokens.begin() + static_cast<std::ptrdiff_t>(i),
                             tokens.begin() + static_cast<std::ptrdiff_t>(end));
        i = end;
        if (snapHere || (sysLen > 0 && i == sysLen && snapTokens_.size() != sysLen)) {
            snapState_.resize(llama_state_seq_get_size(ctx_, 0));
            snapState_.resize(llama_state_seq_get_data(ctx_, snapState_.data(), snapState_.size(), 0));
            snapTokens_.assign(tokens.begin(), tokens.begin() + static_cast<std::ptrdiff_t>(sysLen));
            SaveSnapshot();
            log::Info("LocalBrain: photo d'état {} tokens, {} Ko", sysLen, snapState_.size() / 1024);
        }
    }

    return SampleLoop(opt, nCtx, /*trackTokens=*/true);
}


// Photo d'état sur disque : en-tête, tokens de la personnalité, état llama. Le nom porte une empreinte du
// modèle (nom, taille), du périphérique et des tokens : une personnalité ou un modèle changés = autre fichier.
std::filesystem::path LocalBrain::SnapshotFile(const std::vector<int>& sysTokens) const {
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&](const void* p, size_t n) {
        for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const unsigned char*>(p)[i]) * 1099511628211ull;
    };
    const auto name = cfg_.model.filename().u8string();
    mix(name.data(), name.size());
    std::error_code ec;
    const auto size = std::filesystem::file_size(cfg_.model, ec);
    mix(&size, sizeof(size));
    mix(&useGpu_, sizeof(useGpu_));
    mix(sysTokens.data(), sysTokens.size() * sizeof(int));
    char buf[40];
    std::snprintf(buf, sizeof(buf), "brain-%016llx.state", static_cast<unsigned long long>(h));
    return stateDir_ / buf;
}

void LocalBrain::SaveSnapshot() const {
    if (stateDir_.empty() || snapState_.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(stateDir_, ec);
    const auto file = SnapshotFile(snapTokens_);
    auto tmp = file;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        const std::uint32_t magic = 0x54534243u, version = 1;  // « CBST »
        const std::uint64_t nt = snapTokens_.size(), nb = snapState_.size();
        f.write(reinterpret_cast<const char*>(&magic), 4).write(reinterpret_cast<const char*>(&version), 4);
        f.write(reinterpret_cast<const char*>(&nt), 8).write(reinterpret_cast<const char*>(snapTokens_.data()), static_cast<std::streamsize>(nt * sizeof(int)));
        f.write(reinterpret_cast<const char*>(&nb), 8).write(reinterpret_cast<const char*>(snapState_.data()), static_cast<std::streamsize>(nb));
        if (!f) return;
    }
    std::filesystem::rename(tmp, file, ec);  // écriture atomique : jamais de fichier à moitié écrit
    PruneSnapshots();
}

// Les photos d'état s'accumulaient sans fin (une par consigne : le pilote met le titre de la fenêtre dans la sienne, donc
// presque une par appel, 30 à 70 Mo chacune). On garde les plus récemment servies : 24 fichiers et 1,5 Go au plus.
void LocalBrain::PruneSnapshots() const {
    std::error_code ec;
    struct F {
        std::filesystem::path p;
        std::filesystem::file_time_type t;
        std::uintmax_t size;
    };
    std::vector<F> files;
    for (const auto& e : std::filesystem::directory_iterator(stateDir_, ec))
        if (e.path().extension() == L".state" || e.path().extension() == L".tmp")
            files.push_back({e.path(), e.last_write_time(ec), e.file_size(ec)});
    std::sort(files.begin(), files.end(), [](const F& a, const F& b) { return a.t > b.t; });  // plus récent d'abord
    std::uintmax_t total = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        total += files[i].size;
        if (i >= 24 || total > 1'500'000'000ull) std::filesystem::remove(files[i].p, ec);
    }
}

void LocalBrain::LoadSnapshot(const std::vector<int>& sysTokens) {
    if (stateDir_.empty()) return;
    const auto file = SnapshotFile(sysTokens);
    std::ifstream f(file, std::ios::binary);
    if (!f) return;
    std::error_code tec;
    std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now(), tec);  // servie : gardée en priorité
    std::uint32_t magic = 0, version = 0;
    std::uint64_t nt = 0, nb = 0;
    f.read(reinterpret_cast<char*>(&magic), 4).read(reinterpret_cast<char*>(&version), 4).read(reinterpret_cast<char*>(&nt), 8);
    if (!f || magic != 0x54534243u || version != 1 || nt != sysTokens.size()) return;
    std::vector<int> toks(nt);
    f.read(reinterpret_cast<char*>(toks.data()), static_cast<std::streamsize>(nt * sizeof(int))).read(reinterpret_cast<char*>(&nb), 8);
    if (!f || toks != sysTokens || nb == 0 || nb > (1ull << 32)) return;
    std::vector<uint8_t> state(nb);
    f.read(reinterpret_cast<char*>(state.data()), static_cast<std::streamsize>(nb));
    if (!f) return;
    snapTokens_ = std::move(toks);
    snapState_ = std::move(state);
    log::Info("LocalBrain: photo d'état relue depuis le disque ({} Ko)", snapState_.size() / 1024);
}

std::string LocalBrain::SampleLoop(const GenerateOptions& opt, int nCtx, bool trackTokens) {
    const llama_vocab* vocab = llama_model_get_vocab(model_);
    // Échantillonneur : grammaire (optionnelle) -> pénalités -> top-k -> top-p -> température -> tirage.
    llama_sampler* smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    if (opt.grammar) {
        llama_sampler* g = llama_sampler_init_grammar(vocab, opt.grammar, "root");
        if (g) llama_sampler_chain_add(smpl, g);
        else log::Info("LocalBrain: grammaire GBNF invalide, ignorée");
    }
    llama_sampler_chain_add(smpl, llama_sampler_init_top_k(40));
    llama_sampler_chain_add(smpl, llama_sampler_init_penalties(llama_vocab_n_tokens(vocab), 64, 1.1f, 0.0f, 0.0f));
    llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.9f, 1));
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(opt.temperature));
    llama_sampler_chain_add(smpl, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

    std::string out;
    char piece[256];
    std::chrono::steady_clock::time_point next{};
    for (int i = 0; i < opt.maxTokens; ++i) {
        if (opt.stop.stop_requested()) break;
        Pace(next);
        llama_token tok = llama_sampler_sample(smpl, ctx_, -1);
        if (llama_vocab_is_eog(vocab, tok)) break;
        const int len = llama_token_to_piece(vocab, tok, piece, sizeof(piece), 0, false);
        if (len > 0) {
            out.append(piece, static_cast<size_t>(len));
            if (opt.onPiece && !opt.onPiece(std::string_view(piece, static_cast<size_t>(len)))) break;
        }
        if (trackTokens && static_cast<int>(cachedTokens_.size()) + 1 >= nCtx) break;
        if (llama_decode(ctx_, llama_batch_get_one(&tok, 1)) != 0) break;
        if (trackTokens) cachedTokens_.push_back(tok);
    }
    llama_sampler_free(smpl);
    return out;
}

// Au plus pacingTps_ tokens par seconde : on dort jusqu'au créneau suivant. Les 6 premiers tokens partent
// sans attendre (la 1re phrase arrive vite), ensuite le cerveau suit la voix au lieu de la devancer.
void LocalBrain::Pace(std::chrono::steady_clock::time_point& next) const {
    if (pacingTps_ <= 0) return;
    const auto now = std::chrono::steady_clock::now();
    const auto slot = std::chrono::microseconds(1'000'000 / pacingTps_);
    if (next == std::chrono::steady_clock::time_point{} || now - next > slot * 6) next = now - slot * 6;
    if (next > now) std::this_thread::sleep_until(next);
    next += slot;
}

bool LocalBrain::HasVision() const {
#if COMPANION_WITH_MTMD
    return !cfg_.mmproj.empty() && std::filesystem::exists(cfg_.mmproj) && std::filesystem::exists(cfg_.model);
#else
    return false;
#endif
}

const char* LocalBrain::MediaMarker() {
#if COMPANION_WITH_MTMD
    return mtmd_default_marker();
#else
    return "<__media__>";
#endif
}

std::optional<std::string> LocalBrain::GenerateWithImage(const std::vector<ChatTurn>& messages,
                                                         const unsigned char* rgb, int width, int height,
                                                         const GenerateOptions& opt) {
    const BusyScope busy(busy_);
#if COMPANION_WITH_MTMD
    if (!HasVision()) return std::nullopt;
    if (!IsLoaded() && !Load(cfg_)) return std::nullopt;
    if (!mtmd_) {
        mtmd_context_params mp = mtmd_context_params_default();
        mp.use_gpu = useGpu_;
        mp.n_threads = cfg_.threads;
        mp.print_timings = false;
        mp.warmup = false;
        mp.image_max_tokens = 768;  // borne le coût d'une capture 720p
        mtmd_ = mtmd_init_from_file(log::Narrow(cfg_.mmproj.wstring()).c_str(), model_, mp);
        if (!mtmd_ || !mtmd_support_vision(mtmd_)) {
            log::Info("LocalBrain: projecteur de vision invalide");
            if (mtmd_) mtmd_free(mtmd_);
            mtmd_ = nullptr;
            return std::nullopt;
        }
    }
    const std::string prompt = ApplyTemplate(messages);
    if (prompt.empty()) return std::nullopt;

    // L'image occupe des positions spéciales (M-RoPE…) : on repart d'un cache vide.
    llama_memory_clear(llama_get_memory(ctx_), true);
    cachedTokens_.clear();

    mtmd_bitmap* bitmap = mtmd_bitmap_init(static_cast<uint32_t>(width), static_cast<uint32_t>(height), rgb);
    mtmd_input_chunks* chunks = mtmd_input_chunks_init();
    const mtmd_input_text text{prompt.c_str(), prompt.size(), true, true};
    const mtmd_bitmap* bitmaps[] = {bitmap};
    std::optional<std::string> result;
    llama_pos nPast = 0;
    if (mtmd_tokenize(mtmd_, chunks, &text, bitmaps, 1) == 0 &&
        static_cast<int>(mtmd_helper_get_n_pos(chunks)) + opt.maxTokens < static_cast<int>(llama_n_ctx(ctx_)) &&
        mtmd_helper_eval_chunks(mtmd_, ctx_, chunks, 0, 0, static_cast<int32_t>(llama_n_batch(ctx_)), true, &nPast) == 0) {
        result = SampleLoop(opt, static_cast<int>(llama_n_ctx(ctx_)), /*trackTokens=*/false);
    } else {
        log::Info("LocalBrain: évaluation de l'image impossible");
    }
    mtmd_input_chunks_free(chunks);
    mtmd_bitmap_free(bitmap);
    llama_memory_clear(llama_get_memory(ctx_), true);  // le prochain Generate() repart proprement
    return result;
#else
    (void)messages; (void)rgb; (void)width; (void)height; (void)opt;
    return std::nullopt;
#endif
}

// Reconnaissance vocale avec un modèle « audio -> texte » (Qwen3-ASR) : contexte dans le message système (vocabulaire,
// phrase précédente), langue imposée en début de réponse (« language French<asr_text> »), décodage glouton.
std::optional<std::string> LocalBrain::TranscribeAudio(const std::vector<float>& pcm16k, const std::string& context,
                                                       const std::string& language, int maxTokens, std::string* detected) {
    const BusyScope busy(busy_);
#if COMPANION_WITH_MTMD
    if (cfg_.mmproj.empty() || !std::filesystem::exists(cfg_.mmproj) || pcm16k.size() < 1600) return std::nullopt;
    if (!IsLoaded() && !Load(cfg_)) return std::nullopt;
    if (!mtmd_) {
        mtmd_context_params mp = mtmd_context_params_default();
        mp.use_gpu = useGpu_;
        mp.n_threads = cfg_.threads;
        mp.print_timings = false;
        mp.warmup = false;
        mtmd_ = mtmd_init_from_file(log::Narrow(cfg_.mmproj.wstring()).c_str(), model_, mp);
        if (!mtmd_ || !mtmd_support_audio(mtmd_)) {
            log::Info("LocalBrain: encodeur audio invalide");
            if (mtmd_) mtmd_free(mtmd_);
            mtmd_ = nullptr;
            return std::nullopt;
        }
    }
    std::string prompt;
    if (!context.empty()) prompt += "<|im_start|>system\n" + context + "<|im_end|>\n";
    prompt += std::string("<|im_start|>user\n") + mtmd_default_marker() + "<|im_end|>\n<|im_start|>assistant\n";
    if (!language.empty()) prompt += "language " + language + "<asr_text>";
    llama_memory_clear(llama_get_memory(ctx_), true);
    cachedTokens_.clear();
    mtmd_bitmap* bitmap = mtmd_bitmap_init_from_audio(pcm16k.size(), pcm16k.data());
    mtmd_input_chunks* chunks = mtmd_input_chunks_init();
    const mtmd_input_text text{prompt.c_str(), prompt.size(), true, true};
    const mtmd_bitmap* bitmaps[] = {bitmap};
    std::optional<std::string> result;
    llama_pos nPast = 0;
    GenerateOptions opt;
    opt.maxTokens = maxTokens;
    opt.temperature = 0.f;
    if (mtmd_tokenize(mtmd_, chunks, &text, bitmaps, 1) == 0 &&
        static_cast<int>(mtmd_helper_get_n_pos(chunks)) + maxTokens < static_cast<int>(llama_n_ctx(ctx_)) &&
        mtmd_helper_eval_chunks(mtmd_, ctx_, chunks, 0, 0, static_cast<int32_t>(llama_n_batch(ctx_)), true, &nPast) == 0) {
        result = SampleLoop(opt, static_cast<int>(llama_n_ctx(ctx_)), /*trackTokens=*/false);
        // Sans langue imposée, le modèle écrit « language French<asr_text>… » : on garde le texte.
        if (result) {
            if (const auto p = result->find("<asr_text>"); p != std::string::npos) {
                if (detected && result->starts_with("language ")) *detected = result->substr(9, p - 9);
                result->erase(0, p + 10);
            } else if (detected && language.empty()) {
                *detected = "None";  // pas de « language X<asr_text> » : rien de reconnu comme de la parole
            }
            while (!result->empty() && (result->front() == ' ' || result->front() == '\n')) result->erase(0, 1);
        }
    } else {
        log::Info("LocalBrain: évaluation de l'audio impossible");
    }
    mtmd_input_chunks_free(chunks);
    mtmd_bitmap_free(bitmap);
    llama_memory_clear(llama_get_memory(ctx_), true);
    return result;
#else
    (void)pcm16k; (void)context; (void)language; (void)maxTokens; (void)detected;
    return std::nullopt;
#endif
}

#else  // !COMPANION_WITH_LLAMA : build sans llama.cpp, le niveau 1 est désactivé.

LocalBrain::LocalBrain() = default;
LocalBrain::~LocalBrain() = default;
bool LocalBrain::Available() const { return false; }
bool LocalBrain::Load(const LocalBrainConfig& cfg) { cfg_ = cfg; return false; }
void LocalBrain::Unload() {}
bool LocalBrain::Configure(bool useGpu, int threads) { useGpu_ = useGpu; cfg_.threads = threads; return true; }
std::string LocalBrain::ApplyTemplate(const std::vector<ChatTurn>&) const { return {}; }
std::optional<std::string> LocalBrain::Generate(const std::vector<ChatTurn>&, const GenerateOptions&) { return std::nullopt; }
std::string LocalBrain::SampleLoop(const GenerateOptions&, int, bool) { return {}; }
bool LocalBrain::HasVision() const { return false; }
const char* LocalBrain::MediaMarker() { return "<__media__>"; }
std::optional<std::string> LocalBrain::GenerateWithImage(const std::vector<ChatTurn>&, const unsigned char*, int, int,
                                                         const GenerateOptions&) { return std::nullopt; }
std::optional<std::string> LocalBrain::TranscribeAudio(const std::vector<float>&, const std::string&, const std::string&, int,
                                                       std::string*) {
    return std::nullopt;
}

#endif

}  // namespace companion
