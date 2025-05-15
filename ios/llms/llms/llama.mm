#import "llama.h"
#import <Foundation/Foundation.h>

extern "C" {
    #include "llama.h"
}

// Обёртки для Swift
const char* llama_token_to_piece_wrapper(llama_context *ctx, llama_token token) {
    static std::vector<char> result(8, 0);
    const int n_tokens = llama_token_to_piece(ctx, token, result.data(), result.size());
    if (n_tokens < 0) {
        result.resize(-n_tokens);
        llama_token_to_piece(ctx, token, result.data(), result.size());
    } else {
        result.resize(n_tokens);
    }
    
    return result.data();
}

std::vector<llama_token> llama_tokenize_wrapper(llama_context *ctx, const std::string &text, bool add_bos) {
    std::vector<llama_token> tokens(text.size() + add_bos);
    int n_tokens = llama_tokenize(ctx, text.c_str(), tokens.data(), tokens.size(), add_bos);
    if (n_tokens < 0) {
        tokens.resize(-n_tokens);
        llama_tokenize(ctx, text.c_str(), tokens.data(), tokens.size(), add_bos);
    } else {
        tokens.resize(n_tokens);
    }
    return tokens;
}

llama_token llama_sample_token_wrapper(llama_context *ctx, llama_batch &batch, float temp) {
    llama_token new_token_id = 0;
    auto logits = llama_get_logits_ith(ctx, batch.n_tokens - 1);
    auto vocab_size = llama_n_vocab(llama_get_model(ctx));
    
    std::vector<llama_token_data> candidates;
    candidates.reserve(vocab_size);
    
    for (llama_token token_id = 0; token_id < vocab_size; token_id++) {
        candidates.emplace_back(llama_token_data{token_id, logits[token_id], 0.0f});
    }
    
    llama_token_data_array candidates_p = { candidates.data(), candidates.size(), false };
    
    // Температурная выборка
    llama_sample_temp(ctx, &candidates_p, temp);
    new_token_id = llama_sample_token(ctx, &candidates_p);
    
    return new_token_id;
}