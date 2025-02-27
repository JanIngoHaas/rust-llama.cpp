#include "common.h"
#include "llama.h"
#include "common/common.h"

#include "binding.h"

#include <cassert>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <regex>
#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__))
#include <signal.h>
#include <unistd.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <signal.h>
#endif

#if defined(__unix__) || (defined(__APPLE__) && defined(__MACH__)) || defined(_WIN32)
void sigint_handler(int signo)
{
    if (signo == SIGINT)
    {
        _exit(130);
    }
}
#endif

static std::string llama_token_to_str(const struct llama_context * ctx, llama_token token) {
    std::vector<char> result(8, 0);
    const int n_tokens = llama_token_to_piece(llama_get_model(ctx), token, result.data(), result.size());
    if (n_tokens < 0) {
        result.resize(-n_tokens);
        int check = llama_token_to_piece(llama_get_model(ctx), token, result.data(), result.size());
        GGML_ASSERT(check == -n_tokens);
    } else {
        result.resize(n_tokens);
    }

    return std::string(result.data(), result.size());
}


int get_embeddings(void *params_ptr, void *state_pr, float *res_embeddings)
{
    gpt_params *params_p = (gpt_params *)params_ptr;
    llama_context *ctx = (llama_context *)state_pr;
    gpt_params params = *params_p;

    if (params_p->seed <= 0)
    {
        params_p->seed = time(NULL);
    }

    std::mt19937 rng(params_p->seed);

    llama_backend_init(params_p->numa);

    int n_past = 0;

    // Add a space in front of the first character to match OG llama tokenizer behavior
    params_p->prompt.insert(0, 1, ' ');

    // tokenize the prompt
    auto embd_inp = ::llama_tokenize(ctx, params_p->prompt, true);

    // determine newline token
    auto llama_token_newline = ::llama_tokenize(ctx, "\n", false);

    if (embd_inp.size() > 0)
    {
        if (llama_eval(ctx, embd_inp.data(), embd_inp.size(), n_past))
        {
            fprintf(stderr, "%s : failed to eval\n", __func__);
            return 1;
        }
    }

    const llama_model *model = llama_get_model(ctx);

    const int n_embd = llama_n_embd(model);

    const auto embeddings = llama_get_embeddings(ctx);

    for (int i = 0; i < n_embd; i++)
    {
        res_embeddings[i] = embeddings[i];
    }

    return 0;
}

int get_token_embeddings(void *params_ptr, void *state_pr, int *tokens, int tokenSize, float *res_embeddings)
{
    gpt_params *params_p = (gpt_params *)params_ptr;
    llama_context *ctx = (llama_context *)state_pr;
    gpt_params params = *params_p;

    for (int i = 0; i < tokenSize; i++)
    {
        auto token_str = llama_token_to_str(ctx, tokens[i]);
        if (token_str.empty())
        {
            continue;
        }
        params_p->prompt += token_str;
    }

    return get_embeddings(params_ptr, state_pr, res_embeddings);
}

int eval(void *params_ptr, void *state_pr, char *text)
{
    gpt_params *params_p = (gpt_params *)params_ptr;
    llama_context *ctx = (llama_context *)state_pr;

    auto n_past = 0;
    auto last_n_tokens_data = std::vector<llama_token>(params_p->sparams.penalty_repeat, 0);

    auto tokens = std::vector<llama_token>(params_p->n_ctx);
    auto n_prompt_tokens = llama_tokenize(llama_get_model(ctx), text, strlen(text), tokens.data(), tokens.size(), true, false);

    if (n_prompt_tokens < 1)
    {
        fprintf(stderr, "%s : failed to tokenize prompt\n", __func__);
        return 1;
    }

    // evaluate prompt
    return llama_eval(ctx, tokens.data(), n_prompt_tokens, n_past);
}

int llama_predict(void *params_ptr, void *state_pr, char *result, bool debug)
{
    gpt_params *params_p = (gpt_params *)params_ptr;
    llama_context *ctx = (llama_context *)state_pr;
    
    llama_set_n_threads(ctx, params_p->n_threads, params_p->n_threads_batch);

    const int n_ctx = llama_n_ctx(ctx);

    if (params_p->seed <= 0)
    {
        params_p->seed = time(NULL);
    }

    std::mt19937 rng(params_p->seed);

    // print input
    if (debug)
    {
        fprintf(stderr, "%s: input: %s\n", __func__, params_p->prompt.c_str());
    }

    std::string path_session = params_p->path_prompt_cache;
    std::vector<llama_token> session_tokens;

    if (!path_session.empty())
    {
        if (debug)
        {
            fprintf(stderr, "%s: attempting to load saved session from '%s'\n", __func__, path_session.c_str());
        }
        // fopen to check for existing session
        FILE *fp = std::fopen(path_session.c_str(), "rb");
        if (fp != NULL)
        {
            std::fclose(fp);

            session_tokens.resize(n_ctx);
            size_t n_token_count_out = 0;
            if (!llama_load_session_file(ctx, path_session.c_str(), session_tokens.data(), session_tokens.capacity(), &n_token_count_out))
            {
                fprintf(stderr, "%s: error: failed to load session file '%s'\n", __func__, path_session.c_str());
                return 1;
            }
            session_tokens.resize(n_token_count_out);
            llama_set_rng_seed(ctx, params_p->seed);
            if (debug)
            {
                fprintf(stderr, "%s: loaded a session with prompt size of %d tokens\n", __func__, (int)session_tokens.size());
            }
        }
        else
        {
            if (debug)
            {
                fprintf(stderr, "%s: session file does not exist, will create\n", __func__);
            }
        }
    }

    std::vector<llama_token> embd_inp;
    if (!params_p->prompt.empty() || session_tokens.empty())
    {
        // Add a space in front of the first character to match OG llama tokenizer behavior
        params_p->prompt.insert(0, 1, ' ');

        embd_inp = ::llama_tokenize(ctx, params_p->prompt, true);
    }
    else
    {
        embd_inp = session_tokens;
    }

    // debug message about similarity of saved session, if applicable
    size_t n_matching_session_tokens = 0;
    if (session_tokens.size())
    {
        for (llama_token id : session_tokens)
        {
            if (n_matching_session_tokens >= embd_inp.size() || id != embd_inp[n_matching_session_tokens])
            {
                break;
            }
            n_matching_session_tokens++;
        }
        if (debug)
        {
            if (params_p->prompt.empty() && n_matching_session_tokens == embd_inp.size())
            {
                fprintf(stderr, "%s: using full prompt from session file\n", __func__);
            }
            else if (n_matching_session_tokens >= embd_inp.size())
            {
                fprintf(stderr, "%s: session file has exact match for prompt!\n", __func__);
            }
            else if (n_matching_session_tokens < (embd_inp.size() / 2))
            {
                fprintf(stderr, "%s: warning: session file has low similarity to prompt (%zu / %zu tokens); will mostly be reevaluated\n",
                        __func__, n_matching_session_tokens, embd_inp.size());
            }
            else
            {
                fprintf(stderr, "%s: session file matches %zu / %zu tokens of prompt\n",
                        __func__, n_matching_session_tokens, embd_inp.size());
            }
        }
    }
    // if we will use the cache for the full prompt without reaching the end of the cache, force
    // reevaluation of the last token token to recalculate the cached logits
    if (!embd_inp.empty() && n_matching_session_tokens == embd_inp.size() &&
        session_tokens.size() > embd_inp.size())
    {
        session_tokens.resize(embd_inp.size() - 1);
    }
    // number of tokens to keep when resetting context
    if (params_p->n_keep < 0 || params_p->n_keep > (int)embd_inp.size())
    {
        params_p->n_keep = (int)embd_inp.size();
    }

    // determine newline token
    auto llama_token_newline = ::llama_tokenize(ctx, "\n", false);

    // TODO: replace with ring-buffer
    std::vector<llama_token> last_n_tokens(n_ctx);
    std::fill(last_n_tokens.begin(), last_n_tokens.end(), 0);

    bool need_to_save_session = !path_session.empty() && n_matching_session_tokens < embd_inp.size();
    int n_past = 0;
    int n_remain = params_p->n_predict;
    int n_consumed = 0;
    int n_session_consumed = 0;

    std::vector<llama_token> embd;
    std::string res = "";

    // do one empty run to warm up the model
    {
        llama_token tmp[1] = {
            llama_token_bos(llama_get_model(ctx)),
        };
        llama_eval(ctx, tmp, 1, 0);
        llama_reset_timings(ctx);
    }

    while (n_remain != 0)
    {
        // predict
        if (embd.size() > 0)
        {
            // infinite text generation via context swapping
            // if we run out of context:
            // - take the n_keep first tokens from the original prompt (via n_past)
            // - take half of the last (n_ctx - n_keep) tokens and recompute the logits in batches
            if (n_past + (int)embd.size() > n_ctx)
            {
                const int n_left = n_past - params_p->n_keep;

                // always keep the first token - BOS
                n_past = std::max(1, params_p->n_keep);

                // insert n_left/2 tokens at the start of embd from last_n_tokens
                embd.insert(embd.begin(), last_n_tokens.begin() + n_ctx - n_left / 2 - embd.size(), last_n_tokens.end() - embd.size());

                // stop saving session if we run out of context
                path_session.clear();

                // printf("\n---\n");
                // printf("resetting: '");
                // for (int i = 0; i < (int) embd.size(); i++) {
                //     printf("%s", llama_token_to_str(ctx, embd[i]));
                // }
                // printf("'\n");
                // printf("\n---\n");
            }

            // try to reuse a matching prefix from the loaded session instead of re-eval (via n_past)
            if (n_session_consumed < (int)session_tokens.size())
            {
                size_t i = 0;
                for (; i < embd.size(); i++)
                {
                    if (embd[i] != session_tokens[n_session_consumed])
                    {
                        session_tokens.resize(n_session_consumed);
                        break;
                    }

                    n_past++;
                    n_session_consumed++;

                    if (n_session_consumed >= (int)session_tokens.size())
                    {
                        ++i;
                        break;
                    }
                }
                if (i > 0)
                {
                    embd.erase(embd.begin(), embd.begin() + i);
                }
            }

            // evaluate tokens in batches
            // embd is typically prepared beforehand to fit within a batch, but not always
            for (int i = 0; i < (int)embd.size(); i += params_p->n_batch)
            {
                int n_eval = (int)embd.size() - i;
                if (n_eval > params_p->n_batch)
                {
                    n_eval = params_p->n_batch;
                }
                if (llama_eval(ctx, &embd[i], n_eval, n_past))
                {
                    fprintf(stderr, "%s : failed to eval\n", __func__);
                    return 1;
                }
                n_past += n_eval;
            }

            if (embd.size() > 0 && !path_session.empty())
            {
                session_tokens.insert(session_tokens.end(), embd.begin(), embd.end());
                n_session_consumed = session_tokens.size();
            }
        }

        embd.clear();

        if ((int)embd_inp.size() <= n_consumed)
        {
            // out of user input, sample next token
            const float temp = params_p->sparams.temp;
            const int32_t top_k = params_p->sparams.top_k <= 0 ? llama_n_vocab(llama_get_model(ctx)) : params_p->sparams.top_k;
            const float top_p = params_p->sparams.top_p;
            const float tfs_z = params_p->sparams.tfs_z;
            const float typical_p = params_p->sparams.typical_p;
            const int32_t repeat_last_n = params_p->sparams.penalty_last_n < 0 ? n_ctx : params_p->sparams.penalty_last_n;
            const float repeat_penalty = params_p->sparams.penalty_repeat;
            const float alpha_presence = params_p->sparams.penalty_present;
            const float alpha_frequency = params_p->sparams.penalty_freq;
            const int mirostat = params_p->sparams.mirostat;
            const float mirostat_tau = params_p->sparams.mirostat_tau;
            const float mirostat_eta = params_p->sparams.mirostat_eta;
            const bool penalize_nl = params_p->sparams.penalize_nl;

            // optionally save the session on first sample (for faster prompt loading next time)
            if (!path_session.empty() && need_to_save_session && !params_p->prompt_cache_ro)
            {
                need_to_save_session = false;
                llama_save_session_file(ctx, path_session.c_str(), session_tokens.data(), session_tokens.size());
            }

            llama_token id = 0;

            {
                auto logits = llama_get_logits(ctx);
                auto n_vocab = llama_n_vocab(llama_get_model(ctx));

                // Apply params_p->logit_bias map
                for (auto it = params_p->sparams.logit_bias.begin(); it != params_p->sparams.logit_bias.end(); it++)
                {
                    logits[it->first] += it->second;
                }

                std::vector<llama_token_data> candidates;
                candidates.reserve(n_vocab);
                for (llama_token token_id = 0; token_id < n_vocab; token_id++)
                {
                    candidates.emplace_back(llama_token_data{token_id, logits[token_id], 0.0f});
                }

                llama_token_data_array candidates_p = {candidates.data(), candidates.size(), false};

                // Apply penalties
                float nl_logit = logits[llama_token_nl(llama_get_model(ctx))];
                auto last_n_repeat = std::min(std::min((int)last_n_tokens.size(), repeat_last_n), n_ctx);
                llama_sample_repetition_penalties(ctx, &candidates_p,
                                                last_n_tokens.data() + last_n_tokens.size() - last_n_repeat,
                                                last_n_repeat, repeat_penalty, alpha_frequency, alpha_presence);
                if (!penalize_nl)
                {
                    logits[llama_token_nl(llama_get_model(ctx))] = nl_logit;
                }

                if (temp <= 0)
                {
                    // Greedy sampling
                    id = llama_sample_token_greedy(ctx, &candidates_p);
                }
                else
                {
                    if (mirostat == 1)
                    {
                        static float mirostat_mu = 2.0f * mirostat_tau;
                        const int mirostat_m = 100;
                        llama_sample_temperature(ctx, &candidates_p, temp);
                        id = llama_sample_token_mirostat(ctx, &candidates_p, mirostat_tau, mirostat_eta, mirostat_m, &mirostat_mu);
                    }
                    else if (mirostat == 2)
                    {
                        static float mirostat_mu = 2.0f * mirostat_tau;
                        llama_sample_temperature(ctx, &candidates_p, temp);
                        id = llama_sample_token_mirostat_v2(ctx, &candidates_p, mirostat_tau, mirostat_eta, &mirostat_mu);
                    }
                    else
                    {
                        // Temperature sampling
                        llama_sample_top_k(ctx, &candidates_p, top_k, 1);
                        llama_sample_tail_free(ctx, &candidates_p, tfs_z, 1);
                        llama_sample_typical(ctx, &candidates_p, typical_p, 1);
                        llama_sample_top_p(ctx, &candidates_p, top_p, 1);
                        llama_sample_temperature(ctx, &candidates_p, temp);
                        id = llama_sample_token(ctx, &candidates_p);
                    }
                }
                // printf("`%d`", candidates_p.size);

                last_n_tokens.erase(last_n_tokens.begin());
                last_n_tokens.push_back(id);
            }

            // add it to the context
            embd.push_back(id);

            // decrement remaining sampling budget
            --n_remain;

            // call the token callback, no need to check if one is actually registered, that will
            // be handled on the Go side.
            auto token_str = llama_token_to_str(ctx, id);
            if (!tokenCallback(state_pr, (char*)token_str.c_str()))
            {
                break;
            }
        }
        else
        {
            // some user input remains from prompt or interaction, forward it to processing
            while ((int)embd_inp.size() > n_consumed)
            {
                embd.push_back(embd_inp[n_consumed]);
                last_n_tokens.erase(last_n_tokens.begin());
                last_n_tokens.push_back(embd_inp[n_consumed]);
                ++n_consumed;
                if ((int)embd.size() >= params_p->n_batch)
                {
                    break;
                }
            }
        }

        for (auto id : embd)
        {
            res += llama_token_to_str(ctx, id);
        }

        // check for stop prompt
        if (params_p->antiprompt.size())
        {
            std::string last_output;
            for (auto id : last_n_tokens)
            {
                last_output += llama_token_to_str(ctx, id);
            }
            // Check if each of the reverse prompts appears at the end of the output.
            for (std::string &antiprompt : params_p->antiprompt)
            {
                // size_t extra_padding = params_p->interactive ? 0 : 2;
                size_t extra_padding = 2;
                size_t search_start_pos = last_output.length() > static_cast<size_t>(antiprompt.length() + extra_padding)
                                              ? last_output.length() - static_cast<size_t>(antiprompt.length() + extra_padding)
                                              : 0;

                if (last_output.find(antiprompt.c_str(), search_start_pos) != std::string::npos)
                {
                    goto end;
                }
            }
        }

        // end of text token
        if (!embd.empty() && embd.back() == llama_token_eos(llama_get_model(ctx)))
        {
            break;
        }
    }

    if (!path_session.empty() && params_p->prompt_cache_all && !params_p->prompt_cache_ro)
    {
        if (debug)
        {
            fprintf(stderr, "\n%s: saving final output to session file '%s'\n", __func__, path_session.c_str());
        }
        llama_save_session_file(ctx, path_session.c_str(), session_tokens.data(), session_tokens.size());
    }

end:
#if defined(_WIN32)
    signal(SIGINT, SIG_DFL);
#endif

    if (debug)
    {
        llama_print_timings(ctx);
        llama_reset_timings(ctx);
    }

    strcpy(result, res.c_str());
    return 0;
}

void llama_binding_free_model(void *state_ptr)
{
    llama_context *ctx = (llama_context *)state_ptr;
    llama_free(ctx);
}

void llama_free_params(void *params_ptr)
{
    gpt_params *params = (gpt_params *)params_ptr;
    delete params;
}

std::vector<std::string> create_vector(const char **strings, int count)
{
    std::vector<std::string> *vec = new std::vector<std::string>;
    for (int i = 0; i < count; i++)
    {
        vec->push_back(std::string(strings[i]));
    }
    return *vec;
}

void delete_vector(std::vector<std::string> *vec)
{
    delete vec;
}

int load_state(void *ctx, char *statefile, char *modes)
{
    llama_context *state = (llama_context *)ctx;
    const llama_context *constState = static_cast<const llama_context *>(state);
    const size_t state_size = llama_get_state_size(state);
    uint8_t *state_mem = new uint8_t[state_size];

    {
        FILE *fp_read = fopen(statefile, modes);
        if (state_size != llama_get_state_size(constState))
        {
            fprintf(stderr, "\n%s : failed to validate state size\n", __func__);
            return 1;
        }

        const size_t ret = fread(state_mem, 1, state_size, fp_read);
        if (ret != state_size)
        {
            fprintf(stderr, "\n%s : failed to read state\n", __func__);
            return 1;
        }

        llama_set_state_data(state, state_mem); // could also read directly from memory mapped file
        fclose(fp_read);
    }

    return 0;
}

void save_state(void *ctx, char *dst, char *modes)
{
    llama_context *state = (llama_context *)ctx;

    const size_t state_size = llama_get_state_size(state);
    uint8_t *state_mem = new uint8_t[state_size];

    // Save state (rng, logits, embedding and kv_cache) to file
    {
        FILE *fp_write = fopen(dst, modes);
        llama_copy_state_data(state, state_mem); // could also copy directly to memory mapped file
        fwrite(state_mem, 1, state_size, fp_write);
        fclose(fp_write);
    }
}

void *llama_allocate_params(const char *prompt, int seed, int threads, int tokens, int top_k,
                            float top_p, float temp, float repeat_penalty, int repeat_last_n, bool ignore_eos, bool memory_f16, int n_batch, int n_keep, const char **antiprompt, int antiprompt_count,
                            float tfs_z, float typical_p, float frequency_penalty, float presence_penalty, int mirostat, float mirostat_eta, float mirostat_tau, bool penalize_nl, const char *logit_bias, const char *session_file, bool prompt_cache_all, bool mlock, bool mmap,
                            const char *maingpu, const char *tensorsplit, bool prompt_cache_ro)
{
    gpt_params *params = new gpt_params;
    params->seed = seed;
    params->n_threads = threads;
    params->n_threads_batch = threads;
    params->n_predict = tokens;
    params->sparams.penalty_last_n = repeat_last_n;
    params->prompt_cache_ro = prompt_cache_ro;
    params->sparams.top_k = top_k;
    params->sparams.top_p = top_p;
    params->memory_f16 = memory_f16;
    params->sparams.temp = temp;
    params->use_mmap = mmap;
    params->use_mlock = mlock;
    params->sparams.penalty_repeat = repeat_penalty;
    params->n_batch = n_batch;
    params->n_keep = n_keep;
    if (maingpu[0] != '\0')
    {
        params->main_gpu = std::stoi(maingpu);
    }

    if (tensorsplit[0] != '\0')
    {
        std::string arg_next = tensorsplit;
        // split string by , and /
        const std::regex regex{R"([,/]+)"};
        std::sregex_token_iterator it{arg_next.begin(), arg_next.end(), regex, -1};
        std::vector<std::string> split_arg{it, {}};
        GGML_ASSERT(split_arg.size() <= LLAMA_MAX_DEVICES);

        for (size_t i = 0; i < LLAMA_MAX_DEVICES; ++i)
        {
            if (i < split_arg.size())
            {
                params->tensor_split[i] = std::stof(split_arg[i]);
            }
            else
            {
                params->tensor_split[i] = 0.0f;
            }
        }
    }

    params->prompt_cache_all = prompt_cache_all;
    params->path_prompt_cache = session_file;

    /* if (ignore_eos) // TODO: Cannot be set before context is allocated (llama_token_eos requires context access)
    {
        params->logit_bias[llama_token_eos()] = -INFINITY;
	}
    */
    if (antiprompt_count > 0)
    {
        params->antiprompt = create_vector(antiprompt, antiprompt_count);
    }
    params->sparams.tfs_z = tfs_z;
    params->sparams.typical_p = typical_p;
    params->sparams.penalty_present = presence_penalty;
    params->sparams.mirostat = mirostat;
    params->sparams.mirostat_eta = mirostat_eta;
    params->sparams.mirostat_tau = mirostat_tau;
    params->sparams.penalize_nl = penalize_nl;
    std::stringstream ss(logit_bias);
    llama_token key;
    char sign;
    std::string value_str;
    if (ss >> key && ss >> sign && std::getline(ss, value_str) && (sign == '+' || sign == '-'))
    {
        params->sparams.logit_bias[key] = std::stof(value_str) * ((sign == '-') ? -1.0f : 1.0f);
    }
    params->sparams.penalty_freq = frequency_penalty;
    params->prompt = prompt;

    return params;
}

void *load_model(const char *fname, int n_ctx, int n_seed, bool memory_f16, bool mlock, bool embeddings, bool mmap, bool low_vram, bool vocab_only, int n_gpu_layers, int n_batch, const char *maingpu, const char *tensorsplit, bool numa)
{
    // load the model
    auto lparams = llama_context_default_params();
    auto mparams = llama_model_default_params();

    lparams.n_ctx = n_ctx;
    lparams.seed = n_seed;
    lparams.f16_kv = memory_f16;
    lparams.embedding = embeddings;
    mparams.use_mlock = mlock;
    mparams.n_gpu_layers = n_gpu_layers;
    mparams.use_mmap = mmap;
    // mparams.low_vram = low_vram; LOW_VRAM not a thing anymore in the API? verify
    mparams.vocab_only = vocab_only;

    if (maingpu[0] != '\0')
    {
        mparams.main_gpu = std::stoi(maingpu);
    }

    if (tensorsplit[0] != '\0')
    {
        std::string arg_next = tensorsplit;
        // split string by , and /
        const std::regex regex{R"([,/]+)"};
        std::sregex_token_iterator it{arg_next.begin(), arg_next.end(), regex, -1};
        std::vector<std::string> split_arg{it, {}};
        GGML_ASSERT(split_arg.size() <= LLAMA_MAX_DEVICES);

	float *tsplit = (float*)malloc(sizeof(float) * LLAMA_MAX_DEVICES);

        for (size_t i = 0; i < LLAMA_MAX_DEVICES; ++i)
        {
            if (i < split_arg.size())
            {
                tsplit[i] = std::stof(split_arg[i]);
            }
            else
            {
                tsplit[i] = 0.0f;
            }
        }
	mparams.tensor_split = tsplit;
    }

    if (n_batch > 0)
        lparams.n_batch = n_batch;

    llama_backend_init(numa);
    void *res = nullptr;
    try
    {
        auto model = llama_load_model_from_file(fname, mparams);
	res = llama_new_context_with_model(model, lparams);
    }
    catch (std::runtime_error &e)
    {
        fprintf(stderr, "failed %s", e.what());
        return res;
    }

    return res;
}
