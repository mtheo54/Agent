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

namespace companion {

#if COMPANION_WITH_LLAMA

namespace {
void QuietLog(ggml_log_level level, const char* text, void*) {
    if (level >= GGML_LOG_LEVEL_WARN) OutputDebugStringA(text);
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
    // load_mode par défaut = mmap : un rechargement GPU <-> CPU relit le cache disque de Windows.
    model_ = llama_model_load_from_file(log::Narrow(cfg.model.wstring()).c_str(), mp);
    if (!model_) {
        log::Info("LocalBrain: chargement du modèle impossible");
        return false;
    }
    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = static_cast<uint32_t>(cfg.contextTokens);
    const bool vision = !cfg.mmproj.empty();
    cp.n_batch = vision ? 1024 : 512;  // une image fait plusieurs centaines de tokens
    cp.n_ubatch = vision ? 1024 : 512;
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
}

bool LocalBrain::Configure(bool useGpu, int threads) {
    cfg_.threads = threads;
    if (useGpu != useGpu_) {
        useGpu_ = useGpu;
        if (IsLoaded()) return Load(cfg_);  // rechargement sur le bon périphérique
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

    // Réutilisation du cache KV : on garde le plus long préfixe commun avec l'appel précédent.
    size_t common = 0;
    while (common < cachedTokens_.size() && common < tokens.size() && cachedTokens_[common] == tokens[common]) ++common;
    if (common == tokens.size()) --common;  // il faut au moins un token à évaluer pour obtenir des logits
    llama_memory_t mem = llama_get_memory(ctx_);
    if (!llama_memory_seq_rm(mem, 0, static_cast<llama_pos>(common), -1)) {
        llama_memory_clear(mem, true);
        common = 0;
    }
    cachedTokens_.assign(tokens.begin(), tokens.begin() + static_cast<std::ptrdiff_t>(common));

    for (size_t i = common; i < tokens.size(); i += 512) {
        if (opt.stop.stop_requested()) return std::nullopt;
        const int32_t chunk = static_cast<int32_t>(std::min<size_t>(512, tokens.size() - i));
        if (llama_decode(ctx_, llama_batch_get_one(tokens.data() + i, chunk)) != 0) {
            llama_memory_clear(mem, true);
            cachedTokens_.clear();
            return std::nullopt;
        }
        cachedTokens_.insert(cachedTokens_.end(), tokens.begin() + static_cast<std::ptrdiff_t>(i),
                             tokens.begin() + static_cast<std::ptrdiff_t>(i + static_cast<size_t>(chunk)));
    }

    return SampleLoop(opt, nCtx, /*trackTokens=*/true);
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
    for (int i = 0; i < opt.maxTokens; ++i) {
        if (opt.stop.stop_requested()) break;
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

#endif

}  // namespace companion
