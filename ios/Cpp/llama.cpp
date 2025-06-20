#include "llama-impl.h"
#include "llama-vocab.h"
#include "llama-sampling.h"

#include "unicode.h"

#include "ggml.h"
#include "ggml-alloc.h"
#include "ggml-backend.h"
#include "ggml-cpp.h"

// TODO: replace with ggml API call
#define QK_K 256

#ifdef __has_include
    #if __has_include(<unistd.h>)
        #include <unistd.h>
        #if defined(_POSIX_MAPPED_FILES)
            #include <sys/mman.h>
            #include <fcntl.h>
        #endif
        #if defined(_POSIX_MEMLOCK_RANGE)
            #include <sys/resource.h>
        #endif
    #endif
#endif

#if defined(_WIN32)
    #define WIN32_LEAN_AND_MEAN
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>
    #ifndef PATH_MAX
        #define PATH_MAX MAX_PATH
    #endif
    #include <io.h>
#endif

#if __cplusplus >= 202000L
    #define LU8(x) (const char*)(u8##x)
#else
    #define LU8(x) u8##x
#endif

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cfloat>
#include <cinttypes>
#include <climits>
#include <cmath>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <future>
#include <initializer_list>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <numeric>
#include <set>
#include <sstream>
#include <thread>
#include <type_traits>
#include <unordered_map>

#if defined(_MSC_VER)
#pragma warning(disable: 4244 4267) // possible loss of data
#endif

// bump if necessary
#define LLAMA_MAX_LAYERS  512
#define LLAMA_MAX_EXPERTS 160  // DeepSeekV2

#if defined(__ANDROID__) && defined(DLOGGING_ENABLED)
#include <android/log.h>
#define LLAMA_ANDROID_TAG "RNLLAMA_LOG_ANDROID"
#undef LLAMA_LOG_INFO
#undef LLAMA_LOG_WARN
#undef LLAMA_LOG_ERROR
#define LLAMA_LOG_INFO(...)  __android_log_print(ANDROID_LOG_INFO , LLAMA_ANDROID_TAG, __VA_ARGS__)
#define LLAMA_LOG_WARN(...)  __android_log_print(ANDROID_LOG_WARN , LLAMA_ANDROID_TAG, __VA_ARGS__)
#define LLAMA_LOG_ERROR(...) __android_log_print(ANDROID_LOG_ERROR, LLAMA_ANDROID_TAG, __VA_ARGS__)
#endif // __ANDROID__

//
// helpers
//

// trim whitespace from the beginning and end of a string
static std::string trim(const std::string & str) {
    size_t start = 0;
    size_t end = str.size();
    while (start < end && isspace(str[start])) {
        start += 1;
    }
    while (end > start && isspace(str[end - 1])) {
        end -= 1;
    }
    return str.substr(start, end - start);
}

static bool is_float_close(float a, float b, float abs_tol) {
    // Check for non-negative tolerance
    if (abs_tol < 0.0) {
        throw std::invalid_argument("Tolerance must be non-negative");
    }

    // Exact equality check
    if (a == b) {
        return true;
    }

    // Check for infinities
    if (std::isinf(a) || std::isinf(b)) {
        return false;
    }

    // Regular comparison using the provided absolute tolerance
    return std::fabs(b - a) <= abs_tol;
}

static void zeros(std::ofstream & file, size_t n) {
    char zero = 0;
    for (size_t i = 0; i < n; ++i) {
        file.write(&zero, 1);
    }
}

LLAMA_ATTRIBUTE_FORMAT(1, 2)
static std::string format(const char * fmt, ...) {
    va_list ap;
    va_list ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int size = vsnprintf(NULL, 0, fmt, ap);
    LM_GGML_ASSERT(size >= 0 && size < INT_MAX); // NOLINT
    std::vector<char> buf(size + 1);
    int size2 = vsnprintf(buf.data(), size + 1, fmt, ap2);
    LM_GGML_ASSERT(size2 == size);
    va_end(ap2);
    va_end(ap);
    return std::string(buf.data(), size);
}

//
// gguf constants (sync with gguf.py)
//

enum llm_arch {
    LLM_ARCH_LLAMA,
    LLM_ARCH_FALCON,
    LLM_ARCH_BAICHUAN,
    LLM_ARCH_GROK,
    LLM_ARCH_GPT2,
    LLM_ARCH_GPTJ,
    LLM_ARCH_GPTNEOX,
    LLM_ARCH_MPT,
    LLM_ARCH_STARCODER,
    LLM_ARCH_REFACT,
    LLM_ARCH_BERT,
    LLM_ARCH_NOMIC_BERT,
    LLM_ARCH_JINA_BERT_V2,
    LLM_ARCH_BLOOM,
    LLM_ARCH_STABLELM,
    LLM_ARCH_QWEN,
    LLM_ARCH_QWEN2,
    LLM_ARCH_QWEN2MOE,
    LLM_ARCH_PHI2,
    LLM_ARCH_PHI3,
    LLM_ARCH_PLAMO,
    LLM_ARCH_CODESHELL,
    LLM_ARCH_ORION,
    LLM_ARCH_INTERNLM2,
    LLM_ARCH_MINICPM,
    LLM_ARCH_MINICPM3,
    LLM_ARCH_GEMMA,
    LLM_ARCH_GEMMA2,
    LLM_ARCH_STARCODER2,
    LLM_ARCH_MAMBA,
    LLM_ARCH_XVERSE,
    LLM_ARCH_COMMAND_R,
    LLM_ARCH_DBRX,
    LLM_ARCH_OLMO,
    LLM_ARCH_OLMO_1124,
    LLM_ARCH_OLMOE,
    LLM_ARCH_OPENELM,
    LLM_ARCH_ARCTIC,
    LLM_ARCH_DEEPSEEK2,
    LLM_ARCH_CHATGLM,
    LLM_ARCH_BITNET,
    LLM_ARCH_T5,
    LLM_ARCH_T5ENCODER,
    LLM_ARCH_JAIS,
    LLM_ARCH_NEMOTRON,
    LLM_ARCH_EXAONE,
    LLM_ARCH_RWKV6,
    LLM_ARCH_GRANITE,
    LLM_ARCH_GRANITE_MOE,
    LLM_ARCH_CHAMELEON,
    LLM_ARCH_UNKNOWN,
};

static const std::map<llm_arch, const char *> LLM_ARCH_NAMES = {
    { LLM_ARCH_LLAMA,           "llama"        },
    { LLM_ARCH_FALCON,          "falcon"       },
    { LLM_ARCH_GROK,            "grok"         },
    { LLM_ARCH_GPT2,            "gpt2"         },
    { LLM_ARCH_GPTJ,            "gptj"         },
    { LLM_ARCH_GPTNEOX,         "gptneox"      },
    { LLM_ARCH_MPT,             "mpt"          },
    { LLM_ARCH_BAICHUAN,        "baichuan"     },
    { LLM_ARCH_STARCODER,       "starcoder"    },
    { LLM_ARCH_REFACT,          "refact"       },
    { LLM_ARCH_BERT,            "bert"         },
    { LLM_ARCH_NOMIC_BERT,      "nomic-bert"   },
    { LLM_ARCH_JINA_BERT_V2,    "jina-bert-v2" },
    { LLM_ARCH_BLOOM,           "bloom"        },
    { LLM_ARCH_STABLELM,        "stablelm"     },
    { LLM_ARCH_QWEN,            "qwen"         },
    { LLM_ARCH_QWEN2,           "qwen2"        },
    { LLM_ARCH_QWEN2MOE,        "qwen2moe"     },
    { LLM_ARCH_PHI2,            "phi2"         },
    { LLM_ARCH_PHI3,            "phi3"         },
    { LLM_ARCH_PLAMO,           "plamo"        },
    { LLM_ARCH_CODESHELL,       "codeshell"    },
    { LLM_ARCH_ORION,           "orion"        },
    { LLM_ARCH_INTERNLM2,       "internlm2"    },
    { LLM_ARCH_MINICPM,         "minicpm"      },
    { LLM_ARCH_MINICPM3,        "minicpm3"     },
    { LLM_ARCH_GEMMA,           "gemma"        },
    { LLM_ARCH_GEMMA2,          "gemma2"       },
    { LLM_ARCH_STARCODER2,      "starcoder2"   },
    { LLM_ARCH_MAMBA,           "mamba"        },
    { LLM_ARCH_XVERSE,          "xverse"       },
    { LLM_ARCH_COMMAND_R,       "command-r"    },
    { LLM_ARCH_DBRX,            "dbrx"         },
    { LLM_ARCH_OLMO,            "olmo"         },
    { LLM_ARCH_OLMO_1124,       "olmo_1124"    },
    { LLM_ARCH_OLMOE,           "olmoe"        },
    { LLM_ARCH_OPENELM,         "openelm"      },
    { LLM_ARCH_ARCTIC,          "arctic"       },
    { LLM_ARCH_DEEPSEEK2,       "deepseek2"    },
    { LLM_ARCH_CHATGLM,         "chatglm"      },
    { LLM_ARCH_BITNET,          "bitnet"       },
    { LLM_ARCH_T5,              "t5"           },
    { LLM_ARCH_T5ENCODER,       "t5encoder"    },
    { LLM_ARCH_JAIS,            "jais"         },
    { LLM_ARCH_NEMOTRON,        "nemotron"     },
    { LLM_ARCH_EXAONE,          "exaone"       },
    { LLM_ARCH_RWKV6,           "rwkv6"        },
    { LLM_ARCH_GRANITE,         "granite"      },
    { LLM_ARCH_GRANITE_MOE,     "granitemoe"   },
    { LLM_ARCH_CHAMELEON,       "chameleon"    },
    { LLM_ARCH_UNKNOWN,         "(unknown)"    },
};

enum llm_kv {
    LLM_KV_GENERAL_TYPE,
    LLM_KV_GENERAL_ARCHITECTURE,
    LLM_KV_GENERAL_QUANTIZATION_VERSION,
    LLM_KV_GENERAL_ALIGNMENT,
    LLM_KV_GENERAL_NAME,
    LLM_KV_GENERAL_AUTHOR,
    LLM_KV_GENERAL_VERSION,
    LLM_KV_GENERAL_URL,
    LLM_KV_GENERAL_DESCRIPTION,
    LLM_KV_GENERAL_LICENSE,
    LLM_KV_GENERAL_SOURCE_URL,
    LLM_KV_GENERAL_SOURCE_HF_REPO,

    LLM_KV_VOCAB_SIZE,
    LLM_KV_CONTEXT_LENGTH,
    LLM_KV_EMBEDDING_LENGTH,
    LLM_KV_BLOCK_COUNT,
    LLM_KV_LEADING_DENSE_BLOCK_COUNT,
    LLM_KV_FEED_FORWARD_LENGTH,
    LLM_KV_EXPERT_FEED_FORWARD_LENGTH,
    LLM_KV_EXPERT_SHARED_FEED_FORWARD_LENGTH,
    LLM_KV_USE_PARALLEL_RESIDUAL,
    LLM_KV_TENSOR_DATA_LAYOUT,
    LLM_KV_EXPERT_COUNT,
    LLM_KV_EXPERT_USED_COUNT,
    LLM_KV_EXPERT_SHARED_COUNT,
    LLM_KV_EXPERT_WEIGHTS_SCALE,
    LLM_KV_POOLING_TYPE,
    LLM_KV_LOGIT_SCALE,
    LLM_KV_DECODER_START_TOKEN_ID,
    LLM_KV_ATTN_LOGIT_SOFTCAPPING,
    LLM_KV_FINAL_LOGIT_SOFTCAPPING,
    LLM_KV_SWIN_NORM,
    LLM_KV_RESCALE_EVERY_N_LAYERS,
    LLM_KV_TIME_MIX_EXTRA_DIM,
    LLM_KV_TIME_DECAY_EXTRA_DIM,
    LLM_KV_RESIDUAL_SCALE,
    LLM_KV_EMBEDDING_SCALE,

    LLM_KV_ATTENTION_HEAD_COUNT,
    LLM_KV_ATTENTION_HEAD_COUNT_KV,
    LLM_KV_ATTENTION_MAX_ALIBI_BIAS,
    LLM_KV_ATTENTION_CLAMP_KQV,
    LLM_KV_ATTENTION_KEY_LENGTH,
    LLM_KV_ATTENTION_VALUE_LENGTH,
    LLM_KV_ATTENTION_LAYERNORM_EPS,
    LLM_KV_ATTENTION_LAYERNORM_RMS_EPS,
    LLM_KV_ATTENTION_CAUSAL,
    LLM_KV_ATTENTION_Q_LORA_RANK,
    LLM_KV_ATTENTION_KV_LORA_RANK,
    LLM_KV_ATTENTION_RELATIVE_BUCKETS_COUNT,
    LLM_KV_ATTENTION_SLIDING_WINDOW,
    LLM_KV_ATTENTION_SCALE,

    LLM_KV_ROPE_DIMENSION_COUNT,
    LLM_KV_ROPE_FREQ_BASE,
    LLM_KV_ROPE_SCALE_LINEAR,
    LLM_KV_ROPE_SCALING_TYPE,
    LLM_KV_ROPE_SCALING_FACTOR,
    LLM_KV_ROPE_SCALING_ATTN_FACTOR,
    LLM_KV_ROPE_SCALING_ORIG_CTX_LEN,
    LLM_KV_ROPE_SCALING_FINETUNED,
    LLM_KV_ROPE_SCALING_YARN_LOG_MUL,

    LLM_KV_SPLIT_NO,
    LLM_KV_SPLIT_COUNT,
    LLM_KV_SPLIT_TENSORS_COUNT,

    LLM_KV_SSM_INNER_SIZE,
    LLM_KV_SSM_CONV_KERNEL,
    LLM_KV_SSM_STATE_SIZE,
    LLM_KV_SSM_TIME_STEP_RANK,
    LLM_KV_SSM_DT_B_C_RMS,

    LLM_KV_WKV_HEAD_SIZE,

    LLM_KV_TOKENIZER_MODEL,
    LLM_KV_TOKENIZER_PRE,
    LLM_KV_TOKENIZER_LIST,
    LLM_KV_TOKENIZER_TOKEN_TYPE,
    LLM_KV_TOKENIZER_TOKEN_TYPE_COUNT,
    LLM_KV_TOKENIZER_SCORES,
    LLM_KV_TOKENIZER_MERGES,
    LLM_KV_TOKENIZER_BOS_ID,
    LLM_KV_TOKENIZER_EOS_ID,
    LLM_KV_TOKENIZER_EOT_ID,
    LLM_KV_TOKENIZER_EOM_ID,
    LLM_KV_TOKENIZER_UNK_ID,
    LLM_KV_TOKENIZER_SEP_ID,
    LLM_KV_TOKENIZER_PAD_ID,
    LLM_KV_TOKENIZER_CLS_ID,
    LLM_KV_TOKENIZER_MASK_ID,
    LLM_KV_TOKENIZER_ADD_BOS,
    LLM_KV_TOKENIZER_ADD_EOS,
    LLM_KV_TOKENIZER_ADD_PREFIX,
    LLM_KV_TOKENIZER_REMOVE_EXTRA_WS,
    LLM_KV_TOKENIZER_PRECOMPILED_CHARSMAP,
    LLM_KV_TOKENIZER_HF_JSON,
    LLM_KV_TOKENIZER_RWKV,
    LLM_KV_TOKENIZER_FIM_PRE_ID,
    LLM_KV_TOKENIZER_FIM_SUF_ID,
    LLM_KV_TOKENIZER_FIM_MID_ID,
    LLM_KV_TOKENIZER_FIM_PAD_ID,
    LLM_KV_TOKENIZER_FIM_REP_ID,
    LLM_KV_TOKENIZER_FIM_SEP_ID,

    LLM_KV_ADAPTER_TYPE,
    LLM_KV_ADAPTER_LORA_ALPHA,

    // deprecated:
    LLM_KV_TOKENIZER_PREFIX_ID,
    LLM_KV_TOKENIZER_SUFFIX_ID,
    LLM_KV_TOKENIZER_MIDDLE_ID,
};

static const std::map<llm_kv, const char *> LLM_KV_NAMES = {
    { LLM_KV_GENERAL_TYPE,                  "general.type"                          },
    { LLM_KV_GENERAL_ARCHITECTURE,          "general.architecture"                  },
    { LLM_KV_GENERAL_QUANTIZATION_VERSION,  "general.quantization_version"          },
    { LLM_KV_GENERAL_ALIGNMENT,             "general.alignment"                     },
    { LLM_KV_GENERAL_NAME,                  "general.name"                          },
    { LLM_KV_GENERAL_AUTHOR,                "general.author"                        },
    { LLM_KV_GENERAL_VERSION,               "general.version"                       },
    { LLM_KV_GENERAL_URL,                   "general.url"                           },
    { LLM_KV_GENERAL_DESCRIPTION,           "general.description"                   },
    { LLM_KV_GENERAL_LICENSE,               "general.license"                       },
    { LLM_KV_GENERAL_SOURCE_URL,            "general.source.url"                    },
    { LLM_KV_GENERAL_SOURCE_HF_REPO,        "general.source.huggingface.repository" },

    { LLM_KV_VOCAB_SIZE,                        "%s.vocab_size"                        },
    { LLM_KV_CONTEXT_LENGTH,                    "%s.context_length"                    },
    { LLM_KV_EMBEDDING_LENGTH,                  "%s.embedding_length"                  },
    { LLM_KV_BLOCK_COUNT,                       "%s.block_count"                       },
    { LLM_KV_LEADING_DENSE_BLOCK_COUNT,         "%s.leading_dense_block_count"         },
    { LLM_KV_FEED_FORWARD_LENGTH,               "%s.feed_forward_length"               },
    { LLM_KV_EXPERT_FEED_FORWARD_LENGTH,        "%s.expert_feed_forward_length"        },
    { LLM_KV_EXPERT_SHARED_FEED_FORWARD_LENGTH, "%s.expert_shared_feed_forward_length" },
    { LLM_KV_USE_PARALLEL_RESIDUAL,             "%s.use_parallel_residual"             },
    { LLM_KV_TENSOR_DATA_LAYOUT,                "%s.tensor_data_layout"                },
    { LLM_KV_EXPERT_COUNT,                      "%s.expert_count"                      },
    { LLM_KV_EXPERT_USED_COUNT,                 "%s.expert_used_count"                 },
    { LLM_KV_EXPERT_SHARED_COUNT,               "%s.expert_shared_count"               },
    { LLM_KV_EXPERT_WEIGHTS_SCALE,              "%s.expert_weights_scale"              },
    { LLM_KV_POOLING_TYPE,                      "%s.pooling_type"                      },
    { LLM_KV_LOGIT_SCALE,                       "%s.logit_scale"                       },
    { LLM_KV_DECODER_START_TOKEN_ID,            "%s.decoder_start_token_id"            },
    { LLM_KV_ATTN_LOGIT_SOFTCAPPING,            "%s.attn_logit_softcapping"            },
    { LLM_KV_FINAL_LOGIT_SOFTCAPPING,           "%s.final_logit_softcapping"           },
    { LLM_KV_SWIN_NORM,                         "%s.swin_norm"                         },
    { LLM_KV_RESCALE_EVERY_N_LAYERS,            "%s.rescale_every_n_layers"            },
    { LLM_KV_TIME_MIX_EXTRA_DIM,                "%s.time_mix_extra_dim"                },
    { LLM_KV_TIME_DECAY_EXTRA_DIM,              "%s.time_decay_extra_dim"              },
    { LLM_KV_RESIDUAL_SCALE,                    "%s.residual_scale"                    },
    { LLM_KV_EMBEDDING_SCALE,                   "%s.embedding_scale"                   },

    { LLM_KV_ATTENTION_HEAD_COUNT,             "%s.attention.head_count"             },
    { LLM_KV_ATTENTION_HEAD_COUNT_KV,          "%s.attention.head_count_kv"          },
    { LLM_KV_ATTENTION_MAX_ALIBI_BIAS,         "%s.attention.max_alibi_bias"         },
    { LLM_KV_ATTENTION_CLAMP_KQV,              "%s.attention.clamp_kqv"              },
    { LLM_KV_ATTENTION_KEY_LENGTH,             "%s.attention.key_length"             },
    { LLM_KV_ATTENTION_VALUE_LENGTH,           "%s.attention.value_length"           },
    { LLM_KV_ATTENTION_LAYERNORM_EPS,          "%s.attention.layer_norm_epsilon"     },
    { LLM_KV_ATTENTION_LAYERNORM_RMS_EPS,      "%s.attention.layer_norm_rms_epsilon" },
    { LLM_KV_ATTENTION_CAUSAL,                 "%s.attention.causal"                 },
    { LLM_KV_ATTENTION_Q_LORA_RANK,            "%s.attention.q_lora_rank"            },
    { LLM_KV_ATTENTION_KV_LORA_RANK,           "%s.attention.kv_lora_rank"           },
    { LLM_KV_ATTENTION_RELATIVE_BUCKETS_COUNT, "%s.attention.relative_buckets_count" },
    { LLM_KV_ATTENTION_SLIDING_WINDOW,         "%s.attention.sliding_window"         },
    { LLM_KV_ATTENTION_SCALE,                  "%s.attention.scale"                  },

    { LLM_KV_ROPE_DIMENSION_COUNT,             "%s.rope.dimension_count"                 },
    { LLM_KV_ROPE_FREQ_BASE,                   "%s.rope.freq_base"                       },
    { LLM_KV_ROPE_SCALE_LINEAR,                "%s.rope.scale_linear"                    },
    { LLM_KV_ROPE_SCALING_TYPE,                "%s.rope.scaling.type"                    },
    { LLM_KV_ROPE_SCALING_FACTOR,              "%s.rope.scaling.factor"                  },
    { LLM_KV_ROPE_SCALING_ATTN_FACTOR,         "%s.rope.scaling.attn_factor"             },
    { LLM_KV_ROPE_SCALING_ORIG_CTX_LEN,        "%s.rope.scaling.original_context_length" },
    { LLM_KV_ROPE_SCALING_FINETUNED,           "%s.rope.scaling.finetuned"               },
    { LLM_KV_ROPE_SCALING_YARN_LOG_MUL,        "%s.rope.scaling.yarn_log_multiplier"     },

    { LLM_KV_SPLIT_NO,                         "split.no"            },
    { LLM_KV_SPLIT_COUNT,                      "split.count"         },
    { LLM_KV_SPLIT_TENSORS_COUNT,              "split.tensors.count" },

    { LLM_KV_SSM_CONV_KERNEL,                  "%s.ssm.conv_kernel"    },
    { LLM_KV_SSM_INNER_SIZE,                   "%s.ssm.inner_size"     },
    { LLM_KV_SSM_STATE_SIZE,                   "%s.ssm.state_size"     },
    { LLM_KV_SSM_TIME_STEP_RANK,               "%s.ssm.time_step_rank" },
    { LLM_KV_SSM_DT_B_C_RMS,                   "%s.ssm.dt_b_c_rms"     },

    { LLM_KV_WKV_HEAD_SIZE,                    "%s.wkv.head_size" },

    { LLM_KV_TOKENIZER_MODEL,                  "tokenizer.ggml.model"                    },
    { LLM_KV_TOKENIZER_PRE,                    "tokenizer.ggml.pre"                      },
    { LLM_KV_TOKENIZER_LIST,                   "tokenizer.ggml.tokens"                   },
    { LLM_KV_TOKENIZER_TOKEN_TYPE,             "tokenizer.ggml.token_type"               },
    { LLM_KV_TOKENIZER_TOKEN_TYPE_COUNT,       "tokenizer.ggml.token_type_count"         },
    { LLM_KV_TOKENIZER_SCORES,                 "tokenizer.ggml.scores"                   },
    { LLM_KV_TOKENIZER_MERGES,                 "tokenizer.ggml.merges"                   },
    { LLM_KV_TOKENIZER_BOS_ID,                 "tokenizer.ggml.bos_token_id"             },
    { LLM_KV_TOKENIZER_EOS_ID,                 "tokenizer.ggml.eos_token_id"             },
    { LLM_KV_TOKENIZER_EOT_ID,                 "tokenizer.ggml.eot_token_id"             },
    { LLM_KV_TOKENIZER_EOM_ID,                 "tokenizer.ggml.eom_token_id"             },
    { LLM_KV_TOKENIZER_UNK_ID,                 "tokenizer.ggml.unknown_token_id"         },
    { LLM_KV_TOKENIZER_SEP_ID,                 "tokenizer.ggml.seperator_token_id"       },
    { LLM_KV_TOKENIZER_PAD_ID,                 "tokenizer.ggml.padding_token_id"         },
    { LLM_KV_TOKENIZER_CLS_ID,                 "tokenizer.ggml.cls_token_id"             },
    { LLM_KV_TOKENIZER_MASK_ID,                "tokenizer.ggml.mask_token_id"            },
    { LLM_KV_TOKENIZER_ADD_BOS,                "tokenizer.ggml.add_bos_token"            },
    { LLM_KV_TOKENIZER_ADD_EOS,                "tokenizer.ggml.add_eos_token"            },
    { LLM_KV_TOKENIZER_ADD_PREFIX,             "tokenizer.ggml.add_space_prefix"         },
    { LLM_KV_TOKENIZER_REMOVE_EXTRA_WS,        "tokenizer.ggml.remove_extra_whitespaces" },
    { LLM_KV_TOKENIZER_PRECOMPILED_CHARSMAP,   "tokenizer.ggml.precompiled_charsmap"     },
    { LLM_KV_TOKENIZER_HF_JSON,                "tokenizer.huggingface.json"              },
    { LLM_KV_TOKENIZER_RWKV,                   "tokenizer.rwkv.world"                    },
    { LLM_KV_TOKENIZER_FIM_PRE_ID,             "tokenizer.ggml.fim_pre_token_id"         },
    { LLM_KV_TOKENIZER_FIM_SUF_ID,             "tokenizer.ggml.fim_suf_token_id"         },
    { LLM_KV_TOKENIZER_FIM_MID_ID,             "tokenizer.ggml.fim_mid_token_id"         },
    { LLM_KV_TOKENIZER_FIM_PAD_ID,             "tokenizer.ggml.fim_pad_token_id"         },
    { LLM_KV_TOKENIZER_FIM_REP_ID,             "tokenizer.ggml.fim_rep_token_id"         },
    { LLM_KV_TOKENIZER_FIM_SEP_ID,             "tokenizer.ggml.fim_sep_token_id"         },

    { LLM_KV_ADAPTER_TYPE,                     "adapter.type"       },
    { LLM_KV_ADAPTER_LORA_ALPHA,               "adapter.lora.alpha" },

    // deprecated
    { LLM_KV_TOKENIZER_PREFIX_ID,              "tokenizer.ggml.prefix_token_id" },
    { LLM_KV_TOKENIZER_SUFFIX_ID,              "tokenizer.ggml.suffix_token_id" },
    { LLM_KV_TOKENIZER_MIDDLE_ID,              "tokenizer.ggml.middle_token_id" },
};

struct LLM_KV {
    LLM_KV(llm_arch arch) : arch(arch) {}

    llm_arch arch;

    std::string operator()(llm_kv kv) const {
        return ::format(LLM_KV_NAMES.at(kv), LLM_ARCH_NAMES.at(arch));
    }
};

enum llm_tensor {
    LLM_TENSOR_TOKEN_EMBD,
    LLM_TENSOR_TOKEN_EMBD_NORM,
    LLM_TENSOR_TOKEN_TYPES,
    LLM_TENSOR_POS_EMBD,
    LLM_TENSOR_OUTPUT,
    LLM_TENSOR_OUTPUT_NORM,
    LLM_TENSOR_ROPE_FREQS,
    LLM_TENSOR_ROPE_FACTORS_LONG,
    LLM_TENSOR_ROPE_FACTORS_SHORT,
    LLM_TENSOR_ATTN_Q,
    LLM_TENSOR_ATTN_K,
    LLM_TENSOR_ATTN_V,
    LLM_TENSOR_ATTN_QKV,
    LLM_TENSOR_ATTN_OUT,
    LLM_TENSOR_ATTN_NORM,
    LLM_TENSOR_ATTN_NORM_2,
    LLM_TENSOR_ATTN_OUT_NORM,
    LLM_TENSOR_ATTN_POST_NORM,
    LLM_TENSOR_ATTN_ROT_EMBD,
    LLM_TENSOR_FFN_GATE_INP,
    LLM_TENSOR_FFN_GATE_INP_SHEXP,
    LLM_TENSOR_FFN_NORM,
    LLM_TENSOR_FFN_POST_NORM,
    LLM_TENSOR_FFN_GATE,
    LLM_TENSOR_FFN_DOWN,
    LLM_TENSOR_FFN_UP,
    LLM_TENSOR_FFN_ACT,
    LLM_TENSOR_FFN_DOWN_EXP,  // split experts for backward compatibility
    LLM_TENSOR_FFN_GATE_EXP,
    LLM_TENSOR_FFN_UP_EXP,
    LLM_TENSOR_FFN_NORM_EXPS,
    LLM_TENSOR_FFN_DOWN_EXPS, // merged experts
    LLM_TENSOR_FFN_GATE_EXPS,
    LLM_TENSOR_FFN_UP_EXPS,
    LLM_TENSOR_FFN_DOWN_SHEXP,
    LLM_TENSOR_FFN_GATE_SHEXP,
    LLM_TENSOR_FFN_UP_SHEXP,
    LLM_TENSOR_ATTN_Q_NORM,
    LLM_TENSOR_ATTN_K_NORM,
    LLM_TENSOR_LAYER_OUT_NORM,
    LLM_TENSOR_SSM_IN,
    LLM_TENSOR_SSM_CONV1D,
    LLM_TENSOR_SSM_X,
    LLM_TENSOR_SSM_DT,
    LLM_TENSOR_SSM_A,
    LLM_TENSOR_SSM_D,
    LLM_TENSOR_SSM_OUT,
    LLM_TENSOR_TIME_MIX_W1,
    LLM_TENSOR_TIME_MIX_W2,
    LLM_TENSOR_TIME_MIX_LERP_X,
    LLM_TENSOR_TIME_MIX_LERP_W,
    LLM_TENSOR_TIME_MIX_LERP_K,
    LLM_TENSOR_TIME_MIX_LERP_V,
    LLM_TENSOR_TIME_MIX_LERP_R,
    LLM_TENSOR_TIME_MIX_LERP_G,
    LLM_TENSOR_TIME_MIX_FIRST,
    LLM_TENSOR_TIME_MIX_DECAY,
    LLM_TENSOR_TIME_MIX_DECAY_W1,
    LLM_TENSOR_TIME_MIX_DECAY_W2,
    LLM_TENSOR_TIME_MIX_KEY,
    LLM_TENSOR_TIME_MIX_VALUE,
    LLM_TENSOR_TIME_MIX_RECEPTANCE,
    LLM_TENSOR_TIME_MIX_GATE,
    LLM_TENSOR_TIME_MIX_LN,
    LLM_TENSOR_TIME_MIX_OUTPUT,
    LLM_TENSOR_CHANNEL_MIX_LERP_K,
    LLM_TENSOR_CHANNEL_MIX_LERP_R,
    LLM_TENSOR_CHANNEL_MIX_KEY,
    LLM_TENSOR_CHANNEL_MIX_RECEPTANCE,
    LLM_TENSOR_CHANNEL_MIX_VALUE,
    LLM_TENSOR_ATTN_Q_A,
    LLM_TENSOR_ATTN_Q_B,
    LLM_TENSOR_ATTN_KV_A_MQA,
    LLM_TENSOR_ATTN_KV_B,
    LLM_TENSOR_ATTN_Q_A_NORM,
    LLM_TENSOR_ATTN_KV_A_NORM,
    LLM_TENSOR_ATTN_SUB_NORM,
    LLM_TENSOR_FFN_SUB_NORM,
    LLM_TENSOR_DEC_ATTN_NORM,
    LLM_TENSOR_DEC_ATTN_Q,
    LLM_TENSOR_DEC_ATTN_K,
    LLM_TENSOR_DEC_ATTN_V,
    LLM_TENSOR_DEC_ATTN_OUT,
    LLM_TENSOR_DEC_ATTN_REL_B,
    LLM_TENSOR_DEC_CROSS_ATTN_NORM,
    LLM_TENSOR_DEC_CROSS_ATTN_Q,
    LLM_TENSOR_DEC_CROSS_ATTN_K,
    LLM_TENSOR_DEC_CROSS_ATTN_V,
    LLM_TENSOR_DEC_CROSS_ATTN_OUT,
    LLM_TENSOR_DEC_CROSS_ATTN_REL_B,
    LLM_TENSOR_DEC_FFN_NORM,
    LLM_TENSOR_DEC_FFN_GATE,
    LLM_TENSOR_DEC_FFN_DOWN,
    LLM_TENSOR_DEC_FFN_UP,
    LLM_TENSOR_DEC_OUTPUT_NORM,
    LLM_TENSOR_ENC_ATTN_NORM,
    LLM_TENSOR_ENC_ATTN_Q,
    LLM_TENSOR_ENC_ATTN_K,
    LLM_TENSOR_ENC_ATTN_V,
    LLM_TENSOR_ENC_ATTN_OUT,
    LLM_TENSOR_ENC_ATTN_REL_B,
    LLM_TENSOR_ENC_FFN_NORM,
    LLM_TENSOR_ENC_FFN_GATE,
    LLM_TENSOR_ENC_FFN_DOWN,
    LLM_TENSOR_ENC_FFN_UP,
    LLM_TENSOR_ENC_OUTPUT_NORM,
    LLM_TENSOR_CLS,
    LLM_TENSOR_CLS_OUT,
};

static const std::map<llm_arch, std::map<llm_tensor, const char *>> LLM_TENSOR_NAMES = {
    {
        LLM_ARCH_LLAMA,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ROPE_FREQS,      "rope_freqs" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_ATTN_ROT_EMBD,   "blk.%d.attn_rot_embd" },
            { LLM_TENSOR_FFN_GATE_INP,    "blk.%d.ffn_gate_inp" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
            { LLM_TENSOR_FFN_GATE_EXP,    "blk.%d.ffn_gate.%d" },
            { LLM_TENSOR_FFN_DOWN_EXP,    "blk.%d.ffn_down.%d" },
            { LLM_TENSOR_FFN_UP_EXP,      "blk.%d.ffn_up.%d" },
            { LLM_TENSOR_FFN_GATE_EXPS,   "blk.%d.ffn_gate_exps" },
            { LLM_TENSOR_FFN_DOWN_EXPS,   "blk.%d.ffn_down_exps" },
            { LLM_TENSOR_FFN_UP_EXPS,     "blk.%d.ffn_up_exps" },
        },
    },
    {
        LLM_ARCH_BAICHUAN,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ROPE_FREQS,      "rope_freqs" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_ATTN_ROT_EMBD,   "blk.%d.attn_rot_embd" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
        },
    },
    {
        LLM_ARCH_FALCON,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_NORM_2,     "blk.%d.attn_norm_2" },
            { LLM_TENSOR_ATTN_QKV,        "blk.%d.attn_qkv" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
        },
    },
    {
        LLM_ARCH_GROK,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ROPE_FREQS,      "rope_freqs" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_ATTN_ROT_EMBD,   "blk.%d.attn_rot_embd" },
            { LLM_TENSOR_FFN_GATE_INP,    "blk.%d.ffn_gate_inp" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_GATE_EXP,    "blk.%d.ffn_gate.%d" },
            { LLM_TENSOR_FFN_DOWN_EXP,    "blk.%d.ffn_down.%d" },
            { LLM_TENSOR_FFN_UP_EXP,      "blk.%d.ffn_up.%d" },
            { LLM_TENSOR_FFN_GATE_EXPS,   "blk.%d.ffn_gate_exps" },
            { LLM_TENSOR_FFN_DOWN_EXPS,   "blk.%d.ffn_down_exps" },
            { LLM_TENSOR_FFN_UP_EXPS,     "blk.%d.ffn_up_exps" },
            { LLM_TENSOR_LAYER_OUT_NORM,  "blk.%d.layer_output_norm" },
            { LLM_TENSOR_ATTN_OUT_NORM,   "blk.%d.attn_output_norm" },
        },
    },
    {
        LLM_ARCH_GPT2,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_POS_EMBD,        "position_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_QKV,        "blk.%d.attn_qkv" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
        },
    },
    {
        LLM_ARCH_GPTJ,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
        },
    },
    {
        LLM_ARCH_GPTNEOX,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_QKV,        "blk.%d.attn_qkv" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
        },
    },
    {
        LLM_ARCH_MPT,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output"},
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_ATTN_QKV,        "blk.%d.attn_qkv" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
            { LLM_TENSOR_FFN_ACT,         "blk.%d.ffn.act" },
            { LLM_TENSOR_POS_EMBD,        "position_embd" },
            { LLM_TENSOR_ATTN_Q_NORM,     "blk.%d.attn_q_norm"},
            { LLM_TENSOR_ATTN_K_NORM,     "blk.%d.attn_k_norm"},
        },
    },
    {
        LLM_ARCH_STARCODER,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_POS_EMBD,        "position_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_QKV,        "blk.%d.attn_qkv" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
        },
    },
    {
        LLM_ARCH_REFACT,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
        },
    },
    {
        LLM_ARCH_BERT,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_TOKEN_EMBD_NORM, "token_embd_norm" },
            { LLM_TENSOR_TOKEN_TYPES,     "token_types" },
            { LLM_TENSOR_POS_EMBD,        "position_embd" },
            { LLM_TENSOR_ATTN_OUT_NORM,   "blk.%d.attn_output_norm" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_LAYER_OUT_NORM,  "blk.%d.layer_output_norm" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
            { LLM_TENSOR_CLS,             "cls" },
            { LLM_TENSOR_CLS_OUT,         "cls.output" },
        },
    },
    {
        LLM_ARCH_NOMIC_BERT,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_TOKEN_EMBD_NORM, "token_embd_norm" },
            { LLM_TENSOR_TOKEN_TYPES,     "token_types" },
            { LLM_TENSOR_ATTN_OUT_NORM,   "blk.%d.attn_output_norm" },
            { LLM_TENSOR_ATTN_QKV,        "blk.%d.attn_qkv" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_LAYER_OUT_NORM,  "blk.%d.layer_output_norm" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
        },
    },
    {
        LLM_ARCH_JINA_BERT_V2,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_TOKEN_EMBD_NORM, "token_embd_norm" },
            { LLM_TENSOR_TOKEN_TYPES,     "token_types" },
            { LLM_TENSOR_ATTN_NORM_2,     "blk.%d.attn_norm_2" },
            { LLM_TENSOR_ATTN_OUT_NORM,   "blk.%d.attn_output_norm" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_Q_NORM,     "blk.%d.attn_q_norm" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_K_NORM,     "blk.%d.attn_k_norm" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_LAYER_OUT_NORM,  "blk.%d.layer_output_norm" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
            { LLM_TENSOR_CLS,             "cls" },
        },
    },
    {
        LLM_ARCH_BLOOM,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_TOKEN_EMBD_NORM, "token_embd_norm" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_QKV,        "blk.%d.attn_qkv" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
        },
    },
    {
        LLM_ARCH_STABLELM,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ROPE_FREQS,      "rope_freqs" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
            { LLM_TENSOR_ATTN_Q_NORM,     "blk.%d.attn_q_norm" },
            { LLM_TENSOR_ATTN_K_NORM,     "blk.%d.attn_k_norm" },
        },
    },
    {
        LLM_ARCH_QWEN,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ROPE_FREQS,      "rope_freqs" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_QKV,        "blk.%d.attn_qkv" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
        },
    },
    {
        LLM_ARCH_QWEN2,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
        },
    },
    {
        LLM_ARCH_QWEN2MOE,
        {
            { LLM_TENSOR_TOKEN_EMBD,         "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,        "output_norm" },
            { LLM_TENSOR_OUTPUT,             "output" },
            { LLM_TENSOR_ATTN_NORM,          "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q,             "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,             "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,             "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,           "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_NORM,           "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_GATE_INP,       "blk.%d.ffn_gate_inp" },
            { LLM_TENSOR_FFN_GATE_EXPS,      "blk.%d.ffn_gate_exps" },
            { LLM_TENSOR_FFN_DOWN_EXPS,      "blk.%d.ffn_down_exps" },
            { LLM_TENSOR_FFN_UP_EXPS,        "blk.%d.ffn_up_exps" },
            { LLM_TENSOR_FFN_GATE_INP_SHEXP, "blk.%d.ffn_gate_inp_shexp" },
            { LLM_TENSOR_FFN_GATE_SHEXP,     "blk.%d.ffn_gate_shexp" },
            { LLM_TENSOR_FFN_DOWN_SHEXP,     "blk.%d.ffn_down_shexp" },
            { LLM_TENSOR_FFN_UP_SHEXP,       "blk.%d.ffn_up_shexp" },
        },
    },
    {
        LLM_ARCH_PHI2,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_QKV,        "blk.%d.attn_qkv" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
        },
    },
    {
        LLM_ARCH_PHI3,
        {
            { LLM_TENSOR_TOKEN_EMBD,         "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,        "output_norm" },
            { LLM_TENSOR_OUTPUT,             "output" },
            { LLM_TENSOR_ROPE_FACTORS_LONG,  "rope_factors_long" },
            { LLM_TENSOR_ROPE_FACTORS_SHORT, "rope_factors_short" },
            { LLM_TENSOR_ATTN_NORM,          "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_QKV,           "blk.%d.attn_qkv" },
            { LLM_TENSOR_ATTN_Q,             "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,             "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,             "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,           "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_NORM,           "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_DOWN,           "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,             "blk.%d.ffn_up" },
        },
    },
    {
        LLM_ARCH_PLAMO,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ROPE_FREQS,      "rope_freqs" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_ATTN_ROT_EMBD,   "blk.%d.attn_rot_embd" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
        },
    },
    {
        LLM_ARCH_CODESHELL,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ROPE_FREQS,      "rope_freqs" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_QKV,        "blk.%d.attn_qkv" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_ATTN_ROT_EMBD,   "blk.%d.attn_rot_embd" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
        },
    },
    {
        LLM_ARCH_ORION,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ROPE_FREQS,      "rope_freqs" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_ATTN_ROT_EMBD,   "blk.%d.attn_rot_embd" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
        },
    },
    {
        LLM_ARCH_INTERNLM2,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
        },
    },
    {
        LLM_ARCH_MINICPM,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ROPE_FREQS,      "rope_freqs" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_ATTN_ROT_EMBD,   "blk.%d.attn_rot_embd" },
            { LLM_TENSOR_FFN_GATE_INP,    "blk.%d.ffn_gate_inp" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
            { LLM_TENSOR_FFN_GATE_EXP,    "blk.%d.ffn_gate.%d" },
            { LLM_TENSOR_FFN_DOWN_EXP,    "blk.%d.ffn_down.%d" },
            { LLM_TENSOR_FFN_UP_EXP,      "blk.%d.ffn_up.%d" },
        },
    },
    {
        LLM_ARCH_MINICPM3,
        {
            { LLM_TENSOR_TOKEN_EMBD,         "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,        "output_norm" },
            { LLM_TENSOR_OUTPUT,             "output" },
            { LLM_TENSOR_ROPE_FACTORS_LONG,  "rope_factors_long" },
            { LLM_TENSOR_ROPE_FACTORS_SHORT, "rope_factors_short" },
            { LLM_TENSOR_ATTN_NORM,          "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q_A_NORM,      "blk.%d.attn_q_a_norm" },
            { LLM_TENSOR_ATTN_KV_A_NORM,     "blk.%d.attn_kv_a_norm" },
            { LLM_TENSOR_ATTN_Q,             "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_Q_A,           "blk.%d.attn_q_a" },
            { LLM_TENSOR_ATTN_Q_B,           "blk.%d.attn_q_b" },
            { LLM_TENSOR_ATTN_KV_A_MQA,      "blk.%d.attn_kv_a_mqa" },
            { LLM_TENSOR_ATTN_KV_B,          "blk.%d.attn_kv_b" },
            { LLM_TENSOR_ATTN_OUT,           "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_NORM,           "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_GATE,           "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_UP,             "blk.%d.ffn_up" },
            { LLM_TENSOR_FFN_DOWN,           "blk.%d.ffn_down" },
        },
    },
    {
        LLM_ARCH_GEMMA,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
        },
    },
    {
        LLM_ARCH_GEMMA2,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_ATTN_POST_NORM,  "blk.%d.post_attention_norm" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
            { LLM_TENSOR_FFN_POST_NORM,   "blk.%d.post_ffw_norm" },
        },
    },
    {
        LLM_ARCH_STARCODER2,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ROPE_FREQS,      "rope_freqs" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_ATTN_ROT_EMBD,   "blk.%d.attn_rot_embd" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
        },
    },
    {
        LLM_ARCH_MAMBA,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_SSM_IN,          "blk.%d.ssm_in" },
            { LLM_TENSOR_SSM_CONV1D,      "blk.%d.ssm_conv1d" },
            { LLM_TENSOR_SSM_X,           "blk.%d.ssm_x" },
            { LLM_TENSOR_SSM_DT,          "blk.%d.ssm_dt" },
            { LLM_TENSOR_SSM_A,           "blk.%d.ssm_a" },
            { LLM_TENSOR_SSM_D,           "blk.%d.ssm_d" },
            { LLM_TENSOR_SSM_OUT,         "blk.%d.ssm_out" },
        },
    },
    {
        LLM_ARCH_XVERSE,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ROPE_FREQS,      "rope_freqs" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_ATTN_ROT_EMBD,   "blk.%d.attn_rot_embd" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
        },
    },
    {
        LLM_ARCH_COMMAND_R,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
            { LLM_TENSOR_ATTN_Q_NORM,     "blk.%d.attn_q_norm" },
            { LLM_TENSOR_ATTN_K_NORM,     "blk.%d.attn_k_norm" },
        },
    },
    {
        LLM_ARCH_DBRX,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ATTN_QKV,        "blk.%d.attn_qkv" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_ATTN_OUT_NORM,   "blk.%d.attn_output_norm" },
            { LLM_TENSOR_FFN_GATE_INP,    "blk.%d.ffn_gate_inp" },
            { LLM_TENSOR_FFN_GATE_EXPS,   "blk.%d.ffn_gate_exps" },
            { LLM_TENSOR_FFN_DOWN_EXPS,   "blk.%d.ffn_down_exps" },
            { LLM_TENSOR_FFN_UP_EXPS,     "blk.%d.ffn_up_exps" },
        },
    },
    {
        LLM_ARCH_OLMO,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
        },
    },
    {
        LLM_ARCH_OLMO_1124,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_ATTN_POST_NORM,  "blk.%d.post_attention_norm" },
            { LLM_TENSOR_ATTN_Q_NORM,     "blk.%d.attn_q_norm" },
            { LLM_TENSOR_ATTN_K_NORM,     "blk.%d.attn_k_norm" },
            { LLM_TENSOR_FFN_POST_NORM,   "blk.%d.post_ffw_norm" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
        },
    },
    {
        LLM_ARCH_OLMOE,
        {
            { LLM_TENSOR_TOKEN_EMBD,         "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,        "output_norm" },
            { LLM_TENSOR_OUTPUT,             "output" },
            { LLM_TENSOR_ATTN_NORM,          "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q,             "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,             "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,             "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,           "blk.%d.attn_output" },
            { LLM_TENSOR_ATTN_Q_NORM,        "blk.%d.attn_q_norm" },
            { LLM_TENSOR_ATTN_K_NORM,        "blk.%d.attn_k_norm" },
            { LLM_TENSOR_FFN_NORM,           "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_GATE_INP,       "blk.%d.ffn_gate_inp" },
            { LLM_TENSOR_FFN_GATE_EXPS,      "blk.%d.ffn_gate_exps" },
            { LLM_TENSOR_FFN_DOWN_EXPS,      "blk.%d.ffn_down_exps" },
            { LLM_TENSOR_FFN_UP_EXPS,        "blk.%d.ffn_up_exps" },
        },
    },
    {
        LLM_ARCH_OPENELM,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_QKV,        "blk.%d.attn_qkv" },
            { LLM_TENSOR_ATTN_Q_NORM,     "blk.%d.attn_q_norm" },
            { LLM_TENSOR_ATTN_K_NORM,     "blk.%d.attn_k_norm" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
        },
    },
    {
        LLM_ARCH_ARCTIC,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_GATE_INP,    "blk.%d.ffn_gate_inp" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
            { LLM_TENSOR_FFN_NORM_EXPS,   "blk.%d.ffn_norm_exps" },
            { LLM_TENSOR_FFN_GATE_EXPS,   "blk.%d.ffn_gate_exps" },
            { LLM_TENSOR_FFN_DOWN_EXPS,   "blk.%d.ffn_down_exps" },
            { LLM_TENSOR_FFN_UP_EXPS,     "blk.%d.ffn_up_exps" },
        },
    },
    {
        LLM_ARCH_DEEPSEEK2,
        {
            { LLM_TENSOR_TOKEN_EMBD,         "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,        "output_norm" },
            { LLM_TENSOR_OUTPUT,             "output" },
            { LLM_TENSOR_ATTN_NORM,          "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q_A_NORM,      "blk.%d.attn_q_a_norm" },
            { LLM_TENSOR_ATTN_KV_A_NORM,     "blk.%d.attn_kv_a_norm" },
            { LLM_TENSOR_ATTN_Q,             "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_Q_A,           "blk.%d.attn_q_a" },
            { LLM_TENSOR_ATTN_Q_B,           "blk.%d.attn_q_b" },
            { LLM_TENSOR_ATTN_KV_A_MQA,      "blk.%d.attn_kv_a_mqa" },
            { LLM_TENSOR_ATTN_KV_B,          "blk.%d.attn_kv_b" },
            { LLM_TENSOR_ATTN_OUT,           "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_NORM,           "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_GATE,           "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_UP,             "blk.%d.ffn_up" },
            { LLM_TENSOR_FFN_DOWN,           "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_GATE_INP,       "blk.%d.ffn_gate_inp" },
            { LLM_TENSOR_FFN_GATE_EXPS,      "blk.%d.ffn_gate_exps" },
            { LLM_TENSOR_FFN_DOWN_EXPS,      "blk.%d.ffn_down_exps" },
            { LLM_TENSOR_FFN_UP_EXPS,        "blk.%d.ffn_up_exps" },
            { LLM_TENSOR_FFN_GATE_INP_SHEXP, "blk.%d.ffn_gate_inp_shexp" },
            { LLM_TENSOR_FFN_GATE_SHEXP,     "blk.%d.ffn_gate_shexp" },
            { LLM_TENSOR_FFN_DOWN_SHEXP,     "blk.%d.ffn_down_shexp" },
            { LLM_TENSOR_FFN_UP_SHEXP,       "blk.%d.ffn_up_shexp" },
        },
    },
    {
        LLM_ARCH_CHATGLM,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_ROPE_FREQS,      "rope_freqs" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_QKV,        "blk.%d.attn_qkv" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
        },
    },
    {
        LLM_ARCH_BITNET,
        {
            { LLM_TENSOR_TOKEN_EMBD,         "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,        "output_norm" },
            { LLM_TENSOR_ATTN_Q,             "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,             "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,             "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,           "blk.%d.attn_output" },
            { LLM_TENSOR_ATTN_NORM,          "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_SUB_NORM,      "blk.%d.attn_sub_norm" },
            { LLM_TENSOR_FFN_GATE,           "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_DOWN,           "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,             "blk.%d.ffn_up" },
            { LLM_TENSOR_FFN_NORM,           "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_SUB_NORM,       "blk.%d.ffn_sub_norm" },
        },
    },
    {
        LLM_ARCH_T5,
        {
            { LLM_TENSOR_TOKEN_EMBD,           "token_embd" },
            { LLM_TENSOR_OUTPUT,               "output" },
            { LLM_TENSOR_DEC_OUTPUT_NORM,      "dec.output_norm" },
            { LLM_TENSOR_DEC_ATTN_NORM,        "dec.blk.%d.attn_norm" },
            { LLM_TENSOR_DEC_ATTN_Q,           "dec.blk.%d.attn_q" },
            { LLM_TENSOR_DEC_ATTN_K,           "dec.blk.%d.attn_k" },
            { LLM_TENSOR_DEC_ATTN_V,           "dec.blk.%d.attn_v" },
            { LLM_TENSOR_DEC_ATTN_OUT,         "dec.blk.%d.attn_o" },
            { LLM_TENSOR_DEC_ATTN_REL_B,       "dec.blk.%d.attn_rel_b" },
            { LLM_TENSOR_DEC_CROSS_ATTN_NORM,  "dec.blk.%d.cross_attn_norm" },
            { LLM_TENSOR_DEC_CROSS_ATTN_Q,     "dec.blk.%d.cross_attn_q" },
            { LLM_TENSOR_DEC_CROSS_ATTN_K,     "dec.blk.%d.cross_attn_k" },
            { LLM_TENSOR_DEC_CROSS_ATTN_V,     "dec.blk.%d.cross_attn_v" },
            { LLM_TENSOR_DEC_CROSS_ATTN_OUT,   "dec.blk.%d.cross_attn_o" },
            { LLM_TENSOR_DEC_CROSS_ATTN_REL_B, "dec.blk.%d.cross_attn_rel_b" },
            { LLM_TENSOR_DEC_FFN_NORM,         "dec.blk.%d.ffn_norm" },
            { LLM_TENSOR_DEC_FFN_GATE,         "dec.blk.%d.ffn_gate" },
            { LLM_TENSOR_DEC_FFN_DOWN,         "dec.blk.%d.ffn_down" },
            { LLM_TENSOR_DEC_FFN_UP,           "dec.blk.%d.ffn_up" },
            { LLM_TENSOR_ENC_OUTPUT_NORM,      "enc.output_norm" },
            { LLM_TENSOR_ENC_ATTN_NORM,        "enc.blk.%d.attn_norm" },
            { LLM_TENSOR_ENC_ATTN_Q,           "enc.blk.%d.attn_q" },
            { LLM_TENSOR_ENC_ATTN_K,           "enc.blk.%d.attn_k" },
            { LLM_TENSOR_ENC_ATTN_V,           "enc.blk.%d.attn_v" },
            { LLM_TENSOR_ENC_ATTN_OUT,         "enc.blk.%d.attn_o" },
            { LLM_TENSOR_ENC_ATTN_REL_B,       "enc.blk.%d.attn_rel_b" },
            { LLM_TENSOR_ENC_FFN_NORM,         "enc.blk.%d.ffn_norm" },
            { LLM_TENSOR_ENC_FFN_GATE,         "enc.blk.%d.ffn_gate" },
            { LLM_TENSOR_ENC_FFN_DOWN,         "enc.blk.%d.ffn_down" },
            { LLM_TENSOR_ENC_FFN_UP,           "enc.blk.%d.ffn_up" },
        },
    },
    {
        LLM_ARCH_T5ENCODER,
        {
            { LLM_TENSOR_TOKEN_EMBD,           "token_embd" },
            { LLM_TENSOR_OUTPUT,               "output" },
            { LLM_TENSOR_ENC_OUTPUT_NORM,      "enc.output_norm" },
            { LLM_TENSOR_ENC_ATTN_NORM,        "enc.blk.%d.attn_norm" },
            { LLM_TENSOR_ENC_ATTN_Q,           "enc.blk.%d.attn_q" },
            { LLM_TENSOR_ENC_ATTN_K,           "enc.blk.%d.attn_k" },
            { LLM_TENSOR_ENC_ATTN_V,           "enc.blk.%d.attn_v" },
            { LLM_TENSOR_ENC_ATTN_OUT,         "enc.blk.%d.attn_o" },
            { LLM_TENSOR_ENC_ATTN_REL_B,       "enc.blk.%d.attn_rel_b" },
            { LLM_TENSOR_ENC_FFN_NORM,         "enc.blk.%d.ffn_norm" },
            { LLM_TENSOR_ENC_FFN_GATE,         "enc.blk.%d.ffn_gate" },
            { LLM_TENSOR_ENC_FFN_DOWN,         "enc.blk.%d.ffn_down" },
            { LLM_TENSOR_ENC_FFN_UP,           "enc.blk.%d.ffn_up" },
        },
    },
    {
        LLM_ARCH_JAIS,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_QKV,        "blk.%d.attn_qkv" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
        },
    },
    {
        LLM_ARCH_NEMOTRON,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ROPE_FREQS,      "rope_freqs" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_ATTN_ROT_EMBD,   "blk.%d.attn_rot_embd" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
        },
    },
    {
        LLM_ARCH_EXAONE,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ROPE_FREQS,      "rope_freqs" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_ATTN_ROT_EMBD,   "blk.%d.attn_rot_embd" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
        },
    },
    {
        LLM_ARCH_RWKV6,
        {
            { LLM_TENSOR_TOKEN_EMBD,                "token_embd" },
            { LLM_TENSOR_TOKEN_EMBD_NORM,           "token_embd_norm" },
            { LLM_TENSOR_OUTPUT_NORM,               "output_norm" },
            { LLM_TENSOR_OUTPUT,                    "output" },
            { LLM_TENSOR_ATTN_NORM,                 "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_NORM_2,               "blk.%d.attn_norm_2" },
            { LLM_TENSOR_TIME_MIX_W1,               "blk.%d.time_mix_w1" },
            { LLM_TENSOR_TIME_MIX_W2,               "blk.%d.time_mix_w2" },
            { LLM_TENSOR_TIME_MIX_LERP_X,           "blk.%d.time_mix_lerp_x" },
            { LLM_TENSOR_TIME_MIX_LERP_W,           "blk.%d.time_mix_lerp_w" },
            { LLM_TENSOR_TIME_MIX_LERP_K,           "blk.%d.time_mix_lerp_k" },
            { LLM_TENSOR_TIME_MIX_LERP_V,           "blk.%d.time_mix_lerp_v" },
            { LLM_TENSOR_TIME_MIX_LERP_R,           "blk.%d.time_mix_lerp_r" },
            { LLM_TENSOR_TIME_MIX_LERP_G,           "blk.%d.time_mix_lerp_g" },
            { LLM_TENSOR_TIME_MIX_FIRST,            "blk.%d.time_mix_first" },
            { LLM_TENSOR_TIME_MIX_DECAY,            "blk.%d.time_mix_decay" },
            { LLM_TENSOR_TIME_MIX_DECAY_W1,         "blk.%d.time_mix_decay_w1" },
            { LLM_TENSOR_TIME_MIX_DECAY_W2,         "blk.%d.time_mix_decay_w2" },
            { LLM_TENSOR_TIME_MIX_KEY,              "blk.%d.time_mix_key" },
            { LLM_TENSOR_TIME_MIX_VALUE,            "blk.%d.time_mix_value" },
            { LLM_TENSOR_TIME_MIX_RECEPTANCE,       "blk.%d.time_mix_receptance" },
            { LLM_TENSOR_TIME_MIX_GATE,             "blk.%d.time_mix_gate" },
            { LLM_TENSOR_TIME_MIX_LN,               "blk.%d.time_mix_ln" },
            { LLM_TENSOR_TIME_MIX_OUTPUT,           "blk.%d.time_mix_output" },
            { LLM_TENSOR_CHANNEL_MIX_LERP_K,        "blk.%d.channel_mix_lerp_k" },
            { LLM_TENSOR_CHANNEL_MIX_LERP_R,        "blk.%d.channel_mix_lerp_r" },
            { LLM_TENSOR_CHANNEL_MIX_KEY,           "blk.%d.channel_mix_key" },
            { LLM_TENSOR_CHANNEL_MIX_VALUE,         "blk.%d.channel_mix_value" },
            { LLM_TENSOR_CHANNEL_MIX_RECEPTANCE,    "blk.%d.channel_mix_receptance" },
        },
    },
    {
        LLM_ARCH_GRANITE,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
        },
    },
    {
        LLM_ARCH_GRANITE_MOE,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_GATE_INP,    "blk.%d.ffn_gate_inp" },
            { LLM_TENSOR_FFN_GATE_EXPS,   "blk.%d.ffn_gate_exps" },
            { LLM_TENSOR_FFN_DOWN_EXPS,   "blk.%d.ffn_down_exps" },
            { LLM_TENSOR_FFN_UP_EXPS,     "blk.%d.ffn_up_exps" },
        },
    },
    {
        LLM_ARCH_CHAMELEON,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
            { LLM_TENSOR_OUTPUT_NORM,     "output_norm" },
            { LLM_TENSOR_OUTPUT,          "output" },
            { LLM_TENSOR_ATTN_NORM,       "blk.%d.attn_norm" },
            { LLM_TENSOR_ATTN_Q,          "blk.%d.attn_q" },
            { LLM_TENSOR_ATTN_K,          "blk.%d.attn_k" },
            { LLM_TENSOR_ATTN_V,          "blk.%d.attn_v" },
            { LLM_TENSOR_ATTN_OUT,        "blk.%d.attn_output" },
            { LLM_TENSOR_FFN_NORM,        "blk.%d.ffn_norm" },
            { LLM_TENSOR_FFN_GATE,        "blk.%d.ffn_gate" },
            { LLM_TENSOR_FFN_DOWN,        "blk.%d.ffn_down" },
            { LLM_TENSOR_FFN_UP,          "blk.%d.ffn_up" },
            { LLM_TENSOR_ATTN_Q_NORM,     "blk.%d.attn_q_norm" },
            { LLM_TENSOR_ATTN_K_NORM,     "blk.%d.attn_k_norm" },
        },
    },
    {
        LLM_ARCH_UNKNOWN,
        {
            { LLM_TENSOR_TOKEN_EMBD,      "token_embd" },
        },
    },
};

static llm_arch llm_arch_from_string(const std::string & name) {
    for (const auto & kv : LLM_ARCH_NAMES) { // NOLINT
        if (kv.second == name) {
            return kv.first;
        }
    }

    return LLM_ARCH_UNKNOWN;
}

// helper to handle gguf constants
// usage:
//
//   const auto tn = LLM_TN(LLM_ARCH_LLAMA);
//
//   std::string name = tn(LLM_TENSOR_OUTPUT);                     -> "output"
//   std::string name = tn(LLM_TENSOR_TOKEN_EMBD, "bias");         -> "token_embd.bias"
//   std::string name = tn(LLM_TENSOR_ATTN_NORM, "weight", 3);     -> "blk.3.attn_norm.weight"
//
struct LLM_TN_IMPL {
    const llm_arch arch;
    const llm_tensor tensor;
    const char * const suffix;
    const int bid;
    const int xid;

    std::string str() const {
        if (LLM_TENSOR_NAMES.at(arch).find(tensor) == LLM_TENSOR_NAMES.at(arch).end()) {
            return "__missing__";
        }

        std::string name = ::format(LLM_TENSOR_NAMES.at(arch).at(tensor), bid, xid);

        if (suffix != nullptr) {
            name += ".";
            name += suffix;
        }

        return name;
    }

    operator std::string() const {
        return str();
    }

    friend bool operator==(const std::string & str, const LLM_TN_IMPL & tn) {
        return str == tn.str();
    }

    friend bool operator!=(const std::string & str, const LLM_TN_IMPL & tn) {
        return str != tn.str();
    }
};

struct LLM_TN {
    LLM_TN(llm_arch arch) : arch(arch) {}

    llm_arch arch;

    LLM_TN_IMPL operator()(llm_tensor tensor, const char * suffix, int bid = -1, int xid = -1) const {
        return { arch, tensor, suffix, bid, xid };
    }

    LLM_TN_IMPL operator()(llm_tensor tensor, int bid = -1, int xid = -1) const {
        return { arch, tensor, nullptr, bid, xid };
    }
};

//
// gguf helpers
//

static const std::map<llama_rope_scaling_type, const char *> LLAMA_ROPE_SCALING_TYPES = {
    { LLAMA_ROPE_SCALING_TYPE_NONE,   "none"   },
    { LLAMA_ROPE_SCALING_TYPE_LINEAR, "linear" },
    { LLAMA_ROPE_SCALING_TYPE_YARN,   "yarn"   },
};

static llama_rope_scaling_type llama_rope_scaling_type_from_string(const std::string & name) {
    for (const auto & kv : LLAMA_ROPE_SCALING_TYPES) {
        if (kv.second == name) {
            return (llama_rope_scaling_type) kv.first;
        }
    }

    return LLAMA_ROPE_SCALING_TYPE_UNSPECIFIED;
}

static std::string lm_gguf_data_to_str(enum lm_gguf_type type, const void * data, int i) {
    switch (type) {
        case LM_GGUF_TYPE_UINT8:   return std::to_string(((const uint8_t  *)data)[i]);
        case LM_GGUF_TYPE_INT8:    return std::to_string(((const int8_t   *)data)[i]);
        case LM_GGUF_TYPE_UINT16:  return std::to_string(((const uint16_t *)data)[i]);
        case LM_GGUF_TYPE_INT16:   return std::to_string(((const int16_t  *)data)[i]);
        case LM_GGUF_TYPE_UINT32:  return std::to_string(((const uint32_t *)data)[i]);
        case LM_GGUF_TYPE_INT32:   return std::to_string(((const int32_t  *)data)[i]);
        case LM_GGUF_TYPE_UINT64:  return std::to_string(((const uint64_t *)data)[i]);
        case LM_GGUF_TYPE_INT64:   return std::to_string(((const int64_t  *)data)[i]);
        case LM_GGUF_TYPE_FLOAT32: return std::to_string(((const float    *)data)[i]);
        case LM_GGUF_TYPE_FLOAT64: return std::to_string(((const double   *)data)[i]);
        case LM_GGUF_TYPE_BOOL:    return ((const bool *)data)[i] ? "true" : "false";
        default:                return format("unknown type %d", type);
    }
}

static std::string lm_gguf_kv_to_str(const struct lm_gguf_context * ctx_gguf, int i) {
    const enum lm_gguf_type type = lm_gguf_get_kv_type(ctx_gguf, i);

    switch (type) {
        case LM_GGUF_TYPE_STRING:
            return lm_gguf_get_val_str(ctx_gguf, i);
        case LM_GGUF_TYPE_ARRAY:
            {
                const enum lm_gguf_type arr_type = lm_gguf_get_arr_type(ctx_gguf, i);
                int arr_n = lm_gguf_get_arr_n(ctx_gguf, i);
                const void * data = lm_gguf_get_arr_data(ctx_gguf, i);
                std::stringstream ss;
                ss << "[";
                for (int j = 0; j < arr_n; j++) {
                    if (arr_type == LM_GGUF_TYPE_STRING) {
                        std::string val = lm_gguf_get_arr_str(ctx_gguf, i, j);
                        // escape quotes
                        replace_all(val, "\\", "\\\\");
                        replace_all(val, "\"", "\\\"");
                        ss << '"' << val << '"';
                    } else if (arr_type == LM_GGUF_TYPE_ARRAY) {
                        ss << "???";
                    } else {
                        ss << lm_gguf_data_to_str(arr_type, data, j);
                    }
                    if (j < arr_n - 1) {
                        ss << ", ";
                    }
                }
                ss << "]";
                return ss.str();
            }
        default:
            return lm_gguf_data_to_str(type, lm_gguf_get_val_data(ctx_gguf, i), 0);
    }
}

//
// llama helpers
//

#if defined(_WIN32)
static std::string llama_format_win_err(DWORD err) {
    LPSTR buf;
    size_t size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                 NULL, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&buf, 0, NULL);
    if (!size) {
        return "FormatMessageA failed";
    }
    std::string ret(buf, size);
    LocalFree(buf);
    return ret;
}
#endif

template <typename T>
struct no_init {
    T value;
    no_init() { /* do nothing */ }
};

struct llama_file {

#if defined(_WIN32)
    // use FILE * so we don't have to re-open the file to mmap
    FILE * fp;
    HANDLE fp_win32;
    size_t size;

private:
    std::string GetErrorMessageWin32(DWORD error_code) const {
        std::string ret;
        LPSTR lpMsgBuf = NULL;
        DWORD bufLen = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                    NULL, error_code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&lpMsgBuf, 0, NULL);
        if (!bufLen) {
            ret = format("Win32 error code: %s", error_code);
        } else {
            ret = lpMsgBuf;
            LocalFree(lpMsgBuf);
        }

        return ret;
    }

public:

    llama_file(const char * fname, const char * mode) {
        fp = lm_ggml_fopen(fname, mode);
        if (fp == NULL) {
            throw std::runtime_error(format("failed to open %s: %s", fname, strerror(errno)));
        }
        fp_win32 = (HANDLE) _get_osfhandle(_fileno(fp));
        seek(0, SEEK_END);
        size = tell();
        seek(0, SEEK_SET);
    }

    size_t tell() const {
        // SetFilePointerEx returns the current position when seeking relative 0 bytes
        LARGE_INTEGER li;
        li.QuadPart = 0;
        BOOL ret = SetFilePointerEx(fp_win32, li, &li, FILE_CURRENT);
        if (!ret) {
            throw std::runtime_error(format("read error: %s", GetErrorMessageWin32(GetLastError()).c_str()));
        }

        return li.QuadPart;
    }

    void seek(size_t offset, int whence) const {
        // no need to convert SEEK_* to FILE_*. The enums are the same.
        // Still, keep static asserts to avoid failures in the future.
        static_assert(SEEK_SET == FILE_BEGIN, "SEEK_SET != FILE_BEGIN");
        static_assert(SEEK_CUR == FILE_CURRENT, "SEEK_CUR != FILE_CURRENT");
        static_assert(SEEK_END == FILE_END, "SEEK_END != FILE_END");

        LARGE_INTEGER li;
        li.QuadPart = offset;
        BOOL ret = SetFilePointerEx(fp_win32, li, NULL, whence);
        if (!ret) {
            throw std::runtime_error(format("read error: %s", GetErrorMessageWin32(GetLastError()).c_str()));
        }
    }

    void read_raw(void * ptr, size_t len) const {
        // On Win32 ReadFile is significant faster than fread which is again significant faster than std::fstream. Thus
        // use the Win32 API to do file io instead of the C/C++ library functions.

        // There are conditions under which ReadFile cannot read chunks >64MB.
        // Thus split the operation into smaller chunks if len exceeds this limit.
        size_t bytes_read = 0;
        while (bytes_read < len) {
            size_t chunk_size = std::min<size_t>(len - bytes_read, 64*1024*1024);
            DWORD chunk_read = 0;
            BOOL result = ReadFile(fp_win32, reinterpret_cast<char*>(ptr) + bytes_read, chunk_size, &chunk_read, NULL);
            if (!result) {
                throw std::runtime_error(format("read error: %s", GetErrorMessageWin32(GetLastError()).c_str()));
            }
            if (chunk_read < chunk_size || chunk_read == 0) {
                throw std::runtime_error("unexpectedly reached end of file");
            }

            bytes_read += chunk_read;
        } ;
    }

    uint32_t read_u32() const {
        uint32_t val;
        read_raw(&val, sizeof(val));
        return val;
    }

    void write_raw(const void * ptr, size_t len) const {
        // There are conditions under which WriteFile cannot write chunks >64MB.
        // Thus split the operation into smaller chunks if len exceeds this limit.
        size_t bytes_written = 0;
        while (bytes_written < len) {
            size_t chunk_size = std::min<size_t>(len - bytes_written, 64*1024*1024);
            DWORD chunk_written = 0;
            BOOL result = WriteFile(fp_win32, reinterpret_cast<char const*>(ptr) + bytes_written, chunk_size, &chunk_written, NULL);
            if (!result) {
                throw std::runtime_error(format("write error: %s", GetErrorMessageWin32(GetLastError()).c_str()));
            }
            if (chunk_written < chunk_size || chunk_written == 0) {
                throw std::runtime_error("unexpectedly failed to write bytes");
            }

            bytes_written += chunk_written;
        }
    }

    void write_u32(std::uint32_t val) const {
        write_raw(&val, sizeof(val));
    }

    ~llama_file() {
        if (fp) {
            std::fclose(fp);
        }
    }
#else
    // use FILE * so we don't have to re-open the file to mmap
    FILE * fp;
    size_t size;

    llama_file(const char * fname, const char * mode) {
        fp = lm_ggml_fopen(fname, mode);
        if (fp == NULL) {
            throw std::runtime_error(format("failed to open %s: %s", fname, strerror(errno)));
        }
        seek(0, SEEK_END);
        size = tell();
        seek(0, SEEK_SET);
    }

    size_t tell() const {
#ifdef _WIN32
        __int64 ret = _ftelli64(fp);
#else
        long ret = std::ftell(fp);
#endif
        if (ret == -1) {
            throw std::runtime_error(format("ftell error: %s", strerror(errno)));
        }

        return (size_t) ret;
    }

    void seek(size_t offset, int whence) const {
#ifdef _WIN32
        int ret = _fseeki64(fp, (__int64) offset, whence);
#else
        int ret = std::fseek(fp, (long) offset, whence);
#endif
        if (ret != 0) {
            throw std::runtime_error(format("seek error: %s", strerror(errno)));
        }
    }

    void read_raw(void * ptr, size_t len) const {
        if (len == 0) {
            return;
        }
        errno = 0;
        std::size_t ret = std::fread(ptr, len, 1, fp);
        if (ferror(fp)) {
            throw std::runtime_error(format("read error: %s", strerror(errno)));
        }
        if (ret != 1) {
            throw std::runtime_error("unexpectedly reached end of file");
        }
    }

    uint32_t read_u32() const {
        uint32_t ret;
        read_raw(&ret, sizeof(ret));
        return ret;
    }

    void write_raw(const void * ptr, size_t len) const {
        if (len == 0) {
            return;
        }
        errno = 0;
        size_t ret = std::fwrite(ptr, len, 1, fp);
        if (ret != 1) {
            throw std::runtime_error(format("write error: %s", strerror(errno)));
        }
    }

    void write_u32(std::uint32_t val) const {
        write_raw(&val, sizeof(val));
    }

    ~llama_file() {
        if (fp) {
            std::fclose(fp);
        }
    }
#endif
};
using llama_files = std::vector<std::unique_ptr<llama_file>>;

struct llama_mmap {
    void * addr;
    size_t size;

    llama_mmap(const llama_mmap &) = delete;

#ifdef _POSIX_MAPPED_FILES
    static constexpr bool SUPPORTED = true;

    // list of mapped fragments (first_offset, last_offset)
    std::vector<std::pair<size_t, size_t>> mapped_fragments;

    llama_mmap(struct llama_file * file, size_t prefetch = (size_t) -1 /* -1 = max value */, bool numa = false) {
        size = file->size;
        int fd = fileno(file->fp);
        int flags = MAP_SHARED;
        // prefetch/readahead impairs performance on NUMA systems
        if (numa)  { prefetch = 0; }
#ifdef __linux__
        // advise the kernel to read the file sequentially (increases readahead)
        if (posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL)) {
            LLAMA_LOG_WARN("warning: posix_fadvise(.., POSIX_FADV_SEQUENTIAL) failed: %s\n",
                    strerror(errno));
        }
        if (prefetch) { flags |= MAP_POPULATE; }
#endif
        addr = mmap(NULL, file->size, PROT_READ, flags, fd, 0);
        if (addr == MAP_FAILED) { // NOLINT
            throw std::runtime_error(format("mmap failed: %s", strerror(errno)));
        }

        if (prefetch > 0) {
            // advise the kernel to preload the mapped memory
            if (madvise(addr, std::min(file->size, prefetch), MADV_WILLNEED)) {
                fprintf(stderr, "warning: madvise(.., MADV_WILLNEED) failed: %s\n",
                        strerror(errno));
            }
        }
        if (numa) {
            // advise the kernel not to use readahead
            // (because the next page might not belong on the same node)
            if (madvise(addr, file->size, MADV_RANDOM)) {
                fprintf(stderr, "warning: madvise(.., MADV_RANDOM) failed: %s\n",
                        strerror(errno));
            }
        }

        // initialize list of mapped_fragments
        mapped_fragments.emplace_back(0, file->size);
    }

    static void align_range(size_t * first, size_t * last, size_t page_size) {
        // align first to the next page
        size_t offset_in_page = *first & (page_size - 1);
        size_t offset_to_page = offset_in_page == 0 ? 0 : page_size - offset_in_page;
        *first += offset_to_page;

        // align last to the previous page
        *last = *last & ~(page_size - 1);

        if (*last <= *first) {
            *last = *first;
        }
    }

    // partially unmap the file in the range [first, last)
    void unmap_fragment(size_t first, size_t last) {
        // note: this function must not be called multiple times with overlapping ranges
        // otherwise, there is a risk of invalidating addresses that have been repurposed for other mappings
        int page_size = sysconf(_SC_PAGESIZE);
        align_range(&first, &last, page_size);
        size_t len = last - first;

        if (len == 0) {
            return;
        }

        LM_GGML_ASSERT(first % page_size == 0);
        LM_GGML_ASSERT(last % page_size == 0);
        LM_GGML_ASSERT(last > first);

        void * next_page_start = (uint8_t *) addr + first;

        // unmap the range
        if (munmap(next_page_start, len)) {
            LLAMA_LOG_WARN("warning: munmap failed: %s\n", strerror(errno));
        }

        // update the list of mapped fragments to avoid unmapping the same range again in the destructor
        std::vector<std::pair<size_t, size_t>> new_mapped_fragments;
        for (const auto & frag : mapped_fragments) {
            if (frag.first < first && frag.second > last) {
                // the range is in the middle of the fragment, split it
                new_mapped_fragments.emplace_back(frag.first, first);
                new_mapped_fragments.emplace_back(last, frag.second);
            } else if (frag.first < first && frag.second > first) {
                // the range starts in the middle of the fragment
                new_mapped_fragments.emplace_back(frag.first, first);
            } else if (frag.first < last && frag.second > last) {
                // the range ends in the middle of the fragment
                new_mapped_fragments.emplace_back(last, frag.second);
            } else if (frag.first >= first && frag.second <= last) {
                // the range covers the entire fragment
            } else {
                // the range is outside the fragment
                new_mapped_fragments.push_back(frag);
            }
        }
        mapped_fragments = std::move(new_mapped_fragments);
    }

    ~llama_mmap() {
        for (const auto & frag : mapped_fragments) {
            if (munmap((char *) addr + frag.first, frag.second - frag.first)) {
                LLAMA_LOG_WARN("warning: munmap failed: %s\n", strerror(errno));
            }
        }
    }
#elif defined(_WIN32)
    static constexpr bool SUPPORTED = true;

    llama_mmap(struct llama_file * file, size_t prefetch = (size_t) -1, bool numa = false) {
        LM_GGML_UNUSED(numa);

        size = file->size;

        HANDLE hFile = (HANDLE) _get_osfhandle(_fileno(file->fp));

        HANDLE hMapping = CreateFileMappingA(hFile, NULL, PAGE_READONLY, 0, 0, NULL);

        if (hMapping == NULL) {
            DWORD error = GetLastError();
            throw std::runtime_error(format("CreateFileMappingA failed: %s", llama_format_win_err(error).c_str()));
        }

        addr = MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0);
        DWORD error = GetLastError();
        CloseHandle(hMapping);

        if (addr == NULL) {
            throw std::runtime_error(format("MapViewOfFile failed: %s", llama_format_win_err(error).c_str()));
        }

        if (prefetch > 0) {
#if _WIN32_WINNT >= 0x602
            // PrefetchVirtualMemory is only present on Windows 8 and above, so we dynamically load it
            BOOL (WINAPI *pPrefetchVirtualMemory) (HANDLE, ULONG_PTR, PWIN32_MEMORY_RANGE_ENTRY, ULONG);
            HMODULE hKernel32 = GetModuleHandleW(L"kernel32.dll");

            // may fail on pre-Windows 8 systems
            pPrefetchVirtualMemory = reinterpret_cast<decltype(pPrefetchVirtualMemory)> (GetProcAddress(hKernel32, "PrefetchVirtualMemory"));

            if (pPrefetchVirtualMemory) {
                // advise the kernel to preload the mapped memory
                WIN32_MEMORY_RANGE_ENTRY range;
                range.VirtualAddress = addr;
                range.NumberOfBytes = (SIZE_T) std::min(size, prefetch);
                if (!pPrefetchVirtualMemory(GetCurrentProcess(), 1, &range, 0)) {
                    LLAMA_LOG_WARN("warning: PrefetchVirtualMemory failed: %s\n",
                            llama_format_win_err(GetLastError()).c_str());
                }
            }
#else
            throw std::runtime_error("PrefetchVirtualMemory unavailable");
#endif
        }
    }

    void unmap_fragment(size_t first, size_t last) {
        // not supported
        LM_GGML_UNUSED(first);
        LM_GGML_UNUSED(last);
    }

    ~llama_mmap() {
        if (!UnmapViewOfFile(addr)) {
            LLAMA_LOG_WARN("warning: UnmapViewOfFile failed: %s\n",
                    llama_format_win_err(GetLastError()).c_str());
        }
    }
#else
    static constexpr bool SUPPORTED = false;

    llama_mmap(struct llama_file * file, size_t prefetch = -1, bool numa = false) {
        LM_GGML_UNUSED(file);
        LM_GGML_UNUSED(prefetch);
        LM_GGML_UNUSED(numa);

        throw std::runtime_error("mmap not supported");
    }

    void unmap_fragment(size_t first, size_t last) {
        LM_GGML_UNUSED(first);
        LM_GGML_UNUSED(last);

        throw std::runtime_error("mmap not supported");
    }
#endif
};
using llama_mmaps = std::vector<std::unique_ptr<llama_mmap>>;

// Represents some region of memory being locked using mlock or VirtualLock;
// will automatically unlock on destruction.
struct llama_mlock {
    void * addr = NULL;
    size_t size = 0;

    bool failed_already = false;

    llama_mlock() {}
    llama_mlock(const llama_mlock &) = delete;

    ~llama_mlock() {
        if (size) {
            raw_unlock(addr, size);
        }
    }

    void init(void * ptr) {
        LM_GGML_ASSERT(addr == NULL && size == 0); // NOLINT
        addr = ptr;
    }

    void grow_to(size_t target_size) {
        LM_GGML_ASSERT(addr);
        if (failed_already) {
            return;
        }
        size_t granularity = lock_granularity();
        target_size = (target_size + granularity - 1) & ~(granularity - 1);
        if (target_size > size) {
            if (raw_lock((uint8_t *) addr + size, target_size - size)) {
                size = target_size;
            } else {
                failed_already = true;
            }
        }
    }

#ifdef _POSIX_MEMLOCK_RANGE
    static constexpr bool SUPPORTED = true;

    static size_t lock_granularity() {
        return (size_t) sysconf(_SC_PAGESIZE);
    }

    #ifdef __APPLE__
        #define MLOCK_SUGGESTION \
            "Try increasing the sysctl values 'vm.user_wire_limit' and 'vm.global_user_wire_limit' and/or " \
            "decreasing 'vm.global_no_user_wire_amount'.  Also try increasing RLIMIT_MEMLOCK (ulimit -l).\n"
    #else
        #define MLOCK_SUGGESTION \
            "Try increasing RLIMIT_MEMLOCK ('ulimit -l' as root).\n"
    #endif

    bool raw_lock(const void * addr, size_t size) const {
        if (!mlock(addr, size)) {
            return true;
        }

        char* errmsg = std::strerror(errno);
        bool suggest = (errno == ENOMEM);

        // Check if the resource limit is fine after all
        struct rlimit lock_limit;
        if (suggest && getrlimit(RLIMIT_MEMLOCK, &lock_limit)) {
            suggest = false;
        }
        if (suggest && (lock_limit.rlim_max > lock_limit.rlim_cur + size)) {
            suggest = false;
        }

        LLAMA_LOG_WARN("warning: failed to mlock %zu-byte buffer (after previously locking %zu bytes): %s\n%s",
                size, this->size, errmsg, suggest ? MLOCK_SUGGESTION : "");
        return false;
    }

    #undef MLOCK_SUGGESTION

    static void raw_unlock(void * addr, size_t size) {
        if (munlock(addr, size)) {
            LLAMA_LOG_WARN("warning: failed to munlock buffer: %s\n", std::strerror(errno));
        }
    }
#elif defined(_WIN32)
    static constexpr bool SUPPORTED = true;

    static size_t lock_granularity() {
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        return (size_t) si.dwPageSize;
    }

    bool raw_lock(void * ptr, size_t len) const {
        for (int tries = 1; ; tries++) {
            if (VirtualLock(ptr, len)) {
                return true;
            }
            if (tries == 2) {
                LLAMA_LOG_WARN("warning: failed to VirtualLock %zu-byte buffer (after previously locking %zu bytes): %s\n",
                    len, size, llama_format_win_err(GetLastError()).c_str());
                return false;
            }

            // It failed but this was only the first try; increase the working
            // set size and try again.
            SIZE_T min_ws_size, max_ws_size;
            if (!GetProcessWorkingSetSize(GetCurrentProcess(), &min_ws_size, &max_ws_size)) {
                LLAMA_LOG_WARN("warning: GetProcessWorkingSetSize failed: %s\n",
                        llama_format_win_err(GetLastError()).c_str());
                return false;
            }
            // Per MSDN: "The maximum number of pages that a process can lock
            // is equal to the number of pages in its minimum working set minus
            // a small overhead."
            // Hopefully a megabyte is enough overhead:
            size_t increment = len + 1048576;
            // The minimum must be <= the maximum, so we need to increase both:
            min_ws_size += increment;
            max_ws_size += increment;
            if (!SetProcessWorkingSetSize(GetCurrentProcess(), min_ws_size, max_ws_size)) {
                LLAMA_LOG_WARN("warning: SetProcessWorkingSetSize failed: %s\n",
                        llama_format_win_err(GetLastError()).c_str());
                return false;
            }
        }
    }

    static void raw_unlock(void * ptr, size_t len) {
        if (!VirtualUnlock(ptr, len)) {
            LLAMA_LOG_WARN("warning: failed to VirtualUnlock buffer: %s\n",
                    llama_format_win_err(GetLastError()).c_str());
        }
    }
#else
    static constexpr bool SUPPORTED = false;

    static size_t lock_granularity() {
        return (size_t) 65536;
    }

    bool raw_lock(const void * addr, size_t len) const {
        LLAMA_LOG_WARN("warning: mlock not supported on this system\n");
        return false;
    }

    static void raw_unlock(const void * addr, size_t len) {}
#endif
};
using llama_mlocks = std::vector<std::unique_ptr<llama_mlock>>;

// NOTE: avoid ever using this except for building the token_to_piece caches
static std::string llama_token_to_piece(const struct llama_model * model, llama_token token, bool special) {
    std::string piece;
    piece.resize(piece.capacity());  // using string internal cache
    const int n_chars = llama_token_to_piece(model, token, &piece[0], piece.size(), 0, special);
    if (n_chars < 0) {
        piece.resize(-n_chars);
        int check = llama_token_to_piece(model, token, &piece[0], piece.size(), 0, special);
        LM_GGML_ASSERT(check == -n_chars);
    }
    else {
        piece.resize(n_chars);
    }

    return piece;
}

//
// globals
//

struct llama_logger_state {
    lm_ggml_log_callback log_callback = llama_log_callback_default;
    void * log_callback_user_data = nullptr;
};

static llama_logger_state g_logger_state;

// available llama models
enum e_model {
    MODEL_UNKNOWN,
    MODEL_14M,
    MODEL_17M,
    MODEL_22M,
    MODEL_33M,
    MODEL_60M,
    MODEL_70M,
    MODEL_80M,
    MODEL_109M,
    MODEL_137M,
    MODEL_160M,
    MODEL_220M,
    MODEL_250M,
    MODEL_270M,
    MODEL_335M,
    MODEL_410M,
    MODEL_450M,
    MODEL_770M,
    MODEL_780M,
    MODEL_0_5B,
    MODEL_1B,
    MODEL_1_3B,
    MODEL_1_4B,
    MODEL_1_5B,
    MODEL_1_6B,
    MODEL_2B,
    MODEL_2_8B,
    MODEL_3B,
    MODEL_4B,
    MODEL_6B,
    MODEL_6_9B,
    MODEL_7B,
    MODEL_8B,
    MODEL_9B,
    MODEL_11B,
    MODEL_12B,
    MODEL_13B,
    MODEL_14B,
    MODEL_15B,
    MODEL_16B,
    MODEL_20B,
    MODEL_30B,
    MODEL_34B,
    MODEL_35B,
    MODEL_40B,
    MODEL_65B,
    MODEL_70B,
    MODEL_236B,
    MODEL_314B,
    MODEL_SMALL,
    MODEL_MEDIUM,
    MODEL_LARGE,
    MODEL_XL,
    MODEL_A1_7B,
    MODEL_A2_7B,
    MODEL_8x7B,
    MODEL_8x22B,
    MODEL_16x12B,
    MODEL_10B_128x3_66B,
    MODEL_57B_A14B,
    MODEL_27B,
};

static const size_t kiB = 1024;
static const size_t MiB = 1024*kiB;
static const size_t GiB = 1024*MiB;

struct llama_hparams {
    bool vocab_only;
    bool rope_finetuned;
    bool use_par_res;
    bool swin_norm;

    uint32_t n_vocab;
    uint32_t n_ctx_train; // context size the model was trained on
    uint32_t n_embd;
    uint32_t n_layer;
    uint32_t n_rot;
    uint32_t n_swa = 0; // sliding window attention (SWA)
    uint32_t n_embd_head_k; // dimension of keys (d_k). d_q is assumed to be the same, but there are n_head q heads, and only n_head_kv k-v heads
    uint32_t n_embd_head_v; // dimension of values (d_v) aka n_embd_head
    uint32_t n_expert = 0;
    uint32_t n_expert_used = 0;
    uint32_t n_vocab_type = 0; // for BERT-style token types
    uint32_t n_rel_attn_bkts = 0;

    std::array<uint32_t, LLAMA_MAX_LAYERS> n_head_arr;
    std::array<uint32_t, LLAMA_MAX_LAYERS> n_head_kv_arr;
    std::array<uint32_t, LLAMA_MAX_LAYERS> n_ff_arr;

    uint32_t n_layer_dense_lead = 0;
    uint32_t n_lora_q = 0;
    uint32_t n_lora_kv = 0;
    uint32_t n_ff_exp = 0;
    uint32_t n_ff_shexp = 0;
    uint32_t n_expert_shared = 0;
    float    expert_weights_scale = 0.0;

    float f_norm_eps;
    float f_norm_rms_eps;

    float f_attn_logit_softcapping = 50.0f;
    float f_final_logit_softcapping = 30.0f;

    // for RWKV
    uint32_t rescale_every_n_layers = 0;
    uint32_t time_mix_extra_dim = 0;
    uint32_t time_decay_extra_dim = 0;
    uint32_t wkv_head_size = 0;

    float    rope_attn_factor = 1.0f;
    float    rope_freq_base_train;
    float    rope_freq_scale_train;
    uint32_t n_ctx_orig_yarn;
    float    rope_yarn_log_mul;

    // for State Space Models
    uint32_t ssm_d_conv  = 0;
    uint32_t ssm_d_inner = 0;
    uint32_t ssm_d_state = 0;
    uint32_t ssm_dt_rank = 0;
    bool ssm_dt_b_c_rms = false;

    float f_clamp_kqv      = 0.0f;
    float f_max_alibi_bias = 0.0f;
    float f_logit_scale    = 0.0f;

    // Additional scale factors (Granite/Granite MoE)
    float f_residual_scale  = 0.0f;
    float f_embedding_scale = 0.0f;
    float f_attention_scale = 0.0f;

    bool causal_attn   = true;
    bool use_alibi     = false;
    bool attn_soft_cap = false;

    // needed by encoder-decoder models (e.g. T5, FLAN-T5)
    // ref: https://github.com/ggerganov/llama.cpp/pull/8141
    llama_token dec_start_token_id = LLAMA_TOKEN_NULL;

    enum llama_pooling_type      pooling_type            = LLAMA_POOLING_TYPE_NONE;
    enum llama_rope_type         rope_type               = LLAMA_ROPE_TYPE_NONE;
    enum llama_rope_scaling_type rope_scaling_type_train = LLAMA_ROPE_SCALING_TYPE_NONE;

    bool operator!=(const llama_hparams & other) const {
        if (this->vocab_only    != other.vocab_only)    return true;
        if (this->n_vocab       != other.n_vocab)       return true;
        if (this->n_ctx_train   != other.n_ctx_train)   return true;
        if (this->n_embd        != other.n_embd)        return true;
        if (this->n_layer       != other.n_layer)       return true;
        if (this->n_rot         != other.n_rot)         return true;
        if (this->n_swa         != other.n_swa)         return true;
        if (this->n_embd_head_k != other.n_embd_head_k) return true;
        if (this->n_embd_head_v != other.n_embd_head_v) return true;
        if (this->n_expert      != other.n_expert)      return true;
        if (this->n_expert_used != other.n_expert_used) return true;

        if (this->n_head_arr    != other.n_head_arr)    return true;
        if (this->n_head_kv_arr != other.n_head_kv_arr) return true;
        if (this->n_ff_arr      != other.n_ff_arr)      return true;

        if (this->n_rel_attn_bkts    != other.n_rel_attn_bkts)    return true;
        if (this->n_layer_dense_lead != other.n_layer_dense_lead) return true;
        if (this->n_lora_q           != other.n_lora_q)           return true;
        if (this->n_lora_kv          != other.n_lora_kv)          return true;
        if (this->n_ff_exp           != other.n_ff_exp)           return true;
        if (this->n_ff_shexp         != other.n_ff_shexp)         return true;
        if (this->n_expert_shared    != other.n_expert_shared)    return true;

        if (this->rope_finetuned  != other.rope_finetuned)  return true;
        if (this->n_ctx_orig_yarn != other.n_ctx_orig_yarn) return true;

        if (this->ssm_d_conv  != other.ssm_d_conv)  return true;
        if (this->ssm_d_inner != other.ssm_d_inner) return true;
        if (this->ssm_d_state != other.ssm_d_state) return true;
        if (this->ssm_dt_rank != other.ssm_dt_rank) return true;
        if (this->ssm_dt_b_c_rms != other.ssm_dt_b_c_rms) return true;

        if (this->rescale_every_n_layers != other.rescale_every_n_layers) return true;
        if (this->time_mix_extra_dim     != other.time_mix_extra_dim)     return true;
        if (this->time_decay_extra_dim   != other.time_decay_extra_dim)   return true;
        if (this->wkv_head_size          != other.wkv_head_size)          return true;

        if (this->dec_start_token_id != other.dec_start_token_id) return true;

        const float EPSILON = 1e-9f;

        if (!is_float_close(this->f_norm_eps,            other.f_norm_eps,            EPSILON)) return true;
        if (!is_float_close(this->f_norm_rms_eps,        other.f_norm_rms_eps,        EPSILON)) return true;
        if (!is_float_close(this->rope_attn_factor,      other.rope_attn_factor,      EPSILON)) return true;
        if (!is_float_close(this->rope_freq_base_train,  other.rope_freq_base_train,  EPSILON)) return true;
        if (!is_float_close(this->rope_freq_scale_train, other.rope_freq_scale_train, EPSILON)) return true;
        if (!is_float_close(this->expert_weights_scale,  other.expert_weights_scale,  EPSILON)) return true;
        if (!is_float_close(this->rope_yarn_log_mul,     other.rope_yarn_log_mul,     EPSILON)) return true;
        if (!is_float_close(this->f_residual_scale,      other.f_residual_scale,      EPSILON)) return true;
        if (!is_float_close(this->f_embedding_scale,     other.f_embedding_scale,     EPSILON)) return true;
        if (!is_float_close(this->f_attention_scale,     other.f_attention_scale,     EPSILON)) return true;

        return false;
    }

    uint32_t n_head(uint32_t il = 0) const {
        if (il < n_layer) {
            return n_head_arr[il];
        }

        LM_GGML_ABORT("fatal error");
    }

    uint32_t n_head_kv(uint32_t il = 0) const {
        if (il < n_layer) {
            return n_head_kv_arr[il];
        }

        LM_GGML_ABORT("fatal error");
    }

    uint32_t n_ff(uint32_t il = 0) const {
        if (il < n_layer) {
            return n_ff_arr[il];
        }

        LM_GGML_ABORT("fatal error");
    }

    uint32_t n_gqa(uint32_t il = 0) const {
        const uint32_t n_head    = this->n_head(il);
        const uint32_t n_head_kv = this->n_head_kv(il);

        if (n_head_kv == 0) {
            return 0;
        }

        return n_head/n_head_kv;
    }

    uint32_t n_embd_k_gqa(uint32_t il = 0) const { // dimension of key embeddings across all k-v heads
        const uint32_t n_head_kv = this->n_head_kv(il);

        return n_embd_head_k * n_head_kv;
    }

    uint32_t n_embd_v_gqa(uint32_t il = 0) const { // dimension of value embeddings across all k-v heads
        const uint32_t n_head_kv = this->n_head_kv(il);

        return n_embd_head_v * n_head_kv;
    }

    uint32_t n_embd_k_s() const { // dimension of the rolling state embeddings
        // corresponds to Mamba's conv_states size or RWKV's token_shift states size
        if (wkv_head_size != 0) {
            // for RWKV models
            return 2 * n_embd;
        } else {
            // TODO: maybe support other convolution strides than 1
            // NOTE: since the first column of the conv_state is shifted out each time, it's not actually needed
            return (ssm_d_conv > 0 ? ssm_d_conv - 1 : 0) * ssm_d_inner;
        }
    }

    uint32_t n_embd_v_s() const { // dimension of the recurrent state embeddings
        if (wkv_head_size != 0) {
            // corresponds to RWKV's wkv_states size
            return n_embd * wkv_head_size;
        } else {
            // corresponds to Mamba's ssm_states size
            return ssm_d_state * ssm_d_inner;
        }
    }
};

static_assert(std::is_trivially_copyable<llama_hparams>::value, "llama_hparams must be trivially copyable");

struct llama_cparams {
    uint32_t n_ctx;           // context size used during inference
    uint32_t n_batch;
    uint32_t n_ubatch;
    uint32_t n_seq_max;
    int      n_threads;       // number of threads to use for generation
    int      n_threads_batch; // number of threads to use for batch processing

    float rope_freq_base;
    float rope_freq_scale;

    uint32_t n_ctx_orig_yarn;
    // These hyperparameters are not exposed in GGUF, because all
    // existing YaRN models use the same values for them.
    float yarn_ext_factor;
    float yarn_attn_factor;
    float yarn_beta_fast;
    float yarn_beta_slow;
    float defrag_thold;

    bool embeddings;
    bool causal_attn;
    bool offload_kqv;
    bool flash_attn;
    bool no_perf;

    enum llama_pooling_type pooling_type;

    lm_ggml_backend_sched_eval_callback cb_eval;
    void * cb_eval_user_data;
};

// TODO: separate into "llama_layer_enc" and "llama_layer_dec"
struct llama_layer {
    llama_layer() {
        // initialize all pointers to NULL
        std::memset(this, 0, sizeof(*this));
    }

    // normalization
    struct lm_ggml_tensor * attn_norm;
    struct lm_ggml_tensor * attn_norm_b;
    struct lm_ggml_tensor * attn_norm_2;
    struct lm_ggml_tensor * attn_norm_2_b;
    struct lm_ggml_tensor * attn_q_norm;
    struct lm_ggml_tensor * attn_q_norm_b;
    struct lm_ggml_tensor * attn_k_norm;
    struct lm_ggml_tensor * attn_k_norm_b;
    struct lm_ggml_tensor * attn_out_norm;
    struct lm_ggml_tensor * attn_out_norm_b;
    struct lm_ggml_tensor * attn_q_a_norm;
    struct lm_ggml_tensor * attn_kv_a_norm;
    struct lm_ggml_tensor * attn_sub_norm;
    struct lm_ggml_tensor * attn_post_norm;
    struct lm_ggml_tensor * ffn_sub_norm;
    struct lm_ggml_tensor * attn_norm_cross;
    struct lm_ggml_tensor * attn_norm_enc;

    // attention
    struct lm_ggml_tensor * wq;
    struct lm_ggml_tensor * wk;
    struct lm_ggml_tensor * wv;
    struct lm_ggml_tensor * wo;
    struct lm_ggml_tensor * wqkv;
    struct lm_ggml_tensor * wq_a;
    struct lm_ggml_tensor * wq_b;
    struct lm_ggml_tensor * wkv_a_mqa;
    struct lm_ggml_tensor * wkv_b;
    struct lm_ggml_tensor * wq_cross;
    struct lm_ggml_tensor * wk_cross;
    struct lm_ggml_tensor * wv_cross;
    struct lm_ggml_tensor * wo_cross;
    struct lm_ggml_tensor * wq_enc;
    struct lm_ggml_tensor * wk_enc;
    struct lm_ggml_tensor * wv_enc;
    struct lm_ggml_tensor * wo_enc;

    // attention bias
    struct lm_ggml_tensor * bq;
    struct lm_ggml_tensor * bk;
    struct lm_ggml_tensor * bv;
    struct lm_ggml_tensor * bo;
    struct lm_ggml_tensor * bqkv;

    // relative position bias
    struct lm_ggml_tensor * attn_rel_b;
    struct lm_ggml_tensor * attn_rel_b_enc;
    struct lm_ggml_tensor * attn_rel_b_cross;

    // normalization
    struct lm_ggml_tensor * ffn_norm;
    struct lm_ggml_tensor * ffn_norm_b;
    struct lm_ggml_tensor * ffn_post_norm;
    struct lm_ggml_tensor * layer_out_norm;
    struct lm_ggml_tensor * layer_out_norm_b;
    struct lm_ggml_tensor * ffn_norm_exps;
    struct lm_ggml_tensor * ffn_norm_enc;

    // ff
    struct lm_ggml_tensor * ffn_gate; // w1
    struct lm_ggml_tensor * ffn_down; // w2
    struct lm_ggml_tensor * ffn_up;   // w3
    struct lm_ggml_tensor * ffn_gate_enc;
    struct lm_ggml_tensor * ffn_down_enc;
    struct lm_ggml_tensor * ffn_up_enc;

    // ff MoE
    struct lm_ggml_tensor * ffn_gate_inp;
    struct lm_ggml_tensor * ffn_gate_exps;
    struct lm_ggml_tensor * ffn_down_exps;
    struct lm_ggml_tensor * ffn_up_exps ;

    // ff shared expert (shexp)
    struct lm_ggml_tensor * ffn_gate_inp_shexp;
    struct lm_ggml_tensor * ffn_gate_shexp;
    struct lm_ggml_tensor * ffn_down_shexp;
    struct lm_ggml_tensor * ffn_up_shexp;

    // ff bias
    struct lm_ggml_tensor * ffn_gate_b;
    struct lm_ggml_tensor * ffn_down_b; // b2
    struct lm_ggml_tensor * ffn_up_b; // b3
    struct lm_ggml_tensor * ffn_act;

    // mamba proj
    struct lm_ggml_tensor * ssm_in;
    struct lm_ggml_tensor * ssm_x;
    struct lm_ggml_tensor * ssm_dt;
    struct lm_ggml_tensor * ssm_out;

    // mamba
    struct lm_ggml_tensor * ssm_conv1d;
    struct lm_ggml_tensor * ssm_a;
    struct lm_ggml_tensor * ssm_d;

    // mamba bias
    struct lm_ggml_tensor * ssm_conv1d_b;
    struct lm_ggml_tensor * ssm_dt_b;

    // rwkv
    struct lm_ggml_tensor * time_mix_w1;
    struct lm_ggml_tensor * time_mix_w2;
    struct lm_ggml_tensor * time_mix_lerp_x;
    struct lm_ggml_tensor * time_mix_lerp_w;
    struct lm_ggml_tensor * time_mix_lerp_k;
    struct lm_ggml_tensor * time_mix_lerp_v;
    struct lm_ggml_tensor * time_mix_lerp_r;
    struct lm_ggml_tensor * time_mix_lerp_g;

    struct lm_ggml_tensor * time_mix_first;
    struct lm_ggml_tensor * time_mix_decay;
    struct lm_ggml_tensor * time_mix_decay_w1;
    struct lm_ggml_tensor * time_mix_decay_w2;
    struct lm_ggml_tensor * time_mix_key;
    struct lm_ggml_tensor * time_mix_value;
    struct lm_ggml_tensor * time_mix_receptance;
    struct lm_ggml_tensor * time_mix_gate;

    struct lm_ggml_tensor * time_mix_ln;
    struct lm_ggml_tensor * time_mix_ln_b;
    struct lm_ggml_tensor * time_mix_output;

    struct lm_ggml_tensor * channel_mix_lerp_k;
    struct lm_ggml_tensor * channel_mix_lerp_r;

    struct lm_ggml_tensor * channel_mix_key;
    struct lm_ggml_tensor * channel_mix_receptance;
    struct lm_ggml_tensor * channel_mix_value;

    // long rope factors
    struct lm_ggml_tensor * rope_long  = nullptr;
    struct lm_ggml_tensor * rope_short = nullptr;
    struct lm_ggml_tensor * rope_freqs = nullptr;

    // bitnet scale
    struct lm_ggml_tensor * wq_scale;
    struct lm_ggml_tensor * wk_scale;
    struct lm_ggml_tensor * wv_scale;
    struct lm_ggml_tensor * wo_scale;
    struct lm_ggml_tensor * ffn_gate_scale;
    struct lm_ggml_tensor * ffn_up_scale;
    struct lm_ggml_tensor * ffn_down_scale;
};

// very similar to llama_batch,
// but has more metadata about sequences
struct llama_ubatch {
    bool equal_seqs;
    // TODO: whole_seqs for embeddings?

    uint32_t n_tokens; // total tokens (n_seq_tokens * n_seqs)
    uint32_t n_seq_tokens; // tokens per sequence
    uint32_t n_seqs;

    llama_token  *  token;    // [n_tokens]
    float        *  embd;     // [n_embd, n_tokens]
    llama_pos    *  pos;      // [n_tokens]
    int32_t      *  n_seq_id; // [n_seqs]
    llama_seq_id ** seq_id;   // [n_seqs]
    int8_t       *  output;   // [n_tokens]
};

struct llama_kv_cell {
    llama_pos pos   = -1;
    llama_pos delta = 0;
    int32_t   src   = -1; // used by recurrent state models to copy states
    int32_t   tail  = -1;

    std::set<llama_seq_id> seq_id;

    bool has_seq_id(const llama_seq_id & id) const {
        return seq_id.find(id) != seq_id.end();
    }

    bool is_empty() const {
        return seq_id.empty();
    }

    bool is_same_seq(const llama_kv_cell & other) const {
        return seq_id == other.seq_id;
    }
};

// ring-buffer of cached KV data
struct llama_kv_cache {
    bool has_shift = false;
    bool do_defrag = false;
    bool recurrent = false; // with recurrent state models, a cell can hold the state for more than one past token
    bool v_trans   = true;  // the value tensor is transposed

    // Note: The value of head isn't only used to optimize searching
    // for a free KV slot. llama_decode_internal also uses it, so it
    // cannot be freely changed after a slot has been allocated.
    uint32_t head = 0;
    uint32_t size = 0;
    uint32_t used = 0; // used cells (i.e. at least one seq_id)

    // computed before each graph build
    uint32_t n = 0;

    lm_ggml_type type_k = LM_GGML_TYPE_F16;
    lm_ggml_type type_v = LM_GGML_TYPE_F16;

    std::vector<llama_kv_cell> cells;

    std::vector<struct lm_ggml_tensor *> k_l; // per layer
    std::vector<struct lm_ggml_tensor *> v_l;

    std::vector<lm_ggml_context_ptr> ctxs;
    std::vector<lm_ggml_backend_buffer_ptr> bufs;

    size_t total_size() {
        size_t size = 0;
        for (auto & buf : bufs) {
            size += lm_ggml_backend_buffer_get_size(buf.get());
        }
        return size;
    }
};

struct llama_control_vector {
    std::vector<struct lm_ggml_tensor *> tensors; // per layer
    std::vector<lm_ggml_context_ptr> ctxs;
    std::vector<lm_ggml_backend_buffer_ptr> bufs;

    int32_t layer_start = -1;
    int32_t layer_end   = -1;

    struct lm_ggml_tensor * tensor_for(int il) const {
        if (il < 0 || il < layer_start || il > layer_end || (size_t) il >= tensors.size()) {
            return nullptr;
        }
        return tensors[il];
    }

    struct lm_ggml_tensor * apply_to(struct lm_ggml_context * ctx, struct lm_ggml_tensor * cur, int  il) const {
        lm_ggml_tensor * layer_dir = tensor_for(il);
        if (layer_dir != nullptr) {
            cur = lm_ggml_add(ctx, cur, layer_dir);
        }
        return cur;
    }
};

struct llama_model {
    e_model     type  = MODEL_UNKNOWN;
    llm_arch    arch  = LLM_ARCH_UNKNOWN;
    llama_ftype ftype = LLAMA_FTYPE_ALL_F32;

    std::string name = "n/a";

    llama_hparams hparams = {};
    llama_vocab   vocab;

    struct lm_ggml_tensor * tok_embd = nullptr;
    struct lm_ggml_tensor * type_embd = nullptr;
    struct lm_ggml_tensor * pos_embd = nullptr;
    struct lm_ggml_tensor * tok_norm = nullptr;
    struct lm_ggml_tensor * tok_norm_b = nullptr;

    struct lm_ggml_tensor * output_norm = nullptr;
    struct lm_ggml_tensor * output_norm_b = nullptr;
    struct lm_ggml_tensor * output = nullptr;
    struct lm_ggml_tensor * output_b = nullptr;
    struct lm_ggml_tensor * output_norm_enc = nullptr;

    // classifier
    struct lm_ggml_tensor * cls = nullptr;
    struct lm_ggml_tensor * cls_b = nullptr;
    struct lm_ggml_tensor * cls_out   = nullptr;
    struct lm_ggml_tensor * cls_out_b = nullptr;

    std::vector<llama_layer> layers;

    // gguf metadata
    std::unordered_map<std::string, std::string> lm_gguf_kv;

    llama_split_mode split_mode;
    int main_gpu;
    int n_gpu_layers;

    std::vector<std::string> rpc_servers;

    // list of devices used in this model
    std::vector<lm_ggml_backend_dev_t> devices;


    // lists of buffer types used for each layer
    using buft_list_t = std::vector<std::pair<lm_ggml_backend_dev_t, lm_ggml_backend_buffer_type_t>>;
    buft_list_t cpu_buft_list;
    std::map<lm_ggml_backend_dev_t, buft_list_t> gpu_buft_list;

    struct layer_dev {
        lm_ggml_backend_dev_t dev;
        buft_list_t * buft_list;
    };
    layer_dev dev_input = {};
    layer_dev dev_output = {};
    std::vector<layer_dev> dev_layer;

    // contexts where the model tensors metadata is stored
    std::vector<lm_ggml_context_ptr> ctxs;

    // the model memory buffers for the tensor data
    std::vector<lm_ggml_backend_buffer_ptr> bufs;

    // model memory mapped files
    llama_mmaps mappings;

    // objects representing data potentially being locked in memory
    llama_mlocks mlock_bufs;
    llama_mlocks mlock_mmaps;

    // for quantize-stats only
    std::vector<std::pair<std::string, struct lm_ggml_tensor *>> tensors_by_name;

    int64_t t_load_us  = 0;
    int64_t t_start_us = 0;

    // total number of parameters in the model
    uint64_t n_elements = 0;

    // total size of all the tensors in the model in bytes
    size_t  n_bytes     = 0;

    // keep track of loaded lora adapters
    std::set<struct llama_lora_adapter *> lora_adapters;

    ~llama_model() {
       while (!lora_adapters.empty()) {
            llama_lora_adapter_free(*lora_adapters.begin());
        }
    }
};

struct llama_sbatch_seq {
    int32_t n_seq_id;
    llama_seq_id * seq_id;
    size_t offset;
    size_t length;
};

// sequence-length-aware batch splitting
struct llama_sbatch {
    // tokens left in this batch
    size_t n_tokens;

    size_t n_embd;

    bool logits_all; // TODO: remove once lctx.logits_all is removed too

    // sorted indices into the batch
    std::vector<size_t> ids;
    // batch indices of the output
    std::vector<size_t> out_ids;
    std::vector<llama_sbatch_seq> seq;
    const llama_batch * batch = nullptr;

    // buffers for the ubatch
    std::vector<llama_token>    ubatch_token;
    std::vector<float>          ubatch_embd;
    std::vector<llama_pos>      ubatch_pos;
    std::vector<int32_t>        ubatch_n_seq_id;
    std::vector<llama_seq_id *> ubatch_seq_id;
    std::vector<int8_t>         ubatch_output;

    llama_ubatch reserve_ubatch(size_t n_ubatch, bool has_embd = false) {
        // clear empty sequences
        // the previous ubatch is assumed to be gone,
        // so nothing should refer to values in these sequences anymore.
        for (size_t i = seq.size(); i-- > 0;) {
            if (seq[i].length == 0) {
                seq.pop_back();
            } else {
                break;
            }
        }
        ubatch_token.resize(!has_embd ? n_ubatch : 0);
        ubatch_embd.resize(has_embd ? n_embd * n_ubatch : 0);
        ubatch_pos.resize(n_ubatch);
        ubatch_n_seq_id.resize(n_ubatch);
        ubatch_seq_id.resize(n_ubatch);
        ubatch_output.resize(n_ubatch);
        llama_ubatch ubatch = {
            /*equal_seqs   =*/ true,
            /*n_tokens     =*/ 0,
            /*n_seq_tokens =*/ 0,
            /*n_seqs       =*/ 0,
            /*token        =*/ !has_embd ? ubatch_token.data() : nullptr,
            /*embd         =*/ has_embd  ? ubatch_embd.data()  : nullptr,
            /*pos          =*/ ubatch_pos.data(),
            /*n_seq_id     =*/ ubatch_n_seq_id.data(),
            /*seq_id       =*/ ubatch_seq_id.data(),
            /*output       =*/ ubatch_output.data(),
        };
        return ubatch;
    }

    void add_seq_to_ubatch(llama_ubatch & ubatch, llama_sbatch_seq & seq, size_t length) {
        LM_GGML_ASSERT(batch != nullptr);
        LM_GGML_ASSERT(length <= seq.length);
        // Can only add sequences of equal lengths to a batch,
        // otherwise it isn't clear to which sequence a token belongs
        LM_GGML_ASSERT(seq.n_seq_id == 0 || ubatch.n_seqs == 0 || length == (size_t) ubatch.n_tokens / ubatch.n_seqs);
        LM_GGML_ASSERT((seq.n_seq_id != 0) == ubatch.equal_seqs);
        // NOTE: loops are separated for cache-friendliness
        if (batch->token) {
            if (ubatch.equal_seqs) {
                for (size_t i = 0; i < length; ++i) {
                    ubatch.token[ubatch.n_tokens + i] = batch->token[ids[seq.offset + i]];
                }
            } else {
                // simple split
                ubatch.token = batch->token + seq.offset;
            }
        } else {
            ubatch.token = nullptr;
        }
        if (batch->embd) {
            if (ubatch.equal_seqs) {
                for (size_t i = 0; i < length; ++i) {
                    memcpy(
                        ubatch.embd + n_embd * (ubatch.n_tokens + i),
                        batch->embd + n_embd * ids[seq.offset + i],
                        n_embd * sizeof(float)
                    );
                }
            } else {
                // simple split
                ubatch.embd = batch->embd + (n_embd * seq.offset);
            }
        } else {
            ubatch.embd = nullptr;
        }
        if (ubatch.equal_seqs) {
            for (size_t i = 0; i < length; ++i) {
                ubatch.pos[ubatch.n_tokens + i] = batch->pos[ids[seq.offset + i]];
            }
        } else {
            // simple split
            ubatch.pos = batch->pos + seq.offset;
        }
        if (ubatch.equal_seqs) {
            ubatch.n_seq_id[ubatch.n_seqs] = seq.n_seq_id;
            if (seq.seq_id) {
                ubatch.seq_id[ubatch.n_seqs] = seq.seq_id;
            }
        } else {
            // simple split
            if (batch->n_seq_id) {
                ubatch.n_seq_id = batch->n_seq_id + seq.offset;
            } else {
                for (size_t i = 0; i < length; ++i) {
                    ubatch.n_seq_id[ubatch.n_seqs + i] = 1;
                }
            }
            if (batch->seq_id) {
                ubatch.seq_id = batch->seq_id + seq.offset;
            }
        }
        if (logits_all) {
            for (size_t i = 0; i < length; ++i) {
                ubatch.output[ubatch.n_tokens + i] = 1;
                out_ids.push_back(ids[seq.offset + i]);
            }
        } else if (batch->logits) {
            if (ubatch.equal_seqs) {
                for (size_t i = 0; i < length; ++i) {
                    size_t id = ids[seq.offset + i];
                    int8_t is_output = batch->logits[id];
                    ubatch.output[ubatch.n_tokens + i] = is_output;
                    if (is_output) { out_ids.push_back(id); }
                }
            } else {
                // simple split
                ubatch.output = batch->logits + seq.offset;
                for (size_t i = 0; i < length; ++i) {
                    if (ubatch.output[i] != 0) { out_ids.push_back(seq.offset + i); }
                }
            }
        } else {
            // only get last output
            for (size_t i = 0; i < length; ++i) {
                size_t id = ids[seq.offset + i];
                int8_t is_last = id == ids.size() - 1;
                ubatch.output[ubatch.n_tokens + i] = is_last;
                if (is_last) { out_ids.push_back(id); }
            }
        }
        if (ubatch.n_tokens == 0 && ubatch.n_seqs == 0) {
            ubatch.n_seq_tokens = ubatch.equal_seqs ? length : 1;
        }
        ubatch.n_tokens += length;
        ubatch.n_seqs += ubatch.equal_seqs ? 1 : length; // virtual sequences for simple splits
        seq.offset += length;
        seq.length -= length;
        n_tokens -= length;
        LM_GGML_ASSERT(ubatch.n_tokens == ubatch.n_seq_tokens * ubatch.n_seqs);
    }

    // simple split, unknown number of sequences of unequal lengths
    llama_ubatch split_simple(size_t n_ubatch) {
        n_ubatch = n_tokens < n_ubatch ? n_tokens : n_ubatch;
        llama_ubatch ubatch = reserve_ubatch(n_ubatch, /* has_embd */ batch->embd != nullptr);
        ubatch.equal_seqs = false;
        if (!seq.empty()) {
            llama_sbatch_seq & s = seq[0];
            size_t length = s.length < n_ubatch ? s.length : n_ubatch;
            LM_GGML_ASSERT(seq.size() == 1 && s.n_seq_id == 0); // don't mix with other splits
            add_seq_to_ubatch(ubatch, s, length);
        }
        return ubatch;
    }

    // make batches of equal-length sequences
    llama_ubatch split_equal(size_t n_ubatch) {
        n_ubatch = n_tokens < n_ubatch ? n_tokens : n_ubatch;
        llama_ubatch ubatch = reserve_ubatch(n_ubatch, /* has_embd */ batch->embd != nullptr);
        if (!seq.empty()) {
            size_t length = 0;
            size_t n_tokens_in_ubatch = 0;
            LM_GGML_ASSERT(seq[0].n_seq_id > 0); // should not be mixed with simple splits
            // smallest first, because it's easier to split this way;
            // starting from the end to pop in constant time.
            for (size_t i = seq.size(); i-- > 0;) {
                llama_sbatch_seq & s = seq[i];
                LM_GGML_ASSERT(s.length > 0);
                if (length == 0) {
                    length = s.length < n_ubatch ? s.length : n_ubatch;
                }
                add_seq_to_ubatch(ubatch, s, length);
                n_tokens_in_ubatch += length;
                // shared prompts can't be mixed with any of their sequences,
                // so it's safer to compute them in their own ubatch
                if (s.n_seq_id > 1) { break; }
                // stop when there isn't enough space for another sequence
                if (length + n_tokens_in_ubatch > n_ubatch) { break; }
            }
        }
        return ubatch;
    }

    // sequence-wise split
    llama_ubatch split_seq(size_t n_ubatch) {
        n_ubatch = n_tokens < n_ubatch ? n_tokens : n_ubatch;
        llama_ubatch ubatch = reserve_ubatch(n_ubatch, /* has_embd */ batch->embd != nullptr);
        if (!seq.empty()) {
            llama_sbatch_seq & s = seq[seq.size() - 1];
            size_t length = s.length < n_ubatch ? s.length : n_ubatch;
            LM_GGML_ASSERT(s.n_seq_id > 0); // should not be mixed with simple splits
            add_seq_to_ubatch(ubatch, s, length);
        }
        return ubatch;
    }

    void from_batch(const llama_batch & batch, const size_t n_embd, const bool simple_split = false, const bool logits_all = false) {
        LM_GGML_ASSERT(batch.n_tokens >= 0);
        this->batch = &batch;
        this->n_embd = n_embd;
        this->logits_all = logits_all;

        n_tokens = batch.n_tokens;
        ids.resize(n_tokens);
        out_ids.clear();
        // TODO: reserve out_ids and seq

        for (size_t i = 0; i < n_tokens; ++i) {
            ids[i] = i;
        }
        if (simple_split) {
            seq.resize(1);
            llama_sbatch_seq & s = seq[0];
            s.n_seq_id = 0;
            s.seq_id = nullptr;
            s.offset = 0;
            s.length = n_tokens;
            return;
        }
        std::sort(ids.begin(), ids.end(),
            [&batch](size_t a, size_t b) {
                int32_t n_seq_a = batch.n_seq_id ? batch.n_seq_id[a] : 1;
                int32_t n_seq_b = batch.n_seq_id ? batch.n_seq_id[b] : 1;
                // sort by seq_id, then by pos
                if (n_seq_a == n_seq_b) {
                    if (batch.seq_id) {
                        for (int32_t i = 0; i < n_seq_a; ++i) {
                            llama_seq_id seq_id_a = batch.seq_id[a][i];
                            llama_seq_id seq_id_b = batch.seq_id[b][i];
                            // smaller seq_ids go first
                            if (seq_id_a != seq_id_b) {
                                return seq_id_a < seq_id_b;
                            }
                        }
                    }
                    // when all else is equal, sort by pos
                    if (batch.pos) {
                        return batch.pos[a] < batch.pos[b];
                    }
                    // no pos, sort by id
                    return a < b;
                }
                // shared prompts go first
                return n_seq_a > n_seq_b;
            }
        );
        // init seq
        llama_sbatch_seq * last_seq = nullptr;

        for (size_t i = 0; i < n_tokens; ++i) {
            const size_t bi = ids[i];
            const int32_t n_seqs = batch.n_seq_id[bi];
            llama_seq_id * seq_ids = batch.seq_id[bi];
            if (last_seq != nullptr) {
                bool same = n_seqs == last_seq->n_seq_id;
                for (int32_t j = 0; same && j < n_seqs; ++j) {
                    if (seq_ids[j] != last_seq->seq_id[j]) {
                        same = false;
                    }
                }
                if (same) {
                    last_seq->length += 1;
                    continue;
                }
            }
            llama_sbatch_seq new_seq = {n_seqs, seq_ids, i, 1};
            seq.push_back(new_seq);
            last_seq = &seq.back();
        }
        // keep shared prompts first at the end, then sort by length descending.
        std::sort(seq.begin(), seq.end(),
            [](llama_sbatch_seq & a, llama_sbatch_seq & b) {
                if (a.n_seq_id == b.n_seq_id) {
                    return a.length > b.length;
                }
                return a.n_seq_id < b.n_seq_id;
            }
        );
    }
};

struct llama_context {
    llama_context(const llama_model & model)
        : model(model)
        , t_start_us(model.t_start_us)
        , t_load_us(model.t_load_us) {}

    const struct llama_model & model;

    struct llama_cparams        cparams;
    struct llama_sbatch         sbatch;
    struct llama_kv_cache       kv_self;
    struct llama_control_vector cvec;

    std::unordered_map<struct llama_lora_adapter *, float> lora_adapters;

    std::vector<lm_ggml_backend_ptr> backends;
    std::vector<std::pair<lm_ggml_backend_t, lm_ggml_backend_set_n_threads_t>> set_n_threads_fns;

    lm_ggml_backend_t backend_cpu = nullptr;

    lm_ggml_threadpool_t threadpool       = nullptr;
    lm_ggml_threadpool_t threadpool_batch = nullptr;

    bool has_evaluated_once = false;

    mutable int64_t t_start_us;
    mutable int64_t t_load_us;
    mutable int64_t t_p_eval_us = 0;
    mutable int64_t t_eval_us   = 0;

    mutable int64_t t_compute_start_us = 0;
    mutable int64_t n_queued_tokens = 0;

    mutable int32_t n_p_eval = 0; // number of tokens in eval calls for the prompt (with batch size > 1)
    mutable int32_t n_eval   = 0; // number of eval calls

    // host buffer for the model output (logits and embeddings)
    lm_ggml_backend_buffer_ptr buf_output;

    // decode output (2-dimensional array: [n_outputs][n_vocab])
    size_t  logits_size = 0; // capacity (of floats) for logits
    float * logits      = nullptr;

    std::vector<int32_t> output_ids; // map batch token positions to ids of the logits and embd buffers
    size_t  output_size = 0; // capacity (of tokens positions) for the output buffers
    int32_t n_outputs   = 0; // number of actually-used outputs in the current ubatch or last logical batch

    bool logits_all = false;

    // embeddings output (2-dimensional array: [n_outputs][n_embd])
    // populated only when pooling_type == LLAMA_POOLING_TYPE_NONE
    size_t  embd_size = 0; // capacity (of floats) for embeddings
    float * embd      = nullptr;

    // sequence embeddings output (map of [n_embd] vectors)
    // populated only when pooling_type != LLAMA_POOLING_TYPE_NONE
    std::map<llama_seq_id, std::vector<float>> embd_seq;

    // whether we are computing encoder output or decoder output
    bool is_encoding = false;

    // output of the encoder part of the encoder-decoder models
    std::vector<float> embd_enc;
    std::vector<std::set<llama_seq_id>> seq_ids_enc;

    // memory buffers used to evaluate the model
    std::vector<uint8_t> buf_compute_meta;
    lm_ggml_backend_sched_ptr sched;

    lm_ggml_abort_callback abort_callback      = nullptr;
    void *              abort_callback_data = nullptr;

    // input tensors
    struct lm_ggml_tensor * inp_tokens;      // I32 [n_batch]
    struct lm_ggml_tensor * inp_embd;        // F32 [n_embd, n_batch]
    struct lm_ggml_tensor * inp_pos;         // I32 [n_batch]
    struct lm_ggml_tensor * inp_out_ids;     // I32 [n_outputs]
    struct lm_ggml_tensor * inp_KQ_mask;     // F32 [kv_size, n_batch]
    struct lm_ggml_tensor * inp_KQ_mask_swa; // F32 [kv_size, n_batch]
    struct lm_ggml_tensor * inp_K_shift;     // I32 [kv_size]
    struct lm_ggml_tensor * inp_mean;        // F32 [n_batch, n_batch]
    struct lm_ggml_tensor * inp_cls;         // I32 [n_batch]
    struct lm_ggml_tensor * inp_s_copy;      // I32 [kv_size]
    struct lm_ggml_tensor * inp_s_mask;      // F32 [1, n_kv]
    struct lm_ggml_tensor * inp_s_seq;       // I32 [n_kv, n_batch]
    struct lm_ggml_tensor * inp_pos_bucket;    // I32 [n_batch|n_kv, n_batch]
    struct lm_ggml_tensor * inp_embd_enc;      // F32 [n_embd, n_outputs_enc]
    struct lm_ggml_tensor * inp_KQ_mask_cross; // F32 [n_outputs_enc, n_batch]
};

struct llama_lora_weight {
    struct lm_ggml_tensor * a = nullptr;
    struct lm_ggml_tensor * b = nullptr;
    llama_lora_weight() = default;
    llama_lora_weight(struct lm_ggml_tensor * a, struct lm_ggml_tensor * b): a(a), b(b) {}
};

struct llama_lora_adapter {
    struct llama_model * base_model;
    // map tensor name to lora_a_b
    std::unordered_map<std::string, struct llama_lora_weight> ab_map;
    std::vector<lm_ggml_context_ptr> ctxs;
    std::vector<lm_ggml_backend_buffer_ptr> bufs;

    float alpha;

    llama_lora_adapter(struct llama_model * base_model): base_model(base_model) {
        base_model->lora_adapters.insert(this);
    }

    llama_lora_weight * get_weight(struct lm_ggml_tensor * w) {
        std::string name(w->name);
        auto pos = ab_map.find(name);
        if (ab_map.find(name) != ab_map.end()) {
            return &pos->second;
        }
        return nullptr;
    }

    ~llama_lora_adapter() {
        auto pos = base_model->lora_adapters.find(this);
        if (pos != base_model->lora_adapters.end()) {
            base_model->lora_adapters.erase(pos);
        }
    }
};

static int llama_get_device_count(const llama_model & model) {
    return (int) model.devices.size();
}

template<typename F>
static bool buft_supported(lm_ggml_backend_buffer_type_t buft, lm_ggml_backend_dev_t dev, F & fn) {
    lm_ggml_init_params params = {
        /*.mem_size   =*/ lm_ggml_tensor_overhead()*8,
        /*.mem_buffer =*/ NULL,
        /*.no_alloc   =*/ true,
    };
    lm_ggml_context_ptr ctx { lm_ggml_init(params) };
    if (!ctx) {
        throw std::runtime_error(format("failed to create ggml context"));
    }

    lm_ggml_backend_buffer_ptr buf { lm_ggml_backend_buft_alloc_buffer(buft, 0) };
    lm_ggml_tensor * op_tensor = fn(ctx.get());
    for (int i = 0; i < LM_GGML_MAX_SRC; i++) {
        if (op_tensor->src[i] != nullptr) {
            assert(op_tensor->src[i]->buffer == nullptr);
            op_tensor->src[i]->buffer = buf.get();
        }
    }
    bool op_supported = lm_ggml_backend_dev_supports_op(dev, op_tensor);

    return op_supported;
}

template<typename F>
static lm_ggml_backend_buffer_type_t select_buft(const llama_model::buft_list_t & buft_list, const F & fn) {
    for (const auto & cur : buft_list) {
        lm_ggml_backend_dev_t cur_dev = cur.first;
        lm_ggml_backend_buffer_type_t cur_buft = cur.second;
        if (buft_supported(cur_buft, cur_dev, fn)) {
            return cur_buft;
        }
    }
    throw std::runtime_error(format("no suitable buffer type found"));
}

//
// kv cache helpers
//

static bool llama_kv_cache_init(
             struct llama_kv_cache & cache,
               const llama_context * ctx,
                         lm_ggml_type   type_k,
                         lm_ggml_type   type_v,
                          uint32_t   kv_size,
                              bool   offload) {
    const llama_model & model = ctx->model;
    const llama_cparams & cparams = ctx->cparams;

    const struct llama_hparams & hparams = model.hparams;

    const int64_t  n_layer = hparams.n_layer;

    cache.has_shift = false;

    cache.recurrent = llama_model_is_recurrent(&model);
    cache.v_trans   = !cache.recurrent && !cparams.flash_attn;

    cache.head = 0;
    cache.size = kv_size;
    cache.used = 0;

    cache.type_k = type_k;
    cache.type_v = type_v;

    cache.cells.clear();
    cache.cells.resize(kv_size);

    // create a context for each buffer type
    std::map<lm_ggml_backend_buffer_type_t, lm_ggml_context *> ctx_map;
    auto ctx_for_buft = [&](lm_ggml_backend_buffer_type_t buft) -> lm_ggml_context * {
        auto it = ctx_map.find(buft);
        if (it == ctx_map.end()) {
            struct lm_ggml_init_params params = {
                /*.mem_size   =*/ size_t(2u*n_layer*lm_ggml_tensor_overhead()),
                /*.mem_buffer =*/ NULL,
                /*.no_alloc   =*/ true,
            };
            lm_ggml_context * ctx = lm_ggml_init(params);
            if (!ctx) {
                return nullptr;
            }
            ctx_map[buft] = ctx;
            cache.ctxs.emplace_back(ctx);
            return ctx;
        }
        return it->second;
    };

    cache.k_l.reserve(n_layer);
    cache.v_l.reserve(n_layer);

    for (int i = 0; i < (int) n_layer; i++) {
        const uint32_t n_embd_k_gqa = hparams.n_embd_k_gqa(i) + hparams.n_embd_k_s();
        const uint32_t n_embd_v_gqa = hparams.n_embd_v_gqa(i) + hparams.n_embd_v_s();

        lm_ggml_backend_buffer_type_t buft;
        if (offload) {
            auto * dev = model.dev_layer.at(i).dev;
            buft = lm_ggml_backend_dev_buffer_type(dev);
        } else {
            buft = lm_ggml_backend_cpu_buffer_type();
        }
        lm_ggml_context * ctx = ctx_for_buft(buft);

        if (!ctx) {
            LLAMA_LOG_ERROR("%s: failed to create ggml context for kv cache\n", __func__);
            return false;
        }

        lm_ggml_tensor * k = lm_ggml_new_tensor_1d(ctx, type_k, n_embd_k_gqa*kv_size);
        lm_ggml_tensor * v = lm_ggml_new_tensor_1d(ctx, type_v, n_embd_v_gqa*kv_size);
        lm_ggml_format_name(k, "cache_k_l%d", i);
        lm_ggml_format_name(v, "cache_v_l%d", i);
        cache.k_l.push_back(k);
        cache.v_l.push_back(v);
    }

    // allocate tensors and initialize the buffers to avoid NaNs in the padding
    for (auto it : ctx_map) {
        auto * buft = it.first;
        auto * ctx  = it.second;

        lm_ggml_backend_buffer_t buf = lm_ggml_backend_alloc_ctx_tensors_from_buft(ctx, buft);
        if (!buf) {
            LLAMA_LOG_ERROR("%s: failed to allocate buffer for kv cache\n", __func__);
            return false;
        }
        lm_ggml_backend_buffer_clear(buf, 0);
        LLAMA_LOG_INFO("%s: %10s KV buffer size = %8.2f MiB\n", __func__, lm_ggml_backend_buffer_name(buf), lm_ggml_backend_buffer_get_size(buf)/1024.0/1024.0);
        cache.bufs.emplace_back(buf);
    }

    return true;
}

// a structure holds information about the slot found in llama_kv_cache_find_slot
struct llama_kv_cache_slot_info {
    std::pair<uint32_t, uint32_t> boundaries; // slot boundaries [begin, end)
    bool found = false;                       // the slot was found

    explicit llama_kv_cache_slot_info(bool found_) : found{found_} {}
    llama_kv_cache_slot_info(uint32_t begin, uint32_t end) : boundaries{begin, end}, found{true} {}

    operator bool() const { return found; }
};
static const llama_kv_cache_slot_info llama_kv_cache_slot_info_failed{false};

// find an empty slot of size "n_tokens" in the cache
// updates the cache head
// returns a structure holding information about the slot found
// Note: On success, it's important that cache.head points
// to the first cell of the slot.
static struct llama_kv_cache_slot_info llama_kv_cache_find_slot(
           struct llama_kv_cache & cache,
       const struct llama_ubatch & batch) {
    const uint32_t n_tokens = batch.n_tokens;
    const uint32_t n_seqs   = batch.n_seqs;
    const uint32_t n_seq_tokens = batch.n_seq_tokens;

    if (cache.recurrent) {
        // For recurrent state architectures (like Mamba or RWKV),
        // each cache cell can store the state for a whole sequence.
        // A slot should be always be contiguous.

        // can only process batches with an equal number of new tokens in each sequence
        LM_GGML_ASSERT(batch.equal_seqs);

        int32_t min = cache.size - 1;
        int32_t max = 0;

        // everything should fit if all seq_ids are smaller than the max
        for (uint32_t s = 0; s < n_seqs; ++s) {
            const uint32_t n_seq_id = batch.n_seq_id[s];
            for (uint32_t j = 0; j < n_seq_id; ++j) {
                const llama_seq_id seq_id = batch.seq_id[s][j];

                if (seq_id < 0 || (uint32_t) seq_id >= cache.size) {
                    // too big seq_id
                    // TODO: would it be possible to resize the cache instead?
                    LLAMA_LOG_ERROR("%s: seq_id=%d >= n_seq_max=%d Try using a bigger --parallel value\n", __func__, seq_id, cache.size);
                    return llama_kv_cache_slot_info_failed;
                }
                if (j > 0) {
                    llama_kv_cell & seq = cache.cells[seq_id];
                    if (seq.tail >= 0) {
                        llama_kv_cell & cell = cache.cells[seq.tail];
                        // clear cells from seq_ids that become shared
                        // (should not normally happen, but let's handle it anyway)
                        cell.seq_id.erase(seq_id);
                        seq.tail = -1;
                        if (cell.seq_id.empty()) {
                            cell.pos = -1;
                            cell.src = -1;
                            cache.used -= 1;
                        }
                    }
                }
            }
        }

#ifndef NDEBUG
        {
            std::vector<int32_t> tails_verif;
            tails_verif.assign(cache.size, -1);
            for (uint32_t i = 0; i < cache.size; ++i) {
                llama_kv_cell & cell = cache.cells[i];
                for (llama_seq_id seq_id : cell.seq_id) {
                    if (tails_verif[seq_id] != -1) {
                        LLAMA_LOG_ERROR("%s: duplicate tail for seq_id %d in cell %d and %d\n", __func__, seq_id, i, tails_verif[seq_id]);
                    }
                    tails_verif[seq_id] = i;
                }
            }
            for (uint32_t i = 0; i < cache.size; ++i) {
                if (tails_verif[i] != cache.cells[i].tail) {
                    LLAMA_LOG_ERROR("%s: wrong tail for seq_id %d, (%d instead of %d)\n", __func__, i, cache.cells[i].tail, tails_verif[i]);
                }
            }
        }
#endif

        // find next empty cell
        uint32_t next_empty_cell = cache.head;

        for (uint32_t i = 0; i < cache.size; ++i) {
            if (next_empty_cell >= cache.size) { next_empty_cell -= cache.size; }
            llama_kv_cell & cell = cache.cells[next_empty_cell];
            if (cell.is_empty()) { break; }
            next_empty_cell += 1;
        }

        // find usable cell range
        for (uint32_t s = 0; s < n_seqs; ++s) {
            const llama_seq_id seq_id = batch.seq_id[s][0];
            llama_kv_cell & seq_meta = cache.cells[seq_id];
            bool has_cell = false;
            if (seq_meta.tail >= 0) {
                llama_kv_cell & cell = cache.cells[seq_meta.tail];
                LM_GGML_ASSERT(cell.has_seq_id(seq_id));
                // does this seq_id "own" the cell?
                if (cell.seq_id.size() == 1) { has_cell = true; }
            }
            if (!has_cell) {
                llama_kv_cell & empty_cell = cache.cells[next_empty_cell];
                LM_GGML_ASSERT(empty_cell.is_empty());
                // copy old tail into the empty cell
                if (seq_meta.tail >= 0) {
                    llama_kv_cell & orig_cell = cache.cells[seq_meta.tail];
                    empty_cell.pos = orig_cell.pos;
                    empty_cell.src = orig_cell.src;
                    orig_cell.seq_id.erase(seq_id);
                    empty_cell.seq_id.insert(seq_id); // will be overwritten
                }
                seq_meta.tail = next_empty_cell;
                // find next empty cell
                if (s + 1 < n_seqs) {
                    next_empty_cell += 1;
                    for (uint32_t i = 0; i < cache.size; ++i) {
                        if (next_empty_cell >= cache.size) { next_empty_cell -= cache.size; }
                        llama_kv_cell & cell = cache.cells[next_empty_cell];
                        if (cell.is_empty()) { break; }
                        next_empty_cell += 1;
                    }
                }
            }
            if (min > seq_meta.tail) { min = seq_meta.tail; }
            if (max < seq_meta.tail) { max = seq_meta.tail; }
        }

        // gather and re-order
        for (uint32_t s = 0; s < n_seqs; ++s) {
            int32_t dst_id = s + min;
            int32_t src_id = cache.cells[batch.seq_id[s][0]].tail;
            if (dst_id != src_id) {
                llama_kv_cell & dst_cell = cache.cells[dst_id];
                llama_kv_cell & src_cell = cache.cells[src_id];

                std::swap(dst_cell.pos, src_cell.pos);
                std::swap(dst_cell.src, src_cell.src);
                std::swap(dst_cell.seq_id, src_cell.seq_id);

                // swap tails (assuming they NEVER overlap)
                for (const llama_seq_id seq_id : src_cell.seq_id) {
                    cache.cells[seq_id].tail = src_id;
                }
                for (const llama_seq_id seq_id : dst_cell.seq_id) {
                    cache.cells[seq_id].tail = dst_id;
                }
            }
        }

        // update the pos of the used seqs
        for (uint32_t s = 0; s < n_seqs; ++s) {
            const llama_pos last_pos = batch.pos[n_seq_tokens * s + n_seq_tokens - 1];
            int32_t cell_id = s + min;
            llama_kv_cell & cell = cache.cells[cell_id];

            if (cell.pos >= 0 && last_pos != cell.pos + (llama_pos) n_seq_tokens) {
                // What should happen when the pos backtracks or skips a value?
                // Clearing the state mid-batch would require special-casing which isn't done.
                LLAMA_LOG_WARN("%s: non-consecutive token position %d after %d for sequence %d with %u new tokens\n",
                    __func__, last_pos, cell.pos, batch.seq_id[s][0], n_seq_tokens);
            }
            cell.pos = last_pos;
            cell.seq_id.clear();
            for (int32_t j = 0; j < batch.n_seq_id[s]; ++j) {
                const llama_seq_id seq_id = batch.seq_id[s][j];
                cell.seq_id.insert(seq_id);
                cache.cells[seq_id].tail = cell_id;
            }
        }

        // allow getting the range of used cells, from head to head + n
        cache.head = min;
        cache.n    = max - min + 1;
        cache.used = std::count_if(cache.cells.begin(), cache.cells.end(),
            [](const llama_kv_cell& cell){ return !cell.is_empty(); });

        // sanity check
        return llama_kv_cache_slot_info(cache.n >= n_seqs);
    }
    // otherwise, one cell per token.

    if (n_tokens > cache.size) {
        LLAMA_LOG_ERROR("%s: n_tokens=%d > cache.size=%d\n", __func__, n_tokens, cache.size);
        return llama_kv_cache_slot_info_failed;
    }

    uint32_t n_tested = 0;

    while (true) {
        if (cache.head + n_tokens > cache.size) {
            n_tested += cache.size - cache.head;
            cache.head = 0;
            continue;
        }

        bool found = true;
        for (uint32_t i = 0; i < n_tokens; i++) {
            if (cache.cells[cache.head + i].pos >= 0) {
                found = false;
                cache.head += i + 1;
                n_tested   += i + 1;
                break;
            }
        }

        if (found) {
            break;
        }

        if (n_tested >= cache.size) {
            //LLAMA_LOG_ERROR("%s: failed to find a slot for %d tokens\n", __func__, n_tokens);
            return llama_kv_cache_slot_info_failed;
        }
    }

    for (uint32_t s = 0; s < n_seqs; s++) {
        for (uint32_t i = 0; i < n_seq_tokens; ++i) {
            uint32_t k = s*n_seq_tokens + i;
            cache.cells[cache.head + k].pos = batch.pos[k];

            for (int32_t j = 0; j < batch.n_seq_id[s]; j++) {
                cache.cells[cache.head + k].seq_id.insert(batch.seq_id[s][j]);
            }
        }
    }

    cache.used += n_tokens;

    return llama_kv_cache_slot_info(cache.head, cache.head + n_tokens);
}

// find how many cells are currently in use
static uint32_t llama_kv_cache_cell_max(const struct llama_kv_cache & cache) {
    for (uint32_t i = cache.size; i > 0; --i) {
        const llama_kv_cell & cell = cache.cells[i - 1];

        if (cell.pos >= 0 && !cell.is_empty()) {
            return i;
        }
    }

    return 0;
}

static void llama_kv_cache_clear(struct llama_kv_cache & cache) {
    for (int32_t i = 0; i < (int32_t) cache.size; ++i) {
        cache.cells[i].pos = -1;
        cache.cells[i].seq_id.clear();
        cache.cells[i].src = -1;
        cache.cells[i].tail = -1;
    }
    cache.head = 0;
    cache.used = 0;

    for (auto & buf : cache.bufs) {
        lm_ggml_backend_buffer_clear(buf.get(), 0);
    }
}

static bool llama_kv_cache_seq_rm(
        struct llama_kv_cache & cache,
                 llama_seq_id   seq_id,
                    llama_pos   p0,
                    llama_pos   p1) {
    uint32_t new_head = cache.size;

    if (p0 < 0) p0 = 0;
    if (p1 < 0) p1 = std::numeric_limits<llama_pos>::max();

    // models like Mamba or RWKV can't have a state partially erased
    if (cache.recurrent) {
        if (seq_id >= (int64_t) cache.size) {
            // could be fatal
            return false;
        }
        if (0 <= seq_id) {
            int32_t & tail_id = cache.cells[seq_id].tail;
            if (tail_id >= 0) {
                const llama_kv_cell & cell = cache.cells[tail_id];
                // partial intersection is invalid
                if ((0 < p0 && p0 <= cell.pos) || (0 < p1 && p1 <= cell.pos)) {
                    return false;
                }
                // invalidate tails which will be cleared
                if (p0 <= cell.pos && cell.pos < p1) {
                    tail_id = -1;
                }
            }
        } else {
            // seq_id is negative, then the range should include everything or nothing
            if (p0 != p1 && (p0 != 0 || p1 != std::numeric_limits<llama_pos>::max())) {
                return false;
            }
        }
    }

    for (uint32_t i = 0; i < cache.size; ++i) {
        if (cache.cells[i].pos >= p0 && cache.cells[i].pos < p1) {
            if (seq_id < 0) {
                cache.cells[i].seq_id.clear();
            } else if (cache.cells[i].has_seq_id(seq_id)) {
                cache.cells[i].seq_id.erase(seq_id);
            } else {
                continue;
            }
            if (cache.cells[i].is_empty()) {
                // keep count of the number of used cells
                if (cache.cells[i].pos >= 0) cache.used--;

                cache.cells[i].pos = -1;
                cache.cells[i].src = -1;
                if (new_head == cache.size) new_head = i;
            }
        }
    }

    // If we freed up a slot, set head to it so searching can start there.
    if (new_head != cache.size && new_head < cache.head) cache.head = new_head;

    return true;
}

static void llama_kv_cache_seq_cp(
        struct llama_kv_cache & cache,
                 llama_seq_id   seq_id_src,
                 llama_seq_id   seq_id_dst,
                    llama_pos   p0,
                    llama_pos   p1) {
    if (p0 < 0) p0 = 0;
    if (p1 < 0) p1 = std::numeric_limits<llama_pos>::max();

    if (cache.recurrent) {
        if ((uint32_t) seq_id_dst < cache.size && (uint32_t) seq_id_src < cache.size) {
            llama_kv_cell & tail_src = cache.cells[seq_id_src];
            llama_kv_cell & tail_dst = cache.cells[seq_id_dst];
            if (tail_dst.tail >= 0) {
                // clear destination seq_id if it wasn't empty
                llama_kv_cell & cell_dst = cache.cells[tail_dst.tail];

                cell_dst.seq_id.erase(seq_id_dst);
                tail_dst.tail = -1;
                if (cell_dst.seq_id.empty()) {
                    cell_dst.pos = -1;
                    cell_dst.delta = -1;
                    cell_dst.src = -1;
                    cache.used -= 1;
                }
            }
            if (tail_src.tail >= 0) {
                llama_kv_cell & cell_src = cache.cells[tail_src.tail];

                cell_src.seq_id.insert(seq_id_dst);
                tail_dst.tail = tail_src.tail;
            }
        }

        return;
    }
    // otherwise, this is the KV cache of a Transformer-like model

    cache.head = 0;

    for (uint32_t i = 0; i < cache.size; ++i) {
        if (cache.cells[i].has_seq_id(seq_id_src) && cache.cells[i].pos >= p0 && cache.cells[i].pos < p1) {
            cache.cells[i].seq_id.insert(seq_id_dst);
        }
    }
}

static void llama_kv_cache_seq_keep(struct llama_kv_cache & cache, llama_seq_id seq_id) {
    uint32_t new_head = cache.size;

    for (uint32_t i = 0; i < cache.size; ++i) {
        if (cache.recurrent && (llama_seq_id) i != seq_id) {
            cache.cells[i].tail = -1;
        }
        if (!cache.cells[i].has_seq_id(seq_id)) {
            if (cache.cells[i].pos >= 0) cache.used--;
            cache.cells[i].pos = -1;
            cache.cells[i].src = -1;
            cache.cells[i].seq_id.clear();
            if (new_head == cache.size) new_head = i;
        } else {
            cache.cells[i].seq_id.clear();
            cache.cells[i].seq_id.insert(seq_id);
        }
    }

    // If we freed up a slot, set head to it so searching can start there.
    if (new_head != cache.size && new_head < cache.head) cache.head = new_head;
}

static void llama_kv_cache_seq_add(
        struct llama_kv_cache & cache,
                 llama_seq_id   seq_id,
                    llama_pos   p0,
                    llama_pos   p1,
                    llama_pos   delta) {
    uint32_t new_head = cache.size;

    if (p0 < 0) p0 = 0;
    if (p1 < 0) p1 = std::numeric_limits<llama_pos>::max();
    // If there is no range then return early to avoid looping over the cache.
    if (p0 == p1) return;

    if (cache.recurrent) {
        // for Mamba-like or RWKV models, only the pos needs to be shifted
        if (0 <= seq_id && seq_id < (int64_t) cache.size) {
            const int32_t tail_id = cache.cells[seq_id].tail;
            if (tail_id >= 0) {
                llama_kv_cell & cell = cache.cells[tail_id];
                if (cell.has_seq_id(seq_id) && p0 <= cell.pos && cell.pos < p1) {
                    cell.pos += delta;
                }
            }
        }
        return;
    }

    for (uint32_t i = 0; i < cache.size; ++i) {
        if (cache.cells[i].has_seq_id(seq_id) && cache.cells[i].pos >= p0 && cache.cells[i].pos < p1) {
            cache.has_shift = true;
            cache.cells[i].pos   += delta;
            cache.cells[i].delta += delta;

            if (cache.cells[i].pos < 0) {
                if (!cache.cells[i].is_empty()) {
                    cache.used--;
                }
                cache.cells[i].pos = -1;
                cache.cells[i].seq_id.clear();
                if (new_head == cache.size) {
                    new_head = i;
                }
            }
        }
    }

    // If we freed up a slot, set head to it so searching can start there.
    // Otherwise we just start the next search from the beginning.
    cache.head = new_head != cache.size ? new_head : 0;
}

static void llama_kv_cache_seq_div(
        struct llama_kv_cache & cache,
                 llama_seq_id   seq_id,
                    llama_pos   p0,
                    llama_pos   p1,
                          int   d) {
    if (p0 < 0) p0 = 0;
    if (p1 < 0) p1 = std::numeric_limits<llama_pos>::max();
    // If there is no range then return early to avoid looping over the cache.
    if (p0 == p1) return;

    if (cache.recurrent) {
        // for Mamba-like or RWKV models, only the pos needs to be changed
        if (0 <= seq_id && seq_id < (int64_t) cache.size) {
            const int32_t tail_id = cache.cells[seq_id].tail;
            if (tail_id >= 0) {
                llama_kv_cell & cell = cache.cells[tail_id];
                if (cell.has_seq_id(seq_id) && p0 <= cell.pos && cell.pos < p1) {
                    cell.pos /= d;
                }
            }
        }
        return;
    }

    for (uint32_t i = 0; i < cache.size; ++i) {
        if (cache.cells[i].has_seq_id(seq_id) && cache.cells[i].pos >= p0 && cache.cells[i].pos < p1) {
            cache.has_shift = true;

            {
                llama_pos p_old = cache.cells[i].pos;
                cache.cells[i].pos   /= d;
                cache.cells[i].delta += cache.cells[i].pos - p_old;
            }
        }
    }
}

static llama_pos llama_kv_cache_seq_pos_max(struct llama_kv_cache & cache, llama_seq_id seq_id) {
    llama_pos result = 0;

    for (uint32_t i = 0; i < cache.size; ++i) {
        if (cache.cells[i].has_seq_id(seq_id)) {
            result = std::max(result, cache.cells[i].pos);
        }
    }

    return result;
}

static void llama_kv_cache_defrag(struct llama_kv_cache & cache) {
    if (!cache.recurrent) {
        cache.do_defrag = true;
    }
}

static uint32_t llama_kv_cache_get_padding(const struct llama_cparams & cparams) {
    // the FA kernels require padding to avoid extra runtime boundary checks
    return cparams.flash_attn ? 256u : 32u;
}

// saves the kv_cache state for future recovery.
// used to rollback llama_kv_cache_find_slot changes.
struct llama_kv_slot_restorer {
    struct llama_kv_cache_state {
        uint32_t head = 0;
        uint32_t n    = 0;
    } old_state;

    // for non-recurrent models only
    // list of slots to restore
    std::vector<std::pair<uint32_t, uint32_t>> slot_boundaries;

    bool do_restore = false;

    explicit llama_kv_slot_restorer(const struct llama_kv_cache & cache) {
        old_state.head  = cache.head;
        old_state.n     = cache.n;
    }

    // saves a slot information for future restoration
    void save(const struct llama_kv_cache_slot_info & slot) {
        if (slot) {
            do_restore = true;
            if (slot.boundaries.first != slot.boundaries.second) {
                slot_boundaries.push_back(slot.boundaries);
            }
        }
    }

    // must be explicitly called to restore the kv_cache state
    // and rollback changes from all llama_kv_cache_find_slot calls
    void restore(struct llama_kv_cache & cache) {
        if (do_restore) {
            cache.head  = old_state.head;
            cache.n     = old_state.n;

            if (cache.recurrent) { // recurrent models like Mamba or RWKV can't have a state partially erased
                llama_kv_cache_seq_rm(cache, -1, -1, -1);
            } else {
                for (auto & slot : slot_boundaries) {
                    llama_kv_cache_seq_rm(cache, -1, slot.first, slot.second);
                }
            }
        }
    }
};

//
// model loading and saving
//

enum llama_fver {
    LM_GGUF_FILE_VERSION_V1 = 1,
    LM_GGUF_FILE_VERSION_V2 = 2,
    LM_GGUF_FILE_VERSION_V3 = 3,
};

static const char * llama_file_version_name(llama_fver version) {
    switch (version) {
        case LM_GGUF_FILE_VERSION_V1: return "GGUF V1 (support until nov 2023)";
        case LM_GGUF_FILE_VERSION_V2: return "GGUF V2";
        case LM_GGUF_FILE_VERSION_V3: return "GGUF V3 (latest)";
    }

    return "unknown";
}

static std::string llama_format_tensor_shape(const std::vector<int64_t> & ne) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%5" PRId64, ne.at(0));
    for (size_t i = 1; i < ne.size(); i++) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ", %5" PRId64, ne.at(i));
    }
    return buf;
}

static std::string llama_format_tensor_shape(const struct lm_ggml_tensor * t) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%5" PRId64, t->ne[0]);
    for (int i = 1; i < LM_GGML_MAX_DIMS; i++) {
        snprintf(buf + strlen(buf), sizeof(buf) - strlen(buf), ", %5" PRId64, t->ne[i]);
    }
    return buf;
}

namespace GGUFMeta {
    template <typename T, lm_gguf_type gt_, T (*gfun)(const lm_gguf_context *, const int)>
    struct GKV_Base_Type {
        static constexpr lm_gguf_type gt = gt_;

        static T getter(const lm_gguf_context * ctx, const int kid) {
            return gfun(ctx, kid);
        }
    };

    template<typename T> struct GKV_Base;

    template<> struct GKV_Base<bool        >: GKV_Base_Type<bool,         LM_GGUF_TYPE_BOOL,    lm_gguf_get_val_bool> {};
    template<> struct GKV_Base<uint8_t     >: GKV_Base_Type<uint8_t,      LM_GGUF_TYPE_UINT8,   lm_gguf_get_val_u8  > {};
    template<> struct GKV_Base<uint16_t    >: GKV_Base_Type<uint16_t,     LM_GGUF_TYPE_UINT16,  lm_gguf_get_val_u16 > {};
    template<> struct GKV_Base<uint32_t    >: GKV_Base_Type<uint32_t,     LM_GGUF_TYPE_UINT32,  lm_gguf_get_val_u32 > {};
    template<> struct GKV_Base<uint64_t    >: GKV_Base_Type<uint64_t,     LM_GGUF_TYPE_UINT64,  lm_gguf_get_val_u64 > {};
    template<> struct GKV_Base<int8_t      >: GKV_Base_Type<int8_t,       LM_GGUF_TYPE_INT8,    lm_gguf_get_val_i8  > {};
    template<> struct GKV_Base<int16_t     >: GKV_Base_Type<int16_t,      LM_GGUF_TYPE_INT16,   lm_gguf_get_val_i16 > {};
    template<> struct GKV_Base<int32_t     >: GKV_Base_Type<int32_t,      LM_GGUF_TYPE_INT32,   lm_gguf_get_val_i32 > {};
    template<> struct GKV_Base<int64_t     >: GKV_Base_Type<int64_t,      LM_GGUF_TYPE_INT64,   lm_gguf_get_val_i64 > {};
    template<> struct GKV_Base<float       >: GKV_Base_Type<float,        LM_GGUF_TYPE_FLOAT32, lm_gguf_get_val_f32 > {};
    template<> struct GKV_Base<double      >: GKV_Base_Type<double,       LM_GGUF_TYPE_FLOAT64, lm_gguf_get_val_f64 > {};
    template<> struct GKV_Base<const char *>: GKV_Base_Type<const char *, LM_GGUF_TYPE_STRING,  lm_gguf_get_val_str > {};

    template<> struct GKV_Base<std::string> {
        static constexpr lm_gguf_type gt = LM_GGUF_TYPE_STRING;

        static std::string getter(const lm_gguf_context * ctx, const int kid) {
            return lm_gguf_get_val_str(ctx, kid);
        }
    };

    struct ArrayInfo {
        const lm_gguf_type gt;
        const size_t length;
        const void * data;
    };

    template<> struct GKV_Base<ArrayInfo> {
        public:
        static constexpr lm_gguf_type gt = LM_GGUF_TYPE_ARRAY;
        static ArrayInfo getter(const lm_gguf_context *ctx, const int k) {
            return ArrayInfo {
                lm_gguf_get_arr_type(ctx, k),
                size_t(lm_gguf_get_arr_n(ctx, k)),
                lm_gguf_get_arr_data(ctx, k),
            };
        }
    };

    template<typename T>
    class GKV : public GKV_Base<T> {
        GKV() = delete;

        public:
        static T get_kv(const lm_gguf_context * ctx, const int k) {
            const enum lm_gguf_type kt = lm_gguf_get_kv_type(ctx, k);

            if (kt != GKV::gt) {
                throw std::runtime_error(format("key %s has wrong type %s but expected type %s",
                    lm_gguf_get_key(ctx, k), lm_gguf_type_name(kt), lm_gguf_type_name(GKV::gt)));
            }
            return GKV::getter(ctx, k);
        }

        static const char * override_type_to_str(const llama_model_kv_override_type ty) {
            switch (ty) {
                case LLAMA_KV_OVERRIDE_TYPE_BOOL:  return "bool";
                case LLAMA_KV_OVERRIDE_TYPE_INT:   return "int";
                case LLAMA_KV_OVERRIDE_TYPE_FLOAT: return "float";
                case LLAMA_KV_OVERRIDE_TYPE_STR:   return "str";
            }
            return "unknown";
        }

        static bool validate_override(const llama_model_kv_override_type expected_type, const struct llama_model_kv_override * ovrd) {
            if (!ovrd) { return false; }
            if (ovrd->tag == expected_type) {
                LLAMA_LOG_INFO("%s: Using metadata override (%5s) '%s' = ",
                    __func__, override_type_to_str(ovrd->tag), ovrd->key);
                switch (ovrd->tag) {
                    case LLAMA_KV_OVERRIDE_TYPE_BOOL:  {
                        LLAMA_LOG_INFO("%s\n", ovrd->val_bool ? "true" : "false");
                    } break;
                    case LLAMA_KV_OVERRIDE_TYPE_INT:   {
                        LLAMA_LOG_INFO("%" PRId64 "\n", ovrd->val_i64);
                    } break;
                    case LLAMA_KV_OVERRIDE_TYPE_FLOAT: {
                        LLAMA_LOG_INFO("%.6f\n", ovrd->val_f64);
                    } break;
                    case LLAMA_KV_OVERRIDE_TYPE_STR: {
                        LLAMA_LOG_INFO("%s\n", ovrd->val_str);
                    } break;
                    default:
                        // Shouldn't be possible to end up here, but just in case...
                        throw std::runtime_error(
                            format("Unsupported attempt to override %s type for metadata key %s\n",
                                override_type_to_str(ovrd->tag), ovrd->key));
                }
                return true;
            }
            LLAMA_LOG_WARN("%s: Warning: Bad metadata override type for key '%s', expected %s but got %s\n",
                __func__, ovrd->key, override_type_to_str(expected_type), override_type_to_str(ovrd->tag));
            return false;
        }

        template<typename OT>
        static typename std::enable_if<std::is_same<OT, bool>::value, bool>::type
        try_override(OT & target, const struct llama_model_kv_override * ovrd) {
            if (validate_override(LLAMA_KV_OVERRIDE_TYPE_BOOL, ovrd)) {
                target = ovrd->val_bool;
                return true;
            }
            return false;
        }

        template<typename OT>
        static typename std::enable_if<!std::is_same<OT, bool>::value && std::is_integral<OT>::value, bool>::type
        try_override(OT & target, const struct llama_model_kv_override * ovrd) {
            if (validate_override(LLAMA_KV_OVERRIDE_TYPE_INT, ovrd)) {
                target = ovrd->val_i64;
                return true;
            }
            return false;
        }

        template<typename OT>
        static typename std::enable_if<std::is_floating_point<OT>::value, bool>::type
        try_override(T & target, const struct llama_model_kv_override * ovrd) {
            if (validate_override(LLAMA_KV_OVERRIDE_TYPE_FLOAT, ovrd)) {
                target = ovrd->val_f64;
                return true;
            }
            return false;
        }

        template<typename OT>
        static typename std::enable_if<std::is_same<OT, std::string>::value, bool>::type
        try_override(T & target, const struct llama_model_kv_override * ovrd) {
            if (validate_override(LLAMA_KV_OVERRIDE_TYPE_STR, ovrd)) {
                target = ovrd->val_str;
                return true;
            }
            return false;
        }

        static bool set(const lm_gguf_context * ctx, const int k, T & target, const struct llama_model_kv_override * ovrd = nullptr) {
            if (try_override<T>(target, ovrd)) {
                return true;
            }
            if (k < 0) { return false; }
            target = get_kv(ctx, k);
            return true;
        }

        static bool set(const lm_gguf_context * ctx, const char * key, T & target, const struct llama_model_kv_override * ovrd = nullptr) {
            return set(ctx, lm_gguf_find_key(ctx, key), target, ovrd);
        }

        static bool set(const lm_gguf_context * ctx, const std::string & key, T & target, const struct llama_model_kv_override * ovrd = nullptr) {
            return set(ctx, key.c_str(), target, ovrd);
        }
    };
}

using llama_buf_map = std::unordered_map<uint32_t, lm_ggml_backend_buffer_t>;

static size_t llama_model_max_nodes(const llama_model & model) {
    return std::max<size_t>(8192, model.tensors_by_name.size()*5);
}

struct llama_model_loader {
    int n_kv      = 0;
    int n_tensors = 0;
    int n_created = 0;

    uint64_t n_elements = 0;
    size_t  n_bytes     = 0;

    bool use_mmap = false;
    bool check_tensors;

    llama_files files;
    llama_ftype ftype;
    llama_fver  fver;

    llama_mmaps mappings;

    // Holds information on a model weight
    struct llama_tensor_weight {
        uint16_t  idx; // source file index
        size_t   offs; // tensor data offset in the original file

        lm_ggml_tensor * tensor;

        llama_tensor_weight(const llama_file * file, uint16_t idx, const struct lm_gguf_context * lm_gguf_ctx, lm_ggml_tensor * tensor) : idx(idx), tensor(tensor) {
            const int tensor_idx = lm_gguf_find_tensor(lm_gguf_ctx,  lm_ggml_get_name(tensor));
            if (tensor_idx < 0) {
                throw std::runtime_error(format("tensor '%s' not found in the model", lm_ggml_get_name(tensor)));
            }

            offs = lm_gguf_get_data_offset(lm_gguf_ctx) + lm_gguf_get_tensor_offset(lm_gguf_ctx, tensor_idx);
            if (offs + lm_ggml_nbytes(tensor) < offs || offs + lm_ggml_nbytes(tensor) > file->size) {
                throw std::runtime_error(format("tensor '%s' data is not within the file bounds, model is corrupted or incomplete", lm_ggml_get_name(tensor)));
            }
        }
    };

    // custom comparator to sort weights more nicely by layer
    struct weight_name_comparer {
        bool operator()(const std::string & a, const std::string & b) const {
            int a_layer = -1;
            int b_layer = -1;
            sscanf(a.c_str(), "blk.%d.", &a_layer);
            sscanf(b.c_str(), "blk.%d.", &b_layer);
            if (a_layer != b_layer) {
                return a_layer < b_layer;
            }
            return a < b;
        }
    };

    std::map<std::string, struct llama_tensor_weight, weight_name_comparer> weights_map;
    std::unordered_map<std::string, struct llama_model_kv_override> kv_overrides;

    lm_gguf_context_ptr meta;
    std::vector<lm_ggml_context_ptr> contexts;

    std::string arch_name;
    LLM_KV      llm_kv    = LLM_KV(LLM_ARCH_UNKNOWN);

    llama_model_loader(const std::string & fname, bool use_mmap, bool check_tensors, const struct llama_model_kv_override * param_overrides_p) {
        int trace = 0;
        if (getenv("LLAMA_TRACE")) {
            trace = atoi(getenv("LLAMA_TRACE"));
        }

        if (param_overrides_p != nullptr) {
            for (const struct llama_model_kv_override * p = param_overrides_p; p->key[0] != 0; p++) {
                kv_overrides.insert({std::string(p->key), *p});
            }
        }

        struct lm_ggml_context * ctx = NULL;
        struct lm_gguf_init_params params = {
            /*.no_alloc = */ true,
            /*.ctx      = */ &ctx,
        };

        meta.reset(lm_gguf_init_from_file(fname.c_str(), params));
        if (!meta) {
            throw std::runtime_error(format("%s: failed to load model from %s\n", __func__, fname.c_str()));
        }

        get_key(llm_kv(LLM_KV_GENERAL_ARCHITECTURE), arch_name, false);
        llm_kv = LLM_KV(llm_arch_from_string(arch_name));

        files.emplace_back(new llama_file(fname.c_str(), "rb"));
        contexts.emplace_back(ctx);

        // Save tensors data offset of the main file.
        // For subsidiary files, `meta` tensor data offset must not be used,
        // so we build a unified tensors index for weights.
        for (lm_ggml_tensor * cur = lm_ggml_get_first_tensor(ctx); cur; cur = lm_ggml_get_next_tensor(ctx, cur)) {
            std::string tensor_name = std::string(cur->name);
            // make sure there is no duplicated tensor names
            if (weights_map.find(tensor_name) != weights_map.end()) {
                throw std::runtime_error(format("invalid model: tensor '%s' is duplicated", lm_ggml_get_name(cur)));
            }
            n_elements += lm_ggml_nelements(cur);
            n_bytes    += lm_ggml_nbytes(cur);
            weights_map.emplace(tensor_name, llama_tensor_weight(files.back().get(), 0, meta.get(), cur));
        }
        uint16_t n_split = 0;
        get_key(llm_kv(LLM_KV_SPLIT_COUNT), n_split, false);

        // Load additional GGML contexts
        if (n_split > 1) {
            uint16_t idx = 0;
            get_key(llm_kv(LLM_KV_SPLIT_NO), idx);
            if (idx != 0) {
                throw std::runtime_error(format("illegal split file: %d, model must be loaded with the first split", idx));
            }

            char split_prefix[PATH_MAX] = {0};
            if (!llama_split_prefix(split_prefix, sizeof(split_prefix), fname.c_str(), idx, n_split)) {
                throw std::runtime_error(format("invalid split file: %s", fname.c_str()));
            }

            if (trace > 0) {
                LLAMA_LOG_INFO("%s: loading additional %d GGUFs\n", __func__, n_split);
            }

            char split_path[PATH_MAX] = {0};
            for (idx = 1; idx < n_split; idx++) {
                llama_split_path(split_path, sizeof(split_path), split_prefix, idx, n_split);

                struct lm_gguf_init_params split_params = {
                    /*.no_alloc = */ true,
                    /*.ctx      = */ &ctx,
                };
                lm_gguf_context_ptr ctx_gguf { lm_gguf_init_from_file(split_path, split_params) };
                if (!ctx_gguf) {
                    throw std::runtime_error(format("%s: failed to load GGUF split from %s\n", __func__, split_path));
                }

                files.emplace_back(new llama_file(split_path, "rb"));
                contexts.emplace_back(ctx);

                // Save tensors data offset info of the shard.
                for (lm_ggml_tensor * cur = lm_ggml_get_first_tensor(ctx); cur; cur = lm_ggml_get_next_tensor(ctx, cur)) {
                    std::string tensor_name = std::string(cur->name);
                    // make sure there is no duplicated tensor names
                    if (weights_map.find(tensor_name) != weights_map.end()) {
                        throw std::runtime_error(format("invalid model: tensor '%s' is duplicated", lm_ggml_get_name(cur)));
                    }
                    n_elements += lm_ggml_nelements(cur);
                    n_bytes    += lm_ggml_nbytes(cur);
                    weights_map.emplace(tensor_name, llama_tensor_weight(files.back().get(), idx, ctx_gguf.get(), cur));
                }
            }

            get_key(llm_kv(LLM_KV_SPLIT_TENSORS_COUNT), n_tensors);

            // sanity check
            {
                const int n_tensors_loaded = (int) weights_map.size();
                if (n_tensors != n_tensors_loaded) {
                    throw std::runtime_error(format("corrupted model: %d tensors expected but %d found", n_tensors, n_tensors_loaded));
                }
            }

            LLAMA_LOG_INFO("%s: additional %d GGUFs metadata loaded.\n",  __func__, n_split - 1);
        }

        n_kv      = lm_gguf_get_n_kv(meta.get());
        n_tensors = weights_map.size();

        fver = (enum llama_fver) lm_gguf_get_version(meta.get());

        LLAMA_LOG_INFO("%s: loaded meta data with %d key-value pairs and %d tensors from %s (version %s)\n",
                __func__, n_kv, n_tensors, fname.c_str(), llama_file_version_name(fver));

        // determine file type based on the number of tensors for each quantization and print meta data
        // TODO: make optional
        {
            std::map<enum lm_ggml_type, uint32_t> n_type;

            uint32_t n_type_max = 0;
            enum lm_ggml_type type_max = LM_GGML_TYPE_F32;

            for (const auto & it : weights_map) {
                const llama_tensor_weight & w = it.second;
                const lm_ggml_tensor * tensor = w.tensor;

                enum lm_ggml_type type = tensor->type;

                n_type[type]++;

                if (n_type_max < n_type[type]) {
                    n_type_max = n_type[type];
                    type_max   = type;
                }

                if (trace > 0) {
                    const uint16_t sid = w.idx;
                    LLAMA_LOG_INFO("%s: - tensor split %2d: %32s %-8s [ %s ]\n", __func__, sid, lm_ggml_get_name(tensor), lm_ggml_type_name(type), llama_format_tensor_shape(tensor).c_str());
                }
            }

            switch (type_max) {
                case LM_GGML_TYPE_F32:     ftype = LLAMA_FTYPE_ALL_F32;        break;
                case LM_GGML_TYPE_F16:     ftype = LLAMA_FTYPE_MOSTLY_F16;     break;
                case LM_GGML_TYPE_BF16:    ftype = LLAMA_FTYPE_MOSTLY_BF16;    break;
                case LM_GGML_TYPE_Q4_0:    ftype = LLAMA_FTYPE_MOSTLY_Q4_0;    break;
                case LM_GGML_TYPE_Q4_1:    ftype = LLAMA_FTYPE_MOSTLY_Q4_1;    break;
                case LM_GGML_TYPE_Q5_0:    ftype = LLAMA_FTYPE_MOSTLY_Q5_0;    break;
                case LM_GGML_TYPE_Q5_1:    ftype = LLAMA_FTYPE_MOSTLY_Q5_1;    break;
                case LM_GGML_TYPE_Q8_0:    ftype = LLAMA_FTYPE_MOSTLY_Q8_0;    break;
                case LM_GGML_TYPE_Q2_K:    ftype = LLAMA_FTYPE_MOSTLY_Q2_K;    break;
                case LM_GGML_TYPE_Q3_K:    ftype = LLAMA_FTYPE_MOSTLY_Q3_K_M;  break;
                case LM_GGML_TYPE_Q4_K:    ftype = LLAMA_FTYPE_MOSTLY_Q4_K_M;  break;
                case LM_GGML_TYPE_Q5_K:    ftype = LLAMA_FTYPE_MOSTLY_Q5_K_M;  break;
                case LM_GGML_TYPE_Q6_K:    ftype = LLAMA_FTYPE_MOSTLY_Q6_K;    break;
                case LM_GGML_TYPE_TQ1_0:   ftype = LLAMA_FTYPE_MOSTLY_TQ1_0;   break;
                case LM_GGML_TYPE_TQ2_0:   ftype = LLAMA_FTYPE_MOSTLY_TQ2_0;   break;
                case LM_GGML_TYPE_IQ2_XXS: ftype = LLAMA_FTYPE_MOSTLY_IQ2_XXS; break;
                case LM_GGML_TYPE_IQ2_XS:  ftype = LLAMA_FTYPE_MOSTLY_IQ2_XS;  break;
                case LM_GGML_TYPE_IQ2_S:   ftype = LLAMA_FTYPE_MOSTLY_IQ2_S;   break;
                case LM_GGML_TYPE_IQ3_XXS: ftype = LLAMA_FTYPE_MOSTLY_IQ3_XXS; break;
                case LM_GGML_TYPE_IQ1_S:   ftype = LLAMA_FTYPE_MOSTLY_IQ1_S;   break;
                case LM_GGML_TYPE_IQ1_M:   ftype = LLAMA_FTYPE_MOSTLY_IQ1_M;   break;
                case LM_GGML_TYPE_IQ4_NL:  ftype = LLAMA_FTYPE_MOSTLY_IQ4_NL;  break;
                case LM_GGML_TYPE_IQ4_XS:  ftype = LLAMA_FTYPE_MOSTLY_IQ4_XS;  break;
                case LM_GGML_TYPE_IQ3_S:   ftype = LLAMA_FTYPE_MOSTLY_IQ3_S;   break;
                case LM_GGML_TYPE_Q4_0_4_4: ftype = LLAMA_FTYPE_MOSTLY_Q4_0_4_4; break;
                case LM_GGML_TYPE_Q4_0_4_8: ftype = LLAMA_FTYPE_MOSTLY_Q4_0_4_8; break;
                case LM_GGML_TYPE_Q4_0_8_8: ftype = LLAMA_FTYPE_MOSTLY_Q4_0_8_8; break;
                default:
                    {
                        LLAMA_LOG_WARN("%s: unknown type %s\n", __func__, lm_ggml_type_name(type_max));
                        ftype = LLAMA_FTYPE_ALL_F32;
                    } break;
            }

            // this is a way to mark that we have "guessed" the file type
            ftype = (llama_ftype) (ftype | LLAMA_FTYPE_GUESSED);

            {
                const int kid = lm_gguf_find_key(meta.get(), "general.file_type"); // TODO: use LLM_KV
                if (kid >= 0) {
                    ftype = (llama_ftype) lm_gguf_get_val_u32(meta.get(), kid);
                }
            }

            LLAMA_LOG_INFO("%s: Dumping metadata keys/values. Note: KV overrides do not apply in this output.\n", __func__);

            for (int i = 0; i < n_kv; i++) {
                const char * name           = lm_gguf_get_key(meta.get(), i);
                const enum lm_gguf_type type   = lm_gguf_get_kv_type(meta.get(), i);
                const std::string type_name =
                    type == LM_GGUF_TYPE_ARRAY
                    ? format("%s[%s,%d]", lm_gguf_type_name(type), lm_gguf_type_name(lm_gguf_get_arr_type(meta.get(), i)), lm_gguf_get_arr_n(meta.get(), i))
                    : lm_gguf_type_name(type);

                std::string value          = lm_gguf_kv_to_str(meta.get(), i);
                const size_t MAX_VALUE_LEN = 40;
                if (value.size() > MAX_VALUE_LEN) {
                    value = format("%s...", value.substr(0, MAX_VALUE_LEN - 3).c_str());
                }
                replace_all(value, "\n", "\\n");

                LLAMA_LOG_INFO("%s: - kv %3d: %42s %-16s = %s\n", __func__, i, name, type_name.c_str(), value.c_str());
            }

            // print type counts
            for (auto & kv : n_type) {
                if (kv.second == 0) {
                    continue;
                }

                LLAMA_LOG_INFO("%s: - type %4s: %4d tensors\n", __func__, lm_ggml_type_name(kv.first), kv.second);
            }
        }

        if (!llama_mmap::SUPPORTED) {
            LLAMA_LOG_WARN("%s: mmap is not supported on this platform\n", __func__);
            use_mmap = false;
        }

        this->use_mmap = use_mmap;
        this->check_tensors = check_tensors;
    }

    template<typename T>
    typename std::enable_if<std::is_integral<T>::value, bool>::type
    get_arr_n(const std::string & key, T & result, const bool required = true) {
        const int kid = lm_gguf_find_key(meta.get(), key.c_str());

        if (kid < 0) {
            if (required) {
                throw std::runtime_error(format("key not found in model: %s", key.c_str()));
            }
            return false;
        }

        struct GGUFMeta::ArrayInfo arr_info =
            GGUFMeta::GKV<GGUFMeta::ArrayInfo>::get_kv(meta.get(), kid);


        result = arr_info.length;
        return true;
    }

    template<typename T>
    typename std::enable_if<std::is_integral<T>::value, bool>::type
    get_arr_n(const enum llm_kv kid, T & result, const bool required = true) {
        return get_arr_n(llm_kv(kid), result, required);
    }

    template<typename T>
    bool get_arr(const std::string & key, std::vector<T> & result, const bool required = true) {
        const int kid = lm_gguf_find_key(meta.get(), key.c_str());

        if (kid < 0 || lm_gguf_get_kv_type(meta.get(), kid) != LM_GGUF_TYPE_ARRAY) {
            if (required) {
                throw std::runtime_error(format("array key not found in model: %s", key.c_str()));
            }
            return false;
        }

        struct GGUFMeta::ArrayInfo arr_info =
            GGUFMeta::GKV<GGUFMeta::ArrayInfo>::get_kv(meta.get(), kid);

        switch (arr_info.gt) {
            case LM_GGUF_TYPE_FLOAT32: LM_GGML_ASSERT((std::is_same<T, float>::value)); break;
            case LM_GGUF_TYPE_INT32:   LM_GGML_ASSERT(
                                            (std::is_same<T,  int32_t>::value) ||
                                            (std::is_same<T, uint32_t>::value));  break;
            default:
                throw std::runtime_error(format("%s is not a float32, int32 array", key.c_str()));
        }

        result.resize(arr_info.length);
        result.assign((const T*)arr_info.data, (const T *)arr_info.data + arr_info.length);

        return true;
    }

    template<typename T, size_t N_MAX>
    bool get_arr(const std::string & key, std::array<T, N_MAX> & result, const bool required = true) {
        const int kid = lm_gguf_find_key(meta.get(), key.c_str());

        if (kid < 0 || lm_gguf_get_kv_type(meta.get(), kid) != LM_GGUF_TYPE_ARRAY) {
            if (required) {
                throw std::runtime_error(format("array key not found in model: %s", key.c_str()));
            }
            return false;
        }

        struct GGUFMeta::ArrayInfo arr_info =
            GGUFMeta::GKV<GGUFMeta::ArrayInfo>::get_kv(meta.get(), kid);

        switch (arr_info.gt) {
            case LM_GGUF_TYPE_FLOAT32: LM_GGML_ASSERT((std::is_same<T, float>::value)); break;
            case LM_GGUF_TYPE_INT32:   LM_GGML_ASSERT(
                                            (std::is_same<T,  int32_t>::value) ||
                                            (std::is_same<T, uint32_t>::value));  break;
            default:
                throw std::runtime_error(format("%s is not a float32, int32 array", key.c_str()));
        }

        if (arr_info.length > N_MAX) {
            throw std::runtime_error(format("array length %u for key %s exceeds max %u", (uint32_t) arr_info.length, key.c_str(), (uint32_t) N_MAX));
        }

        std::copy((const T*)arr_info.data, (const T *)arr_info.data + arr_info.length, result.begin());

        return true;
    }

    template<typename T>
    bool get_arr(const enum llm_kv kid, T & result, const bool required = true) {
        return get_arr(llm_kv(kid), result, required);
    }

    template<typename T>
    bool get_key(const std::string & key, T & result, const bool required = true) {
        auto it = kv_overrides.find(key);

        const struct llama_model_kv_override * override =
            it != kv_overrides.end() ? &it->second : nullptr;

        const bool found = GGUFMeta::GKV<T>::set(meta.get(), key, result, override);

        if (required && !found) {
            throw std::runtime_error(format("key not found in model: %s", key.c_str()));
        }

        return found;
    }

    template<typename T>
    bool get_key(const enum llm_kv kid, T & result, const bool required = true) {
        return get_key(llm_kv(kid), result, required);
    }

    // get array of n <= N_MAX elements, or a single element repeated n times
    template<typename T, size_t N_MAX>
    bool get_key_or_arr(const std::string & key, std::array<T, N_MAX> & result, uint32_t n, const bool required = true) {
        const int kid = lm_gguf_find_key(meta.get(), key.c_str());

        if (kid < 0) {
            if (required) {
                throw std::runtime_error(format("key not found in model: %s", key.c_str()));
            }
            return false;
        }

        if (n > N_MAX) {
            throw std::runtime_error(format("n > N_MAX: %u > %u for key %s", (uint32_t) n, (uint32_t) N_MAX, key.c_str()));
        }

        if (lm_gguf_get_kv_type(meta.get(), kid) == LM_GGUF_TYPE_ARRAY) {
            struct GGUFMeta::ArrayInfo arr_info =
                GGUFMeta::GKV<GGUFMeta::ArrayInfo>::get_kv(meta.get(), kid);

            if (n != arr_info.length) {
                throw std::runtime_error(format("key %s has wrong array length; expected %u, got %u", key.c_str(), n, (uint32_t) arr_info.length));
            }

            return get_arr(key, result, required);
        } else {
            T value;

            bool ok = get_key(key, value, required);
            if (!ok) {
                return false;
            }

            for (uint32_t i = 0; i < n; i++) {
                result[i] = value;
            }

            return true;
        }
    }

    template<typename T>
    bool get_key_or_arr(const enum llm_kv kid, T & result, uint32_t n, const bool required = true) {
        return get_key_or_arr(llm_kv(kid), result, n, required);
    }

    std::string get_arch_name() const {
        return arch_name;
    }

    enum llm_arch get_arch() const {
        return llm_kv.arch;
    }

    const llama_tensor_weight * get_weight(const char * name) const {
        auto pos = weights_map.find(name);
        if (pos != weights_map.end()) {
            return &pos->second;
        }

        return nullptr;
    }

    const llama_tensor_weight & require_weight(const char * name) const {
        const llama_tensor_weight * weight = get_weight(name);
        if (!weight) {
            throw std::runtime_error(format("%s: tensor '%s' not found", __func__, name));
        }
        return *weight;
    }

    struct lm_ggml_tensor * get_tensor_meta(const char * name) const {
        const auto * weight = get_weight(name);
        if (!weight) {
            return nullptr;
        }
        return weight->tensor;
    }

    struct lm_ggml_tensor * require_tensor_meta(const std::string & name) const {
        struct lm_ggml_tensor * tensor = get_tensor_meta(name.c_str());
        if (!tensor) {
            throw std::runtime_error(format("%s: tensor '%s' not found", __func__, name.c_str()));
        }
        return tensor;
    }

    const struct lm_ggml_tensor * check_tensor_dims(const std::string & name, const std::vector<int64_t> & ne, bool required) const {
        const struct lm_ggml_tensor * cur = get_tensor_meta(name.c_str());

        if (cur == NULL) {
            if (!required) {
                return NULL;
            }
            throw std::runtime_error(format("%s: tensor '%s' not found", __func__, name.c_str()));
        }

        {
            bool is_ok = true;
            for (size_t i = 0; i < LM_GGML_MAX_DIMS; ++i) {
                if ((i < ne.size() && ne[i] != cur->ne[i]) || (i >= ne.size() && cur->ne[i] != 1)) {
                    is_ok = false;
                    break;
                }
            }
            if (!is_ok) {
                throw std::runtime_error(
                        format("%s: tensor '%s' has wrong shape; expected %s, got %s",
                            __func__, name.c_str(),
                            llama_format_tensor_shape(ne).c_str(),
                            llama_format_tensor_shape(cur).c_str()));
            }
        }

        return cur;
    }

    static const int TENSOR_NOT_REQUIRED = 1;
    static const int TENSOR_DUPLICATED   = 2;

    struct lm_ggml_tensor * create_tensor(struct lm_ggml_context * ctx, const std::string & name, const std::initializer_list<int64_t> & ne, int flags = 0) {
        const struct lm_ggml_tensor * cur = check_tensor_dims(name, ne, !(flags & TENSOR_NOT_REQUIRED));

        if (cur == NULL) {
            return NULL;
        }

        bool duplicated = flags & TENSOR_DUPLICATED;

        struct lm_ggml_tensor * tensor = lm_ggml_dup_tensor(ctx, cur);
        lm_ggml_set_name(tensor, lm_ggml_get_name(cur));

        if (duplicated) {
            size_data += lm_ggml_nbytes(cur);
        } else {
            n_created++;
        }

        return tensor;

    }

    struct lm_ggml_tensor * create_tensor_as_view(struct lm_ggml_context * ctx, struct lm_ggml_tensor * base, const std::string & name, const std::initializer_list<int64_t> & ne, size_t offset, bool required = true) {
        const struct lm_ggml_tensor * cur = check_tensor_dims(name, ne, required);

        if (cur == NULL) {
            return NULL;
        }

        if (cur->type != base->type) {
            throw std::runtime_error(format("%s: tensor '%s' has wrong type; expected %s, got %s", __func__, name.c_str(), lm_ggml_type_name(base->type), lm_ggml_type_name(cur->type)));
        }

        std::array<int64_t, LM_GGML_MAX_DIMS> dims;
        for (size_t i = 0; i < LM_GGML_MAX_DIMS; ++i) {
            dims[i] = i < ne.size() ? ne.begin()[i] : 1;
        }

        struct lm_ggml_tensor * tensor = lm_ggml_view_4d(ctx, base,
                                        dims[0], dims[1], dims[2], dims[3],
                                        cur->nb[1], cur->nb[2], cur->nb[3],
                                        offset);

        lm_ggml_set_name(tensor, name.c_str());

        n_created++;

        return tensor;
    }

    void done_getting_tensors() const {
        if (n_created != n_tensors) {
            throw std::runtime_error(format("%s: wrong number of tensors; expected %d, got %d", __func__, n_tensors, n_created));
        }
    }

    void init_mappings(bool prefetch = true, llama_mlocks * mlock_mmaps = nullptr) {
        if (use_mmap) {
            mappings.reserve(files.size());
            mmaps_used.reserve(files.size());
            for (const auto & file : files) {
                std::unique_ptr<llama_mmap> mapping(new llama_mmap(file.get(), prefetch ? -1 : 0, lm_ggml_is_numa()));
                mmaps_used.emplace_back(mapping->size, 0);
                if (mlock_mmaps) {
                    std::unique_ptr<llama_mlock> mlock_mmap(new llama_mlock());
                    mlock_mmap->init(mapping->addr);
                    mlock_mmaps->emplace_back(std::move(mlock_mmap));
                }
                mappings.emplace_back(std::move(mapping));
            }
        }

        // compute the total size of all tensors for progress reporting
        for (const auto & it : weights_map) {
            size_data += lm_ggml_nbytes(it.second.tensor);
        }
    }

    void get_mapping_range(size_t * first, size_t * last, void ** addr, int idx, lm_ggml_context * ctx) const {
        LM_GGML_ASSERT(!mappings.empty());
        const auto & mapping = mappings.at(idx);

        *first = mapping->size;
        *last  = 0;
        *addr = mapping->addr;
        for (lm_ggml_tensor * tensor = lm_ggml_get_first_tensor(ctx); tensor; tensor = lm_ggml_get_next_tensor(ctx, tensor)) {
            const auto * weight = get_weight(lm_ggml_get_name(tensor));
            if (!weight || weight->idx != idx) {
                continue;
            }
            *first = std::min(*first, weight->offs);
            *last  = std::max(*last,  weight->offs + lm_ggml_nbytes(tensor));
        }
    }

    // for backwards compatibility, does not support ggml-backend
    void load_data_for(struct lm_ggml_tensor * cur) const {
        const auto & w = require_weight(lm_ggml_get_name(cur));

        if (use_mmap) {
            const auto & mapping = mappings.at(w.idx);
            if (cur->data == nullptr) {
                cur->data = (uint8_t *)mapping->addr + w.offs;
            } else {
                memcpy(cur->data, (uint8_t *)mapping->addr + w.offs, lm_ggml_nbytes(cur));
            }
        } else {
            LM_GGML_ASSERT(cur->data != nullptr);
            LM_GGML_ASSERT(w.idx < files.size());
            const auto & file = files.at(w.idx);
            file->seek(w.offs, SEEK_SET);
            file->read_raw(cur->data, lm_ggml_nbytes(cur));
        }

        if (check_tensors && !lm_ggml_validate_row_data(cur->type, cur->data, lm_ggml_nbytes(cur))) {
            throw std::runtime_error(format("tensor '%s' has invalid data", lm_ggml_get_name(cur)));
        }
    }

    size_t size_done = 0;
    size_t size_data = 0;
    std::vector<std::pair<size_t, size_t>> mmaps_used;

    // Returns false if cancelled by progress_callback
    bool load_all_data(
            struct lm_ggml_context * ctx,
            llama_buf_map & bufs,
            llama_mlocks * lmlocks,
            llama_progress_callback progress_callback,
            void * progress_callback_user_data) {
        LM_GGML_ASSERT(size_data != 0 && "call init_mappings() first");

        std::vector<no_init<uint8_t>> read_buf;
        std::vector<std::future<std::pair<lm_ggml_tensor *, bool>>> validation_result;

        // 4 staging buffers for async uploads, each sized 1MB seems to be a good default for single NVMe drives.
        // NVMe raid configurations might require more / larger buffers.
        constexpr size_t n_buffers = 4;
        constexpr size_t buffer_size = 1 * 1024 * 1024; // 1MB

        std::vector<lm_ggml_backend_buffer_t> host_buffers;
        std::vector<lm_ggml_backend_event_t> events;
        std::vector<void *> host_ptrs;
        size_t buffer_idx = 0; // buffer to use for async loads
        lm_ggml_backend_t upload_backend = [&](const char * func) -> lm_ggml_backend_t {
            if (use_mmap || check_tensors) {
                return nullptr;
            }
            // When not using mmaped io use async uploads from pinned memory to GPU memory.
            // First determine if the backend supports the necessary features for async uploads.
            auto * buf = bufs.count(0) ? bufs.at(0) : nullptr;
            if (!buf) {
                LLAMA_LOG_DEBUG("%s: no buffer found for async uploads\n", func);
                return nullptr;
            }

            auto * buft = lm_ggml_backend_buffer_get_type(buf);
            auto * dev = lm_ggml_backend_buft_get_device(buft);
            if (!dev) {
                LLAMA_LOG_DEBUG("%s: no device found for buffer type %s for async uploads\n", func,
                    lm_ggml_backend_buft_name(buft));
                return nullptr;
            }

            if (buft != lm_ggml_backend_dev_buffer_type(dev)) {
                LLAMA_LOG_DEBUG("%s: buffer type %s is not the default buffer type for device %s for async uploads\n", func,
                    lm_ggml_backend_buft_name(buft), lm_ggml_backend_dev_name(dev));
                return nullptr;
            }

            lm_ggml_backend_dev_props props;
            lm_ggml_backend_dev_get_props(dev, &props);
            if (!props.caps.async || !props.caps.host_buffer || !props.caps.events) {
                LLAMA_LOG_DEBUG("%s: device %s does not support async, host buffers or events\n", func,
                    lm_ggml_backend_dev_name(dev));
                return nullptr;
            }

            auto * host_buft = lm_ggml_backend_dev_host_buffer_type(dev);
            if (!host_buft) {
                LLAMA_LOG_DEBUG("%s: no host buffer type found for device %s\n", func,
                    lm_ggml_backend_dev_name(dev));
                return nullptr;
            }

            // If the backend is supported, create pinned memory buffers and events for synchronisation.
            for (size_t idx = 0; idx < n_buffers; ++idx) {
                auto * buf = lm_ggml_backend_buft_alloc_buffer(host_buft, buffer_size);
                if (!buf) {
                    LLAMA_LOG_DEBUG("%s: failed to allocate host buffer for async uploads for device %s\n", func,
                        lm_ggml_backend_dev_name(dev));
                    return nullptr;
                }

                host_buffers.emplace_back(buf);
                host_ptrs.emplace_back(lm_ggml_backend_buffer_get_base(buf));

                auto * event = lm_ggml_backend_event_new(dev);
                if (!event) {
                    LLAMA_LOG_DEBUG("%s: failed to create event for async uploads for device %s\n", func,
                        lm_ggml_backend_dev_name(dev));
                    return nullptr;
                }

                events.emplace_back(event);
            }

            lm_ggml_backend_t backend = lm_ggml_backend_dev_init(dev, nullptr);
            if (!backend) {
                LLAMA_LOG_DEBUG("%s: failed to initialize backend for device %s for async uploads\n", func,
                    lm_ggml_backend_dev_name(dev));
                return nullptr;
            }

            return backend;
        }(__func__);

        if (upload_backend) {
            LLAMA_LOG_DEBUG("%s: using async uploads for device %s, buffer type %s, backend %s\n", __func__,
                lm_ggml_backend_dev_name(lm_ggml_backend_get_device(upload_backend)),
                lm_ggml_backend_buft_name(lm_ggml_backend_buffer_get_type(bufs.at(0))),
                lm_ggml_backend_name(upload_backend));
        }

        for (struct lm_ggml_tensor * cur = lm_ggml_get_first_tensor(ctx); cur != NULL; cur = lm_ggml_get_next_tensor(ctx, cur)) {
            const auto * weight = get_weight(lm_ggml_get_name(cur));
            if (weight == nullptr) {
                // this can happen with split experts models
                continue;
            }

            if (progress_callback) {
                if (!progress_callback((float) size_done / size_data, progress_callback_user_data)) {
                    return false;
                }
            }

            size_t n_size = lm_ggml_nbytes(cur);

            if (use_mmap) {
                const auto & mapping = mappings.at(weight->idx);
                lm_ggml_backend_buffer_t buf_mmap = nullptr;
                if (bufs.count(weight->idx)) {
                    buf_mmap = bufs.at(weight->idx);
                }
                uint8_t * data = (uint8_t *) mapping->addr + weight->offs;

                if (check_tensors) {
                    validation_result.emplace_back(std::async(std::launch::async, [cur, data, n_size] {
                        return std::make_pair(cur, lm_ggml_validate_row_data(cur->type, data, n_size));
                    }));
                }

                LM_GGML_ASSERT(buf_mmap || cur->data); // either we have a buffer to allocate the tensor in, or it is already allocated
                if (buf_mmap && cur->data == nullptr) {
                    lm_ggml_backend_tensor_alloc(buf_mmap, cur, data);
                    if (lmlocks) {
                        const auto & lmlock = lmlocks->at(weight->idx);
                        lmlock->grow_to(weight->offs + n_size);
                    }

                    auto & mmap_used = mmaps_used[weight->idx];
                    mmap_used.first  = std::min(mmap_used.first,  weight->offs);
                    mmap_used.second = std::max(mmap_used.second, weight->offs + n_size);
                } else {
                    lm_ggml_backend_tensor_set(cur, data, 0, n_size);
                }
            } else {
                const auto & file = files.at(weight->idx);
                if (lm_ggml_backend_buffer_is_host(cur->buffer)) {
                    file->seek(weight->offs, SEEK_SET);
                    file->read_raw(cur->data, n_size);
                    if (check_tensors) {
                        validation_result.emplace_back(std::async(std::launch::async, [cur, n_size] {
                            return std::make_pair(cur, lm_ggml_validate_row_data(cur->type, cur->data, n_size));
                        }));
                    }
                } else {
                    // If upload_backend is valid load the tensor in chunks to pinned memory and upload the buffers asynchronously to the GPU.
                    if (upload_backend) {
                        file->seek(weight->offs, SEEK_SET);

                        size_t bytes_read = 0;

                        while (bytes_read < n_size) {
                            size_t read_iteration = std::min<size_t>(buffer_size, n_size - bytes_read);

                            lm_ggml_backend_event_synchronize(events[buffer_idx]);
                            file->read_raw(host_ptrs[buffer_idx], read_iteration);
                            lm_ggml_backend_tensor_set_async(upload_backend, cur, host_ptrs[buffer_idx], bytes_read, read_iteration);
                            lm_ggml_backend_event_record(events[buffer_idx], upload_backend);

                            bytes_read += read_iteration;
                            ++buffer_idx;
                            buffer_idx %= n_buffers;
                        }
                    } else {
                        read_buf.resize(n_size);
                        file->seek(weight->offs, SEEK_SET);
                        file->read_raw(read_buf.data(), n_size);
                        lm_ggml_backend_tensor_set(cur, read_buf.data(), 0, n_size);
                        if (check_tensors && !lm_ggml_validate_row_data(cur->type, read_buf.data(), n_size)) {
                            throw std::runtime_error(format("tensor '%s' has invalid data", lm_ggml_get_name(cur)));
                        }
                    }
                }
            }

            size_done += n_size;
        }

        // free temporary resources used for async uploads
        for (auto * event : events) {
            lm_ggml_backend_event_synchronize(event);
            lm_ggml_backend_event_free(event);
        }
        for (auto * buf : host_buffers) {
            lm_ggml_backend_buffer_free(buf);
        }
        lm_ggml_backend_free(upload_backend);

        // check validation results
        bool validation_failed = false;
        for (auto & future : validation_result) {
            auto result = future.get();
            if (!result.second) {
                LLAMA_LOG_ERROR("%s: tensor '%s' has invalid data\n", __func__, lm_ggml_get_name(result.first));
                validation_failed = true;
            }
        }
        if (validation_failed) {
            throw std::runtime_error("found tensors with invalid data");
        }

        // check if this is the last call and do final cleanup
        if (size_done >= size_data) {
            // unmap offloaded tensors and metadata
            if (use_mmap) {
                for (uint32_t idx = 0; idx < mappings.size(); idx++) {
                    const auto & mmap_used = mmaps_used.at(idx);
                    auto & mapping = mappings.at(idx);
                    mapping->unmap_fragment(0, mmap_used.first);
                    if (mmap_used.second != 0) {
                        mapping->unmap_fragment(mmap_used.second, mapping->size);
                    }
                }
            }
            if (progress_callback) {
                // Even though the model is done loading, we still honor
                // cancellation since we need to free allocations.
                return progress_callback(1.0f, progress_callback_user_data);
            }
        }

        return true;
    }
};

// temporary allocate memory for the input batch if needed
static const llama_seq_id batch_default_seq_id = 0;
struct llama_batch_allocr {
    std::array<llama_seq_id, 1> seq_id_0 = {batch_default_seq_id};
    std::vector<llama_pos>      pos;
    std::vector<int32_t>        n_seq_id;
    std::vector<llama_seq_id *> seq_id;
    std::vector<int8_t>         logits;
    struct llama_batch          batch;
    // optionally fulfill the batch returned by llama_batch_get_one
    llama_batch_allocr(llama_context & ctx, struct llama_batch in_batch) {
        batch = in_batch;
        LM_GGML_ASSERT(batch.n_tokens > 0);
        if (!batch.pos) {
            // determine the last position in KV cache
            llama_pos last_pos = -1;
            for (const auto & cell : ctx.kv_self.cells) {
                if (cell.has_seq_id(batch_default_seq_id)) {
                    last_pos = std::max(last_pos, cell.pos);
                }
            }
            last_pos++; // next position
            pos.resize(batch.n_tokens);
            for (int32_t i = 0; i < batch.n_tokens; i++) {
                pos[i] = i+last_pos;
            }
            batch.pos = pos.data();
        }
        if (!batch.n_seq_id) {
            n_seq_id.resize(batch.n_tokens);
            for (int32_t i = 0; i < batch.n_tokens; i++) {
                n_seq_id[i] = seq_id_0.size();
            }
            batch.n_seq_id = n_seq_id.data();
        }
        if (!batch.seq_id) {
            seq_id.resize(batch.n_tokens + 1);
            seq_id[batch.n_tokens] = NULL;
            for (int32_t i = 0; i < batch.n_tokens; i++) {
                seq_id[i] = seq_id_0.data();
            }
            batch.seq_id = seq_id.data();
        }
        if (!batch.logits) {
            logits.resize(batch.n_tokens);
            logits[logits.size() - 1] = true;
            batch.logits = logits.data();
        }
    }
};

template<>
bool llama_model_loader::get_key(const enum llm_kv kid, enum llama_pooling_type & result, const bool required) {
    uint32_t tmp;
    const bool found = get_key(kid, tmp, required);
    if (found) {
        result = (enum llama_pooling_type) tmp;
    } else {
        result = LLAMA_POOLING_TYPE_UNSPECIFIED;
    }
    return found;
}


//
// load LLaMA models
//

static const char * llama_model_arch_name(llm_arch arch) {
    auto it = LLM_ARCH_NAMES.find(arch);
    if (it == LLM_ARCH_NAMES.end()) {
        return "unknown";
    }
    return it->second;
}

static std::string llama_model_ftype_name(llama_ftype ftype) {
    if (ftype & LLAMA_FTYPE_GUESSED) {
        return llama_model_ftype_name((enum llama_ftype) (ftype & ~LLAMA_FTYPE_GUESSED)) + " (guessed)";
    }

    switch (ftype) {
        case LLAMA_FTYPE_ALL_F32:         return "all F32";
        case LLAMA_FTYPE_MOSTLY_F16:      return "F16";
        case LLAMA_FTYPE_MOSTLY_BF16:     return "BF16";
        case LLAMA_FTYPE_MOSTLY_Q4_0:     return "Q4_0";
        case LLAMA_FTYPE_MOSTLY_Q4_1:     return "Q4_1";
        case LLAMA_FTYPE_MOSTLY_Q5_0:     return "Q5_0";
        case LLAMA_FTYPE_MOSTLY_Q5_1:     return "Q5_1";
        case LLAMA_FTYPE_MOSTLY_Q8_0:     return "Q8_0";
        case LLAMA_FTYPE_MOSTLY_Q2_K:     return "Q2_K - Medium";
        case LLAMA_FTYPE_MOSTLY_Q2_K_S:   return "Q2_K - Small";
        case LLAMA_FTYPE_MOSTLY_Q3_K_S:   return "Q3_K - Small";
        case LLAMA_FTYPE_MOSTLY_Q3_K_M:   return "Q3_K - Medium";
        case LLAMA_FTYPE_MOSTLY_Q3_K_L:   return "Q3_K - Large";
        case LLAMA_FTYPE_MOSTLY_Q4_K_S:   return "Q4_K - Small";
        case LLAMA_FTYPE_MOSTLY_Q4_K_M:   return "Q4_K - Medium";
        case LLAMA_FTYPE_MOSTLY_Q5_K_S:   return "Q5_K - Small";
        case LLAMA_FTYPE_MOSTLY_Q5_K_M:   return "Q5_K - Medium";
        case LLAMA_FTYPE_MOSTLY_Q6_K:     return "Q6_K";
        case LLAMA_FTYPE_MOSTLY_TQ1_0:    return "TQ1_0 - 1.69 bpw ternary";
        case LLAMA_FTYPE_MOSTLY_TQ2_0:    return "TQ2_0 - 2.06 bpw ternary";
        case LLAMA_FTYPE_MOSTLY_IQ2_XXS:  return "IQ2_XXS - 2.0625 bpw";
        case LLAMA_FTYPE_MOSTLY_IQ2_XS:   return "IQ2_XS - 2.3125 bpw";
        case LLAMA_FTYPE_MOSTLY_IQ2_S:    return "IQ2_S - 2.5 bpw";
        case LLAMA_FTYPE_MOSTLY_IQ2_M:    return "IQ2_M - 2.7 bpw";
        case LLAMA_FTYPE_MOSTLY_IQ3_XS:   return "IQ3_XS - 3.3 bpw";
        case LLAMA_FTYPE_MOSTLY_IQ3_XXS:  return "IQ3_XXS - 3.0625 bpw";
        case LLAMA_FTYPE_MOSTLY_IQ1_S:    return "IQ1_S - 1.5625 bpw";
        case LLAMA_FTYPE_MOSTLY_IQ1_M:    return "IQ1_M - 1.75 bpw";
        case LLAMA_FTYPE_MOSTLY_IQ4_NL:   return "IQ4_NL - 4.5 bpw";
        case LLAMA_FTYPE_MOSTLY_IQ4_XS:   return "IQ4_XS - 4.25 bpw";
        case LLAMA_FTYPE_MOSTLY_IQ3_S:    return "IQ3_S - 3.4375 bpw";
        case LLAMA_FTYPE_MOSTLY_IQ3_M:    return "IQ3_S mix - 3.66 bpw";
        case LLAMA_FTYPE_MOSTLY_Q4_0_4_4: return "Q4_0_4_4";
        case LLAMA_FTYPE_MOSTLY_Q4_0_4_8: return "Q4_0_4_8";
        case LLAMA_FTYPE_MOSTLY_Q4_0_8_8: return "Q4_0_8_8";

        default: return "unknown, may not work";
    }
}

static const char * llama_model_type_name(e_model type) {
    switch (type) {
        case MODEL_14M:           return "14M";
        case MODEL_17M:           return "17M";
        case MODEL_22M:           return "22M";
        case MODEL_33M:           return "33M";
        case MODEL_60M:           return "60M";
        case MODEL_70M:           return "70M";
        case MODEL_80M:           return "80M";
        case MODEL_109M:          return "109M";
        case MODEL_137M:          return "137M";
        case MODEL_160M:          return "160M";
        case MODEL_220M:          return "220M";
        case MODEL_250M:          return "250M";
        case MODEL_270M:          return "270M";
        case MODEL_335M:          return "335M";
        case MODEL_410M:          return "410M";
        case MODEL_450M:          return "450M";
        case MODEL_770M:          return "770M";
        case MODEL_780M:          return "780M";
        case MODEL_0_5B:          return "0.5B";
        case MODEL_1B:            return "1B";
        case MODEL_1_3B:          return "1.3B";
        case MODEL_1_4B:          return "1.4B";
        case MODEL_1_5B:          return "1.5B";
        case MODEL_1_6B:          return "1.6B";
        case MODEL_2B:            return "2B";
        case MODEL_2_8B:          return "2.8B";
        case MODEL_3B:            return "3B";
        case MODEL_4B:            return "4B";
        case MODEL_6B:            return "6B";
        case MODEL_6_9B:          return "6.9B";
        case MODEL_7B:            return "7B";
        case MODEL_8B:            return "8B";
        case MODEL_9B:            return "9B";
        case MODEL_11B:           return "11B";
        case MODEL_12B:           return "12B";
        case MODEL_13B:           return "13B";
        case MODEL_14B:           return "14B";
        case MODEL_15B:           return "15B";
        case MODEL_16B:           return "16B";
        case MODEL_20B:           return "20B";
        case MODEL_30B:           return "30B";
        case MODEL_34B:           return "34B";
        case MODEL_35B:           return "35B";
        case MODEL_40B:           return "40B";
        case MODEL_65B:           return "65B";
        case MODEL_70B:           return "70B";
        case MODEL_236B:          return "236B";
        case MODEL_314B:          return "314B";
        case MODEL_SMALL:         return "0.1B";
        case MODEL_MEDIUM:        return "0.4B";
        case MODEL_LARGE:         return "0.8B";
        case MODEL_XL:            return "1.5B";
        case MODEL_A1_7B:         return "A1.7B";
        case MODEL_A2_7B:         return "A2.7B";
        case MODEL_8x7B:          return "8x7B";
        case MODEL_8x22B:         return "8x22B";
        case MODEL_16x12B:        return "16x12B";
        case MODEL_10B_128x3_66B: return "10B+128x3.66B";
        case MODEL_57B_A14B:      return "57B.A14B";
        case MODEL_27B:           return "27B";
        default:                  return "?B";
    }
}

static const char * llama_model_vocab_type_name(enum llama_vocab_type type){
    switch (type) {
        case LLAMA_VOCAB_TYPE_NONE: return "no vocab";
        case LLAMA_VOCAB_TYPE_SPM:  return "SPM";
        case LLAMA_VOCAB_TYPE_BPE:  return "BPE";
        case LLAMA_VOCAB_TYPE_WPM:  return "WPM";
        case LLAMA_VOCAB_TYPE_UGM:  return "UGM";
        case LLAMA_VOCAB_TYPE_RWKV: return "RWKV";
        default:                    return "unknown";
    }
}

static void llm_load_stats(llama_model_loader & ml, llama_model & model) {
    model.n_elements = ml.n_elements;
    model.n_bytes = ml.n_bytes;
}

static void llm_load_arch(llama_model_loader & ml, llama_model & model) {
    model.arch = ml.get_arch();
    if (model.arch == LLM_ARCH_UNKNOWN) {
        throw std::runtime_error("unknown model architecture: '" + ml.get_arch_name() + "'");
    }
}

static void llm_load_hparams(
        llama_model_loader & ml,
        llama_model & model) {
    auto & hparams = model.hparams;
    const lm_gguf_context * ctx = ml.meta.get();

    // get metadata as string
    for (int i = 0; i < lm_gguf_get_n_kv(ctx); i++) {
        enum lm_gguf_type type = lm_gguf_get_kv_type(ctx, i);
        if (type == LM_GGUF_TYPE_ARRAY) {
            continue;
        }
        const char * name = lm_gguf_get_key(ctx, i);
        const std::string value = lm_gguf_kv_to_str(ctx, i);
        model.lm_gguf_kv.emplace(name, value);
    }

    // get general kv
    ml.get_key(LLM_KV_GENERAL_NAME, model.name, false);

    // get hparams kv
    ml.get_key(LLM_KV_VOCAB_SIZE, hparams.n_vocab, false) || ml.get_arr_n(LLM_KV_TOKENIZER_LIST, hparams.n_vocab);

    // everything past this point is not vocab-related
    if (hparams.vocab_only) {
        return;
    }

    ml.get_key(LLM_KV_CONTEXT_LENGTH,    hparams.n_ctx_train);
    ml.get_key(LLM_KV_EMBEDDING_LENGTH,  hparams.n_embd);
    ml.get_key(LLM_KV_BLOCK_COUNT,       hparams.n_layer);
    ml.get_key(LLM_KV_EXPERT_COUNT,      hparams.n_expert,      false);
    ml.get_key(LLM_KV_EXPERT_USED_COUNT, hparams.n_expert_used, false);

    LM_GGML_ASSERT(hparams.n_expert <= LLAMA_MAX_EXPERTS);
    LM_GGML_ASSERT(hparams.n_expert_used <= hparams.n_expert);
    if (hparams.n_expert > 0) {
        LM_GGML_ASSERT(hparams.n_expert_used > 0);
    } else {
        LM_GGML_ASSERT(hparams.n_expert_used == 0);
    }

    // zero-out the per-layer hparams
    std::fill(hparams.n_head_arr.begin(),    hparams.n_head_arr.end(),    0);
    std::fill(hparams.n_head_kv_arr.begin(), hparams.n_head_kv_arr.end(), 0);
    std::fill(hparams.n_ff_arr.begin(),      hparams.n_ff_arr.end(),      0);

    ml.get_key_or_arr(LLM_KV_FEED_FORWARD_LENGTH,  hparams.n_ff_arr,   hparams.n_layer);
    ml.get_key_or_arr(LLM_KV_ATTENTION_HEAD_COUNT, hparams.n_head_arr, hparams.n_layer);

    // n_head_kv is optional, default to n_head
    hparams.n_head_kv_arr = hparams.n_head_arr;

    ml.get_key_or_arr(LLM_KV_ATTENTION_HEAD_COUNT_KV, hparams.n_head_kv_arr, hparams.n_layer, false);

    bool rope_finetuned = false;
    ml.get_key(LLM_KV_ROPE_SCALING_FINETUNED, rope_finetuned, false);
    hparams.rope_finetuned = rope_finetuned;

    hparams.n_ctx_orig_yarn = hparams.n_ctx_train;
    ml.get_key(LLM_KV_ROPE_SCALING_ORIG_CTX_LEN, hparams.n_ctx_orig_yarn, false);

    // rope_freq_base (optional)
    hparams.rope_freq_base_train = 10000.0f;
    ml.get_key(LLM_KV_ROPE_FREQ_BASE, hparams.rope_freq_base_train, false);

    std::string rope_scaling("linear");
    ml.get_key(LLM_KV_ROPE_SCALING_TYPE, rope_scaling, false);
    hparams.rope_scaling_type_train = llama_rope_scaling_type_from_string(rope_scaling);
    LM_GGML_ASSERT(hparams.rope_scaling_type_train != LLAMA_ROPE_SCALING_TYPE_UNSPECIFIED);

    // rope_freq_scale (inverse of the kv) is optional
    float ropescale = 0.0f;
    if (!ml.get_key(LLM_KV_ROPE_SCALING_FACTOR, ropescale, false)) {
        // try the old key name
        ml.get_key(LLM_KV_ROPE_SCALE_LINEAR, ropescale, false);
    }
    hparams.rope_freq_scale_train = ropescale == 0.0f ? 1.0f : 1.0f/ropescale;

    ml.get_key(LLM_KV_ROPE_SCALING_ATTN_FACTOR, hparams.rope_attn_factor, false);

    // non-transformer models do not have attention heads
    if (hparams.n_head() > 0) {
        // gpt-neox n_rot = rotary_pct * (n_embd / n_head)
        // gpt-j n_rot = rotary_dim

        hparams.n_embd_head_k = hparams.n_embd / hparams.n_head();
        ml.get_key(LLM_KV_ATTENTION_KEY_LENGTH, hparams.n_embd_head_k, false);

        hparams.n_embd_head_v = hparams.n_embd / hparams.n_head();
        ml.get_key(LLM_KV_ATTENTION_VALUE_LENGTH, hparams.n_embd_head_v, false);

        // sanity check for n_rot (optional)
        hparams.n_rot = hparams.n_embd_head_k;

        ml.get_key(LLM_KV_ROPE_DIMENSION_COUNT, hparams.n_rot, false);

        if (model.arch == LLM_ARCH_LLAMA || model.arch == LLM_ARCH_FALCON) {
            if (hparams.n_rot != hparams.n_embd_head_k) {
                throw std::runtime_error(format("invalid n_rot: %u, expected %u", hparams.n_rot, hparams.n_embd_head_k));
            }
        }
    } else {
        hparams.n_rot = 0;
        hparams.n_embd_head_k = 0;
        hparams.n_embd_head_v = 0;
    }

    // arch-specific KVs
    switch (model.arch) {
        case LLM_ARCH_LLAMA:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);

                if (hparams.n_expert == 8) {
                    switch (hparams.n_layer) {
                        case 32: model.type = e_model::MODEL_8x7B; break;
                        case 56: model.type = e_model::MODEL_8x22B; break;
                        default: model.type = e_model::MODEL_UNKNOWN;
                    }
                } else {
                    switch (hparams.n_layer) {
                        case 16: model.type = e_model::MODEL_1B; break; // Llama 3.2 1B
                        case 22: model.type = e_model::MODEL_1B; break;
                        case 26: model.type = e_model::MODEL_3B; break;
                        case 28: model.type = e_model::MODEL_3B; break; // Llama 3.2 3B
                        // granite uses a vocab with len 49152
                        case 32: model.type = hparams.n_vocab == 49152 ? e_model::MODEL_3B : (hparams.n_vocab < 40000 ? e_model::MODEL_7B : e_model::MODEL_8B); break;
                        case 36: model.type = e_model::MODEL_8B; break; // granite
                        case 40: model.type = e_model::MODEL_13B; break;
                        case 48: model.type = e_model::MODEL_34B; break;
                        case 60: model.type = e_model::MODEL_30B; break;
                        case 80: model.type = hparams.n_head() == hparams.n_head_kv() ? e_model::MODEL_65B : e_model::MODEL_70B; break;
                        default: model.type = e_model::MODEL_UNKNOWN;
                    }
                }
            } break;
        case LLM_ARCH_MINICPM:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);

                switch (hparams.n_layer) {
                    case 40: model.type = e_model::MODEL_2B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_MINICPM3:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);
                ml.get_key(LLM_KV_ATTENTION_Q_LORA_RANK, hparams.n_lora_q);
                ml.get_key(LLM_KV_ATTENTION_KV_LORA_RANK, hparams.n_lora_kv);

                switch (hparams.n_layer) {
                    case 62: model.type = e_model::MODEL_4B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_GROK:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);

                switch (hparams.n_layer) {
                    case 64: model.type = e_model::MODEL_314B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_FALCON:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_EPS, hparams.f_norm_eps);

                switch (hparams.n_layer) {
                    case 32: model.type = e_model::MODEL_7B; break;
                    case 60: model.type = e_model::MODEL_40B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_BAICHUAN:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);
                switch (hparams.n_layer) {
                    case 32: model.type = e_model::MODEL_7B; break;
                    case 40: model.type = e_model::MODEL_13B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }

                if (model.type == e_model::MODEL_13B) {
                    // TODO: become GGUF KV parameter
                    hparams.f_max_alibi_bias = 8.0f;
                }
            } break;
        case LLM_ARCH_STARCODER:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_EPS, hparams.f_norm_eps);
                switch (hparams.n_layer) {
                    case 24: model.type = e_model::MODEL_1B; break;
                    case 36: model.type = e_model::MODEL_3B; break;
                    case 42: model.type = e_model::MODEL_7B; break;
                    case 40: model.type = e_model::MODEL_15B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_REFACT:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);
                switch (hparams.n_layer) {
                    case 32: model.type = e_model::MODEL_1B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }

                // TODO: become GGUF KV parameter
                hparams.f_max_alibi_bias = 8.0f;
            } break;
        case LLM_ARCH_BERT:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_EPS,    hparams.f_norm_eps);
                ml.get_key(LLM_KV_ATTENTION_CAUSAL,           hparams.causal_attn);
                ml.get_key(LLM_KV_TOKENIZER_TOKEN_TYPE_COUNT, hparams.n_vocab_type);
                ml.get_key(LLM_KV_POOLING_TYPE,               hparams.pooling_type, false);

                switch (hparams.n_layer) {
                    case 3:
                        model.type = e_model::MODEL_17M; break; // bge-micro
                    case 6:
                        model.type = e_model::MODEL_22M; break; // MiniLM-L6
                    case 12:
                        switch (hparams.n_embd) {
                            case 384: model.type = e_model::MODEL_33M; break; // MiniLM-L12, bge-small
                            case 768: model.type = e_model::MODEL_109M; break; // bge-base
                        } break;
                    case 24:
                        model.type = e_model::MODEL_335M; break; // bge-large
                }
            } break;
        case LLM_ARCH_JINA_BERT_V2:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_EPS,    hparams.f_norm_eps);
                ml.get_key(LLM_KV_ATTENTION_CAUSAL,           hparams.causal_attn);
                ml.get_key(LLM_KV_TOKENIZER_TOKEN_TYPE_COUNT, hparams.n_vocab_type);
                ml.get_key(LLM_KV_POOLING_TYPE,               hparams.pooling_type, false);
                hparams.f_max_alibi_bias = 8.0f;

                switch (hparams.n_layer) {
                    case 4:  model.type = e_model::MODEL_33M;  break; // jina-embeddings-small
                    case 12: model.type = e_model::MODEL_137M; break; // jina-embeddings-base
                }
            } break;
        case LLM_ARCH_NOMIC_BERT:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_EPS,    hparams.f_norm_eps);
                ml.get_key(LLM_KV_ATTENTION_CAUSAL,           hparams.causal_attn);
                ml.get_key(LLM_KV_TOKENIZER_TOKEN_TYPE_COUNT, hparams.n_vocab_type);
                ml.get_key(LLM_KV_POOLING_TYPE,               hparams.pooling_type);

                if (hparams.n_layer == 12 && hparams.n_embd == 768) {
                    model.type = e_model::MODEL_137M;
                }
            } break;
        case LLM_ARCH_BLOOM:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_EPS, hparams.f_norm_eps);

                switch (hparams.n_layer) {
                    case 24: model.type = e_model::MODEL_1B; break;
                    case 30:
                        switch (hparams.n_embd) {
                            case 2560: model.type = e_model::MODEL_3B; break;
                            case 4096: model.type = e_model::MODEL_7B; break;
                        } break;
                }

                // TODO: become GGUF KV parameter
                hparams.f_max_alibi_bias = 8.0f;
            } break;
        case LLM_ARCH_MPT:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_EPS,  hparams.f_norm_eps);
                ml.get_key(LLM_KV_ATTENTION_CLAMP_KQV,      hparams.f_clamp_kqv, false);
                ml.get_key(LLM_KV_ATTENTION_MAX_ALIBI_BIAS, hparams.f_max_alibi_bias);

                switch (hparams.n_layer) {
                    case 32: model.type = e_model::MODEL_7B; break;
                    case 48: model.type = e_model::MODEL_30B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_STABLELM:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_EPS, hparams.f_norm_eps);

                switch (hparams.n_layer) {
                    case 24: model.type = e_model::MODEL_1B; break;
                    case 32: model.type = e_model::MODEL_3B; break;
                    case 40: model.type = e_model::MODEL_12B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
               }
            } break;
        case LLM_ARCH_QWEN:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);

                switch (hparams.n_layer) {
                    case 32: model.type = e_model::MODEL_7B; break;
                    case 40: model.type = e_model::MODEL_13B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_QWEN2:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);
                switch (hparams.n_layer) {
                    case 24: model.type = hparams.n_embd == 1024 ? e_model::MODEL_0_5B : e_model::MODEL_1B; break;
                    case 28: model.type = hparams.n_embd == 1536 ? e_model::MODEL_1_5B : e_model::MODEL_7B; break;
                    case 32: model.type = e_model::MODEL_7B; break;
                    case 40: model.type = hparams.n_head() == 20 ? e_model::MODEL_4B : e_model::MODEL_13B; break;
                    case 80: model.type = e_model::MODEL_70B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_QWEN2MOE:
            {
                ml.get_key(LLM_KV_EXPERT_FEED_FORWARD_LENGTH, hparams.n_ff_exp, false);
                ml.get_key(LLM_KV_EXPERT_SHARED_FEED_FORWARD_LENGTH, hparams.n_ff_shexp, false);

                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);
                switch (hparams.n_layer) {
                    case 24: model.type = e_model::MODEL_A2_7B; break;
                    case 28: model.type = e_model::MODEL_57B_A14B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_PHI2:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_EPS, hparams.f_norm_eps);

                switch (hparams.n_layer) {
                    case 24: model.type = e_model::MODEL_1B; break;
                    case 32: model.type = e_model::MODEL_3B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_PHI3:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);

                switch (hparams.n_layer) {
                    case 24: model.type = e_model::MODEL_1B; break;
                    case 32: model.type = e_model::MODEL_3B; break;
                    case 40: model.type = e_model::MODEL_14B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }

                // for backward compatibility ; see: https://github.com/ggerganov/llama.cpp/pull/8931
                if ((hparams.n_layer == 32 || hparams.n_layer == 40) && hparams.n_ctx_train == 4096) {
                    // default value for Phi-3-mini-4k-instruct and Phi-3-medium-4k-instruct
                    hparams.n_swa = 2047;
                } else if (hparams.n_layer == 32 && hparams.n_head_kv(0) == 32 && hparams.n_ctx_train == 131072) {
                    // default value for Phi-3-mini-128k-instruct
                    hparams.n_swa = 262144;
                } else if (hparams.n_layer == 40 && hparams.n_ctx_train == 131072) {
                    // default value for Phi-3-medium-128k-instruct
                    hparams.n_swa = 131072;
                }
                bool found_swa = ml.get_key(LLM_KV_ATTENTION_SLIDING_WINDOW, hparams.n_swa, false);
                if (!found_swa && hparams.n_swa == 0) {
                    throw std::runtime_error("invalid value for sliding_window");
                }
            } break;
        case LLM_ARCH_PLAMO:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);

                switch (hparams.n_layer) {
                    case 40: model.type = e_model::MODEL_13B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
               }
            } break;
        case LLM_ARCH_GPT2:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_EPS, hparams.f_norm_eps);
                switch (hparams.n_layer) {
                    case 12: model.type = e_model::MODEL_SMALL; break;
                    case 24: model.type = e_model::MODEL_MEDIUM; break;
                    case 36: model.type = e_model::MODEL_LARGE; break;
                    case 48: model.type = e_model::MODEL_XL; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_CODESHELL:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_EPS, hparams.f_norm_eps);
                switch (hparams.n_layer) {
                    case 42: model.type = e_model::MODEL_7B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_ORION:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_EPS, hparams.f_norm_eps);

                switch (hparams.n_layer) {
                    case 40: model.type = e_model::MODEL_14B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_INTERNLM2:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);
                switch (hparams.n_layer) {
                    case 32: model.type = e_model::MODEL_7B; break;
                    case 48: model.type = e_model::MODEL_20B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_GEMMA:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);

                switch (hparams.n_layer) {
                    case 18: model.type = e_model::MODEL_2B; break;
                    case 28: model.type = e_model::MODEL_7B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
               }
            } break;
        case LLM_ARCH_GEMMA2:
            {
                hparams.n_swa = 4096; // default value of gemma 2
                ml.get_key(LLM_KV_ATTENTION_SLIDING_WINDOW, hparams.n_swa, false);
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);
                ml.get_key(LLM_KV_ATTN_LOGIT_SOFTCAPPING, hparams.f_attn_logit_softcapping, false);
                ml.get_key(LLM_KV_FINAL_LOGIT_SOFTCAPPING, hparams.f_final_logit_softcapping, false);
                hparams.attn_soft_cap = true;

                switch (hparams.n_layer) {
                    case 26: model.type = e_model::MODEL_2B; break;
                    case 42: model.type = e_model::MODEL_9B; break;
                    case 46: model.type = e_model::MODEL_27B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
               }
            } break;
        case LLM_ARCH_STARCODER2:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_EPS, hparams.f_norm_eps);
                switch (hparams.n_layer) {
                    case 30: model.type = e_model::MODEL_3B; break;
                    case 32: model.type = e_model::MODEL_7B; break;
                    case 40: model.type = e_model::MODEL_15B; break;
                    case 52: model.type = e_model::MODEL_20B; break; // granite
                    case 88: model.type = e_model::MODEL_34B; break; // granite
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_MAMBA:
            {
                ml.get_key(LLM_KV_SSM_CONV_KERNEL,    hparams.ssm_d_conv);
                ml.get_key(LLM_KV_SSM_INNER_SIZE,     hparams.ssm_d_inner);
                ml.get_key(LLM_KV_SSM_STATE_SIZE,     hparams.ssm_d_state);
                ml.get_key(LLM_KV_SSM_TIME_STEP_RANK, hparams.ssm_dt_rank);
                ml.get_key(LLM_KV_SSM_DT_B_C_RMS, hparams.ssm_dt_b_c_rms, false);

                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);

                switch (hparams.n_layer) {
                    case 24:
                        switch (hparams.n_embd) {
                            case 768: model.type = e_model::MODEL_SMALL; break;
                            default: model.type = e_model::MODEL_UNKNOWN;
                        } break;
                    case 48:
                        switch (hparams.n_embd) {
                            case 1024: model.type = e_model::MODEL_MEDIUM; break;
                            case 1536: model.type = e_model::MODEL_LARGE; break;
                            case 2048: model.type = e_model::MODEL_XL; break;
                            default: model.type = e_model::MODEL_UNKNOWN;
                        } break;
                    case 64:
                        switch (hparams.n_embd) {
                            case 2560: model.type = e_model::MODEL_3B; break;
                            default: model.type = e_model::MODEL_UNKNOWN;
                        } break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_XVERSE:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);
                switch (hparams.n_layer) {
                    case 32: model.type = e_model::MODEL_7B; break;
                    case 40: model.type = e_model::MODEL_13B; break;
                    case 80: model.type = e_model::MODEL_65B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_COMMAND_R:
            {
                ml.get_key(LLM_KV_LOGIT_SCALE, hparams.f_logit_scale);
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_EPS, hparams.f_norm_eps);
                switch (hparams.n_layer) {
                    case 40: model.type = e_model::MODEL_35B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_DBRX:
        {
            ml.get_key(LLM_KV_ATTENTION_LAYERNORM_EPS,  hparams.f_norm_eps);
            ml.get_key(LLM_KV_ATTENTION_CLAMP_KQV,      hparams.f_clamp_kqv);

            switch (hparams.n_layer) {
                case 40: model.type = e_model::MODEL_16x12B; break;
                default: model.type = e_model::MODEL_UNKNOWN;
            }
        } break;
        case LLM_ARCH_OLMO:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_EPS, hparams.f_norm_eps);
                ml.get_key(LLM_KV_ATTENTION_CLAMP_KQV,     hparams.f_clamp_kqv, false);

                switch (hparams.n_layer) {
                    case 22: model.type = e_model::MODEL_1B; break;
                    case 32: model.type = e_model::MODEL_7B; break;
                    case 80: model.type = e_model::MODEL_70B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_OLMO_1124:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);

                switch (hparams.n_layer) {
                    case 16: model.type = e_model::MODEL_1B; break;
                    case 32: model.type = e_model::MODEL_7B; break;
                    case 40: model.type = e_model::MODEL_13B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_OLMOE:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);
                switch (hparams.n_layer) {
                    case 16: model.type = e_model::MODEL_A1_7B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_OPENELM:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);

                switch (hparams.n_layer) {
                case 16: model.type = e_model::MODEL_270M; break;
                case 20: model.type = e_model::MODEL_450M; break;
                case 28: model.type = e_model::MODEL_1B; break;
                case 36: model.type = e_model::MODEL_3B; break;
                default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_GPTNEOX:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_EPS, hparams.f_norm_eps);
                ml.get_key(LLM_KV_USE_PARALLEL_RESIDUAL, hparams.use_par_res);
                switch (hparams.n_layer) {
                    case 6:
                        switch (hparams.n_ff()) {
                            case 512: model.type = e_model::MODEL_14M; break;
                            case 2048: model.type = e_model::MODEL_70M; break;
                            default: model.type = e_model::MODEL_UNKNOWN;
                        } break;
                    case 12:
                        switch (hparams.n_ff()) {
                            case 3072: model.type = e_model::MODEL_160M; break;
                            default: model.type = e_model::MODEL_UNKNOWN;
                        } break;
                    case 16:
                        switch (hparams.n_ff()) {
                            case 8192: model.type = e_model::MODEL_1B; break;
                            default: model.type = e_model::MODEL_UNKNOWN;
                        } break;
                    case 24:
                        switch (hparams.n_ff()) {
                            case 4096: model.type = e_model::MODEL_410M; break;
                            case 8192: model.type = e_model::MODEL_1_4B; break;
                            default: model.type = e_model::MODEL_UNKNOWN;
                        } break;
                    case 32:
                        switch (hparams.n_ff()) {
                            case 10240: model.type = e_model::MODEL_2_8B; break;
                            case 16384: model.type = e_model::MODEL_6_9B; break;
                            default: model.type = e_model::MODEL_UNKNOWN;
                        } break;
                    case 36:
                        switch (hparams.n_ff()) {
                            case 20480: model.type = e_model::MODEL_12B; break;
                            default: model.type = e_model::MODEL_UNKNOWN;
                        } break;
                    case 44:
                        switch (hparams.n_ff()) {
                            case 24576: model.type = e_model::MODEL_20B; break;
                            default: model.type = e_model::MODEL_UNKNOWN;
                        } break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_ARCTIC:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);

                if (hparams.n_expert == 128) {
                    switch (hparams.n_layer) {
                        case 35: model.type = e_model::MODEL_10B_128x3_66B; break;
                        default: model.type = e_model::MODEL_UNKNOWN;
                    }
                } else {
                    model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_DEEPSEEK2:
            {
                bool is_lite = (hparams.n_layer == 27);
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);
                ml.get_key(LLM_KV_LEADING_DENSE_BLOCK_COUNT, hparams.n_layer_dense_lead);
                if (!is_lite) {
                    ml.get_key(LLM_KV_ATTENTION_Q_LORA_RANK, hparams.n_lora_q);
                }
                ml.get_key(LLM_KV_ATTENTION_KV_LORA_RANK, hparams.n_lora_kv);
                ml.get_key(LLM_KV_EXPERT_FEED_FORWARD_LENGTH, hparams.n_ff_exp);
                ml.get_key(LLM_KV_EXPERT_SHARED_COUNT, hparams.n_expert_shared);
                ml.get_key(LLM_KV_EXPERT_WEIGHTS_SCALE, hparams.expert_weights_scale);
                ml.get_key(LLM_KV_ROPE_SCALING_YARN_LOG_MUL, hparams.rope_yarn_log_mul);

                switch (hparams.n_layer) {
                    case 27: model.type = e_model::MODEL_16B; break;
                    case 60: model.type = e_model::MODEL_236B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_CHATGLM:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);
                switch (hparams.n_layer) {
                    case 28: model.type = e_model::MODEL_6B; break;
                    case 40: model.type = e_model::MODEL_9B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_BITNET:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);

                switch (hparams.n_layer) {
                    case 26: model.type = e_model::MODEL_3B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_T5:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);
                ml.get_key(LLM_KV_ATTENTION_RELATIVE_BUCKETS_COUNT, hparams.n_rel_attn_bkts);

                uint32_t dec_start_token_id;
                if (ml.get_key(LLM_KV_DECODER_START_TOKEN_ID, dec_start_token_id, false)) {
                    hparams.dec_start_token_id = dec_start_token_id;
                }

                switch (hparams.n_layer) {
                    case 6:  model.type = e_model::MODEL_60M;  break; // t5-small
                    case 8:  model.type = e_model::MODEL_80M;  break; // flan-t5-small
                    case 12:
                        switch (hparams.n_ff()) {
                            case 3072: model.type = e_model::MODEL_220M; break; // t5-base
                            case 2048: model.type = e_model::MODEL_250M; break; // flan-t5-base
                            default: model.type = e_model::MODEL_UNKNOWN;
                        } break;
                    case 24:
                        switch (hparams.n_ff()) {
                            case 4096:  model.type = e_model::MODEL_770M; break; // t5-large
                            case 2816:  model.type = e_model::MODEL_780M; break; // flan-t5-large
                            case 16384: model.type = e_model::MODEL_3B;   break; // t5-3b
                            case 5120:  model.type = e_model::MODEL_3B;   break; // flan-t5-xl
                            case 65536: model.type = e_model::MODEL_11B;  break; // t5-11b
                            case 10240: model.type = e_model::MODEL_11B;  break; // flan-t5-xxl
                            default: model.type = e_model::MODEL_UNKNOWN;
                        } break;
                    default: model.type = e_model::MODEL_UNKNOWN;
               }
            } break;
        case LLM_ARCH_T5ENCODER:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);
                ml.get_key(LLM_KV_ATTENTION_RELATIVE_BUCKETS_COUNT, hparams.n_rel_attn_bkts);
                model.type = e_model::MODEL_UNKNOWN;
            } break;
        case LLM_ARCH_JAIS:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_EPS, hparams.f_norm_eps);
                ml.get_key(LLM_KV_ATTENTION_MAX_ALIBI_BIAS, hparams.f_max_alibi_bias);

                switch (hparams.n_layer) {
                    case 24: model.type = e_model::MODEL_1_3B; break;
                    case 40: model.type = e_model::MODEL_13B; break;
                    /* TODO: add variants */
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_NEMOTRON:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_EPS, hparams.f_norm_eps);
                switch (hparams.n_layer) {
                    case 32: model.type = e_model::MODEL_4B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_EXAONE:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);

                switch (hparams.n_layer) {
                    case 32: model.type = e_model::MODEL_8B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_RWKV6:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_EPS, hparams.f_norm_eps);
                ml.get_key(LLM_KV_WKV_HEAD_SIZE, hparams.wkv_head_size);
                ml.get_key(LLM_KV_TIME_MIX_EXTRA_DIM, hparams.time_mix_extra_dim);
                ml.get_key(LLM_KV_TIME_DECAY_EXTRA_DIM, hparams.time_decay_extra_dim);
                ml.get_key(LLM_KV_RESCALE_EVERY_N_LAYERS, hparams.rescale_every_n_layers, false);

                switch (hparams.n_layer) {
                    case 24: model.type = e_model::MODEL_1_6B; break;
                    case 32:
                        switch (hparams.n_embd) {
                            case 2560: model.type = e_model::MODEL_3B; break;
                            case 4096: model.type = e_model::MODEL_7B; break;
                            default: model.type = e_model::MODEL_UNKNOWN;
                        } break;
                    case 61: model.type = e_model::MODEL_14B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_GRANITE:
        case LLM_ARCH_GRANITE_MOE:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);
                ml.get_key(LLM_KV_LOGIT_SCALE, hparams.f_logit_scale);
                ml.get_key(LLM_KV_RESIDUAL_SCALE, hparams.f_residual_scale);
                ml.get_key(LLM_KV_EMBEDDING_SCALE, hparams.f_embedding_scale);
                ml.get_key(LLM_KV_ATTENTION_SCALE, hparams.f_attention_scale);

                switch (hparams.n_layer) {
                    case 32: model.type = e_model::MODEL_3B; break;
                    case 40: model.type = e_model::MODEL_3B; break;
                    // Add additional layer/vocab/etc checks here for other model sizes
                    default: model.type = e_model::MODEL_UNKNOWN;
                }
            } break;
        case LLM_ARCH_CHAMELEON:
            {
                ml.get_key(LLM_KV_ATTENTION_LAYERNORM_RMS_EPS, hparams.f_norm_rms_eps);
                hparams.f_norm_eps = 1e-5;  // eps for qk-norm, torch default
                ml.get_key(LLM_KV_SWIN_NORM, hparams.swin_norm);

                switch (hparams.n_layer) {
                    case 32: model.type = e_model::MODEL_7B; break;
                    case 48: model.type = e_model::MODEL_34B; break;
                    default: model.type = e_model::MODEL_UNKNOWN;
               }
            } break;
        default: (void)0;
    }

    model.ftype = ml.ftype;

    if (hparams.f_max_alibi_bias > 0.0f) {
        hparams.use_alibi = true;
    }

    hparams.rope_type = llama_rope_type(&model);
}

static void llm_load_vocab(
        llama_model_loader & ml,
        llama_model & model) {
    auto & vocab = model.vocab;

    struct lm_gguf_context * ctx = ml.meta.get();

    const auto kv = LLM_KV(model.arch);

    // determine vocab type
    {
        std::string tokenizer_model;
        std::string tokenizer_pre;

        ml.get_key(LLM_KV_TOKENIZER_MODEL, tokenizer_model);
        ml.get_key(LLM_KV_TOKENIZER_PRE,   tokenizer_pre, false);

        if (tokenizer_model == "no_vocab") {
            vocab.type = LLAMA_VOCAB_TYPE_NONE;

            // default special tokens
            vocab.special_bos_id  = LLAMA_TOKEN_NULL;
            vocab.special_eos_id  = LLAMA_TOKEN_NULL;
            vocab.special_unk_id  = LLAMA_TOKEN_NULL;
            vocab.special_sep_id  = LLAMA_TOKEN_NULL;
            vocab.special_pad_id  = LLAMA_TOKEN_NULL;
            vocab.special_cls_id  = LLAMA_TOKEN_NULL;
            vocab.special_mask_id = LLAMA_TOKEN_NULL;
            vocab.linefeed_id     = LLAMA_TOKEN_NULL;

            // read vocab size from metadata
            if (!ml.get_key(LLM_KV_VOCAB_SIZE, vocab.n_vocab, false)) {
                vocab.n_vocab = 0;
                LLAMA_LOG_WARN("%s: there is no vocab_size in metadata, vocab.n_vocab will be set to %u\n", __func__, vocab.n_vocab);
            }
            return;
        }

        if (tokenizer_model == "llama") {
            vocab.type = LLAMA_VOCAB_TYPE_SPM;

            // default special tokens
            vocab.special_bos_id  = 1;
            vocab.special_eos_id  = 2;
            vocab.special_unk_id  = 0;
            vocab.special_sep_id  = LLAMA_TOKEN_NULL;
            vocab.special_pad_id  = LLAMA_TOKEN_NULL;
            vocab.special_cls_id  = LLAMA_TOKEN_NULL;
            vocab.special_mask_id = LLAMA_TOKEN_NULL;
        } else if (tokenizer_model == "bert") {
            vocab.type = LLAMA_VOCAB_TYPE_WPM;

            // default special tokens
            vocab.special_bos_id  = LLAMA_TOKEN_NULL;
            vocab.special_eos_id  = LLAMA_TOKEN_NULL;
            vocab.special_unk_id  = 100;
            vocab.special_sep_id  = 102;
            vocab.special_pad_id  = 0;
            vocab.special_cls_id  = 101;
            vocab.special_mask_id = 103;
        } else if (tokenizer_model == "gpt2") {
            vocab.type = LLAMA_VOCAB_TYPE_BPE;

            // read bpe merges and populate bpe ranks
            const int merges_keyidx = lm_gguf_find_key(ctx, kv(LLM_KV_TOKENIZER_MERGES).c_str());
            if (merges_keyidx == -1) {
                throw std::runtime_error("cannot find tokenizer merges in model file\n");
            }

            const int n_merges = lm_gguf_get_arr_n(ctx, merges_keyidx);
            for (int i = 0; i < n_merges; i++) {
                const std::string word = lm_gguf_get_arr_str(ctx, merges_keyidx, i);
                LM_GGML_ASSERT(unicode_cpts_from_utf8(word).size() > 0);

                std::string first;
                std::string second;

                const size_t pos = word.find(' ', 1);

                if (pos != std::string::npos) {
                    first  = word.substr(0, pos);
                    second = word.substr(pos + 1);
                }

                vocab.bpe_ranks.emplace(std::make_pair(first, second), i);
            }

            // default special tokens
            vocab.special_bos_id  = 11;
            vocab.special_eos_id  = 11;
            vocab.special_unk_id  = LLAMA_TOKEN_NULL;
            vocab.special_sep_id  = LLAMA_TOKEN_NULL;
            vocab.special_pad_id  = LLAMA_TOKEN_NULL;
            vocab.special_cls_id  = LLAMA_TOKEN_NULL;
            vocab.special_mask_id = LLAMA_TOKEN_NULL;
        } else if (tokenizer_model == "t5") {
            vocab.type = LLAMA_VOCAB_TYPE_UGM;

            // default special tokens
            vocab.special_bos_id  = LLAMA_TOKEN_NULL;
            vocab.special_eos_id  = 1;
            vocab.special_unk_id  = 2;
            vocab.special_sep_id  = LLAMA_TOKEN_NULL;
            vocab.special_pad_id  = 0;
            vocab.special_cls_id  = LLAMA_TOKEN_NULL;
            vocab.special_mask_id = LLAMA_TOKEN_NULL;

            const int precompiled_charsmap_keyidx = lm_gguf_find_key(ctx, kv(LLM_KV_TOKENIZER_PRECOMPILED_CHARSMAP).c_str());
            if (precompiled_charsmap_keyidx != -1) {
                size_t n_precompiled_charsmap = lm_gguf_get_arr_n(ctx, precompiled_charsmap_keyidx);
                const char * precompiled_charsmap = (const char *) lm_gguf_get_arr_data(ctx, precompiled_charsmap_keyidx);
                vocab.precompiled_charsmap.assign(precompiled_charsmap, precompiled_charsmap + n_precompiled_charsmap);
#ifdef IS_BIG_ENDIAN
                // correct endiannes of data in precompiled_charsmap binary blob
                uint32_t * xcda_blob_size = (uint32_t *) &vocab.precompiled_charsmap[0];
                *xcda_blob_size = __builtin_bswap32(*xcda_blob_size);
                assert(*xcda_blob_size + sizeof(uint32_t) < n_precompiled_charsmap);
                size_t xcda_array_size = *xcda_blob_size / sizeof(uint32_t);
                uint32_t * xcda_array = (uint32_t *) &vocab.precompiled_charsmap[sizeof(uint32_t)];
                for (size_t i = 0; i < xcda_array_size; ++i) {
                    xcda_array[i] = __builtin_bswap32(xcda_array[i]);
                }
#endif
            }
        } else if (tokenizer_model == "rwkv") {
            vocab.type = LLAMA_VOCAB_TYPE_RWKV;

            // default special tokens
            vocab.special_bos_id = LLAMA_TOKEN_NULL;
            vocab.special_eos_id = LLAMA_TOKEN_NULL;
            vocab.special_unk_id = LLAMA_TOKEN_NULL;
            vocab.special_sep_id = LLAMA_TOKEN_NULL;
            vocab.special_pad_id = LLAMA_TOKEN_NULL;
        } else {
            throw std::runtime_error(format("unknown tokenizer: '%s'", tokenizer_model.c_str()));
        }

        // for now, only BPE models have pre-tokenizers
        if (vocab.type == LLAMA_VOCAB_TYPE_BPE) {
            vocab.tokenizer_add_space_prefix = false;
            vocab.tokenizer_clean_spaces = true;
            if (tokenizer_pre.empty()) {
                LLAMA_LOG_WARN("%s: missing pre-tokenizer type, using: 'default'\n", __func__);
                LLAMA_LOG_WARN("%s:                                             \n", __func__);
                LLAMA_LOG_WARN("%s: ************************************        \n", __func__);
                LLAMA_LOG_WARN("%s: GENERATION QUALITY WILL BE DEGRADED!        \n", __func__);
                LLAMA_LOG_WARN("%s: CONSIDER REGENERATING THE MODEL             \n", __func__);
                LLAMA_LOG_WARN("%s: ************************************        \n", __func__);
                LLAMA_LOG_WARN("%s:                                             \n", __func__);
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_DEFAULT;
            } else if (tokenizer_pre == "default") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_DEFAULT;
            } else if (
                    tokenizer_pre == "llama3"   ||
                    tokenizer_pre == "llama-v3" ||
                    tokenizer_pre == "llama-bpe") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_LLAMA3;
                vocab.tokenizer_ignore_merges = true;
                vocab.tokenizer_add_bos = true;
            } else if (
                    tokenizer_pre == "deepseek-llm") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_DEEPSEEK_LLM;
                vocab.tokenizer_clean_spaces = false;
            } else if (
                    tokenizer_pre == "deepseek-coder") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_DEEPSEEK_CODER;
                vocab.tokenizer_clean_spaces = false;
            } else if (
                    tokenizer_pre == "falcon") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_FALCON;
            } else if (
                    tokenizer_pre == "mpt") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_MPT;
            } else if (
                    tokenizer_pre == "starcoder") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_STARCODER;
            } else if (
                    tokenizer_pre == "gpt-2"   ||
                    tokenizer_pre == "phi-2"   ||
                    tokenizer_pre == "jina-es" ||
                    tokenizer_pre == "jina-de" ||
                    tokenizer_pre == "jina-v1-en" ||
                    tokenizer_pre == "jina-v2-es" ||
                    tokenizer_pre == "jina-v2-de" ||
                    tokenizer_pre == "jina-v2-code") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_GPT2;
            } else if (
                    tokenizer_pre == "refact") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_REFACT;
            } else if (
                tokenizer_pre == "command-r") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_COMMAND_R;
                vocab.tokenizer_clean_spaces = false;
            } else if (
                tokenizer_pre == "qwen2") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_QWEN2;
                vocab.tokenizer_clean_spaces = false;
            } else if (
                tokenizer_pre == "stablelm2") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_STABLELM2;
            } else if (
                tokenizer_pre == "olmo") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_OLMO;
            } else if (
                tokenizer_pre == "dbrx") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_DBRX;
            } else if (
                tokenizer_pre == "smaug-bpe") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_SMAUG;
            } else if (
                tokenizer_pre == "poro-chat") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_PORO;
                vocab.tokenizer_clean_spaces = false;
            } else if (
                tokenizer_pre == "chatglm-bpe") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_CHATGLM4;
                vocab.special_bos_id = LLAMA_TOKEN_NULL;
            } else if (
                tokenizer_pre == "viking") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_VIKING;
                vocab.tokenizer_clean_spaces = false;
            } else if (
                tokenizer_pre == "jais") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_JAIS;
            } else if (
                tokenizer_pre == "tekken") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_TEKKEN;
                vocab.tokenizer_clean_spaces = false;
                vocab.tokenizer_ignore_merges = true;
                vocab.tokenizer_add_bos = true;
            } else if (
                tokenizer_pre == "smollm") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_SMOLLM;
                vocab.tokenizer_clean_spaces = false;
            } else if (
                tokenizer_pre == "codeshell") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_CODESHELL;
            } else if (
                tokenizer_pre == "bloom") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_BLOOM;
            } else if (
                tokenizer_pre == "gpt3-finnish") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_GPT3_FINNISH;
            } else if (
                tokenizer_pre == "exaone") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_EXAONE;
            } else if (
                tokenizer_pre == "chameleon") {
                vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_CHAMELEON;
                vocab.tokenizer_add_bos = true;
                vocab.tokenizer_clean_spaces = false;
            } else {
                throw std::runtime_error(format("unknown pre-tokenizer type: '%s'", tokenizer_pre.c_str()));
            }
        } else if (vocab.type == LLAMA_VOCAB_TYPE_SPM) {
            vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_DEFAULT;
            vocab.tokenizer_add_space_prefix = true;
            vocab.tokenizer_clean_spaces = false;
            vocab.tokenizer_add_bos = true;
            vocab.tokenizer_add_eos = false;
        } else if (vocab.type == LLAMA_VOCAB_TYPE_WPM) {
            vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_DEFAULT;
            vocab.tokenizer_add_space_prefix = false;
            vocab.tokenizer_clean_spaces = true;
            vocab.tokenizer_add_bos = true;
            vocab.tokenizer_add_eos = false;
        } else if (vocab.type == LLAMA_VOCAB_TYPE_UGM) {
            vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_DEFAULT;
            vocab.tokenizer_add_bos = false;
            vocab.tokenizer_add_eos = true;
        } else if (vocab.type == LLAMA_VOCAB_TYPE_RWKV) {
            vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_DEFAULT;
            vocab.tokenizer_add_space_prefix = false;
            vocab.tokenizer_clean_spaces = false;
            vocab.tokenizer_add_bos = false;
            vocab.tokenizer_add_eos = false;
        } else {
            vocab.type_pre = LLAMA_VOCAB_PRE_TYPE_DEFAULT;
        }

        ml.get_key(LLM_KV_TOKENIZER_ADD_PREFIX,      vocab.tokenizer_add_space_prefix,         false);
        ml.get_key(LLM_KV_TOKENIZER_REMOVE_EXTRA_WS, vocab.tokenizer_remove_extra_whitespaces, false);
    }

    const int token_idx = lm_gguf_find_key(ctx, kv(LLM_KV_TOKENIZER_LIST).c_str());
    if (token_idx == -1) {
        throw std::runtime_error("cannot find tokenizer vocab in model file\n");
    }

    const float * scores = nullptr;
    const int score_idx = lm_gguf_find_key(ctx, kv(LLM_KV_TOKENIZER_SCORES).c_str());
    if (score_idx != -1) {
        scores = (const float * ) lm_gguf_get_arr_data(ctx, score_idx);
    }

    const int * toktypes = nullptr;
    const int toktype_idx = lm_gguf_find_key(ctx, kv(LLM_KV_TOKENIZER_TOKEN_TYPE).c_str());
    if (toktype_idx != -1) {
        toktypes = (const int * ) lm_gguf_get_arr_data(ctx, toktype_idx);
    }

    const uint32_t n_vocab = lm_gguf_get_arr_n(ctx, token_idx);

    vocab.n_vocab = n_vocab;
    vocab.id_to_token.resize(n_vocab);

    for (uint32_t i = 0; i < n_vocab; i++) {
        std::string word = lm_gguf_get_arr_str(ctx, token_idx, i);

        //LM_GGML_ASSERT(unicode_cpts_from_utf8(word).size() > 0);
        if (word.empty()) {
            LLAMA_LOG_WARN("%s: empty token at index %u\n", __func__, i);
            word = "[EMPTY_" + std::to_string(i) + "]";
        }

        vocab.token_to_id[word] = i;
        vocab.max_token_len = std::max(vocab.max_token_len, (int) word.size());

        auto & token_data = vocab.id_to_token[i];
        token_data.text  = std::move(word);
        token_data.score = scores ? scores[i] : 0.0f;
        token_data.attr  = LLAMA_TOKEN_ATTR_NORMAL;

        if (toktypes) {  //TODO: remove, required until per token attributes are available from GGUF file
            switch(toktypes[i]) {
                case LLAMA_TOKEN_TYPE_UNKNOWN:      token_data.attr = LLAMA_TOKEN_ATTR_UNKNOWN;      break;
                case LLAMA_TOKEN_TYPE_UNUSED:       token_data.attr = LLAMA_TOKEN_ATTR_UNUSED;       break;
                case LLAMA_TOKEN_TYPE_NORMAL:       token_data.attr = LLAMA_TOKEN_ATTR_NORMAL;       break;
                case LLAMA_TOKEN_TYPE_CONTROL:      token_data.attr = LLAMA_TOKEN_ATTR_CONTROL;      break;
                case LLAMA_TOKEN_TYPE_USER_DEFINED: token_data.attr = LLAMA_TOKEN_ATTR_USER_DEFINED; break;
                case LLAMA_TOKEN_TYPE_BYTE:         token_data.attr = LLAMA_TOKEN_ATTR_BYTE;         break;
                case LLAMA_TOKEN_TYPE_UNDEFINED:    token_data.attr = LLAMA_TOKEN_ATTR_UNDEFINED;    break;
                default:                            token_data.attr = LLAMA_TOKEN_ATTR_UNDEFINED;    break;
            }
        }
    }
    LM_GGML_ASSERT(vocab.id_to_token.size() == vocab.token_to_id.size());

    vocab.init_tokenizer();

    // determine the newline token: LLaMA "<0x0A>" == 10 == '\n', Falcon 193 == '\n'
    if (vocab.type == LLAMA_VOCAB_TYPE_SPM) {
        try {
            vocab.linefeed_id = llama_byte_to_token_impl(vocab, '\n');
        } catch (const std::exception & e) {
            LLAMA_LOG_WARN("%s: SPM vocabulary, but newline token not found: %s! Using special_pad_id instead.", __func__, e.what());
            vocab.linefeed_id = vocab.special_pad_id;
        }
    } else if (vocab.type == LLAMA_VOCAB_TYPE_WPM) {
        vocab.linefeed_id = vocab.special_pad_id;
    } else if (vocab.type == LLAMA_VOCAB_TYPE_RWKV) {
        const std::vector<int> ids = llama_tokenize_internal(vocab, "\n", false);
        LM_GGML_ASSERT(!ids.empty() && "model vocab missing newline token");
        vocab.linefeed_id = ids[0];
    } else {
        const std::vector<int> ids = llama_tokenize_internal(vocab, "\xC4\x8A", false); // U+010A

        //LM_GGML_ASSERT(!ids.empty() && "model vocab missing newline token");
        if (ids.empty()) {
            LLAMA_LOG_WARN("%s: model vocab missing newline token, using special_pad_id instead\n", __func__);
            vocab.linefeed_id = vocab.special_pad_id;
        } else {
            vocab.linefeed_id = ids[0];
        }
    }

    // special tokens
    {
        const std::vector<std::pair<enum llm_kv, int32_t &>> special_token_types = {
            { LLM_KV_TOKENIZER_BOS_ID,     vocab.special_bos_id     },
            { LLM_KV_TOKENIZER_EOS_ID,     vocab.special_eos_id     },
            { LLM_KV_TOKENIZER_EOT_ID,     vocab.special_eot_id     },
            { LLM_KV_TOKENIZER_EOM_ID,     vocab.special_eom_id     },
            { LLM_KV_TOKENIZER_UNK_ID,     vocab.special_unk_id     },
            { LLM_KV_TOKENIZER_SEP_ID,     vocab.special_sep_id     },
            { LLM_KV_TOKENIZER_PAD_ID,     vocab.special_pad_id     },
            { LLM_KV_TOKENIZER_CLS_ID,     vocab.special_cls_id     },
            { LLM_KV_TOKENIZER_MASK_ID,    vocab.special_mask_id    },
            { LLM_KV_TOKENIZER_FIM_PRE_ID, vocab.special_fim_pre_id },
            { LLM_KV_TOKENIZER_FIM_SUF_ID, vocab.special_fim_suf_id },
            { LLM_KV_TOKENIZER_FIM_MID_ID, vocab.special_fim_mid_id },
            { LLM_KV_TOKENIZER_FIM_PAD_ID, vocab.special_fim_pad_id },
            { LLM_KV_TOKENIZER_FIM_REP_ID, vocab.special_fim_rep_id },
            { LLM_KV_TOKENIZER_FIM_SEP_ID, vocab.special_fim_sep_id },

            // deprecated
            { LLM_KV_TOKENIZER_PREFIX_ID, vocab.special_fim_pre_id },
            { LLM_KV_TOKENIZER_SUFFIX_ID, vocab.special_fim_suf_id },
            { LLM_KV_TOKENIZER_MIDDLE_ID, vocab.special_fim_mid_id },
        };

        for (const auto & it : special_token_types) {
            const std::string & key = kv(std::get<0>(it));
            int32_t & id = std::get<1>(it);

            uint32_t new_id;
            if (!ml.get_key(std::get<0>(it), new_id, false)) {
                continue;
            }
            if (new_id >= vocab.id_to_token.size()) {
                LLAMA_LOG_WARN("%s: bad special token: '%s' = %ud, using default id %d\n",
                    __func__, key.c_str(), new_id, id);
            } else {
                id = new_id;
            }
        }

        // Handle add_bos_token and add_eos_token
        {
            bool temp = true;

            if (ml.get_key(LLM_KV_TOKENIZER_ADD_BOS, temp, false)) {
                vocab.tokenizer_add_bos = temp;
            }
            if (ml.get_key(LLM_KV_TOKENIZER_ADD_EOS, temp, false)) {
                vocab.tokenizer_add_eos = temp;
            }
        }

        // auto-detect special tokens by text
        // TODO: convert scripts should provide these tokens through the KV metadata LLM_KV_TOKENIZER_...
        //       for now, we apply this workaround to find the tokens based on their text

        for (const auto & t : vocab.token_to_id) {
            // find EOT token: "<|eot_id|>", "<|im_end|>", "<end_of_turn>", etc.
            if (vocab.special_eot_id == LLAMA_TOKEN_NULL) {
                if (false
                        || t.first == "<|eot_id|>"
                        || t.first == "<|im_end|>"
                        || t.first == "<|end|>"
                        || t.first == "<end_of_turn>"
                        || t.first == "<|endoftext|>"
                        || t.first == "<EOT>"
                        || t.first == "<｜end▁of▁sentence｜>" // DeepSeek
                   ) {
                    vocab.special_eot_id = t.second;
                    if ((vocab.id_to_token[t.second].attr & LLAMA_TOKEN_ATTR_CONTROL) == 0) {
                        LLAMA_LOG_WARN("%s: control-looking token: %6d '%s' was not control-type; this is probably a bug in the model. its type will be overridden\n",
                                __func__, t.second, t.first.c_str());
                        vocab.id_to_token[t.second].attr = LLAMA_TOKEN_ATTR_CONTROL;
                    }
                }
            }

            // find EOM token: "<|eom_id|>"
            if (vocab.special_eom_id == LLAMA_TOKEN_NULL) {
                if (false
                        || t.first == "<|eom_id|>"
                        ) {
                    vocab.special_eom_id = t.second;
                    if ((vocab.id_to_token[t.second].attr & LLAMA_TOKEN_ATTR_CONTROL) == 0) {
                        LLAMA_LOG_WARN("%s: control-looking token: %6d '%s' was not control-type; this is probably a bug in the model. its type will be overridden\n",
                                __func__, t.second, t.first.c_str());
                        vocab.id_to_token[t.second].attr = LLAMA_TOKEN_ATTR_CONTROL;
                    }
                }
            }

            // find FIM_PRE token: "<|fim_prefix|>", "<fim-prefix>", "<PRE>", etc.
            if (vocab.special_fim_pre_id == LLAMA_TOKEN_NULL) {
                if (false
                        || t.first == "<|fim_prefix|>"  // Qwen
                        || t.first == "<fim-prefix>"
                        || t.first == "<｜fim▁begin｜>" // DeepSeek
                        || t.first == "<PRE>"
                        ) {
                    vocab.special_fim_pre_id = t.second;
                    if ((vocab.id_to_token[t.second].attr & LLAMA_TOKEN_ATTR_CONTROL) == 0) {
                        LLAMA_LOG_WARN("%s: control-looking token: %6d '%s' was not control-type; this is probably a bug in the model. its type will be overridden\n",
                                __func__, t.second, t.first.c_str());
                        vocab.id_to_token[t.second].attr = LLAMA_TOKEN_ATTR_CONTROL;
                    }
                }
            }

            // find FIM_SUF token: "<|fim_suffix|>", "<fim-suffix>", "<SUF>", etc.
            if (vocab.special_fim_suf_id == LLAMA_TOKEN_NULL) {
                if (false
                        || t.first == "<|fim_suffix|>" // Qwen
                        || t.first == "<fim-suffix>"
                        || t.first == "<｜fim▁hole｜>" // DeepSeek
                        || t.first == "<SUF>"
                        ) {
                    vocab.special_fim_suf_id = t.second;
                    if ((vocab.id_to_token[t.second].attr & LLAMA_TOKEN_ATTR_CONTROL) == 0) {
                        LLAMA_LOG_WARN("%s: control-looking token: %6d '%s' was not control-type; this is probably a bug in the model. its type will be overridden\n",
                                __func__, t.second, t.first.c_str());
                        vocab.id_to_token[t.second].attr = LLAMA_TOKEN_ATTR_CONTROL;
                    }
                }
            }

            // find FIM_MID token: "<|fim_middle|>", "<fim-middle>", "<MID>", etc.
            if (vocab.special_fim_mid_id == LLAMA_TOKEN_NULL) {
                if (false
                        || t.first == "<|fim_middle|>" // Qwen
                        || t.first == "<fim-middle>"
                        || t.first == "<｜fim▁end｜>"  // DeepSeek
                        || t.first == "<MID>"
                        ) {
                    vocab.special_fim_mid_id = t.second;
                    if ((vocab.id_to_token[t.second].attr & LLAMA_TOKEN_ATTR_CONTROL) == 0) {
                        LLAMA_LOG_WARN("%s: control-looking token: %6d '%s' was not control-type; this is probably a bug in the model. its type will be overridden\n",
                                __func__, t.second, t.first.c_str());
                        vocab.id_to_token[t.second].attr = LLAMA_TOKEN_ATTR_CONTROL;
                    }
                }
            }

            // find FIM_PAD token: "<|fim_pad|>", "<fim-pad>", "<PAD>", etc.
            if (vocab.special_fim_pad_id == LLAMA_TOKEN_NULL) {
                if (false
                        || t.first == "<|fim_pad|>" // Qwen
                        || t.first == "<fim-pad>"
                        || t.first == "<PAD>"
                        ) {
                    vocab.special_fim_pad_id = t.second;
                    if ((vocab.id_to_token[t.second].attr & LLAMA_TOKEN_ATTR_CONTROL) == 0) {
                        LLAMA_LOG_WARN("%s: control-looking token: %6d '%s' was not control-type; this is probably a bug in the model. its type will be overridden\n",
                                __func__, t.second, t.first.c_str());
                        vocab.id_to_token[t.second].attr = LLAMA_TOKEN_ATTR_CONTROL;
                    }
                }
            }

            // find FIM_REP token: "<|fim_repo|>", "<fim-repo>", "<REP>", etc.
            if (vocab.special_fim_rep_id == LLAMA_TOKEN_NULL) {
                if (false
                        || t.first == "<|fim_repo|>"  // Qwen
                        || t.first == "<|repo_name|>"
                        || t.first == "<fim-repo>"
                        || t.first == "<REPO>"
                        ) {
                    vocab.special_fim_rep_id = t.second;
                    if ((vocab.id_to_token[t.second].attr & LLAMA_TOKEN_ATTR_CONTROL) == 0) {
                        LLAMA_LOG_WARN("%s: control-looking token: %6d '%s' was not control-type; this is probably a bug in the model. its type will be overridden\n",
                                __func__, t.second, t.first.c_str());
                        vocab.id_to_token[t.second].attr = LLAMA_TOKEN_ATTR_CONTROL;
                    }
                }
            }

            // find FIM_SEP token: "<|file_sep|>"
            if (vocab.special_fim_sep_id == LLAMA_TOKEN_NULL) {
                if (false
                        || t.first == "<|file_sep|>" // Qwen
                        ) {
                    vocab.special_fim_sep_id = t.second;
                    if ((vocab.id_to_token[t.second].attr & LLAMA_TOKEN_ATTR_CONTROL) == 0) {
                        LLAMA_LOG_WARN("%s: control-looking token: %6d '%s' was not control-type; this is probably a bug in the model. its type will be overridden\n",
                                __func__, t.second, t.first.c_str());
                        vocab.id_to_token[t.second].attr = LLAMA_TOKEN_ATTR_CONTROL;
                    }
                }
            }
        }

        // maintain a list of tokens that cause end-of-generation
        // this is currently determined based on the token text, which is obviously not ideal
        // ref: https://github.com/ggerganov/llama.cpp/issues/9606
        vocab.special_eog_ids.clear();

        if (vocab.special_fim_pad_id != LLAMA_TOKEN_NULL && vocab.special_eog_ids.count(vocab.special_fim_pad_id) == 0) {
            vocab.special_eog_ids.insert(vocab.special_fim_pad_id);
        }

        if (vocab.special_fim_rep_id != LLAMA_TOKEN_NULL && vocab.special_eog_ids.count(vocab.special_fim_rep_id) == 0) {
            vocab.special_eog_ids.insert(vocab.special_fim_rep_id);
        }

        if (vocab.special_fim_sep_id != LLAMA_TOKEN_NULL && vocab.special_eog_ids.count(vocab.special_fim_sep_id) == 0) {
            vocab.special_eog_ids.insert(vocab.special_fim_sep_id);
        }

        for (const auto & t : vocab.token_to_id) {
            if (false
                    || t.first == "<|eot_id|>"
                    || t.first == "<|im_end|>"
                    || t.first == "<|end|>"
                    || t.first == "<end_of_turn>"
                    || t.first == "<|endoftext|>"
                    || t.first == "<|eom_id|>"
                    || t.first == "<EOT>"
               ) {
                vocab.special_eog_ids.insert(t.second);
                if ((vocab.id_to_token[t.second].attr & LLAMA_TOKEN_ATTR_CONTROL) == 0) {
                    LLAMA_LOG_WARN("%s: control-looking token: %6d '%s' was not control-type; this is probably a bug in the model. its type will be overridden\n",
                            __func__, t.second, t.first.c_str());
                    vocab.id_to_token[t.second].attr = LLAMA_TOKEN_ATTR_CONTROL;
                }
            } else {
                // token is control, but not marked as EOG -> print a debug log
                if (vocab.id_to_token[t.second].attr & LLAMA_TOKEN_ATTR_CONTROL && vocab.special_eog_ids.count(t.second) == 0) {
                    LLAMA_LOG_DEBUG("%s: control token: %6d '%s' is not marked as EOG\n",
                            __func__, t.second, t.first.c_str());
                }
            }
        }

        // sanity checks
        if (vocab.special_eos_id != LLAMA_TOKEN_NULL && vocab.special_eog_ids.count(vocab.special_eos_id) == 0) {
            vocab.special_eog_ids.insert(vocab.special_eos_id);
            LLAMA_LOG_WARN("%s: special_eos_id is not in special_eog_ids - the tokenizer config may be incorrect\n", __func__);
        }

        if (vocab.special_eot_id != LLAMA_TOKEN_NULL && vocab.special_eog_ids.count(vocab.special_eot_id) == 0) {
            vocab.special_eog_ids.insert(vocab.special_eot_id);
            LLAMA_LOG_WARN("%s: special_eot_id is not in special_eog_ids - the tokenizer config may be incorrect\n", __func__);
        }

        if (vocab.special_eom_id != LLAMA_TOKEN_NULL && vocab.special_eog_ids.count(vocab.special_eom_id) == 0) {
            vocab.special_eog_ids.insert(vocab.special_eom_id);
            LLAMA_LOG_WARN("%s: special_eom_id is not in special_eog_ids - the tokenizer config may be incorrect\n", __func__);
        }
    }

    // build special tokens cache
    {
        for (llama_vocab::id id = 0; id < (llama_vocab::id)n_vocab; ++id) {
            if (vocab.id_to_token[id].attr & (LLAMA_TOKEN_ATTR_CONTROL | LLAMA_TOKEN_ATTR_USER_DEFINED | LLAMA_TOKEN_ATTR_UNKNOWN)) {
                vocab.cache_special_tokens.push_back(id);
            }
        }

        std::sort(vocab.cache_special_tokens.begin(), vocab.cache_special_tokens.end(),
            [&] (const llama_vocab::id a, const llama_vocab::id b) {
                return vocab.id_to_token[a].text.size() > vocab.id_to_token[b].text.size();
            }
        );

        LLAMA_LOG_INFO("%s: special tokens cache size = %u\n", __func__, (uint32_t)vocab.cache_special_tokens.size());
    }

    // build token to piece cache
    {
        size_t size_cache = 0;

        std::vector<llama_vocab::token> cache_token_to_piece(n_vocab);

        for (uint32_t id = 0; id < n_vocab; ++id) {
            cache_token_to_piece[id] = llama_token_to_piece(&model, id, true);

            size_cache += cache_token_to_piece[id].size();
        }

        std::swap(vocab.cache_token_to_piece, cache_token_to_piece);

        LLAMA_LOG_INFO("%s: token to piece cache size = %.4f MB\n", __func__, size_cache / 1024.0 / 1024.0);
    }

    // Handle per token attributes
    //NOTE: Each model customizes per token attributes.
    //NOTE: Per token attributes are missing from the GGUF file.
    //TODO: Extract attributes from GGUF file.
    {
        auto _contains_any = [] (const std::string &str, const std::vector<std::string> &substrs) -> bool {
            for (auto substr : substrs) {
                if (str.find(substr) < std::string::npos) {
                    return true;
                }
            }
            return false;
        };

        auto _set_tokenid_attr = [&] (const llama_vocab::id id, llama_token_attr attr, bool value) {
            uint32_t current = vocab.id_to_token.at(id).attr;
            current = value ? (current | attr) : (current & ~attr);
            vocab.id_to_token[id].attr = (llama_token_attr) current;
        };

        auto _set_token_attr = [&] (const std::string & token, llama_token_attr attr, bool value) {
            _set_tokenid_attr(vocab.token_to_id.at(token), attr, value);
        };

        std::string model_name;
        std::string tokenizer_pre;

        ml.get_key(LLM_KV_GENERAL_NAME, model_name, false);
        ml.get_key(LLM_KV_TOKENIZER_PRE, tokenizer_pre, false);

        // model name to lowercase
        std::transform(model_name.begin(), model_name.end(), model_name.begin(),
            [] (const std::string::value_type x) {
                return std::tolower(x);
            }
        );

        // set attributes by model/tokenizer name
        if (_contains_any(tokenizer_pre, {"jina-v2-de", "jina-v2-es", "jina-v2-code"})) {
            _set_token_attr("<mask>", LLAMA_TOKEN_ATTR_LSTRIP, true);
        } else if (_contains_any(model_name, {"phi-3", "phi3"})) {
            for (auto id : vocab.cache_special_tokens) {
                _set_tokenid_attr(id, LLAMA_TOKEN_ATTR_RSTRIP, true);
            }
            for (auto token : {"</s>"}) {
                _set_token_attr(token, LLAMA_TOKEN_ATTR_RSTRIP, true);
            }
            for (auto token : {"<unk>", "<s>", "<|endoftext|>"}) {
                _set_token_attr(token, LLAMA_TOKEN_ATTR_RSTRIP, false);
            }
        }
    }
}

static void llm_load_print_meta(llama_model_loader & ml, llama_model & model) {
    const auto & hparams = model.hparams;
    const auto & vocab   = model.vocab;

    const char * rope_scaling_type = LLAMA_ROPE_SCALING_TYPES.at(hparams.rope_scaling_type_train);

    auto print_f = [](const std::function<uint32_t(uint32_t)> & f, uint32_t n) {
        bool is_var = false;

        std::vector<uint32_t> v;
        for (uint32_t i = 0; i < n; ++i) {
            v.push_back(f(i));
            if (v[i] != v[0]) {
                is_var = true;
            }
        }

        std::stringstream ss;

        if (is_var) {
            ss << "[";
            for (uint32_t i = 0; i < n; ++i) {
                ss << v[i];
                if (i < n - 1) {
                    ss << ", ";
                }
            }
            ss << "]";
        } else {
            ss << v[0];
        }

        return ss.str();
    };

    // hparams
    LLAMA_LOG_INFO("%s: format           = %s\n",     __func__, llama_file_version_name(ml.fver));
    LLAMA_LOG_INFO("%s: arch             = %s\n",     __func__, LLM_ARCH_NAMES.at(model.arch));
    LLAMA_LOG_INFO("%s: vocab type       = %s\n",     __func__, llama_model_vocab_type_name(vocab.type));
    LLAMA_LOG_INFO("%s: n_vocab          = %u\n",     __func__, hparams.n_vocab);
    LLAMA_LOG_INFO("%s: n_merges         = %u\n",     __func__, (int) vocab.bpe_ranks.size());
    LLAMA_LOG_INFO("%s: vocab_only       = %d\n",     __func__, hparams.vocab_only);

    if (!hparams.vocab_only) {
        LLAMA_LOG_INFO("%s: n_ctx_train      = %u\n",     __func__, hparams.n_ctx_train);
        LLAMA_LOG_INFO("%s: n_embd           = %u\n",     __func__, hparams.n_embd);
        LLAMA_LOG_INFO("%s: n_layer          = %u\n",     __func__, hparams.n_layer);
        LLAMA_LOG_INFO("%s: n_head           = %s\n",     __func__, print_f([&](uint32_t il) { return hparams.n_head(il);    }, hparams.n_layer).c_str());
        LLAMA_LOG_INFO("%s: n_head_kv        = %s\n",     __func__, print_f([&](uint32_t il) { return hparams.n_head_kv(il); }, hparams.n_layer).c_str());
        LLAMA_LOG_INFO("%s: n_rot            = %u\n",     __func__, hparams.n_rot);
        LLAMA_LOG_INFO("%s: n_swa            = %u\n",     __func__, hparams.n_swa);
        LLAMA_LOG_INFO("%s: n_embd_head_k    = %u\n",     __func__, hparams.n_embd_head_k);
        LLAMA_LOG_INFO("%s: n_embd_head_v    = %u\n",     __func__, hparams.n_embd_head_v);
        LLAMA_LOG_INFO("%s: n_gqa            = %s\n",     __func__, print_f([&](uint32_t il) { return hparams.n_gqa(il);        }, hparams.n_layer).c_str());
        LLAMA_LOG_INFO("%s: n_embd_k_gqa     = %s\n",     __func__, print_f([&](uint32_t il) { return hparams.n_embd_k_gqa(il); }, hparams.n_layer).c_str());
        LLAMA_LOG_INFO("%s: n_embd_v_gqa     = %s\n",     __func__, print_f([&](uint32_t il) { return hparams.n_embd_v_gqa(il); }, hparams.n_layer).c_str());
        LLAMA_LOG_INFO("%s: f_norm_eps       = %.1e\n",   __func__, hparams.f_norm_eps);
        LLAMA_LOG_INFO("%s: f_norm_rms_eps   = %.1e\n",   __func__, hparams.f_norm_rms_eps);
        LLAMA_LOG_INFO("%s: f_clamp_kqv      = %.1e\n",   __func__, hparams.f_clamp_kqv);
        LLAMA_LOG_INFO("%s: f_max_alibi_bias = %.1e\n",   __func__, hparams.f_max_alibi_bias);
        LLAMA_LOG_INFO("%s: f_logit_scale    = %.1e\n",   __func__, hparams.f_logit_scale);
        LLAMA_LOG_INFO("%s: n_ff             = %s\n",     __func__, print_f([&](uint32_t il) { return hparams.n_ff(il); }, hparams.n_layer).c_str());
        LLAMA_LOG_INFO("%s: n_expert         = %u\n",     __func__, hparams.n_expert);
        LLAMA_LOG_INFO("%s: n_expert_used    = %u\n",     __func__, hparams.n_expert_used);
        LLAMA_LOG_INFO("%s: causal attn      = %d\n",     __func__, hparams.causal_attn);
        LLAMA_LOG_INFO("%s: pooling type     = %d\n",     __func__, hparams.pooling_type);
        LLAMA_LOG_INFO("%s: rope type        = %d\n",     __func__, hparams.rope_type);
        LLAMA_LOG_INFO("%s: rope scaling     = %s\n",     __func__, rope_scaling_type);
        LLAMA_LOG_INFO("%s: freq_base_train  = %.1f\n",   __func__, hparams.rope_freq_base_train);
        LLAMA_LOG_INFO("%s: freq_scale_train = %g\n",     __func__, hparams.rope_freq_scale_train);
        LLAMA_LOG_INFO("%s: n_ctx_orig_yarn  = %u\n",     __func__, hparams.n_ctx_orig_yarn);
        LLAMA_LOG_INFO("%s: rope_finetuned   = %s\n",     __func__, hparams.rope_finetuned ? "yes" : "unknown");
        LLAMA_LOG_INFO("%s: ssm_d_conv       = %u\n",     __func__, hparams.ssm_d_conv);
        LLAMA_LOG_INFO("%s: ssm_d_inner      = %u\n",     __func__, hparams.ssm_d_inner);
        LLAMA_LOG_INFO("%s: ssm_d_state      = %u\n",     __func__, hparams.ssm_d_state);
        LLAMA_LOG_INFO("%s: ssm_dt_rank      = %u\n",     __func__, hparams.ssm_dt_rank);
        LLAMA_LOG_INFO("%s: ssm_dt_b_c_rms   = %d\n",     __func__, hparams.ssm_dt_b_c_rms);
    }

    LLAMA_LOG_INFO("%s: model type       = %s\n",     __func__, llama_model_type_name(model.type));
    LLAMA_LOG_INFO("%s: model ftype      = %s\n",     __func__, llama_model_ftype_name(model.ftype).c_str());
    if (ml.n_elements >= 1e12) {
        LLAMA_LOG_INFO("%s: model params     = %.2f T\n", __func__, ml.n_elements*1e-12);
    } else if (ml.n_elements >= 1e9) {
        LLAMA_LOG_INFO("%s: model params     = %.2f B\n", __func__, ml.n_elements*1e-9);
    } else if (ml.n_elements >= 1e6) {
        LLAMA_LOG_INFO("%s: model params     = %.2f M\n", __func__, ml.n_elements*1e-6);
    } else {
        LLAMA_LOG_INFO("%s: model params     = %.2f K\n", __func__, ml.n_elements*1e-3);
    }
    if (ml.n_bytes < GiB) {
        LLAMA_LOG_INFO("%s: model size       = %.2f MiB (%.2f BPW) \n", __func__, ml.n_bytes/1024.0/1024.0,        ml.n_bytes*8.0/ml.n_elements);
    } else {
        LLAMA_LOG_INFO("%s: model size       = %.2f GiB (%.2f BPW) \n", __func__, ml.n_bytes/1024.0/1024.0/1024.0, ml.n_bytes*8.0/ml.n_elements);
    }

    // general kv
    LLAMA_LOG_INFO("%s: general.name     = %s\n",    __func__, model.name.c_str());

    // special tokens
    if (vocab.special_bos_id  != -1)    { LLAMA_LOG_INFO( "%s: BOS token        = %d '%s'\n", __func__, vocab.special_bos_id,     vocab.id_to_token[vocab.special_bos_id].text.c_str() );  }
    if (vocab.special_eos_id  != -1)    { LLAMA_LOG_INFO( "%s: EOS token        = %d '%s'\n", __func__, vocab.special_eos_id,     vocab.id_to_token[vocab.special_eos_id].text.c_str() );  }
    if (vocab.special_eot_id  != -1)    { LLAMA_LOG_INFO( "%s: EOT token        = %d '%s'\n", __func__, vocab.special_eot_id,     vocab.id_to_token[vocab.special_eot_id].text.c_str() );  }
    if (vocab.special_eom_id  != -1)    { LLAMA_LOG_INFO( "%s: EOM token        = %d '%s'\n", __func__, vocab.special_eom_id,     vocab.id_to_token[vocab.special_eom_id].text.c_str() );  }
    if (vocab.special_unk_id  != -1)    { LLAMA_LOG_INFO( "%s: UNK token        = %d '%s'\n", __func__, vocab.special_unk_id,     vocab.id_to_token[vocab.special_unk_id].text.c_str() );  }
    if (vocab.special_sep_id  != -1)    { LLAMA_LOG_INFO( "%s: SEP token        = %d '%s'\n", __func__, vocab.special_sep_id,     vocab.id_to_token[vocab.special_sep_id].text.c_str() );  }
    if (vocab.special_pad_id  != -1)    { LLAMA_LOG_INFO( "%s: PAD token        = %d '%s'\n", __func__, vocab.special_pad_id,     vocab.id_to_token[vocab.special_pad_id].text.c_str() );  }
    if (vocab.special_cls_id  != -1)    { LLAMA_LOG_INFO( "%s: CLS token        = %d '%s'\n", __func__, vocab.special_cls_id,     vocab.id_to_token[vocab.special_cls_id].text.c_str() );  }
    if (vocab.special_mask_id != -1)    { LLAMA_LOG_INFO( "%s: MASK token       = %d '%s'\n", __func__, vocab.special_mask_id,    vocab.id_to_token[vocab.special_mask_id].text.c_str() ); }

    if (vocab.linefeed_id != -1)        { LLAMA_LOG_INFO( "%s: LF token         = %d '%s'\n", __func__, vocab.linefeed_id,        vocab.id_to_token[vocab.linefeed_id].text.c_str() ); }

    if (vocab.special_fim_pre_id != -1) { LLAMA_LOG_INFO( "%s: FIM PRE token    = %d '%s'\n", __func__, vocab.special_fim_pre_id, vocab.id_to_token[vocab.special_fim_pre_id].text.c_str() ); }
    if (vocab.special_fim_suf_id != -1) { LLAMA_LOG_INFO( "%s: FIM SUF token    = %d '%s'\n", __func__, vocab.special_fim_suf_id, vocab.id_to_token[vocab.special_fim_suf_id].text.c_str() ); }
    if (vocab.special_fim_mid_id != -1) { LLAMA_LOG_INFO( "%s: FIM MID token    = %d '%s'\n", __func__, vocab.special_fim_mid_id, vocab.id_to_token[vocab.special_fim_mid_id].text.c_str() ); }
    if (vocab.special_fim_pad_id != -1) { LLAMA_LOG_INFO( "%s: FIM PAD token    = %d '%s'\n", __func__, vocab.special_fim_pad_id, vocab.id_to_token[vocab.special_fim_pad_id].text.c_str() ); }
    if (vocab.special_fim_rep_id != -1) { LLAMA_LOG_INFO( "%s: FIM REP token    = %d '%s'\n", __func__, vocab.special_fim_rep_id, vocab.id_to_token[vocab.special_fim_rep_id].text.c_str() ); }
    if (vocab.special_fim_sep_id != -1) { LLAMA_LOG_INFO( "%s: FIM SEP token    = %d '%s'\n", __func__, vocab.special_fim_sep_id, vocab.id_to_token[vocab.special_fim_sep_id].text.c_str() ); }

    for (const auto & id : vocab.special_eog_ids) {
        LLAMA_LOG_INFO( "%s: EOG token        = %d '%s'\n", __func__, id, vocab.id_to_token[id].text.c_str() );
    }

    LLAMA_LOG_INFO("%s: max token length = %d\n", __func__, vocab.max_token_len);

    if (model.arch == LLM_ARCH_DEEPSEEK2) {
        LLAMA_LOG_INFO("%s: n_layer_dense_lead   = %d\n",     __func__, hparams.n_layer_dense_lead);
        LLAMA_LOG_INFO("%s: n_lora_q             = %d\n",     __func__, hparams.n_lora_q);
        LLAMA_LOG_INFO("%s: n_lora_kv            = %d\n",     __func__, hparams.n_lora_kv);
        LLAMA_LOG_INFO("%s: n_ff_exp             = %d\n",     __func__, hparams.n_ff_exp);
        LLAMA_LOG_INFO("%s: n_expert_shared      = %d\n",     __func__, hparams.n_expert_shared);
        LLAMA_LOG_INFO("%s: expert_weights_scale = %.1f\n",   __func__, hparams.expert_weights_scale);
        LLAMA_LOG_INFO("%s: rope_yarn_log_mul    = %.4f\n",   __func__, hparams.rope_yarn_log_mul);
    }

    if (model.arch == LLM_ARCH_QWEN2MOE) {
        LLAMA_LOG_INFO("%s: n_ff_exp         = %d\n",     __func__, hparams.n_ff_exp);
        LLAMA_LOG_INFO("%s: n_ff_shexp       = %d\n",     __func__, hparams.n_ff_shexp);
    }

    if (model.arch == LLM_ARCH_GRANITE || model.arch == LLM_ARCH_GRANITE_MOE) {
        LLAMA_LOG_INFO("%s: f_embedding_scale = %f\n", __func__, hparams.f_embedding_scale);
        LLAMA_LOG_INFO("%s: f_residual_scale  = %f\n", __func__, hparams.f_residual_scale);
        LLAMA_LOG_INFO("%s: f_attention_scale = %f\n", __func__, hparams.f_attention_scale);
    }
}

enum llm_tensor_layer {
    LLM_TENSOR_LAYER_INPUT,
    LLM_TENSOR_LAYER_REPEATING,
    LLM_TENSOR_LAYER_OUTPUT,
};

struct llm_tensor_info {
    llm_tensor_layer layer;
    lm_ggml_op op;
};

static const std::map<llm_tensor, llm_tensor_info> llm_tensor_info_mapping = {
    {LLM_TENSOR_TOKEN_EMBD,                 {LLM_TENSOR_LAYER_INPUT, LM_GGML_OP_GET_ROWS}},
    {LLM_TENSOR_POS_EMBD,                   {LLM_TENSOR_LAYER_INPUT, LM_GGML_OP_GET_ROWS}},
    {LLM_TENSOR_TOKEN_EMBD_NORM,            {LLM_TENSOR_LAYER_INPUT, LM_GGML_OP_GET_ROWS}},
    {LLM_TENSOR_TOKEN_TYPES,                {LLM_TENSOR_LAYER_INPUT, LM_GGML_OP_GET_ROWS}},
    {LLM_TENSOR_OUTPUT,                     {LLM_TENSOR_LAYER_OUTPUT, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_CLS,                        {LLM_TENSOR_LAYER_OUTPUT, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_CLS_OUT,                    {LLM_TENSOR_LAYER_OUTPUT, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_OUTPUT_NORM,                {LLM_TENSOR_LAYER_OUTPUT, LM_GGML_OP_MUL}},
    {LLM_TENSOR_DEC_OUTPUT_NORM,            {LLM_TENSOR_LAYER_OUTPUT, LM_GGML_OP_MUL}},
    {LLM_TENSOR_ENC_OUTPUT_NORM,            {LLM_TENSOR_LAYER_OUTPUT, LM_GGML_OP_MUL}},
    {LLM_TENSOR_ROPE_FREQS,                 {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_ROPE}},
    {LLM_TENSOR_ROPE_FACTORS_LONG,          {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_ROPE}},
    {LLM_TENSOR_ROPE_FACTORS_SHORT,         {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_ROPE}},
    {LLM_TENSOR_ATTN_Q,                     {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_ATTN_K,                     {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_ATTN_V,                     {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_ATTN_QKV,                   {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_ATTN_OUT,                   {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_FFN_GATE,                   {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_FFN_DOWN,                   {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_FFN_UP,                     {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_FFN_DOWN_SHEXP,             {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_FFN_GATE_SHEXP,             {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_FFN_UP_SHEXP,               {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_ATTN_Q_A,                   {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_ATTN_Q_B,                   {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_ATTN_KV_A_MQA,              {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_ATTN_KV_B,                  {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_DEC_ATTN_Q,                 {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_DEC_ATTN_K,                 {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_ATTN_Q,                     {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_ATTN_K,                     {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_ATTN_V,                     {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_ATTN_QKV,                   {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_ATTN_OUT,                   {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_FFN_GATE,                   {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_FFN_DOWN,                   {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_FFN_UP,                     {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_FFN_DOWN_SHEXP,             {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_FFN_GATE_SHEXP,             {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_FFN_UP_SHEXP,               {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_ATTN_Q_A,                   {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_ATTN_Q_B,                   {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_ATTN_KV_A_MQA,              {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_ATTN_KV_B,                  {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_DEC_ATTN_Q,                 {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_DEC_ATTN_K,                 {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_DEC_ATTN_V,                 {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_DEC_ATTN_OUT,               {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_DEC_CROSS_ATTN_Q,           {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_DEC_CROSS_ATTN_K,           {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_DEC_CROSS_ATTN_V,           {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_DEC_CROSS_ATTN_OUT,         {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_DEC_FFN_GATE,               {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_DEC_FFN_DOWN,               {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_DEC_FFN_UP,                 {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_ENC_ATTN_Q,                 {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_ENC_ATTN_K,                 {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_ENC_ATTN_V,                 {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_ENC_ATTN_OUT,               {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_ENC_FFN_GATE,               {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_ENC_FFN_DOWN,               {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_ENC_FFN_UP,                 {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_FFN_GATE_INP_SHEXP,         {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_FFN_GATE_INP,               {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_SSM_IN,                     {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_SSM_X,                      {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_SSM_DT,                     {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_SSM_OUT,                    {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_TIME_MIX_W1,                {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_TIME_MIX_W2,                {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_TIME_MIX_DECAY_W1,          {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_TIME_MIX_DECAY_W2,          {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_TIME_MIX_KEY,               {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_TIME_MIX_VALUE,             {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_TIME_MIX_RECEPTANCE,        {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_TIME_MIX_GATE,              {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_TIME_MIX_OUTPUT,            {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_CHANNEL_MIX_KEY,            {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_CHANNEL_MIX_RECEPTANCE,     {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_CHANNEL_MIX_VALUE,          {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT}},
    {LLM_TENSOR_FFN_ACT,                    {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_DIV}},
    {LLM_TENSOR_SSM_CONV1D,                 {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_SSM_CONV}},
    {LLM_TENSOR_SSM_A,                      {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_SSM_SCAN}},
    {LLM_TENSOR_SSM_D,                      {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL}},
    {LLM_TENSOR_TIME_MIX_LERP_X,            {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL}},
    {LLM_TENSOR_TIME_MIX_LN,                {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL}},
    {LLM_TENSOR_CHANNEL_MIX_LERP_K,         {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL}},
    {LLM_TENSOR_CHANNEL_MIX_LERP_R,         {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL}},
    {LLM_TENSOR_TIME_MIX_LERP_W,            {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_ADD}},
    {LLM_TENSOR_TIME_MIX_LERP_K,            {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_ADD}},
    {LLM_TENSOR_TIME_MIX_LERP_V,            {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_ADD}},
    {LLM_TENSOR_TIME_MIX_LERP_R,            {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_ADD}},
    {LLM_TENSOR_TIME_MIX_LERP_G,            {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_ADD}},
    {LLM_TENSOR_TIME_MIX_DECAY,             {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_ADD}},
    {LLM_TENSOR_TIME_MIX_FIRST,             {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_RWKV_WKV6}},
    {LLM_TENSOR_ATTN_NORM,                  {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL}},
    {LLM_TENSOR_ATTN_NORM_2,                {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL}},
    {LLM_TENSOR_ATTN_OUT_NORM,              {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL}},
    {LLM_TENSOR_ATTN_POST_NORM,             {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL}},
    {LLM_TENSOR_FFN_NORM,                   {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL}},
    {LLM_TENSOR_FFN_POST_NORM,              {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL}},
    {LLM_TENSOR_FFN_NORM_EXPS,              {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL}},
    {LLM_TENSOR_ATTN_Q_NORM,                {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL}},
    {LLM_TENSOR_ATTN_K_NORM,                {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL}},
    {LLM_TENSOR_LAYER_OUT_NORM,             {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL}},
    {LLM_TENSOR_ATTN_Q_A_NORM,              {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL}},
    {LLM_TENSOR_ATTN_KV_A_NORM,             {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL}},
    {LLM_TENSOR_ATTN_SUB_NORM,              {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL}},
    {LLM_TENSOR_FFN_SUB_NORM,               {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL}},
    {LLM_TENSOR_DEC_ATTN_NORM,              {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL}},
    {LLM_TENSOR_DEC_CROSS_ATTN_NORM,        {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL}},
    {LLM_TENSOR_DEC_FFN_NORM,               {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL}},
    {LLM_TENSOR_ENC_ATTN_NORM,              {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL}},
    {LLM_TENSOR_ENC_FFN_NORM,               {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL}},
    {LLM_TENSOR_DEC_ATTN_REL_B,             {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_GET_ROWS}},
    {LLM_TENSOR_ENC_ATTN_REL_B,             {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_GET_ROWS}},
    {LLM_TENSOR_FFN_DOWN_EXPS,              {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT_ID}},
    {LLM_TENSOR_FFN_GATE_EXPS,              {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT_ID}},
    {LLM_TENSOR_FFN_UP_EXPS,                {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_MUL_MAT_ID}},
    // this tensor is loaded for T5, but never used
    {LLM_TENSOR_DEC_CROSS_ATTN_REL_B,       {LLM_TENSOR_LAYER_REPEATING, LM_GGML_OP_NONE}},
};

// checks if the weight tensor can be used with the specified buffer type and device
static bool weight_buft_supported(const llama_hparams & hparams, lm_ggml_tensor * w, lm_ggml_op op, lm_ggml_backend_buffer_type_t buft, lm_ggml_backend_dev_t dev) {
    LM_GGML_ASSERT(w != nullptr);

    if (op == LM_GGML_OP_NONE) {
        return true;
    }

    lm_ggml_init_params params = {
        /*.mem_size   =*/ lm_ggml_tensor_overhead()*8,
        /*.mem_buffer =*/ NULL,
        /*.no_alloc   =*/ true,
    };
    lm_ggml_context_ptr ctx_ptr { lm_ggml_init(params) };
    if (!ctx_ptr) {
        throw std::runtime_error(format("failed to create ggml context"));
    }
    lm_ggml_context * ctx = ctx_ptr.get();

    lm_ggml_tensor * op_tensor = nullptr;

    switch (op) {
        case LM_GGML_OP_GET_ROWS:
            {
                lm_ggml_tensor * b = lm_ggml_new_tensor_1d(ctx, LM_GGML_TYPE_I32, 512);
                op_tensor = lm_ggml_get_rows(ctx, w, b);
            } break;
        case LM_GGML_OP_MUL_MAT:
            {
                lm_ggml_tensor * b = lm_ggml_new_tensor_4d(ctx, LM_GGML_TYPE_F32, w->ne[0], 512, w->ne[2], w->ne[3]);
                op_tensor = lm_ggml_mul_mat(ctx, w, b);
            } break;
        case LM_GGML_OP_MUL_MAT_ID:
            {
                int n_expert_used = hparams.n_expert_used;
                lm_ggml_tensor * b = lm_ggml_new_tensor_3d(ctx, LM_GGML_TYPE_F32, w->ne[0], n_expert_used, 512);
                lm_ggml_tensor * ids = lm_ggml_new_tensor_2d(ctx, LM_GGML_TYPE_I32, n_expert_used, 512);
                op_tensor = lm_ggml_mul_mat_id(ctx, w, b, ids);
            } break;
        case LM_GGML_OP_ADD:
            {
                lm_ggml_tensor * a = lm_ggml_new_tensor_2d(ctx, LM_GGML_TYPE_F32, w->ne[0], 512);
                op_tensor = lm_ggml_add(ctx, a, w);
            } break;
        case LM_GGML_OP_MUL:
            {
                lm_ggml_tensor * a = lm_ggml_new_tensor_2d(ctx, LM_GGML_TYPE_F32, w->ne[0], 512);
                op_tensor = lm_ggml_mul(ctx, a, w);
            } break;
        case LM_GGML_OP_DIV:
            {
                lm_ggml_tensor * a = lm_ggml_new_tensor_1d(ctx, LM_GGML_TYPE_F32, w->ne[0]);
                op_tensor = lm_ggml_div(ctx, a, w);
            } break;
        case LM_GGML_OP_ROPE:
            {
                int n_embd_head = hparams.n_embd_head_v;
                int n_head = hparams.n_head();
                lm_ggml_tensor * a = lm_ggml_new_tensor_3d(ctx, LM_GGML_TYPE_F32, n_embd_head, n_head, 512);
                lm_ggml_tensor * b = lm_ggml_new_tensor_1d(ctx, LM_GGML_TYPE_I32, 512);
                op_tensor = lm_ggml_rope_ext(
                    ctx, a, b, w,
                    0, 0, 0, 0, 0,
                    0, 0, 0, 0
                );

            } break;
        case LM_GGML_OP_SSM_CONV:
            {
                // FIXME
                lm_ggml_tensor * conv_x = lm_ggml_new_tensor_3d(ctx, LM_GGML_TYPE_F32, 12345, w->ne[1], 6789);
                op_tensor = lm_ggml_ssm_conv(ctx, conv_x, w);
            } break;
        case LM_GGML_OP_SSM_SCAN:
            {
                // FIXME
                const int64_t d_state      = w->ne[0];
                const int64_t d_inner      = w->ne[1];
                const int64_t n_seq_tokens = 512;
                const int64_t n_seqs       = 1;
                lm_ggml_tensor * s  = lm_ggml_new_tensor_3d(ctx, LM_GGML_TYPE_F32, d_state, d_inner, n_seqs);
                lm_ggml_tensor * x = lm_ggml_new_tensor_3d(ctx, LM_GGML_TYPE_F32, d_inner, n_seq_tokens, n_seqs);
                lm_ggml_tensor * dt = lm_ggml_new_tensor_3d(ctx, LM_GGML_TYPE_F32, d_inner, n_seq_tokens, n_seqs);
                lm_ggml_tensor * B = lm_ggml_new_tensor_3d(ctx, LM_GGML_TYPE_F32, d_state, n_seq_tokens, n_seqs);
                lm_ggml_tensor * C = lm_ggml_new_tensor_3d(ctx, LM_GGML_TYPE_F32, d_state, n_seq_tokens, n_seqs);
                op_tensor = lm_ggml_ssm_scan(ctx, s, x, dt, w, B, C);
            } break;
        case LM_GGML_OP_RWKV_WKV6:
            {
                // FIXME
                const int64_t S = 123;
                const int64_t H = 123;
                const int64_t n_tokens = 123;
                const int64_t n_seqs = 123;
                lm_ggml_tensor  * k = lm_ggml_new_tensor_4d(ctx, LM_GGML_TYPE_F32, S, 1, H, n_tokens);
                lm_ggml_tensor  * v = lm_ggml_new_tensor_4d(ctx, LM_GGML_TYPE_F32, 1, S, H, n_tokens);
                lm_ggml_tensor  * r = lm_ggml_new_tensor_4d(ctx, LM_GGML_TYPE_F32, 1, S, H, n_tokens);
                lm_ggml_tensor  * tf = w;
                lm_ggml_tensor  * td = lm_ggml_new_tensor_4d(ctx, LM_GGML_TYPE_F32, 1, S, H, n_tokens);
                lm_ggml_tensor  * state = lm_ggml_new_tensor_4d(ctx, LM_GGML_TYPE_F32, S, n_seqs, S, H);
                op_tensor = lm_ggml_rwkv_wkv6(ctx, k, v, r, tf, td, state);
            } break;
        default:
            LM_GGML_ABORT("%s: missing test for op %s for tensor %s", __func__, lm_ggml_op_name(op), w->name);
    }

    // create a temporary dummy buffer for the weight so that supports_op can check the buffer type
    LM_GGML_ASSERT(w->buffer == nullptr);
    w->buffer = lm_ggml_backend_buft_alloc_buffer(buft, 0);
    bool op_supported = lm_ggml_backend_dev_supports_op(dev, op_tensor);
    lm_ggml_backend_buffer_free(w->buffer);
    w->buffer = nullptr;

    return op_supported;
}

// find the first buffer type in the list that can use the tensor
static lm_ggml_backend_buffer_type_t select_weight_buft(const llama_model & model, lm_ggml_tensor * tensor, lm_ggml_op op, const llama_model::buft_list_t & buft_list) {
    LM_GGML_ASSERT(!buft_list.empty());
    for (const auto & cur : buft_list) {
        lm_ggml_backend_dev_t cur_dev = cur.first;
        lm_ggml_backend_buffer_type_t cur_buft = cur.second;
        if (weight_buft_supported(model.hparams, tensor, op, cur_buft, cur_dev)) {
            return cur_buft;
        }
    }
    return nullptr;
}

// CPU: ACCEL -> CPU extra -> GPU host -> CPU
static llama_model::buft_list_t make_cpu_buft_list(llama_model & model) {
    llama_model::buft_list_t buft_list;

    // add ACCEL buffer types
    for (size_t i = 0; i < lm_ggml_backend_dev_count(); ++i) {
        lm_ggml_backend_dev_t dev = lm_ggml_backend_dev_get(i);
        if (lm_ggml_backend_dev_type(dev) == LM_GGML_BACKEND_DEVICE_TYPE_ACCEL) {
            auto * buft = lm_ggml_backend_dev_buffer_type(dev);
            // skip
            if (buft != lm_ggml_backend_cpu_buffer_type()) {
                buft_list.emplace_back(dev, buft);
            }
        }
    }

    // add extra buffer types
    auto * cpu_dev = lm_ggml_backend_dev_by_type(LM_GGML_BACKEND_DEVICE_TYPE_CPU);
    auto * cpu_reg = lm_ggml_backend_dev_backend_reg(cpu_dev);
    auto lm_ggml_backend_dev_get_extra_bufts_fn = (lm_ggml_backend_dev_get_extra_bufts_t)
        lm_ggml_backend_reg_get_proc_address(cpu_reg, "lm_ggml_backend_dev_get_extra_bufts");
    if (lm_ggml_backend_dev_get_extra_bufts_fn) {
        lm_ggml_backend_buffer_type_t * extra_bufts = lm_ggml_backend_dev_get_extra_bufts_fn(cpu_dev);
        while (extra_bufts && *extra_bufts) {
            buft_list.emplace_back(cpu_dev, *extra_bufts);
            ++extra_bufts;
        }
    }

    // add a host buffer type
    // storing the tensors in a host buffer is useful when the processing of large batches
    // is offloaded to a GPU device, since it reduces the time spent on data transfers
    // generally, this will be done using the first device in the list
    // a better approach would be to handle this on a weight-by-weight basis using the offload_op
    // function of the device to determine if it would benefit from being stored in a host buffer
    for (auto * dev : model.devices) {
        lm_ggml_backend_buffer_type_t buft = lm_ggml_backend_dev_host_buffer_type(dev);
        if (buft) {
            buft_list.emplace_back(dev, buft);
            break;
        }
    }

    // add the CPU buffer type
    for (size_t i = 0; i < lm_ggml_backend_dev_count(); ++i) {
        lm_ggml_backend_dev_t dev = lm_ggml_backend_dev_get(i);
        if (lm_ggml_backend_dev_type(dev) == LM_GGML_BACKEND_DEVICE_TYPE_CPU) {
            buft_list.emplace_back(dev, lm_ggml_backend_dev_buffer_type(dev));
        }
    }

    return buft_list;
}

// GPU: split if LLAMA_SPLIT_MODE_ROW -> GPU
static llama_model::buft_list_t make_gpu_buft_list(lm_ggml_backend_dev_t dev, enum llama_split_mode split_mode, const float * tensor_split) {
    llama_model::buft_list_t buft_list;

    // add the device split buffer type if requested and available
    if (split_mode == LLAMA_SPLIT_MODE_ROW) {
        lm_ggml_backend_reg_t reg = lm_ggml_backend_dev_backend_reg(dev);
        auto lm_ggml_backend_split_buffer_type_fn = (lm_ggml_backend_split_buffer_type_t)
            lm_ggml_backend_reg_get_proc_address(reg, "lm_ggml_backend_split_buffer_type");
        if (lm_ggml_backend_split_buffer_type_fn) {
            size_t dev_index = [&]() {
                auto * reg = lm_ggml_backend_dev_backend_reg(dev);
                for (size_t i = 0; i < lm_ggml_backend_reg_dev_count(reg); ++i) {
                    if (lm_ggml_backend_reg_dev_get(reg, i) == dev) {
                        return i;
                    }
                }
                throw std::runtime_error(format("device %s not found in its backend reg", lm_ggml_backend_dev_name(dev)));
            }();
            auto * buft = lm_ggml_backend_split_buffer_type_fn(dev_index, tensor_split);
            if (buft != nullptr) {
                buft_list.emplace_back(dev, buft);
            }
        }
    }

    // add the device default buffer type
    buft_list.emplace_back(dev, lm_ggml_backend_dev_buffer_type(dev));

    return buft_list;
}

// Returns false if cancelled by progress_callback
static bool llm_load_tensors(
        llama_model_loader & ml,
        llama_model & model,
        int n_gpu_layers,
        enum llama_split_mode split_mode,
        int main_gpu,
        const float * tensor_split,
        bool use_mlock,
        llama_progress_callback progress_callback,
        void * progress_callback_user_data) {
    auto & hparams = model.hparams;

    model.split_mode   = split_mode;
    model.main_gpu     = main_gpu;
    model.n_gpu_layers = n_gpu_layers;

    const int n_layer     = hparams.n_layer;
    bool use_mmap_buffer = true;

    // build a list of buffer types for the CPU and GPU devices
    model.cpu_buft_list = make_cpu_buft_list(model);
    for (auto * dev : model.devices) {
        llama_model::buft_list_t buft_list = make_gpu_buft_list(dev, split_mode, tensor_split);
        // add CPU buffer types as a fallback
        buft_list.insert(buft_list.end(), model.cpu_buft_list.begin(), model.cpu_buft_list.end());
        model.gpu_buft_list.emplace(dev, std::move(buft_list));
    }

    // calculate the split points
    int device_count = llama_get_device_count(model);
    bool all_zero = tensor_split == nullptr || std::all_of(tensor_split, tensor_split + device_count, [](float x) { return x == 0.0f; });
    std::vector<float> splits(device_count);
    if (all_zero) {
        // default split, by free memory
        for (int i = 0; i < device_count; ++i) {
            lm_ggml_backend_dev_t dev = model.devices[i];
            size_t total;
            size_t free;
            lm_ggml_backend_dev_memory(dev, &free, &total);
            splits[i] = free;
        }
    } else {
        std::copy(tensor_split, tensor_split + device_count, splits.begin());
    }

    // sum and normalize the splits to get the split points
    float split_sum = 0.0f;
    for (int i = 0; i < device_count; ++i) {
        split_sum += splits[i];
        splits[i] = split_sum;
    }
    for (int i = 0; i < device_count; ++i) {
        splits[i] /= split_sum;
    }

    lm_ggml_backend_dev_t cpu_dev = lm_ggml_backend_dev_by_type(LM_GGML_BACKEND_DEVICE_TYPE_CPU);
    const int i_gpu_start = std::max((int) hparams.n_layer - n_gpu_layers, (int) 0);
    const int act_gpu_layers = model.devices.empty() ? 0 : std::min(n_gpu_layers, (int)n_layer + 1);
    auto get_layer_buft_list = [&](int il) -> llama_model::layer_dev {
        if (il < i_gpu_start || (il - i_gpu_start) >= act_gpu_layers) {
            return {cpu_dev, &model.cpu_buft_list};
        }
        int layer_gpu = std::upper_bound(splits.begin(), splits.begin() + device_count, float(il - i_gpu_start)/act_gpu_layers) - splits.begin();
        auto * dev = model.devices.at(layer_gpu);
        return {dev, &model.gpu_buft_list.at(dev)};
    };

    // assign the input layer
    // there is very little benefit to offloading the input layer, so always keep it on the CPU
    model.dev_input = { cpu_dev, &model.cpu_buft_list };

    // assign the repeating layers to the devices according to the splits
    model.dev_layer.resize(n_layer);
    for (int il = 0; il < n_layer; ++il) {
        model.dev_layer[il] = get_layer_buft_list(il);
    }
    // assign the output layer
    model.dev_output = get_layer_buft_list(n_layer);

    // one ggml context per buffer type
    int max_n_tensors = ml.n_tensors;
    max_n_tensors += 1;         // duplicated output tensor
    max_n_tensors += n_layer*2; // duplicated rope freq tensors
    const size_t ctx_size = lm_ggml_tensor_overhead()*max_n_tensors;

    std::map<lm_ggml_backend_buffer_type_t, lm_ggml_context *> ctx_map;
    auto ctx_for_buft = [&](lm_ggml_backend_buffer_type_t buft) -> lm_ggml_context * {
        auto it = ctx_map.find(buft);
        if (it == ctx_map.end()) {
            lm_ggml_init_params params = {
                /*.mem_size   =*/ ctx_size,
                /*.mem_buffer =*/ NULL,
                /*.no_alloc   =*/ true,
            };
            lm_ggml_context * ctx = lm_ggml_init(params);
            if (!ctx) {
                throw std::runtime_error(format("failed to create ggml context"));
            }
            ctx_map[buft] = ctx;
            model.ctxs.emplace_back(ctx);
            return ctx;
        }
        return it->second;
    };

    // create tensors for the weights
    {
        // note: cast to int64_t since we will use these for the tensor dimensions
        const int64_t n_head        = hparams.n_head();
        const int64_t n_head_kv     = hparams.n_head_kv();
        const int64_t n_embd        = hparams.n_embd;
        const int64_t n_embd_k_gqa  = hparams.n_embd_k_gqa();
        const int64_t n_embd_v_gqa  = hparams.n_embd_v_gqa();
        const int64_t n_embd_head_k = hparams.n_embd_head_k;
        const int64_t n_embd_head_v = hparams.n_embd_head_v;
        const int64_t n_ff          = hparams.n_ff();
        const int64_t n_embd_gqa    = n_embd_v_gqa;
        const int64_t n_vocab       = hparams.n_vocab;
        const int64_t n_vocab_type  = hparams.n_vocab_type;
        const int64_t n_rot         = hparams.n_rot;
        const int64_t n_expert      = hparams.n_expert;
        const int64_t n_expert_used = hparams.n_expert_used;
        const int64_t n_ctx_train   = hparams.n_ctx_train;

        if (n_expert > 0 && hparams.n_expert_used == 0) {
            throw std::runtime_error("model has expert layers but no expert layers are used");
        }

        int n_moved_tensors = 0;
        lm_ggml_tensor * first_moved_tensor = nullptr;
        lm_ggml_backend_buffer_type_t first_moved_from_buft = nullptr;
        lm_ggml_backend_buffer_type_t first_moved_to_buft = nullptr;

        auto create_tensor = [&](const LLM_TN_IMPL & tn, const std::initializer_list<int64_t> & ne, int flags) -> lm_ggml_tensor * {
            lm_ggml_tensor * t_meta = ml.get_tensor_meta(tn.str().c_str());

            if (!t_meta) {
                if (flags & llama_model_loader::TENSOR_NOT_REQUIRED) {
                    return nullptr;
                }
                throw std::runtime_error(format("missing tensor '%s'", tn.str().c_str()));
            }

            // some models use the token embedding tensor as the output, but since these are used in different layers and with different ops
            // the tensor is duplicated
            // to handle this, we check if the tensor is duplicated, and if so, we assume that it is being loaded as the output tensor
            llm_tensor tn_tensor = tn.tensor;
            if (tn.tensor == LLM_TENSOR_TOKEN_EMBD && flags & llama_model_loader::TENSOR_DUPLICATED) {
                tn_tensor = LLM_TENSOR_OUTPUT;
            }

            auto it = llm_tensor_info_mapping.find(tn_tensor);
            if (it == llm_tensor_info_mapping.end()) {
                throw std::runtime_error(format("missing tensor info mapping for %s", tn.str().c_str()));
            }
            const auto & info = it->second;

            // tensors with "bias" suffix are always used with LM_GGML_OP_ADD
            lm_ggml_op op;
            bool bias = tn.suffix != nullptr && strcmp(tn.suffix, "bias") == 0;
            if (bias) {
                op = LM_GGML_OP_ADD;
            } else {
                op = info.op;
            }

            // sanity checks
            if (info.layer == LLM_TENSOR_LAYER_INPUT || info.layer == LLM_TENSOR_LAYER_OUTPUT) {
                if (tn.bid != -1) {
                    LM_GGML_ABORT("input/output layer tensor %s used with a layer number", tn.str().c_str());
                }
            } else {
                if (tn.bid == -1) {
                    LM_GGML_ABORT("repeating layer tensor %s used without a layer number", tn.str().c_str());
                }
            }

            // select the buffer type for this tensor
            llama_model::buft_list_t * buft_list;
            switch (info.layer) {
                case LLM_TENSOR_LAYER_INPUT:
                    buft_list = model.dev_input.buft_list;
                    break;
                case LLM_TENSOR_LAYER_OUTPUT:
                    buft_list = model.dev_output.buft_list;
                    break;
                case LLM_TENSOR_LAYER_REPEATING:
                    buft_list = model.dev_layer.at(tn.bid).buft_list;
                    break;
                default:
                    LM_GGML_ABORT("invalid layer %d for tensor %s", info.layer, tn.str().c_str());
            }

            lm_ggml_backend_buffer_type_t buft = select_weight_buft(model, t_meta, op, *buft_list);
            if (!buft) {
                throw std::runtime_error(format("failed to find a compatible buffer type for tensor %s", tn.str().c_str()));
            }

            // avoid using a host buffer when using mmap
            auto * buft_dev = lm_ggml_backend_buft_get_device(buft);
            if (ml.use_mmap && buft_dev && buft == lm_ggml_backend_dev_host_buffer_type(buft_dev)) {
                auto * cpu_dev = lm_ggml_backend_dev_by_type(LM_GGML_BACKEND_DEVICE_TYPE_CPU);
                buft = lm_ggml_backend_dev_buffer_type(cpu_dev);
            }

            if (buft != buft_list->front().second) {
                n_moved_tensors++;
                if (!first_moved_tensor) {
                    first_moved_tensor = t_meta;
                    first_moved_from_buft = buft_list->front().second;
                    first_moved_to_buft   = buft;
                }
            }

            lm_ggml_context * ctx = ctx_for_buft(buft);

            // if duplicated, check if the original tensor was allocated in the same buffer type context and avoid creating a new one
            if (flags & llama_model_loader::TENSOR_DUPLICATED) {
                lm_ggml_tensor * t = lm_ggml_get_tensor(ctx, tn.str().c_str());
                if (t) {
                    return t;
                }
            }
            return ml.create_tensor(ctx, tn, ne, flags);
        };

        model.layers.resize(n_layer);

        // TODO: move to a separate function
        const auto tn = LLM_TN(model.arch);
        switch (model.arch) {
            case LLM_ARCH_LLAMA:
            case LLM_ARCH_REFACT:
            case LLM_ARCH_MINICPM:
            case LLM_ARCH_GRANITE:
            case LLM_ARCH_GRANITE_MOE:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output      = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_NOT_REQUIRED);

                    // if output is NULL, init from the input tok embed
                    if (model.output == NULL) {
                        model.output = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_DUPLICATED);
                    }

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);

                        layer.wq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "weight", i), {n_embd, n_embd_head_k * n_head}, 0);
                        layer.wk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "weight", i), {n_embd, n_embd_k_gqa}, 0);
                        layer.wv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "weight", i), {n_embd, n_embd_v_gqa}, 0);
                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd_head_k * n_head, n_embd}, 0);

                        // optional bias tensors
                        layer.bq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "bias", i), {n_embd},     llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.bk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "bias", i), {n_embd_gqa}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.bv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "bias", i), {n_embd_gqa}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.bo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "bias", i), {n_embd},     llama_model_loader::TENSOR_NOT_REQUIRED);

                        layer.ffn_norm = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);

                        layer.rope_freqs = create_tensor(tn(LLM_TENSOR_ROPE_FREQS, "weight", i), {n_rot/2}, llama_model_loader::TENSOR_NOT_REQUIRED | (i != 0 ? llama_model_loader::TENSOR_DUPLICATED : 0));

                        if (n_expert == 0) {
                            layer.ffn_gate = create_tensor(tn(LLM_TENSOR_FFN_GATE, "weight", i), {n_embd,   n_ff}, 0);
                            layer.ffn_down = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {  n_ff, n_embd}, 0);
                            layer.ffn_up   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd,   n_ff}, 0);

                            // optional MLP bias
                            layer.ffn_gate_b = create_tensor(tn(LLM_TENSOR_FFN_GATE, "bias", i), {n_ff}, llama_model_loader::TENSOR_NOT_REQUIRED);
                            layer.ffn_down_b = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "bias", i), {n_embd}, llama_model_loader::TENSOR_NOT_REQUIRED);
                            layer.ffn_up_b   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "bias", i), {n_ff}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        } else {
                            layer.ffn_gate_inp  = create_tensor(tn(LLM_TENSOR_FFN_GATE_INP,  "weight", i), {n_embd, n_expert}, 0);
                            layer.ffn_gate_exps = create_tensor(tn(LLM_TENSOR_FFN_GATE_EXPS, "weight", i), {n_embd,   n_ff, n_expert}, llama_model_loader::TENSOR_NOT_REQUIRED);
                            layer.ffn_down_exps = create_tensor(tn(LLM_TENSOR_FFN_DOWN_EXPS, "weight", i), {  n_ff, n_embd, n_expert}, 0);
                            layer.ffn_up_exps   = create_tensor(tn(LLM_TENSOR_FFN_UP_EXPS,   "weight", i), {n_embd,   n_ff, n_expert}, 0);
                        }
                    }
                } break;
            case LLM_ARCH_MINICPM3:
                {
                    const int64_t n_embd_head_qk_rope = hparams.n_rot;
                    const int64_t n_embd_head_qk_nope = hparams.n_embd_head_k - hparams.n_rot;

                    const int64_t q_lora_rank  = hparams.n_lora_q;
                    const int64_t kv_lora_rank = hparams.n_lora_kv;
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output      = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_NOT_REQUIRED);

                    // if output is NULL, init from the input tok embed
                    if (model.output == NULL) {
                        model.output = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_DUPLICATED);
                    }

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);
                        layer.attn_q_a_norm = create_tensor(tn(LLM_TENSOR_ATTN_Q_A_NORM, "weight", i), {q_lora_rank}, 0);

                        layer.attn_kv_a_norm = create_tensor(tn(LLM_TENSOR_ATTN_KV_A_NORM, "weight", i), {kv_lora_rank}, 0);

                        layer.wq_a = create_tensor(tn(LLM_TENSOR_ATTN_Q_A, "weight", i), {n_embd, q_lora_rank}, 0);
                        layer.wq_b = create_tensor(tn(LLM_TENSOR_ATTN_Q_B, "weight", i), {q_lora_rank, n_head * n_embd_head_k}, 0);

                        layer.wkv_a_mqa = create_tensor(tn(LLM_TENSOR_ATTN_KV_A_MQA, "weight", i), {n_embd, kv_lora_rank + (n_embd_head_qk_rope)}, 0);
                        layer.wkv_b     = create_tensor(tn(LLM_TENSOR_ATTN_KV_B,     "weight", i), {kv_lora_rank, n_head * (n_embd_head_qk_nope + n_embd_head_v)}, 0);
                        layer.wo        = create_tensor(tn(LLM_TENSOR_ATTN_OUT,      "weight", i), {              n_head * (                      n_embd_head_v), n_embd}, 0);

                        layer.ffn_norm = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);

                        layer.ffn_gate = create_tensor(tn(LLM_TENSOR_FFN_GATE, "weight", i), {n_embd,   n_ff}, 0);
                        layer.ffn_down = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {  n_ff, n_embd}, 0);
                        layer.ffn_up   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd,   n_ff}, 0);

                        layer.rope_long  = create_tensor(tn(LLM_TENSOR_ROPE_FACTORS_LONG,  "weight", i), { n_embd_head_qk_rope/2 }, llama_model_loader::TENSOR_NOT_REQUIRED | (i != 0 ? llama_model_loader::TENSOR_DUPLICATED : 0));
                        layer.rope_short = create_tensor(tn(LLM_TENSOR_ROPE_FACTORS_SHORT, "weight", i), { n_embd_head_qk_rope/2 }, llama_model_loader::TENSOR_NOT_REQUIRED | (i != 0 ? llama_model_loader::TENSOR_DUPLICATED : 0));
                    }
                } break;
            case LLM_ARCH_GROK:
                {
                    if (n_expert == 0) {
                        throw std::runtime_error("Grok model cannot have zero experts");
                    }

                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output      = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_NOT_REQUIRED);

                    // if output is NULL, init from the input tok embed
                    if (model.output == NULL) {
                        model.output = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_DUPLICATED);
                    }

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);

                        layer.wq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "weight", i), {n_embd, n_embd}, 0);
                        layer.wk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);

                        layer.attn_out_norm   = create_tensor(tn(LLM_TENSOR_ATTN_OUT_NORM, "weight", i), {n_embd}, 0);

                        layer.ffn_norm = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);

                        layer.ffn_gate_inp  = create_tensor(tn(LLM_TENSOR_FFN_GATE_INP,  "weight", i), {n_embd, n_expert}, 0);
                        layer.ffn_gate_exps = create_tensor(tn(LLM_TENSOR_FFN_GATE_EXPS, "weight", i), {n_embd, n_ff, n_expert}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.ffn_down_exps = create_tensor(tn(LLM_TENSOR_FFN_DOWN_EXPS, "weight", i), {  n_ff, n_embd, n_expert}, 0);
                        layer.ffn_up_exps   = create_tensor(tn(LLM_TENSOR_FFN_UP_EXPS,   "weight", i), {n_embd,   n_ff, n_expert}, 0);

                        layer.layer_out_norm   = create_tensor(tn(LLM_TENSOR_LAYER_OUT_NORM, "weight", i), {n_embd}, 0);
                    }
                } break;
            case LLM_ARCH_DBRX:
                {
                    if (n_expert == 0) {
                        throw std::runtime_error("DBRX model cannot have zero experts");
                    }

                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output      = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, 0);

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);

                        layer.wqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "weight", i), {n_embd, n_embd + 2*n_embd_gqa}, 0);
                        layer.wo   = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);

                        layer.attn_out_norm = create_tensor(tn(LLM_TENSOR_ATTN_OUT_NORM, "weight", i), {n_embd}, 0);

                        layer.ffn_gate_inp  = create_tensor(tn(LLM_TENSOR_FFN_GATE_INP,  "weight", i), {n_embd, n_expert}, 0);
                        layer.ffn_gate_exps = create_tensor(tn(LLM_TENSOR_FFN_GATE_EXPS, "weight", i), {n_embd, n_ff,   n_expert}, 0);
                        layer.ffn_down_exps = create_tensor(tn(LLM_TENSOR_FFN_DOWN_EXPS, "weight", i), {n_ff,   n_embd, n_expert}, 0);
                        layer.ffn_up_exps   = create_tensor(tn(LLM_TENSOR_FFN_UP_EXPS,   "weight", i), {n_embd, n_ff,   n_expert}, 0);
                    }
                } break;
            case LLM_ARCH_BAICHUAN:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);
                    {
                        model.output_norm = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                        model.output      = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, 0);
                    }

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);

                        layer.wq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "weight", i), {n_embd, n_embd}, 0);
                        layer.wk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);

                        layer.ffn_norm = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);

                        layer.ffn_gate = create_tensor(tn(LLM_TENSOR_FFN_GATE, "weight", i), {n_embd,   n_ff}, 0);
                        layer.ffn_down = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {  n_ff, n_embd}, 0);
                        layer.ffn_up   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd,   n_ff}, 0);
                    }
                } break;
            case LLM_ARCH_FALCON:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    {
                        model.output_norm   = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                        model.output_norm_b = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "bias"),   {n_embd}, 0);

                        model.output = create_tensor(tn(LLM_TENSOR_OUTPUT, "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        if (!model.output) {
                            model.output = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_DUPLICATED); // needs to be on GPU
                        }
                    }

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm   = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);
                        layer.attn_norm_b = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "bias", i),   {n_embd}, 0);

                        layer.attn_norm_2   = create_tensor(tn(LLM_TENSOR_ATTN_NORM_2, "weight", i), {n_embd}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.attn_norm_2_b = create_tensor(tn(LLM_TENSOR_ATTN_NORM_2, "bias", i),   {n_embd}, llama_model_loader::TENSOR_NOT_REQUIRED);

                        layer.wqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "weight", i), {n_embd, n_embd + 2*n_embd_gqa}, 0);
                        layer.wo   = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);

                        layer.ffn_down = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {  n_ff, n_embd}, 0);
                        layer.ffn_up   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd,   n_ff}, 0);
                    }
                } break;
            case LLM_ARCH_STARCODER:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);
                    model.pos_embd = create_tensor(tn(LLM_TENSOR_POS_EMBD,   "weight"), {n_embd, n_ctx_train}, 0);

                    // output
                    {
                        model.output_norm   = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                        model.output_norm_b = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "bias"),   {n_embd}, 0);
                        model.output        = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        if (!model.output) {
                            // needs to be on GPU
                            model.output = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_DUPLICATED);
                        }

                    }

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm   = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);
                        layer.attn_norm_b = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "bias", i),   {n_embd}, 0);

                        layer.wqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "weight", i), {n_embd, n_embd + 2*n_embd_gqa}, 0);
                        layer.bqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "bias", i),   {n_embd + 2*n_embd_gqa}, 0);

                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);
                        layer.bo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "bias", i),   {n_embd}, 0);

                        layer.ffn_norm   = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);
                        layer.ffn_norm_b = create_tensor(tn(LLM_TENSOR_FFN_NORM, "bias", i),   {n_embd}, 0);

                        layer.ffn_down   = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {n_ff, n_embd}, 0);
                        layer.ffn_down_b = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "bias", i),   {n_embd}, 0);

                        layer.ffn_up   = create_tensor(tn(LLM_TENSOR_FFN_UP, "weight", i),   {n_embd, n_ff}, 0);
                        layer.ffn_up_b = create_tensor(tn(LLM_TENSOR_FFN_UP, "bias", i),     {n_ff}, 0);
                    }
                } break;
            case LLM_ARCH_BERT:
            case LLM_ARCH_NOMIC_BERT:
                {
                    model.tok_embd     = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD,  "weight"), {n_embd, n_vocab}, 0);
                    model.type_embd    = create_tensor(tn(LLM_TENSOR_TOKEN_TYPES, "weight"), {n_embd, n_vocab_type}, 0);

                    if (model.arch == LLM_ARCH_BERT) {
                        model.pos_embd = create_tensor(tn(LLM_TENSOR_POS_EMBD,    "weight"), {n_embd, n_ctx_train}, 0);

                        model.cls   = create_tensor(tn(LLM_TENSOR_CLS, "weight"), {n_embd, n_embd}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        model.cls_b = create_tensor(tn(LLM_TENSOR_CLS, "bias"),   {n_embd},         llama_model_loader::TENSOR_NOT_REQUIRED);

                        model.cls_out   = create_tensor(tn(LLM_TENSOR_CLS_OUT, "weight"), {n_embd, 1}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        model.cls_out_b = create_tensor(tn(LLM_TENSOR_CLS_OUT, "bias"),   {1},         llama_model_loader::TENSOR_NOT_REQUIRED);
                    }

                    model.tok_norm   = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD_NORM, "weight"), {n_embd}, 0);
                    model.tok_norm_b = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD_NORM, "bias"),   {n_embd}, 0);

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        if (model.arch == LLM_ARCH_BERT) {
                            layer.wq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "weight", i), {n_embd, n_embd}, 0);
                            layer.bq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "bias", i),   {n_embd}, 0);

                            layer.wk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "weight", i), {n_embd, n_embd_gqa}, 0);
                            layer.bk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "bias", i),   {n_embd_gqa}, 0);

                            layer.wv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "weight", i), {n_embd, n_embd_gqa}, 0);
                            layer.bv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "bias", i),   {n_embd_gqa}, 0);
                        } else {
                            layer.wqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "weight", i), {n_embd, n_embd + 2*n_embd_gqa}, 0);
                        }

                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT,      "weight", i), {n_embd, n_embd}, 0);

                        layer.attn_out_norm   = create_tensor(tn(LLM_TENSOR_ATTN_OUT_NORM, "weight", i), {n_embd}, 0);
                        layer.attn_out_norm_b = create_tensor(tn(LLM_TENSOR_ATTN_OUT_NORM, "bias", i),   {n_embd}, 0);

                        layer.ffn_up   = create_tensor(tn(LLM_TENSOR_FFN_UP,        "weight", i), {n_embd, n_ff}, 0);
                        layer.ffn_down = create_tensor(tn(LLM_TENSOR_FFN_DOWN,      "weight", i), {n_ff, n_embd}, 0);

                        if (model.arch == LLM_ARCH_BERT) {
                            layer.bo         = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "bias", i), {n_embd}, 0);
                            layer.ffn_up_b   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "bias", i), {n_ff}, 0);
                            layer.ffn_down_b = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "bias", i), {n_embd}, 0);
                        } else {
                            layer.ffn_gate = create_tensor(tn(LLM_TENSOR_FFN_GATE, "weight", i), {n_embd, n_ff}, 0);
                        }

                        layer.layer_out_norm   = create_tensor(tn(LLM_TENSOR_LAYER_OUT_NORM, "weight", i), {n_embd}, 0);
                        layer.layer_out_norm_b = create_tensor(tn(LLM_TENSOR_LAYER_OUT_NORM, "bias", i),   {n_embd}, 0);
                    }
                } break;
            case LLM_ARCH_JINA_BERT_V2:
                {
                    model.tok_embd  = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD,  "weight"), {n_embd, n_vocab}, 0); // word_embeddings
                    model.type_embd = create_tensor(tn(LLM_TENSOR_TOKEN_TYPES, "weight"), {n_embd, n_vocab_type}, 0); // token_type_embeddings

                    model.tok_norm   = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD_NORM, "weight"), {n_embd}, 0); // LayerNorm
                    model.tok_norm_b = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD_NORM, "bias"),   {n_embd}, 0); //LayerNorm bias

                    model.cls   = create_tensor(tn(LLM_TENSOR_CLS, "weight"), {n_embd, 1}, llama_model_loader::TENSOR_NOT_REQUIRED);
                    model.cls_b = create_tensor(tn(LLM_TENSOR_CLS, "bias"),   {1},         llama_model_loader::TENSOR_NOT_REQUIRED);
                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i]; // JinaBertLayer

                        layer.wq = create_tensor(tn(LLM_TENSOR_ATTN_Q, "weight", i), {n_embd, n_embd}, 0);
                        layer.bq = create_tensor(tn(LLM_TENSOR_ATTN_Q, "bias", i),   {n_embd}, 0);

                        layer.attn_q_norm   = create_tensor(tn(LLM_TENSOR_ATTN_Q_NORM, "weight", i), {n_embd}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.attn_q_norm_b = create_tensor(tn(LLM_TENSOR_ATTN_Q_NORM, "bias",   i), {n_embd}, llama_model_loader::TENSOR_NOT_REQUIRED);

                        layer.wk = create_tensor(tn(LLM_TENSOR_ATTN_K, "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.bk = create_tensor(tn(LLM_TENSOR_ATTN_K, "bias",   i), {n_embd_gqa}, 0);

                        layer.attn_k_norm   = create_tensor(tn(LLM_TENSOR_ATTN_K_NORM, "weight", i), {n_embd}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.attn_k_norm_b = create_tensor(tn(LLM_TENSOR_ATTN_K_NORM, "bias",   i), {n_embd}, llama_model_loader::TENSOR_NOT_REQUIRED);

                        layer.wv = create_tensor(tn(LLM_TENSOR_ATTN_V, "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.bv = create_tensor(tn(LLM_TENSOR_ATTN_V, "bias",   i), {n_embd_gqa}, 0);

                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0); //output_dens
                        layer.bo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "bias",   i), {n_embd}, 0); //output_dens

                        layer.attn_out_norm   = create_tensor(tn(LLM_TENSOR_ATTN_OUT_NORM, "weight", i), {n_embd}, 0); //output_norm
                        layer.attn_out_norm_b = create_tensor(tn(LLM_TENSOR_ATTN_OUT_NORM, "bias",   i), {n_embd}, 0);

                        layer.attn_norm_2   = create_tensor(tn(LLM_TENSOR_ATTN_NORM_2, "weight", i), {n_embd}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.attn_norm_2_b = create_tensor(tn(LLM_TENSOR_ATTN_NORM_2, "bias",   i), {n_embd}, llama_model_loader::TENSOR_NOT_REQUIRED);

                        layer.ffn_up   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd, n_ff}, 0);
                        layer.ffn_gate = create_tensor(tn(LLM_TENSOR_FFN_GATE, "weight", i), {n_embd, n_ff}, 0);

                        layer.ffn_down   = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {n_ff, n_embd}, 0);
                        layer.ffn_down_b = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "bias",   i), {n_embd}, 0);

                        layer.layer_out_norm   = create_tensor(tn(LLM_TENSOR_LAYER_OUT_NORM, "weight", i), {n_embd}, 0);
                        layer.layer_out_norm_b = create_tensor(tn(LLM_TENSOR_LAYER_OUT_NORM, "bias",   i), {n_embd}, 0);
                    }
                } break;
            case LLM_ARCH_BLOOM:
                {
                    model.tok_embd   = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD,      "weight"), {n_embd, n_vocab}, 0);
                    model.tok_norm   = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD_NORM, "weight"), {n_embd}, 0);
                    model.tok_norm_b = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD_NORM, "bias"),   {n_embd}, 0);

                    // output
                    model.output_norm   = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output_norm_b = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "bias"),   {n_embd}, 0);
                    model.output        = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, 0);

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm   = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);
                        layer.attn_norm_b = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "bias",   i), {n_embd}, 0);

                        layer.wqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "weight", i), {n_embd, n_embd + 2*n_embd_gqa}, 0);
                        layer.bqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "bias",   i), {n_embd + 2*n_embd_gqa}, 0);

                        layer.wo   = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);
                        layer.bo   = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "bias",   i), {n_embd}, 0);

                        layer.ffn_norm   = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);
                        layer.ffn_norm_b = create_tensor(tn(LLM_TENSOR_FFN_NORM, "bias",   i), {n_embd}, 0);

                        layer.ffn_down   = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {n_ff, n_embd}, 0);
                        layer.ffn_down_b = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "bias",   i), {n_embd}, 0);

                        layer.ffn_up     = create_tensor(tn(LLM_TENSOR_FFN_UP, "weight", i), {n_embd, n_ff}, 0);
                        layer.ffn_up_b   = create_tensor(tn(LLM_TENSOR_FFN_UP, "bias",   i), {n_ff}, 0);
                    }
                } break;
            case LLM_ARCH_MPT:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);
                    model.pos_embd = create_tensor(tn(LLM_TENSOR_POS_EMBD,   "weight"), {n_embd, n_ctx_train}, llama_model_loader::TENSOR_NOT_REQUIRED);

                    // output
                    model.output_norm   = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output_norm_b = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "bias"),   {n_embd}, llama_model_loader::TENSOR_NOT_REQUIRED);

                    model.output        = create_tensor(tn(LLM_TENSOR_OUTPUT, "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_NOT_REQUIRED);
                    if (!model.output) {
                        model.output    = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_DUPLICATED); // needs to be on GPU
                    }

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm   = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);
                        layer.attn_norm_b = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "bias", i),   {n_embd}, llama_model_loader::TENSOR_NOT_REQUIRED);

                        layer.wqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "weight", i), {n_embd, n_embd + 2*n_embd_gqa}, 0);
                        layer.bqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "bias", i),   {n_embd + 2*n_embd_gqa}, llama_model_loader::TENSOR_NOT_REQUIRED);

                        layer.wo   = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);
                        layer.bo   = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "bias", i),   {n_embd}, llama_model_loader::TENSOR_NOT_REQUIRED);

                        layer.ffn_norm   = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);
                        layer.ffn_norm_b = create_tensor(tn(LLM_TENSOR_FFN_NORM, "bias", i),   {n_embd}, llama_model_loader::TENSOR_NOT_REQUIRED);

                        layer.ffn_down   = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {n_ff, n_embd}, 0);
                        layer.ffn_down_b = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "bias", i),   {n_embd}, llama_model_loader::TENSOR_NOT_REQUIRED);

                        layer.ffn_up     = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd,   n_ff}, 0);
                        layer.ffn_up_b   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "bias", i),   {n_ff}, llama_model_loader::TENSOR_NOT_REQUIRED);

                        layer.attn_q_norm   = create_tensor(tn(LLM_TENSOR_ATTN_Q_NORM, "weight", i), {n_embd}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.attn_q_norm_b = create_tensor(tn(LLM_TENSOR_ATTN_Q_NORM, "bias",   i), {n_embd}, llama_model_loader::TENSOR_NOT_REQUIRED);

                        layer.attn_k_norm   = create_tensor(tn(LLM_TENSOR_ATTN_K_NORM, "weight", i), {n_embd}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.attn_k_norm_b = create_tensor(tn(LLM_TENSOR_ATTN_K_NORM, "bias",   i), {n_embd}, llama_model_loader::TENSOR_NOT_REQUIRED);

                        // AWQ ScaleActivation layer
                        layer.ffn_act = create_tensor(tn(LLM_TENSOR_FFN_ACT, "scales", i), {n_ff}, llama_model_loader::TENSOR_NOT_REQUIRED);
                    }
                } break;
            case LLM_ARCH_STABLELM:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm_b = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "bias"),   {n_embd}, 0);
                    model.output_norm   = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output        = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, 0);

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm =   create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);
                        layer.attn_norm_b = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "bias", i), {n_embd}, 0);

                        layer.wq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "weight", i), {n_embd, n_embd}, 0);
                        layer.wk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);

                        // optional bias tensors, present in Stable LM 2 1.6B
                        layer.bq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "bias", i), {n_embd},     llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.bk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "bias", i), {n_embd_gqa}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.bv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "bias", i), {n_embd_gqa}, llama_model_loader::TENSOR_NOT_REQUIRED);

                        // optional q and k layernorms, present in StableLM 2 12B
                        layer.attn_q_norm = create_tensor(tn(LLM_TENSOR_ATTN_Q_NORM, "weight", i), {n_embd_head_k, n_head},    llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.attn_k_norm = create_tensor(tn(LLM_TENSOR_ATTN_K_NORM, "weight", i), {n_embd_head_k, n_head_kv}, llama_model_loader::TENSOR_NOT_REQUIRED);

                        // optional FFN norm, not present in StableLM 2 12B which uses parallel residual
                        layer.ffn_norm   = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.ffn_norm_b = create_tensor(tn(LLM_TENSOR_FFN_NORM, "bias", i),   {n_embd}, llama_model_loader::TENSOR_NOT_REQUIRED);

                        layer.ffn_gate = create_tensor(tn(LLM_TENSOR_FFN_GATE, "weight", i), {n_embd,   n_ff}, 0);
                        layer.ffn_down = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {  n_ff, n_embd}, 0);
                        layer.ffn_up   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd,   n_ff}, 0);
                    }
                } break;
            case LLM_ARCH_QWEN:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output      = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, 0);

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);

                        layer.wqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "weight", i), {n_embd, n_embd*3}, 0);
                        layer.bqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "bias", i),   {n_embd*3}, 0);
                        layer.wo   = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);

                        layer.ffn_norm = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);

                        layer.ffn_gate = create_tensor(tn(LLM_TENSOR_FFN_GATE, "weight", i), {n_embd, n_ff/2}, 0);
                        layer.ffn_down = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {n_ff/2, n_embd}, 0);
                        layer.ffn_up   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd, n_ff/2}, 0);
                    }
                } break;
            case LLM_ARCH_QWEN2:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output      = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_NOT_REQUIRED);
                    // if output is NULL, init from the input tok embed
                    if (model.output == NULL) {
                        model.output = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_DUPLICATED);
                    }

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);

                        layer.wq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "weight", i), {n_embd, n_embd}, 0);
                        layer.wk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);

                        // optional bias tensors
                        layer.bq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "bias", i), {n_embd}, 0);
                        layer.bk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "bias", i), {n_embd_gqa}, 0);
                        layer.bv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "bias", i), {n_embd_gqa}, 0);

                        layer.ffn_norm = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);

                        layer.ffn_gate = create_tensor(tn(LLM_TENSOR_FFN_GATE, "weight", i), {n_embd,   n_ff}, 0);
                        layer.ffn_down = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {  n_ff, n_embd}, 0);
                        layer.ffn_up   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd,   n_ff}, 0);
                    }
                } break;
            case LLM_ARCH_QWEN2MOE:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output      = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, 0);

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);

                        layer.wq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "weight", i), {n_embd, n_embd}, 0);
                        layer.wk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);

                        // optional bias tensors
                        layer.bq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "bias", i), {n_embd}, 0);
                        layer.bk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "bias", i), {n_embd_gqa}, 0);
                        layer.bv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "bias", i), {n_embd_gqa}, 0);

                        layer.ffn_norm = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);

                        layer.ffn_gate_inp = create_tensor(tn(LLM_TENSOR_FFN_GATE_INP, "weight", i), {n_embd, n_expert}, 0);

                        if (n_expert == 0) {
                            throw std::runtime_error("n_expert must be > 0 for QWEN2MOE");
                        }
                        if (n_expert_used == 0) {
                            throw std::runtime_error("n_expert_used must be > 0 for QWEN2MOE");
                        }

                        // MoE branch
                        const int64_t n_ff_exp = hparams.n_ff_exp ? hparams.n_ff_exp : n_ff / n_expert_used;

                        layer.ffn_gate_exps = create_tensor(tn(LLM_TENSOR_FFN_GATE_EXPS, "weight", i), {  n_embd, n_ff_exp, n_expert}, 0);
                        layer.ffn_down_exps = create_tensor(tn(LLM_TENSOR_FFN_DOWN_EXPS, "weight", i), {n_ff_exp,   n_embd, n_expert}, 0);
                        layer.ffn_up_exps   = create_tensor(tn(LLM_TENSOR_FFN_UP_EXPS,   "weight", i), {  n_embd, n_ff_exp, n_expert}, 0);

                        // Shared expert branch
                        const int64_t n_ff_shexp = hparams.n_ff_shexp ? hparams.n_ff_shexp : n_ff;

                        layer.ffn_gate_inp_shexp = create_tensor(tn(LLM_TENSOR_FFN_GATE_INP_SHEXP, "weight", i), {n_embd}, 0);
                        layer.ffn_gate_shexp = create_tensor(tn(LLM_TENSOR_FFN_GATE_SHEXP, "weight", i), {    n_embd, n_ff_shexp}, 0);
                        layer.ffn_down_shexp = create_tensor(tn(LLM_TENSOR_FFN_DOWN_SHEXP, "weight", i), {n_ff_shexp,     n_embd}, 0);
                        layer.ffn_up_shexp   = create_tensor(tn(LLM_TENSOR_FFN_UP_SHEXP,   "weight", i), {    n_embd, n_ff_shexp}, 0);
                    }
                } break;
            case LLM_ARCH_PHI2:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm   = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output_norm_b = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "bias"),   {n_embd}, 0);
                    model.output        = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, 0);
                    model.output_b      = create_tensor(tn(LLM_TENSOR_OUTPUT,      "bias"),   {n_vocab}, 0);

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm   = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);
                        layer.attn_norm_b = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "bias", i),   {n_embd}, 0);

                        layer.wqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "weight", i), {n_embd, n_embd + 2*n_embd_gqa}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.bqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "bias", i),   {n_embd + 2*n_embd_gqa}, llama_model_loader::TENSOR_NOT_REQUIRED);

                        if (layer.wqkv == nullptr) {
                            layer.wq = create_tensor(tn(LLM_TENSOR_ATTN_Q, "weight", i), {n_embd, n_embd}, 0);
                            layer.bq = create_tensor(tn(LLM_TENSOR_ATTN_Q, "bias", i),   {n_embd}, 0);

                            layer.wk = create_tensor(tn(LLM_TENSOR_ATTN_K, "weight", i), {n_embd, n_embd_gqa}, 0);
                            layer.bk = create_tensor(tn(LLM_TENSOR_ATTN_K, "bias", i),   {n_embd_gqa}, 0);

                            layer.wv = create_tensor(tn(LLM_TENSOR_ATTN_V, "weight", i), {n_embd, n_embd_gqa}, 0);
                            layer.bv = create_tensor(tn(LLM_TENSOR_ATTN_V, "bias", i),   {n_embd_gqa}, 0);
                        }

                        layer.wo   = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);
                        layer.bo   = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "bias", i),   {n_embd}, 0);

                        layer.ffn_down   = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {n_ff, n_embd}, 0);
                        layer.ffn_down_b = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "bias", i),   {n_embd}, 0);

                        layer.ffn_up     = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd, n_ff}, 0);
                        layer.ffn_up_b   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "bias", i),   {n_ff}, 0);
                    }
                } break;
            case LLM_ARCH_PHI3:
                {
                    const int64_t n_embd_head = n_embd / n_head;

                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), { n_embd, n_vocab }, 0);

                    // output
                    model.output_norm = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), { n_embd }, 0);
                    model.output = create_tensor(tn(LLM_TENSOR_OUTPUT, "weight"), { n_embd, n_vocab }, 0);

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), { n_embd }, 0);

                        layer.wqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "weight", i), { n_embd, n_embd + 2 * n_embd_gqa }, llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.wo   = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), { n_embd, n_embd }, 0);

                        layer.ffn_norm = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), { n_embd }, 0);

                        layer.ffn_down = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), { n_ff, n_embd }, 0);
                        layer.ffn_up = create_tensor(tn(LLM_TENSOR_FFN_UP, "weight", i), { n_embd, 2 * n_ff }, 0);

                        layer.rope_long  = create_tensor(tn(LLM_TENSOR_ROPE_FACTORS_LONG,  "weight", i), { n_embd_head/2 }, llama_model_loader::TENSOR_NOT_REQUIRED | (i != 0 ? llama_model_loader::TENSOR_DUPLICATED : 0));
                        layer.rope_short = create_tensor(tn(LLM_TENSOR_ROPE_FACTORS_SHORT, "weight", i), { n_embd_head/2 }, llama_model_loader::TENSOR_NOT_REQUIRED | (i != 0 ? llama_model_loader::TENSOR_DUPLICATED : 0));
                    }
                } break;
            case LLM_ARCH_PLAMO:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output      = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, 0);

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);

                        layer.wq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "weight", i), {n_embd, n_embd}, 0);
                        layer.wk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);

                        layer.ffn_gate = create_tensor(tn(LLM_TENSOR_FFN_GATE, "weight", i), {n_embd,   n_ff}, 0);
                        layer.ffn_down = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {  n_ff, n_embd}, 0);
                        layer.ffn_up   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd,   n_ff}, 0);
                    }
                } break;
            case LLM_ARCH_GPT2:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);
                    model.pos_embd = create_tensor(tn(LLM_TENSOR_POS_EMBD,   "weight"), {n_embd, n_ctx_train}, 0);

                    // output
                    model.output_norm   = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output_norm_b = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "bias"),   {n_embd}, 0);
                    model.output        = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, 0);

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm   = create_tensor(tn(LLM_TENSOR_ATTN_NORM,   "weight", i), {n_embd}, 0);
                        layer.attn_norm_b = create_tensor(tn(LLM_TENSOR_ATTN_NORM,   "bias", i),   {n_embd}, 0);

                        layer.wqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "weight", i), {n_embd, n_embd + 2*n_embd_gqa}, 0);
                        layer.bqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "bias", i),   {n_embd + 2*n_embd_gqa}, 0);

                        layer.wo   = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);
                        layer.bo   = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "bias", i),   {n_embd}, 0);

                        layer.ffn_norm   = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);
                        layer.ffn_norm_b = create_tensor(tn(LLM_TENSOR_FFN_NORM, "bias", i),   {n_embd}, 0);

                        layer.ffn_down   = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {n_ff, n_embd}, 0);
                        layer.ffn_down_b = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "bias", i),   {n_embd}, 0);

                        layer.ffn_up     = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd, n_ff}, 0);
                        layer.ffn_up_b   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "bias", i),   {n_ff}, 0);
                    }
                } break;
            case LLM_ARCH_CODESHELL:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm   = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output_norm_b = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "bias"),   {n_embd}, 0);
                    model.output        = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, 0);

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm   = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);
                        layer.attn_norm_b = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "bias", i),   {n_embd}, 0);

                        layer.wqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "weight", i), {n_embd, n_embd + 2*n_embd_gqa}, 0);
                        layer.bqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "bias", i),   {n_embd + 2*n_embd_gqa}, 0);

                        layer.wo   = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);
                        layer.bo   = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "bias", i),   {n_embd}, 0);

                        layer.ffn_norm   = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);
                        layer.ffn_norm_b = create_tensor(tn(LLM_TENSOR_FFN_NORM, "bias", i),   {n_embd}, 0);

                        layer.ffn_down   = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {n_ff, n_embd}, 0);
                        layer.ffn_down_b = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "bias", i),   {n_embd}, 0);

                        layer.ffn_up     = create_tensor(tn(LLM_TENSOR_FFN_UP, "weight", i),   {n_embd, n_ff}, 0);
                        layer.ffn_up_b   = create_tensor(tn(LLM_TENSOR_FFN_UP, "bias", i),     {n_ff}, 0);
                    }
                } break;
            case LLM_ARCH_ORION:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    model.output_norm   = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output_norm_b = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "bias"),   {n_embd}, 0);
                    model.output        = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, 0);

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm   = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);
                        layer.attn_norm_b = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "bias", i),   {n_embd}, 0);

                        layer.wq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "weight", i), {n_embd, n_embd}, 0);
                        layer.wk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);

                        layer.ffn_norm   = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);
                        layer.ffn_norm_b = create_tensor(tn(LLM_TENSOR_FFN_NORM, "bias", i),   {n_embd}, 0);

                        layer.ffn_gate = create_tensor(tn(LLM_TENSOR_FFN_GATE, "weight", i), {n_embd,   n_ff}, 0);
                        layer.ffn_down = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {  n_ff, n_embd}, 0);
                        layer.ffn_up   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd,   n_ff}, 0);
                    }
                } break;
            case LLM_ARCH_INTERNLM2:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output      = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, 0);

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);
                        // layer.wqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "weight", i), {n_embd, n_embd + 2*n_embd_gqa}, 0);
                        layer.wq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "weight", i), {n_embd, n_embd}, 0);
                        layer.wk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "weight", i), {n_embd, n_embd_gqa}, 0);

                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);
                        layer.ffn_norm = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);
                        layer.ffn_gate = create_tensor(tn(LLM_TENSOR_FFN_GATE, "weight", i), {n_embd,   n_ff}, 0);
                        layer.ffn_down = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {  n_ff, n_embd}, 0);
                        layer.ffn_up   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd,   n_ff}, 0);
                    }
                } break;
            case LLM_ARCH_GEMMA:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output      = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD,  "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_DUPLICATED); // same as tok_embd, duplicated to allow offloading

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);

                        layer.wq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "weight", i), {n_embd, n_embd_head_k * n_head}, 0);
                        layer.wk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "weight", i), {n_embd, n_embd_k_gqa}, 0);
                        layer.wv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "weight", i), {n_embd, n_embd_v_gqa}, 0);
                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd_head_k * n_head, n_embd}, 0);

                        layer.ffn_norm = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);
                        layer.ffn_gate = create_tensor(tn(LLM_TENSOR_FFN_GATE, "weight", i), {n_embd,   n_ff}, 0);
                        layer.ffn_up   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd,   n_ff}, 0);
                        layer.ffn_down = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {  n_ff, n_embd}, 0);
                    }
                } break;
            case LLM_ARCH_GEMMA2:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output      = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD,  "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_DUPLICATED); // same as tok_embd, duplicated to allow offloading

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);

                        layer.wq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "weight", i), {n_embd, n_embd_head_k * n_head}, 0);
                        layer.wk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "weight", i), {n_embd, n_embd_k_gqa}, 0);
                        layer.wv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "weight", i), {n_embd, n_embd_v_gqa}, 0);
                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd_head_k * n_head, n_embd}, 0);
                        layer.attn_post_norm = create_tensor(tn(LLM_TENSOR_ATTN_POST_NORM, "weight", i), {n_embd}, 0);

                        layer.ffn_norm = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);
                        layer.ffn_gate = create_tensor(tn(LLM_TENSOR_FFN_GATE, "weight", i), {n_embd,   n_ff}, 0);
                        layer.ffn_up   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd,   n_ff}, 0);
                        layer.ffn_down = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {  n_ff, n_embd}, 0);
                        layer.ffn_post_norm = create_tensor(tn(LLM_TENSOR_FFN_POST_NORM, "weight", i), {n_embd}, 0);
                    }
                } break;
            case LLM_ARCH_STARCODER2:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm   = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output_norm_b = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "bias"),   {n_embd}, 0);

                    model.output = create_tensor(tn(LLM_TENSOR_OUTPUT, "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_NOT_REQUIRED);
                    // if output is NULL, init from the input tok embed
                    if (model.output == NULL) {
                        model.output = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_DUPLICATED);
                    }

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm   = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);
                        layer.attn_norm_b = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "bias", i),   {n_embd}, 0);

                        layer.wq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "weight", i), {n_embd, n_embd}, 0);
                        layer.wk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);

                        // optional bias tensors
                        layer.bq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "bias", i), {n_embd}, 0);
                        layer.bk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "bias", i), {n_embd_gqa}, 0);
                        layer.bv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "bias", i), {n_embd_gqa}, 0);
                        layer.bo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "bias", i), {n_embd}, 0);

                        layer.ffn_norm   = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);
                        layer.ffn_norm_b = create_tensor(tn(LLM_TENSOR_FFN_NORM, "bias", i),   {n_embd}, 0);

                        layer.ffn_down = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {  n_ff, n_embd}, 0);
                        layer.ffn_up   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd,   n_ff}, 0);

                        // optional bias tensors
                        layer.ffn_down_b = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "bias", i), {n_embd}, 0);
                        layer.ffn_up_b   = create_tensor(tn(LLM_TENSOR_FFN_UP ,  "bias", i), {  n_ff}, 0);
                    }
                } break;
            case LLM_ARCH_MAMBA:
                {
                    const int64_t d_conv  = hparams.ssm_d_conv;
                    const int64_t d_inner = hparams.ssm_d_inner;
                    const int64_t d_state = hparams.ssm_d_state;
                    const int64_t dt_rank = hparams.ssm_dt_rank;

                    // only an expansion factor of 2 is supported for now
                    if (2 * n_embd != d_inner) {
                        throw std::runtime_error("only an expansion factor of 2 is supported for now");
                    }

                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);

                    model.output = create_tensor(tn(LLM_TENSOR_OUTPUT, "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_NOT_REQUIRED);
                    // if output is NULL, init from the input tok embed, duplicated to allow offloading
                    if (model.output == NULL) {
                        model.output = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_DUPLICATED);
                    }

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        // norm
                        layer.attn_norm = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);

                        layer.ssm_in = create_tensor(tn(LLM_TENSOR_SSM_IN, "weight", i), {n_embd, 2*d_inner}, 0);

                        layer.ssm_conv1d = create_tensor(tn(LLM_TENSOR_SSM_CONV1D, "weight", i), {d_conv, d_inner}, 0);
                        layer.ssm_conv1d_b = create_tensor(tn(LLM_TENSOR_SSM_CONV1D, "bias", i), {d_inner}, 0);

                        layer.ssm_x = create_tensor(tn(LLM_TENSOR_SSM_X, "weight", i), {d_inner, dt_rank + 2*d_state}, 0);

                        layer.ssm_dt = create_tensor(tn(LLM_TENSOR_SSM_DT, "weight", i), {dt_rank, d_inner}, 0);
                        layer.ssm_dt_b = create_tensor(tn(LLM_TENSOR_SSM_DT, "bias", i), {d_inner}, 0);

                        // no "weight" suffix for these
                        layer.ssm_a = create_tensor(tn(LLM_TENSOR_SSM_A, i), {d_state, d_inner}, 0);
                        layer.ssm_d = create_tensor(tn(LLM_TENSOR_SSM_D, i), {d_inner}, 0);

                        // out_proj
                        layer.ssm_out = create_tensor(tn(LLM_TENSOR_SSM_OUT, "weight", i), {d_inner, n_embd}, 0);
                    }
                } break;
            case LLM_ARCH_XVERSE:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    model.output_norm = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output      = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, 0);

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);

                        layer.wq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "weight", i), {n_embd, n_embd}, 0);
                        layer.wk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);

                        layer.ffn_norm = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);
                        layer.ffn_gate = create_tensor(tn(LLM_TENSOR_FFN_GATE, "weight", i), {n_embd,   n_ff}, 0);
                        layer.ffn_down = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {  n_ff, n_embd}, 0);
                        layer.ffn_up   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd,   n_ff}, 0);
                    }
                } break;
            case LLM_ARCH_COMMAND_R:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    // init output from the input tok embed
                    model.output = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_DUPLICATED);

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);

                        if (n_layer >= 64){
                            layer.attn_q_norm = create_tensor(tn(LLM_TENSOR_ATTN_Q_NORM, "weight", i), {n_embd_head_k, n_head}, 0);
                            layer.attn_k_norm = create_tensor(tn(LLM_TENSOR_ATTN_K_NORM, "weight", i), {n_embd_head_k, n_head_kv}, 0);
                        }

                        layer.wq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "weight", i), {n_embd, n_embd}, 0);
                        layer.wk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);

                        layer.ffn_gate = create_tensor(tn(LLM_TENSOR_FFN_GATE, "weight", i), {n_embd,   n_ff}, 0);
                        layer.ffn_down = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {  n_ff, n_embd}, 0);
                        layer.ffn_up   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd,   n_ff}, 0);
                    }
                } break;
            case LLM_ARCH_OLMO:  // adapted from LLM_ARCH_LLAMA with norm params removed
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output = create_tensor(tn(LLM_TENSOR_OUTPUT, "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_NOT_REQUIRED);
                    // if output is NULL, init from the input tok embed
                    if (model.output == NULL) {
                        model.output = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_DUPLICATED);
                    }

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.wq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "weight", i), {n_embd, n_embd}, 0);
                        layer.wk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);

                        layer.ffn_gate = create_tensor(tn(LLM_TENSOR_FFN_GATE, "weight", i), {n_embd,   n_ff}, 0);
                        layer.ffn_down = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {  n_ff, n_embd}, 0);
                        layer.ffn_up   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd,   n_ff}, 0);
                    }
                } break;
            case LLM_ARCH_OLMO_1124:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output      = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, 0);

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.wq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "weight", i), {n_embd, n_embd}, 0);
                        layer.wk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);
                        layer.attn_q_norm = create_tensor(tn(LLM_TENSOR_ATTN_Q_NORM, "weight", i), {n_embd}, 0);
                        layer.attn_k_norm = create_tensor(tn(LLM_TENSOR_ATTN_K_NORM, "weight", i), {n_embd}, 0);
                        layer.attn_post_norm = create_tensor(tn(LLM_TENSOR_ATTN_POST_NORM, "weight", i), {n_embd}, 0);

                        layer.ffn_gate = create_tensor(tn(LLM_TENSOR_FFN_GATE, "weight", i), {n_embd,   n_ff}, 0);
                        layer.ffn_up   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd,   n_ff}, 0);
                        layer.ffn_down = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {  n_ff, n_embd}, 0);
                        layer.ffn_post_norm = create_tensor(tn(LLM_TENSOR_FFN_POST_NORM, "weight", i), {n_embd}, 0);
                    }
                } break;
            case LLM_ARCH_OLMOE:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output      = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, 0);

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);

                        layer.wq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "weight", i), {n_embd, n_embd}, 0);
                        layer.wk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);
                        layer.attn_q_norm = create_tensor(tn(LLM_TENSOR_ATTN_Q_NORM, "weight", i), {n_embd}, 0);
                        layer.attn_k_norm = create_tensor(tn(LLM_TENSOR_ATTN_K_NORM, "weight", i), {n_embd}, 0);

                        layer.ffn_norm = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);

                        layer.ffn_gate_inp = create_tensor(tn(LLM_TENSOR_FFN_GATE_INP, "weight", i), {n_embd, n_expert}, 0);

                        if (n_expert == 0) {
                            throw std::runtime_error("n_expert must be > 0");
                        }
                        if (n_expert_used == 0) {
                            throw std::runtime_error("n_expert_used must be > 0");
                        }

                        // MoE branch
                        layer.ffn_gate_exps = create_tensor(tn(LLM_TENSOR_FFN_GATE_EXPS, "weight", i), {n_embd, n_ff,   n_expert}, 0);
                        layer.ffn_down_exps = create_tensor(tn(LLM_TENSOR_FFN_DOWN_EXPS, "weight", i), {n_ff,   n_embd, n_expert}, 0);
                        layer.ffn_up_exps   = create_tensor(tn(LLM_TENSOR_FFN_UP_EXPS,   "weight", i), {n_embd, n_ff,   n_expert}, 0);
                    }
                } break;
            case LLM_ARCH_OPENELM:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    // init output from the input tok embed
                    model.output = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_DUPLICATED);

                    for (int i = 0; i < n_layer; ++i) {
                        const int64_t n_head      =   hparams.n_head(i);
                        const int64_t n_head_qkv  = 2*hparams.n_head_kv(i) + n_head;
                        const int64_t n_ff        =   hparams.n_ff(i);

                        auto & layer = model.layers[i];

                        layer.attn_norm = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);

                        layer.wqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "weight", i), {n_embd, n_head_qkv*n_embd_head_k}, 0);
                        layer.attn_q_norm = create_tensor(tn(LLM_TENSOR_ATTN_Q_NORM, "weight", i), {n_embd_head_k}, 0);
                        layer.attn_k_norm = create_tensor(tn(LLM_TENSOR_ATTN_K_NORM, "weight", i), {n_embd_head_k}, 0);
                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_head*n_embd_head_k, n_embd}, 0);

                        layer.ffn_norm = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);
                        layer.ffn_gate = create_tensor(tn(LLM_TENSOR_FFN_GATE, "weight", i), {n_embd, n_ff}, 0);
                        layer.ffn_down = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {n_ff, n_embd}, 0);
                        layer.ffn_up   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd, n_ff}, 0);
                    }
                } break;
            case LLM_ARCH_GPTNEOX:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm   = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output_norm_b = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "bias"),   {n_embd}, 0);
                    model.output        = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, 0);

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm   = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);
                        layer.attn_norm_b = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "bias", i),   {n_embd}, 0);

                        layer.wqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "weight", i), {n_embd, n_embd + 2*n_embd_gqa}, 0);
                        layer.bqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "bias", i),   {n_embd + 2*n_embd_gqa}, 0);

                        layer.wo   = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);
                        layer.bo   = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "bias", i),   {n_embd}, 0);

                        layer.ffn_norm   = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);
                        layer.ffn_norm_b = create_tensor(tn(LLM_TENSOR_FFN_NORM, "bias", i),   {n_embd}, 0);

                        layer.ffn_down   = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {n_ff, n_embd}, 0);
                        layer.ffn_down_b = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "bias", i),   {n_embd}, 0);

                        layer.ffn_up     = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd, n_ff}, 0);
                        layer.ffn_up_b   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "bias", i),   {n_ff}, 0);
                    }
                } break;
            case LLM_ARCH_ARCTIC:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output      = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_NOT_REQUIRED);

                    // if output is NULL, init from the input tok embed
                    if (model.output == NULL) {
                        model.output = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_DUPLICATED);
                    }

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);

                        layer.wq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "weight", i), {n_embd, n_embd}, 0);
                        layer.wk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);

                        layer.ffn_norm = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);

                        layer.ffn_gate = create_tensor(tn(LLM_TENSOR_FFN_GATE, "weight", i), {n_embd, n_embd}, 0);
                        layer.ffn_down = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {n_embd, n_embd}, 0);
                        layer.ffn_up   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd, n_embd}, 0);

                        layer.ffn_gate_inp = create_tensor(tn(LLM_TENSOR_FFN_GATE_INP, "weight", i), {n_embd, n_expert}, 0);
                        layer.ffn_norm_exps = create_tensor(tn(LLM_TENSOR_FFN_NORM_EXPS, "weight", i), {n_embd}, 0);
                        layer.ffn_gate_exps = create_tensor(tn(LLM_TENSOR_FFN_GATE_EXPS, "weight", i), {n_embd,   n_ff, n_expert}, false);
                        layer.ffn_down_exps = create_tensor(tn(LLM_TENSOR_FFN_DOWN_EXPS, "weight", i), {  n_ff, n_embd, n_expert}, 0);
                        layer.ffn_up_exps   = create_tensor(tn(LLM_TENSOR_FFN_UP_EXPS,   "weight", i), {n_embd,   n_ff, n_expert}, 0);
                    }
                } break;
            case LLM_ARCH_DEEPSEEK2:
                {
                    const bool is_lite = (hparams.n_layer == 27);

                    const int64_t n_embd_head_qk_rope = hparams.n_rot;
                    const int64_t n_embd_head_qk_nope = hparams.n_embd_head_k - hparams.n_rot;

                    const int64_t q_lora_rank  = hparams.n_lora_q;
                    const int64_t kv_lora_rank = hparams.n_lora_kv;

                    const int64_t n_ff_exp        = hparams.n_ff_exp;
                    const int64_t n_expert_shared = hparams.n_expert_shared;

                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output      = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, 0);

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);
                        if (!is_lite) {
                            layer.attn_q_a_norm = create_tensor(tn(LLM_TENSOR_ATTN_Q_A_NORM, "weight", i), {q_lora_rank}, 0);
                        }

                        layer.attn_kv_a_norm = create_tensor(tn(LLM_TENSOR_ATTN_KV_A_NORM, "weight", i), {kv_lora_rank}, 0);

                        if (!is_lite) {
                            layer.wq_a = create_tensor(tn(LLM_TENSOR_ATTN_Q_A, "weight", i), {n_embd, q_lora_rank}, 0);
                            layer.wq_b = create_tensor(tn(LLM_TENSOR_ATTN_Q_B, "weight", i), {q_lora_rank, n_head * n_embd_head_k}, 0);
                        } else {
                            layer.wq = create_tensor(tn(LLM_TENSOR_ATTN_Q, "weight", i), {n_embd, n_embd_k_gqa}, 0);
                        }

                        layer.wkv_a_mqa = create_tensor(tn(LLM_TENSOR_ATTN_KV_A_MQA, "weight", i), {n_embd, kv_lora_rank + (n_embd_head_qk_rope)}, 0);
                        layer.wkv_b     = create_tensor(tn(LLM_TENSOR_ATTN_KV_B,     "weight", i), {kv_lora_rank, n_head * (n_embd_head_qk_nope + n_embd_head_v)}, 0);
                        layer.wo        = create_tensor(tn(LLM_TENSOR_ATTN_OUT,      "weight", i), {              n_head * (                      n_embd_head_v), n_embd}, 0);

                        layer.ffn_norm = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);

                        if (i < (int) hparams.n_layer_dense_lead) {
                            layer.ffn_gate = create_tensor(tn(LLM_TENSOR_FFN_GATE, "weight", i), {n_embd,   n_ff}, 0);
                            layer.ffn_down = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {  n_ff, n_embd}, 0);
                            layer.ffn_up   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd,   n_ff}, 0);
                        } else {
                            layer.ffn_gate_inp = create_tensor(tn(LLM_TENSOR_FFN_GATE_INP, "weight", i), {n_embd, n_expert}, 0);

                            if (n_expert == 0) {
                                throw std::runtime_error("n_expert must be > 0");
                            }
                            if (n_expert_used == 0) {
                                throw std::runtime_error("n_expert_used must be > 0");
                            }

                            // MoE branch
                            layer.ffn_gate_exps = create_tensor(tn(LLM_TENSOR_FFN_GATE_EXPS, "weight", i), {  n_embd, n_ff_exp, n_expert}, 0);
                            layer.ffn_down_exps = create_tensor(tn(LLM_TENSOR_FFN_DOWN_EXPS, "weight", i), {n_ff_exp,   n_embd, n_expert}, 0);
                            layer.ffn_up_exps   = create_tensor(tn(LLM_TENSOR_FFN_UP_EXPS,   "weight", i), {  n_embd, n_ff_exp, n_expert}, 0);

                            // Shared expert branch
                            layer.ffn_gate_shexp = create_tensor(tn(LLM_TENSOR_FFN_GATE_SHEXP, "weight", i), {n_embd, n_ff_exp * n_expert_shared}, 0);
                            layer.ffn_down_shexp = create_tensor(tn(LLM_TENSOR_FFN_DOWN_SHEXP, "weight", i), {        n_ff_exp * n_expert_shared, n_embd}, 0);
                            layer.ffn_up_shexp   = create_tensor(tn(LLM_TENSOR_FFN_UP_SHEXP,   "weight", i), {n_embd, n_ff_exp * n_expert_shared}, 0);
                        }
                    }
                } break;
            case LLM_ARCH_BITNET:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm     = create_tensor(tn(LLM_TENSOR_ATTN_NORM,     "weight", i), {n_embd}, 0);
                        layer.attn_sub_norm = create_tensor(tn(LLM_TENSOR_ATTN_SUB_NORM, "weight", i), {n_embd}, 0);

                        layer.wq       = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "weight", i), {n_embd, n_embd}, 0);
                        layer.wq_scale = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "scale",  i), {1}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.wk       = create_tensor(tn(LLM_TENSOR_ATTN_K,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wk_scale = create_tensor(tn(LLM_TENSOR_ATTN_K,   "scale",  i), {1}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.wv       = create_tensor(tn(LLM_TENSOR_ATTN_V,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wv_scale = create_tensor(tn(LLM_TENSOR_ATTN_V,   "scale",  i), {1}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.wo       = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);
                        layer.wo_scale = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "scale",  i), {1}, llama_model_loader::TENSOR_NOT_REQUIRED);

                        layer.ffn_norm     = create_tensor(tn(LLM_TENSOR_FFN_NORM,     "weight", i), {n_embd}, 0);
                        layer.ffn_sub_norm = create_tensor(tn(LLM_TENSOR_FFN_SUB_NORM, "weight", i), {n_ff}, 0);

                        layer.ffn_gate       = create_tensor(tn(LLM_TENSOR_FFN_GATE, "weight", i), {n_embd, n_ff}, 0);
                        layer.ffn_gate_scale = create_tensor(tn(LLM_TENSOR_FFN_GATE, "scale",  i), {1}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.ffn_down       = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {n_ff, n_embd}, 0);
                        layer.ffn_down_scale = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "scale",  i), {1}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.ffn_up         = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd, n_ff}, 0);
                        layer.ffn_up_scale   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "scale",  i), {1}, llama_model_loader::TENSOR_NOT_REQUIRED);
                    }
                } break;
            case LLM_ARCH_T5:
                {
                    const auto n_rel_attn_bkts = hparams.n_rel_attn_bkts;

                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm_enc = create_tensor(tn(LLM_TENSOR_ENC_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output_norm     = create_tensor(tn(LLM_TENSOR_DEC_OUTPUT_NORM, "weight"), {n_embd}, 0);

                    model.output = create_tensor(tn(LLM_TENSOR_OUTPUT, "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_NOT_REQUIRED);
                    // if output is NULL, init from the input tok embed
                    if (model.output == NULL) {
                        model.output = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_DUPLICATED);
                    }

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm_enc  = create_tensor(tn(LLM_TENSOR_ENC_ATTN_NORM,  "weight", i), {n_embd}, 0);
                        layer.attn_rel_b_enc = create_tensor(tn(LLM_TENSOR_ENC_ATTN_REL_B, "weight", i), {n_head, n_rel_attn_bkts}, llama_model_loader::TENSOR_NOT_REQUIRED);

                        layer.wq_enc = create_tensor(tn(LLM_TENSOR_ENC_ATTN_Q,   "weight", i), {n_embd, n_embd_k_gqa}, 0);
                        layer.wk_enc = create_tensor(tn(LLM_TENSOR_ENC_ATTN_K,   "weight", i), {n_embd, n_embd_k_gqa}, 0);
                        layer.wv_enc = create_tensor(tn(LLM_TENSOR_ENC_ATTN_V,   "weight", i), {n_embd, n_embd_v_gqa}, 0);
                        layer.wo_enc = create_tensor(tn(LLM_TENSOR_ENC_ATTN_OUT, "weight", i), {n_embd_v_gqa, n_embd}, 0);

                        layer.ffn_norm_enc = create_tensor(tn(LLM_TENSOR_ENC_FFN_NORM, "weight", i), {n_embd}, 0);
                        layer.ffn_gate_enc = create_tensor(tn(LLM_TENSOR_ENC_FFN_GATE, "weight", i), {n_embd,   n_ff}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.ffn_down_enc = create_tensor(tn(LLM_TENSOR_ENC_FFN_DOWN, "weight", i), {  n_ff, n_embd}, 0);
                        layer.ffn_up_enc   = create_tensor(tn(LLM_TENSOR_ENC_FFN_UP,   "weight", i), {n_embd,   n_ff}, 0);

                        layer.attn_norm  = create_tensor(tn(LLM_TENSOR_DEC_ATTN_NORM,  "weight", i), {n_embd}, 0);
                        layer.attn_rel_b = create_tensor(tn(LLM_TENSOR_DEC_ATTN_REL_B, "weight", i), {n_head, n_rel_attn_bkts}, llama_model_loader::TENSOR_NOT_REQUIRED);

                        layer.wq = create_tensor(tn(LLM_TENSOR_DEC_ATTN_Q,   "weight", i), {n_embd, n_embd_k_gqa}, 0);
                        layer.wk = create_tensor(tn(LLM_TENSOR_DEC_ATTN_K,   "weight", i), {n_embd, n_embd_k_gqa}, 0);
                        layer.wv = create_tensor(tn(LLM_TENSOR_DEC_ATTN_V,   "weight", i), {n_embd, n_embd_v_gqa}, 0);
                        layer.wo = create_tensor(tn(LLM_TENSOR_DEC_ATTN_OUT, "weight", i), {n_embd_v_gqa, n_embd}, 0);

                        layer.attn_norm_cross  = create_tensor(tn(LLM_TENSOR_DEC_CROSS_ATTN_NORM,  "weight", i), {n_embd}, 0);
                        // this tensor seems to be unused in HF transformers implementation
                        layer.attn_rel_b_cross = create_tensor(tn(LLM_TENSOR_DEC_CROSS_ATTN_REL_B, "weight", i), {n_head, n_rel_attn_bkts}, llama_model_loader::TENSOR_NOT_REQUIRED);

                        layer.wq_cross = create_tensor(tn(LLM_TENSOR_DEC_CROSS_ATTN_Q,   "weight", i), {n_embd, n_embd_k_gqa}, 0);
                        layer.wk_cross = create_tensor(tn(LLM_TENSOR_DEC_CROSS_ATTN_K,   "weight", i), {n_embd, n_embd_k_gqa}, 0);
                        layer.wv_cross = create_tensor(tn(LLM_TENSOR_DEC_CROSS_ATTN_V,   "weight", i), {n_embd, n_embd_v_gqa}, 0);
                        layer.wo_cross = create_tensor(tn(LLM_TENSOR_DEC_CROSS_ATTN_OUT, "weight", i), {n_embd_v_gqa, n_embd}, 0);

                        layer.ffn_norm = create_tensor(tn(LLM_TENSOR_DEC_FFN_NORM, "weight", i), {n_embd}, 0);
                        layer.ffn_gate = create_tensor(tn(LLM_TENSOR_DEC_FFN_GATE, "weight", i), {n_embd,   n_ff}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.ffn_down = create_tensor(tn(LLM_TENSOR_DEC_FFN_DOWN, "weight", i), {  n_ff, n_embd}, 0);
                        layer.ffn_up   = create_tensor(tn(LLM_TENSOR_DEC_FFN_UP,   "weight", i), {n_embd,   n_ff}, 0);
                    }
                } break;
            case LLM_ARCH_T5ENCODER:
                {
                    const auto n_rel_attn_bkts = hparams.n_rel_attn_bkts;

                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm_enc = create_tensor(tn(LLM_TENSOR_ENC_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output = create_tensor(tn(LLM_TENSOR_OUTPUT, "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_NOT_REQUIRED);
                    // if output is NULL, init from the input tok embed
                    if (model.output == NULL) {
                        model.output = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_DUPLICATED);
                    }

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm_enc  = create_tensor(tn(LLM_TENSOR_ENC_ATTN_NORM,  "weight", i), {n_embd}, 0);
                        layer.attn_rel_b_enc = create_tensor(tn(LLM_TENSOR_ENC_ATTN_REL_B, "weight", i), {n_head, n_rel_attn_bkts}, llama_model_loader::TENSOR_NOT_REQUIRED);

                        layer.wq_enc = create_tensor(tn(LLM_TENSOR_ENC_ATTN_Q,   "weight", i), {n_embd, n_embd_k_gqa}, 0);
                        layer.wk_enc = create_tensor(tn(LLM_TENSOR_ENC_ATTN_K,   "weight", i), {n_embd, n_embd_k_gqa}, 0);
                        layer.wv_enc = create_tensor(tn(LLM_TENSOR_ENC_ATTN_V,   "weight", i), {n_embd, n_embd_v_gqa}, 0);
                        layer.wo_enc = create_tensor(tn(LLM_TENSOR_ENC_ATTN_OUT, "weight", i), {n_embd_v_gqa, n_embd}, 0);

                        layer.ffn_norm_enc = create_tensor(tn(LLM_TENSOR_ENC_FFN_NORM, "weight", i), {n_embd}, 0);
                        layer.ffn_gate_enc = create_tensor(tn(LLM_TENSOR_ENC_FFN_GATE, "weight", i), {n_embd,   n_ff}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.ffn_down_enc = create_tensor(tn(LLM_TENSOR_ENC_FFN_DOWN, "weight", i), {  n_ff, n_embd}, 0);
                        layer.ffn_up_enc   = create_tensor(tn(LLM_TENSOR_ENC_FFN_UP,   "weight", i), {n_embd,   n_ff}, 0);
                    }
                } break;
            case LLM_ARCH_JAIS:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm   = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output_norm_b = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "bias"),   {n_embd}, 0);
                    model.output        = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, 0);

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm   = create_tensor(tn(LLM_TENSOR_ATTN_NORM,   "weight", i), {n_embd}, 0);
                        layer.attn_norm_b = create_tensor(tn(LLM_TENSOR_ATTN_NORM,   "bias", i),   {n_embd}, 0);

                        layer.wqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "weight", i), {n_embd, n_embd + 2*n_embd_gqa}, 0);
                        layer.bqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "bias", i),   {n_embd + 2*n_embd_gqa}, 0);

                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);
                        layer.bo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "bias", i),   {n_embd}, 0);

                        layer.ffn_norm   = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);
                        layer.ffn_norm_b = create_tensor(tn(LLM_TENSOR_FFN_NORM, "bias", i),   {n_embd}, 0);

                        layer.ffn_down   = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {n_ff, n_embd}, 0);
                        layer.ffn_down_b = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "bias", i),   {n_embd}, 0);

                        layer.ffn_gate   = create_tensor(tn(LLM_TENSOR_FFN_GATE,   "weight", i), {n_embd, n_ff}, 0);
                        layer.ffn_gate_b = create_tensor(tn(LLM_TENSOR_FFN_GATE,   "bias", i),   {n_ff}, 0);

                        layer.ffn_up     = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd, n_ff}, 0);
                        layer.ffn_up_b   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "bias", i),   {n_ff}, 0);
                    }
                } break;
            case LLM_ARCH_CHATGLM:
                {
                    model.tok_embd   = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD,      "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm   = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output        = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, 0);

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);

                        layer.wqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "weight", i), {n_embd, n_embd + 2*n_embd_gqa}, 0);
                        layer.bqkv = create_tensor(tn(LLM_TENSOR_ATTN_QKV, "bias", i),   {n_embd + 2*n_embd_gqa}, 0);

                        layer.wo   = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);

                        layer.ffn_norm   = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);

                        layer.ffn_up     = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd, n_ff * 2}, 0);

                        layer.ffn_down   = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {n_ff, n_embd}, 0);
                    }
                } break;
            case LLM_ARCH_NEMOTRON:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm   = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output_norm_b = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "bias"), {n_embd}, 0);
                    model.output        = create_tensor(tn(LLM_TENSOR_OUTPUT, "weight"), {n_embd, n_vocab}, 0);

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm   = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);
                        layer.attn_norm_b = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "bias", i), {n_embd}, 0);

                        layer.wq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "weight", i), {n_embd, n_embd}, 0);
                        layer.wk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);

                        // optional bias tensors
                        layer.bq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "bias", i), {n_embd},     llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.bk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "bias", i), {n_embd_gqa}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.bv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "bias", i), {n_embd_gqa}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.bo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "bias", i), {n_embd},     llama_model_loader::TENSOR_NOT_REQUIRED);

                        layer.ffn_norm = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);
                        layer.ffn_norm_b = create_tensor(tn(LLM_TENSOR_FFN_NORM, "bias", i), {n_embd}, 0);

                        layer.ffn_down = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {  n_ff, n_embd}, 0);
                        layer.ffn_up   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd,   n_ff}, 0);

                        // optional MLP bias
                        layer.ffn_down_b = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "bias", i), {n_embd}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.ffn_up_b   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "bias", i), {n_ff}, llama_model_loader::TENSOR_NOT_REQUIRED);
                    }
                } break;
            case LLM_ARCH_EXAONE:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // output
                    model.output_norm = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output      = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, 0);

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);

                        layer.wq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "weight", i), {n_embd, n_embd_head_k * n_head}, 0);
                        layer.wk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "weight", i), {n_embd, n_embd_k_gqa}, 0);
                        layer.wv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "weight", i), {n_embd, n_embd_v_gqa}, 0);
                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd_head_k * n_head, n_embd}, 0);

                        layer.ffn_norm   = create_tensor(tn(LLM_TENSOR_FFN_NORM,   "weight", i), {n_embd}, 0);
                        layer.rope_freqs = create_tensor(tn(LLM_TENSOR_ROPE_FREQS, "weight", i), {n_rot/2}, llama_model_loader::TENSOR_NOT_REQUIRED | (i != 0 ? llama_model_loader::TENSOR_DUPLICATED : 0));
                        layer.ffn_gate   = create_tensor(tn(LLM_TENSOR_FFN_GATE,   "weight", i), {n_embd,   n_ff}, 0);
                        layer.ffn_down   = create_tensor(tn(LLM_TENSOR_FFN_DOWN,   "weight", i), {  n_ff, n_embd}, 0);
                        layer.ffn_up     = create_tensor(tn(LLM_TENSOR_FFN_UP,     "weight", i), {n_embd,   n_ff}, 0);
                    }
                } break;
            case LLM_ARCH_RWKV6:
                {
                    model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                    // Block 0, LN0
                    model.tok_norm = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD_NORM, "weight"), {n_embd}, 0);
                    model.tok_norm_b = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD_NORM, "bias"), {n_embd}, 0);

                    // output
                    model.output_norm = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output_norm_b = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "bias"), {n_embd}, 0);
                    model.output = create_tensor(tn(LLM_TENSOR_OUTPUT, "weight"), {n_embd, n_vocab}, 0);

                    const int time_mix_extra_dim = hparams.time_mix_extra_dim;
                    const int time_decay_extra_dim = hparams.time_decay_extra_dim;
                    const int head_size = hparams.wkv_head_size;
                    const int attn_hidden_size = n_embd;
                    const int ffn_size = hparams.n_ff_arr[0];

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm   = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);
                        layer.attn_norm_b = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "bias", i),   {n_embd}, 0);

                        layer.attn_norm_2   = create_tensor(tn(LLM_TENSOR_ATTN_NORM_2, "weight", i), {n_embd}, 0);
                        layer.attn_norm_2_b = create_tensor(tn(LLM_TENSOR_ATTN_NORM_2, "bias", i),   {n_embd}, 0);

                        layer.time_mix_w1 = create_tensor(tn(LLM_TENSOR_TIME_MIX_W1, "weight", i), {n_embd, time_mix_extra_dim * 5}, 0);
                        layer.time_mix_w2 = create_tensor(tn(LLM_TENSOR_TIME_MIX_W2, "weight", i), {time_mix_extra_dim, n_embd, 5}, 0);

                        layer.time_mix_lerp_x = create_tensor(tn(LLM_TENSOR_TIME_MIX_LERP_X, "weight", i), {n_embd, 1, 1}, 0);
                        layer.time_mix_lerp_w = create_tensor(tn(LLM_TENSOR_TIME_MIX_LERP_W, "weight", i), {n_embd, 1, 1}, 0);
                        layer.time_mix_lerp_k = create_tensor(tn(LLM_TENSOR_TIME_MIX_LERP_K, "weight", i), {n_embd, 1, 1}, 0);
                        layer.time_mix_lerp_v = create_tensor(tn(LLM_TENSOR_TIME_MIX_LERP_V, "weight", i), {n_embd, 1, 1}, 0);
                        layer.time_mix_lerp_r = create_tensor(tn(LLM_TENSOR_TIME_MIX_LERP_R, "weight", i), {n_embd, 1, 1}, 0);
                        layer.time_mix_lerp_g = create_tensor(tn(LLM_TENSOR_TIME_MIX_LERP_G, "weight", i), {n_embd, 1, 1}, 0);

                        layer.time_mix_first = create_tensor(tn(LLM_TENSOR_TIME_MIX_FIRST, "weight", i), {head_size, n_embd / head_size}, 0);
                        layer.time_mix_decay = create_tensor(tn(LLM_TENSOR_TIME_MIX_DECAY, "weight", i), {n_embd}, 0);
                        layer.time_mix_decay_w1 = create_tensor(tn(LLM_TENSOR_TIME_MIX_DECAY_W1, "weight", i), {n_embd, time_decay_extra_dim}, 0);
                        layer.time_mix_decay_w2 = create_tensor(tn(LLM_TENSOR_TIME_MIX_DECAY_W2, "weight", i), {time_decay_extra_dim, attn_hidden_size}, 0);
                        layer.time_mix_key = create_tensor(tn(LLM_TENSOR_TIME_MIX_KEY, "weight", i), {attn_hidden_size, n_embd}, 0);
                        layer.time_mix_value = create_tensor(tn(LLM_TENSOR_TIME_MIX_VALUE, "weight", i), {attn_hidden_size, n_embd}, 0);
                        layer.time_mix_receptance = create_tensor(tn(LLM_TENSOR_TIME_MIX_RECEPTANCE, "weight", i), {attn_hidden_size, n_embd}, 0);
                        layer.time_mix_gate = create_tensor(tn(LLM_TENSOR_TIME_MIX_GATE, "weight", i), {attn_hidden_size, n_embd}, 0);

                        layer.time_mix_ln = create_tensor(tn(LLM_TENSOR_TIME_MIX_LN, "weight", i), {n_embd}, 0);
                        layer.time_mix_ln_b = create_tensor(tn(LLM_TENSOR_TIME_MIX_LN, "bias", i), {n_embd}, 0);
                        layer.time_mix_output = create_tensor(tn(LLM_TENSOR_TIME_MIX_OUTPUT, "weight", i), {n_embd, attn_hidden_size}, 0);

                        layer.channel_mix_lerp_k = create_tensor(tn(LLM_TENSOR_CHANNEL_MIX_LERP_K, "weight", i), {n_embd, 1, 1}, 0);
                        layer.channel_mix_lerp_r = create_tensor(tn(LLM_TENSOR_CHANNEL_MIX_LERP_R, "weight", i), {n_embd, 1, 1}, 0);

                        layer.channel_mix_key = create_tensor(tn(LLM_TENSOR_CHANNEL_MIX_KEY, "weight", i), {n_embd, ffn_size}, 0);
                        layer.channel_mix_value = create_tensor(tn(LLM_TENSOR_CHANNEL_MIX_VALUE, "weight", i), {ffn_size, n_embd}, 0);
                        layer.channel_mix_receptance = create_tensor(tn(LLM_TENSOR_CHANNEL_MIX_RECEPTANCE, "weight", i), {n_embd, n_embd}, 0);
                    }

                } break;
            case LLM_ARCH_CHAMELEON:
                {
                 model.tok_embd = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, 0);

                 // output
                    model.output_norm = create_tensor(tn(LLM_TENSOR_OUTPUT_NORM, "weight"), {n_embd}, 0);
                    model.output      = create_tensor(tn(LLM_TENSOR_OUTPUT,      "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_NOT_REQUIRED);
                    // if output is NULL, init from the input tok embed
                    if (model.output == NULL) {
                        model.output = create_tensor(tn(LLM_TENSOR_TOKEN_EMBD, "weight"), {n_embd, n_vocab}, llama_model_loader::TENSOR_DUPLICATED);
                    }

                    for (int i = 0; i < n_layer; ++i) {
                        auto & layer = model.layers[i];

                        layer.attn_norm = create_tensor(tn(LLM_TENSOR_ATTN_NORM, "weight", i), {n_embd}, 0);
                        layer.attn_q_norm = create_tensor(tn(LLM_TENSOR_ATTN_Q_NORM, "weight", i), {n_embd_head_k, n_head}, 0);
                        layer.attn_k_norm = create_tensor(tn(LLM_TENSOR_ATTN_K_NORM, "weight", i), {n_embd_head_k, n_head_kv}, 0);
                        layer.attn_q_norm_b = create_tensor(tn(LLM_TENSOR_ATTN_Q_NORM, "bias", i),  {n_embd_head_k, n_head}, llama_model_loader::TENSOR_NOT_REQUIRED);
                        layer.attn_k_norm_b = create_tensor(tn(LLM_TENSOR_ATTN_K_NORM, "bias", i),  {n_embd_head_k, n_head_kv}, llama_model_loader::TENSOR_NOT_REQUIRED);

                        layer.wq = create_tensor(tn(LLM_TENSOR_ATTN_Q,   "weight", i), {n_embd, n_embd}, 0);
                        layer.wk = create_tensor(tn(LLM_TENSOR_ATTN_K,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wv = create_tensor(tn(LLM_TENSOR_ATTN_V,   "weight", i), {n_embd, n_embd_gqa}, 0);
                        layer.wo = create_tensor(tn(LLM_TENSOR_ATTN_OUT, "weight", i), {n_embd, n_embd}, 0);

                        layer.ffn_norm = create_tensor(tn(LLM_TENSOR_FFN_NORM, "weight", i), {n_embd}, 0);

                        layer.ffn_gate = create_tensor(tn(LLM_TENSOR_FFN_GATE, "weight", i), {n_embd,   n_ff}, 0);
                        layer.ffn_down = create_tensor(tn(LLM_TENSOR_FFN_DOWN, "weight", i), {  n_ff, n_embd}, 0);
                        layer.ffn_up   = create_tensor(tn(LLM_TENSOR_FFN_UP,   "weight", i), {n_embd,   n_ff}, 0);
                    }
                } break;
            default:
                throw std::runtime_error("unknown architecture");
        }

        if (n_moved_tensors > 0) {
            LLAMA_LOG_DEBUG("%s: tensor '%s' (%s) (and %d others) cannot be used with preferred buffer type %s, using %s instead\n",
                __func__, first_moved_tensor->name, lm_ggml_type_name(first_moved_tensor->type), n_moved_tensors - 1,
                lm_ggml_backend_buft_name(first_moved_from_buft), lm_ggml_backend_buft_name(first_moved_to_buft));
        }
    }

    ml.done_getting_tensors();

    ml.init_mappings(true, use_mlock ? &model.mlock_mmaps : nullptr);
    model.mappings.reserve(ml.mappings.size());

    // create the backend buffers
    std::vector<std::pair<lm_ggml_context *, llama_buf_map>> ctx_bufs;
    ctx_bufs.reserve(ctx_map.size());

    // Ensure we have enough capacity for the maximum backend buffer we will potentially create
    const size_t n_max_backend_buffer = ctx_map.size() * ml.files.size();
    model.bufs.reserve(n_max_backend_buffer);

    for (auto & it : ctx_map) {
        lm_ggml_backend_buffer_type_t buft = it.first;
        lm_ggml_context * ctx              = it.second;

        // skip contexts without tensors
        if (lm_ggml_get_first_tensor(ctx) == nullptr) {
            continue;
        }

        llama_buf_map bufs;
        bufs.reserve(n_max_backend_buffer);

        // check if it is possible to use buffer_from_host_ptr with this buffer type
        lm_ggml_backend_dev_t dev = lm_ggml_backend_buft_get_device(buft);
        if (!dev) {
            // FIXME: workaround for CPU backend buft having a NULL device
            dev = lm_ggml_backend_reg_dev_get(lm_ggml_backend_cpu_reg(), 0);
        }
        lm_ggml_backend_dev_props props;
        lm_ggml_backend_dev_get_props(dev, &props);
        bool buffer_from_host_ptr_supported = props.caps.buffer_from_host_ptr;
        bool is_default_buft = buft == lm_ggml_backend_dev_buffer_type(dev);

        if (ml.use_mmap && use_mmap_buffer && buffer_from_host_ptr_supported && is_default_buft) {
            for (uint32_t idx = 0; idx < ml.files.size(); idx++) {
                // only the mmap region containing the tensors in the model is mapped to the backend buffer
                // this is important for metal with apple silicon: if the entire model could be mapped to a metal buffer, then we could just use metal for all layers
                // this allows using partial offloading when the model size exceeds the metal buffer size, but not the RAM size
                void * addr = nullptr;
                size_t first, last; // NOLINT
                ml.get_mapping_range(&first, &last, &addr, idx, ctx);
                if (first >= last) {
                    continue;
                }
                const size_t max_size = lm_ggml_get_max_tensor_size(ctx);
                lm_ggml_backend_buffer_t buf = lm_ggml_backend_dev_buffer_from_host_ptr(dev, (char *) addr + first, last - first, max_size);
                if (buf == nullptr) {
                    throw std::runtime_error(format("unable to allocate %s buffer", lm_ggml_backend_buft_name(buft)));
                }
                model.bufs.emplace_back(buf);
                bufs.emplace(idx, buf);
            }
        }
        else {
            lm_ggml_backend_buffer_t buf = lm_ggml_backend_alloc_ctx_tensors_from_buft(ctx, buft);
            if (buf == nullptr) {
                throw std::runtime_error(format("unable to allocate %s buffer", lm_ggml_backend_buft_name(buft)));
            }
            model.bufs.emplace_back(buf);
            if (use_mlock && lm_ggml_backend_buffer_is_host(buf)) {
                model.mlock_bufs.emplace_back(new llama_mlock);
                auto & mlock_buf = model.mlock_bufs.back();
                mlock_buf->init   (lm_ggml_backend_buffer_get_base(buf));
                mlock_buf->grow_to(lm_ggml_backend_buffer_get_size(buf));
            }
            for (uint32_t idx = 0; idx < ml.files.size(); idx++) {
                bufs.emplace(idx, buf);
            }
        }

        if (bufs.empty()) {
            throw std::runtime_error("failed to allocate buffer");
        }

        for (auto & buf : bufs) {
            // indicate that this buffer contains weights
            // this is used by lm_ggml_backend_sched to improve op scheduling: ops that use a weight are preferably scheduled to the backend that contains the weight
            lm_ggml_backend_buffer_set_usage(buf.second, LM_GGML_BACKEND_BUFFER_USAGE_WEIGHTS);
        }

        ctx_bufs.emplace_back(ctx, bufs);
    }

    if (llama_supports_gpu_offload()) {
        const int n_gpu = std::min(n_gpu_layers, int(hparams.n_layer));

        LLAMA_LOG_INFO("%s: offloading %d repeating layers to GPU\n", __func__, n_gpu);
        if (n_gpu_layers > (int) hparams.n_layer) {
            LLAMA_LOG_INFO("%s: offloading output layer to GPU\n", __func__);
        }

        const int max_backend_supported_layers = hparams.n_layer + 1;
        const int max_offloadable_layers       = hparams.n_layer + 1;

        LLAMA_LOG_INFO("%s: offloaded %d/%d layers to GPU\n", __func__, std::min(n_gpu_layers, max_offloadable_layers), max_backend_supported_layers);
    }

    // print memory requirements per buffer type
    for (auto & buf : model.bufs) {
        LLAMA_LOG_INFO("%s: %12s model buffer size = %8.2f MiB\n", __func__, lm_ggml_backend_buffer_name(buf.get()), lm_ggml_backend_buffer_get_size(buf.get()) / 1024.0 / 1024.0);
    }

    // populate tensors_by_name
    for (auto & ctx : model.ctxs) {
        for (auto * cur = lm_ggml_get_first_tensor(ctx.get()); cur != NULL; cur = lm_ggml_get_next_tensor(ctx.get(), cur)) {
            model.tensors_by_name.emplace_back(lm_ggml_get_name(cur), cur);
        }
    }

    // load tensor data
    for (auto & it : ctx_bufs) {
        lm_ggml_context * ctx = it.first;
        auto & bufs = it.second;
        if (!ml.load_all_data(ctx, bufs, use_mlock ? &model.mlock_mmaps : NULL, progress_callback, progress_callback_user_data)) {
            return false;
        }
    }

    if (use_mmap_buffer) {
        for (auto & mapping : ml.mappings) {
            model.mappings.emplace_back(std::move(mapping));
        }
    }

    return true;
}

// Returns 0 on success, -1 on error, and -2 on cancellation via llama_progress_callback
static int llama_model_load(const std::string & fname, llama_model & model, llama_model_params & params) {
    model.t_start_us = lm_ggml_time_us();

    try {
        llama_model_loader ml(fname, params.use_mmap, params.check_tensors, params.kv_overrides);

        model.hparams.vocab_only = params.vocab_only;

        try {
            llm_load_arch(ml, model);
        } catch(const std::exception & e) {
            throw std::runtime_error("error loading model architecture: " + std::string(e.what()));
        }
        try {
            llm_load_hparams(ml, model);
        } catch(const std::exception & e) {
            throw std::runtime_error("error loading model hyperparameters: " + std::string(e.what()));
        }
        try {
            llm_load_vocab(ml, model);
        } catch(const std::exception & e) {
            throw std::runtime_error("error loading model vocabulary: " + std::string(e.what()));
        }

        llm_load_stats(ml, model);
        llm_load_print_meta(ml, model);

        if (model.vocab.type != LLAMA_VOCAB_TYPE_NONE &&
            model.hparams.n_vocab != model.vocab.id_to_token.size()) {
            throw std::runtime_error("vocab size mismatch");
        }

        if (params.vocab_only) {
            LLAMA_LOG_INFO("%s: vocab only - skipping tensors\n", __func__);
            return 0;
        }

        if (!llm_load_tensors(
            ml, model, params.n_gpu_layers, params.split_mode,  params.main_gpu, params.tensor_split, params.use_mlock,
            params.progress_callback, params.progress_callback_user_data
        )) {
            return -2;
        }
    } catch (const std::exception & err) {
        LLAMA_LOG_ERROR("%s: error loading model: %s\n", __func__, err.what());
        return -1;
    }

    // loading time will be recalculate after the first eval, so
    // we take page faults deferred by mmap() into consideration
    model.t_load_us = lm_ggml_time_us() - model.t_start_us;

    return 0;
}

//
// llm_build
//

using llm_build_cb = std::function<void(struct lm_ggml_tensor * cur, const char * name, int nl)>;

enum llm_ffn_op_type {
    LLM_FFN_SILU,
    LLM_FFN_GELU,
    LLM_FFN_RELU,
    LLM_FFN_RELU_SQR,
    LLM_FFN_SWIGLU,
};

enum llm_ffn_gate_type {
    LLM_FFN_SEQ,
    LLM_FFN_PAR, // ffn_gate is parallel to ffn_up
};

enum llm_norm_type {
    LLM_NORM,
    LLM_NORM_RMS,
};

static struct lm_ggml_tensor * llm_build_inp_embd(
        struct lm_ggml_context * ctx,
       struct llama_context & lctx,
        const llama_hparams & hparams,
         const llama_ubatch & batch,
         struct lm_ggml_tensor * tok_embd,
         const llm_build_cb & cb) {
    const int64_t n_embd = hparams.n_embd;

    struct lm_ggml_tensor * inpL;

    if (batch.token) {
        lctx.inp_tokens = lm_ggml_new_tensor_1d(ctx, LM_GGML_TYPE_I32, batch.n_tokens);
        cb(lctx.inp_tokens, "inp_tokens", -1);
        lm_ggml_set_input(lctx.inp_tokens);

        inpL = lm_ggml_get_rows(ctx, tok_embd, lctx.inp_tokens);
    } else {
       lctx.inp_embd = lm_ggml_new_tensor_2d(ctx, LM_GGML_TYPE_F32, n_embd, batch.n_tokens);
        inpL = lctx.inp_embd;
        lm_ggml_set_input(lctx.inp_embd);
    }

    // For Granite architecture
    if (hparams.f_embedding_scale != 0.0f) {
        inpL = lm_ggml_scale(ctx, inpL, hparams.f_embedding_scale);
    }

    cb(inpL, "inp_embd", -1);

    return inpL;
}

static void llm_build_kv_store(
        struct lm_ggml_context * ctx,
        const llama_hparams & hparams,
        const llama_cparams & cparams,
       const llama_kv_cache & kv,
         struct lm_ggml_cgraph * graph,
         struct lm_ggml_tensor * k_cur,
         struct lm_ggml_tensor * v_cur,
                    int32_t   n_tokens,
                    int32_t   kv_head,
         const llm_build_cb & cb,
                    int64_t   il) {
    const int64_t n_ctx = cparams.n_ctx;

    const int64_t n_embd_k_gqa = hparams.n_embd_k_gqa(il);
    const int64_t n_embd_v_gqa = hparams.n_embd_v_gqa(il);

    LM_GGML_ASSERT(kv.size == n_ctx);

    struct lm_ggml_tensor * k_cache_view = lm_ggml_view_1d(ctx, kv.k_l[il], n_tokens*n_embd_k_gqa, lm_ggml_row_size(kv.k_l[il]->type, n_embd_k_gqa)*kv_head);
    cb(k_cache_view, "k_cache_view", il);

    // note: storing RoPE-ed version of K in the KV cache
    lm_ggml_build_forward_expand(graph, lm_ggml_cpy(ctx, k_cur, k_cache_view));

    assert(v_cur->ne[0] == n_embd_v_gqa && v_cur->ne[1] == n_tokens);

    struct lm_ggml_tensor * v_cache_view = nullptr;

    if (cparams.flash_attn) {
        v_cache_view = lm_ggml_view_1d(ctx, kv.v_l[il], n_tokens*n_embd_v_gqa, lm_ggml_row_size(kv.v_l[il]->type, n_embd_v_gqa)*kv_head);
    } else {
        // note: the V cache is transposed when not using flash attention
        v_cache_view = lm_ggml_view_2d(ctx, kv.v_l[il], n_tokens, n_embd_v_gqa,
                (  n_ctx)*lm_ggml_element_size(kv.v_l[il]),
                (kv_head)*lm_ggml_element_size(kv.v_l[il]));

        v_cur = lm_ggml_transpose(ctx, v_cur);
    }
    cb(v_cache_view, "v_cache_view", il);

    lm_ggml_build_forward_expand(graph, lm_ggml_cpy(ctx, v_cur, v_cache_view));
}

// do mat_mul, while optionally apply lora
static struct lm_ggml_tensor * llm_build_lora_mm(
        struct llama_context & lctx,
         struct lm_ggml_context * ctx0,
          struct lm_ggml_tensor * w,
          struct lm_ggml_tensor * cur) {
    struct lm_ggml_tensor * res = lm_ggml_mul_mat(ctx0, w, cur);
    for (auto & it : lctx.lora_adapters) {
        struct llama_lora_weight * lora = it.first->get_weight(w);
        if (lora == nullptr) {
            continue;
        }
        const float alpha = it.first->alpha;
        const float rank  = (float) lora->b->ne[0];
        const float scale = alpha ? it.second * alpha / rank : it.second;
        struct lm_ggml_tensor * ab_cur = lm_ggml_mul_mat(
            ctx0, lora->b,
            lm_ggml_mul_mat(ctx0, lora->a, cur)
        );
        ab_cur = lm_ggml_scale(ctx0, ab_cur, scale);
        res = lm_ggml_add(ctx0, res, ab_cur);
    }
    return res;
}

// do mat_mul_id, while optionally apply lora
static struct lm_ggml_tensor * llm_build_lora_mm_id(
        struct llama_context & lctx,
         struct lm_ggml_context * ctx0,
          struct lm_ggml_tensor * w,   // struct lm_ggml_tensor * as
          struct lm_ggml_tensor * cur, // struct lm_ggml_tensor * b
          struct lm_ggml_tensor * ids) {
    struct lm_ggml_tensor * res = lm_ggml_mul_mat_id(ctx0, w, cur, ids);
    for (auto & it : lctx.lora_adapters) {
        struct llama_lora_weight * lora = it.first->get_weight(w);
        if (lora == nullptr) {
            continue;
        }
        const float alpha = it.first->alpha;
        const float rank  = (float) lora->b->ne[0];
        const float scale = alpha ? it.second * alpha / rank : it.second;
        struct lm_ggml_tensor * ab_cur = lm_ggml_mul_mat_id(
            ctx0, lora->b,
            lm_ggml_mul_mat_id(ctx0, lora->a, cur, ids),
            ids
        );
        ab_cur = lm_ggml_scale(ctx0, ab_cur, scale);
        res = lm_ggml_add(ctx0, res, ab_cur);
    }
    return res;
}

static struct lm_ggml_tensor * llm_build_norm(
        struct lm_ggml_context * ctx,
         struct lm_ggml_tensor * cur,
        const llama_hparams & hparams,
         struct lm_ggml_tensor * mw,
         struct lm_ggml_tensor * mb,
              llm_norm_type   type,
         const llm_build_cb & cb,
                        int   il) {
    switch (type) {
        case LLM_NORM:     cur = lm_ggml_norm    (ctx, cur, hparams.f_norm_eps);     break;
        case LLM_NORM_RMS: cur = lm_ggml_rms_norm(ctx, cur, hparams.f_norm_rms_eps); break;
    }

    if (mw || mb) {
        cb(cur, "norm", il);
    }

    if (mw) {
        cur = lm_ggml_mul(ctx, cur, mw);
        if (mb) {
            cb(cur, "norm_w", il);
        }
    }

    if (mb) {
        cur = lm_ggml_add(ctx, cur, mb);
    }

    return cur;
}

static struct lm_ggml_tensor * llm_build_ffn(
        struct lm_ggml_context * ctx,
       struct llama_context & lctx,
         struct lm_ggml_tensor * cur,
         struct lm_ggml_tensor * up,
         struct lm_ggml_tensor * up_b,
         struct lm_ggml_tensor * up_s,
         struct lm_ggml_tensor * gate,
         struct lm_ggml_tensor * gate_b,
         struct lm_ggml_tensor * gate_s,
         struct lm_ggml_tensor * down,
         struct lm_ggml_tensor * down_b,
         struct lm_ggml_tensor * down_s,
         struct lm_ggml_tensor * act_scales,
            llm_ffn_op_type   type_op,
          llm_ffn_gate_type   type_gate,
         const llm_build_cb & cb,
                        int   il) {
    struct lm_ggml_tensor * tmp = up ? llm_build_lora_mm(lctx, ctx, up, cur) : cur;
    cb(tmp, "ffn_up", il);

    if (up_b) {
        tmp = lm_ggml_add(ctx, tmp, up_b);
        cb(tmp, "ffn_up_b", il);
    }

    if (up_s) {
        tmp = lm_ggml_mul(ctx, tmp, up_s);
        cb(tmp, "ffn_up_s", il);
    }

    if (gate) {
        switch (type_gate) {
            case LLM_FFN_SEQ:
                {
                    cur = llm_build_lora_mm(lctx, ctx, gate, tmp);
                    cb(cur, "ffn_gate", il);
                } break;
            case LLM_FFN_PAR:
                {
                    cur = llm_build_lora_mm(lctx, ctx, gate, cur);
                    cb(cur, "ffn_gate", il);
                } break;
        }

        if (gate_b) {
            cur = lm_ggml_add(ctx, cur, gate_b);
            cb(cur, "ffn_gate_b", il);
        }

        if (gate_s) {
            cur = lm_ggml_mul(ctx, cur, gate_s);
            cb(cur, "ffn_gate_s", il);
        }

    } else {
        cur = tmp;
    }

    switch (type_op) {
        case LLM_FFN_SILU:
            {
                cur = lm_ggml_silu(ctx, cur);
                cb(cur, "ffn_silu", il);
            } break;
        case LLM_FFN_GELU:
            {
                cur = lm_ggml_gelu(ctx, cur);
                cb(cur, "ffn_gelu", il);
                if (act_scales != NULL) {
                    cur = lm_ggml_div(ctx, cur, act_scales);
                    cb(cur, "ffn_act", il);
                }
            } break;
        case LLM_FFN_RELU:
            {
                cur = lm_ggml_relu(ctx, cur);
                cb(cur, "ffn_relu", il);
            } break;
        case LLM_FFN_RELU_SQR:
            {
                cur = lm_ggml_relu(ctx, cur);
                cb(cur, "ffn_relu", il);

                cur = lm_ggml_sqr(ctx, cur);
                cb(cur, "ffn_sqr(relu)", il);
            } break;
        case LLM_FFN_SWIGLU:
            {
                // Project to 4h. If using swiglu double the output width, see https://arxiv.org/pdf/2002.05202.pdf
                int64_t split_point = cur->ne[0] / 2;
                struct lm_ggml_tensor * x0 = lm_ggml_cont(ctx, lm_ggml_view_2d(ctx, cur, split_point, cur->ne[1], cur->nb[1], 0));
                struct lm_ggml_tensor * x1 = lm_ggml_cont(ctx, lm_ggml_view_2d(ctx, cur, split_point, cur->ne[1], cur->nb[1], split_point * lm_ggml_element_size(cur)));

                x0 = lm_ggml_silu(ctx, x0);
                cb(cur, "ffn_silu", il);

                cur = lm_ggml_mul(ctx, x0, x1);
                cb(cur, "ffn_mul", il);
            } break;
    }

    if (type_gate == LLM_FFN_PAR) {
        cur = lm_ggml_mul(ctx, cur, tmp);
        cb(cur, "ffn_gate_par", il);
    }

    if (down) {
        cur = llm_build_lora_mm(lctx, ctx, down, cur);
    }

    if (down_b) {
        cb(cur, "ffn_down", il);
    }

    if (down_b) {
        cur = lm_ggml_add(ctx, cur, down_b);
    }

    if (down_s) {
        cur = lm_ggml_mul(ctx, cur, down_s);
        cb(cur, "ffn_down_s", il);
    }

    return cur;
}

static struct lm_ggml_tensor * llm_build_moe_ffn(
        struct lm_ggml_context * ctx,
       struct llama_context & lctx,
         struct lm_ggml_tensor * cur,
         struct lm_ggml_tensor * gate_inp,
         struct lm_ggml_tensor * up_exps,
         struct lm_ggml_tensor * gate_exps,
         struct lm_ggml_tensor * down_exps,
                    int64_t   n_expert,
                    int64_t   n_expert_used,
            llm_ffn_op_type   type_op,
                       bool   norm_w,
                       bool   scale_w,
                      float   w_scale,
         const llm_build_cb & cb,
                        int   il) {
    int64_t n_embd = cur->ne[0];
    int64_t n_tokens = cur->ne[1];

    lm_ggml_tensor * logits = llm_build_lora_mm(lctx, ctx, gate_inp, cur); // [n_expert, n_tokens]
    cb(logits, "ffn_moe_logits", il);

    lm_ggml_tensor * probs = lm_ggml_soft_max(ctx, logits); // [n_expert, n_tokens]
    cb(probs, "ffn_moe_probs", il);

    // select experts
    lm_ggml_tensor * selected_experts = lm_ggml_top_k(ctx, probs, n_expert_used); // [n_expert_used, n_tokens]
    cb(selected_experts->src[0], "ffn_moe_argsort", il);
    cb(selected_experts, "ffn_moe_topk", il);

    lm_ggml_tensor * weights = lm_ggml_get_rows(ctx,
            lm_ggml_reshape_3d(ctx, probs, 1, n_expert, n_tokens), selected_experts); // [1, n_expert_used, n_tokens]
    cb(weights, "ffn_moe_weights", il);

    if (norm_w) {
        weights = lm_ggml_reshape_2d(ctx, weights, n_expert_used, n_tokens);

        lm_ggml_tensor * weights_sum = lm_ggml_sum_rows(ctx, weights); // [1, n_tokens]
        cb(weights_sum, "ffn_moe_weights_sum", il);

        weights = lm_ggml_div(ctx, weights, weights_sum); // [n_expert_used, n_tokens]
        cb(weights, "ffn_moe_weights_norm", il);

        weights = lm_ggml_reshape_3d(ctx, weights, 1, n_expert_used, n_tokens);
    }
    if (scale_w) {
        weights = lm_ggml_scale(ctx, weights, w_scale);
        cb(weights, "ffn_moe_weights_scaled", il);
    }

    cur = lm_ggml_reshape_3d(ctx, cur, n_embd, 1, n_tokens);
    lm_ggml_tensor * up = llm_build_lora_mm_id(lctx, ctx, up_exps, cur, selected_experts); // [n_ff, n_expert_used, n_tokens]
    cb(up, "ffn_moe_up", il);

    lm_ggml_tensor * gate = llm_build_lora_mm_id(lctx, ctx, gate_exps, cur, selected_experts); // [n_ff, n_expert_used, n_tokens]
    cb(gate, "ffn_moe_gate", il);

    switch (type_op) {
        case LLM_FFN_SILU:
            {
                gate = lm_ggml_silu(ctx, gate);
                cb(gate, "ffn_moe_silu", il);
            } break;
        case LLM_FFN_GELU:
            {
                gate = lm_ggml_gelu(ctx, gate);
                cb(gate, "ffn_moe_gelu", il);
            } break;
        default:
            LM_GGML_ABORT("fatal error");
    }

    lm_ggml_tensor * par = lm_ggml_mul(ctx, up, gate); // [n_ff, n_expert_used, n_tokens]
    cb(par, "ffn_moe_gate_par", il);

    lm_ggml_tensor * experts = llm_build_lora_mm_id(lctx, ctx, down_exps, par, selected_experts); // [n_embd, n_expert_used, n_tokens]
    cb(experts, "ffn_moe_down", il);

    experts = lm_ggml_mul(ctx, experts, weights);

    // aggregate experts
    lm_ggml_tensor * moe_out = nullptr;
    for (int i = 0; i < n_expert_used; ++i) {
        lm_ggml_tensor * cur_expert = lm_ggml_view_2d(ctx, experts, n_embd, n_tokens,
                experts->nb[2], i*experts->nb[1]);

        if (i == 0) {
            moe_out = cur_expert;
        } else {
            moe_out = lm_ggml_add(ctx, moe_out, cur_expert);
        }
    }

    if (n_expert_used == 1) {
        // avoid returning a non-contiguous tensor
        moe_out = lm_ggml_cont(ctx, moe_out);
    }

    return moe_out;
}

static struct lm_ggml_tensor * llm_build_kqv(
        struct lm_ggml_context * ctx,
       struct llama_context & lctx,
       const llama_kv_cache & kv,
         struct lm_ggml_cgraph * graph,
         struct lm_ggml_tensor * wo,
         struct lm_ggml_tensor * wo_b,
         struct lm_ggml_tensor * q_cur,
         struct lm_ggml_tensor * kq_mask,
                    int32_t   n_tokens,
                    int32_t   n_kv,
                    float     kq_scale,
         const llm_build_cb & cb,
                    int       il) {
    const llama_model   & model   = lctx.model;
    const llama_hparams & hparams = lctx.model.hparams;
    const llama_cparams & cparams = lctx.cparams;

    const int64_t n_ctx         = cparams.n_ctx;
    const int64_t n_head        = hparams.n_head(il);
    const int64_t n_head_kv     = hparams.n_head_kv(il);
    const int64_t n_embd_head_k = hparams.n_embd_head_k;
    const int64_t n_embd_k_gqa  = hparams.n_embd_k_gqa(il);
    const int64_t n_embd_head_v = hparams.n_embd_head_v;
    const int64_t n_embd_v_gqa  = hparams.n_embd_v_gqa(il);

    struct lm_ggml_tensor * q = lm_ggml_permute(ctx, q_cur, 0, 2, 1, 3);
    cb(q, "q", il);

    struct lm_ggml_tensor * k =
        lm_ggml_view_3d(ctx, kv.k_l[il],
                n_embd_head_k, n_kv, n_head_kv,
                lm_ggml_row_size(kv.k_l[il]->type, n_embd_k_gqa),
                lm_ggml_row_size(kv.k_l[il]->type, n_embd_head_k),
                0);
    cb(k, "k", il);

    struct lm_ggml_tensor * cur;

    if (cparams.flash_attn) {
        LM_GGML_UNUSED(model);
        LM_GGML_UNUSED(n_ctx);

        // split cached v into n_head heads (not transposed)
        struct lm_ggml_tensor * v =
            lm_ggml_view_3d(ctx, kv.v_l[il],
                    n_embd_head_v, n_kv, n_head_kv,
                    lm_ggml_row_size(kv.v_l[il]->type, n_embd_v_gqa),
                    lm_ggml_row_size(kv.v_l[il]->type, n_embd_head_v),
                    0);
        cb(v, "v", il);

        cur = lm_ggml_flash_attn_ext(ctx, q, k, v, kq_mask, kq_scale, hparams.f_max_alibi_bias,
                                  hparams.attn_soft_cap ? hparams.f_attn_logit_softcapping : 0.0f);

        lm_ggml_flash_attn_ext_set_prec(cur, LM_GGML_PREC_F32);

        cur = lm_ggml_reshape_2d(ctx, cur, n_embd_head_v*n_head, n_tokens);
    } else {
        struct lm_ggml_tensor * kq = lm_ggml_mul_mat(ctx, k, q);
        cb(kq, "kq", il);

        // note: this op tends to require high floating point range
        //       while for some models F16 is enough, for others it is not, so we default to F32 here
        lm_ggml_mul_mat_set_prec(kq, LM_GGML_PREC_F32);

        if (model.arch == LLM_ARCH_GROK) {
            // need to do the following:
            // multiply by attn_output_multiplyer of 0.08838834764831845
            // and then :
            // kq = 30 * tanh(kq / 30)
            // before the softmax below

            kq = lm_ggml_tanh(ctx, lm_ggml_scale(ctx, kq, 0.08838834764831845f/30.0f));
            kq = lm_ggml_scale(ctx, kq, 30);
        }

        if (hparams.attn_soft_cap) {
            kq = lm_ggml_scale(ctx, kq, 1.0f / hparams.f_attn_logit_softcapping);
            kq = lm_ggml_tanh(ctx, kq);
            kq = lm_ggml_scale(ctx, kq, hparams.f_attn_logit_softcapping);
        }

        kq = lm_ggml_soft_max_ext(ctx, kq, kq_mask, kq_scale, hparams.f_max_alibi_bias);
        cb(kq, "kq_soft_max_ext", il);

        LM_GGML_ASSERT(kv.size == n_ctx);

        // split cached v into n_head heads
        struct lm_ggml_tensor * v =
            lm_ggml_view_3d(ctx, kv.v_l[il],
                    n_kv, n_embd_head_v, n_head_kv,
                    lm_ggml_element_size(kv.v_l[il])*n_ctx,
                    lm_ggml_element_size(kv.v_l[il])*n_ctx*n_embd_head_v,
                    0);
        cb(v, "v", il);

        struct lm_ggml_tensor * kqv = lm_ggml_mul_mat(ctx, v, kq);
        cb(kqv, "kqv", il);

        struct lm_ggml_tensor * kqv_merged = lm_ggml_permute(ctx, kqv, 0, 2, 1, 3);
        cb(kqv_merged, "kqv_merged", il);

        cur = lm_ggml_cont_2d(ctx, kqv_merged, n_embd_head_v*n_head, n_tokens);
        cb(cur, "kqv_merged_cont", il);
    }

    lm_ggml_build_forward_expand(graph, cur);

    if (wo) {
        cur = llm_build_lora_mm(lctx, ctx, wo, cur);
    }

    if (wo_b) {
        cb(cur, "kqv_wo", il);
    }

    if (wo_b) {
        cur = lm_ggml_add(ctx, cur, wo_b);
    }

    return cur;
}

static struct lm_ggml_tensor * llm_build_kv(
        struct lm_ggml_context * ctx,
       struct llama_context & lctx,
       const llama_kv_cache & kv,
         struct lm_ggml_cgraph * graph,
         struct lm_ggml_tensor * wo,
         struct lm_ggml_tensor * wo_b,
         struct lm_ggml_tensor * k_cur,
         struct lm_ggml_tensor * v_cur,
         struct lm_ggml_tensor * q_cur,
         struct lm_ggml_tensor * kq_mask,
                    int32_t   n_tokens,
                    int32_t   kv_head,
                    int32_t   n_kv,
                    float     kq_scale,
         const llm_build_cb & cb,
                    int       il) {
    const llama_hparams & hparams = lctx.model.hparams;
    const llama_cparams & cparams = lctx.cparams;

    // these nodes are added to the graph together so that they are not reordered
    // by doing so, the number of splits in the graph is reduced
    lm_ggml_build_forward_expand(graph, q_cur);
    lm_ggml_build_forward_expand(graph, k_cur);
    lm_ggml_build_forward_expand(graph, v_cur);

    llm_build_kv_store(ctx, hparams, cparams, kv, graph, k_cur, v_cur, n_tokens, kv_head, cb, il);

    struct lm_ggml_tensor * cur;

    cur  = llm_build_kqv(ctx, lctx, kv, graph, wo, wo_b, q_cur, kq_mask, n_tokens, n_kv, kq_scale, cb, il);
    cb(cur, "kqv_out", il);

    return cur;
}

static struct lm_ggml_tensor * llm_build_copy_mask_state(
        struct lm_ggml_context * ctx,
         struct lm_ggml_cgraph * graph,
         struct lm_ggml_tensor * s,
         struct lm_ggml_tensor * state_copy,
         struct lm_ggml_tensor * state_mask,
                    int32_t   n_state,
                    int32_t   kv_size,
                    int32_t   kv_head,
                    int32_t   n_kv,
                    int32_t   n_seqs) {
    struct lm_ggml_tensor * states = lm_ggml_reshape_2d(ctx, s, n_state, kv_size);

    // copy states
    // NOTE: assuming the copy destinations are ALL contained between kv_head and kv_head + n_kv
    // this shrinks the tensors's ne[1] to n_kv
    states = lm_ggml_get_rows(ctx, states, state_copy);

    // clear states of sequences which are starting at the beginning of this batch
    // FIXME: zero-out NANs?
    states = lm_ggml_mul(ctx, states, state_mask);

    // copy states which won't be changed further (between n_seqs and n_kv)
    lm_ggml_build_forward_expand(graph,
        lm_ggml_cpy(ctx,
            lm_ggml_view_1d(ctx, states, n_state*(n_kv - n_seqs), n_seqs*n_state*lm_ggml_element_size(states)),
            lm_ggml_view_1d(ctx, s, n_state*(n_kv - n_seqs), (kv_head + n_seqs)*n_state*lm_ggml_element_size(s))));

    // the part of the states that will be used and modified
    return lm_ggml_view_2d(ctx, states, n_state, n_seqs, states->nb[1], 0);
}

// TODO: split
static struct lm_ggml_tensor * llm_build_mamba(
        struct lm_ggml_context * ctx,
       struct llama_context & lctx,
         const llama_ubatch & batch,
         struct lm_ggml_cgraph * graph,
         struct lm_ggml_tensor * cur,
         struct lm_ggml_tensor * state_copy,
         struct lm_ggml_tensor * state_mask,
                    int32_t   kv_head,
                    int32_t   n_kv,
         const llm_build_cb & cb,
                    int       il) {
    const llama_model    & model   = lctx.model;
    const llama_hparams  & hparams = model.hparams;
    const llama_kv_cache & kv      = lctx.kv_self;
    const int64_t d_conv  = hparams.ssm_d_conv;
    const int64_t d_inner = hparams.ssm_d_inner;
    const int64_t d_state = hparams.ssm_d_state;
    const int64_t dt_rank = hparams.ssm_dt_rank;
    const int64_t n_seqs  = batch.n_seqs;
    // Some variants of Mamba arch (e.g. FalconMamba do apply layer norm on B and Dt layers)
    const bool ssm_dt_b_c_rms = hparams.ssm_dt_b_c_rms;
    // Use the same RMS norm as the final layer norm
    const float norm_rms_eps = hparams.f_norm_rms_eps;

    const int64_t n_seq_tokens = batch.n_seq_tokens;

    LM_GGML_ASSERT(n_seqs != 0);
    LM_GGML_ASSERT(batch.equal_seqs);
    LM_GGML_ASSERT(batch.n_tokens == n_seq_tokens * n_seqs);

    struct lm_ggml_tensor * conv_states_all = kv.k_l[il];
    struct lm_ggml_tensor * ssm_states_all  = kv.v_l[il];

    // (ab)using the KV cache to store the states
    struct lm_ggml_tensor * conv = llm_build_copy_mask_state(ctx,
            graph, conv_states_all, state_copy, state_mask,
            hparams.n_embd_k_s(), kv.size, kv_head, n_kv, n_seqs);
    conv = lm_ggml_reshape_3d(ctx, conv, d_conv - 1, d_inner, n_seqs);
    struct lm_ggml_tensor * ssm = llm_build_copy_mask_state(ctx,
            graph, ssm_states_all, state_copy, state_mask,
            hparams.n_embd_v_s(), kv.size, kv_head, n_kv, n_seqs);
    ssm = lm_ggml_reshape_3d(ctx, ssm, d_state, d_inner, n_seqs);

    // {n_embd, n_tokens} => {n_embd, n_seq_tokens, n_seqs}
    cur = lm_ggml_reshape_3d(ctx, cur, cur->ne[0], n_seq_tokens, n_seqs);

    // {n_embd, 2*d_inner} @ {n_embd, n_seq_tokens, n_seqs} => {2*d_inner, n_seq_tokens, n_seqs}
    struct lm_ggml_tensor * xz = llm_build_lora_mm(lctx, ctx, model.layers[il].ssm_in, cur);
    // split the above in two
    // => {d_inner, n_seq_tokens, n_seqs}
    struct lm_ggml_tensor * x = lm_ggml_view_3d(ctx, xz, d_inner, xz->ne[1], xz->ne[2], xz->nb[1], xz->nb[2], 0);
    struct lm_ggml_tensor * z = lm_ggml_view_3d(ctx, xz, d_inner, xz->ne[1], xz->ne[2], xz->nb[1], xz->nb[2], d_inner*lm_ggml_element_size(xz));

    // conv
    {
        // => {d_conv - 1 + n_seq_tokens, d_inner, n_seqs}
        struct lm_ggml_tensor * conv_x = lm_ggml_concat(ctx, conv, lm_ggml_transpose(ctx, x), 0);

        // copy last (d_conv - 1) columns back into the state cache
        struct lm_ggml_tensor * last_conv = lm_ggml_view_3d(ctx, conv_x, d_conv - 1, d_inner, n_seqs, conv_x->nb[1], conv_x->nb[2], n_seq_tokens*(conv_x->nb[0]));

        lm_ggml_build_forward_expand(graph,
            lm_ggml_cpy(ctx, last_conv,
                lm_ggml_view_1d(ctx, conv_states_all,
                    (d_conv - 1)*(d_inner)*(n_seqs),
                    kv_head*(d_conv - 1)*(d_inner)*lm_ggml_element_size(conv_states_all))));

        // 1D convolution
        // The equivalent is to make a self-overlapping view of conv_x
        // over d_conv columns at each stride in the 3rd dimension,
        // then element-wise multiply that with the conv1d weight,
        // then sum the elements of each row,
        // (the last two steps are a dot product over rows (also doable with mul_mat))
        // then permute away the ne[0] dimension,
        // and then you're left with the resulting x tensor.
        // For simultaneous sequences, all sequences need to have the same length.
        x = lm_ggml_ssm_conv(ctx, conv_x, model.layers[il].ssm_conv1d);

        // bias
        x = lm_ggml_add(ctx, x, model.layers[il].ssm_conv1d_b);

        x = lm_ggml_silu(ctx, x);
    }

    // ssm
    {
        // {d_inner, dt_rank + 2*d_state} @ {d_inner, n_seq_tokens, n_seqs} => {dt_rank + 2*d_state, n_seq_tokens, n_seqs}
        struct lm_ggml_tensor * x_db = llm_build_lora_mm(lctx, ctx, model.layers[il].ssm_x, x);
        // split
        struct lm_ggml_tensor * dt = lm_ggml_view_3d(ctx, x_db, dt_rank, n_seq_tokens, n_seqs, x_db->nb[1], x_db->nb[2], 0);
        struct lm_ggml_tensor * B  = lm_ggml_view_3d(ctx, x_db, d_state, n_seq_tokens, n_seqs, x_db->nb[1], x_db->nb[2], lm_ggml_element_size(x_db)*dt_rank);
        struct lm_ggml_tensor * C  = lm_ggml_view_3d(ctx, x_db, d_state, n_seq_tokens, n_seqs, x_db->nb[1], x_db->nb[2], lm_ggml_element_size(x_db)*(dt_rank+d_state));

        // Some Mamba variants (e.g. FalconMamba) apply RMS norm in B, C & Dt layers
        if (ssm_dt_b_c_rms) {
            dt = lm_ggml_rms_norm(ctx, dt, norm_rms_eps);
            B = lm_ggml_rms_norm(ctx, B, norm_rms_eps);
            C = lm_ggml_rms_norm(ctx, C, norm_rms_eps);
        }

        // {dt_rank, d_inner} @ {dt_rank, n_seq_tokens, n_seqs} => {d_inner, n_seq_tokens, n_seqs}
        dt = llm_build_lora_mm(lctx, ctx, model.layers[il].ssm_dt, dt);
        dt = lm_ggml_add(ctx, dt, model.layers[il].ssm_dt_b);

        // Custom operator to optimize the parallel associative scan
        // as described in the Annex D of the Mamba paper.
        // => {d_inner, n_seq_tokens, n_seqs} and {d_state, d_inner, n_seqs}
        struct lm_ggml_tensor * y_ssm = lm_ggml_ssm_scan(ctx, ssm, x, dt, model.layers[il].ssm_a, B, C);

        // store last states
        lm_ggml_build_forward_expand(graph,
            lm_ggml_cpy(ctx,
                lm_ggml_view_1d(ctx, y_ssm, d_state*d_inner*n_seqs, x->nb[3]),
                lm_ggml_view_1d(ctx, ssm_states_all, d_state*d_inner*n_seqs, kv_head*d_state*d_inner*lm_ggml_element_size(ssm_states_all))));

        struct lm_ggml_tensor * y = lm_ggml_view_3d(ctx, y_ssm, d_inner, n_seq_tokens, n_seqs, x->nb[1], x->nb[2], 0);

        // TODO: skip computing output earlier for unused tokens

        // {d_inner, n_seq_tokens, n_seqs} * {d_inner} => {d_inner, n_seq_tokens, n_seqs}
        y = lm_ggml_add(ctx, y, lm_ggml_mul(ctx, x, model.layers[il].ssm_d));
        y = lm_ggml_mul(ctx, y, lm_ggml_silu(ctx, lm_ggml_cont(ctx, z)));

        // {d_inner, n_embd} @ {d_inner, n_seq_tokens, n_seqs} => {n_embd, n_seq_tokens, n_seqs}
        cur = llm_build_lora_mm(lctx, ctx, model.layers[il].ssm_out, y);
    }

    // {n_embd, n_seq_tokens, n_seqs} => {n_embd, n_tokens}
    cur = lm_ggml_reshape_2d(ctx, cur, cur->ne[0], n_seq_tokens * n_seqs);
    cb(cur, "mamba_out", il);

    return cur;
}

static struct lm_ggml_tensor * llm_build_rwkv6_time_mix(
        struct llama_context & lctx,
        struct lm_ggml_context * ctx,
        const struct llama_layer * layer,
        struct lm_ggml_tensor * cur,
        struct lm_ggml_tensor * x_prev,
        struct lm_ggml_tensor ** wkv_state) {
    size_t n_embd       = cur->ne[0];
    size_t n_seq_tokens = cur->ne[1];
    size_t n_seqs       = cur->ne[2];

    size_t head_size  = layer->time_mix_first->ne[0];
    size_t head_count = layer->time_mix_first->ne[1];

    size_t n_tokens = n_seqs * n_seq_tokens;

    struct lm_ggml_tensor * sx = lm_ggml_sub(ctx, x_prev, cur);

    sx  = lm_ggml_reshape_2d(ctx, sx,  n_embd, n_tokens);
    cur = lm_ggml_reshape_2d(ctx, cur, n_embd, n_tokens);

    struct lm_ggml_tensor * xxx = lm_ggml_add(ctx, lm_ggml_mul(ctx, sx, layer->time_mix_lerp_x), cur);

    xxx = lm_ggml_reshape_4d(
        ctx,
        lm_ggml_tanh(
            ctx,
            lm_ggml_mul_mat(ctx, layer->time_mix_w1, xxx)
        ),
        layer->time_mix_w1->ne[1] / 5, 1, 5, n_tokens
    );

    xxx = lm_ggml_cont(ctx, lm_ggml_permute(ctx, xxx, 0, 1, 3, 2));

    xxx = lm_ggml_mul_mat(
        ctx,
        lm_ggml_reshape_4d(
            ctx,
            layer->time_mix_w2,
            layer->time_mix_w2->ne[0], layer->time_mix_w2->ne[1], 1, 5
        ),
        xxx
    );

    struct lm_ggml_tensor *mw = lm_ggml_view_2d(ctx, xxx, n_embd, n_tokens, xxx->nb[1], 0);
    struct lm_ggml_tensor *mk = lm_ggml_view_2d(ctx, xxx, n_embd, n_tokens, xxx->nb[1], n_embd * n_tokens * sizeof(float));
    struct lm_ggml_tensor *mv = lm_ggml_view_2d(ctx, xxx, n_embd, n_tokens, xxx->nb[1], n_embd * n_tokens * 2 * sizeof(float));
    struct lm_ggml_tensor *mr = lm_ggml_view_2d(ctx, xxx, n_embd, n_tokens, xxx->nb[1], n_embd * n_tokens * 3 * sizeof(float));
    struct lm_ggml_tensor *mg = lm_ggml_view_2d(ctx, xxx, n_embd, n_tokens, xxx->nb[1], n_embd * n_tokens * 4 * sizeof(float));

    struct lm_ggml_tensor * xw = lm_ggml_add(
        ctx,
        lm_ggml_mul(
            ctx,
            lm_ggml_add(ctx, mw, layer->time_mix_lerp_w),
            sx
        ),
        cur
    );

    struct lm_ggml_tensor * xk = lm_ggml_add(
        ctx,
        lm_ggml_mul(
            ctx,
            lm_ggml_add(ctx, mk, layer->time_mix_lerp_k),
            sx
        ),
        cur
    );

    struct lm_ggml_tensor * xv = lm_ggml_add(
        ctx,
        lm_ggml_mul(
            ctx,
            lm_ggml_add(ctx, mv, layer->time_mix_lerp_v),
            sx
        ),
        cur
    );

    struct lm_ggml_tensor * xr = lm_ggml_add(
        ctx,
        lm_ggml_mul(
            ctx,
            lm_ggml_add(ctx, mr, layer->time_mix_lerp_r),
            sx
        ),
        cur
    );

    struct lm_ggml_tensor * xg = lm_ggml_add(
        ctx,
        lm_ggml_mul(
            ctx,
            lm_ggml_add(ctx, mg, layer->time_mix_lerp_g),
            sx
        ),
        cur
    );

    struct lm_ggml_tensor * r = lm_ggml_reshape_4d(ctx, llm_build_lora_mm(lctx, ctx, layer->time_mix_receptance, xr), head_size, 1,         head_count, n_tokens);
    struct lm_ggml_tensor * k = lm_ggml_reshape_4d(ctx, llm_build_lora_mm(lctx, ctx, layer->time_mix_key,        xk), 1,         head_size, head_count, n_tokens);
    struct lm_ggml_tensor * v = lm_ggml_reshape_4d(ctx, llm_build_lora_mm(lctx, ctx, layer->time_mix_value,      xv), head_size, 1,         head_count, n_tokens);
    struct lm_ggml_tensor * g = lm_ggml_silu(
        ctx,
        llm_build_lora_mm(lctx, ctx, layer->time_mix_gate, xg)
    );

    struct lm_ggml_tensor * w = lm_ggml_mul_mat(
        ctx,
        layer->time_mix_decay_w2,
        lm_ggml_tanh(
            ctx,
            lm_ggml_mul_mat(ctx, layer->time_mix_decay_w1, xw)
        )
    );

    w = lm_ggml_add(ctx, w, lm_ggml_reshape_1d(ctx, layer->time_mix_decay, n_embd));
    w = lm_ggml_exp(ctx, lm_ggml_neg(ctx, lm_ggml_exp(ctx, w)));
    w = lm_ggml_reshape_4d(ctx, w, 1, head_size, head_count, n_tokens);

    k = lm_ggml_transpose(ctx, k);
    v = lm_ggml_transpose(ctx, v);
    r = lm_ggml_transpose(ctx, r);

    struct lm_ggml_tensor * wkv_output = lm_ggml_rwkv_wkv6(ctx, k, v, r, layer->time_mix_first, w, *wkv_state);
    cur = lm_ggml_view_1d(ctx, wkv_output, n_embd * n_tokens, 0);
    *wkv_state = lm_ggml_view_1d(ctx, wkv_output, n_embd * head_size * n_seqs, n_embd * n_tokens * sizeof(float));

    // group norm with head_count groups
    cur = lm_ggml_reshape_3d(ctx, cur, n_embd / head_count, head_count, n_tokens);
    cur = lm_ggml_norm(ctx, cur, 64e-5f);

    // Convert back to regular vectors.
    cur = lm_ggml_reshape_2d(ctx, cur, n_embd, n_tokens);
    cur = lm_ggml_add(ctx, lm_ggml_mul(ctx, cur, layer->time_mix_ln), layer->time_mix_ln_b);

    cur = lm_ggml_mul(ctx, cur, g);
    cur = llm_build_lora_mm(lctx, ctx, layer->time_mix_output, cur);

    return lm_ggml_reshape_3d(ctx, cur, n_embd, n_seq_tokens, n_seqs);
}

static struct lm_ggml_tensor * llm_build_rwkv6_channel_mix(
        struct llama_context & lctx,
        struct lm_ggml_context * ctx,
        const struct llama_layer * layer,
        struct lm_ggml_tensor * cur,
        struct lm_ggml_tensor * x_prev) {
    struct lm_ggml_tensor * sx = lm_ggml_sub(ctx, x_prev, cur);
    struct lm_ggml_tensor * xk = lm_ggml_add(ctx, lm_ggml_mul(ctx, sx, layer->channel_mix_lerp_k), cur);
    struct lm_ggml_tensor * xr = lm_ggml_add(ctx, lm_ggml_mul(ctx, sx, layer->channel_mix_lerp_r), cur);

    struct lm_ggml_tensor * r = lm_ggml_sigmoid(ctx, llm_build_lora_mm(lctx, ctx, layer->channel_mix_receptance, xr));
    struct lm_ggml_tensor * k = lm_ggml_sqr(
        ctx,
        lm_ggml_relu(
            ctx,
            llm_build_lora_mm(lctx, ctx, layer->channel_mix_key, xk)
        )
    );

    return lm_ggml_mul(ctx, r, llm_build_lora_mm(lctx, ctx, layer->channel_mix_value, k));
}

struct llm_build_context {
    const llama_model    & model;
          llama_context  & lctx;
    const llama_hparams  & hparams;
    const llama_cparams  & cparams;
    const llama_ubatch   & ubatch;
    const llama_kv_cache & kv_self;

    const int64_t n_embd;
    const int64_t n_layer;
    const int64_t n_rot;
    const int64_t n_ctx;       // user-specified context size (can be different from n_ctx_train)
    const int64_t n_head;
    const int64_t n_head_kv;
    const int64_t n_embd_head_k;
    const int64_t n_embd_k_gqa;
    const int64_t n_embd_head_v;
    const int64_t n_embd_v_gqa;
    const int64_t n_expert;
    const int64_t n_expert_used;

    const float freq_base;
    const float freq_scale;
    const float ext_factor;
    const float attn_factor;
    const float beta_fast;
    const float beta_slow;
    const float norm_eps;
    const float norm_rms_eps;

    const int32_t n_tokens;
    const int32_t n_kv;     // size of KV cache to consider (n_kv <= kv_self.size)
    const int32_t n_outputs;
    const int32_t n_outputs_enc;
    const int32_t kv_head;  // index of where we store new KV data in the cache
    const int32_t n_ctx_orig;

    const bool flash_attn;

    const enum llama_pooling_type pooling_type;
    const enum llama_rope_type    rope_type;

    const llm_build_cb & cb;

    std::vector<uint8_t> & buf_compute_meta;

    struct lm_ggml_context * ctx0 = nullptr;

    // TODO: consider making the entire interface noexcept
    llm_build_context(
        llama_context  & lctx,
    const llama_ubatch & ubatch,
    const llm_build_cb & cb,
                  bool   worst_case) :
        model            (lctx.model),
        lctx             (lctx),
        hparams          (model.hparams),
        cparams          (lctx.cparams),
        ubatch           (ubatch),
        kv_self          (lctx.kv_self),
        n_embd           (hparams.n_embd),
        n_layer          (hparams.n_layer),
        n_rot            (hparams.n_rot),
        n_ctx            (cparams.n_ctx),
        n_head           (hparams.n_head()),
        n_head_kv        (hparams.n_head_kv()),
        n_embd_head_k    (hparams.n_embd_head_k),
        n_embd_k_gqa     (hparams.n_embd_k_gqa()),
        n_embd_head_v    (hparams.n_embd_head_v),
        n_embd_v_gqa     (hparams.n_embd_v_gqa()),
        n_expert         (hparams.n_expert),
        n_expert_used    (hparams.n_expert_used),
        freq_base        (cparams.rope_freq_base),
        freq_scale       (cparams.rope_freq_scale),
        ext_factor       (cparams.yarn_ext_factor),
        attn_factor      (cparams.yarn_attn_factor),
        beta_fast        (cparams.yarn_beta_fast),
        beta_slow        (cparams.yarn_beta_slow),
        norm_eps         (hparams.f_norm_eps),
        norm_rms_eps     (hparams.f_norm_rms_eps),
        n_tokens         (ubatch.n_tokens),
        n_kv             (worst_case ? kv_self.size : kv_self.n),
        n_outputs        (worst_case ? n_tokens : lctx.n_outputs),
        n_outputs_enc    (worst_case ? n_tokens : lctx.embd_enc.size() / hparams.n_embd),
        kv_head          (worst_case ? (kv_self.recurrent ? 0 : kv_self.size - n_tokens) : kv_self.head),
        n_ctx_orig       (cparams.n_ctx_orig_yarn),
        flash_attn       (cparams.flash_attn),
        pooling_type     (cparams.pooling_type),
        rope_type        (hparams.rope_type),
        cb               (cb),
        buf_compute_meta (lctx.buf_compute_meta) {
            // all initializations should be done in init()
        }

    void init() {
        struct lm_ggml_init_params params = {
            /*.mem_size   =*/ buf_compute_meta.size(),
            /*.mem_buffer =*/ buf_compute_meta.data(),
            /*.no_alloc   =*/ true,
        };

        ctx0 = lm_ggml_init(params);

        lctx.inp_tokens      = nullptr;
        lctx.inp_embd        = nullptr;
        lctx.inp_pos         = nullptr;
        lctx.inp_out_ids     = nullptr;
        lctx.inp_KQ_mask     = nullptr;
        lctx.inp_KQ_mask_swa = nullptr;
        lctx.inp_K_shift     = nullptr;
        lctx.inp_mean        = nullptr;
        lctx.inp_cls         = nullptr;
        lctx.inp_s_copy      = nullptr;
        lctx.inp_s_mask      = nullptr;
        lctx.inp_s_seq       = nullptr;
        lctx.inp_pos_bucket    = nullptr;
        lctx.inp_embd_enc      = nullptr;
        lctx.inp_KQ_mask_cross = nullptr;
    }

    void free() {
        lm_ggml_free(ctx0);
        ctx0 = nullptr;
    }

    struct lm_ggml_cgraph * build_k_shift() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        LM_GGML_ASSERT(kv_self.size == n_ctx);

        lctx.inp_K_shift = lm_ggml_new_tensor_1d(ctx0, LM_GGML_TYPE_I32, n_ctx);
        cb(lctx.inp_K_shift, "K_shift", -1);
        lm_ggml_set_input(lctx.inp_K_shift);

        for (int il = 0; il < n_layer; ++il) {
            const int64_t n_head_kv = hparams.n_head_kv(il);
            const int64_t n_embd_k_gqa = hparams.n_embd_k_gqa(il);
            struct lm_ggml_tensor * rope_factors = build_rope_factors(il);
            struct lm_ggml_tensor * k =
                lm_ggml_view_3d(ctx0, kv_self.k_l[il],
                    n_embd_head_k, n_head_kv, n_ctx,
                    lm_ggml_row_size(kv_self.k_l[il]->type, n_embd_head_k),
                    lm_ggml_row_size(kv_self.k_l[il]->type, n_embd_k_gqa),
                    0);

            struct lm_ggml_tensor * tmp;
            if (lm_ggml_is_quantized(k->type)) {
                // dequantize to f32 -> RoPE -> quantize back
                tmp = lm_ggml_cast(ctx0, k, LM_GGML_TYPE_F32);
                cb(tmp, "K_f32", il);
                for (auto & backend : lctx.backends) {
                    // Figure out which backend KV cache belongs to
                    if (lm_ggml_backend_supports_buft(backend.get(), lm_ggml_backend_buffer_get_type(kv_self.k_l[il]->buffer))) {
                        lm_ggml_backend_sched_set_tensor_backend(lctx.sched.get(), tmp, backend.get());
                        break;
                    }
                }
                tmp = lm_ggml_rope_ext_inplace(ctx0, tmp,
                        lctx.inp_K_shift, rope_factors, n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                        ext_factor, attn_factor, beta_fast, beta_slow);
                cb(tmp, "K_shifted_f32", il);
                tmp = lm_ggml_cpy(ctx0, tmp, k);
            } else {
                // we rotate only the first n_rot dimensions
                tmp = lm_ggml_rope_ext_inplace(ctx0, k,
                        lctx.inp_K_shift, rope_factors, n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                        ext_factor, attn_factor, beta_fast, beta_slow);
            }
            cb(tmp, "K_shifted", il);
            lm_ggml_build_forward_expand(gf, tmp);
        }

        return gf;
    }

    struct lm_ggml_cgraph * build_defrag(const std::vector<uint32_t> & ids) {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        for (uint32_t i = 0; i < ids.size(); ++i) {
            const uint32_t id = ids[i];

            if (i == id || id == ids.size()) {
                continue;
            }

            uint32_t nm = 1;

            while (i + nm < ids.size() && ids[i + nm] == id + nm) {
                nm++;
            }

            for (int il = 0; il < n_layer; ++il) {
                const int64_t n_embd_k_gqa = hparams.n_embd_k_gqa(il);
                const int64_t n_embd_v_gqa = hparams.n_embd_v_gqa(il);

                lm_ggml_tensor * view_k_src = lm_ggml_view_2d(ctx0, kv_self.k_l[il],
                        n_embd_k_gqa, nm,
                        lm_ggml_row_size(kv_self.k_l[il]->type, n_embd_k_gqa),
                        lm_ggml_row_size(kv_self.k_l[il]->type, n_embd_k_gqa*i));

                lm_ggml_tensor * view_k_dst = lm_ggml_view_2d(ctx0, kv_self.k_l[il],
                        n_embd_k_gqa, nm,
                        lm_ggml_row_size(kv_self.k_l[il]->type, n_embd_k_gqa),
                        lm_ggml_row_size(kv_self.k_l[il]->type, n_embd_k_gqa*id));

                lm_ggml_tensor * view_v_src;
                lm_ggml_tensor * view_v_dst;

                if (flash_attn) {
                    // NOTE: the V cache is not transposed when using flash attention
                    view_v_src = lm_ggml_view_2d(ctx0, kv_self.v_l[il],
                            n_embd_v_gqa, nm,
                            lm_ggml_row_size(kv_self.v_l[il]->type, n_embd_v_gqa),
                            lm_ggml_row_size(kv_self.v_l[il]->type, n_embd_v_gqa*i));

                    view_v_dst = lm_ggml_view_2d(ctx0, kv_self.v_l[il],
                            n_embd_v_gqa, nm,
                            lm_ggml_row_size(kv_self.v_l[il]->type, n_embd_v_gqa),
                            lm_ggml_row_size(kv_self.v_l[il]->type, n_embd_v_gqa*id));
                } else {
                    view_v_src = lm_ggml_view_2d(ctx0, kv_self.v_l[il],
                            nm, n_embd_v_gqa,
                            lm_ggml_row_size(kv_self.v_l[il]->type, kv_self.size),
                            lm_ggml_row_size(kv_self.v_l[il]->type, i));

                    view_v_dst = lm_ggml_view_2d(ctx0, kv_self.v_l[il],
                            nm, n_embd_v_gqa,
                            lm_ggml_row_size(kv_self.v_l[il]->type, kv_self.size),
                            lm_ggml_row_size(kv_self.v_l[il]->type, id));
                }

                lm_ggml_build_forward_expand(gf, lm_ggml_cpy(ctx0, view_k_src, view_k_dst));
                lm_ggml_build_forward_expand(gf, lm_ggml_cpy(ctx0, view_v_src, view_v_dst));
            }

            i += nm - 1;
        }

        //LLAMA_LOG_INFO("gf->n_nodes = %d\n", gf->n_nodes);

        return gf;
    }

    struct lm_ggml_tensor * build_inp_pos() {
        lctx.inp_pos = lm_ggml_new_tensor_1d(ctx0, LM_GGML_TYPE_I32, n_tokens);
        cb(lctx.inp_pos, "inp_pos", -1);
        lm_ggml_set_input(lctx.inp_pos);
        return lctx.inp_pos;
    }

    struct lm_ggml_tensor * build_rope_factors(int il) {
        // choose long/short freq factors based on the context size
        const auto n_ctx_pre_seq = cparams.n_ctx / cparams.n_seq_max;

        if (model.layers[il].rope_freqs != nullptr) {
            return model.layers[il].rope_freqs;
        }

        if (n_ctx_pre_seq > hparams.n_ctx_orig_yarn) {
            return model.layers[il].rope_long;
        }

        return model.layers[il].rope_short;
    }

    struct lm_ggml_tensor * build_inp_out_ids() {
        lctx.inp_out_ids = lm_ggml_new_tensor_1d(ctx0, LM_GGML_TYPE_I32, n_outputs);
        cb(lctx.inp_out_ids, "inp_out_ids", -1);
        lm_ggml_set_input(lctx.inp_out_ids);
        return lctx.inp_out_ids;
    }

    struct lm_ggml_tensor * build_inp_KQ_mask(bool causal = true) {
        lctx.inp_KQ_mask = causal
            ? lm_ggml_new_tensor_2d(ctx0, LM_GGML_TYPE_F32, n_kv,     LM_GGML_PAD(n_tokens, LM_GGML_KQ_MASK_PAD))
            : lm_ggml_new_tensor_2d(ctx0, LM_GGML_TYPE_F32, n_tokens, LM_GGML_PAD(n_tokens, LM_GGML_KQ_MASK_PAD));
        cb(lctx.inp_KQ_mask, "KQ_mask", -1);
        lm_ggml_set_input(lctx.inp_KQ_mask);

        return flash_attn ? lm_ggml_cast(ctx0, lctx.inp_KQ_mask, LM_GGML_TYPE_F16) : lctx.inp_KQ_mask;
    }

    struct lm_ggml_tensor * build_inp_KQ_mask_swa(bool causal = true) {
        LM_GGML_ASSERT(hparams.n_swa > 0);

        lctx.inp_KQ_mask_swa = causal
            ? lm_ggml_new_tensor_2d(ctx0, LM_GGML_TYPE_F32, n_kv,     LM_GGML_PAD(n_tokens, LM_GGML_KQ_MASK_PAD))
            : lm_ggml_new_tensor_2d(ctx0, LM_GGML_TYPE_F32, n_tokens, LM_GGML_PAD(n_tokens, LM_GGML_KQ_MASK_PAD));
        cb(lctx.inp_KQ_mask_swa, "KQ_mask_swa", -1);
        lm_ggml_set_input(lctx.inp_KQ_mask_swa);

        return flash_attn ? lm_ggml_cast(ctx0, lctx.inp_KQ_mask_swa, LM_GGML_TYPE_F16) : lctx.inp_KQ_mask_swa;
    }

    struct lm_ggml_tensor * build_inp_mean() {
        lctx.inp_mean = lm_ggml_new_tensor_2d(ctx0, LM_GGML_TYPE_F32, n_tokens, n_tokens);
        cb(lctx.inp_mean, "inp_mean", -1);
        lm_ggml_set_input(lctx.inp_mean);
        return lctx.inp_mean;
    }

    struct lm_ggml_tensor * build_inp_cls() {
        lctx.inp_cls = lm_ggml_new_tensor_1d(ctx0, LM_GGML_TYPE_I32, n_tokens);
        cb(lctx.inp_cls, "inp_cls", -1);
        lm_ggml_set_input(lctx.inp_cls);
        return lctx.inp_cls;
    }

    struct lm_ggml_tensor * build_inp_s_copy() {
        lctx.inp_s_copy = lm_ggml_new_tensor_1d(ctx0, LM_GGML_TYPE_I32, n_kv);
        cb(lctx.inp_s_copy, "inp_s_copy", -1);
        lm_ggml_set_input(lctx.inp_s_copy);
        return lctx.inp_s_copy;
    }

    struct lm_ggml_tensor * build_inp_s_mask() {
        lctx.inp_s_mask = lm_ggml_new_tensor_2d(ctx0, LM_GGML_TYPE_F32, 1, n_kv);
        cb(lctx.inp_s_mask, "inp_s_mask", -1);
        lm_ggml_set_input(lctx.inp_s_mask);
        return lctx.inp_s_mask;
    }

    struct lm_ggml_cgraph * append_pooling(struct lm_ggml_cgraph * gf) {
        // find result_norm tensor for input
        struct lm_ggml_tensor * inp = nullptr;
        for (int i = lm_ggml_graph_n_nodes(gf) - 1; i >= 0; --i) {
            inp = lm_ggml_graph_node(gf, i);
            if (strcmp(inp->name, "result_norm") == 0 || strcmp(inp->name, "result_embd") == 0) {
                break;
            } else {
                inp = nullptr;
            }
        }
        LM_GGML_ASSERT(inp != nullptr && "missing result_norm/result_embd tensor");

        struct lm_ggml_tensor * cur;

        switch (pooling_type) {
            case LLAMA_POOLING_TYPE_NONE:
                {
                    cur = inp;
                } break;
            case LLAMA_POOLING_TYPE_MEAN:
                {
                    struct lm_ggml_tensor * inp_mean = build_inp_mean();
                    cur = lm_ggml_mul_mat(ctx0, lm_ggml_cont(ctx0, lm_ggml_transpose(ctx0, inp)), inp_mean);
                } break;
            case LLAMA_POOLING_TYPE_CLS:
            case LLAMA_POOLING_TYPE_LAST:
                {
                    struct lm_ggml_tensor * inp_cls = build_inp_cls();
                    cur = lm_ggml_get_rows(ctx0, inp, inp_cls);
                } break;
            case LLAMA_POOLING_TYPE_RANK:
                {
                    struct lm_ggml_tensor * inp_cls = build_inp_cls();
                    inp = lm_ggml_get_rows(ctx0, inp, inp_cls);

                    // classification head
                    // https://github.com/huggingface/transformers/blob/5af7d41e49bbfc8319f462eb45253dcb3863dfb7/src/transformers/models/roberta/modeling_roberta.py#L1566
                    LM_GGML_ASSERT(model.cls       != nullptr);
                    LM_GGML_ASSERT(model.cls_b     != nullptr);

                    cur = lm_ggml_add (ctx0, lm_ggml_mul_mat(ctx0, model.cls, inp), model.cls_b);
                    cur = lm_ggml_tanh(ctx0, cur);

                    // some models don't have `cls_out`, for example: https://huggingface.co/jinaai/jina-reranker-v1-tiny-en
                    // https://huggingface.co/jinaai/jina-reranker-v1-tiny-en/blob/cb5347e43979c3084a890e3f99491952603ae1b7/modeling_bert.py#L884-L896
                    if (model.cls_out) {
                        LM_GGML_ASSERT(model.cls_out_b != nullptr);

                        cur = lm_ggml_add (ctx0, lm_ggml_mul_mat(ctx0, model.cls_out, cur), model.cls_out_b);
                    }
                } break;
            default:
                {
                    LM_GGML_ABORT("unknown pooling type");
                }
        }

        cb(cur, "result_embd_pooled", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_tensor * llm_build_pos_bucket(bool causal) {
        if (causal) {
            lctx.inp_pos_bucket = lm_ggml_new_tensor_2d(ctx0, LM_GGML_TYPE_I32, n_kv,     n_tokens);
        } else {
            lctx.inp_pos_bucket = lm_ggml_new_tensor_2d(ctx0, LM_GGML_TYPE_I32, n_tokens, n_tokens);
        }

        lm_ggml_set_input(lctx.inp_pos_bucket);
        cb(lctx.inp_pos_bucket, "pos_bucket", -1);

        return lctx.inp_pos_bucket;
    }

    struct lm_ggml_tensor * llm_build_pos_bias(struct lm_ggml_tensor * pos_bucket, struct lm_ggml_tensor * attn_rel_b) {
        struct lm_ggml_tensor * pos_bucket_1d = lm_ggml_view_1d(ctx0, pos_bucket, pos_bucket->ne[0] * pos_bucket->ne[1], 0);
        cb(pos_bucket_1d, "pos_bucket_1d", -1);

        struct lm_ggml_tensor * pos_bias = lm_ggml_get_rows(ctx0, attn_rel_b, pos_bucket_1d);
        cb(pos_bias, "pos_bias", -1);

        pos_bias = lm_ggml_view_3d(ctx0, pos_bias, pos_bias->ne[0], lctx.inp_pos_bucket->ne[0], lctx.inp_pos_bucket->ne[1], lm_ggml_element_size(pos_bias) * pos_bias->ne[0], lm_ggml_element_size(pos_bias) * pos_bias->ne[0] * lctx.inp_pos_bucket->ne[0],  0);
        cb(pos_bias, "pos_bias", -1);

        pos_bias = lm_ggml_permute(ctx0, pos_bias, 2, 0, 1, 3);
        cb(pos_bias, "pos_bias", -1);

        pos_bias = lm_ggml_cont(ctx0, pos_bias);
        cb(pos_bias, "pos_bias", -1);

        return pos_bias;
    }

    struct lm_ggml_tensor * llm_build_inp_embd_enc() {
        const int64_t n_embd = hparams.n_embd;
        lctx.inp_embd_enc = lm_ggml_new_tensor_2d(ctx0, LM_GGML_TYPE_F32, n_embd, n_outputs_enc);
        lm_ggml_set_input(lctx.inp_embd_enc);
        cb(lctx.inp_embd_enc, "embd_enc", -1);
        return lctx.inp_embd_enc;
    }

    struct lm_ggml_tensor * llm_build_inp_KQ_mask_cross() {
        lctx.inp_KQ_mask_cross = lm_ggml_new_tensor_2d(ctx0, LM_GGML_TYPE_F32, n_outputs_enc, LM_GGML_PAD(n_tokens, LM_GGML_KQ_MASK_PAD));
        lm_ggml_set_input(lctx.inp_KQ_mask_cross);
        cb(lctx.inp_KQ_mask_cross, "KQ_mask_cross", -1);
        return lctx.inp_KQ_mask_cross;
    }

    struct lm_ggml_cgraph * build_llama() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        // mutable variable, needed during the last layer of the computation to skip unused tokens
        int32_t n_tokens = this->n_tokens;

        const int64_t n_embd_head = hparams.n_embd_head_v;
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);
        LM_GGML_ASSERT(n_embd_head == hparams.n_rot);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        const float kq_scale = hparams.f_attention_scale == 0.0f ? 1.0f/sqrtf(float(n_embd_head)) : hparams.f_attention_scale;
        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            // norm
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                // rope freq factors for llama3; may return nullptr for llama2 and other models
                struct lm_ggml_tensor * rope_factors = build_rope_factors(il);

                // compute Q and K and RoPE them
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur);
                cb(Qcur, "Qcur", il);
                if (model.layers[il].bq) {
                    Qcur = lm_ggml_add(ctx0, Qcur, model.layers[il].bq);
                    cb(Qcur, "Qcur", il);
                }

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur);
                cb(Kcur, "Kcur", il);
                if (model.layers[il].bk) {
                    Kcur = lm_ggml_add(ctx0, Kcur, model.layers[il].bk);
                    cb(Kcur, "Kcur", il);
                }

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur);
                cb(Vcur, "Vcur", il);
                if (model.layers[il].bv) {
                    Vcur = lm_ggml_add(ctx0, Vcur, model.layers[il].bv);
                    cb(Vcur, "Vcur", il);
                }

                Qcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens), inp_pos, rope_factors,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens), inp_pos, rope_factors,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, model.layers[il].bo,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, kq_scale, cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                n_tokens = n_outputs;
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            // For Granite architecture
            if (hparams.f_residual_scale) {
                cur = lm_ggml_scale(ctx0, cur, hparams.f_residual_scale);
            }

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);

            // feed-forward network
            if (model.layers[il].ffn_gate_inp == nullptr) {
                cur = llm_build_norm(ctx0, ffn_inp, hparams,
                        model.layers[il].ffn_norm, NULL,
                        LLM_NORM_RMS, cb, il);
                cb(cur, "ffn_norm", il);

                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   model.layers[il].ffn_up_b,   NULL,
                        model.layers[il].ffn_gate, model.layers[il].ffn_gate_b, NULL,
                        model.layers[il].ffn_down, model.layers[il].ffn_down_b, NULL,
                        NULL,
                        LLM_FFN_SILU, LLM_FFN_PAR, cb, il);
                cb(cur, "ffn_out", il);
            } else {
                // MoE branch
                cur = llm_build_norm(ctx0, ffn_inp, hparams,
                        model.layers[il].ffn_norm, NULL,
                        LLM_NORM_RMS, cb, il);
                cb(cur, "ffn_norm", il);

                cur = llm_build_moe_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_gate_inp,
                        model.layers[il].ffn_up_exps,
                        model.layers[il].ffn_gate_exps,
                        model.layers[il].ffn_down_exps,
                        n_expert, n_expert_used,
                        LLM_FFN_SILU, true,
                        false, 0.0,
                        cb, il);
                cb(cur, "ffn_moe_out", il);
            }

            // For Granite architecture
            if (hparams.f_residual_scale) {
                cur = lm_ggml_scale(ctx0, cur, hparams.f_residual_scale);
            }

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cb(cur, "ffn_out", il);

            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, NULL,
                LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);

        // For Granite architecture
        if (hparams.f_logit_scale) {
            cur = lm_ggml_scale(ctx0, cur, 1.0f / hparams.f_logit_scale);
        }

        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_baichuan() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);
        LM_GGML_ASSERT(n_embd_head == hparams.n_rot);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = model.type == MODEL_7B ? build_inp_pos() : nullptr;

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur);
                cb(Qcur, "Qcur", il);

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur);
                cb(Kcur, "Kcur", il);

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur);
                cb(Vcur, "Vcur", il);

                switch (model.type) {
                    case MODEL_7B:
                        Qcur = lm_ggml_rope_ext(
                            ctx0, lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens), inp_pos, nullptr,
                            n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                            ext_factor, attn_factor, beta_fast, beta_slow
                        );
                        Kcur = lm_ggml_rope_ext(
                            ctx0, lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens), inp_pos, nullptr,
                            n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                            ext_factor, attn_factor, beta_fast, beta_slow
                        );
                        break;
                    case MODEL_13B:
                        Qcur = lm_ggml_reshape_3d(ctx0, Qcur, n_embd/n_head, n_head, n_tokens);
                        Kcur = lm_ggml_reshape_3d(ctx0, Kcur, n_embd/n_head, n_head, n_tokens);
                        break;
                    default:
                        LM_GGML_ABORT("fatal error");
                }
                cb(Qcur, "Qcur", il);
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, NULL,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);

            // feed-forward network
            {
                cur = llm_build_norm(ctx0, ffn_inp, hparams,
                        model.layers[il].ffn_norm, NULL,
                        LLM_NORM_RMS, cb, il);
                cb(cur, "ffn_norm", il);

                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   NULL, NULL,
                        model.layers[il].ffn_gate, NULL, NULL,
                        model.layers[il].ffn_down, NULL, NULL,
                        NULL,
                        LLM_FFN_SILU, LLM_FFN_PAR, cb, il);
                cb(cur, "ffn_out", il);
            }

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, NULL,
                LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_xverse() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);
        LM_GGML_ASSERT(n_embd_head == hparams.n_rot);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur);
                cb(Qcur, "Qcur", il);

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur);
                cb(Kcur, "Kcur", il);

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur);
                cb(Vcur, "Vcur", il);

                Qcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);
                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, NULL,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur   = lm_ggml_get_rows(ctx0,      cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);

            // feed-forward network
            {
                cur = llm_build_norm(ctx0, ffn_inp, hparams,
                        model.layers[il].ffn_norm, NULL,
                        LLM_NORM_RMS, cb, il);
                cb(cur, "ffn_norm", il);

                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   NULL, NULL,
                        model.layers[il].ffn_gate, NULL, NULL,
                        model.layers[il].ffn_down, NULL, NULL,
                        NULL,
                        LLM_FFN_SILU, LLM_FFN_PAR, cb, il);
                cb(cur, "ffn_out", il);
            }

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams, model.output_norm, NULL, LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_falcon() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        const int64_t n_embd_gqa  = hparams.n_embd_v_gqa();
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);
        LM_GGML_ASSERT(n_embd_head == hparams.n_rot);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * attn_norm;

            attn_norm = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm,
                    model.layers[il].attn_norm_b,
                    LLM_NORM, cb, il);
            cb(attn_norm, "attn_norm", il);

            // self-attention
            {
                if (model.layers[il].attn_norm_2) {
                    // Falcon-40B
                    cur = llm_build_norm(ctx0, inpL, hparams,
                            model.layers[il].attn_norm_2,
                            model.layers[il].attn_norm_2_b,
                            LLM_NORM, cb, il);
                    cb(cur, "attn_norm_2", il);
                } else {
                    cur = attn_norm;
                }

                cur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wqkv, cur);
                cb(cur, "wqkv", il);

                struct lm_ggml_tensor * Qcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd,     n_tokens, cur->nb[1], 0*sizeof(float)*(n_embd)));
                struct lm_ggml_tensor * Kcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*sizeof(float)*(n_embd)));
                struct lm_ggml_tensor * Vcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*sizeof(float)*(n_embd + n_embd_gqa)));

                cb(Qcur, "Qcur", il);
                cb(Kcur, "Kcur", il);
                cb(Vcur, "Vcur", il);

                Qcur = lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head,    n_tokens);
                Kcur = lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens);

                // using mode = 2 for neox mode
                Qcur = lm_ggml_rope_ext(
                    ctx0, Qcur, inp_pos, nullptr, n_rot, rope_type, n_ctx_orig,
                    freq_base, freq_scale, ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, Kcur, inp_pos, nullptr, n_rot, rope_type, n_ctx_orig,
                    freq_base, freq_scale, ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, NULL,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur       = lm_ggml_get_rows(ctx0,       cur, inp_out_ids);
                inpL      = lm_ggml_get_rows(ctx0,      inpL, inp_out_ids);
                attn_norm = lm_ggml_get_rows(ctx0, attn_norm, inp_out_ids);
            }

            struct lm_ggml_tensor * ffn_inp = cur;

            // feed forward
            {
                cur = llm_build_ffn(ctx0, lctx, attn_norm, // !! use the attn norm, not the result
                        model.layers[il].ffn_up,   NULL, NULL,
                        NULL,                      NULL, NULL,
                        model.layers[il].ffn_down, NULL, NULL,
                        NULL,
                        LLM_FFN_GELU, LLM_FFN_SEQ, cb, il);
                cb(cur, "ffn_out", il);
            }

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cur = lm_ggml_add(ctx0, cur, inpL);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        // norm
        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm,
                model.output_norm_b,
                LLM_NORM, cb, -1);
        cb(cur, "result_norm", -1);

        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_grok() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        // mutable variable, needed during the last layer of the computation to skip unused tokens
        int32_t n_tokens = this->n_tokens;

        const int64_t n_embd_head = hparams.n_embd_head_v;
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);
        LM_GGML_ASSERT(n_embd_head == hparams.n_rot);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // multiply by embedding_multiplier_scale of 78.38367176906169
        inpL = lm_ggml_scale(ctx0, inpL, 78.38367176906169f);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            // norm
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_norm", il);


            // self-attention
            {
                // compute Q and K and RoPE them
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur);
                cb(Qcur, "Qcur", il);
                if (model.layers[il].bq) {
                    Qcur = lm_ggml_add(ctx0, Qcur, model.layers[il].bq);
                    cb(Qcur, "Qcur", il);
                }

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur);
                cb(Kcur, "Kcur", il);
                if (model.layers[il].bk) {
                    Kcur = lm_ggml_add(ctx0, Kcur, model.layers[il].bk);
                    cb(Kcur, "Kcur", il);
                }

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur);
                cb(Vcur, "Vcur", il);
                if (model.layers[il].bv) {
                    Vcur = lm_ggml_add(ctx0, Vcur, model.layers[il].bv);
                    cb(Vcur, "Vcur", il);
                }

                Qcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, model.layers[il].bo,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f, cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                n_tokens = n_outputs;
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            // Grok
            // if attn_out_norm is present then apply it before adding the input
            if (model.layers[il].attn_out_norm) {
                cur = llm_build_norm(ctx0, cur, hparams,
                        model.layers[il].attn_out_norm, NULL,
                        LLM_NORM_RMS, cb, il);
                cb(cur, "attn_out_norm", il);
            }

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);

            // feed-forward network
            // MoE branch
            cur = llm_build_norm(ctx0, ffn_inp, hparams,
                    model.layers[il].ffn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "ffn_norm", il);

            cur = llm_build_moe_ffn(ctx0, lctx, cur,
                    model.layers[il].ffn_gate_inp,
                    model.layers[il].ffn_up_exps,
                    model.layers[il].ffn_gate_exps,
                    model.layers[il].ffn_down_exps,
                    n_expert, n_expert_used,
                    LLM_FFN_GELU, true,
                    false, 0.0,
                    cb, il);
            cb(cur, "ffn_moe_out", il);

            // Grok
            // if layer_out_norm is present then apply it before adding the input
            // Idea: maybe ffn_out_norm is a better name
            if (model.layers[il].layer_out_norm) {
                cur = llm_build_norm(ctx0, cur, hparams,
                        model.layers[il].layer_out_norm, NULL,
                        LLM_NORM_RMS, cb, il);
                cb(cur, "layer_out_norm", il);
            }

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cb(cur, "ffn_out", il);

            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, NULL,
                LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);

        // Grok
        // multiply logits by output_multiplier_scale of 0.5773502691896257

        cur = lm_ggml_scale(ctx0, cur, 0.5773502691896257f);

        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_dbrx() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        // mutable variable, needed during the last layer of the computation to skip unused tokens
        int32_t n_tokens = this->n_tokens;

        const int64_t n_embd_head = hparams.n_embd_head_v;
        const int64_t n_embd_gqa  = hparams.n_embd_v_gqa();
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);
        LM_GGML_ASSERT(n_embd_head == hparams.n_rot);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            // norm
            cur = llm_build_norm(ctx0, inpL, hparams,
                                 model.layers[il].attn_norm, NULL,
                                 LLM_NORM, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                struct lm_ggml_tensor * Qcur = nullptr;
                struct lm_ggml_tensor * Kcur = nullptr;
                struct lm_ggml_tensor * Vcur = nullptr;

                cur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wqkv, cur);
                cb(cur, "wqkv", il);

                cur = lm_ggml_clamp(ctx0, cur, -hparams.f_clamp_kqv, hparams.f_clamp_kqv);
                cb(cur, "wqkv_clamped", il);

                Qcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd,     n_tokens, cur->nb[1], 0*sizeof(float)*(n_embd)));
                Kcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*sizeof(float)*(n_embd)));
                Vcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*sizeof(float)*(n_embd + n_embd_gqa)));

                cb(Qcur, "Qcur", il);
                cb(Kcur, "Kcur", il);
                cb(Vcur, "Vcur", il);

                Qcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, NULL,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                n_tokens = n_outputs;
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);

            // feed-forward network
            // MoE branch
            cur = llm_build_norm(ctx0, ffn_inp, hparams,
                                 model.layers[il].attn_out_norm, NULL,
                                 LLM_NORM, cb, il);
            cb(cur, "attn_out_norm", il);

            cur = llm_build_moe_ffn(ctx0, lctx, cur,
                    model.layers[il].ffn_gate_inp,
                    model.layers[il].ffn_up_exps,
                    model.layers[il].ffn_gate_exps,
                    model.layers[il].ffn_down_exps,
                    n_expert, n_expert_used,
                    LLM_FFN_SILU, true,
                    false, 0.0,
                    cb, il);
            cb(cur, "ffn_moe_out", il);

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cb(cur, "ffn_out", il);

            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                             model.output_norm, NULL,
                             LLM_NORM, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);

        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_starcoder() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        const int64_t n_embd_gqa  = hparams.n_embd_v_gqa();
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        struct lm_ggml_tensor * pos = lm_ggml_get_rows(ctx0, model.pos_embd, inp_pos);
        cb(pos, "pos_embd", -1);

        inpL = lm_ggml_add(ctx0, inpL, pos);
        cb(inpL, "inpL", -1);

        for (int il = 0; il < n_layer; ++il) {
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm,
                    model.layers[il].attn_norm_b,
                    LLM_NORM, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                cur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wqkv, cur);
                cb(cur, "wqkv", il);

                cur = lm_ggml_add(ctx0, cur, model.layers[il].bqkv);
                cb(cur, "bqkv", il);

                struct lm_ggml_tensor * Qcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd,     n_tokens, cur->nb[1], 0*sizeof(float)*(n_embd)));
                struct lm_ggml_tensor * Kcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*sizeof(float)*(n_embd)));
                struct lm_ggml_tensor * Vcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*sizeof(float)*(n_embd + n_embd_gqa)));

                cb(Qcur, "Qcur", il);
                cb(Kcur, "Kcur", il);
                cb(Vcur, "Vcur", il);

                Qcur = lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, model.layers[il].bo,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur  = lm_ggml_get_rows(ctx0,  cur, inp_out_ids);
                inpL = lm_ggml_get_rows(ctx0, inpL, inp_out_ids);
            }

            // add the input
            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpL);
            cb(ffn_inp, "ffn_inp", il);

            // FF
            {
                cur = llm_build_norm(ctx0, ffn_inp, hparams,
                        model.layers[il].ffn_norm,
                        model.layers[il].ffn_norm_b,
                        LLM_NORM, cb, il);
                cb(cur, "ffn_norm", il);

                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   model.layers[il].ffn_up_b,   NULL,
                        NULL,                      NULL,                        NULL,
                        model.layers[il].ffn_down, model.layers[il].ffn_down_b, NULL,
                        NULL,
                        LLM_FFN_GELU, LLM_FFN_SEQ, cb, il);
                cb(cur, "ffn_out", il);
            }

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = llm_build_norm(ctx0, inpL, hparams,
                model.output_norm,
                model.output_norm_b,
                LLM_NORM, cb, -1);
        cb(cur, "result_norm", -1);

        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_refact() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur);
                cb(Qcur, "Qcur", il);

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur);
                cb(Kcur, "Kcur", il);

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur);
                cb(Vcur, "Vcur", il);

                Kcur = lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens);
                cb(Kcur, "Kcur", il);

                Qcur = lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head,    n_tokens);
                cb(Qcur, "Qcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, NULL,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);

            // feed-forward network
            {
                cur = llm_build_norm(ctx0, ffn_inp, hparams,
                        model.layers[il].ffn_norm, NULL,
                        LLM_NORM_RMS, cb, il);
                cb(cur, "ffn_norm", il);

                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   NULL, NULL,
                        model.layers[il].ffn_gate, NULL, NULL,
                        model.layers[il].ffn_down, NULL, NULL,
                        NULL,
                        LLM_FFN_SILU, LLM_FFN_PAR, cb, il);
                cb(cur, "ffn_out", il);
            }

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, NULL,
                LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_bert() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        const int64_t n_embd_gqa  = hparams.n_embd_v_gqa();

        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;
        struct lm_ggml_tensor * inp_pos = nullptr;

        if (model.arch != LLM_ARCH_JINA_BERT_V2) {
            inp_pos = build_inp_pos();
        }

        // construct input embeddings (token, type, position)
        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // token types are hardcoded to zero ("Sentence A")
        struct lm_ggml_tensor * type_row0 = lm_ggml_view_1d(ctx0, model.type_embd, n_embd, 0);
        inpL = lm_ggml_add(ctx0, inpL, type_row0);
        if (model.arch == LLM_ARCH_BERT) {
            inpL = lm_ggml_add(ctx0, lm_ggml_get_rows(ctx0, model.pos_embd, inp_pos), inpL);
        }
        cb(inpL, "inp_embd", -1);

        // embed layer norm
        inpL = llm_build_norm(ctx0, inpL, hparams, model.tok_norm, model.tok_norm_b, LLM_NORM, cb, -1);
        cb(inpL, "inp_norm", -1);

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask(false);

        // iterate layers
        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * cur = inpL;

            struct lm_ggml_tensor * Qcur;
            struct lm_ggml_tensor * Kcur;
            struct lm_ggml_tensor * Vcur;

            // self-attention
            if (model.arch == LLM_ARCH_BERT || model.arch == LLM_ARCH_JINA_BERT_V2) {
                Qcur = lm_ggml_add(ctx0, llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur), model.layers[il].bq);
                cb(Qcur, "Qcur", il);

                if (model.layers[il].attn_q_norm) {
                    Qcur = llm_build_norm(ctx0, Qcur, hparams,
                            model.layers[il].attn_q_norm,
                            model.layers[il].attn_q_norm_b,
                            LLM_NORM, cb, il);
                }

                Kcur = lm_ggml_add(ctx0, llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur), model.layers[il].bk);
                cb(Kcur, "Kcur", il);

                if (model.layers[il].attn_k_norm) {
                    Kcur = llm_build_norm(ctx0, Kcur, hparams,
                            model.layers[il].attn_k_norm,
                            model.layers[il].attn_k_norm_b,
                            LLM_NORM, cb, il);
                }
                Vcur = lm_ggml_add(ctx0, llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur), model.layers[il].bv);
                cb(Vcur, "Vcur", il);

                Qcur = lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head,    n_tokens);
                Kcur = lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens);
            } else {
                // compute Q and K and RoPE them
                cur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wqkv, cur);
                cb(cur, "wqkv", il);

                Qcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd,     n_tokens, cur->nb[1], 0*sizeof(float)*(n_embd)));
                Kcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*sizeof(float)*(n_embd)));
                Vcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*sizeof(float)*(n_embd + n_embd_gqa)));

                cb(Qcur, "Qcur", il);
                cb(Kcur, "Kcur", il);
                cb(Vcur, "Vcur", il);

                Qcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head,    n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);
            }

            struct lm_ggml_tensor * q =                 lm_ggml_permute(ctx0, Qcur, 0, 2, 1, 3);
            struct lm_ggml_tensor * k = lm_ggml_cont(ctx0, lm_ggml_permute(ctx0, Kcur, 0, 2, 1, 3));

            struct lm_ggml_tensor * kq = lm_ggml_mul_mat(ctx0, k, q);
            cb(kq, "kq", il);

            kq = lm_ggml_soft_max_ext(ctx0, kq, KQ_mask, 1.0f/sqrtf(float(n_embd_head)), hparams.f_max_alibi_bias);
            cb(kq, "kq_soft_max_ext", il);

            struct lm_ggml_tensor * v = lm_ggml_cont(ctx0, lm_ggml_transpose(ctx0, lm_ggml_reshape_2d(ctx0, Vcur, n_embd_gqa, n_tokens)));
            cb(v, "v", il);

            struct lm_ggml_tensor * kqv = lm_ggml_mul_mat(ctx0, lm_ggml_reshape_3d(ctx0, v, n_tokens, n_embd_head, n_head_kv), kq);
            cb(kqv, "kqv", il);

            struct lm_ggml_tensor * kqv_merged = lm_ggml_permute(ctx0, kqv, 0, 2, 1, 3);
            cb(kqv_merged, "kqv_merged", il);

            cur = lm_ggml_cont_2d(ctx0, kqv_merged, n_embd_gqa, n_tokens);
            cb(cur, "kqv_merged_cont", il);

            lm_ggml_build_forward_expand(gf, cur);

            cur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wo, cur);
            if (model.layers[il].bo) {
                cb(cur, "kqv_wo", il);
            }

            if (model.layers[il].bo) {
                cur = lm_ggml_add(ctx0, cur, model.layers[il].bo);
            }
            cb(cur, "kqv_out", il);

            if (il == n_layer - 1 && pooling_type == LLAMA_POOLING_TYPE_NONE) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur  = lm_ggml_get_rows(ctx0,  cur, inp_out_ids);
                inpL = lm_ggml_get_rows(ctx0, inpL, inp_out_ids);
            }

            // re-add the layer input
            cur = lm_ggml_add(ctx0, cur, inpL);

            // attention layer norm
            cur = llm_build_norm(ctx0, cur, hparams, model.layers[il].attn_out_norm, model.layers[il].attn_out_norm_b, LLM_NORM, cb, il);

            if (model.layers[il].attn_norm_2 != nullptr) {
                cur = lm_ggml_add(ctx0, cur, inpL); // re-add the layer input
                cur = llm_build_norm(ctx0, cur, hparams, model.layers[il].attn_norm_2, model.layers[il].attn_norm_2_b, LLM_NORM, cb, il);
            }

            struct lm_ggml_tensor * ffn_inp = cur;
            cb(ffn_inp, "ffn_inp", il);

            // feed-forward network
            if (model.arch == LLM_ARCH_BERT) {
                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   model.layers[il].ffn_up_b,   NULL,
                        NULL,                      NULL,                        NULL,
                        model.layers[il].ffn_down, model.layers[il].ffn_down_b, NULL,
                        NULL,
                        LLM_FFN_GELU, LLM_FFN_SEQ, cb, il);
            } else if (model.arch == LLM_ARCH_JINA_BERT_V2) {
                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   NULL,                        NULL,
                        model.layers[il].ffn_gate, NULL,                        NULL,
                        model.layers[il].ffn_down, model.layers[il].ffn_down_b, NULL,
                        NULL,
                        LLM_FFN_GELU, LLM_FFN_PAR, cb, il);
            } else {
                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   NULL, NULL,
                        model.layers[il].ffn_gate, NULL, NULL,
                        model.layers[il].ffn_down, NULL, NULL,
                        NULL,
                        LLM_FFN_SILU, LLM_FFN_PAR, cb, il);
            }
            cb(cur, "ffn_out", il);

            // attentions bypass the intermediate layer
            cur = lm_ggml_add(ctx0, cur, ffn_inp);

            // output layer norm
            cur = llm_build_norm(ctx0, cur, hparams, model.layers[il].layer_out_norm, model.layers[il].layer_out_norm_b, LLM_NORM, cb, il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cb(cur, "result_embd", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_bloom() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        const int64_t n_embd_gqa  = hparams.n_embd_v_gqa();
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        inpL = llm_build_norm(ctx0, inpL, hparams,
                model.tok_norm,
                model.tok_norm_b,
                LLM_NORM, cb, -1);
        cb(inpL, "inp_norm", -1);

        for (int il = 0; il < n_layer; ++il) {
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm,
                    model.layers[il].attn_norm_b,
                    LLM_NORM, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                cur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wqkv, cur);
                cb(cur, "wqkv", il);

                cur = lm_ggml_add(ctx0, cur, model.layers[il].bqkv);
                cb(cur, "bqkv", il);

                struct lm_ggml_tensor * Qcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd,     n_tokens, cur->nb[1], 0*sizeof(float)*(n_embd)));
                struct lm_ggml_tensor * Kcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*sizeof(float)*(n_embd)));
                struct lm_ggml_tensor * Vcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*sizeof(float)*(n_embd + n_embd_gqa)));

                cb(Qcur, "Qcur", il);
                cb(Kcur, "Kcur", il);
                cb(Vcur, "Vcur", il);

                Qcur = lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, model.layers[il].bo,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur  = lm_ggml_get_rows(ctx0,  cur, inp_out_ids);
                inpL = lm_ggml_get_rows(ctx0, inpL, inp_out_ids);
            }

            // Add the input
            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpL);
            cb(ffn_inp, "ffn_inp", il);

            // FF
            {
                cur = llm_build_norm(ctx0, ffn_inp, hparams,
                        model.layers[il].ffn_norm,
                        model.layers[il].ffn_norm_b,
                        LLM_NORM, cb, il);
                cb(cur, "ffn_norm", il);

                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   model.layers[il].ffn_up_b,   NULL,
                        NULL,                      NULL,                        NULL,
                        model.layers[il].ffn_down, model.layers[il].ffn_down_b, NULL,
                        NULL,
                        LLM_FFN_GELU, LLM_FFN_SEQ, cb, il);
                cb(cur, "ffn_out", il);
            }

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = llm_build_norm(ctx0, inpL, hparams,
                model.output_norm,
                model.output_norm_b,
                LLM_NORM, cb, -1);
        cb(cur, "result_norm", -1);

        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_mpt() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        const int64_t n_embd_gqa  = hparams.n_embd_v_gqa();
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * pos;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        if (model.pos_embd) {
            // inp_pos - contains the positions
            struct lm_ggml_tensor * inp_pos = build_inp_pos();
            pos = lm_ggml_get_rows(ctx0, model.pos_embd, inp_pos);
            cb(pos, "pos_embd", -1);

            inpL = lm_ggml_add(ctx0, inpL, pos);
            cb(inpL, "inpL", -1);
        }

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * attn_norm;

            attn_norm = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm,
                    model.layers[il].attn_norm_b,
                    LLM_NORM, cb, il);
            cb(attn_norm, "attn_norm", il);

            // self-attention
            {
                cur = attn_norm;

                cur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wqkv, cur);
                cb(cur, "wqkv", il);

                if (model.layers[il].bqkv){
                    cur = lm_ggml_add(ctx0, cur, model.layers[il].bqkv);
                    cb(cur, "bqkv", il);
                }

                if (hparams.f_clamp_kqv > 0.0f) {
                    cur = lm_ggml_clamp(ctx0, cur, -hparams.f_clamp_kqv, hparams.f_clamp_kqv);
                    cb(cur, "wqkv_clamped", il);
                }

                struct lm_ggml_tensor * Qcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd,     n_tokens, cur->nb[1], 0*sizeof(float)*(n_embd)));
                struct lm_ggml_tensor * Kcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*sizeof(float)*(n_embd)));
                struct lm_ggml_tensor * Vcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*sizeof(float)*(n_embd + n_embd_gqa)));

                cb(Qcur, "Qcur", il);
                cb(Kcur, "Kcur", il);
                cb(Vcur, "Vcur", il);

                // Q/K Layernorm
                if (model.layers[il].attn_q_norm) {
                    Qcur = llm_build_norm(ctx0, Qcur, hparams,
                            model.layers[il].attn_q_norm,
                            model.layers[il].attn_q_norm_b,
                            LLM_NORM, cb, il);
                    cb(Qcur, "Qcur", il);

                    Kcur = llm_build_norm(ctx0, Kcur, hparams,
                            model.layers[il].attn_k_norm,
                            model.layers[il].attn_k_norm_b,
                            LLM_NORM, cb, il);
                    cb(Kcur, "Kcur", il);

                    Qcur = lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head,    n_tokens);
                    Kcur = lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens);

                    cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                            model.layers[il].wo, model.layers[il].bo,
                            Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
                } else {
                    Qcur = lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens);

                    cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                            model.layers[il].wo, model.layers[il].bo,
                            Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
                }
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur  = lm_ggml_get_rows(ctx0,  cur, inp_out_ids);
                inpL = lm_ggml_get_rows(ctx0, inpL, inp_out_ids);
            }

            // Add the input
            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpL);
            cb(ffn_inp, "ffn_inp", il);

            // feed forward
            {
                cur = llm_build_norm(ctx0, ffn_inp, hparams,
                        model.layers[il].ffn_norm,
                        model.layers[il].ffn_norm_b,
                        LLM_NORM, cb, il);
                cb(cur, "ffn_norm", il);
                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   model.layers[il].ffn_up_b,   NULL,
                        NULL,                      NULL,                        NULL,
                        model.layers[il].ffn_down, model.layers[il].ffn_down_b, NULL,
                        model.layers[il].ffn_act,
                        LLM_FFN_GELU, LLM_FFN_SEQ, cb, il);
                cb(cur, "ffn_out", il);
            }

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm,
                model.output_norm_b,
                LLM_NORM, cb, -1);
        cb(cur, "result_norm", -1);

        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_stablelm() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph(ctx0);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {


            // norm
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm,
                    model.layers[il].attn_norm_b,
                    LLM_NORM, cb, il);
            cb(cur, "attn_norm", il);

            struct lm_ggml_tensor * inpSA = cur;

            // self-attention
            {
                // compute Q and K and RoPE them
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur);
                cb(Qcur, "Qcur", il);
                if (model.layers[il].bq) {
                    Qcur = lm_ggml_add(ctx0, Qcur, model.layers[il].bq);
                    cb(Qcur, "Qcur", il);
                }

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur);
                cb(Kcur, "Kcur", il);
                if (model.layers[il].bk) {
                    Kcur = lm_ggml_add(ctx0, Kcur, model.layers[il].bk);
                    cb(Kcur, "Kcur", il);
                }

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur);
                cb(Vcur, "Vcur", il);
                if (model.layers[il].bv) {
                    Vcur = lm_ggml_add(ctx0, Vcur, model.layers[il].bv);
                    cb(Vcur, "Vcur", il);
                }

                Qcur = lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head,    n_tokens);
                cb(Qcur, "Qcur", il);
                Kcur = lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens);
                cb(Kcur, "Kcur", il);

                if (model.layers[il].attn_q_norm) {
                    Qcur = llm_build_norm(ctx0, Qcur, hparams,
                            model.layers[il].attn_q_norm,
                            NULL,
                            LLM_NORM, cb, il);
                    cb(Qcur, "Qcur", il);
                }
                if (model.layers[il].attn_k_norm) {
                    Kcur = llm_build_norm(ctx0, Kcur, hparams,
                            model.layers[il].attn_k_norm,
                            NULL,
                            LLM_NORM, cb, il);
                    cb(Kcur, "Kcur", il);
                }


                Qcur = lm_ggml_rope_ext(
                    ctx0, Qcur, inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, Kcur, inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, NULL,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpL  = lm_ggml_get_rows(ctx0,  inpL, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpL);
            cb(ffn_inp, "ffn_inp", il);

            // feed-forward network
            {
                if (model.layers[il].ffn_norm) {
                    cur = llm_build_norm(ctx0, ffn_inp, hparams,
                            model.layers[il].ffn_norm,
                            model.layers[il].ffn_norm_b,
                            LLM_NORM, cb, il);
                    cb(cur, "ffn_norm", il);
                } else {
                    // parallel residual
                    cur = inpSA;
                }
                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   NULL, NULL,
                        model.layers[il].ffn_gate, NULL, NULL,
                        model.layers[il].ffn_down, NULL, NULL,
                        NULL,
                        LLM_FFN_SILU, LLM_FFN_PAR, cb, il);
                cb(cur, "ffn_out", il);
            }

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm,
                model.output_norm_b,
                LLM_NORM, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_qwen() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                cur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wqkv, cur);
                cb(cur, "wqkv", il);

                cur = lm_ggml_add(ctx0, cur, model.layers[il].bqkv);
                cb(cur, "bqkv", il);

                struct lm_ggml_tensor * Qcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd, n_tokens, cur->nb[1], 0*sizeof(float)*(n_embd)));
                struct lm_ggml_tensor * Kcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd, n_tokens, cur->nb[1], 1*sizeof(float)*(n_embd)));
                struct lm_ggml_tensor * Vcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd, n_tokens, cur->nb[1], 2*sizeof(float)*(n_embd)));

                cb(Qcur, "Qcur", il);
                cb(Kcur, "Kcur", il);
                cb(Vcur, "Vcur", il);

                Qcur = lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head,    n_tokens);
                Kcur = lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens);

                // using mode = 2 for neox mode
                Qcur = lm_ggml_rope_ext(
                    ctx0, Qcur, inp_pos, nullptr, n_rot, rope_type, n_ctx_orig,
                    freq_base, freq_scale, ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, Kcur, inp_pos, nullptr, n_rot, rope_type, n_ctx_orig,
                    freq_base, freq_scale, ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, NULL,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);

            // feed-forward forward
            {
                cur = llm_build_norm(ctx0, ffn_inp, hparams,
                        model.layers[il].ffn_norm, NULL,
                        LLM_NORM_RMS, cb, il);
                cb(cur, "ffn_norm", il);

                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   NULL, NULL,
                        model.layers[il].ffn_gate, NULL, NULL,
                        model.layers[il].ffn_down, NULL, NULL,
                        NULL,
                        LLM_FFN_SILU, LLM_FFN_PAR, cb, il);
                cb(cur, "ffn_out", il);
            }

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, NULL,
                LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_qwen2() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);
        LM_GGML_ASSERT(n_embd_head == hparams.n_rot);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            // norm
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                // compute Q and K and RoPE them
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur);
                cb(Qcur, "Qcur", il);
                Qcur = lm_ggml_add(ctx0, Qcur, model.layers[il].bq);
                cb(Qcur, "Qcur", il);

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur);
                cb(Kcur, "Kcur", il);
                Kcur = lm_ggml_add(ctx0, Kcur, model.layers[il].bk);
                cb(Kcur, "Kcur", il);

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur);
                cb(Vcur, "Vcur", il);
                Vcur = lm_ggml_add(ctx0, Vcur, model.layers[il].bv);
                cb(Vcur, "Vcur", il);

                Qcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head,    n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, model.layers[il].bo,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);

            // feed-forward network
            cur = llm_build_norm(ctx0, ffn_inp, hparams,
                    model.layers[il].ffn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "ffn_norm", il);

            cur = llm_build_ffn(ctx0, lctx, cur,
                    model.layers[il].ffn_up,   NULL, NULL,
                    model.layers[il].ffn_gate, NULL, NULL,
                    model.layers[il].ffn_down, NULL, NULL,
                    NULL,
                    LLM_FFN_SILU, LLM_FFN_PAR, cb, il);
            cb(cur, "ffn_out", il);

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, NULL,
                LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_qwen2moe() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        // mutable variable, needed during the last layer of the computation to skip unused tokens
        int32_t n_tokens = this->n_tokens;

        const int64_t n_embd_head = hparams.n_embd_head_v;
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);
        LM_GGML_ASSERT(n_embd_head == hparams.n_rot);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            // norm
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_norm", il);

            // self_attention
            {
                // compute Q and K and RoPE them
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur);
                cb(Qcur, "Qcur", il);
                Qcur = lm_ggml_add(ctx0, Qcur, model.layers[il].bq);
                cb(Qcur, "Qcur", il);

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur);
                cb(Kcur, "Kcur", il);
                Kcur = lm_ggml_add(ctx0, Kcur, model.layers[il].bk);
                cb(Kcur, "Kcur", il);

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur);
                cb(Vcur, "Vcur", il);
                Vcur = lm_ggml_add(ctx0, Vcur, model.layers[il].bv);
                cb(Vcur, "Vcur", il);

                Qcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, model.layers[il].bo,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                n_tokens = n_outputs;
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);

            // MoE branch
            cur = llm_build_norm(ctx0, ffn_inp, hparams,
                    model.layers[il].ffn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "ffn_norm", il);

            lm_ggml_tensor * moe_out =
                    llm_build_moe_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_gate_inp,
                        model.layers[il].ffn_up_exps,
                        model.layers[il].ffn_gate_exps,
                        model.layers[il].ffn_down_exps,
                        n_expert, n_expert_used,
                        LLM_FFN_SILU, false,
                        false, 0.0,
                        cb, il);
            cb(cur, "ffn_moe_out", il);

            // FFN shared expert
            {
                lm_ggml_tensor * cur_gate_inp = llm_build_lora_mm(lctx, ctx0, model.layers[il].ffn_gate_inp_shexp, cur);
                cb(cur_gate_inp, "ffn_shexp_gate_inp", il);

                // sigmoid
                lm_ggml_tensor * cur_gate = lm_ggml_div(ctx0, lm_ggml_silu(ctx0, cur_gate_inp), cur_gate_inp);
                cb(cur_gate, "ffn_shexp_gate", il);

                lm_ggml_tensor * cur_ffn = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up_shexp,   NULL, NULL,
                        model.layers[il].ffn_gate_shexp, NULL, NULL,
                        model.layers[il].ffn_down_shexp, NULL, NULL,
                        NULL,
                        LLM_FFN_SILU, LLM_FFN_PAR, cb, il);
                cb(cur_ffn, "ffn_shexp", il);

                lm_ggml_tensor * ffn_shexp_out = lm_ggml_mul(ctx0, cur_ffn, cur_gate);
                cb(ffn_shexp_out, "ffn_shexp_out", il);

                moe_out = lm_ggml_add(ctx0, moe_out, ffn_shexp_out);
                cb(moe_out, "ffn_out", il);

                cur = moe_out;
            }

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, NULL,
                LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_phi2() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        const int64_t n_embd_gqa  = hparams.n_embd_v_gqa();
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * attn_norm_output;
        struct lm_ggml_tensor * ffn_output;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            attn_norm_output = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm,
                    model.layers[il].attn_norm_b,
                    LLM_NORM, cb, il);
            cb(attn_norm_output, "attn_norm", il);

            // self-attention
            {
                struct lm_ggml_tensor * Qcur = nullptr;
                struct lm_ggml_tensor * Kcur = nullptr;
                struct lm_ggml_tensor * Vcur = nullptr;

                if (model.layers[il].wqkv) {
                    cur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wqkv, attn_norm_output);
                    cb(cur, "wqkv", il);

                    cur = lm_ggml_add(ctx0, cur, model.layers[il].bqkv);
                    cb(cur, "bqkv", il);

                    Qcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd,     n_tokens, cur->nb[1], 0*sizeof(float)*(n_embd)));
                    Kcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*sizeof(float)*(n_embd)));
                    Vcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*sizeof(float)*(n_embd + n_embd_gqa)));
                } else {
                    Qcur = lm_ggml_add(ctx0, llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, attn_norm_output), model.layers[il].bq);
                    Kcur = lm_ggml_add(ctx0, llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, attn_norm_output), model.layers[il].bk);
                    Vcur = lm_ggml_add(ctx0, llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, attn_norm_output), model.layers[il].bv);
                }

                cb(Qcur, "Qcur", il);
                cb(Kcur, "Kcur", il);
                cb(Vcur, "Vcur", il);

                Qcur = lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head,    n_tokens);
                Kcur = lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens);

                Qcur = lm_ggml_rope_ext(
                    ctx0, Qcur, inp_pos, nullptr, n_rot, rope_type, n_ctx_orig,
                    freq_base, freq_scale, ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                // with phi2, we scale the Q to avoid precision issues
                // ref: https://github.com/ml-explore/mlx-examples/blob/08e862336ade809bc37d1035f94b359e7d1a5152/phi2/phi2.py#L64-L66
                Qcur = lm_ggml_scale(ctx0, Qcur, 1.0f/sqrtf(float(n_embd_head)));
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, Kcur, inp_pos, nullptr, n_rot, rope_type, n_ctx_orig,
                    freq_base, freq_scale, ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, model.layers[il].bo,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f, cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur              = lm_ggml_get_rows(ctx0,              cur, inp_out_ids);
                inpL             = lm_ggml_get_rows(ctx0,             inpL, inp_out_ids);
                attn_norm_output = lm_ggml_get_rows(ctx0, attn_norm_output, inp_out_ids);
            }

            // FF
            {
                ffn_output = llm_build_ffn(ctx0, lctx, attn_norm_output,
                        model.layers[il].ffn_up,   model.layers[il].ffn_up_b,   NULL,
                        NULL,                      NULL,                        NULL,
                        model.layers[il].ffn_down, model.layers[il].ffn_down_b, NULL,
                        NULL,
                        LLM_FFN_GELU, LLM_FFN_SEQ, cb, il);
                cb(ffn_output, "ffn_out", il);
            }

            cur = lm_ggml_add(ctx0, cur, ffn_output);
            cur = lm_ggml_add(ctx0, cur, inpL);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = llm_build_norm(ctx0, inpL, hparams,
                model.output_norm,
                model.output_norm_b,
                LLM_NORM, cb, -1);
        cb(cur, "result_norm", -1);

        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output_no_bias", -1);

        cur = lm_ggml_add(ctx0, cur, model.output_b);
        cb(cur, "result_output", -1);
        lm_ggml_build_forward_expand(gf, cur);
        return gf;
    }

    struct lm_ggml_cgraph * build_phi3() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        const int64_t n_embd_gqa = hparams.n_embd_v_gqa();
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask_swa = build_inp_KQ_mask_swa();

        for (int il = 0; il < n_layer; ++il) {
            auto residual = inpL;

            // self-attention
            {
                // rope freq factors for 128k context
                struct lm_ggml_tensor * rope_factors = build_rope_factors(il);

                struct lm_ggml_tensor* attn_norm_output = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm,
                    NULL,
                    LLM_NORM_RMS, cb, il);
                cb(attn_norm_output, "attn_norm", il);

                struct lm_ggml_tensor * Qcur = nullptr;
                struct lm_ggml_tensor * Kcur = nullptr;
                struct lm_ggml_tensor * Vcur = nullptr;

                if (model.layers[il].wqkv) {
                    cur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wqkv, attn_norm_output);
                    cb(cur, "wqkv", il);

                    Qcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd,     n_tokens, cur->nb[1], 0 * sizeof(float) * (n_embd)));
                    Kcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1 * sizeof(float) * (n_embd)));
                    Vcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1 * sizeof(float) * (n_embd + n_embd_gqa)));
                }
                else {
                    Qcur = lm_ggml_add(ctx0, llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, attn_norm_output), model.layers[il].bq);
                    Kcur = lm_ggml_add(ctx0, llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, attn_norm_output), model.layers[il].bk);
                    Vcur = lm_ggml_add(ctx0, llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, attn_norm_output), model.layers[il].bv);
                }

                cb(Qcur, "Qcur", il);
                cb(Kcur, "Kcur", il);
                cb(Vcur, "Vcur", il);

                Qcur = lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head,    n_tokens);
                Kcur = lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens);

                Qcur = lm_ggml_rope_ext(
                    ctx0, Qcur, inp_pos, rope_factors, n_rot, rope_type, n_ctx_orig,
                    freq_base, freq_scale, ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                Qcur = lm_ggml_scale(ctx0, Qcur, 1.0f / sqrtf(float(n_embd_head)));
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, Kcur, inp_pos, rope_factors, n_rot, rope_type, n_ctx_orig,
                    freq_base, freq_scale, ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, model.layers[il].bo,
                        Kcur, Vcur, Qcur, KQ_mask_swa, n_tokens, kv_head, n_kv, 1.0f, cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor* inp_out_ids = build_inp_out_ids();
                cur = lm_ggml_get_rows(ctx0, cur, inp_out_ids);
                residual = lm_ggml_get_rows(ctx0, residual, inp_out_ids);
            }

            cur = lm_ggml_add(ctx0, cur, residual);
            residual = cur;

            cur = llm_build_norm(ctx0, cur, hparams,
                model.layers[il].ffn_norm, NULL,
                LLM_NORM_RMS, cb, il);
            cb(cur, "ffn_norm", il);

            // FF
            // special-case: the up and gate tensors are merged into a single tensor
            // TOOD: support into llm_build_ffn
            {
                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   NULL, NULL,
                        NULL,                      NULL, NULL,
                        model.layers[il].ffn_down, NULL, NULL,
                        NULL,
                        LLM_FFN_SWIGLU, LLM_FFN_SEQ, cb, il);
                cb(cur, "ffn_out", il);
            }

            cur = lm_ggml_add(ctx0, residual, cur);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = llm_build_norm(ctx0, inpL, hparams,
            model.output_norm,
            NULL,
            LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }


    struct lm_ggml_cgraph * build_plamo() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph(ctx0);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);
        LM_GGML_ASSERT(n_embd_head == hparams.n_rot);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {

            // norm
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_norm", il);

            struct lm_ggml_tensor * attention_norm = cur;

            // self-attention
            {
                // compute Q and K and RoPE them
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur);
                cb(Qcur, "Qcur", il);

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur);
                cb(Kcur, "Kcur", il);

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur);
                cb(Vcur, "Vcur", il);

                Qcur = lm_ggml_rope_ext(
                        ctx0, lm_ggml_reshape_3d(ctx0, Qcur, n_rot, n_head,    n_tokens), inp_pos, nullptr,
                        n_embd_head, rope_type, n_ctx_orig, freq_base, freq_scale,
                        ext_factor, attn_factor, beta_fast, beta_slow);
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                        ctx0, lm_ggml_reshape_3d(ctx0, Kcur, n_rot, n_head_kv, n_tokens), inp_pos, nullptr,
                        n_embd_head, rope_type, n_ctx_orig, freq_base, freq_scale,
                        ext_factor, attn_factor, beta_fast, beta_slow);
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, NULL,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }
            struct lm_ggml_tensor * sa_out = cur;

            cur = attention_norm;

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur    = lm_ggml_get_rows(ctx0,    cur, inp_out_ids);
                sa_out = lm_ggml_get_rows(ctx0, sa_out, inp_out_ids);
                inpL   = lm_ggml_get_rows(ctx0,   inpL, inp_out_ids);
            }

            // feed-forward network
            {
                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   NULL, NULL,
                        model.layers[il].ffn_gate, NULL, NULL,
                        model.layers[il].ffn_down, NULL, NULL,
                        NULL,
                        LLM_FFN_SILU, LLM_FFN_PAR, cb, il);
                cb(cur, "ffn_out", il);
            }

            cur = lm_ggml_add(ctx0, cur, sa_out);
            cur = lm_ggml_add(ctx0, cur, inpL);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, NULL,
                LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_gpt2() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        const int64_t n_embd_gqa  = hparams.n_embd_v_gqa();
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * pos;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        pos = lm_ggml_get_rows(ctx0, model.pos_embd, inp_pos);
        cb(pos, "pos_embd", -1);

        inpL = lm_ggml_add(ctx0, inpL, pos);
        cb(inpL, "inpL", -1);

        for (int il = 0; il < n_layer; ++il) {
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm,
                    model.layers[il].attn_norm_b,
                    LLM_NORM, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                cur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wqkv, cur);
                cb(cur, "wqkv", il);

                cur = lm_ggml_add(ctx0, cur, model.layers[il].bqkv);
                cb(cur, "bqkv", il);

                struct lm_ggml_tensor * Qcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd,     n_tokens, cur->nb[1], 0*sizeof(float)*(n_embd)));
                struct lm_ggml_tensor * Kcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*sizeof(float)*(n_embd)));
                struct lm_ggml_tensor * Vcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*sizeof(float)*(n_embd + n_embd_gqa)));

                cb(Qcur, "Qcur", il);
                cb(Kcur, "Kcur", il);
                cb(Vcur, "Vcur", il);

                Qcur = lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, model.layers[il].bo,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur  = lm_ggml_get_rows(ctx0,  cur, inp_out_ids);
                inpL = lm_ggml_get_rows(ctx0, inpL, inp_out_ids);
            }

            // add the input
            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpL);
            cb(ffn_inp, "ffn_inp", il);

            // FF
            {
                cur = llm_build_norm(ctx0, ffn_inp, hparams,
                        model.layers[il].ffn_norm,
                        model.layers[il].ffn_norm_b,
                        LLM_NORM, cb, il);
                cb(cur, "ffn_norm", il);

                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   model.layers[il].ffn_up_b,   NULL,
                        NULL,                      NULL,                        NULL,
                        model.layers[il].ffn_down, model.layers[il].ffn_down_b, NULL,
                        NULL,
                        LLM_FFN_GELU, LLM_FFN_SEQ, cb, il);
                cb(cur, "ffn_out", il);
            }

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = llm_build_norm(ctx0, inpL, hparams,
                model.output_norm,
                model.output_norm_b,
                LLM_NORM, cb, -1);
        cb(cur, "result_norm", -1);

        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_codeshell() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        const int64_t n_embd_gqa  = hparams.n_embd_v_gqa();
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);
        LM_GGML_ASSERT(n_embd_head == hparams.n_rot);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm,
                    model.layers[il].attn_norm_b,
                    LLM_NORM, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                cur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wqkv, cur);
                cb(cur, "wqkv", il);

                cur = lm_ggml_add(ctx0, cur, model.layers[il].bqkv);
                cb(cur, "bqkv", il);

                struct lm_ggml_tensor * tmpq = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd,     n_tokens, cur->nb[1], 0*sizeof(float)*(n_embd)));
                struct lm_ggml_tensor * tmpk = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*sizeof(float)*(n_embd)));
                struct lm_ggml_tensor * Vcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*sizeof(float)*(n_embd + n_embd_gqa)));

                cb(tmpq, "tmpq", il);
                cb(tmpk, "tmpk", il);
                cb(Vcur, "Vcur", il);

                struct lm_ggml_tensor * Qcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, tmpq, n_embd_head, n_head,    n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                struct lm_ggml_tensor * Kcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, tmpk, n_embd_head, n_head_kv, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, model.layers[il].bo,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur  = lm_ggml_get_rows(ctx0,  cur, inp_out_ids);
                inpL = lm_ggml_get_rows(ctx0, inpL, inp_out_ids);
            }

            // add the input
            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpL);
            cb(ffn_inp, "ffn_inp", il);

            // FF
            {
                cur = llm_build_norm(ctx0, ffn_inp, hparams,
                        model.layers[il].ffn_norm,
                        model.layers[il].ffn_norm_b,
                        LLM_NORM, cb, il);
                cb(cur, "ffn_norm", il);

                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   model.layers[il].ffn_up_b,   NULL,
                        NULL,                      NULL,                        NULL,
                        model.layers[il].ffn_down, model.layers[il].ffn_down_b, NULL,
                        NULL,
                        LLM_FFN_GELU, LLM_FFN_SEQ, cb, il);
                cb(cur, "ffn_out", il);
            }

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = llm_build_norm(ctx0, inpL, hparams,
                model.output_norm,
                model.output_norm_b,
                LLM_NORM, cb, -1);
        cb(cur, "result_norm", -1);

        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_orion() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);
        LM_GGML_ASSERT(n_embd_head == hparams.n_rot);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            // norm
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, model.layers[il].attn_norm_b,
                    LLM_NORM, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                // compute Q and K and RoPE them
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur);
                cb(Qcur, "Qcur", il);
                // if (model.layers[il].bq) {
                //     Qcur = lm_ggml_add(ctx0, Qcur, model.layers[il].bq);
                //     cb(Qcur, "Qcur", il);
                // }

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur);
                cb(Kcur, "Kcur", il);
                // if (model.layers[il].bk) {
                //     Kcur = lm_ggml_add(ctx0, Kcur, model.layers[il].bk);
                //     cb(Kcur, "Kcur", il);
                // }

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur);
                cb(Vcur, "Vcur", il);
                // if (model.layers[il].bv) {
                //     Vcur = lm_ggml_add(ctx0, Vcur, model.layers[il].bv);
                //     cb(Vcur, "Vcur", il);
                // }

                Qcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head,    n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, NULL,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);

            // feed-forward network
            cur = llm_build_norm(ctx0, ffn_inp, hparams,
                    model.layers[il].ffn_norm, model.layers[il].ffn_norm_b,
                    LLM_NORM, cb, il);
            cb(cur, "ffn_norm", il);

            cur = llm_build_ffn(ctx0, lctx, cur,
                    model.layers[il].ffn_up,   NULL, NULL,
                    model.layers[il].ffn_gate, NULL, NULL,
                    model.layers[il].ffn_down, NULL, NULL,
                    NULL,
                    LLM_FFN_SILU, LLM_FFN_PAR, cb, il);
            cb(cur, "ffn_out", il);

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, model.output_norm_b,
                LLM_NORM, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_internlm2() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);
        LM_GGML_ASSERT(n_embd_head == hparams.n_rot);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            // norm
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                // compute Q and K and RoPE them
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur);
                cb(Qcur, "Qcur", il);
                if (model.layers[il].bq) {
                    Qcur = lm_ggml_add(ctx0, Qcur, model.layers[il].bq);
                    cb(Qcur, "Qcur", il);
                }

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur);
                cb(Kcur, "Kcur", il);
                if (model.layers[il].bk) {
                    Kcur = lm_ggml_add(ctx0, Kcur, model.layers[il].bk);
                    cb(Kcur, "Kcur", il);
                }

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur);
                cb(Vcur, "Vcur", il);
                if (model.layers[il].bv) {
                    Vcur = lm_ggml_add(ctx0, Vcur, model.layers[il].bv);
                    cb(Vcur, "Vcur", il);
                }

                Qcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head,    n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, model.layers[il].bo,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);

            // feed-forward network
            cur = llm_build_norm(ctx0, ffn_inp, hparams,
                    model.layers[il].ffn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "ffn_norm", il);

            cur = llm_build_ffn(ctx0, lctx, cur,
                    model.layers[il].ffn_up,   NULL, NULL,
                    model.layers[il].ffn_gate, NULL, NULL,
                    model.layers[il].ffn_down, NULL, NULL,
                    NULL,
                    LLM_FFN_SILU, LLM_FFN_PAR, cb, il);
            cb(cur, "ffn_out", il);

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, NULL,
                LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    // ref: https://arxiv.org/abs/2203.03466
    //      https://github.com/ggerganov/llama.cpp/issues/5276#issuecomment-1925774738
    // based on the original build_llama() function
    struct lm_ggml_cgraph * build_minicpm() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);
        LM_GGML_ASSERT(n_embd_head == hparams.n_rot);

        const int64_t n_embd = hparams.n_embd;
        //TODO: if the model varies, these parameters need to be read from the model
        const int64_t n_embd_base = 256;
        const float scale_embd  = 12.0f;
        const float scale_depth = 1.4f;

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // scale the input embeddings
        inpL = lm_ggml_scale(ctx0, inpL, scale_embd);
        cb(inpL, "inp_scaled", -1);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            // norm
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                // compute Q and K and RoPE them
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur);
                cb(Qcur, "Qcur", il);
                if (model.layers[il].bq) {
                    Qcur = lm_ggml_add(ctx0, Qcur, model.layers[il].bq);
                    cb(Qcur, "Qcur", il);
                }

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur);
                cb(Kcur, "Kcur", il);
                if (model.layers[il].bk) {
                    Kcur = lm_ggml_add(ctx0, Kcur, model.layers[il].bk);
                    cb(Kcur, "Kcur", il);
                }

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur);
                cb(Vcur, "Vcur", il);
                if (model.layers[il].bv) {
                    Vcur = lm_ggml_add(ctx0, Vcur, model.layers[il].bv);
                    cb(Vcur, "Vcur", il);
                }

                Qcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head,    n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, model.layers[il].bo,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            // scale_res - scale the hidden states for residual connection
            const float scale_res = scale_depth/sqrtf(float(n_layer));
            cur = lm_ggml_scale(ctx0, cur, scale_res);
            cb(cur, "hidden_scaled", -1);

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);

            // feed-forward network
            {
                cur = llm_build_norm(ctx0, ffn_inp, hparams,
                        model.layers[il].ffn_norm, NULL,
                        LLM_NORM_RMS, cb, il);
                cb(cur, "ffn_norm", il);

                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   NULL, NULL,
                        model.layers[il].ffn_gate, NULL, NULL,
                        model.layers[il].ffn_down, NULL, NULL,
                        NULL,
                        LLM_FFN_SILU, LLM_FFN_PAR, cb, il);
                cb(cur, "ffn_out", il);
            }

            // scale the hidden states for residual connection
            cur = lm_ggml_scale(ctx0, cur, scale_res);
            cb(cur, "hidden_scaled_ffn", -1);

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, NULL,
                LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head scaling
        const float scale_lmhead = float(n_embd_base)/float(n_embd);
        cur = lm_ggml_scale(ctx0, cur, scale_lmhead);
        cb(cur, "lmhead_scaling", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_minicpm3() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        //TODO: if the model varies, these parameters need to be read from the model
        const int64_t n_embd_base = 256;
        const float scale_embd  = 12.0f;
        const float scale_depth = 1.4f;
        const float kq_scale = 1.0f / sqrtf(float(hparams.n_embd_head_k));

        const uint32_t n_embd_head_qk_rope = hparams.n_rot;
        const uint32_t n_embd_head_qk_nope = hparams.n_embd_head_k - hparams.n_rot;
        const uint32_t kv_lora_rank = hparams.n_lora_kv;

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // scale the input embeddings
        inpL = lm_ggml_scale(ctx0, inpL, scale_embd);
        cb(inpL, "inp_scaled", -1);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            struct lm_ggml_tensor * rope_factors = build_rope_factors(il);
            // norm
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_norm", il);

            // self_attention
            {
                struct lm_ggml_tensor * q = NULL;
                // {n_embd, q_lora_rank} * {n_embd, n_tokens} -> {q_lora_rank, n_tokens}
                q = lm_ggml_mul_mat(ctx0, model.layers[il].wq_a, cur);
                cb(q, "q", il);

                q = llm_build_norm(ctx0, q, hparams,
                        model.layers[il].attn_q_a_norm, NULL,
                        LLM_NORM_RMS, cb, il);
                cb(q, "q", il);

                // {q_lora_rank, n_head * hparams.n_embd_head_k} * {q_lora_rank, n_tokens} -> {n_head * hparams.n_embd_head_k, n_tokens}
                q = lm_ggml_mul_mat(ctx0, model.layers[il].wq_b, q);
                cb(q, "q", il);

                // split into {n_head * n_embd_head_qk_nope, n_tokens}
                struct lm_ggml_tensor * q_nope = lm_ggml_view_3d(ctx0, q, n_embd_head_qk_nope, n_head, n_tokens,
                        lm_ggml_row_size(q->type, hparams.n_embd_head_k),
                        lm_ggml_row_size(q->type, hparams.n_embd_head_k * n_head),
                        0);
                cb(q_nope, "q_nope", il);

                // and {n_head * n_embd_head_qk_rope, n_tokens}
                struct lm_ggml_tensor * q_pe = lm_ggml_view_3d(ctx0, q, n_embd_head_qk_rope, n_head, n_tokens,
                        lm_ggml_row_size(q->type, hparams.n_embd_head_k),
                        lm_ggml_row_size(q->type, hparams.n_embd_head_k * n_head),
                        lm_ggml_row_size(q->type, n_embd_head_qk_nope));
                cb(q_pe, "q_pe", il);

                // {n_embd, kv_lora_rank + n_embd_head_qk_rope} * {n_embd, n_tokens} -> {kv_lora_rank + n_embd_head_qk_rope, n_tokens}
                struct lm_ggml_tensor * kv_pe_compresseed = lm_ggml_mul_mat(ctx0, model.layers[il].wkv_a_mqa, cur);
                cb(kv_pe_compresseed, "kv_pe_compresseed", il);

                // split into {kv_lora_rank, n_tokens}
                struct lm_ggml_tensor * kv_compressed = lm_ggml_view_2d(ctx0, kv_pe_compresseed, kv_lora_rank, n_tokens,
                        kv_pe_compresseed->nb[1],
                        0);
                cb(kv_compressed, "kv_compressed", il);

                // and {n_embd_head_qk_rope, n_tokens}
                struct lm_ggml_tensor * k_pe = lm_ggml_view_3d(ctx0, kv_pe_compresseed, n_embd_head_qk_rope, 1, n_tokens,
                        kv_pe_compresseed->nb[1],
                        kv_pe_compresseed->nb[1],
                        lm_ggml_row_size(kv_pe_compresseed->type, kv_lora_rank));
                cb(k_pe, "k_pe", il);

                kv_compressed = lm_ggml_cont(ctx0, kv_compressed); // TODO: the CUDA backend does not support non-contiguous norm
                kv_compressed = llm_build_norm(ctx0, kv_compressed, hparams,
                        model.layers[il].attn_kv_a_norm, NULL,
                        LLM_NORM_RMS, cb, il);
                cb(kv_compressed, "kv_compressed", il);

                // {kv_lora_rank, n_head * (n_embd_head_qk_nope + n_embd_head_v)} * {kv_lora_rank, n_tokens} -> {n_head * (n_embd_head_qk_nope + n_embd_head_v), n_tokens}
                struct lm_ggml_tensor * kv = lm_ggml_mul_mat(ctx0, model.layers[il].wkv_b, kv_compressed);
                cb(kv, "kv", il);

                // split into {n_head * n_embd_head_qk_nope, n_tokens}
                struct lm_ggml_tensor * k_nope = lm_ggml_view_3d(ctx0, kv, n_embd_head_qk_nope, n_head, n_tokens,
                        lm_ggml_row_size(kv->type, n_embd_head_qk_nope + hparams.n_embd_head_v),
                        lm_ggml_row_size(kv->type, n_head * (n_embd_head_qk_nope + hparams.n_embd_head_v)),
                        0);
                cb(k_nope, "k_nope", il);

                // and {n_head * n_embd_head_v, n_tokens}
                struct lm_ggml_tensor * v_states = lm_ggml_view_3d(ctx0, kv, hparams.n_embd_head_v, n_head, n_tokens,
                        lm_ggml_row_size(kv->type, (n_embd_head_qk_nope + hparams.n_embd_head_v)),
                        lm_ggml_row_size(kv->type, (n_embd_head_qk_nope + hparams.n_embd_head_v)*n_head),
                        lm_ggml_row_size(kv->type, (n_embd_head_qk_nope)));
                cb(v_states, "v_states", il);

                v_states = lm_ggml_cont(ctx0, v_states);
                cb(v_states, "v_states", il);

                v_states = lm_ggml_view_2d(ctx0, v_states, hparams.n_embd_head_v * n_head, n_tokens,
                    lm_ggml_row_size(kv->type, hparams.n_embd_head_v * n_head),
                    0);
                cb(v_states, "v_states", il);

                q_pe = lm_ggml_cont(ctx0, q_pe); // TODO: the CUDA backend does not support non-contiguous RoPE
                q_pe = lm_ggml_rope_ext(
                    ctx0, q_pe, inp_pos, rope_factors,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(q_pe, "q_pe", il);

                // shared RoPE key
                k_pe = lm_ggml_cont(ctx0, k_pe); // TODO: the CUDA backend does not support non-contiguous RoPE
                k_pe = lm_ggml_rope_ext(
                    ctx0, k_pe, inp_pos, rope_factors,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(k_pe, "k_pe", il);

                struct lm_ggml_tensor * q_states = lm_ggml_concat(ctx0, q_nope, q_pe, 0);
                cb(q_states, "q_states", il);

                struct lm_ggml_tensor * k_states = lm_ggml_concat(ctx0, k_nope, lm_ggml_repeat(ctx0, k_pe, q_pe), 0);
                cb(k_states, "k_states", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, NULL,
                        k_states, v_states, q_states, KQ_mask, n_tokens, kv_head, n_kv, kq_scale, cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            // scale_res - scale the hidden states for residual connection
            const float scale_res = scale_depth/sqrtf(float(n_layer));
            cur = lm_ggml_scale(ctx0, cur, scale_res);
            cb(cur, "hidden_scaled", il);

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);

            // feed-forward network
            {
                cur = llm_build_norm(ctx0, ffn_inp, hparams,
                        model.layers[il].ffn_norm, NULL,
                        LLM_NORM_RMS, cb, il);
                cb(cur, "ffn_norm", il);

                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   NULL, NULL,
                        model.layers[il].ffn_gate, NULL, NULL,
                        model.layers[il].ffn_down, NULL, NULL,
                        NULL,
                        LLM_FFN_SILU, LLM_FFN_PAR, cb, il);
                cb(cur, "ffn_out", il);
            }

            // scale the hidden states for residual connection
            cur = lm_ggml_scale(ctx0, cur, scale_res);
            cb(cur, "hidden_scaled_ffn", il);

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, NULL,
                LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head scaling
        const float scale_lmhead = float(n_embd_base)/float(n_embd);
        cur = lm_ggml_scale(ctx0, cur, scale_lmhead);
        cb(cur, "lmhead_scaling", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_gemma() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head_k = hparams.n_embd_head_k;

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        inpL = lm_ggml_scale(ctx0, inpL, sqrtf(n_embd));
        cb(inpL, "inp_scaled", -1);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            // norm
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                // compute Q and K and RoPE them
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur);
                cb(Qcur, "Qcur", il);

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur);
                cb(Kcur, "Kcur", il);

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur);
                cb(Vcur, "Vcur", il);

                Qcur = lm_ggml_rope_ext(
                        ctx0, lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head_k, n_head,    n_tokens), inp_pos, nullptr,
                        n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                        ext_factor, attn_factor, beta_fast, beta_slow);
                cb(Qcur, "Qcur", il);

                Qcur = lm_ggml_scale(ctx0, Qcur, 1.0f / sqrtf(float(n_embd_head_k)));
                cb(Qcur, "Qcur_scaled", il);

                Kcur = lm_ggml_rope_ext(
                        ctx0, lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head_k, n_head_kv, n_tokens), inp_pos, nullptr,
                        n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                        ext_factor, attn_factor, beta_fast, beta_slow);
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, NULL,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f, cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur  = lm_ggml_get_rows(ctx0,  cur, inp_out_ids);
                inpL = lm_ggml_get_rows(ctx0, inpL, inp_out_ids);
            }

            struct lm_ggml_tensor * sa_out = lm_ggml_add(ctx0, cur, inpL);
            cb(sa_out, "sa_out", il);

            cur = llm_build_norm(ctx0, sa_out, hparams,
                    model.layers[il].ffn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "ffn_norm", il);

            // feed-forward network
            {
                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   NULL, NULL,
                        model.layers[il].ffn_gate, NULL, NULL,
                        model.layers[il].ffn_down, NULL, NULL,
                        NULL,
                        LLM_FFN_GELU, LLM_FFN_PAR, cb, il);
                cb(cur, "ffn_out", il);
            }

            cur = lm_ggml_add(ctx0, cur, sa_out);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, NULL,
                LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_gemma2() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head_k = hparams.n_embd_head_k;

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        inpL = lm_ggml_scale(ctx0, inpL, sqrtf(n_embd));
        cb(inpL, "inp_scaled", -1);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        // gemma 2 requires different mask for layers using sliding window (SWA)
        struct lm_ggml_tensor * KQ_mask     = build_inp_KQ_mask(true);
        struct lm_ggml_tensor * KQ_mask_swa = build_inp_KQ_mask_swa(true);

        for (int il = 0; il < n_layer; ++il) {
            // (il % 2) layers use SWA
            struct lm_ggml_tensor * KQ_mask_l = (il % 2 == 0) ? KQ_mask_swa : KQ_mask;

            // norm
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                // compute Q and K and RoPE them
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur);
                cb(Qcur, "Qcur", il);

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur);
                cb(Kcur, "Kcur", il);

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur);
                cb(Vcur, "Vcur", il);

                Qcur = lm_ggml_rope_ext(
                        ctx0, lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head_k, n_head,    n_tokens), inp_pos, nullptr,
                        n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                        ext_factor, attn_factor, beta_fast, beta_slow);
                cb(Qcur, "Qcur", il);

                // ref: https://github.com/google/gemma_pytorch/commit/03e657582d17cb5a8617ebf333c1c16f3694670e
                switch (model.type) {
                    case e_model::MODEL_2B:
                    case e_model::MODEL_9B:  Qcur = lm_ggml_scale(ctx0, Qcur, 1.0f / sqrtf(float(n_embd_head_k)));   break;
                    case e_model::MODEL_27B: Qcur = lm_ggml_scale(ctx0, Qcur, 1.0f / sqrtf(float(n_embd / n_head))); break;
                    default: LM_GGML_ABORT("fatal error");
                };
                cb(Qcur, "Qcur_scaled", il);

                Kcur = lm_ggml_rope_ext(
                        ctx0, lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head_k, n_head_kv, n_tokens), inp_pos, nullptr,
                        n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                        ext_factor, attn_factor, beta_fast, beta_slow);
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, NULL,
                        Kcur, Vcur, Qcur, KQ_mask_l, n_tokens, kv_head, n_kv, 1.0f, cb, il);
            }

            cur = llm_build_norm(ctx0, cur, hparams,
                    model.layers[il].attn_post_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_post_norm", il);

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur  = lm_ggml_get_rows(ctx0,  cur, inp_out_ids);
                inpL = lm_ggml_get_rows(ctx0, inpL, inp_out_ids);
            }

            struct lm_ggml_tensor * sa_out = lm_ggml_add(ctx0, cur, inpL);
            cb(sa_out, "sa_out", il);

            cur = llm_build_norm(ctx0, sa_out, hparams,
                    model.layers[il].ffn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "ffn_norm", il);

            // feed-forward network
            {
                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   NULL, NULL,
                        model.layers[il].ffn_gate, NULL, NULL,
                        model.layers[il].ffn_down, NULL, NULL,
                        NULL,
                        LLM_FFN_GELU, LLM_FFN_PAR, cb, il);
                cb(cur, "ffn_out", il);
            }

            cur = llm_build_norm(ctx0, cur, hparams,
                model.layers[il].ffn_post_norm, NULL,
                LLM_NORM_RMS, cb, -1);
            cb(cur, "ffn_post_norm", -1);

            cur = lm_ggml_add(ctx0, cur, sa_out);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, NULL,
                LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);

        // final logit soft-capping
        cur = lm_ggml_scale(ctx0, cur, 1.0f / hparams.f_final_logit_softcapping);
        cur = lm_ggml_tanh(ctx0, cur);
        cur = lm_ggml_scale(ctx0, cur, hparams.f_final_logit_softcapping);

        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }


    struct lm_ggml_cgraph * build_starcoder2() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);
        LM_GGML_ASSERT(n_embd_head == hparams.n_rot);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            // norm
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, model.layers[il].attn_norm_b,
                    LLM_NORM, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                // compute Q and K and RoPE them
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur);
                cb(Qcur, "Qcur", il);
                if (model.layers[il].bq) {
                    Qcur = lm_ggml_add(ctx0, Qcur, model.layers[il].bq);
                    cb(Qcur, "Qcur", il);
                }

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur);
                cb(Kcur, "Kcur", il);
                if (model.layers[il].bk) {
                    Kcur = lm_ggml_add(ctx0, Kcur, model.layers[il].bk);
                    cb(Kcur, "Kcur", il);
                }

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur);
                cb(Vcur, "Vcur", il);
                if (model.layers[il].bv) {
                    Vcur = lm_ggml_add(ctx0, Vcur, model.layers[il].bv);
                    cb(Vcur, "Vcur", il);
                }

                Qcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, model.layers[il].bo,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);

            // feed-forward network

            cur = llm_build_norm(ctx0, ffn_inp, hparams,
                    model.layers[il].ffn_norm, model.layers[il].ffn_norm_b,
                    LLM_NORM, cb, il);
            cb(cur, "ffn_norm", il);

            cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   model.layers[il].ffn_up_b,   NULL,
                        NULL,                      NULL,                        NULL,
                        model.layers[il].ffn_down, model.layers[il].ffn_down_b, NULL,
                        NULL,
                        LLM_FFN_GELU, LLM_FFN_SEQ, cb, il);
            cb(cur, "ffn_out", il);

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, model.output_norm_b,
                LLM_NORM, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_mamba() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        // {n_embd, n_tokens}
        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        struct lm_ggml_tensor * state_copy = build_inp_s_copy();
        struct lm_ggml_tensor * state_mask = build_inp_s_mask();

        for (int il = 0; il < n_layer; ++il) {
            // norm
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_norm", il);

            cur = llm_build_mamba(ctx0, lctx, ubatch, gf, cur,
                    state_copy, state_mask,
                    kv_head, n_kv, cb, il);

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur  = lm_ggml_get_rows(ctx0,  cur, inp_out_ids);
                inpL = lm_ggml_get_rows(ctx0, inpL, inp_out_ids);
            }

            // residual
            cur = lm_ggml_add(ctx0, cur, inpL);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        // final rmsnorm
        cur = llm_build_norm(ctx0, inpL, hparams,
                model.output_norm, NULL,
                LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_command_r() {

        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);
        const float f_logit_scale = hparams.f_logit_scale;

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {

            // norm
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, NULL,
                    LLM_NORM, cb, il);
            cb(cur, "attn_norm", il);
            struct lm_ggml_tensor * ffn_inp = cur;

            // self-attention
            {
                // compute Q and K and RoPE them
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur);
                cb(Qcur, "Qcur", il);
                if (model.layers[il].bq) {
                    Qcur = lm_ggml_add(ctx0, Qcur, model.layers[il].bq);
                    cb(Qcur, "Qcur", il);
                }

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur);
                cb(Kcur, "Kcur", il);
                if (model.layers[il].bk) {
                    Kcur = lm_ggml_add(ctx0, Kcur, model.layers[il].bk);
                    cb(Kcur, "Kcur", il);
                }

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur);
                cb(Vcur, "Vcur", il);
                if (model.layers[il].bv) {
                    Vcur = lm_ggml_add(ctx0, Vcur, model.layers[il].bv);
                    cb(Vcur, "Vcur", il);
                }

                if (model.layers[il].attn_q_norm) {
                    Qcur = lm_ggml_view_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens,
                                lm_ggml_element_size(Qcur) * n_embd_head,
                                lm_ggml_element_size(Qcur) * n_embd_head * n_head,
                                0);
                    cb(Qcur, "Qcur", il);
                    Kcur = lm_ggml_view_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens,
                                lm_ggml_element_size(Kcur) * n_embd_head,
                                lm_ggml_element_size(Kcur) * n_embd_head * n_head_kv,
                                0);
                    cb(Kcur, "Kcur", il);

                    Qcur = llm_build_norm(ctx0, Qcur, hparams,
                                model.layers[il].attn_q_norm,
                                NULL,
                                LLM_NORM, cb, il);
                    cb(Qcur, "Qcur", il);

                    Kcur = llm_build_norm(ctx0, Kcur, hparams,
                            model.layers[il].attn_k_norm,
                            NULL,
                            LLM_NORM, cb, il);
                    cb(Kcur, "Kcur", il);
                }

                Qcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, model.layers[il].bo,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur     = lm_ggml_get_rows(ctx0,     cur, inp_out_ids);
                inpL    = lm_ggml_get_rows(ctx0,    inpL, inp_out_ids);
                ffn_inp = lm_ggml_get_rows(ctx0, ffn_inp, inp_out_ids);
            }

            struct lm_ggml_tensor * attn_out = cur;

            // feed-forward network
            {
                cur = llm_build_ffn(ctx0, lctx, ffn_inp,
                        model.layers[il].ffn_up,   NULL, NULL,
                        model.layers[il].ffn_gate, NULL, NULL,
                        model.layers[il].ffn_down, NULL, NULL,
                        NULL,
                        LLM_FFN_SILU, LLM_FFN_PAR, cb, il);
                cb(cur, "ffn_out", il);
            }

            // add together residual + FFN + self-attention
            cur = lm_ggml_add(ctx0, cur, inpL);
            cur = lm_ggml_add(ctx0, cur, attn_out);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, NULL,
                LLM_NORM, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);

        if (f_logit_scale) {
            cur = lm_ggml_scale(ctx0, cur, f_logit_scale);
        }

        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;

    }

    // ref: https://allenai.org/olmo
    // based on the original build_llama() function, changes:
    //   * non-parametric layer norm
    //   * clamp qkv
    //   * removed bias
    //   * removed MoE
    struct lm_ggml_cgraph * build_olmo() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        // mutable variable, needed during the last layer of the computation to skip unused tokens
        int32_t n_tokens = this->n_tokens;

        const int64_t n_embd_head = hparams.n_embd_head_v;
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);
        LM_GGML_ASSERT(n_embd_head == hparams.n_rot);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            // norm
            cur = llm_build_norm(ctx0, inpL, hparams,
                    NULL, NULL,
                    LLM_NORM, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                // compute Q and K and RoPE them
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur);
                cb(Qcur, "Qcur", il);
                if (hparams.f_clamp_kqv > 0.0f) {
                    Qcur = lm_ggml_clamp(ctx0, Qcur, -hparams.f_clamp_kqv, hparams.f_clamp_kqv);
                    cb(Qcur, "Qcur", il);
                }

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur);
                cb(Kcur, "Kcur", il);
                if (hparams.f_clamp_kqv > 0.0f) {
                    Kcur = lm_ggml_clamp(ctx0, Kcur, -hparams.f_clamp_kqv, hparams.f_clamp_kqv);
                    cb(Kcur, "Kcur", il);
                }

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur);
                cb(Vcur, "Vcur", il);
                if (hparams.f_clamp_kqv > 0.0f) {
                    Vcur = lm_ggml_clamp(ctx0, Vcur, -hparams.f_clamp_kqv, hparams.f_clamp_kqv);
                    cb(Vcur, "Vcur", il);
                }

                Qcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, nullptr,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                n_tokens = n_outputs;
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);

            // feed-forward network
            cur = llm_build_norm(ctx0, ffn_inp, hparams,
                    NULL, NULL,
                    LLM_NORM, cb, il);
            cb(cur, "ffn_norm", il);

            cur = llm_build_ffn(ctx0, lctx, cur,
                    model.layers[il].ffn_up,   NULL, NULL,
                    model.layers[il].ffn_gate, NULL, NULL,
                    model.layers[il].ffn_down, NULL, NULL,
                    NULL,
                    LLM_FFN_SILU, LLM_FFN_PAR, cb, il);
            cb(cur, "ffn_out", il);

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cb(cur, "ffn_out", il);

            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                NULL, NULL,
                LLM_NORM, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_olmo_1124() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        // mutable variable, needed during the last layer of the computation to skip unused tokens
        int32_t n_tokens = this->n_tokens;

        const int64_t n_embd_head = hparams.n_embd_head_v;
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);
        LM_GGML_ASSERT(n_embd_head == hparams.n_rot);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            cur = inpL;

            // self_attention
            {
                // compute Q and K and RoPE them
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur);
                cb(Qcur, "Qcur", il);

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur);
                cb(Kcur, "Kcur", il);

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur);
                cb(Vcur, "Vcur", il);

                Qcur = llm_build_norm(ctx0, Qcur, hparams, model.layers[il].attn_q_norm, NULL,
                        LLM_NORM_RMS, cb, il);
                cb(Qcur, "Qcur_normed", il);

                Kcur = llm_build_norm(ctx0, Kcur, hparams, model.layers[il].attn_k_norm, NULL,
                        LLM_NORM_RMS, cb, il);
                cb(Kcur, "Kcur_normed", il);

                Qcur = lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens);
                Kcur = lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens);

                Qcur = lm_ggml_rope_ext(
                    ctx0, Qcur, inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur_rope", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, Kcur, inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur_rope", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, NULL,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            cur = llm_build_norm(ctx0, cur, hparams,
                    model.layers[il].attn_post_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_post_norm", il);

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                n_tokens = n_outputs;
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);

            // feed-forward network
            cur = llm_build_ffn(ctx0, lctx, ffn_inp,
                    model.layers[il].ffn_up,   NULL, NULL,
                    model.layers[il].ffn_gate, NULL, NULL,
                    model.layers[il].ffn_down, NULL, NULL,
                    NULL,
                    LLM_FFN_SILU, LLM_FFN_PAR, cb, il);
            cb(cur, "ffn_out", il);

            cur = llm_build_norm(ctx0, cur, hparams,
                model.layers[il].ffn_post_norm, NULL,
                LLM_NORM_RMS, cb, -1);
            cb(cur, "ffn_post_norm", -1);

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cb(cur, "ffn_out", il);

            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, NULL,
                LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    // based on the build_qwen2moe() function, changes:
    //   * removed shared experts
    //   * removed bias
    //   * added q, k norm
    struct lm_ggml_cgraph * build_olmoe() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        // mutable variable, needed during the last layer of the computation to skip unused tokens
        int32_t n_tokens = this->n_tokens;

        const int64_t n_embd_head = hparams.n_embd_head_v;
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);
        LM_GGML_ASSERT(n_embd_head == hparams.n_rot);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            // norm
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_norm", il);

            // self_attention
            {
                // compute Q and K and RoPE them
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur);
                cb(Qcur, "Qcur", il);

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur);
                cb(Kcur, "Kcur", il);

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur);
                cb(Vcur, "Vcur", il);

                Qcur = llm_build_norm(ctx0, Qcur, hparams, model.layers[il].attn_q_norm, NULL,
                        LLM_NORM_RMS, cb, il);
                cb(Qcur, "Qcur_normed", il);

                Kcur = llm_build_norm(ctx0, Kcur, hparams, model.layers[il].attn_k_norm, NULL,
                        LLM_NORM_RMS, cb, il);
                cb(Kcur, "Kcur_normed", il);

                Qcur = lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens);
                Kcur = lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens);

                Qcur = lm_ggml_rope_ext(
                    ctx0, Qcur, inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur_rope", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, Kcur, inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur_rope", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, NULL,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                n_tokens = n_outputs;
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);

            // MoE branch
            cur = llm_build_norm(ctx0, ffn_inp, hparams,
                    model.layers[il].ffn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "ffn_norm", il);

            cur = llm_build_moe_ffn(ctx0, lctx, cur,
                    model.layers[il].ffn_gate_inp,
                    model.layers[il].ffn_up_exps,
                    model.layers[il].ffn_gate_exps,
                    model.layers[il].ffn_down_exps,
                    n_expert, n_expert_used,
                    LLM_FFN_SILU, false,
                    false, 0.0,
                    cb, il);
            cb(cur, "ffn_moe_out", il);

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, NULL,
                LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_openelm() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;
        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            const int64_t n_head    = hparams.n_head(il);
            const int64_t n_head_kv = hparams.n_head_kv(il);
            const int64_t n_head_qkv = 2*n_head_kv + n_head;

            cur = inpL;
            struct lm_ggml_tensor * residual = cur;

            // norm
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                cur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wqkv, cur);
                cb(cur, "wqkv", il);

                cur = lm_ggml_reshape_3d(ctx0, cur, n_embd_head_k, n_head_qkv, n_tokens);

                struct lm_ggml_tensor * Qcur = lm_ggml_cont(ctx0, lm_ggml_view_3d(ctx0, cur, n_embd_head, n_head, n_tokens, cur->nb[1], cur->nb[2], 0));
                cb(Qcur, "Qcur", il);

                struct lm_ggml_tensor * Kcur = lm_ggml_cont(ctx0, lm_ggml_view_3d(ctx0, cur, n_embd_head, n_head_kv, n_tokens, cur->nb[1], cur->nb[2], cur->nb[1]*n_head));
                cb(Kcur, "Kcur", il);

                struct lm_ggml_tensor * Vcur = lm_ggml_cont(ctx0, lm_ggml_view_3d(ctx0, cur, n_embd_head, n_head_kv, n_tokens, cur->nb[1], cur->nb[2], cur->nb[1]*(n_head+n_head_kv)));
                cb(Vcur, "Vcur", il);

                Qcur = llm_build_norm(ctx0, Qcur, hparams,
                        model.layers[il].attn_q_norm, NULL,
                        LLM_NORM_RMS, cb, il);
                cb(Qcur, "Qcur", il);

                Kcur = llm_build_norm(ctx0, Kcur, hparams,
                        model.layers[il].attn_k_norm, NULL,
                        LLM_NORM_RMS, cb, il);
                cb(Kcur, "Kcur", il);

                Qcur = lm_ggml_rope_ext(
                    ctx0, Qcur, inp_pos, NULL, n_rot, rope_type, n_ctx_orig,
                    freq_base, freq_scale, ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, Kcur, inp_pos, NULL, n_rot, rope_type, n_ctx_orig,
                    freq_base, freq_scale, ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);

                Vcur = lm_ggml_reshape_2d(ctx0, Vcur, n_embd_head * n_head_kv, n_tokens);
                cb(Qcur, "Vcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, NULL,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                residual = lm_ggml_get_rows(ctx0, residual, inp_out_ids);
                cur = lm_ggml_get_rows(ctx0, cur, inp_out_ids);
            }

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, residual, cur);
            cb(ffn_inp, "ffn_inp", il);

            // feed-forward network
            {
                cur = llm_build_norm(ctx0, ffn_inp, hparams,
                        model.layers[il].ffn_norm, NULL,
                        LLM_NORM_RMS, cb, il);
                cb(cur, "ffn_norm", il);

                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   NULL, NULL,
                        model.layers[il].ffn_gate, NULL, NULL,
                        model.layers[il].ffn_down, NULL, NULL,
                        NULL,
                        LLM_FFN_SILU, LLM_FFN_PAR, cb, il);
                cb(cur, "ffn_out", il);
            }

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            inpL = cur;
        }

        cur = inpL;

        // norm
        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, NULL,
                LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_gptneox() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        const int64_t n_embd_gqa  = hparams.n_embd_v_gqa();
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm,
                    model.layers[il].attn_norm_b,
                    LLM_NORM, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                cur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wqkv, cur);
                cb(cur, "wqkv", il);

                cur = lm_ggml_add(ctx0, cur, model.layers[il].bqkv);
                cb(cur, "bqkv", il);

                struct lm_ggml_tensor * Qcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd,     n_tokens, cur->nb[1], 0*sizeof(float)*(n_embd)));
                struct lm_ggml_tensor * Kcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*sizeof(float)*(n_embd)));
                struct lm_ggml_tensor * Vcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*sizeof(float)*(n_embd + n_embd_gqa)));

                cb(Qcur, "Qcur", il);
                cb(Kcur, "Kcur", il);
                cb(Vcur, "Vcur", il);

                Qcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, model.layers[il].bo,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur  = lm_ggml_get_rows(ctx0,  cur, inp_out_ids);
                inpL = lm_ggml_get_rows(ctx0, inpL, inp_out_ids);
            }

            // ffn
            if (hparams.use_par_res) {
                // attention and ffn are computed in parallel
                // x = x + attn(ln1(x)) + ffn(ln2(x))

                struct lm_ggml_tensor * attn_out = cur;

                cur = llm_build_norm(ctx0, inpL, hparams,
                        model.layers[il].ffn_norm,
                        model.layers[il].ffn_norm_b,
                        LLM_NORM, cb, il);
                cb(cur, "ffn_norm", il);

                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   model.layers[il].ffn_up_b,   NULL,
                        NULL,                      NULL,                        NULL,
                        model.layers[il].ffn_down, model.layers[il].ffn_down_b, NULL,
                        NULL,
                        LLM_FFN_GELU, LLM_FFN_SEQ, cb, il);
                cb(cur, "ffn_out", il);

                cur = lm_ggml_add(ctx0, cur, inpL);
                cb(cur, "ffn_out", il);

                cur = lm_ggml_add(ctx0, cur, attn_out);
                cur = lctx.cvec.apply_to(ctx0, cur, il);
                cb(cur, "l_out", il);

                // input for next layer
                inpL = cur;
            } else {
                // attention and ffn are computed sequentially
                // x = x + attn(ln1(x))
                // x = x + ffn(ln2(x))

                struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpL);
                cb(ffn_inp, "ffn_inp", il);

                cur = llm_build_norm(ctx0, ffn_inp, hparams,
                        model.layers[il].ffn_norm,
                        model.layers[il].ffn_norm_b,
                        LLM_NORM, cb, il);
                cb(cur, "ffn_norm", il);

                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   model.layers[il].ffn_up_b,   NULL,
                        NULL,                      NULL,                        NULL,
                        model.layers[il].ffn_down, model.layers[il].ffn_down_b, NULL,
                        NULL,
                        LLM_FFN_GELU, LLM_FFN_SEQ, cb, il);
                cb(cur, "ffn_out", il);

                cur = lm_ggml_add(ctx0, cur, ffn_inp);
                cur = lctx.cvec.apply_to(ctx0, cur, il);
                cb(cur, "l_out", il);

                // input for next layer
                inpL = cur;
            }
        }

        cur = llm_build_norm(ctx0, inpL, hparams,
                model.output_norm,
                model.output_norm_b,
                LLM_NORM, cb, -1);
        cb(cur, "result_norm", -1);

        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_arctic() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        // mutable variable, needed during the last layer of the computation to skip unused tokens
        int32_t n_tokens = this->n_tokens;

        const int64_t n_embd_head = hparams.n_embd_head_v;
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);
        LM_GGML_ASSERT(n_embd_head == hparams.n_rot);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            // norm
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                // compute Q and K and RoPE them
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur);
                cb(Qcur, "Qcur", il);

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur);
                cb(Kcur, "Kcur", il);

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur);
                cb(Vcur, "Vcur", il);

                Qcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, NULL,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                n_tokens = n_outputs;
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);

            // feed-forward network
            cur = llm_build_norm(ctx0, ffn_inp, hparams,
                    model.layers[il].ffn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "ffn_norm", il);

            cur = llm_build_ffn(ctx0, lctx, cur,
                    model.layers[il].ffn_up,   NULL, NULL,
                    model.layers[il].ffn_gate, NULL, NULL,
                    model.layers[il].ffn_down, NULL, NULL,
                    NULL,
                    LLM_FFN_SILU, LLM_FFN_PAR, cb, il);
            cb(cur, "ffn_out", il);

            struct lm_ggml_tensor * ffn_out = lm_ggml_add(ctx0, cur, ffn_inp);
            cb(ffn_out, "ffn_out", il);

            // MoE
            cur = llm_build_norm(ctx0, inpSA, hparams,
                    model.layers[il].ffn_norm_exps, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "ffn_norm_exps", il);

            cur = llm_build_moe_ffn(ctx0, lctx, cur,
                    model.layers[il].ffn_gate_inp,
                    model.layers[il].ffn_up_exps,
                    model.layers[il].ffn_gate_exps,
                    model.layers[il].ffn_down_exps,
                    n_expert, n_expert_used,
                    LLM_FFN_SILU, true,
                    false, 0.0,
                    cb, il);
            cb(cur, "ffn_moe_out", il);

            cur = lm_ggml_add(ctx0, cur, ffn_out);
            cb(cur, "ffn_out", il);

            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, NULL,
                LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_deepseek2() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        // mutable variable, needed during the last layer of the computation to skip unused tokens
        int32_t n_tokens = this->n_tokens;

        bool is_lite = (hparams.n_layer == 27);

        // We have to pre-scale kq_scale and attn_factor to make the YaRN RoPE work correctly.
        // See https://github.com/ggerganov/llama.cpp/discussions/7416 for detailed explanation.
        const float mscale = attn_factor * (1.0f + hparams.rope_yarn_log_mul * logf(1.0f / freq_scale));
        const float kq_scale = 1.0f*mscale*mscale/sqrtf(float(hparams.n_embd_head_k));
        const float attn_factor_scaled = 1.0f / (1.0f + 0.1f * logf(1.0f / freq_scale));

        const uint32_t n_embd_head_qk_rope = hparams.n_rot;
        const uint32_t n_embd_head_qk_nope = hparams.n_embd_head_k - hparams.n_rot;
        const uint32_t kv_lora_rank = hparams.n_lora_kv;

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        // {n_embd, n_tokens}
        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            // norm
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_norm", il);

            // self_attention
            {
                struct lm_ggml_tensor * q = NULL;
                if (!is_lite) {
                    // {n_embd, q_lora_rank} * {n_embd, n_tokens} -> {q_lora_rank, n_tokens}
                    q = lm_ggml_mul_mat(ctx0, model.layers[il].wq_a, cur);
                    cb(q, "q", il);

                    q = llm_build_norm(ctx0, q, hparams,
                            model.layers[il].attn_q_a_norm, NULL,
                            LLM_NORM_RMS, cb, il);
                    cb(q, "q", il);

                    // {q_lora_rank, n_head * hparams.n_embd_head_k} * {q_lora_rank, n_tokens} -> {n_head * hparams.n_embd_head_k, n_tokens}
                    q = lm_ggml_mul_mat(ctx0, model.layers[il].wq_b, q);
                    cb(q, "q", il);
                } else {
                    q = lm_ggml_mul_mat(ctx0, model.layers[il].wq, cur);
                    cb(q, "q", il);
                }

                // split into {n_head * n_embd_head_qk_nope, n_tokens}
                struct lm_ggml_tensor * q_nope = lm_ggml_view_3d(ctx0, q, n_embd_head_qk_nope, n_head, n_tokens,
                        lm_ggml_row_size(q->type, hparams.n_embd_head_k),
                        lm_ggml_row_size(q->type, hparams.n_embd_head_k * n_head),
                        0);
                cb(q_nope, "q_nope", il);

                // and {n_head * n_embd_head_qk_rope, n_tokens}
                struct lm_ggml_tensor * q_pe = lm_ggml_view_3d(ctx0, q, n_embd_head_qk_rope, n_head, n_tokens,
                        lm_ggml_row_size(q->type, hparams.n_embd_head_k),
                        lm_ggml_row_size(q->type, hparams.n_embd_head_k * n_head),
                        lm_ggml_row_size(q->type, n_embd_head_qk_nope));
                cb(q_pe, "q_pe", il);

                // {n_embd, kv_lora_rank + n_embd_head_qk_rope} * {n_embd, n_tokens} -> {kv_lora_rank + n_embd_head_qk_rope, n_tokens}
                struct lm_ggml_tensor * kv_pe_compresseed = lm_ggml_mul_mat(ctx0, model.layers[il].wkv_a_mqa, cur);
                cb(kv_pe_compresseed, "kv_pe_compresseed", il);

                // split into {kv_lora_rank, n_tokens}
                struct lm_ggml_tensor * kv_compressed = lm_ggml_view_2d(ctx0, kv_pe_compresseed, kv_lora_rank, n_tokens,
                        kv_pe_compresseed->nb[1],
                        0);
                cb(kv_compressed, "kv_compressed", il);

                // and {n_embd_head_qk_rope, n_tokens}
                struct lm_ggml_tensor * k_pe = lm_ggml_view_3d(ctx0, kv_pe_compresseed, n_embd_head_qk_rope, 1, n_tokens,
                        kv_pe_compresseed->nb[1],
                        kv_pe_compresseed->nb[1],
                        lm_ggml_row_size(kv_pe_compresseed->type, kv_lora_rank));
                cb(k_pe, "k_pe", il);

                kv_compressed = lm_ggml_cont(ctx0, kv_compressed); // TODO: the CUDA backend does not support non-contiguous norm
                kv_compressed = llm_build_norm(ctx0, kv_compressed, hparams,
                        model.layers[il].attn_kv_a_norm, NULL,
                        LLM_NORM_RMS, cb, il);
                cb(kv_compressed, "kv_compressed", il);

                // {kv_lora_rank, n_head * (n_embd_head_qk_nope + n_embd_head_v)} * {kv_lora_rank, n_tokens} -> {n_head * (n_embd_head_qk_nope + n_embd_head_v), n_tokens}
                struct lm_ggml_tensor * kv = lm_ggml_mul_mat(ctx0, model.layers[il].wkv_b, kv_compressed);
                cb(kv, "kv", il);

                // split into {n_head * n_embd_head_qk_nope, n_tokens}
                struct lm_ggml_tensor * k_nope = lm_ggml_view_3d(ctx0, kv, n_embd_head_qk_nope, n_head, n_tokens,
                        lm_ggml_row_size(kv->type, n_embd_head_qk_nope + hparams.n_embd_head_v),
                        lm_ggml_row_size(kv->type, n_head * (n_embd_head_qk_nope + hparams.n_embd_head_v)),
                        0);
                cb(k_nope, "k_nope", il);

                // and {n_head * n_embd_head_v, n_tokens}
                struct lm_ggml_tensor * v_states = lm_ggml_view_3d(ctx0, kv, hparams.n_embd_head_v, n_head, n_tokens,
                        lm_ggml_row_size(kv->type, (n_embd_head_qk_nope + hparams.n_embd_head_v)),
                        lm_ggml_row_size(kv->type, (n_embd_head_qk_nope + hparams.n_embd_head_v)*n_head),
                        lm_ggml_row_size(kv->type, (n_embd_head_qk_nope)));
                cb(v_states, "v_states", il);

                v_states = lm_ggml_cont(ctx0, v_states);
                cb(v_states, "v_states", il);

                v_states = lm_ggml_view_2d(ctx0, v_states, hparams.n_embd_head_v * n_head, n_tokens,
                    lm_ggml_row_size(kv->type, hparams.n_embd_head_v * n_head),
                    0);
                cb(v_states, "v_states", il);

                q_pe = lm_ggml_cont(ctx0, q_pe); // TODO: the CUDA backend does not support non-contiguous RoPE
                q_pe = lm_ggml_rope_ext(
                    ctx0, q_pe, inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor_scaled, beta_fast, beta_slow
                );
                cb(q_pe, "q_pe", il);

                // shared RoPE key
                k_pe = lm_ggml_cont(ctx0, k_pe); // TODO: the CUDA backend does not support non-contiguous RoPE
                k_pe = lm_ggml_rope_ext(
                    ctx0, k_pe, inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor_scaled, beta_fast, beta_slow
                );
                cb(k_pe, "k_pe", il);

                struct lm_ggml_tensor * q_states = lm_ggml_concat(ctx0, q_nope, q_pe, 0);
                cb(q_states, "q_states", il);

                struct lm_ggml_tensor * k_states = lm_ggml_concat(ctx0, k_nope, lm_ggml_repeat(ctx0, k_pe, q_pe), 0);
                cb(k_states, "k_states", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, NULL,
                        k_states, v_states, q_states, KQ_mask, n_tokens, kv_head, n_kv, kq_scale, cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                n_tokens = n_outputs;
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);

            cur = llm_build_norm(ctx0, ffn_inp, hparams,
                    model.layers[il].ffn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "ffn_norm", il);

            if ((uint32_t) il < hparams.n_layer_dense_lead) {
                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   NULL, NULL,
                        model.layers[il].ffn_gate, NULL, NULL,
                        model.layers[il].ffn_down, NULL, NULL,
                        NULL,
                        LLM_FFN_SILU, LLM_FFN_PAR, cb, il);
                cb(cur, "ffn_out", il);
            } else {
                // MoE branch
                lm_ggml_tensor * moe_out =
                        llm_build_moe_ffn(ctx0, lctx, cur,
                            model.layers[il].ffn_gate_inp,
                            model.layers[il].ffn_up_exps,
                            model.layers[il].ffn_gate_exps,
                            model.layers[il].ffn_down_exps,
                            n_expert, n_expert_used,
                            LLM_FFN_SILU, false,
                            true, hparams.expert_weights_scale,
                            cb, il);
                cb(moe_out, "ffn_moe_out", il);

                // FFN shared expert
                {
                    lm_ggml_tensor * ffn_shexp = llm_build_ffn(ctx0, lctx, cur,
                            model.layers[il].ffn_up_shexp,   NULL, NULL,
                            model.layers[il].ffn_gate_shexp, NULL, NULL,
                            model.layers[il].ffn_down_shexp, NULL, NULL,
                            NULL,
                            LLM_FFN_SILU, LLM_FFN_PAR, cb, il);
                    cb(ffn_shexp, "ffn_shexp", il);

                    cur = lm_ggml_add(ctx0, moe_out, ffn_shexp);
                    cb(cur, "ffn_out", il);
                }
            }

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, NULL,
                LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = lm_ggml_mul_mat(ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_bitnet() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                // compute Q and K and RoPE them
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur);
                if (model.layers[il].wq_scale) {
                    Qcur = lm_ggml_mul(ctx0, Qcur, model.layers[il].wq_scale);
                }
                cb(Qcur, "Qcur", il);
                if (model.layers[il].bq) {
                    Qcur = lm_ggml_add(ctx0, Qcur, model.layers[il].bq);
                    cb(Qcur, "Qcur", il);
                }

                // B1.K
                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur);
                if (model.layers[il].wk_scale) {
                    Kcur = lm_ggml_mul(ctx0, Kcur, model.layers[il].wk_scale);
                }
                cb(Kcur, "Kcur", il);
                if (model.layers[il].bk) {
                    Kcur = lm_ggml_add(ctx0, Kcur, model.layers[il].bk);
                    cb(Kcur, "Kcur", il);
                }

                // B1.V
                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur);
                if (model.layers[il].wv_scale) {
                    Vcur = lm_ggml_mul(ctx0, Vcur, model.layers[il].wv_scale);
                }
                cb(Vcur, "Vcur", il);
                if (model.layers[il].bv) {
                    Vcur = lm_ggml_add(ctx0, Vcur, model.layers[il].bv);
                    cb(Vcur, "Vcur", il);
                }

                Qcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        NULL, NULL,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);

                cur = llm_build_norm(ctx0, cur, hparams,
                        model.layers[il].attn_sub_norm, NULL,
                        LLM_NORM_RMS, cb, il);
                cb(cur, "attn_sub_norm", il);

                cur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wo, cur);
                if (model.layers[il].wo_scale) {
                    cur = lm_ggml_mul(ctx0, cur, model.layers[il].wo_scale);
                }
                if (model.layers[il].bo) {
                    cur = lm_ggml_add(ctx0, cur, model.layers[il].bo);
                }
                cb(cur, "attn_o_out", il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);

            // feed-forward forward
            cur = llm_build_norm(ctx0, ffn_inp, hparams,
                    model.layers[il].ffn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "ffn_norm", il);

            cur = llm_build_ffn(ctx0, lctx, cur,
                    model.layers[il].ffn_up,   NULL, model.layers[il].ffn_up_scale,
                    model.layers[il].ffn_gate, NULL, model.layers[il].ffn_gate_scale,
                    NULL,                      NULL, NULL,
                    NULL,
                    LLM_FFN_SILU, LLM_FFN_PAR, cb, il);
            cb(cur, "ffn_sub_out", il);

            cur = llm_build_norm(ctx0, cur, hparams,
                            model.layers[il].ffn_sub_norm, NULL,
                            LLM_NORM_RMS, cb, il);
            cb(cur, "ffn_sub_norm", il);

            cur = llm_build_lora_mm(lctx, ctx0, model.layers[il].ffn_down, cur);
            if (model.layers[il].ffn_down_scale) {
                cur = lm_ggml_mul(ctx0, cur, model.layers[il].ffn_down_scale);
            }
            cb(cur, "ffn_down", il);

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, NULL,
                LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        // FIXME: do not use model.tok_embd directly, duplicate as model.output
        cur = llm_build_lora_mm(lctx, ctx0, model.tok_embd, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);
        return gf;
    }

    struct lm_ggml_cgraph * build_t5_encoder() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        // mutable variable, needed during the last layer of the computation to skip unused tokens
        int32_t n_tokens = this->n_tokens;

        const int64_t n_embd_head = hparams.n_embd_head_v;
        const int64_t n_embd_gqa  = hparams.n_embd_v_gqa();
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        LM_GGML_ASSERT(lctx.is_encoding);
        struct lm_ggml_tensor * pos_bucket_enc = llm_build_pos_bucket(false);

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask_enc = build_inp_KQ_mask(false);

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            // norm
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm_enc, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq_enc, cur);
                cb(Qcur, "Qcur", il);

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk_enc, cur);
                cb(Kcur, "Kcur", il);

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv_enc, cur);
                cb(Vcur, "Vcur", il);

                Qcur = lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens);
                Kcur = lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens);

                struct lm_ggml_tensor * q =                 lm_ggml_permute(ctx0, Qcur, 0, 2, 1, 3);
                struct lm_ggml_tensor * k = lm_ggml_cont(ctx0, lm_ggml_permute(ctx0, Kcur, 0, 2, 1, 3));

                struct lm_ggml_tensor * kq = lm_ggml_mul_mat(ctx0, k, q);
                cb(kq, "kq", il);

                struct lm_ggml_tensor * attn_rel_b = model.layers[il].attn_rel_b_enc ? model.layers[il].attn_rel_b_enc : model.layers[0].attn_rel_b_enc;
                struct lm_ggml_tensor * pos_bias = llm_build_pos_bias(pos_bucket_enc, attn_rel_b);
                struct lm_ggml_tensor * kq_b = lm_ggml_add(ctx0, kq, pos_bias);
                cb(kq_b, "kq_b", il);

                kq = lm_ggml_soft_max_ext(ctx0, kq_b, KQ_mask_enc, 1.0f, hparams.f_max_alibi_bias);
                cb(kq, "kq_soft_max_ext", il);

                struct lm_ggml_tensor * v = lm_ggml_cont(ctx0, lm_ggml_transpose(ctx0, lm_ggml_reshape_2d(ctx0, Vcur, n_embd_gqa, n_tokens)));
                cb(v, "v", il);

                struct lm_ggml_tensor * kqv = lm_ggml_mul_mat(ctx0, lm_ggml_reshape_3d(ctx0, v, n_tokens, n_embd_head, n_head_kv), kq);
                cb(kqv, "kqv", il);

                struct lm_ggml_tensor * kqv_merged = lm_ggml_permute(ctx0, kqv, 0, 2, 1, 3);
                cb(kqv_merged, "kqv_merged", il);

                cur = lm_ggml_cont_2d(ctx0, kqv_merged, n_embd_gqa, n_tokens);
                cb(cur, "kqv_merged_cont", il);

                lm_ggml_build_forward_expand(gf, cur);

                cur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wo_enc, cur);
                cb(cur, "kqv_out", il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                n_tokens = n_outputs;
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);

            // feed-forward network
            {
                cur = llm_build_norm(ctx0, ffn_inp, hparams,
                        model.layers[il].ffn_norm_enc, NULL,
                        LLM_NORM_RMS, cb, il);
                cb(cur, "ffn_norm", il);

                // T5 uses relu, flan-T5 uses gelu-gated
                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up_enc,   NULL, NULL,
                        model.layers[il].ffn_gate_enc, NULL, NULL,
                        model.layers[il].ffn_down_enc, NULL, NULL,
                        NULL,
                        model.layers[il].ffn_gate_enc ? LLM_FFN_GELU : LLM_FFN_RELU,
                        model.layers[il].ffn_gate_enc ? LLM_FFN_PAR  : LLM_FFN_SEQ,
                        cb, il);
                cb(cur, "ffn_out", il);
            }

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cb(cur, "ffn_out", il);

            lm_ggml_tensor * layer_dir = lctx.cvec.tensor_for(il);
            if (layer_dir != nullptr) {
                cur = lm_ggml_add(ctx0, cur, layer_dir);
            }
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;
        cb(cur, "result_embd", -1);

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm_enc, NULL,
                LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_t5_decoder() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        // mutable variable, needed during the last layer of the computation to skip unused tokens
        int32_t n_tokens = this->n_tokens;

        const int64_t n_embd_head = hparams.n_embd_head_v;
        const int64_t n_embd_gqa  = hparams.n_embd_v_gqa();
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        LM_GGML_ASSERT(!lctx.is_encoding);
        LM_GGML_ASSERT(n_outputs_enc > 0 && "call llama_encode() first");

        struct lm_ggml_tensor * embd_enc       = llm_build_inp_embd_enc();
        struct lm_ggml_tensor * pos_bucket_dec = llm_build_pos_bucket(true);

        struct lm_ggml_tensor * KQ_mask_dec   = build_inp_KQ_mask();
        struct lm_ggml_tensor * KQ_mask_cross = llm_build_inp_KQ_mask_cross();

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            // norm
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur);
                cb(Qcur, "Qcur", il);

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur);
                cb(Kcur, "Kcur", il);

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur);
                cb(Vcur, "Vcur", il);

                llm_build_kv_store(ctx0, hparams, cparams, kv_self, gf, Kcur, Vcur, n_tokens, kv_head, cb, il);

                struct lm_ggml_tensor * k =
                    lm_ggml_view_3d(ctx0, kv_self.k_l[il],
                            n_embd_head_k, n_kv, n_head_kv,
                            lm_ggml_row_size(kv_self.k_l[il]->type, n_embd_k_gqa),
                            lm_ggml_row_size(kv_self.k_l[il]->type, n_embd_head_k),
                            0);
                cb(k, "k", il);

                struct lm_ggml_tensor * v =
                    lm_ggml_view_3d(ctx0, kv_self.v_l[il],
                            n_kv, n_embd_head_v, n_head_kv,
                            lm_ggml_element_size(kv_self.v_l[il])*n_ctx,
                            lm_ggml_element_size(kv_self.v_l[il])*n_ctx*n_embd_head_v,
                            0);
                cb(v, "v", il);

                Qcur = lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens);

                struct lm_ggml_tensor * q = lm_ggml_permute(ctx0, Qcur, 0, 2, 1, 3);

                struct lm_ggml_tensor * kq = lm_ggml_mul_mat(ctx0, k, q);
                cb(kq, "kq", il);

                struct lm_ggml_tensor * attn_rel_b = model.layers[il].attn_rel_b ? model.layers[il].attn_rel_b : model.layers[0].attn_rel_b;
                struct lm_ggml_tensor * pos_bias = llm_build_pos_bias(pos_bucket_dec, attn_rel_b);
                struct lm_ggml_tensor * kq_b = lm_ggml_add(ctx0, kq, pos_bias);
                cb(kq_b, "kq_b", il);

                kq = lm_ggml_soft_max_ext(ctx0, kq_b, KQ_mask_dec, 1.0f, hparams.f_max_alibi_bias);
                cb(kq, "kq_soft_max_ext", il);

                struct lm_ggml_tensor * kqv = lm_ggml_mul_mat(ctx0, v, kq);
                cb(kqv, "kqv", il);

                struct lm_ggml_tensor * kqv_merged = lm_ggml_permute(ctx0, kqv, 0, 2, 1, 3);
                cb(kqv_merged, "kqv_merged", il);

                cur = lm_ggml_cont_2d(ctx0, kqv_merged, n_embd_gqa, n_tokens);
                cb(cur, "kqv_merged_cont", il);

                lm_ggml_build_forward_expand(gf, cur);

                cur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wo, cur);
                cb(cur, "kqv_out", il);
            }

            cur = lm_ggml_add(ctx0, cur, inpSA);
            cb(cur, "cross_inp", il);

            struct lm_ggml_tensor * inpCA = cur;

            // norm
            cur = llm_build_norm(ctx0, cur, hparams,
                    model.layers[il].attn_norm_cross, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_norm_cross", il);

            // cross-attention
            {
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq_cross, cur);
                cb(Qcur, "Qcur", il);

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk_cross, embd_enc);
                cb(Kcur, "Kcur", il);

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv_cross, embd_enc);
                cb(Vcur, "Vcur", il);

                Qcur = lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head,    n_tokens);
                Kcur = lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_outputs_enc);

                struct lm_ggml_tensor * q =                 lm_ggml_permute(ctx0, Qcur, 0, 2, 1, 3);
                struct lm_ggml_tensor * k = lm_ggml_cont(ctx0, lm_ggml_permute(ctx0, Kcur, 0, 2, 1, 3));

                struct lm_ggml_tensor * kq = lm_ggml_mul_mat(ctx0, k, q);
                cb(kq, "kq", il);

                kq = lm_ggml_soft_max_ext(ctx0, kq, KQ_mask_cross, 1.0f, hparams.f_max_alibi_bias);
                cb(kq, "kq_soft_max_ext", il);

                struct lm_ggml_tensor * v = lm_ggml_cont(ctx0, lm_ggml_transpose(ctx0, lm_ggml_reshape_2d(ctx0, Vcur, n_embd_gqa, n_outputs_enc)));
                cb(v, "v", il);

                struct lm_ggml_tensor * kqv = lm_ggml_mul_mat(ctx0, lm_ggml_reshape_3d(ctx0, v, n_outputs_enc, n_embd_head, n_head_kv), kq);
                cb(kqv, "kqv", il);

                struct lm_ggml_tensor * kqv_merged = lm_ggml_permute(ctx0, kqv, 0, 2, 1, 3);
                cb(kqv_merged, "kqv_merged", il);

                cur = lm_ggml_cont_2d(ctx0, kqv_merged, n_embd_gqa, n_tokens);
                cb(cur, "kqv_merged_cont", il);

                lm_ggml_build_forward_expand(gf, cur);

                cur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wo_cross, cur);
                cb(cur, "kqv_out", il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                n_tokens = n_outputs;
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
                inpCA = lm_ggml_get_rows(ctx0, inpCA, inp_out_ids);
            }

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpCA);
            cb(ffn_inp, "ffn_inp", il);

            // feed-forward network
            {
                cur = llm_build_norm(ctx0, ffn_inp, hparams,
                        model.layers[il].ffn_norm, NULL,
                        LLM_NORM_RMS, cb, il);
                cb(cur, "ffn_norm", il);

                // T5 uses relu, flan-T5 uses gelu-gated
                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   NULL, NULL,
                        model.layers[il].ffn_gate, NULL, NULL,
                        model.layers[il].ffn_down, NULL, NULL,
                        NULL,
                        model.layers[il].ffn_gate_enc ? LLM_FFN_GELU : LLM_FFN_RELU,
                        model.layers[il].ffn_gate_enc ? LLM_FFN_PAR : LLM_FFN_SEQ,
                        cb, il);
                cb(cur, "ffn_out", il);
            }

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cb(cur, "ffn_out", il);

            lm_ggml_tensor * layer_dir = lctx.cvec.tensor_for(il);
            if (layer_dir != nullptr) {
                cur = lm_ggml_add(ctx0, cur, layer_dir);
            }
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;
        cb(cur, "result_embd", -1);

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, NULL,
                LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_jais() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        const int64_t n_embd_gqa  = hparams.n_embd_v_gqa();
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm,
                    model.layers[il].attn_norm_b,
                    LLM_NORM, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                cur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wqkv, cur);
                cb(cur, "wqkv", il);

                cur = lm_ggml_add(ctx0, cur, model.layers[il].bqkv);
                cb(cur, "bqkv", il);

                struct lm_ggml_tensor * Qcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd,     n_tokens, cur->nb[1], 0*cur->nb[0]*(n_embd)));
                struct lm_ggml_tensor * Kcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*cur->nb[0]*(n_embd)));
                struct lm_ggml_tensor * Vcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*cur->nb[0]*(n_embd + n_embd_gqa)));

                cb(Qcur, "Qcur", il);
                cb(Kcur, "Kcur", il);
                cb(Vcur, "Vcur", il);

                Qcur = lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, model.layers[il].bo,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/float(n_embd_head), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur  = lm_ggml_get_rows(ctx0,  cur, inp_out_ids);
                inpL = lm_ggml_get_rows(ctx0, inpL, inp_out_ids);
            }

            // add the input
            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpL);
            cb(ffn_inp, "ffn_inp", il);

            // FF
            {
                cur = llm_build_norm(ctx0, ffn_inp, hparams,
                        model.layers[il].ffn_norm,
                        model.layers[il].ffn_norm_b,
                        LLM_NORM, cb, il);
                cb(cur, "ffn_norm", il);

                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   model.layers[il].ffn_up_b,   NULL,
                        model.layers[il].ffn_gate, model.layers[il].ffn_gate_b, NULL,
                        model.layers[il].ffn_down, model.layers[il].ffn_down_b, NULL,
                        NULL,
                        LLM_FFN_SILU, LLM_FFN_PAR, cb, il);
                cb(cur, "ffn_out", il);
            }

            inpL = lm_ggml_add(ctx0, cur, ffn_inp);
            cb(inpL, "l_out", il);
        }

        cur = llm_build_norm(ctx0, inpL, hparams,
                model.output_norm,
                model.output_norm_b,
                LLM_NORM, cb, -1);
        cb(cur, "result_norm", -1);

        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);

        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_chatglm() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        const int64_t n_embd_gqa  = hparams.n_embd_v_gqa();
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm,
                    NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                struct lm_ggml_tensor * Qcur = nullptr;
                struct lm_ggml_tensor * Kcur = nullptr;
                struct lm_ggml_tensor * Vcur = nullptr;

                cur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wqkv, cur);
                cb(cur, "wqkv", il);

                cur = lm_ggml_add(ctx0, cur, model.layers[il].bqkv);
                cb(cur, "bqkv", il);

                Qcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd,     n_tokens, cur->nb[1], 0*sizeof(float)*(n_embd)));
                Kcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*sizeof(float)*(n_embd)));
                Vcur = lm_ggml_cont(ctx0, lm_ggml_view_2d(ctx0, cur, n_embd_gqa, n_tokens, cur->nb[1], 1*sizeof(float)*(n_embd + n_embd_gqa)));

                cb(Qcur, "Qcur", il);
                cb(Kcur, "Kcur", il);
                cb(Vcur, "Vcur", il);
                //printf("freq_base: %f freq_scale: %f ext_factor: %f attn_factor: %f\n", freq_base, freq_scale, ext_factor, attn_factor);
                Qcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur_rope", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur_rope", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, NULL,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);

            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            // Add the input
            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);

            // FF
            {
                cur = llm_build_norm(ctx0, ffn_inp, hparams,
                        model.layers[il].ffn_norm,
                        NULL,
                        LLM_NORM_RMS, cb, il);
                cb(cur, "ffn_norm", il);

                cur = llm_build_ffn(ctx0, lctx, cur,
                        model.layers[il].ffn_up,   NULL, NULL,
                        NULL,                      NULL, NULL,
                        model.layers[il].ffn_down, NULL, NULL,
                        NULL,
                        LLM_FFN_SWIGLU, LLM_FFN_SEQ, cb, il);
                cb(cur, "ffn_out", il);

            }

            inpL = lm_ggml_add(ctx0, cur, ffn_inp);
            cb(inpL, "l_out", il);
        }

        cur = llm_build_norm(ctx0, inpL, hparams,
                model.output_norm,
                NULL,
                LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_nemotron() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        const int64_t n_embd_head = hparams.n_embd_head_v;
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);
        //LM_GGML_ASSERT(n_embd_head == hparams.n_rot);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            // norm
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm,
                    model.layers[il].attn_norm_b,
                    LLM_NORM, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                // compute Q and K and RoPE them
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur);
                cb(Qcur, "Qcur", il);
                if (model.layers[il].bq) {
                    Qcur = lm_ggml_add(ctx0, Qcur, model.layers[il].bq);
                    cb(Qcur, "Qcur", il);
                }

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur);
                cb(Kcur, "Kcur", il);
                if (model.layers[il].bk) {
                    Kcur = lm_ggml_add(ctx0, Kcur, model.layers[il].bk);
                    cb(Kcur, "Kcur", il);
                }

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur);
                cb(Vcur, "Vcur", il);
                if (model.layers[il].bv) {
                    Vcur = lm_ggml_add(ctx0, Vcur, model.layers[il].bv);
                    cb(Vcur, "Vcur", il);
                }

                Qcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, model.layers[il].bo,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);

            // feed-forward network
            cur = llm_build_norm(ctx0, ffn_inp, hparams,
                    model.layers[il].ffn_norm,
                    model.layers[il].ffn_norm_b,
                    LLM_NORM, cb, il);
            cb(cur, "ffn_norm", il);

            cur = llm_build_ffn(ctx0, lctx, cur,
                    model.layers[il].ffn_up,   model.layers[il].ffn_up_b,   NULL,
                    NULL,                      NULL,                        NULL,
                    model.layers[il].ffn_down, model.layers[il].ffn_down_b, NULL,
                    NULL,
                    LLM_FFN_RELU_SQR, LLM_FFN_SEQ, cb, il);

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cb(cur, "ffn_out", il);

            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, model.output_norm_b,
                LLM_NORM, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    struct lm_ggml_cgraph * build_exaone() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        // mutable variable, needed during the last layer of the computation to skip unused tokens
        int32_t n_tokens = this->n_tokens;

        const int64_t n_embd_head = hparams.n_embd_head_v;
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);
        LM_GGML_ASSERT(n_embd_head == hparams.n_rot);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            // norm
            cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "attn_norm", il);

            // self-attention
            {
                // rope freq factors for llama3; may return nullptr for llama2 and other models
                struct lm_ggml_tensor * rope_factors = build_rope_factors(il);

                // compute Q and K and RoPE them
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur);
                cb(Qcur, "Qcur", il);
                if (model.layers[il].bq) {
                    Qcur = lm_ggml_add(ctx0, Qcur, model.layers[il].bq);
                    cb(Qcur, "Qcur", il);
                }

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur);
                cb(Kcur, "Kcur", il);
                if (model.layers[il].bk) {
                    Kcur = lm_ggml_add(ctx0, Kcur, model.layers[il].bk);
                    cb(Kcur, "Kcur", il);
                }

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur);
                cb(Vcur, "Vcur", il);
                if (model.layers[il].bv) {
                    Vcur = lm_ggml_add(ctx0, Vcur, model.layers[il].bv);
                    cb(Vcur, "Vcur", il);
                }

                Qcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens), inp_pos, rope_factors,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens), inp_pos, rope_factors,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, model.layers[il].bo,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                n_tokens = n_outputs;
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);

            // feed-forward network
            cur = llm_build_norm(ctx0, ffn_inp, hparams,
                    model.layers[il].ffn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
            cb(cur, "ffn_norm", il);

            cur = llm_build_ffn(ctx0, lctx, cur,
                    model.layers[il].ffn_up,   NULL, NULL,
                    model.layers[il].ffn_gate, NULL, NULL,
                    model.layers[il].ffn_down, NULL, NULL,
                    NULL,
                    LLM_FFN_SILU, LLM_FFN_PAR, cb, il);
            cb(cur, "ffn_out", il);

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cb(cur, "ffn_out", il);

            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, NULL,
                LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    lm_ggml_cgraph * build_rwkv6() {
        lm_ggml_cgraph *gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        // Token shift state dimensions should be 2 * n_emb
        LM_GGML_ASSERT(n_embd == hparams.n_embd_k_s() / 2);

        const int64_t n_seqs = ubatch.n_seqs;
        const int64_t n_seq_tokens = ubatch.n_seq_tokens;
        const int64_t n_tokens = ubatch.n_tokens;
        LM_GGML_ASSERT(n_seqs != 0);
        LM_GGML_ASSERT(ubatch.equal_seqs);
        LM_GGML_ASSERT(n_tokens == n_seq_tokens * n_seqs);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;
        struct lm_ggml_tensor * state_copy = build_inp_s_copy();
        struct lm_ggml_tensor * state_mask = build_inp_s_mask();

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);
        inpL = llm_build_norm(ctx0, inpL, hparams, model.tok_norm, model.tok_norm_b, LLM_NORM, cb, -1);

        for (int il = 0; il < n_layer; ++il) {
            const llama_layer * layer = &model.layers[il];

            // (ab)using the KV cache to store the states
            struct lm_ggml_tensor * token_shift = llm_build_copy_mask_state(ctx0,
                    gf, kv_self.k_l[il], state_copy, state_mask,
                    hparams.n_embd_k_s(), kv_self.size, kv_head, n_kv, n_seqs);
            struct lm_ggml_tensor * wkv_states = llm_build_copy_mask_state(ctx0,
                    gf, kv_self.v_l[il], state_copy, state_mask,
                    hparams.n_embd_v_s(), kv_self.size, kv_head, n_kv, n_seqs);

            cur = lm_ggml_reshape_3d(ctx0, inpL, n_embd, n_seq_tokens, n_seqs);
            token_shift = lm_ggml_reshape_3d(ctx0, token_shift, n_embd, 2, n_seqs);

            struct lm_ggml_tensor * att_shift = lm_ggml_view_3d(ctx0, token_shift, n_embd, 1, n_seqs, token_shift->nb[1], token_shift->nb[2], 0);
            struct lm_ggml_tensor * ffn_shift = lm_ggml_view_3d(ctx0, token_shift, n_embd, 1, n_seqs, token_shift->nb[1], token_shift->nb[2], n_embd * lm_ggml_element_size(token_shift));

            struct lm_ggml_tensor * x_norm_att = llm_build_norm(ctx0, cur, hparams, layer->attn_norm, layer->attn_norm_b, LLM_NORM, cb, il);
            struct lm_ggml_tensor * x_prev = lm_ggml_concat(
                ctx0,
                att_shift,
                lm_ggml_view_3d(ctx0, x_norm_att, n_embd, n_seq_tokens - 1, n_seqs, x_norm_att->nb[1], x_norm_att->nb[2], 0),
                1
            );

            cur = lm_ggml_add(ctx0, cur, llm_build_rwkv6_time_mix(lctx, ctx0, layer, x_norm_att, x_prev, &wkv_states));
            lm_ggml_build_forward_expand(gf, cur);
            lm_ggml_build_forward_expand(
                gf,
                lm_ggml_cpy(
                    ctx0,
                    wkv_states,
                    lm_ggml_view_1d(
                        ctx0,
                        kv_self.v_l[il],
                        hparams.n_embd_v_s() * n_seqs,
                        hparams.n_embd_v_s() * kv_head * lm_ggml_element_size(kv_self.v_l[il])
                    )
                )
            );

            struct lm_ggml_tensor * x_norm_ffn = llm_build_norm(ctx0, cur, hparams, layer->attn_norm_2, layer->attn_norm_2_b, LLM_NORM, cb, il);
            x_prev = lm_ggml_concat(
                ctx0,
                ffn_shift,
                lm_ggml_view_3d(ctx0, x_norm_ffn, n_embd, n_seq_tokens - 1, n_seqs, x_norm_ffn->nb[1], x_norm_ffn->nb[2], 0),
                1
            );
            cur = lm_ggml_add(ctx0, cur, llm_build_rwkv6_channel_mix(lctx, ctx0, layer, x_norm_ffn, x_prev));
            lm_ggml_build_forward_expand(gf, cur);

            struct lm_ggml_tensor * last_norm_att = lm_ggml_view_3d(ctx0, x_norm_att, n_embd, 1, n_seqs, x_norm_att->nb[1], x_norm_att->nb[2], (n_seq_tokens-1)*n_embd*lm_ggml_element_size(x_norm_att));
            struct lm_ggml_tensor * last_norm_ffn = lm_ggml_view_3d(ctx0, x_norm_ffn, n_embd, 1, n_seqs, x_norm_ffn->nb[1], x_norm_ffn->nb[2], (n_seq_tokens-1)*n_embd*lm_ggml_element_size(x_norm_ffn));

            token_shift = lm_ggml_concat(ctx0, last_norm_att, last_norm_ffn, 1);

            lm_ggml_build_forward_expand(
                gf,
                lm_ggml_cpy(
                    ctx0,
                    lm_ggml_view_1d(ctx0, token_shift, n_embd * n_seqs * 2, 0),
                    lm_ggml_view_1d(ctx0, kv_self.k_l[il], hparams.n_embd_k_s() * n_seqs, hparams.n_embd_k_s() * kv_head * lm_ggml_element_size(kv_self.k_l[il]))
                )
            );

            if (hparams.rescale_every_n_layers != 0 && (il + 1) % hparams.rescale_every_n_layers == 0) {
                cur = lm_ggml_scale(ctx0, cur, 0.5F);
            }

            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;
        struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
        cur = lm_ggml_reshape_2d(ctx0, cur, n_embd, n_tokens);
        cur = lm_ggml_get_rows(ctx0, cur, inp_out_ids);

        cur = llm_build_norm(ctx0, cur, hparams, model.output_norm, model.output_norm_b, LLM_NORM, cb, -1);
        cb(cur, "result_norm", -1);

        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }

    // ref: https://github.com/facebookresearch/chameleon
    // based on the original build_llama() function, changes:
    //   * qk-norm
    //   * swin-norm
    //   * removed bias
    //   * removed MoE
    struct lm_ggml_cgraph * build_chameleon() {
        struct lm_ggml_cgraph * gf = lm_ggml_new_graph_custom(ctx0, llama_model_max_nodes(model), false);

        // mutable variable, needed during the last layer of the computation to skip unused tokens
        int32_t n_tokens = this->n_tokens;

        const int64_t n_embd_head = hparams.n_embd_head_v;
        LM_GGML_ASSERT(n_embd_head == hparams.n_embd_head_k);
        LM_GGML_ASSERT(n_embd_head == hparams.n_rot);

        struct lm_ggml_tensor * cur;
        struct lm_ggml_tensor * inpL;

        inpL = llm_build_inp_embd(ctx0, lctx, hparams, ubatch, model.tok_embd, cb);

        // inp_pos - contains the positions
        struct lm_ggml_tensor * inp_pos = build_inp_pos();

        // KQ_mask (mask for 1 head, it will be broadcasted to all heads)
        struct lm_ggml_tensor * KQ_mask = build_inp_KQ_mask();

        for (int il = 0; il < n_layer; ++il) {
            struct lm_ggml_tensor * inpSA = inpL;

            // norm
            if (hparams.swin_norm) {
                cur = inpL;
            } else {
                cur = llm_build_norm(ctx0, inpL, hparams,
                    model.layers[il].attn_norm, NULL,
                    LLM_NORM_RMS, cb, il);
                cb(cur, "attn_norm", il);
            }

            // self-attention
            {
                // compute Q and K and RoPE them
                struct lm_ggml_tensor * Qcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wq, cur);
                cb(Qcur, "Qcur", il);

                struct lm_ggml_tensor * Kcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wk, cur);
                cb(Kcur, "Kcur", il);

                struct lm_ggml_tensor * Vcur = llm_build_lora_mm(lctx, ctx0, model.layers[il].wv, cur);
                cb(Vcur, "Vcur", il);

                if (model.layers[il].attn_q_norm) {
                    Qcur = lm_ggml_view_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens,
                                lm_ggml_element_size(Qcur) * n_embd_head,
                                lm_ggml_element_size(Qcur) * n_embd_head * n_head,
                                0);
                    cb(Qcur, "Qcur", il);

                    Qcur = llm_build_norm(ctx0, Qcur, hparams,
                                model.layers[il].attn_q_norm,
                                model.layers[il].attn_q_norm_b,
                                LLM_NORM, cb, il);
                    cb(Qcur, "Qcur", il);
                }

                if (model.layers[il].attn_k_norm) {
                    Kcur = lm_ggml_view_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens,
                                lm_ggml_element_size(Kcur) * n_embd_head,
                                lm_ggml_element_size(Kcur) * n_embd_head * n_head_kv,
                                0);
                    cb(Kcur, "Kcur", il);

                    Kcur = llm_build_norm(ctx0, Kcur, hparams,
                               model.layers[il].attn_k_norm,
                               model.layers[il].attn_k_norm_b,
                               LLM_NORM, cb, il);
                    cb(Kcur, "Kcur", il);
                }

                Qcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Qcur, n_embd_head, n_head, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Qcur, "Qcur", il);

                Kcur = lm_ggml_rope_ext(
                    ctx0, lm_ggml_reshape_3d(ctx0, Kcur, n_embd_head, n_head_kv, n_tokens), inp_pos, nullptr,
                    n_rot, rope_type, n_ctx_orig, freq_base, freq_scale,
                    ext_factor, attn_factor, beta_fast, beta_slow
                );
                cb(Kcur, "Kcur", il);

                cur = llm_build_kv(ctx0, lctx, kv_self, gf,
                        model.layers[il].wo, nullptr,
                        Kcur, Vcur, Qcur, KQ_mask, n_tokens, kv_head, n_kv, 1.0f/sqrtf(float(n_embd_head)), cb, il);

                if (hparams.swin_norm) {
                    cur = llm_build_norm(ctx0, cur, hparams,
                        model.layers[il].attn_norm, NULL,
                        LLM_NORM_RMS, cb, il);
                }
            }

            if (il == n_layer - 1) {
                // skip computing output for unused tokens
                struct lm_ggml_tensor * inp_out_ids = build_inp_out_ids();
                n_tokens = n_outputs;
                cur   = lm_ggml_get_rows(ctx0,   cur, inp_out_ids);
                inpSA = lm_ggml_get_rows(ctx0, inpSA, inp_out_ids);
            }

            struct lm_ggml_tensor * ffn_inp = lm_ggml_add(ctx0, cur, inpSA);
            cb(ffn_inp, "ffn_inp", il);

            // feed-forward network
            if (!hparams.swin_norm) {
                cur = llm_build_norm(ctx0, ffn_inp, hparams,
                        model.layers[il].ffn_norm, NULL,
                        LLM_NORM_RMS, cb, il);
                cb(cur, "ffn_norm", il);
            }

            cur = llm_build_ffn(ctx0, lctx, cur,
                    model.layers[il].ffn_up,   NULL, NULL,
                    model.layers[il].ffn_gate, NULL, NULL,
                    model.layers[il].ffn_down, NULL, NULL,
                    NULL,
                    LLM_FFN_SILU, LLM_FFN_PAR, cb, il);
            cb(cur, "ffn_out", il);

            if (hparams.swin_norm) {
                cur = llm_build_norm(ctx0, cur, hparams,
                        model.layers[il].ffn_norm, NULL,
                        LLM_NORM_RMS, cb, il);
                cb(cur, "ffn_norm", il);
            }

            cur = lm_ggml_add(ctx0, cur, ffn_inp);
            cb(cur, "ffn_out", il);

            cur = lctx.cvec.apply_to(ctx0, cur, il);
            cb(cur, "l_out", il);

            // input for next layer
            inpL = cur;
        }

        cur = inpL;

        cur = llm_build_norm(ctx0, cur, hparams,
                model.output_norm, NULL,
                LLM_NORM_RMS, cb, -1);
        cb(cur, "result_norm", -1);

        // lm_head
        cur = llm_build_lora_mm(lctx, ctx0, model.output, cur);
        cb(cur, "result_output_with_img_logits", -1);

        // TODO: this suppresses the output of image tokens, which is required to enable text-only outputs.
        // Needs to be removed once image outputs are supported.
        int img_token_end_idx = 8196;
        int img_token_start_idx = 4;
        int num_img_tokens = img_token_end_idx - img_token_start_idx;
        // creates 1d tensor of size num_img_tokens and values -FLT_MAX,
        // which ensures that text token values are always at least larger than image token values
        struct lm_ggml_tensor * img_logits = lm_ggml_new_tensor_1d(ctx0, LM_GGML_TYPE_F32, num_img_tokens);
        img_logits = lm_ggml_clamp(ctx0, img_logits, -FLT_MAX, -FLT_MAX);
        cb(img_logits, "img_logits", -1);
        cur = lm_ggml_set_1d(ctx0, cur, img_logits, lm_ggml_element_size(cur) * img_token_start_idx);
        cb(cur, "result_output", -1);

        lm_ggml_build_forward_expand(gf, cur);

        return gf;
    }
};

static struct lm_ggml_cgraph * llama_build_graph_defrag(llama_context & lctx, const std::vector<uint32_t> & ids) {
    llama_ubatch dummy = {};
    dummy.equal_seqs = true;

    llm_build_cb cb = [&](struct lm_ggml_tensor * , const char * , int ) { };

    struct llm_build_context llm(lctx, dummy, cb, false);

    llm.init();

    struct lm_ggml_cgraph * result = llm.build_defrag(ids);

    llm.free();

    return result;
}

static struct lm_ggml_cgraph * llama_build_graph_k_shift(llama_context & lctx) {
    llama_ubatch dummy = {};
    dummy.equal_seqs = true;

    llm_build_cb cb = [&](struct lm_ggml_tensor * , const char * , int ) { };

    struct llm_build_context llm(lctx, dummy, cb, false);

    llm.init();

    struct lm_ggml_cgraph * result = llm.build_k_shift();

    llm.free();

    return result;
}

static struct lm_ggml_cgraph * llama_build_graph(
         llama_context & lctx,
    const llama_ubatch & ubatch,
                  bool   worst_case) {
    const auto & model = lctx.model;

    // this callback allows us to apply custom logic to each tensor (e.g. ggml-alloc, offloading, etc.)
    llm_build_cb cb = [&](struct lm_ggml_tensor * cur, const char * name, int il) {
        if (il >= 0) {
            lm_ggml_format_name(cur, "%s-%d", name, il);
        } else {
            lm_ggml_set_name(cur, name);
        }

        if (!lctx.cparams.offload_kqv) {
            if (strcmp(name, "kqv_merged_cont") == 0) {
                // all nodes between the KV store and the attention output are run on the CPU
                lm_ggml_backend_sched_set_tensor_backend(lctx.sched.get(), cur, lctx.backend_cpu);
            }
        }

        // norm may be automatically assigned to the backend of the previous layer, increasing data transfer between backends
        // FIXME: fix in lm_ggml_backend_sched
        const bool full_offload = lctx.model.n_gpu_layers > (int)lctx.model.hparams.n_layer;
        if (ubatch.n_tokens < 32 || full_offload) {
            if (il != -1 && strcmp(name, "norm") == 0) {
                const auto & dev_layer = lctx.model.dev_layer.at(il);
                for (auto & backend : lctx.backends) {
                    if (lm_ggml_backend_get_device(backend.get()) == dev_layer.dev) {
                        if (lm_ggml_backend_supports_op(backend.get(), cur)) {
                            lm_ggml_backend_sched_set_tensor_backend(lctx.sched.get(), cur, backend.get());
                        }
                    }
                }
            }
        }
    };

    struct lm_ggml_cgraph * result = NULL;

    struct llm_build_context llm(lctx, ubatch, cb, worst_case);

    llm.init();

    switch (model.arch) {
        case LLM_ARCH_LLAMA:
        case LLM_ARCH_GRANITE:
        case LLM_ARCH_GRANITE_MOE:
            {
                result = llm.build_llama();
            } break;
        case LLM_ARCH_BAICHUAN:
            {
                result = llm.build_baichuan();
            } break;
        case LLM_ARCH_FALCON:
            {
                result = llm.build_falcon();
            } break;
        case LLM_ARCH_GROK:
            {
                result = llm.build_grok();
            } break;
        case LLM_ARCH_STARCODER:
            {
                result = llm.build_starcoder();
            } break;
        case LLM_ARCH_REFACT:
            {
                result = llm.build_refact();
            } break;
        case LLM_ARCH_BERT:
        case LLM_ARCH_JINA_BERT_V2:
        case LLM_ARCH_NOMIC_BERT:
            {
                result = llm.build_bert();
            } break;
        case LLM_ARCH_BLOOM:
            {
                result = llm.build_bloom();
            } break;
        case LLM_ARCH_MPT:
            {
                result = llm.build_mpt();
            } break;
         case LLM_ARCH_STABLELM:
            {
                result = llm.build_stablelm();
            } break;
        case LLM_ARCH_QWEN:
            {
                result = llm.build_qwen();
            } break;
        case LLM_ARCH_QWEN2:
            {
                result = llm.build_qwen2();
            } break;
        case LLM_ARCH_QWEN2MOE:
            {
                result = llm.build_qwen2moe();
            } break;
        case LLM_ARCH_PHI2:
            {
                result = llm.build_phi2();
            } break;
        case LLM_ARCH_PHI3:
            {
                result = llm.build_phi3();
            } break;
        case LLM_ARCH_PLAMO:
            {
                result = llm.build_plamo();
            } break;
        case LLM_ARCH_GPT2:
            {
                result = llm.build_gpt2();
            } break;
        case LLM_ARCH_CODESHELL:
            {
                result = llm.build_codeshell();
            } break;
        case LLM_ARCH_ORION:
            {
                result = llm.build_orion();
            } break;
        case LLM_ARCH_INTERNLM2:
            {
                result = llm.build_internlm2();
            } break;
        case LLM_ARCH_MINICPM:
            {
                result = llm.build_minicpm();
            } break;
        case LLM_ARCH_MINICPM3:
            {
                result = llm.build_minicpm3();
            } break;
        case LLM_ARCH_GEMMA:
            {
                result = llm.build_gemma();
            } break;
        case LLM_ARCH_GEMMA2:
            {
                result = llm.build_gemma2();
            } break;
        case LLM_ARCH_STARCODER2:
            {
                result = llm.build_starcoder2();
            } break;
        case LLM_ARCH_MAMBA:
            {
                result = llm.build_mamba();
            } break;
        case LLM_ARCH_XVERSE:
            {
                result = llm.build_xverse();
            } break;
        case LLM_ARCH_COMMAND_R:
            {
                result = llm.build_command_r();
            } break;
        case LLM_ARCH_DBRX:
            {
                result = llm.build_dbrx();
            } break;
        case LLM_ARCH_OLMO:
            {
                result = llm.build_olmo();
            } break;
        case LLM_ARCH_OLMO_1124:
            {
                result = llm.build_olmo_1124();
            } break;
        case LLM_ARCH_OLMOE:
            {
                result = llm.build_olmoe();
            } break;
        case LLM_ARCH_OPENELM:
            {
                result = llm.build_openelm();
            } break;
        case LLM_ARCH_GPTNEOX:
            {
                result = llm.build_gptneox();
            } break;
        case LLM_ARCH_ARCTIC:
            {
                result = llm.build_arctic();
            } break;
        case LLM_ARCH_DEEPSEEK2:
            {
                result = llm.build_deepseek2();
            } break;
        case LLM_ARCH_CHATGLM:
            {
                result = llm.build_chatglm();
            } break;
        case LLM_ARCH_BITNET:
            {
                result = llm.build_bitnet();
            } break;
        case LLM_ARCH_T5:
            {
                if (lctx.is_encoding) {
                    result = llm.build_t5_encoder();
                } else {
                    result = llm.build_t5_decoder();
                }
            } break;
        case LLM_ARCH_T5ENCODER:
            {
                result = llm.build_t5_encoder();
            } break;
        case LLM_ARCH_JAIS:
            {
                result = llm.build_jais();
            } break;
        case LLM_ARCH_NEMOTRON:
            {
                result = llm.build_nemotron();
            } break;
        case LLM_ARCH_EXAONE:
            {
                result = llm.build_exaone();
            } break;
        case LLM_ARCH_RWKV6:
            {
                result = llm.build_rwkv6();
            } break;
        case LLM_ARCH_CHAMELEON:
            {
                result = llm.build_chameleon();
            } break;
        default:
            LM_GGML_ABORT("fatal error");
    }

    // add on pooling layer
    if (lctx.cparams.embeddings) {
        result = llm.append_pooling(result);
    }

    llm.free();

    return result;
}

static void llama_set_k_shift(llama_context & lctx) {
    const int64_t kv_size = lctx.kv_self.size;

    assert(lm_ggml_backend_buffer_is_host(lctx.inp_K_shift->buffer));

    int32_t * data = (int32_t *) lctx.inp_K_shift->data;

    for (int i = 0; i < kv_size; ++i) {
        data[i] = lctx.kv_self.cells[i].delta;
    }
}

static void llama_set_s_copy(llama_context & lctx) {
    const int64_t kv_size = lctx.kv_self.size;

    assert(lm_ggml_backend_buffer_is_host(lctx.inp_s_copy->buffer));

    int32_t * data = (int32_t *) lctx.inp_s_copy->data;

    for (int i = 0; i < kv_size; ++i) {
        data[i] = lctx.kv_self.cells[i].src;
    }
}

static int32_t llama_relative_position_bucket(llama_pos x, llama_pos y, uint64_t n_buckets, bool bidirectional) {
    // TODO move to hparams if a T5 variant appears that uses a different value
    const int64_t max_distance = 128;

    if (bidirectional) {
        n_buckets >>= 1;
    }

    const int64_t max_exact = n_buckets >> 1;

    int32_t relative_position = x - y;
    int32_t relative_bucket = 0;
    if (bidirectional) {
        relative_bucket += (relative_position > 0) * n_buckets;
        relative_position = abs(relative_position);
    } else {
        relative_position = -std::min<int32_t>(relative_position, 0);
    }
    int32_t relative_position_if_large = floorf(max_exact + logf(1.0 * relative_position / max_exact) * (n_buckets - max_exact) / log(1.0 * max_distance / max_exact));
    relative_position_if_large = std::min<int32_t>(relative_position_if_large, n_buckets - 1);
    relative_bucket += (relative_position < max_exact ? relative_position : relative_position_if_large);
    return relative_bucket;
}

static void llama_set_inputs(llama_context & lctx, const llama_ubatch & ubatch) {
    //
    // set input data
    //

    const auto & hparams = lctx.model.hparams;
    const auto & cparams = lctx.cparams;
    const auto & kv_self = lctx.kv_self;

    if (ubatch.token) {
        const int64_t n_tokens = ubatch.n_tokens;

        lm_ggml_backend_tensor_set(lctx.inp_tokens, ubatch.token, 0, n_tokens*lm_ggml_element_size(lctx.inp_tokens));
    }

    if (ubatch.embd) {
        const int64_t n_embd   = hparams.n_embd;
        const int64_t n_tokens = ubatch.n_tokens;

        lm_ggml_backend_tensor_set(lctx.inp_embd, ubatch.embd, 0, n_tokens*n_embd*lm_ggml_element_size(lctx.inp_embd));
    }

    if (ubatch.pos && lctx.inp_pos) {
        const int64_t n_tokens = ubatch.n_tokens;

        lm_ggml_backend_tensor_set(lctx.inp_pos, ubatch.pos, 0, n_tokens*lm_ggml_element_size(lctx.inp_pos));
    }

    if (hparams.causal_attn || cparams.pooling_type == LLAMA_POOLING_TYPE_NONE) {
        LM_GGML_ASSERT(lctx.inp_out_ids && "every model that can must skip unused outputs");
        const int64_t n_tokens = ubatch.n_tokens;

        LM_GGML_ASSERT(lm_ggml_backend_buffer_is_host(lctx.inp_out_ids->buffer));
        int32_t * data = (int32_t *) lctx.inp_out_ids->data;

        if (lctx.n_outputs == n_tokens) {
            for (int i = 0; i < n_tokens; ++i) {
                data[i] = i;
            }
        } else if (ubatch.output) {
            int32_t n_outputs = 0;
            for (int i = 0; i < n_tokens; ++i) {
                if (ubatch.output[i]) {
                    data[n_outputs++] = i;
                }
            }
            // the graph needs to have been passed the correct number of outputs
            LM_GGML_ASSERT(lctx.n_outputs == n_outputs);
        } else if (lctx.n_outputs == 1) {
            // only keep last output
            data[0] = n_tokens - 1;
        } else {
            LM_GGML_ASSERT(lctx.n_outputs == 0);
        }
    }

    LM_GGML_ASSERT(
        // (!a || b) is a logical implication (a -> b)
        // !hparams.causal_attn -> !cparams.causal_attn
        (hparams.causal_attn || !cparams.causal_attn) &&
        "causal attention is not supported by this model"
    );

    if (lctx.inp_KQ_mask || lctx.inp_KQ_mask_swa) {
        // NOTE: hparams.causal_attn indicates the model is capable of generation and uses the kv cache.
        if (cparams.causal_attn && !lctx.is_encoding) {
            const int64_t n_kv         = kv_self.n;
            const int64_t n_tokens     = ubatch.n_tokens;
            const int64_t n_seq_tokens = ubatch.n_seq_tokens;
            const int64_t n_seqs       = ubatch.n_seqs;


            float * data     = nullptr;
            float * data_swa = nullptr;

            if (lctx.inp_KQ_mask) {
                LM_GGML_ASSERT(lm_ggml_backend_buffer_is_host(lctx.inp_KQ_mask->buffer));
                data = (float *) lctx.inp_KQ_mask->data;
            }

            if (lctx.inp_KQ_mask_swa) {
                LM_GGML_ASSERT(lm_ggml_backend_buffer_is_host(lctx.inp_KQ_mask_swa->buffer));
                data_swa = (float *) lctx.inp_KQ_mask_swa->data;
            }

            // For causal attention, use only the previous KV cells
            // of the correct sequence for each token of the ubatch.
            // It's assumed that if a token in the batch has multiple sequences, they are equivalent.
            for (int h = 0; h < 1; ++h) {
                for (int s = 0; s < n_seqs; ++s) {
                    const llama_seq_id seq_id = ubatch.seq_id[s][0];

                    for (int j = 0; j < n_seq_tokens; ++j) {
                        const llama_pos pos = ubatch.pos[s*n_seq_tokens + j];

                        for (int i = 0; i < n_kv; ++i) {
                            float f;
                            if (!kv_self.cells[i].has_seq_id(seq_id) || kv_self.cells[i].pos > pos) {
                                f = -INFINITY;
                            } else {
                                if (hparams.use_alibi) {
                                    f = -std::abs(kv_self.cells[i].pos - pos);
                                } else {
                                    f = 0.0f;
                                }
                            }

                            if (data) {
                                data[h*(n_kv*n_tokens) + s*(n_kv*n_seq_tokens) + j*n_kv + i] = f;
                            }

                            // may need to cut off old tokens for sliding window
                            if (data_swa) {
                                if (pos - kv_self.cells[i].pos >= (int32_t)hparams.n_swa) {
                                    f = -INFINITY;
                                }
                                data_swa[h*(n_kv*n_tokens) + s*(n_kv*n_seq_tokens) + j*n_kv + i] = f;
                            }
                        }
                    }
                }

                if (data) {
                    for (int i = n_tokens; i < LM_GGML_PAD(n_tokens, LM_GGML_KQ_MASK_PAD); ++i) {
                        for (int j = 0; j < n_kv; ++j) {
                            data[h*(n_kv*n_tokens) + i*n_kv + j] = -INFINITY;
                        }
                    }
                }

                if (data_swa) {
                    for (int i = n_tokens; i < LM_GGML_PAD(n_tokens, LM_GGML_KQ_MASK_PAD); ++i) {
                        for (int j = 0; j < n_kv; ++j) {
                            data_swa[h*(n_kv*n_tokens) + i*n_kv + j] = -INFINITY;
                        }
                    }
                }
            }
        } else {
            const int64_t n_tokens     = ubatch.n_tokens;
            const int64_t n_seq_tokens = ubatch.n_seq_tokens;
            const int64_t n_seqs       = ubatch.n_seqs;
            // when using kv cache, the mask needs to match the kv cache size
            const int64_t n_stride = hparams.causal_attn && !lctx.is_encoding ? kv_self.n : n_tokens;

            LM_GGML_ASSERT(lm_ggml_backend_buffer_is_host(lctx.inp_KQ_mask->buffer));

            float * data = (float *) lctx.inp_KQ_mask->data;

            for (int h = 0; h < 1; ++h) {
                for (int s1 = 0; s1 < n_seqs; ++s1) {
                    const llama_seq_id seq_id = ubatch.seq_id[s1][0];

                    for (int j = 0; j < n_seq_tokens; ++j) {
                        const int32_t tj = s1*n_seq_tokens + j;

                        for (int s0 = 0; s0 < n_seqs; ++s0) {
                            for (int i = 0; i < n_seq_tokens; ++i) {
                                const int32_t ti = s0*n_seq_tokens + i;
                                float f = -INFINITY;

                                for (int s = 0; s < ubatch.n_seq_id[s0]; ++s) {
                                    if (ubatch.seq_id[s0][s] == seq_id) {
                                        if (hparams.use_alibi) {
                                            f = -std::abs(ubatch.pos[ti] - ubatch.pos[tj]);
                                        } else {
                                            f = 0.0f;
                                        }
                                        break;
                                    }
                                }

                                data[h*(n_tokens*n_tokens) + tj*n_stride + ti] = f;
                            }
                        }

                        for (int i = n_tokens; i < n_stride; ++i) {
                            data[h*(n_tokens*n_tokens) + tj*n_stride + i] = -INFINITY;
                        }
                    }
                }
            }
        }
    }

    if (cparams.embeddings && cparams.pooling_type == LLAMA_POOLING_TYPE_MEAN) {
        const int64_t n_tokens     = ubatch.n_tokens;
        const int64_t n_seq_tokens = ubatch.n_seq_tokens;
        const int64_t n_seqs       = ubatch.n_seqs;

        LM_GGML_ASSERT(lctx.inp_mean);
        LM_GGML_ASSERT(lm_ggml_backend_buffer_is_host(lctx.inp_mean->buffer));

        float * data = (float *) lctx.inp_mean->data;
        memset(lctx.inp_mean->data, 0, n_tokens * n_tokens * lm_ggml_element_size(lctx.inp_mean));

        std::vector<uint64_t> sum(n_tokens, 0);

        for (int s = 0; s < n_seqs; ++s) {
            const llama_seq_id seq_id = ubatch.seq_id[s][0];

            // TODO: adapt limits to n_seqs when ubatch.equal_seqs is true
            LM_GGML_ASSERT(seq_id < n_tokens && "seq_id cannot be larger than n_tokens with pooling_type == MEAN");

            sum[seq_id] += ubatch.n_seq_tokens;
        }

        std::vector<float> div(n_tokens, 0.0f);
        for (int i = 0; i < n_tokens; ++i) {
            const uint64_t s = sum[i];
            if (s > 0) {
                div[i] = 1.0f/float(s);
            }
        }

        for (int s = 0; s < n_seqs; ++s) {
            const llama_seq_id seq_id = ubatch.seq_id[s][0];

            for (int i = 0; i < n_seq_tokens; ++i) {
                data[seq_id*n_tokens + s*n_seq_tokens + i] = div[seq_id];
            }
        }
    }

    if (cparams.embeddings && (
                cparams.pooling_type == LLAMA_POOLING_TYPE_CLS ||
                cparams.pooling_type == LLAMA_POOLING_TYPE_RANK)) {
        const int64_t n_tokens     = ubatch.n_tokens;
        const int64_t n_seq_tokens = ubatch.n_seq_tokens;
        const int64_t n_seqs       = ubatch.n_seqs;

        LM_GGML_ASSERT(lctx.inp_cls);
        LM_GGML_ASSERT(lm_ggml_backend_buffer_is_host(lctx.inp_cls->buffer));

        uint32_t * data = (uint32_t *) lctx.inp_cls->data;
        memset(lctx.inp_cls->data, 0, n_tokens * lm_ggml_element_size(lctx.inp_cls));

        for (int s = 0; s < n_seqs; ++s) {
            const llama_seq_id seq_id = ubatch.seq_id[s][0];

            // TODO: adapt limits to n_seqs when ubatch.equal_seqs is true
            LM_GGML_ASSERT(seq_id < n_tokens && "seq_id cannot be larger than n_tokens with pooling_type == CLS or RANK");

            for (int i = 0; i < n_seq_tokens; ++i) {
                const llama_pos pos = ubatch.pos[s*n_seq_tokens + i];

                if (pos == 0) {
                    data[seq_id] = s*n_seq_tokens + i;
                }
            }
        }
    }

    if (cparams.embeddings && cparams.pooling_type == LLAMA_POOLING_TYPE_LAST) {
        const int64_t n_tokens     = ubatch.n_tokens;
        const int64_t n_seq_tokens = ubatch.n_seq_tokens;
        const int64_t n_seqs       = ubatch.n_seqs;

        LM_GGML_ASSERT(lctx.inp_cls);
        LM_GGML_ASSERT(lm_ggml_backend_buffer_is_host(lctx.inp_cls->buffer));

        uint32_t * data = (uint32_t *) lctx.inp_cls->data;
        memset(lctx.inp_cls->data, 0, n_tokens * lm_ggml_element_size(lctx.inp_cls));

        std::vector<int> last_pos(n_tokens, -1);
        std::vector<int> last_row(n_tokens, -1);

        for (int s = 0; s < n_seqs; ++s) {
            const llama_seq_id seq_id = ubatch.seq_id[s][0];

            // TODO: adapt limits to n_seqs when ubatch.equal_seqs is true
            LM_GGML_ASSERT(seq_id < n_tokens && "seq_id cannot be larger than n_tokens with pooling_type == LAST");

            for (int i = 0; i < n_seq_tokens; ++i) {
                const llama_pos pos = ubatch.pos[s*n_seq_tokens + i];

                if (pos >= last_pos[seq_id]) {
                    last_pos[seq_id] = pos;
                    last_row[seq_id] = s*n_seq_tokens + i;
                }
            }
        }

        for (int i = 0; i < n_tokens; ++i) {
            if (last_row[i] >= 0) {
                data[i] = last_row[i];
            }
        }
    }

    if (kv_self.recurrent) {
        const int64_t n_kv = kv_self.n;

        if (lctx.inp_s_mask) {
            LM_GGML_ASSERT(lm_ggml_backend_buffer_is_host(lctx.inp_s_mask->buffer));
            float * data = (float *) lctx.inp_s_mask->data;

            // clear unused states
            for (int i = 0; i < n_kv; ++i) {
                const uint32_t  cell_id = i + kv_self.head;
                llama_kv_cell & kv_cell = lctx.kv_self.cells[cell_id];

                data[i] = (float) (kv_cell.src >= 0);

                // only clear once
                if (kv_cell.src < 0) {
                    kv_cell.src = cell_id;
                }
            }
        }

        if (lctx.inp_s_copy) {
            LM_GGML_ASSERT(lm_ggml_backend_buffer_is_host(lctx.inp_s_copy->buffer));
            int32_t * data = (int32_t *) lctx.inp_s_copy->data;

            // assuming copy destinations ALWAYS happen ONLY on the cells between head and head+n
            for (uint32_t i = 0; i < n_kv; ++i) {
                const uint32_t  cell_id = i + kv_self.head;
                llama_kv_cell & kv_cell = lctx.kv_self.cells[cell_id];

                // prevent out-of-bound sources
                if (kv_cell.src < 0 || (uint32_t) kv_cell.src >= kv_self.size) {
                    kv_cell.src = cell_id;
                }

                data[i] = kv_cell.src;

                // ensure copy only happens once
                if (kv_cell.src != (int32_t) cell_id) {
                    kv_cell.src = cell_id;
                }
            }
        }
    }

    if (lctx.inp_pos_bucket) {
        const int64_t n_tokens = ubatch.n_tokens;

        LM_GGML_ASSERT(lm_ggml_backend_buffer_is_host(lctx.inp_pos_bucket->buffer));
        LM_GGML_ASSERT(!ubatch.equal_seqs); // TODO: use ubatch.n_seqs instead of failing

        int32_t * data = (int32_t *) lctx.inp_pos_bucket->data;

        if (!lctx.is_encoding) {
            const int64_t n_kv = kv_self.n;
            for (int h = 0; h < 1; ++h) {
                for (int j = 0; j < n_tokens; ++j) {
                    for (int i = 0; i < n_kv; ++i) {
                        data[h*(n_kv*n_tokens) + j*n_kv + i] = llama_relative_position_bucket(lctx.kv_self.cells[i].pos, ubatch.pos[j], hparams.n_rel_attn_bkts, lctx.is_encoding);
                    }
                }
            }
        } else {
            for (int h = 0; h < 1; ++h) {
                for (int j = 0; j < n_tokens; ++j) {
                    for (int i = 0; i < n_tokens; ++i) {
                        data[h*(n_tokens*n_tokens) + j*n_tokens + i] = llama_relative_position_bucket(ubatch.pos[i], ubatch.pos[j], hparams.n_rel_attn_bkts, lctx.is_encoding);
                    }
                }
            }
        }
    }

    if (!lctx.is_encoding && lctx.inp_embd_enc) {
        assert(lctx.inp_embd_enc->type == LM_GGML_TYPE_F32);
        assert((size_t) lm_ggml_nelements(lctx.inp_embd_enc) == lctx.embd_enc.size());

        lm_ggml_backend_tensor_set(lctx.inp_embd_enc, lctx.embd_enc.data(), 0, lm_ggml_nbytes(lctx.inp_embd_enc));
    }

    if (!lctx.is_encoding && lctx.inp_KQ_mask_cross) {
        const int64_t n_output_enc = lctx.embd_enc.size() / hparams.n_embd;
        const int64_t n_tokens = ubatch.n_tokens;

        LM_GGML_ASSERT(lm_ggml_backend_buffer_is_host(lctx.inp_KQ_mask_cross->buffer));
        LM_GGML_ASSERT(!ubatch.equal_seqs); // TODO: use ubatch.n_seqs instead of failing

        float * data = (float *) lctx.inp_KQ_mask_cross->data;

        for (int h = 0; h < 1; ++h) {
            for (int j = 0; j < n_tokens; ++j) {
                for (int i = 0; i < n_output_enc; ++i) {
                    float f = -INFINITY;
                    for (int s = 0; s < ubatch.n_seq_id[j]; ++s) {
                        const llama_seq_id seq_id = ubatch.seq_id[j][s];
                        if (lctx.seq_ids_enc[i].find(seq_id) != lctx.seq_ids_enc[i].end()) {
                            f = 0.0f;
                        }
                    }
                    data[h*(n_output_enc*n_tokens) + j*n_output_enc + i] = f;
                }
            }

            for (int i = n_tokens; i < LM_GGML_PAD(n_tokens, LM_GGML_KQ_MASK_PAD); ++i) {
                for (int j = 0; j < n_output_enc; ++j) {
                    data[h*(n_output_enc*n_tokens) + i*n_output_enc + j] = -INFINITY;
                }
            }
        }
    }
}

// Make sure enough space is available for outputs.
// Returns max number of outputs for which space was reserved.
static size_t llama_output_reserve(llama_context & lctx, size_t n_outputs) {
    const auto & cparams = lctx.cparams;
    const auto & hparams = lctx.model.hparams;

    const size_t n_outputs_max = std::max(n_outputs, (size_t) cparams.n_seq_max);

    const auto n_batch = cparams.n_batch;
    const auto n_vocab = hparams.n_vocab;
    const auto n_embd  = hparams.n_embd;

    // TODO: use a per-batch flag for logits presence instead
    const bool has_logits = !cparams.embeddings;
    const bool has_embd   =  cparams.embeddings && (cparams.pooling_type == LLAMA_POOLING_TYPE_NONE);

    const size_t logits_size = has_logits ? n_vocab*n_outputs_max : 0;
    const size_t embd_size   = has_embd   ?  n_embd*n_outputs_max : 0;

    if (lctx.output_ids.empty()) {
        // init, never resized afterwards
        lctx.output_ids.resize(n_batch);
    }

    const size_t prev_size = lctx.buf_output ? lm_ggml_backend_buffer_get_size(lctx.buf_output.get()) : 0;
    const size_t new_size  = (logits_size + embd_size) * sizeof(float);

    // alloc only when more than the current capacity is required
    // TODO: also consider shrinking the buffer
    if (!lctx.buf_output || prev_size < new_size) {
        if (lctx.buf_output) {
#ifndef NDEBUG
            // This doesn't happen often, but may be annoying in some cases (like the HellaSwag benchmark)
            LLAMA_LOG_INFO("%s: reallocating output buffer from size %.02f MiB to %.02f MiB\n", __func__, prev_size / 1024.0 / 1024.0, new_size / 1024.0 / 1024.0);
#endif
            lctx.buf_output = nullptr;
            lctx.logits = nullptr;
            lctx.embd = nullptr;
        }

        auto * buft = lm_ggml_backend_cpu_buffer_type();
        // try to use the host buffer of the device where the output tensor is allocated for faster transfer to system memory
        auto * output_dev = lctx.model.dev_output.dev;
        auto * output_dev_host_buft = output_dev ? lm_ggml_backend_dev_host_buffer_type(output_dev) : nullptr;
        if (output_dev_host_buft) {
            buft = output_dev_host_buft;
        }
        lctx.buf_output.reset(lm_ggml_backend_buft_alloc_buffer(buft, new_size));
        if (lctx.buf_output == nullptr) {
            LLAMA_LOG_ERROR("%s: failed to allocate output buffer of size %.2f MiB\n", __func__, new_size / (1024.0 * 1024.0));
            return 0;
        }
    }

    float * output_base = (float *) lm_ggml_backend_buffer_get_base(lctx.buf_output.get());

    lctx.logits = has_logits ? output_base               : nullptr;
    lctx.embd   = has_embd   ? output_base + logits_size : nullptr;

    lctx.output_size = n_outputs_max;
    lctx.logits_size = logits_size;
    lctx.embd_size   = embd_size;

    // set all ids as invalid (negative)
    std::fill(lctx.output_ids.begin(), lctx.output_ids.end(), -1);

    lm_ggml_backend_buffer_clear(lctx.buf_output.get(), 0);

    lctx.n_outputs = 0;

    return n_outputs_max;
}

// make the outputs have the same order they had in the user-provided batch
static void llama_output_reorder(struct llama_context * ctx) {
    std::vector<size_t> & out_ids = ctx->sbatch.out_ids;
    if (!out_ids.empty()) {
        uint32_t n_vocab = ctx->model.hparams.n_vocab;
        uint32_t n_embd  = ctx->model.hparams.n_embd;
        int32_t n_outputs = ctx->n_outputs;
        LM_GGML_ASSERT((size_t) n_outputs == out_ids.size());
        // TODO: is there something more efficient which also minimizes swaps?
        // selection sort, to minimize swaps (from https://en.wikipedia.org/wiki/Selection_sort)
        for (int32_t i = 0; i < n_outputs - 1; ++i) {
            int32_t j_min = i;
            for (int32_t j = i + 1; j < n_outputs; ++j) {
                if (out_ids[j] < out_ids[j_min]) {
                    j_min = j;
                }
            }
            if (j_min == i) { continue; }
            std::swap(out_ids[i], out_ids[j_min]);
            if (ctx->logits_size > 0) {
                for (uint32_t k = 0; k < n_vocab; k++) {
                    std::swap(ctx->logits[i*n_vocab + k], ctx->logits[j_min*n_vocab + k]);
                }
            }
            if (ctx->embd_size > 0) {
                for (uint32_t k = 0; k < n_embd; k++) {
                    std::swap(ctx->embd[i*n_embd + k], ctx->embd[j_min*n_embd + k]);
                }
            }
        }
        std::fill(ctx->output_ids.begin(), ctx->output_ids.end(), -1);
        for (int32_t i = 0; i < n_outputs; ++i) {
            ctx->output_ids[out_ids[i]] = i;
        }
        out_ids.clear();
    }
}

// returns the result of lm_ggml_backend_sched_graph_compute_async execution
static enum lm_ggml_status llama_graph_compute(
          llama_context & lctx,
            lm_ggml_cgraph * gf,
                    int   n_threads,
        lm_ggml_threadpool * threadpool) {
    if (lctx.backend_cpu != nullptr) {
        lm_ggml_backend_cpu_set_threadpool(lctx.backend_cpu, threadpool);
        lm_ggml_backend_cpu_set_abort_callback(lctx.backend_cpu, lctx.abort_callback, lctx.abort_callback_data);
    }

    // set the number of threads for all the backends
    for (const auto & set_n_threads_fn : lctx.set_n_threads_fns) {
        set_n_threads_fn.second(set_n_threads_fn.first, n_threads);
    }

    auto status = lm_ggml_backend_sched_graph_compute_async(lctx.sched.get(), gf);
    if (status != LM_GGML_STATUS_SUCCESS) {
        LLAMA_LOG_ERROR("%s: lm_ggml_backend_sched_graph_compute_async failed with error %d\n", __func__, status);
    }

    // fprintf(stderr, "splits: %d\n", lm_ggml_backend_sched_get_n_splits(lctx.sched));

    return status;
}

// decode a batch of tokens by evaluating the transformer
// in case of unsuccessful decoding (error or warning),
// the kv_cache state will be returned to its original state
// (for non-recurrent models) or cleaned (for recurrent models)
//
//   - lctx:      llama context
//   - batch:     batch to evaluate
//
// return 0 on success
// return positive int on warning
// return negative int on error
//
static int llama_decode_internal(
         llama_context & lctx,
           llama_batch   inp_batch) {

    lctx.is_encoding = false;

    if (inp_batch.n_tokens == 0) {
        LLAMA_LOG_ERROR("%s: n_tokens == 0\n", __func__);
        return -1;
    }

    // temporary allocate memory for the input batch if needed
    llama_batch_allocr batch_allocr(lctx, inp_batch);
    const llama_batch & batch = batch_allocr.batch;
    const uint32_t n_tokens_all = batch.n_tokens;

    const auto & model   = lctx.model;
    const auto & hparams = model.hparams;
    const auto & cparams = lctx.cparams;

    LM_GGML_ASSERT((!batch.token && batch.embd) || (batch.token && !batch.embd)); // NOLINT

    if (batch.token) {
        for (uint32_t i = 0; i < n_tokens_all; ++i) {
            if (batch.token[i] < 0 || (uint32_t)batch.token[i] >= model.vocab.n_vocab) {
                LLAMA_LOG_ERROR("%s: invalid token[%d] = %d\n", __func__, i, batch.token[i]);
                return -1;
            }
        }
    }

    LM_GGML_ASSERT(n_tokens_all <= cparams.n_batch);

    LM_GGML_ASSERT((cparams.causal_attn || cparams.n_ubatch >= n_tokens_all) && "non-causal attention requires n_ubatch >= n_tokens");

    if (lctx.t_compute_start_us == 0) {
        lctx.t_compute_start_us = lm_ggml_time_us();
    }
    lctx.n_queued_tokens += n_tokens_all;

    auto & kv_self = lctx.kv_self;
    llama_kv_slot_restorer kv_slot_restorer(kv_self);

    const int64_t n_embd  = hparams.n_embd;
    const int64_t n_vocab = hparams.n_vocab;

    uint32_t n_outputs = 0;
    uint32_t n_outputs_prev = 0;

    const auto n_ubatch = cparams.n_ubatch;

    // this indicates we are doing pooled embedding, so we ignore batch.logits and output all tokens
    const bool embd_pooled = cparams.embeddings && cparams.pooling_type != LLAMA_POOLING_TYPE_NONE;

    lctx.embd_seq.clear();

    // count outputs
    if (batch.logits && !embd_pooled) {
        for (uint32_t i = 0; i < n_tokens_all; ++i) {
            n_outputs += batch.logits[i] != 0;
        }
    } else if (lctx.logits_all || embd_pooled) {
        n_outputs = n_tokens_all;
    } else {
        // keep last output only
        n_outputs = 1;
    }

    lctx.sbatch.from_batch(batch, n_embd,
        /* simple_split */ !kv_self.recurrent,
        /* logits_all   */ n_outputs == n_tokens_all);

    // reserve output buffer
    if (llama_output_reserve(lctx, n_outputs) < n_outputs) {
        LLAMA_LOG_ERROR("%s: could not reserve space for batch with %u outputs\n", __func__, n_outputs);
        return -2;
    };

    while (lctx.sbatch.n_tokens > 0) {
        llama_ubatch ubatch;
        if (kv_self.recurrent) {
            if (embd_pooled) {
                // Pooled embeddings cannot be split across ubatches (yet)
                ubatch = lctx.sbatch.split_seq(n_ubatch);
            } else {
                // recurrent model architectures are easier to implement
                // with equal-length sequences
                ubatch = lctx.sbatch.split_equal(n_ubatch);
            }
        } else {
            ubatch = lctx.sbatch.split_simple(n_ubatch);
        }
        const uint32_t n_tokens = ubatch.n_tokens;

        // count the outputs in this u_batch
        {
            int32_t n_outputs_new = 0;

            if (n_outputs == n_tokens_all) {
                n_outputs_new = n_tokens;
            } else {
                LM_GGML_ASSERT(ubatch.output);
                for (uint32_t i = 0; i < n_tokens; i++) {
                    n_outputs_new += (int32_t) (ubatch.output[i] != 0);
                }
            }

            // needs to happen before the graph is built
            lctx.n_outputs = n_outputs_new;
        }

        int n_threads = n_tokens == 1 ? cparams.n_threads : cparams.n_threads_batch;
        lm_ggml_threadpool_t threadpool = n_tokens == 1 ? lctx.threadpool : lctx.threadpool_batch;

        LM_GGML_ASSERT(n_threads > 0);

        // non-causal masks do not use the KV cache
        if (hparams.causal_attn) {
            llama_kv_cache_update(&lctx);

            // if we have enough unused cells before the current head ->
            //   better to start searching from the beginning of the cache, hoping to fill it
            if (kv_self.head > kv_self.used + 2*n_tokens) {
                kv_self.head = 0;
            }

            const auto slot = llama_kv_cache_find_slot(kv_self, ubatch);
            if (!slot) {
                return 1;
            }
            kv_slot_restorer.save(slot);

            if (!kv_self.recurrent) {
                // a heuristic, to avoid attending the full cache if it is not yet utilized
                // after enough generations, the benefit from this heuristic disappears
                // if we start defragmenting the cache, the benefit from this will be more important
                const uint32_t pad = llama_kv_cache_get_padding(cparams);
                kv_self.n = std::min(kv_self.size, std::max(pad, LM_GGML_PAD(llama_kv_cache_cell_max(kv_self), pad)));
                //kv_self.n = llama_kv_cache_cell_max(kv_self);
            }
        }

        //printf("kv_self.n = %5d, kv_self.used = %5d, kv_self.head = %5d\n", kv_self.n, kv_self.used, kv_self.head);

        lm_ggml_backend_sched_reset(lctx.sched.get());
        lm_ggml_backend_sched_set_eval_callback(lctx.sched.get(), lctx.cparams.cb_eval, lctx.cparams.cb_eval_user_data);

        lm_ggml_cgraph * gf = llama_build_graph(lctx, ubatch, false);

        // the output is always the last tensor in the graph
        struct lm_ggml_tensor * res  = lm_ggml_graph_node(gf, -1);
        struct lm_ggml_tensor * embd = lm_ggml_graph_node(gf, -2);

        if (lctx.n_outputs == 0) {
            // no output
            res  = nullptr;
            embd = nullptr;
        } else if (cparams.embeddings) {
            res  = nullptr; // do not extract logits for embedding case
            embd = nullptr;
            for (int i = lm_ggml_graph_n_nodes(gf) - 1; i >= 0; --i) {
                if (strcmp(lm_ggml_graph_node(gf, i)->name, "result_embd_pooled") == 0) {
                    embd = lm_ggml_graph_node(gf, i);
                    break;
                }
            }
            LM_GGML_ASSERT(embd != nullptr && "missing embeddings tensor");
        } else {
            embd = nullptr; // do not extract embeddings when not needed
            LM_GGML_ASSERT(strcmp(res->name, "result_output") == 0 && "missing result_output tensor");
        }
        // LLAMA_LOG_INFO("graph build time: %.3f ms (%d nodes, %d leafs)\n", (lm_ggml_time_us() - t_start_us)/1000.0, gf->n_nodes, gf->n_leafs);

        lm_ggml_backend_sched_alloc_graph(lctx.sched.get(), gf);

        llama_set_inputs(lctx, ubatch);

        const auto compute_status = llama_graph_compute(lctx, gf, n_threads, threadpool);
        if (compute_status != LM_GGML_STATUS_SUCCESS) {
            kv_slot_restorer.restore(kv_self);
            switch (compute_status) {
                case LM_GGML_STATUS_ABORTED:
                    return 2;
                case LM_GGML_STATUS_ALLOC_FAILED:
                    return -2;
                case LM_GGML_STATUS_FAILED:
                default:
                    return -3;
            }
        }

        // update the kv ring buffer
        {
            kv_self.head += n_tokens;

            // Ensure kv cache head points to a valid index.
            if (kv_self.head >= kv_self.size) {
                kv_self.head = 0;
            }
        }

        // plot the computation graph in dot format (for debugging purposes)
        //if (n_past%100 == 0) {
        //    lm_ggml_graph_dump_dot(gf, NULL, "llama.dot");
        //}

        // extract logits
        if (res) {
            lm_ggml_backend_t backend_res = lm_ggml_backend_sched_get_tensor_backend(lctx.sched.get(), res);
            LM_GGML_ASSERT(backend_res != nullptr);
            LM_GGML_ASSERT(lctx.logits != nullptr);

            float * logits_out = lctx.logits + n_outputs_prev*n_vocab;
            const int32_t n_outputs_new = lctx.n_outputs;

            if (n_outputs_new) {
                LM_GGML_ASSERT( n_outputs_prev + n_outputs_new <= n_outputs);
                LM_GGML_ASSERT((n_outputs_prev + n_outputs_new)*n_vocab <= (int64_t) lctx.logits_size);
                lm_ggml_backend_tensor_get_async(backend_res, res, logits_out, 0, n_outputs_new*n_vocab*sizeof(float));
            }
        }

        // extract embeddings
        if (embd) {
            lm_ggml_backend_t backend_embd = lm_ggml_backend_sched_get_tensor_backend(lctx.sched.get(), embd);
            LM_GGML_ASSERT(backend_embd != nullptr);

            switch (cparams.pooling_type) {
                case LLAMA_POOLING_TYPE_NONE:
                    {
                        // extract token embeddings
                        LM_GGML_ASSERT(lctx.embd != nullptr);
                        float * embd_out = lctx.embd + n_outputs_prev*n_embd;
                        const int32_t n_outputs_new = lctx.n_outputs;

                        if (n_outputs_new) {
                            LM_GGML_ASSERT( n_outputs_prev + n_outputs_new <= n_outputs);
                            LM_GGML_ASSERT((n_outputs_prev + n_outputs_new)*n_embd <= (int64_t) lctx.embd_size);
                            lm_ggml_backend_tensor_get_async(backend_embd, embd, embd_out, 0, n_outputs_new*n_embd*sizeof(float));
                        }
                    } break;
                case LLAMA_POOLING_TYPE_MEAN:
                case LLAMA_POOLING_TYPE_CLS:
                case LLAMA_POOLING_TYPE_LAST:
                    {
                        // extract sequence embeddings (cleared before processing each batch)
                        auto & embd_seq_out = lctx.embd_seq;

                        for (uint32_t s = 0; s < ubatch.n_seqs; ++s) {
                            const llama_seq_id seq_id = ubatch.seq_id[s][0];
                            if (embd_seq_out.find(seq_id) != embd_seq_out.end()) {
                                continue;
                            }
                            embd_seq_out[seq_id].resize(n_embd);
                            lm_ggml_backend_tensor_get_async(backend_embd, embd, embd_seq_out[seq_id].data(), (n_embd*seq_id)*sizeof(float), n_embd*sizeof(float));
                        }
                    } break;
                case LLAMA_POOLING_TYPE_RANK:
                    {
                        // extract the rerank score - a single float per sequence
                        auto & embd_seq_out = lctx.embd_seq;

                        for (uint32_t s = 0; s < ubatch.n_seqs; ++s) {
                            const llama_seq_id seq_id = ubatch.seq_id[s][0];
                            if (embd_seq_out.find(seq_id) != embd_seq_out.end()) {
                                continue;
                            }
                            embd_seq_out[seq_id].resize(1);
                            lm_ggml_backend_tensor_get_async(backend_embd, embd, embd_seq_out[seq_id].data(), (seq_id)*sizeof(float), sizeof(float));
                        }
                    } break;
                case LLAMA_POOLING_TYPE_UNSPECIFIED:
                    {
                        LM_GGML_ABORT("unknown pooling type");
                    }
            }
        }
        n_outputs_prev += lctx.n_outputs;
    }

    // set output mappings
    {
        bool sorted_output = true;

        LM_GGML_ASSERT(lctx.sbatch.out_ids.size() == n_outputs);

        for (size_t i = 0; i < n_outputs; ++i) {
            size_t out_id = lctx.sbatch.out_ids[i];
            lctx.output_ids[out_id] = i;
            if (out_id != i) {
                sorted_output = false;
            }
        }

        if (sorted_output) {
            lctx.sbatch.out_ids.clear();
        }
    }

    // set to total number of outputs in the batch, for use in llama_get_logits_ith
    lctx.n_outputs = n_outputs;

    // wait for the computation to finish (automatically done when obtaining the model output)
    //llama_synchronize(&lctx);

    // decide if we need to defrag the kv cache
    if (cparams.causal_attn && cparams.defrag_thold >= 0.0f) {
        const float fragmentation = kv_self.n >= 128 ? 1.0f - float(kv_self.used)/float(kv_self.n) : 0.0f;

        // queue defragmentation for next llama_kv_cache_update
        if (fragmentation > cparams.defrag_thold) {
            //LLAMA_LOG_INFO("fragmentation: %.2f\n", fragmentation);

            llama_kv_cache_defrag(kv_self);
        }
    }

    // Reset state for the next token before backend sync, to allow the CPU activities in the reset to
    // overlap with device computation.
    lm_ggml_backend_sched_reset(lctx.sched.get());

    return 0;
}

// encode a batch of tokens by evaluating the encoder part of the transformer
//
//   - lctx:      llama context
//   - batch:     batch to evaluate
//
// return 0 on success
// return positive int on warning
// return negative int on error
//
static int llama_encode_internal(
         llama_context & lctx,
           llama_batch   inp_batch) {

    lctx.is_encoding = true;

    if (inp_batch.n_tokens == 0) {
        LLAMA_LOG_ERROR("%s: n_tokens == 0\n", __func__);
        return -1;
    }

    // temporary allocate memory for the input batch if needed
    llama_batch_allocr batch_allocr(lctx, inp_batch);
    const llama_batch & batch = batch_allocr.batch;
    const uint32_t n_tokens = batch.n_tokens;

    const auto & model   = lctx.model;
    const auto & hparams = model.hparams;
    const auto & cparams = lctx.cparams;

    LM_GGML_ASSERT((!batch.token && batch.embd) || (batch.token && !batch.embd)); // NOLINT

    if (batch.token) {
        for (uint32_t i = 0; i < n_tokens; ++i) {
            if (batch.token[i] < 0 || (uint32_t)batch.token[i] >= model.vocab.n_vocab) {
                LLAMA_LOG_ERROR("%s: invalid token[%d] = %d\n", __func__, i, batch.token[i]);
                return -1;
            }
        }
    }

    // micro-batching is not possible for non-causal encoding, so we process the batch in a single shot
    LM_GGML_ASSERT(cparams.n_ubatch >= n_tokens && "encoder requires n_ubatch >= n_tokens");

    if (lctx.t_compute_start_us == 0) {
        lctx.t_compute_start_us = lm_ggml_time_us();
    }

    lctx.n_queued_tokens += n_tokens;

    const int64_t n_embd = hparams.n_embd;

    lctx.sbatch.from_batch(batch, n_embd, /* simple_split */ true, /* logits_all */ true);

    const llama_ubatch ubatch = lctx.sbatch.split_simple(n_tokens);

    // reserve output buffer
    if (llama_output_reserve(lctx, n_tokens) < n_tokens) {
        LLAMA_LOG_ERROR("%s: could not reserve space for batch with %u outputs\n", __func__, n_tokens);
        return -2;
    };

    for (uint32_t i = 0; i < n_tokens; ++i) {
        lctx.output_ids[i] = i;
    }

    lctx.inp_embd_enc = NULL;
    lctx.n_outputs = n_tokens;

    int n_threads = n_tokens == 1 ? cparams.n_threads : cparams.n_threads_batch;
    lm_ggml_threadpool_t threadpool = n_tokens == 1 ? lctx.threadpool : lctx.threadpool_batch;

    LM_GGML_ASSERT(n_threads > 0);

    lm_ggml_backend_sched_reset(lctx.sched.get());
    lm_ggml_backend_sched_set_eval_callback(lctx.sched.get(), lctx.cparams.cb_eval, lctx.cparams.cb_eval_user_data);

    lm_ggml_cgraph * gf = llama_build_graph(lctx, ubatch, false);

    // the output embeddings after the final encoder normalization
    struct lm_ggml_tensor * embd = nullptr;

    // there are two cases here
    if (llama_model_has_decoder(&lctx.model)) {
        // first case is an encoder-decoder T5 model where embeddings are passed to decoder
        embd = lm_ggml_graph_node(gf, -1);
        LM_GGML_ASSERT(strcmp(embd->name, "result_norm") == 0 && "missing result_output tensor");
    } else {
        // second case is an encoder-only T5 model
        if (cparams.embeddings) {
            // only output embeddings if required
            embd = lm_ggml_graph_node(gf, -1);
            if (strcmp(embd->name, "result_embd_pooled") != 0) {
                embd = lm_ggml_graph_node(gf, -2);
            }
            LM_GGML_ASSERT(strcmp(embd->name, "result_embd_pooled") == 0 && "missing embeddings tensor");
        }
    }

    lm_ggml_backend_sched_alloc_graph(lctx.sched.get(), gf);

    llama_set_inputs(lctx, ubatch);

    const auto compute_status = llama_graph_compute(lctx, gf, n_threads, threadpool);
    switch (compute_status) {
        case LM_GGML_STATUS_SUCCESS:
            break;
        case LM_GGML_STATUS_ABORTED:
            return 2;
        case LM_GGML_STATUS_ALLOC_FAILED:
            return -2;
        case LM_GGML_STATUS_FAILED:
        default:
            return -3;
    }

    // extract embeddings
    if (embd) {
        lm_ggml_backend_t backend_embd = lm_ggml_backend_sched_get_tensor_backend(lctx.sched.get(), embd);
        LM_GGML_ASSERT(backend_embd != nullptr);

        if (llama_model_has_decoder(&lctx.model)) {
            lctx.embd_enc.resize(n_tokens*n_embd);
            float * embd_out = lctx.embd_enc.data();

            lm_ggml_backend_tensor_get_async(backend_embd, embd, embd_out, 0, n_tokens*n_embd*sizeof(float));
            LM_GGML_ASSERT(!ubatch.equal_seqs); // TODO: handle equal splits

            // remember the sequence ids used during the encoding - needed for cross attention later
            lctx.seq_ids_enc.resize(n_tokens);
            for (uint32_t i = 0; i < n_tokens; i++) {
                for (int s = 0; s < ubatch.n_seq_id[i]; s++) {
                    llama_seq_id seq_id = ubatch.seq_id[i][s];
                    lctx.seq_ids_enc[i].insert(seq_id);
                }
            }
        } else {
            LM_GGML_ASSERT(lctx.embd != nullptr);

            switch (cparams.pooling_type) {
                case LLAMA_POOLING_TYPE_NONE:
                    {
                        // extract token embeddings
                        LM_GGML_ASSERT(lctx.embd != nullptr);
                        float * embd_out = lctx.embd;

                        LM_GGML_ASSERT(n_tokens*n_embd <= (int64_t) lctx.embd_size);
                        lm_ggml_backend_tensor_get_async(backend_embd, embd, embd_out, 0, n_tokens*n_embd*sizeof(float));
                    } break;
                case LLAMA_POOLING_TYPE_MEAN:
                case LLAMA_POOLING_TYPE_CLS:
                case LLAMA_POOLING_TYPE_LAST:
                    {
                        // extract sequence embeddings
                        auto & embd_seq_out = lctx.embd_seq;
                        embd_seq_out.clear();

                        LM_GGML_ASSERT(!ubatch.equal_seqs); // TODO: handle equal splits

                        for (uint32_t i = 0; i < n_tokens; i++) {
                            const llama_seq_id seq_id = ubatch.seq_id[i][0];
                            if (embd_seq_out.find(seq_id) != embd_seq_out.end()) {
                                continue;
                            }
                            embd_seq_out[seq_id].resize(n_embd);
                            lm_ggml_backend_tensor_get_async(backend_embd, embd, embd_seq_out[seq_id].data(), (n_embd*seq_id)*sizeof(float), n_embd*sizeof(float));
                        }
                    } break;
                case LLAMA_POOLING_TYPE_RANK:
                    {
                        // TODO: this likely should be the same logic as in llama_decoder_internal, but better to
                        //       wait for an encoder model that requires this pooling type in order to test it
                        //       https://github.com/ggerganov/llama.cpp/pull/9510
                        LM_GGML_ABORT("RANK pooling not implemented yet");
                    }
                case LLAMA_POOLING_TYPE_UNSPECIFIED:
                    {
                        LM_GGML_ABORT("unknown pooling type");
                    }
            }
        }
    }

    // Reset state for the next token before backend sync, to allow the CPU activities in the reset to
    // overlap with device computation.
    lm_ggml_backend_sched_reset(lctx.sched.get());

    return 0;
}

// find holes from the beginning of the KV cache and fill them by moving data from the end of the cache
static void llama_kv_cache_defrag_internal(struct llama_context & lctx) {
    auto & kv_self = lctx.kv_self;

    const auto & hparams = lctx.model.hparams;

    const uint32_t n_layer = hparams.n_layer;

    const uint32_t n_kv   = llama_kv_cache_cell_max(kv_self);
    const uint32_t n_used = kv_self.used;

    assert(n_used <= n_kv);

    //const int64_t t_start = lm_ggml_time_us();

    // number of cells moved
    uint32_t n_moves = 0;

    // each move requires 6*n_layer tensors (see build_defrag)
    //   - source view, destination view, copy operation
    //   - x2 for keys and values
    //const uint32_t max_moves = llama_model_max_nodes(model)/(6*n_layer);
    // TODO: tmp fix https://github.com/ggerganov/llama.cpp/issues/6685#issuecomment-2057579516
    const uint32_t max_moves = (llama_model_max_nodes(lctx.model) - 2*n_layer)/(6*n_layer);

    // determine which KV cells to move where
    //
    //  cell i moves to ids[i]
    //
    //  if ids[i] == i || ids[i] == n_kv, then cell i is not moved
    //
    std::vector<uint32_t> ids(n_kv, n_kv);

    for (uint32_t i0 = 0; i0 < n_used; ++i0) {
        const auto & cell0 = kv_self.cells[i0];

        if (!cell0.is_empty()) {
            ids[i0] = i0;

            continue;
        }

        // found a hole - fill it with data from the end of the cache

        uint32_t nh = 1;

        // determine the size of the hole
        while (i0 + nh < n_used && kv_self.cells[i0 + nh].is_empty()) {
            nh++;
        }

        uint32_t nf = 0;
        uint32_t is = n_kv - 1;

        // starting from the end, find nh non-empty cells
        for (; is > i0; --is) {
            const auto & cell1 = kv_self.cells[is];

            if (cell1.is_empty() || ids[is] != n_kv) {
                continue;
            }

            // non-empty cell which is not yet moved
            nf++;

            if (nf == nh) {
                break;
            }
        }

        // this can only happen if `n_used` is not accurate, which would be a bug
        LM_GGML_ASSERT(nf == nh && "KV defrag bug: nf != nh");

        nf = 0;

        uint32_t i1 = is;

        // are we moving a continuous block of memory?
        bool cont = false;

        // should we stop searching for the next move?
        bool stop = false;

        // go back and move the nf cells to the hole
        for (; i1 < n_kv; ++i1) {
            auto & cell1 = kv_self.cells[i1];

            if (cell1.is_empty() || ids[i1] != n_kv) {
                if (n_moves == max_moves) {
                    stop = true;
                    break;
                }

                cont = false;
                continue;
            }

            // this cell goes to (i0 + nf)
            ids[i1] = i0 + nf;

            // move the cell meta data
            kv_self.cells[i0 + nf] = cell1;

            // clear the old cell and move the head there
            cell1 = llama_kv_cell();
            kv_self.head = n_used;

            if (!cont) {
                n_moves++;
                cont = true;
            }

            nf++;

            if (nf == nh) {
                break;
            }
        }

        if (stop || n_moves == max_moves) {
            break;
        }

        //LLAMA_LOG_INFO("(tmp log) KV defrag: move [%u, %u) to [%u, %u)\n", is, i1 + 1, i0, i0 + nh);

        i0 += nh - 1;
    }

    if (n_moves == 0) {
        return;
    }

    //LLAMA_LOG_INFO("(tmp log) KV defrag cell moves: %u\n", n_moves);

    //LLAMA_LOG_INFO("expected gf nodes: %u\n", 6*n_moves*n_layer);

#if 0
    // CPU defrag
    //
    // TODO: optimizations are possible:
    //       - multiple threads
    //       - avoid copying to the host memory when already there
    //
    // likely not worth the effort, as we have lm_ggml_graph based defrag
    //

    const uint32_t n_embd_k_gqa = hparams.n_embd_k_gqa();
    const uint32_t n_embd_v_gqa = hparams.n_embd_v_gqa();

    const uint32_t kv_size = kv_self.size;

    std::vector<uint8_t> buf_k;
    std::vector<uint8_t> buf_v;

    for (uint32_t il = 0; il < n_layer; ++il) {
        const size_t k_size_row = lm_ggml_row_size(kv_self.k_l[il]->type, n_embd_k_gqa);
        const size_t k_size     = lm_ggml_row_size(kv_self.k_l[il]->type, n_embd_k_gqa*kv_size);

        const size_t v_size_el = lm_ggml_type_size(kv_self.v_l[il]->type);
        const size_t v_size    = lm_ggml_row_size (kv_self.v_l[il]->type, n_embd_v_gqa*kv_size);

        buf_k.resize(k_size);
        buf_v.resize(v_size);

        lm_ggml_backend_tensor_get(kv_self.k_l[il], buf_k.data(), 0, buf_k.size());
        lm_ggml_backend_tensor_get(kv_self.v_l[il], buf_v.data(), 0, buf_v.size());

        // batch move [i, i+nm) to [id, id+nm)
        // note: cells can move only to a lower index
        for (uint32_t i = 0; i < n_kv; ++i) {
            const uint32_t id = ids[i];

            if (i == id || id == n_kv) {
                continue;
            }

            uint32_t nm = 1;

            while (i + nm < n_kv && ids[i + nm] == id + nm) {
                nm++;
            }

            // move keys
            {
                const int64_t os =  i*k_size_row;
                const int64_t od = id*k_size_row;

                memcpy(buf_k.data() + od, buf_k.data() + os, nm*k_size_row);
            }

            // move values (note: they are transposed)
            {
                const int64_t os =  i;
                const int64_t od = id;

                for (uint32_t j = 0; j < n_embd_v_gqa; ++j) {
                    memcpy(buf_v.data() + (od + j*kv_size)*v_size_el, buf_v.data() + (os + j*kv_size)*v_size_el, nm*v_size_el);
                }
            }

            i += nm - 1;
        }

        lm_ggml_backend_tensor_set(kv_self.k_l[il], buf_k.data(), 0, buf_k.size());
        lm_ggml_backend_tensor_set(kv_self.v_l[il], buf_v.data(), 0, buf_v.size());
    }
#else
    // lm_ggml_graph defrag

    lm_ggml_backend_sched_reset(lctx.sched.get());

    lm_ggml_cgraph * gf = llama_build_graph_defrag(lctx, ids);

    llama_graph_compute(lctx, gf, lctx.cparams.n_threads, lctx.threadpool);
#endif

    //const int64_t t_end = lm_ggml_time_us();

    //LLAMA_LOG_INFO("(tmp log) KV defrag time: %.3f ms\n", (t_end - t_start)/1000.0);
}

static void llama_kv_cache_update_internal(struct llama_context & lctx) {
    bool need_reserve = false;

    if (lctx.kv_self.has_shift) {
        if (!llama_kv_cache_can_shift(&lctx)) {
            LM_GGML_ABORT("The current context does not support K-shift");
        }

        // apply K-shift if needed
        if (lctx.model.hparams.rope_type != LLAMA_ROPE_TYPE_NONE) {
            lm_ggml_backend_sched_reset(lctx.sched.get());

            lm_ggml_cgraph * gf = llama_build_graph_k_shift(lctx);

            lm_ggml_backend_sched_alloc_graph(lctx.sched.get(), gf);

            llama_set_k_shift(lctx);

            llama_graph_compute(lctx, gf, lctx.cparams.n_threads, lctx.threadpool);

            need_reserve = true;
        }

        {
            auto & kv_self = lctx.kv_self;

            kv_self.has_shift = false;

            for (uint32_t i = 0; i < kv_self.size; ++i) {
                kv_self.cells[i].delta = 0;
            }
        }
    }

    // defragment the KV cache if needed
    if (lctx.kv_self.do_defrag) {
        llama_kv_cache_defrag_internal(lctx);

        need_reserve = true;

        lctx.kv_self.do_defrag = false;
    }

    // reserve a worst case graph again
    if (need_reserve) {
        // TODO: extract to a function
        // build worst-case graph
        uint32_t n_seqs = 1; // TODO: worst-case number of sequences
        uint32_t n_tokens = std::min(lctx.cparams.n_ctx, lctx.cparams.n_ubatch);
        llama_token token = llama_token_bos(&lctx.model); // not actually used by llama_build_graph, but required to choose between token and embedding inputs graph
        llama_ubatch ubatch = { true, n_tokens, n_tokens / n_seqs, n_seqs, &token, nullptr, nullptr, nullptr, nullptr, nullptr};
        lm_ggml_cgraph * gf = llama_build_graph(lctx, ubatch, true);

        // initialize scheduler with the worst-case graph
        lm_ggml_backend_sched_reset(lctx.sched.get());
        if (!lm_ggml_backend_sched_reserve(lctx.sched.get(), gf)) {
            LLAMA_LOG_ERROR("%s: failed to allocate compute buffers\n", __func__);
        }
    }
}

//
// quantization
//

struct quantize_state_internal {
    const llama_model                 & model;
    const llama_model_quantize_params * params;

    int n_attention_wv    = 0;
    int n_ffn_down        = 0;
    int n_ffn_gate        = 0;
    int n_ffn_up          = 0;
    int i_attention_wv    = 0;
    int i_ffn_down        = 0;
    int i_ffn_gate        = 0;
    int i_ffn_up          = 0;

    int n_k_quantized     = 0;
    int n_fallback        = 0;

    bool has_imatrix      = false;

    // used to figure out if a model shares tok_embd with the output weight
    bool has_output       = false;

    quantize_state_internal(const llama_model & model, const llama_model_quantize_params * params)
        : model(model)
        , params(params)
        {}
};

static void llama_tensor_dequantize_internal(
    struct lm_ggml_tensor * tensor, std::vector<no_init<float>> & output, std::vector<std::thread> & workers,
    const size_t nelements, const int nthread
) {
    if (output.size() < nelements) {
        output.resize(nelements);
    }
    float * f32_output = (float *) output.data();

    const lm_ggml_type_traits * qtype = lm_ggml_get_type_traits(tensor->type);
    if (lm_ggml_is_quantized(tensor->type)) {
        if (qtype->to_float == NULL) {
            throw std::runtime_error(format("type %s unsupported for integer quantization: no dequantization available", lm_ggml_type_name(tensor->type)));
        }
    } else if (tensor->type != LM_GGML_TYPE_F16 &&
               tensor->type != LM_GGML_TYPE_BF16) {
        throw std::runtime_error(format("cannot dequantize/convert tensor type %s", lm_ggml_type_name(tensor->type)));
    }

    if (nthread < 2) {
        if (tensor->type == LM_GGML_TYPE_F16) {
            lm_ggml_fp16_to_fp32_row((lm_ggml_fp16_t *)tensor->data, f32_output, nelements);
        } else if (tensor->type == LM_GGML_TYPE_BF16) {
            lm_ggml_bf16_to_fp32_row((lm_ggml_bf16_t *)tensor->data, f32_output, nelements);
        } else if (lm_ggml_is_quantized(tensor->type)) {
            qtype->to_float(tensor->data, f32_output, nelements);
        } else {
            LM_GGML_ABORT("fatal error"); // unreachable
        }
        return;
    }

    size_t block_size;
    if (tensor->type == LM_GGML_TYPE_F16 ||
        tensor->type == LM_GGML_TYPE_BF16) {
        block_size = 1;
    } else {
        block_size = (size_t)lm_ggml_blck_size(tensor->type);
    }

    size_t block_size_bytes = lm_ggml_type_size(tensor->type);

    LM_GGML_ASSERT(nelements % block_size == 0);
    size_t nblocks = nelements / block_size;
    size_t blocks_per_thread = nblocks / nthread;
    size_t spare_blocks = nblocks - (blocks_per_thread * nthread); // if blocks aren't divisible by thread count

    size_t in_buff_offs = 0;
    size_t out_buff_offs = 0;

    for (int tnum = 0; tnum < nthread; tnum++) {
        size_t thr_blocks = blocks_per_thread + (tnum == nthread - 1 ? spare_blocks : 0); // num blocks for this thread
        size_t thr_elems = thr_blocks * block_size; // number of elements for this thread
        size_t thr_block_bytes = thr_blocks * block_size_bytes; // number of input bytes for this thread

        auto compute = [qtype] (lm_ggml_type typ, uint8_t * inbuf, float * outbuf, int nels) {
            if (typ == LM_GGML_TYPE_F16) {
                lm_ggml_fp16_to_fp32_row((lm_ggml_fp16_t *)inbuf, outbuf, nels);
            } else if (typ == LM_GGML_TYPE_BF16) {
                lm_ggml_bf16_to_fp32_row((lm_ggml_bf16_t *)inbuf, outbuf, nels);
            } else {
                qtype->to_float(inbuf, outbuf, nels);
            }
        };
        workers.emplace_back(compute, tensor->type, (uint8_t *) tensor->data + in_buff_offs, f32_output + out_buff_offs, thr_elems);
        in_buff_offs += thr_block_bytes;
        out_buff_offs += thr_elems;
    }
    for (auto & w : workers) { w.join(); }
    workers.clear();
}

static lm_ggml_type llama_tensor_get_type(quantize_state_internal & qs, lm_ggml_type new_type, const lm_ggml_tensor * tensor, llama_ftype ftype) {
    const std::string name = lm_ggml_get_name(tensor);

    // TODO: avoid hardcoded tensor names - use the TN_* constants
    const llm_arch arch = qs.model.arch;
    const auto       tn = LLM_TN(arch);

    auto use_more_bits = [](int i_layer, int n_layers) -> bool {
        return i_layer < n_layers/8 || i_layer >= 7*n_layers/8 || (i_layer - n_layers/8)%3 == 2;
    };
    const int n_expert = std::max(1, (int)qs.model.hparams.n_expert);
    auto layer_info = [n_expert] (int i_layer, int n_layer, const char * name) {
        if (n_expert > 1) {
            // Believe it or not, "experts" in the FFN of Mixtral-8x7B are not consecutive, but occasionally randomly
            // sprinkled in the model. Hence, simply dividing i_ffn_down by n_expert does not work
            // for getting the current layer as I initially thought, and we need to resort to parsing the
            // tensor name.
            if (sscanf(name, "blk.%d.", &i_layer) != 1) {
                throw std::runtime_error(format("Failed to determine layer for tensor %s", name));
            }
            if (i_layer < 0 || i_layer >= n_layer) {
                throw std::runtime_error(format("Bad layer %d for tensor %s. Must be in [0, %d)", i_layer, name, n_layer));
            }
        }
        return std::make_pair(i_layer, n_layer);
    };

    // for arches that share the same tensor between the token embeddings and the output, we quantize the token embeddings
    // with the quantization of the output tensor
    if (name == tn(LLM_TENSOR_OUTPUT, "weight") || (!qs.has_output && name == tn(LLM_TENSOR_TOKEN_EMBD, "weight"))) {
        if (qs.params->output_tensor_type < LM_GGML_TYPE_COUNT) {
            new_type = qs.params->output_tensor_type;
        } else {
            int nx = tensor->ne[0];
            if (arch == LLM_ARCH_FALCON || nx % QK_K != 0) {
                new_type = LM_GGML_TYPE_Q8_0;
            }
            else if (ftype == LLAMA_FTYPE_MOSTLY_IQ2_XXS || ftype == LLAMA_FTYPE_MOSTLY_IQ2_XS || ftype == LLAMA_FTYPE_MOSTLY_IQ3_XXS ||
                     ftype == LLAMA_FTYPE_MOSTLY_IQ1_S   || ftype == LLAMA_FTYPE_MOSTLY_IQ2_S  || ftype == LLAMA_FTYPE_MOSTLY_IQ2_M   ||
                     ftype == LLAMA_FTYPE_MOSTLY_IQ1_M) {
                new_type = LM_GGML_TYPE_Q5_K;
            }
            else if (new_type != LM_GGML_TYPE_Q8_0) {
                new_type = LM_GGML_TYPE_Q6_K;
            }
        }
    } else if (name == "token_embd.weight") {
        if (qs.params->token_embedding_type < LM_GGML_TYPE_COUNT) {
            new_type = qs.params->token_embedding_type;
        } else {
            if (ftype == LLAMA_FTYPE_MOSTLY_IQ2_XXS || ftype == LLAMA_FTYPE_MOSTLY_IQ2_XS ||
                ftype == LLAMA_FTYPE_MOSTLY_IQ1_S   || ftype == LLAMA_FTYPE_MOSTLY_IQ1_M) {
                new_type = LM_GGML_TYPE_Q2_K;
            }
            else if (ftype == LLAMA_FTYPE_MOSTLY_IQ2_S || ftype == LLAMA_FTYPE_MOSTLY_IQ2_M) {
                new_type = LM_GGML_TYPE_IQ3_S;
            }
            else if (ftype == LLAMA_FTYPE_MOSTLY_IQ3_XXS) {
                new_type = LM_GGML_TYPE_IQ3_S;
            }
            else if (new_type == LM_GGML_TYPE_Q4_0_4_4 || new_type == LM_GGML_TYPE_Q4_0_4_8 ||
                     new_type == LM_GGML_TYPE_Q4_0_8_8) {
                new_type = LM_GGML_TYPE_Q4_0;
            }
            else if (ftype == LLAMA_FTYPE_MOSTLY_TQ1_0 || ftype == LLAMA_FTYPE_MOSTLY_TQ2_0) {
                new_type = LM_GGML_TYPE_Q4_K;
            }
        }
    } else if (ftype == LLAMA_FTYPE_MOSTLY_IQ2_XXS || ftype == LLAMA_FTYPE_MOSTLY_IQ2_XS || ftype == LLAMA_FTYPE_MOSTLY_IQ1_S ||
               ftype == LLAMA_FTYPE_MOSTLY_IQ2_S || ftype == LLAMA_FTYPE_MOSTLY_IQ2_M    || ftype == LLAMA_FTYPE_MOSTLY_IQ1_M) {
        if (name.find("attn_v.weight") != std::string::npos) {
            if (qs.model.hparams.n_gqa() >= 4 || qs.model.hparams.n_expert >= 4) new_type = LM_GGML_TYPE_Q4_K;
            else new_type = ftype == LLAMA_FTYPE_MOSTLY_IQ2_S || ftype == LLAMA_FTYPE_MOSTLY_IQ2_M ? LM_GGML_TYPE_IQ3_S : LM_GGML_TYPE_Q2_K;
            ++qs.i_attention_wv;
        }
        else if (qs.model.hparams.n_expert == 8 && name.find("attn_k.weight") != std::string::npos) {
            new_type = LM_GGML_TYPE_Q4_K;
        }
        else if (name.find("ffn_down") != std::string::npos) {
            if (qs.i_ffn_down < qs.n_ffn_down/8) {
                new_type = ftype == LLAMA_FTYPE_MOSTLY_IQ2_S || ftype == LLAMA_FTYPE_MOSTLY_IQ2_M ? LM_GGML_TYPE_IQ3_S : LM_GGML_TYPE_Q2_K;
            }
            ++qs.i_ffn_down;
        }
        else if (name.find("attn_output.weight") != std::string::npos) {
            if (qs.model.hparams.n_expert == 8) {
                new_type = LM_GGML_TYPE_Q5_K;
            } else {
                if (ftype == LLAMA_FTYPE_MOSTLY_IQ1_S || ftype == LLAMA_FTYPE_MOSTLY_IQ1_M) new_type = LM_GGML_TYPE_IQ2_XXS;
                else if (ftype == LLAMA_FTYPE_MOSTLY_IQ2_S || ftype == LLAMA_FTYPE_MOSTLY_IQ2_M) new_type = LM_GGML_TYPE_IQ3_S;
            }
        }
    } else if (name.find("attn_v.weight") != std::string::npos) {
        if      (ftype == LLAMA_FTYPE_MOSTLY_Q2_K) {
            new_type = qs.model.hparams.n_gqa() >= 4 ? LM_GGML_TYPE_Q4_K : LM_GGML_TYPE_Q3_K;
        }
        else if (ftype == LLAMA_FTYPE_MOSTLY_Q2_K_S && qs.model.hparams.n_gqa() >= 4) {
            new_type = LM_GGML_TYPE_Q4_K;
        }
        else if (ftype == LLAMA_FTYPE_MOSTLY_IQ3_XXS) {
            new_type = qs.model.hparams.n_gqa() >= 4 ? LM_GGML_TYPE_Q4_K : !qs.has_imatrix ? LM_GGML_TYPE_IQ3_S : LM_GGML_TYPE_IQ3_XXS;
        }
        else if ((ftype == LLAMA_FTYPE_MOSTLY_IQ3_XS || ftype == LLAMA_FTYPE_MOSTLY_IQ3_S) && qs.model.hparams.n_gqa() >= 4) {
            new_type = LM_GGML_TYPE_Q4_K;
        }
        else if (ftype == LLAMA_FTYPE_MOSTLY_IQ3_M) {
            new_type = LM_GGML_TYPE_Q4_K;
        }
        else if (ftype == LLAMA_FTYPE_MOSTLY_Q3_K_M) {
            new_type = qs.i_attention_wv < 2 ? LM_GGML_TYPE_Q5_K : LM_GGML_TYPE_Q4_K;
        }
        else if (ftype == LLAMA_FTYPE_MOSTLY_Q3_K_L) new_type = LM_GGML_TYPE_Q5_K;
        else if ((ftype == LLAMA_FTYPE_MOSTLY_IQ4_NL || ftype == LLAMA_FTYPE_MOSTLY_IQ4_XS) && qs.model.hparams.n_gqa() >= 4) {
            new_type = LM_GGML_TYPE_Q5_K;
        }
        else if ((ftype == LLAMA_FTYPE_MOSTLY_Q4_K_M || ftype == LLAMA_FTYPE_MOSTLY_Q5_K_M) &&
                use_more_bits(qs.i_attention_wv, qs.n_attention_wv)) new_type = LM_GGML_TYPE_Q6_K;
        else if (ftype == LLAMA_FTYPE_MOSTLY_Q4_K_S && qs.i_attention_wv < 4) new_type = LM_GGML_TYPE_Q5_K;
        if (qs.model.type == MODEL_70B) {
            // In the 70B model we have 8 heads sharing the same attn_v weights. As a result, the attn_v.weight tensor is
            // 8x smaller compared to attn_q.weight. Hence, we can get a nice boost in quantization accuracy with
            // nearly negligible increase in model size by quantizing this tensor with more bits:
            if (new_type == LM_GGML_TYPE_Q3_K || new_type == LM_GGML_TYPE_Q4_K) new_type = LM_GGML_TYPE_Q5_K;
        }
        if (qs.model.hparams.n_expert == 8) {
            // for the 8-expert model, bumping this to Q8_0 trades just ~128MB
            // TODO: explore better strategies
            new_type = LM_GGML_TYPE_Q8_0;
        }
        ++qs.i_attention_wv;
    } else if (name.find("attn_k.weight") != std::string::npos) {
        if (qs.model.hparams.n_expert == 8) {
            // for the 8-expert model, bumping this to Q8_0 trades just ~128MB
            // TODO: explore better strategies
            new_type = LM_GGML_TYPE_Q8_0;
        }
        else if (ftype == LLAMA_FTYPE_MOSTLY_IQ3_XS) {
            new_type = LM_GGML_TYPE_IQ3_XXS;
        }
        else if (ftype == LLAMA_FTYPE_MOSTLY_IQ3_XXS) {
            new_type = LM_GGML_TYPE_IQ2_S;
        }
    } else if (name.find("attn_q.weight") != std::string::npos) {
        if (ftype == LLAMA_FTYPE_MOSTLY_IQ3_XS) {
            new_type = LM_GGML_TYPE_IQ3_XXS;
        }
        else if (ftype == LLAMA_FTYPE_MOSTLY_IQ3_XXS) {
            new_type = LM_GGML_TYPE_IQ2_S;
        }
    } else if (name.find("ffn_down") != std::string::npos) {
        auto info = layer_info(qs.i_ffn_down, qs.n_ffn_down, name.c_str());
        int i_layer = info.first, n_layer = info.second;
        if      (ftype == LLAMA_FTYPE_MOSTLY_Q2_K) new_type = LM_GGML_TYPE_Q3_K;
        else if (ftype == LLAMA_FTYPE_MOSTLY_Q2_K_S) {
            if (i_layer < n_layer/8) new_type = LM_GGML_TYPE_Q4_K;
        }
        else if (ftype == LLAMA_FTYPE_MOSTLY_IQ3_XXS && !qs.has_imatrix) {
            new_type = i_layer < n_layer/8 ? LM_GGML_TYPE_Q4_K : LM_GGML_TYPE_Q3_K;
        }
        else if (ftype == LLAMA_FTYPE_MOSTLY_Q3_K_M) {
            new_type = i_layer < n_layer/16 ? LM_GGML_TYPE_Q5_K
                     : arch != LLM_ARCH_FALCON || use_more_bits(i_layer, n_layer) ? LM_GGML_TYPE_Q4_K
                     : LM_GGML_TYPE_Q3_K;
        }
        else if (ftype == LLAMA_FTYPE_MOSTLY_IQ3_M && (i_layer < n_layer/8 ||
                    (qs.model.hparams.n_expert == 8 && use_more_bits(i_layer, n_layer)))) {
            new_type = LM_GGML_TYPE_Q4_K;
        }
        else if (ftype == LLAMA_FTYPE_MOSTLY_Q3_K_L) {
            new_type = arch == LLM_ARCH_FALCON ? LM_GGML_TYPE_Q4_K : LM_GGML_TYPE_Q5_K;
        }
        else if (ftype == LLAMA_FTYPE_MOSTLY_Q4_K_M) {
            if (arch == LLM_ARCH_FALCON) {
                new_type = i_layer < n_layer/16 ? LM_GGML_TYPE_Q6_K :
                           use_more_bits(i_layer, n_layer) ? LM_GGML_TYPE_Q5_K : LM_GGML_TYPE_Q4_K;
            } else {
                if (use_more_bits(i_layer, n_layer)) new_type = LM_GGML_TYPE_Q6_K;
            }
        }
        else if (i_layer < n_layer/8 && (ftype == LLAMA_FTYPE_MOSTLY_IQ4_NL || ftype == LLAMA_FTYPE_MOSTLY_IQ4_XS) && !qs.has_imatrix) {
            new_type = LM_GGML_TYPE_Q5_K;
        }
        else if (ftype == LLAMA_FTYPE_MOSTLY_Q5_K_M && use_more_bits(i_layer, n_layer)) new_type = LM_GGML_TYPE_Q6_K;
        else if (ftype == LLAMA_FTYPE_MOSTLY_Q4_K_S && arch != LLM_ARCH_FALCON && i_layer < n_layer/8) {
            new_type = LM_GGML_TYPE_Q5_K;
        }
        else if ((ftype == LLAMA_FTYPE_MOSTLY_Q4_0 || ftype == LLAMA_FTYPE_MOSTLY_Q5_0)
                && qs.has_imatrix && i_layer < n_layer/8) {
            // Guard against craziness in the first few ffn_down layers that can happen even with imatrix for Q4_0/Q5_0.
            // We only do it when an imatrix is provided because a) we want to make sure that one can always get the
            // same quantization as before imatrix stuff, and b) Q4_1/Q5_1 do go crazy on ffn_down without an imatrix.
            new_type = ftype == LLAMA_FTYPE_MOSTLY_Q4_0 ? LM_GGML_TYPE_Q4_1 : LM_GGML_TYPE_Q5_1;
        }
        ++qs.i_ffn_down;
    } else if (name.find("attn_output.weight") != std::string::npos) {
        if (arch != LLM_ARCH_FALCON) {
            if (qs.model.hparams.n_expert == 8) {
                if (ftype == LLAMA_FTYPE_MOSTLY_Q2_K   || ftype == LLAMA_FTYPE_MOSTLY_IQ3_XS || ftype == LLAMA_FTYPE_MOSTLY_IQ3_XXS ||
                    ftype == LLAMA_FTYPE_MOSTLY_Q3_K_S || ftype == LLAMA_FTYPE_MOSTLY_Q3_K_M  || ftype == LLAMA_FTYPE_MOSTLY_IQ4_NL  ||
                    ftype == LLAMA_FTYPE_MOSTLY_Q4_K_S || ftype == LLAMA_FTYPE_MOSTLY_Q4_K_M  || ftype == LLAMA_FTYPE_MOSTLY_IQ3_S  ||
                    ftype == LLAMA_FTYPE_MOSTLY_IQ3_M  || ftype == LLAMA_FTYPE_MOSTLY_IQ4_XS) {
                    new_type = LM_GGML_TYPE_Q5_K;
                }
            } else {
                if      (ftype == LLAMA_FTYPE_MOSTLY_Q2_K   ) new_type = LM_GGML_TYPE_Q3_K;
                else if (ftype == LLAMA_FTYPE_MOSTLY_IQ3_XXS) new_type = LM_GGML_TYPE_IQ3_S;
                else if (ftype == LLAMA_FTYPE_MOSTLY_Q3_K_M ) new_type = LM_GGML_TYPE_Q4_K;
                else if (ftype == LLAMA_FTYPE_MOSTLY_Q3_K_L ) new_type = LM_GGML_TYPE_Q5_K;
                else if (ftype == LLAMA_FTYPE_MOSTLY_IQ3_M  ) new_type = LM_GGML_TYPE_Q4_K;
            }
        } else {
            if (ftype == LLAMA_FTYPE_MOSTLY_Q3_K_L) new_type = LM_GGML_TYPE_Q4_K;
        }
    }
    else if (name.find("attn_qkv.weight") != std::string::npos) {
        if (ftype == LLAMA_FTYPE_MOSTLY_Q3_K_M || ftype == LLAMA_FTYPE_MOSTLY_Q3_K_L || ftype == LLAMA_FTYPE_MOSTLY_IQ3_M) {
            new_type = LM_GGML_TYPE_Q4_K;
        }
        else if (ftype == LLAMA_FTYPE_MOSTLY_Q4_K_M) new_type = LM_GGML_TYPE_Q5_K;
        else if (ftype == LLAMA_FTYPE_MOSTLY_Q5_K_M) new_type = LM_GGML_TYPE_Q6_K;
    }
    else if (name.find("ffn_gate") != std::string::npos) {
        auto info = layer_info(qs.i_ffn_gate, qs.n_ffn_gate, name.c_str());
        int i_layer = info.first, n_layer = info.second;
        if (ftype == LLAMA_FTYPE_MOSTLY_IQ3_XS && (i_layer >= n_layer/8 && i_layer < 7*n_layer/8)) {
            new_type = LM_GGML_TYPE_IQ3_XXS;
        }
        ++qs.i_ffn_gate;
    }
    else if (name.find("ffn_up") != std::string::npos) {
        auto info = layer_info(qs.i_ffn_up, qs.n_ffn_up, name.c_str());
        int i_layer = info.first, n_layer = info.second;
        if (ftype == LLAMA_FTYPE_MOSTLY_IQ3_XS && (i_layer >= n_layer/8 && i_layer < 7*n_layer/8)) {
            new_type = LM_GGML_TYPE_IQ3_XXS;
        }
        ++qs.i_ffn_up;
    }

    //    if (ftype == LLAMA_FTYPE_MOSTLY_Q2_K) new_type = LM_GGML_TYPE_Q3_K;
    //}
    // IK: let's remove this, else Q2_K is almost the same as Q3_K_S
    //else if (name.find("ffn_gate") != std::string::npos || name.find("ffn_up") != std::string::npos) {
    //    if (ftype == LLAMA_FTYPE_MOSTLY_Q2_K) new_type = LM_GGML_TYPE_Q3_K;
    //}
    // This can be used to reduce the size of the Q5_K_S model.
    // The associated PPL increase is fully in line with the size reduction
    //else {
    //    if (ftype == LLAMA_FTYPE_MOSTLY_Q5_K_S) new_type = LM_GGML_TYPE_Q4_K;
    //}
    bool convert_incompatible_tensor = false;
    if (new_type == LM_GGML_TYPE_Q2_K    || new_type == LM_GGML_TYPE_Q3_K    || new_type == LM_GGML_TYPE_Q4_K   ||
        new_type == LM_GGML_TYPE_Q5_K    || new_type == LM_GGML_TYPE_Q6_K    || new_type == LM_GGML_TYPE_IQ4_XS ||
        new_type == LM_GGML_TYPE_IQ2_XS  || new_type == LM_GGML_TYPE_IQ2_XXS || new_type == LM_GGML_TYPE_IQ2_S  ||
        new_type == LM_GGML_TYPE_IQ3_XXS || new_type == LM_GGML_TYPE_IQ1_S   || new_type == LM_GGML_TYPE_IQ3_S  ||
        new_type == LM_GGML_TYPE_IQ1_M) {
        int nx = tensor->ne[0];
        int ny = tensor->ne[1];
        if (nx % QK_K != 0) {
            LLAMA_LOG_WARN("\n\n%s : tensor cols %d x %d are not divisible by %d, required for %s", __func__, nx, ny, QK_K, lm_ggml_type_name(new_type));
            convert_incompatible_tensor = true;
        } else {
            ++qs.n_k_quantized;
        }
    }
    if (convert_incompatible_tensor) {
        switch (new_type) {
            case LM_GGML_TYPE_TQ1_0:
            case LM_GGML_TYPE_TQ2_0:  new_type = LM_GGML_TYPE_Q4_0; break;  // TODO: use a symmetric type instead
            case LM_GGML_TYPE_IQ2_XXS:
            case LM_GGML_TYPE_IQ2_XS:
            case LM_GGML_TYPE_IQ2_S:
            case LM_GGML_TYPE_IQ3_XXS:
            case LM_GGML_TYPE_IQ3_S:
            case LM_GGML_TYPE_IQ1_S:
            case LM_GGML_TYPE_IQ1_M:
            case LM_GGML_TYPE_Q2_K:
            case LM_GGML_TYPE_Q3_K:
            case LM_GGML_TYPE_IQ4_XS: new_type = LM_GGML_TYPE_IQ4_NL; break;
            case LM_GGML_TYPE_Q4_K:   new_type = LM_GGML_TYPE_Q5_0;   break;
            case LM_GGML_TYPE_Q5_K:   new_type = LM_GGML_TYPE_Q5_1;   break;
            case LM_GGML_TYPE_Q6_K:   new_type = LM_GGML_TYPE_Q8_0;   break;
            default: throw std::runtime_error("\nUnsupported tensor size encountered\n");
        }
        if (tensor->ne[0] % lm_ggml_blck_size(new_type) != 0) {
            new_type = LM_GGML_TYPE_F16;
        }
        LLAMA_LOG_WARN(" - using fallback quantization %s\n", lm_ggml_type_name(new_type));
        ++qs.n_fallback;
    }

    return new_type;
}

static size_t llama_tensor_quantize_internal(enum lm_ggml_type new_type, const float * f32_data, void * new_data, const int64_t chunk_size, int64_t nrows, int64_t n_per_row, const float * imatrix, std::vector<std::thread> & workers, const int nthread) {
    if (nthread < 2) {
        // single-thread
        size_t new_size = lm_ggml_quantize_chunk(new_type, f32_data, new_data, 0, nrows, n_per_row, imatrix);
        if (!lm_ggml_validate_row_data(new_type, new_data, new_size)) {
            throw std::runtime_error("quantized data validation failed");
        }
        return new_size;
    }

    std::mutex mutex;
    int64_t counter = 0;
    size_t new_size = 0;
    bool valid = true;
    auto compute = [&mutex, &counter, &new_size, &valid, new_type, f32_data, new_data, chunk_size,
            nrows, n_per_row, imatrix]() {
        const int64_t nrows_per_chunk = chunk_size / n_per_row;
        size_t local_size = 0;
        while (true) {
            std::unique_lock<std::mutex> lock(mutex);
            int64_t first_row = counter; counter += nrows_per_chunk;
            if (first_row >= nrows) {
                if (local_size > 0) {
                    new_size += local_size;
                }
                break;
            }
            lock.unlock();
            const int64_t this_nrow = std::min(nrows - first_row, nrows_per_chunk);
            size_t this_size = lm_ggml_quantize_chunk(new_type, f32_data, new_data, first_row * n_per_row, this_nrow, n_per_row, imatrix);
            local_size += this_size;

            // validate the quantized data
            const size_t row_size  = lm_ggml_row_size(new_type, n_per_row);
            void * this_data = (char *) new_data + first_row * row_size;
            if (!lm_ggml_validate_row_data(new_type, this_data, this_size)) {
                std::unique_lock<std::mutex> lock(mutex);
                valid = false;
                break;
            }
        }
    };
    for (int it = 0; it < nthread - 1; ++it) {
        workers.emplace_back(compute);
    }
    compute();
    for (auto & w : workers) { w.join(); }
    workers.clear();
    if (!valid) {
        throw std::runtime_error("quantized data validation failed");
    }
    return new_size;
}

static void llama_model_quantize_internal(const std::string & fname_inp, const std::string & fname_out, const llama_model_quantize_params * params) {
    lm_ggml_type default_type;
    llama_ftype ftype = params->ftype;

    switch (params->ftype) {
        case LLAMA_FTYPE_MOSTLY_Q4_0: default_type = LM_GGML_TYPE_Q4_0; break;
        case LLAMA_FTYPE_MOSTLY_Q4_1: default_type = LM_GGML_TYPE_Q4_1; break;
        case LLAMA_FTYPE_MOSTLY_Q5_0: default_type = LM_GGML_TYPE_Q5_0; break;
        case LLAMA_FTYPE_MOSTLY_Q5_1: default_type = LM_GGML_TYPE_Q5_1; break;
        case LLAMA_FTYPE_MOSTLY_Q8_0: default_type = LM_GGML_TYPE_Q8_0; break;
        case LLAMA_FTYPE_MOSTLY_F16:  default_type = LM_GGML_TYPE_F16;  break;
        case LLAMA_FTYPE_MOSTLY_BF16: default_type = LM_GGML_TYPE_BF16; break;
        case LLAMA_FTYPE_ALL_F32:     default_type = LM_GGML_TYPE_F32;  break;

        // K-quants
        case LLAMA_FTYPE_MOSTLY_Q2_K_S:
        case LLAMA_FTYPE_MOSTLY_Q2_K:    default_type = LM_GGML_TYPE_Q2_K;    break;
        case LLAMA_FTYPE_MOSTLY_IQ3_XS:  default_type = LM_GGML_TYPE_IQ3_S;   break;
        case LLAMA_FTYPE_MOSTLY_Q3_K_S:
        case LLAMA_FTYPE_MOSTLY_Q3_K_M:
        case LLAMA_FTYPE_MOSTLY_Q3_K_L:  default_type = LM_GGML_TYPE_Q3_K;    break;
        case LLAMA_FTYPE_MOSTLY_Q4_K_S:
        case LLAMA_FTYPE_MOSTLY_Q4_K_M:  default_type = LM_GGML_TYPE_Q4_K;    break;
        case LLAMA_FTYPE_MOSTLY_Q5_K_S:
        case LLAMA_FTYPE_MOSTLY_Q5_K_M:  default_type = LM_GGML_TYPE_Q5_K;    break;
        case LLAMA_FTYPE_MOSTLY_Q6_K:    default_type = LM_GGML_TYPE_Q6_K;    break;
        case LLAMA_FTYPE_MOSTLY_TQ1_0:   default_type = LM_GGML_TYPE_TQ1_0;   break;
        case LLAMA_FTYPE_MOSTLY_TQ2_0:   default_type = LM_GGML_TYPE_TQ2_0;   break;
        case LLAMA_FTYPE_MOSTLY_IQ2_XXS: default_type = LM_GGML_TYPE_IQ2_XXS; break;
        case LLAMA_FTYPE_MOSTLY_IQ2_XS:  default_type = LM_GGML_TYPE_IQ2_XS;  break;
        case LLAMA_FTYPE_MOSTLY_IQ2_S:   default_type = LM_GGML_TYPE_IQ2_XS;  break;
        case LLAMA_FTYPE_MOSTLY_IQ2_M:   default_type = LM_GGML_TYPE_IQ2_S;   break;
        case LLAMA_FTYPE_MOSTLY_IQ3_XXS: default_type = LM_GGML_TYPE_IQ3_XXS; break;
        case LLAMA_FTYPE_MOSTLY_IQ1_S:   default_type = LM_GGML_TYPE_IQ1_S;   break;
        case LLAMA_FTYPE_MOSTLY_IQ1_M:   default_type = LM_GGML_TYPE_IQ1_M;   break;
        case LLAMA_FTYPE_MOSTLY_IQ4_NL:  default_type = LM_GGML_TYPE_IQ4_NL;  break;
        case LLAMA_FTYPE_MOSTLY_IQ4_XS:  default_type = LM_GGML_TYPE_IQ4_XS;  break;
        case LLAMA_FTYPE_MOSTLY_IQ3_S:   default_type = LM_GGML_TYPE_IQ3_S;   break;
        case LLAMA_FTYPE_MOSTLY_IQ3_M:   default_type = LM_GGML_TYPE_IQ3_S;   break;
        case LLAMA_FTYPE_MOSTLY_Q4_0_4_4: default_type = LM_GGML_TYPE_Q4_0_4_4; break;
        case LLAMA_FTYPE_MOSTLY_Q4_0_4_8: default_type = LM_GGML_TYPE_Q4_0_4_8; break;
        case LLAMA_FTYPE_MOSTLY_Q4_0_8_8: default_type = LM_GGML_TYPE_Q4_0_8_8; break;

        default: throw std::runtime_error(format("invalid output file type %d\n", ftype));
    }

    int nthread = params->nthread;

    if (nthread <= 0) {
        nthread = std::thread::hardware_concurrency();
    }

    // mmap consistently increases speed Linux, and also increases speed on Windows with
    // hot cache. It may cause a slowdown on macOS, possibly related to free memory.
#if defined(__linux__) || defined(_WIN32)
    constexpr bool use_mmap = true;
#else
    constexpr bool use_mmap = false;
#endif

    llama_model_kv_override * kv_overrides = nullptr;
    if (params->kv_overrides) {
        auto v = (std::vector<llama_model_kv_override>*)params->kv_overrides;
        kv_overrides = v->data();
    }
    llama_model_loader ml(fname_inp, use_mmap, /*check_tensors*/ true, kv_overrides);
    ml.init_mappings(false); // no prefetching

    llama_model model;
    llm_load_arch(ml, model);
    llm_load_hparams(ml, model);
    llm_load_stats(ml, model);

    struct quantize_state_internal qs(model, params);

    if (params->only_copy) {
        ftype = model.ftype;
    }
    const std::unordered_map<std::string, std::vector<float>> * imatrix_data = nullptr;
    if (params->imatrix) {
        imatrix_data = static_cast<const std::unordered_map<std::string, std::vector<float>>*>(params->imatrix);
        if (imatrix_data) {
            LLAMA_LOG_INFO("================================ Have weights data with %d entries\n",int(imatrix_data->size()));
            qs.has_imatrix = true;
            // check imatrix for nans or infs
            for (const auto & kv : *imatrix_data) {
                for (float f : kv.second) {
                    if (!std::isfinite(f)) {
                        throw std::runtime_error(format("imatrix contains non-finite value %f\n", f));
                    }
                }
            }
        }
    }

    const size_t align = LM_GGUF_DEFAULT_ALIGNMENT;
    lm_gguf_context_ptr ctx_out { lm_gguf_init_empty() };

    // copy the KV pairs from the input file
    lm_gguf_set_kv     (ctx_out.get(), ml.meta.get());
    lm_gguf_set_val_u32(ctx_out.get(), "general.quantization_version", LM_GGML_QNT_VERSION); // TODO: use LLM_KV
    lm_gguf_set_val_u32(ctx_out.get(), "general.file_type", ftype); // TODO: use LLM_KV

    // Remove split metadata
    lm_gguf_remove_key(ctx_out.get(), ml.llm_kv(LLM_KV_SPLIT_NO).c_str());
    lm_gguf_remove_key(ctx_out.get(), ml.llm_kv(LLM_KV_SPLIT_COUNT).c_str());
    lm_gguf_remove_key(ctx_out.get(), ml.llm_kv(LLM_KV_SPLIT_TENSORS_COUNT).c_str());

    if (params->kv_overrides) {
        const std::vector<llama_model_kv_override> & overrides = *(const std::vector<llama_model_kv_override> *)params->kv_overrides;
        for (const auto & o : overrides) {
            if (o.key[0] == 0) break;
            if (o.tag == LLAMA_KV_OVERRIDE_TYPE_FLOAT) {
                lm_gguf_set_val_f32(ctx_out.get(), o.key, o.val_f64);
            } else if (o.tag == LLAMA_KV_OVERRIDE_TYPE_INT) {
                lm_gguf_set_val_i32(ctx_out.get(), o.key, o.val_i64);
            } else if (o.tag == LLAMA_KV_OVERRIDE_TYPE_BOOL) {
                lm_gguf_set_val_bool(ctx_out.get(), o.key, o.val_bool);
            } else if (o.tag == LLAMA_KV_OVERRIDE_TYPE_STR) {
                lm_gguf_set_val_str(ctx_out.get(), o.key, o.val_str);
            } else {
                LLAMA_LOG_WARN("%s: unknown KV override type for key %s\n", __func__, o.key);
            }
        }
    }

    // make a list of weights
    std::vector<const llama_model_loader::llama_tensor_weight *> tensors;
    tensors.reserve(ml.weights_map.size());
    for (const auto & it : ml.weights_map) {
        tensors.push_back(&it.second);
    }

    // keep_split requires that the weights are sorted by split index
    if (params->keep_split) {
        std::sort(tensors.begin(), tensors.end(), [](const llama_model_loader::llama_tensor_weight * a, const llama_model_loader::llama_tensor_weight * b) {
            if (a->idx == b->idx) {
                return a->offs < b->offs;
            }
            return a->idx < b->idx;
        });
    }

    for (const auto * it : tensors) {
        const struct lm_ggml_tensor * tensor = it->tensor;

        const std::string name = lm_ggml_get_name(tensor);

        // TODO: avoid hardcoded tensor names - use the TN_* constants
        if (name.find("attn_v.weight")   != std::string::npos ||
            name.find("attn_qkv.weight") != std::string::npos ||
            name.find("attn_kv_b.weight")!= std::string::npos) {
            ++qs.n_attention_wv;
        } else if (name == LLM_TN(model.arch)(LLM_TENSOR_OUTPUT, "weight")) {
            qs.has_output = true;
        }
    }

    qs.n_ffn_down = qs.n_ffn_gate = qs.n_ffn_up = (int)model.hparams.n_layer;

    // sanity checks
    {
        const auto & n_head_kv_iter = model.hparams.n_head_kv_arr.begin();
        // attention layers have a non-zero number of kv heads
        int32_t n_attn_layer = model.hparams.n_layer - std::count(n_head_kv_iter, n_head_kv_iter + model.hparams.n_layer, 0);
        if (llama_model_has_encoder(&model)) {
            n_attn_layer *= 3;
        }
        LM_GGML_ASSERT((qs.n_attention_wv == n_attn_layer) && "n_attention_wv is unexpected");
    }

    size_t total_size_org = 0;
    size_t total_size_new = 0;

    std::vector<std::thread> workers;
    workers.reserve(nthread);

    int idx = 0;

    std::vector<no_init<uint8_t>> read_data;
    std::vector<no_init<uint8_t>> work;
    std::vector<no_init<float>> f32_conv_buf;

    uint16_t n_split = 1;

    // Assume split index is continuous
    if (params->keep_split) {
        for (const auto * it : tensors) {
            n_split = std::max(uint16_t(it->idx + 1), n_split);
        }
    }
    std::vector<lm_gguf_context_ptr> ctx_outs(n_split);
    ctx_outs[0] = std::move(ctx_out);

    // populate the original tensors so we get an initial meta data
    for (const auto * it : tensors) {
        uint16_t i_split = params->keep_split ? it->idx : 0;
        struct lm_ggml_tensor * tensor = it->tensor;
        if (!ctx_outs[i_split]) {
            ctx_outs[i_split].reset(lm_gguf_init_empty());
        }
        lm_gguf_add_tensor(ctx_outs[i_split].get(), tensor);
    }

    // Set split info if needed
    if (n_split > 1) {
        for (size_t i = 0; i < ctx_outs.size(); ++i) {
            lm_gguf_set_val_u16(ctx_outs[i].get(), ml.llm_kv(LLM_KV_SPLIT_NO).c_str(), i);
            lm_gguf_set_val_u16(ctx_outs[i].get(), ml.llm_kv(LLM_KV_SPLIT_COUNT).c_str(), n_split);
            lm_gguf_set_val_i32(ctx_outs[i].get(), ml.llm_kv(LLM_KV_SPLIT_TENSORS_COUNT).c_str(), ml.n_tensors);
        }
    }

    int cur_split = -1;
    std::ofstream fout;
    auto close_ofstream = [&]() {
        // Write metadata and close file handler
        if (fout.is_open()) {
            fout.seekp(0);
            std::vector<uint8_t> data(lm_gguf_get_meta_size(ctx_outs[cur_split].get()));
            lm_gguf_get_meta_data(ctx_outs[cur_split].get(), data.data());
            fout.write((const char *) data.data(), data.size());
            fout.close();
        }
    };
    auto new_ofstream = [&](int index) {
        cur_split = index;
        LM_GGML_ASSERT(ctx_outs[cur_split] && "Find uninitialized lm_gguf_context");
        std::string fname = fname_out;
        if (params->keep_split) {
            char split_path[PATH_MAX] = {0};
            llama_split_path(split_path, sizeof(split_path), fname_out.c_str(), cur_split, n_split);
            fname = std::string(split_path);
        }

        fout = std::ofstream(fname, std::ios::binary);
        fout.exceptions(std::ofstream::failbit); // fail fast on write errors
        const size_t meta_size = lm_gguf_get_meta_size(ctx_outs[cur_split].get());
        // placeholder for the meta data
        ::zeros(fout, meta_size);
    };

    const auto tn = LLM_TN(model.arch);
    new_ofstream(0);
    for (const auto * it : tensors) {
        const auto & weight = *it;
        struct lm_ggml_tensor * tensor = weight.tensor;
        if (weight.idx != cur_split && params->keep_split) {
            close_ofstream();
            new_ofstream(weight.idx);
        }

        const std::string name = lm_ggml_get_name(tensor);

        if (!ml.use_mmap) {
            if (read_data.size() < lm_ggml_nbytes(tensor)) {
                read_data.resize(lm_ggml_nbytes(tensor));
            }
            tensor->data = read_data.data();
        }
        ml.load_data_for(tensor);

        LLAMA_LOG_INFO("[%4d/%4d] %36s - [%s], type = %6s, ",
               ++idx, ml.n_tensors,
               lm_ggml_get_name(tensor),
               llama_format_tensor_shape(tensor).c_str(),
               lm_ggml_type_name(tensor->type));

        // This used to be a regex, but <regex> has an extreme cost to compile times.
        bool quantize = name.rfind("weight") == name.size() - 6; // ends with 'weight'?

        // quantize only 2D and 3D tensors (experts)
        quantize &= (lm_ggml_n_dims(tensor) >= 2);

        // do not quantize norm tensors
        quantize &= name.find("_norm.weight") == std::string::npos;

        quantize &= params->quantize_output_tensor || name != "output.weight";
        quantize &= !params->only_copy;

        // do not quantize expert gating tensors
        // NOTE: can't use LLM_TN here because the layer number is not known
        quantize &= name.find("ffn_gate_inp.weight") == std::string::npos;

        // do not quantize positional embeddings and token types (BERT)
        quantize &= name != LLM_TN(model.arch)(LLM_TENSOR_POS_EMBD,    "weight");
        quantize &= name != LLM_TN(model.arch)(LLM_TENSOR_TOKEN_TYPES, "weight");

        // do not quantize Mamba's small yet 2D weights
        // NOTE: can't use LLM_TN here because the layer number is not known
        quantize &= name.find("ssm_conv1d.weight") == std::string::npos;

        // do not quantize RWKV's time_mix_first tensors
        quantize &= name.find("time_mix_first.weight") == std::string::npos;
        quantize &= name.find("time_mix_w1.weight") == std::string::npos;
        quantize &= name.find("time_mix_w2.weight") == std::string::npos;
        quantize &= name.find("time_mix_decay_w1.weight") == std::string::npos;
        quantize &= name.find("time_mix_decay_w2.weight") == std::string::npos;

        // do not quantize relative position bias (T5)
        quantize &= name.find("attn_rel_b.weight") == std::string::npos;

        enum lm_ggml_type new_type;
        void * new_data;
        size_t new_size;

        if (quantize) {
            new_type = default_type;

            // get more optimal quantization type based on the tensor shape, layer, etc.
            if (!params->pure && lm_ggml_is_quantized(default_type)) {
                new_type = llama_tensor_get_type(qs, new_type, tensor, ftype);
            }
            if (params->token_embedding_type < LM_GGML_TYPE_COUNT && strcmp(tensor->name, "token_embd.weight") == 0) {
                new_type = params->token_embedding_type;
            }
            if (params->output_tensor_type < LM_GGML_TYPE_COUNT && strcmp(tensor->name, "output.weight") == 0) {
                new_type = params->output_tensor_type;
            }

            // If we've decided to quantize to the same type the tensor is already
            // in then there's nothing to do.
            quantize = tensor->type != new_type;
        }

        if (!quantize) {
            new_type = tensor->type;
            new_data = tensor->data;
            new_size = lm_ggml_nbytes(tensor);
            LLAMA_LOG_INFO("size = %8.3f MB\n", lm_ggml_nbytes(tensor)/1024.0/1024.0);
        } else {
            const int64_t nelements = lm_ggml_nelements(tensor);

            const float * imatrix = nullptr;
            if (imatrix_data) {
                auto it = imatrix_data->find(tensor->name);
                if (it == imatrix_data->end()) {
                    LLAMA_LOG_INFO("\n====== %s: did not find weights for %s\n", __func__, tensor->name);
                } else {
                    if (it->second.size() == (size_t)tensor->ne[0]*tensor->ne[2]) {
                        imatrix = it->second.data();
                    } else {
                        LLAMA_LOG_INFO("\n====== %s: imatrix size %d is different from tensor size %d for %s\n", __func__,
                                int(it->second.size()), int(tensor->ne[0]*tensor->ne[2]), tensor->name);

                        // this can happen when quantizing an old mixtral model with split tensors with a new incompatible imatrix
                        // this is a significant error and it may be good idea to abort the process if this happens,
                        // since many people will miss the error and not realize that most of the model is being quantized without an imatrix
                        // tok_embd should be ignored in this case, since it always causes this warning
                        if (name != tn(LLM_TENSOR_TOKEN_EMBD, "weight")) {
                            throw std::runtime_error(format("imatrix size %d is different from tensor size %d for %s",
                                    int(it->second.size()), int(tensor->ne[0]*tensor->ne[2]), tensor->name));
                        }
                    }
                }
            }
            if ((new_type == LM_GGML_TYPE_IQ2_XXS ||
                 new_type == LM_GGML_TYPE_IQ2_XS  ||
                 new_type == LM_GGML_TYPE_IQ2_S   ||
                 new_type == LM_GGML_TYPE_IQ1_S   ||
                (new_type == LM_GGML_TYPE_IQ1_M && strcmp(tensor->name, "token_embd.weight") && strcmp(tensor->name, "output.weight"))  ||
                (new_type == LM_GGML_TYPE_Q2_K && params->ftype == LLAMA_FTYPE_MOSTLY_Q2_K_S && strcmp(tensor->name, "token_embd.weight") != 0)) && !imatrix) {
                LLAMA_LOG_ERROR("\n\n============================================================\n");
                LLAMA_LOG_ERROR("Missing importance matrix for tensor %s in a very low-bit quantization\n", tensor->name);
                LLAMA_LOG_ERROR("The result will be garbage, so bailing out\n");
                LLAMA_LOG_ERROR("============================================================\n\n");
                throw std::runtime_error(format("Missing importance matrix for tensor %s in a very low-bit quantization", tensor->name));
            }

            float * f32_data;

            if (tensor->type == LM_GGML_TYPE_F32) {
                f32_data = (float *) tensor->data;
            } else if (lm_ggml_is_quantized(tensor->type) && !params->allow_requantize) {
                throw std::runtime_error(format("requantizing from type %s is disabled", lm_ggml_type_name(tensor->type)));
            } else {
                llama_tensor_dequantize_internal(tensor, f32_conv_buf, workers, nelements, nthread);
                f32_data = (float *) f32_conv_buf.data();
            }

            int chunk_size_multiplier = 1;
            if (new_type == LM_GGML_TYPE_Q4_0_4_4 || new_type == LM_GGML_TYPE_Q4_0_4_8 || new_type == LM_GGML_TYPE_Q4_0_8_8) {
                if ((new_type == LM_GGML_TYPE_Q4_0_8_8) && (tensor->ne[1] % 8 != 0)) new_type = LM_GGML_TYPE_Q4_0;
                else if (tensor->ne[1] % 4 != 0) new_type = LM_GGML_TYPE_Q4_0;
                if (new_type == LM_GGML_TYPE_Q4_0_8_8) chunk_size_multiplier = 8;
                else if (new_type == LM_GGML_TYPE_Q4_0_4_4 || new_type == LM_GGML_TYPE_Q4_0_4_8) chunk_size_multiplier = 4;
            }

            LLAMA_LOG_INFO("converting to %s .. ", lm_ggml_type_name(new_type));
            fflush(stdout);

            if (work.size() < (size_t)nelements * 4) {
                work.resize(nelements * 4); // upper bound on size
            }
            new_data = work.data();

            const int64_t n_per_row = tensor->ne[0];
            const int64_t nrows = tensor->ne[1];

            static const int64_t min_chunk_size = 32 * 512;
            const int64_t chunk_size = (n_per_row >= min_chunk_size ? n_per_row : n_per_row * ((min_chunk_size + n_per_row - 1)/n_per_row)) *
                                       chunk_size_multiplier;

            const int64_t nelements_matrix = tensor->ne[0] * tensor->ne[1];
            const int64_t nchunk = (nelements_matrix + chunk_size - 1)/chunk_size;
            const int64_t nthread_use = nthread > 1 ? std::max((int64_t)1, std::min((int64_t)nthread, nchunk)) : 1;

            // quantize each expert separately since they have different importance matrices
            new_size = 0;
            for (int64_t i03 = 0; i03 < tensor->ne[2]; ++i03) {
                const float * f32_data_03 = f32_data + i03 * nelements_matrix;
                void * new_data_03 = (char *)new_data + lm_ggml_row_size(new_type, n_per_row) * i03 * nrows;
                const float * imatrix_03 = imatrix ? imatrix + i03 * n_per_row : nullptr;

                new_size += llama_tensor_quantize_internal(new_type, f32_data_03, new_data_03, chunk_size, nrows, n_per_row, imatrix_03, workers, nthread_use);
            }
            LLAMA_LOG_INFO("size = %8.2f MiB -> %8.2f MiB\n", lm_ggml_nbytes(tensor)/1024.0/1024.0, new_size/1024.0/1024.0);
        }
        total_size_org += lm_ggml_nbytes(tensor);
        total_size_new += new_size;

        // update the gguf meta data as we go
        lm_gguf_set_tensor_type(ctx_outs[cur_split].get(), name.c_str(), new_type);
        lm_gguf_set_tensor_data(ctx_outs[cur_split].get(), name.c_str(), new_data, new_size);

        // write tensor data + padding
        fout.write((const char *) new_data, new_size);
        zeros(fout, LM_GGML_PAD(new_size, align) - new_size);
    }
    close_ofstream();

    LLAMA_LOG_INFO("%s: model size  = %8.2f MB\n", __func__, total_size_org/1024.0/1024.0);
    LLAMA_LOG_INFO("%s: quant size  = %8.2f MB\n", __func__, total_size_new/1024.0/1024.0);

    if (qs.n_fallback > 0) {
        LLAMA_LOG_WARN("%s: WARNING: %d of %d tensor(s) required fallback quantization\n",
                __func__, qs.n_fallback, qs.n_k_quantized + qs.n_fallback);
    }
}

static void llama_lora_adapter_init_internal(struct llama_model * model, const char * path_lora, struct llama_lora_adapter & adapter) {
    LLAMA_LOG_INFO("%s: loading lora adapter from '%s' ...\n", __func__, path_lora);

    lm_ggml_context * ctx_init;
    struct lm_gguf_init_params meta_lm_gguf_params = {
        /* .no_alloc = */ true,
        /* .ctx      = */ &ctx_init,
    };

    lm_gguf_context_ptr ctx_gguf { lm_gguf_init_from_file(path_lora, meta_lm_gguf_params) };
    if (!ctx_gguf) {
        throw std::runtime_error("failed to load lora adapter file from " + std::string(path_lora));
    }

    lm_ggml_context_ptr ctx { ctx_init };

    // check metadata
    {
        auto get_kv_str = [&](const std::string & key) -> std::string {
            int id = lm_gguf_find_key(ctx_gguf.get(), key.c_str());
            return id < 0 ? "" : std::string(lm_gguf_get_val_str(ctx_gguf.get(), id));
        };
        auto get_kv_f32 = [&](const std::string & key) -> float {
            int id = lm_gguf_find_key(ctx_gguf.get(), key.c_str());
            return id < 0 ? 0.0f : lm_gguf_get_val_f32(ctx_gguf.get(), id);
        };
        LLM_KV llm_kv = LLM_KV(LLM_ARCH_UNKNOWN);

        auto general_type = get_kv_str(llm_kv(LLM_KV_GENERAL_TYPE));
        if (general_type != "adapter") {
            throw std::runtime_error("expect general.type to be 'adapter', but got: " + general_type);
        }

        auto general_arch_str = get_kv_str(llm_kv(LLM_KV_GENERAL_ARCHITECTURE));
        auto general_arch = llm_arch_from_string(general_arch_str);
        if (general_arch != model->arch) {
            throw std::runtime_error("model arch and LoRA arch mismatch");
        }

        auto adapter_type = get_kv_str(llm_kv(LLM_KV_ADAPTER_TYPE));
        if (adapter_type != "lora") {
            throw std::runtime_error("expect adapter.type to be 'lora', but got: " + adapter_type);
        }

        adapter.alpha = get_kv_f32(llm_kv(LLM_KV_ADAPTER_LORA_ALPHA));
    }

    int n_tensors = lm_gguf_get_n_tensors(ctx_gguf.get());

    // contexts for each buffer type
    std::map<lm_ggml_backend_buffer_type_t, lm_ggml_context *> ctx_map;
    auto ctx_for_buft = [&](lm_ggml_backend_buffer_type_t buft) -> lm_ggml_context * {
        auto it = ctx_map.find(buft);
        if (it == ctx_map.end()) {
            // add a new context
            struct lm_ggml_init_params params = {
                /*.mem_size   =*/ n_tensors*lm_ggml_tensor_overhead(),
                /*.mem_buffer =*/ NULL,
                /*.no_alloc   =*/ true,
            };
            lm_ggml_context * buft_ctx = lm_ggml_init(params);
            if (!buft_ctx) {
                return nullptr;
            }
            ctx_map[buft] = buft_ctx;
            adapter.ctxs.emplace_back(buft_ctx);
            return buft_ctx;
        };
        return it->second;
    };

    // bundle lora_a and lora_b into pairs
    std::map<std::string, llama_lora_weight> ab_map;
    auto str_endswith = [](const std::string & str, const std::string & suffix) {
        return str.size() >= suffix.size() && str.compare(str.size()-suffix.size(), suffix.size(), suffix) == 0;
    };
    for (lm_ggml_tensor * cur = lm_ggml_get_first_tensor(ctx.get()); cur; cur = lm_ggml_get_next_tensor(ctx.get(), cur)) {
        std::string name(cur->name);
        if (str_endswith(name, ".lora_a")) {
            replace_all(name, ".lora_a", "");
            if (ab_map.find(name) == ab_map.end()) {
                ab_map[name] = llama_lora_weight(cur, nullptr);
            } else {
                ab_map[name].a = cur;
            }
        } else if (str_endswith(name, ".lora_b")) {
            replace_all(name, ".lora_b", "");
            if (ab_map.find(name) == ab_map.end()) {
                ab_map[name] = llama_lora_weight(nullptr, cur);
            } else {
                ab_map[name].b = cur;
            }
        } else {
            throw std::runtime_error("LoRA tensor '" + name + "' has unexpected suffix");
        }
    }

    // add tensors
    for (auto & it : ab_map) {
        const std::string & name = it.first;
        llama_lora_weight & w = it.second;

        if (!w.a || !w.b) {
            throw std::runtime_error("LoRA tensor pair for '" + name + "' is missing one component");
        }

        // device buft and device ctx
        auto * model_tensor = llama_get_model_tensor(model, name.c_str());
        if (!model_tensor) {
            throw std::runtime_error("LoRA tensor '" + name + "' does not exist in base model");
        }
        struct lm_ggml_context * dev_ctx = ctx_for_buft(lm_ggml_backend_buffer_get_type(model_tensor->buffer));
        // validate tensor shape
        if (model_tensor->ne[0] != w.a->ne[0] || model_tensor->ne[1] != w.b->ne[1]) {
            throw std::runtime_error("tensor '" + name + "' has incorrect shape");
        }
        if (w.a->ne[1] != w.b->ne[0]) {
            throw std::runtime_error("lora_a tensor is not transposed (hint: adapter from \"finetune\" example is no longer supported)");
        }
        // save tensor to adapter
        struct lm_ggml_tensor * tensor_a = lm_ggml_dup_tensor(dev_ctx, w.a);
        struct lm_ggml_tensor * tensor_b = lm_ggml_dup_tensor(dev_ctx, w.b);
        lm_ggml_set_name(tensor_a, w.a->name);
        lm_ggml_set_name(tensor_b, w.b->name);
        adapter.ab_map[name] = llama_lora_weight(tensor_a, tensor_b);
    }

    // allocate tensors / buffers and zero
    {
        adapter.ctxs.reserve(ctx_map.size());
        adapter.bufs.reserve(ctx_map.size());
        for (auto & it : ctx_map) {
            lm_ggml_backend_buffer_type_t buft = it.first;
            lm_ggml_context * ctx_dev = it.second;
            lm_ggml_backend_buffer_ptr buf { lm_ggml_backend_alloc_ctx_tensors_from_buft(ctx_dev, buft) };
            if (!buf) {
                throw std::runtime_error("failed to allocate buffer for lora adapter\n");
            }
            LLAMA_LOG_INFO("%s: %10s LoRA buffer size = %8.2f MiB\n", __func__, lm_ggml_backend_buffer_name(buf.get()), lm_ggml_backend_buffer_get_size(buf.get())/1024.0/1024.0);
            adapter.bufs.emplace_back(std::move(buf));
        }
    }

    // set tensor data
    {
        llama_file lm_gguf_file(path_lora, "rb");
        std::vector<uint8_t> read_buf;
        auto set_tensor = [&](struct lm_ggml_tensor * orig, struct lm_ggml_tensor * dev) {
            size_t offs = lm_gguf_get_data_offset(ctx_gguf.get()) + lm_gguf_get_tensor_offset(ctx_gguf.get(), lm_gguf_find_tensor(ctx_gguf.get(), orig->name));
            size_t size = lm_ggml_nbytes(orig);
            read_buf.resize(size);
            lm_gguf_file.seek(offs, SEEK_SET);
            lm_gguf_file.read_raw(read_buf.data(), size);
            lm_ggml_backend_tensor_set(dev, read_buf.data(), 0, size);
        };
        for (auto & it : adapter.ab_map) {
            auto orig = ab_map[it.first];
            auto dev  = it.second;
            set_tensor(orig.a, dev.a);
            set_tensor(orig.b, dev.b);
        }
    }

    LLAMA_LOG_INFO("%s: loaded %zu tensors from lora file\n", __func__, adapter.ab_map.size()*2);
}

int32_t llama_lora_adapter_set(
            struct llama_context * ctx,
            struct llama_lora_adapter * adapter,
            float scale) {
    if (ctx->cparams.flash_attn) {
        LLAMA_LOG_ERROR("%s: flash_attn is not compatible with LoRA\n", __func__);
        return -1;
    }
    ctx->lora_adapters[adapter] = scale;
    return 0;
}

int32_t llama_lora_adapter_remove(
            struct llama_context * ctx,
            struct llama_lora_adapter * adapter) {
    auto pos = ctx->lora_adapters.find(adapter);
    if (pos != ctx->lora_adapters.end()) {
        ctx->lora_adapters.erase(pos);
        return 0;
    }
    return -1;
}

void llama_lora_adapter_clear(struct llama_context * ctx) {
    ctx->lora_adapters.clear();
}

void llama_lora_adapter_free(struct llama_lora_adapter * adapter) {
    delete adapter;
}

//
// interface implementation
//
struct llama_model_params llama_model_default_params() {
    struct llama_model_params result = {
        /*.n_gpu_layers                =*/ 0,
        /*.split_mode                  =*/ LLAMA_SPLIT_MODE_LAYER,
        /*.main_gpu                    =*/ 0,
        /*.tensor_split                =*/ nullptr,
        /*.rpc_servers                 =*/ nullptr,
        /*.progress_callback           =*/ nullptr,
        /*.progress_callback_user_data =*/ nullptr,
        /*.kv_overrides                =*/ nullptr,
        /*.vocab_only                  =*/ false,
        /*.use_mmap                    =*/ true,
        /*.use_mlock                   =*/ false,
        /*.check_tensors               =*/ false,
    };

#ifdef LM_GGML_USE_METAL
    // note: we usually have plenty of VRAM, so by default offload all layers to the GPU
    result.n_gpu_layers = 999;
#endif

    return result;
}

struct llama_context_params llama_context_default_params() {
    struct llama_context_params result = {
        /*.n_ctx                       =*/ 512,
        /*.n_batch                     =*/ 2048,
        /*.n_ubatch                    =*/ 512,
        /*.n_seq_max                   =*/ 1,
        /*.n_threads                   =*/ LM_GGML_DEFAULT_N_THREADS, // TODO: better default
        /*.n_threads_batch             =*/ LM_GGML_DEFAULT_N_THREADS,
        /*.rope_scaling_type           =*/ LLAMA_ROPE_SCALING_TYPE_UNSPECIFIED,
        /*.pooling_type                =*/ LLAMA_POOLING_TYPE_UNSPECIFIED,
        /*.attention_type              =*/ LLAMA_ATTENTION_TYPE_UNSPECIFIED,
        /*.rope_freq_base              =*/ 0.0f,
        /*.rope_freq_scale             =*/ 0.0f,
        /*.yarn_ext_factor             =*/ -1.0f,
        /*.yarn_attn_factor            =*/ 1.0f,
        /*.yarn_beta_fast              =*/ 32.0f,
        /*.yarn_beta_slow              =*/ 1.0f,
        /*.yarn_orig_ctx               =*/ 0,
        /*.defrag_thold                =*/ -1.0f,
        /*.cb_eval                     =*/ nullptr,
        /*.cb_eval_user_data           =*/ nullptr,
        /*.type_k                      =*/ LM_GGML_TYPE_F16,
        /*.type_v                      =*/ LM_GGML_TYPE_F16,
        /*.logits_all                  =*/ false,
        /*.embeddings                  =*/ false,
        /*.offload_kqv                 =*/ true,
        /*.flash_attn                  =*/ false,
        /*.no_perf                     =*/ true,
        /*.abort_callback              =*/ nullptr,
        /*.abort_callback_data         =*/ nullptr,
    };

    return result;
}

struct llama_sampler_chain_params llama_sampler_chain_default_params() {
    struct llama_sampler_chain_params result = {
        /*.no_perf                     =*/ true,
    };

    return result;
}

struct llama_model_quantize_params llama_model_quantize_default_params() {
    struct llama_model_quantize_params result = {
        /*.nthread                     =*/ 0,
        /*.ftype                       =*/ LLAMA_FTYPE_MOSTLY_Q5_1,
        /*.output_tensor_type          =*/ LM_GGML_TYPE_COUNT,
        /*.token_embedding_type        =*/ LM_GGML_TYPE_COUNT,
        /*.allow_requantize            =*/ false,
        /*.quantize_output_tensor      =*/ true,
        /*.only_copy                   =*/ false,
        /*.pure                        =*/ false,
        /*.keep_split                  =*/ false,
        /*.imatrix                     =*/ nullptr,
        /*.kv_overrides                =*/ nullptr,
    };

    return result;
}

size_t llama_max_devices(void) {
    return 16;
}

bool llama_supports_mmap(void) {
    return llama_mmap::SUPPORTED;
}

bool llama_supports_mlock(void) {
    return llama_mlock::SUPPORTED;
}

bool llama_supports_gpu_offload(void) {
    return lm_ggml_backend_dev_by_type(LM_GGML_BACKEND_DEVICE_TYPE_GPU) != nullptr ||
           llama_supports_rpc();
}

bool llama_supports_rpc(void) {
    return lm_ggml_backend_reg_by_name("RPC") != nullptr;
}

void llama_backend_init(void) {
    lm_ggml_time_init();

    // needed to initialize f16 tables
    {
        struct lm_ggml_init_params params = { 0, NULL, false };
        struct lm_ggml_context * ctx = lm_ggml_init(params);
        lm_ggml_free(ctx);
    }
}

void llama_numa_init(enum lm_ggml_numa_strategy numa) {
    if (numa != LM_GGML_NUMA_STRATEGY_DISABLED) {
        lm_ggml_numa_init(numa);
    }
}

void llama_attach_threadpool(
             struct llama_context * ctx,
        lm_ggml_threadpool_t   threadpool,
        lm_ggml_threadpool_t   threadpool_batch) {
    ctx->threadpool       = threadpool;
    ctx->threadpool_batch = threadpool_batch ? threadpool_batch : threadpool;
}

void llama_detach_threadpool(struct llama_context * ctx) {
    ctx->threadpool       = nullptr;
    ctx->threadpool_batch = nullptr;
}

void llama_backend_free(void) {
    lm_ggml_quantize_free();
}

int64_t llama_time_us(void) {
    return lm_ggml_time_us();
}

struct llama_model * llama_load_model_from_file(
        const char * path_model,
        struct llama_model_params   params) {
    lm_ggml_time_init();

    llama_model * model = new llama_model;

    unsigned cur_percentage = 0;
    if (params.progress_callback == NULL) {
        params.progress_callback_user_data = &cur_percentage;
        params.progress_callback = [](float progress, void * ctx) {
            unsigned * cur_percentage_p = (unsigned *) ctx;
            unsigned percentage = (unsigned) (100 * progress);
            while (percentage > *cur_percentage_p) {
                *cur_percentage_p = percentage;
                LLAMA_LOG_CONT(".");
                if (percentage >= 100) {
                    LLAMA_LOG_CONT("\n");
                }
            }
            return true;
        };
    }

    if (params.rpc_servers != nullptr && params.rpc_servers[0] != '\0') {
        // split the servers set them into model->rpc_servers
        std::string servers(params.rpc_servers);
        size_t pos = 0;
        while ((pos = servers.find(',')) != std::string::npos) {
            std::string server = servers.substr(0, pos);
            model->rpc_servers.push_back(server);
            servers.erase(0, pos + 1);
        }
        model->rpc_servers.push_back(servers);
    }

    // add RPC devices
    if (!model->rpc_servers.empty()) {
        lm_ggml_backend_reg_t rpc_reg = lm_ggml_backend_reg_by_name("RPC");
        if (!rpc_reg) {
            LLAMA_LOG_ERROR("%s: failed to find RPC backend\n", __func__);
            llama_free_model(model);
            return nullptr;
        }

        typedef lm_ggml_backend_dev_t (*lm_ggml_backend_rpc_add_device_t)(const char * endpoint);
        lm_ggml_backend_rpc_add_device_t lm_ggml_backend_rpc_add_device_fn = (lm_ggml_backend_rpc_add_device_t) lm_ggml_backend_reg_get_proc_address(rpc_reg, "lm_ggml_backend_rpc_add_device");
        if (!lm_ggml_backend_rpc_add_device_fn) {
            LLAMA_LOG_ERROR("%s: failed to find RPC device add function\n", __func__);
            llama_free_model(model);
            return nullptr;
        }

        for (const std::string & server : model->rpc_servers) {
            lm_ggml_backend_dev_t dev = lm_ggml_backend_rpc_add_device_fn(server.c_str());
            if (dev) {
                model->devices.push_back(dev);
            } else {
                LLAMA_LOG_ERROR("%s: failed to add RPC device for server '%s'\n", __func__, server.c_str());
                llama_free_model(model);
                return nullptr;
            }
        }
    }

    // create list of devices to use with this model
    // currently, we use all available devices
    // TODO: rework API to give user more control over device selection
    for (size_t i = 0; i < lm_ggml_backend_dev_count(); ++i) {
        lm_ggml_backend_dev_t dev = lm_ggml_backend_dev_get(i);
        switch (lm_ggml_backend_dev_type(dev)) {
            case LM_GGML_BACKEND_DEVICE_TYPE_CPU:
            case LM_GGML_BACKEND_DEVICE_TYPE_ACCEL:
                // skip CPU backends since they are handled separately
                break;

            case LM_GGML_BACKEND_DEVICE_TYPE_GPU:
                model->devices.push_back(dev);
                break;
        }
    }

    // if using single GPU mode, remove all except the main GPU
    if (params.split_mode == LLAMA_SPLIT_MODE_NONE) {
        if (params.main_gpu < 0 || params.main_gpu >= (int)model->devices.size()) {
            LLAMA_LOG_ERROR("%s: invalid value for main_gpu: %d (available devices: %d)\n", __func__, params.main_gpu, (int)model->devices.size());
            llama_free_model(model);
            return nullptr;
        }
        lm_ggml_backend_dev_t main_gpu = model->devices[params.main_gpu];
        model->devices.clear();
        model->devices.push_back(main_gpu);
    }

    for (auto * dev : model->devices) {
        size_t free, total; // NOLINT
        lm_ggml_backend_dev_memory(dev, &free, &total);
        LLAMA_LOG_INFO("%s: using device %s (%s) - %zu MiB free\n", __func__, lm_ggml_backend_dev_name(dev), lm_ggml_backend_dev_description(dev), free/1024/1024);
    }

    int status = llama_model_load(path_model, *model, params);
    LM_GGML_ASSERT(status <= 0);
    if (status < 0) {
        if (status == -1) {
            LLAMA_LOG_ERROR("%s: failed to load model\n", __func__);
        } else if (status == -2) {
            LLAMA_LOG_INFO("%s: cancelled model load\n", __func__);
        }
        llama_free_model(model);
        return nullptr;
    }

    return model;
}

void llama_free_model(struct llama_model * model) {
    delete model;
}

struct llama_context * llama_new_context_with_model(
                 struct llama_model * model,
        struct llama_context_params   params) {

    if (!model) {
        LLAMA_LOG_ERROR("%s: model cannot be NULL\n", __func__);
        return nullptr;
    }

    if (params.n_batch == 0 && params.n_ubatch == 0) {
        LLAMA_LOG_ERROR("%s: n_batch and n_ubatch cannot both be zero\n", __func__);
        return nullptr;
    }

    if (params.n_ctx == 0 && model->hparams.n_ctx_train == 0) {
        LLAMA_LOG_ERROR("%s: n_ctx and model->hparams.n_ctx_train cannot both be zero\n", __func__);
        return nullptr;
    }

    if (params.flash_attn && model->arch == LLM_ARCH_GROK) {
        LLAMA_LOG_WARN("%s: flash_attn is not compatible with Grok - forcing off\n", __func__);
        params.flash_attn = false;
    }

    if (params.flash_attn && model->hparams.n_embd_head_k != model->hparams.n_embd_head_v) {
        LLAMA_LOG_WARN("%s: flash_attn requires n_embd_head_k == n_embd_head_v - forcing off\n", __func__);
        params.flash_attn = false;
    }

    if (lm_ggml_is_quantized(params.type_v) && !params.flash_attn) {
        LLAMA_LOG_ERROR("%s: V cache quantization requires flash_attn\n", __func__);
        return nullptr;
    }

    llama_context * ctx = new llama_context(*model);

    const auto & hparams = model->hparams;
    auto       & cparams = ctx->cparams;

    cparams.n_seq_max        = std::max(1u, params.n_seq_max);
    cparams.n_threads        = params.n_threads;
    cparams.n_threads_batch  = params.n_threads_batch;
    cparams.yarn_ext_factor  = params.yarn_ext_factor;
    cparams.yarn_attn_factor = params.yarn_attn_factor;
    cparams.yarn_beta_fast   = params.yarn_beta_fast;
    cparams.yarn_beta_slow   = params.yarn_beta_slow;
    cparams.defrag_thold     = params.defrag_thold;
    cparams.embeddings       = params.embeddings;
    cparams.offload_kqv      = params.offload_kqv;
    cparams.flash_attn       = params.flash_attn;
    cparams.no_perf          = params.no_perf;
    cparams.pooling_type     = params.pooling_type;

    cparams.n_ctx            = params.n_ctx           == 0    ? hparams.n_ctx_train           : params.n_ctx;
    cparams.rope_freq_base   = params.rope_freq_base  == 0.0f ? hparams.rope_freq_base_train  : params.rope_freq_base;
    cparams.rope_freq_scale  = params.rope_freq_scale == 0.0f ? hparams.rope_freq_scale_train : params.rope_freq_scale;

    // this is necessary due to kv_self.n being padded later during inference
    cparams.n_ctx            = LM_GGML_PAD(cparams.n_ctx, llama_kv_cache_get_padding(cparams));

    // with causal attention, the batch size is limited by the context size
    cparams.n_batch          = hparams.causal_attn ? std::min(cparams.n_ctx, params.n_batch) : params.n_batch;

    // the batch has to be at least LM_GGML_KQ_MASK_PAD because we will be padding the KQ_mask
    // this is required by GPU kernels in order to avoid out-of-bounds accesses (e.g. lm_ggml_flash_attn_ext)
    // ref: https://github.com/ggerganov/llama.cpp/pull/5021
    if (cparams.n_batch < LM_GGML_KQ_MASK_PAD) {
        LLAMA_LOG_WARN("%s: n_batch is less than LM_GGML_KQ_MASK_PAD - increasing to %d\n", __func__, LM_GGML_KQ_MASK_PAD);
        cparams.n_batch = LM_GGML_KQ_MASK_PAD;
    }

    cparams.n_ubatch         = std::min(cparams.n_batch, params.n_ubatch == 0 ? params.n_batch : params.n_ubatch);

    cparams.n_ctx_orig_yarn  = params.yarn_orig_ctx    != 0 ? params.yarn_orig_ctx    :
                               hparams.n_ctx_orig_yarn != 0 ? hparams.n_ctx_orig_yarn :
                                                              hparams.n_ctx_train;

    cparams.cb_eval           = params.cb_eval;
    cparams.cb_eval_user_data = params.cb_eval_user_data;

    auto rope_scaling_type = params.rope_scaling_type;
    if (rope_scaling_type == LLAMA_ROPE_SCALING_TYPE_UNSPECIFIED) {
        rope_scaling_type = hparams.rope_scaling_type_train;
    }

    if (rope_scaling_type == LLAMA_ROPE_SCALING_TYPE_NONE) {
        cparams.rope_freq_scale = 1.0f; // never scale if scaling type is none
    }

    if (cparams.yarn_ext_factor < 0.0f) { // negative indicates 'not set'
        cparams.yarn_ext_factor = rope_scaling_type == LLAMA_ROPE_SCALING_TYPE_YARN ? 1.0f : 0.0f;
    }

    cparams.yarn_attn_factor *= hparams.rope_attn_factor;

    if (cparams.pooling_type == LLAMA_POOLING_TYPE_UNSPECIFIED) {
        if (hparams.pooling_type == LLAMA_POOLING_TYPE_UNSPECIFIED) {
            cparams.pooling_type = LLAMA_POOLING_TYPE_NONE;
        } else {
            cparams.pooling_type = hparams.pooling_type;
        }
    }

    if (params.attention_type == LLAMA_ATTENTION_TYPE_UNSPECIFIED) {
        cparams.causal_attn = hparams.causal_attn;
    } else {
        cparams.causal_attn = params.attention_type == LLAMA_ATTENTION_TYPE_CAUSAL;
    }

    const uint32_t n_ctx_per_seq = cparams.n_ctx / cparams.n_seq_max;

    LLAMA_LOG_INFO("%s: n_seq_max     = %u\n",   __func__, cparams.n_seq_max);
    LLAMA_LOG_INFO("%s: n_ctx         = %u\n",   __func__, cparams.n_ctx);
    LLAMA_LOG_INFO("%s: n_ctx_per_seq = %u\n",   __func__, n_ctx_per_seq);
    LLAMA_LOG_INFO("%s: n_batch       = %u\n",   __func__, cparams.n_batch);
    LLAMA_LOG_INFO("%s: n_ubatch      = %u\n",   __func__, cparams.n_ubatch);
    LLAMA_LOG_INFO("%s: flash_attn    = %d\n",   __func__, cparams.flash_attn);
    LLAMA_LOG_INFO("%s: freq_base     = %.1f\n", __func__, cparams.rope_freq_base);
    LLAMA_LOG_INFO("%s: freq_scale    = %g\n",   __func__, cparams.rope_freq_scale);

    if (n_ctx_per_seq < hparams.n_ctx_train) {
        LLAMA_LOG_WARN("%s: n_ctx_per_seq (%u) < n_ctx_train (%u) -- the full capacity of the model will not be utilized\n",
                __func__, n_ctx_per_seq, hparams.n_ctx_train);
    }

    if (n_ctx_per_seq > hparams.n_ctx_train) {
        LLAMA_LOG_WARN("%s: n_ctx_pre_seq (%u) > n_ctx_train (%u) -- possible training context overflow\n",
                __func__, n_ctx_per_seq, hparams.n_ctx_train);
    }

    ctx->abort_callback      = params.abort_callback;
    ctx->abort_callback_data = params.abort_callback_data;

    ctx->logits_all = params.logits_all;

    // build worst-case graph for encoder if a model contains encoder
    ctx->is_encoding = llama_model_has_encoder(model);

    uint32_t kv_size = cparams.n_ctx;
    lm_ggml_type type_k = params.type_k;
    lm_ggml_type type_v = params.type_v;

    // Mamba only needs a constant number of KV cache cells per sequence
    if (llama_model_is_recurrent(model)) {
        // Mamba needs at least as many KV cells as there are sequences kept at any time
        kv_size = std::max((uint32_t) 1, params.n_seq_max);
        // it's probably best to keep as much precision as possible for the states
        type_k = LM_GGML_TYPE_F32; // required by lm_ggml_ssm_conv for Mamba's conv_states
        type_v = LM_GGML_TYPE_F32; // required by lm_ggml_ssm_scan for Mamba's ssm_states
    }

    LM_GGML_ASSERT(hparams.n_embd_head_k % lm_ggml_blck_size(type_k) == 0);
    LM_GGML_ASSERT(hparams.n_embd_head_v % lm_ggml_blck_size(type_v) == 0);

    if (!hparams.vocab_only) {
        // GPU backends
        for (auto * dev : model->devices) {
            lm_ggml_backend_t backend = lm_ggml_backend_dev_init(dev, nullptr);
            if (backend == nullptr) {
                LLAMA_LOG_ERROR("%s: failed to initialize %s backend\n", __func__, lm_ggml_backend_dev_name(dev));
                llama_free(ctx);
                return nullptr;
            }
            ctx->backends.emplace_back(backend);
        }

        // add ACCEL backends (such as BLAS)
        for (size_t i = 0; i < lm_ggml_backend_dev_count(); ++i) {
            lm_ggml_backend_dev_t dev = lm_ggml_backend_dev_get(i);
            if (lm_ggml_backend_dev_type(dev) == LM_GGML_BACKEND_DEVICE_TYPE_ACCEL) {
                lm_ggml_backend_t backend = lm_ggml_backend_dev_init(dev, nullptr);
                if (backend == nullptr) {
                    LLAMA_LOG_ERROR("%s: failed to initialize %s backend\n", __func__, lm_ggml_backend_dev_name(dev));
                    llama_free(ctx);
                    return nullptr;
                }
                ctx->backends.emplace_back(backend);
            }
        }

        // add CPU backend
        ctx->backend_cpu = lm_ggml_backend_cpu_init();
        if (ctx->backend_cpu == nullptr) {
            LLAMA_LOG_ERROR("%s: failed to initialize CPU backend\n", __func__);
            llama_free(ctx);
            return nullptr;
        }
        ctx->backends.emplace_back(ctx->backend_cpu);

        // create a list of the set_n_threads functions in the backends
        for (auto & backend : ctx->backends) {
            lm_ggml_backend_dev_t dev = lm_ggml_backend_get_device(backend.get());
            lm_ggml_backend_reg_t reg = dev ? lm_ggml_backend_dev_backend_reg(dev) : nullptr;
            if (reg) {
                auto lm_ggml_backend_set_n_threads_fn = (lm_ggml_backend_set_n_threads_t) lm_ggml_backend_reg_get_proc_address(reg, "lm_ggml_backend_set_n_threads");
                if (lm_ggml_backend_set_n_threads_fn) {
                    ctx->set_n_threads_fns.emplace_back(backend.get(), lm_ggml_backend_set_n_threads_fn);
                }
            }
        }

        if (!llama_kv_cache_init(ctx->kv_self, ctx, type_k, type_v, kv_size, cparams.offload_kqv)) {
            LLAMA_LOG_ERROR("%s: llama_kv_cache_init() failed for self-attention cache\n", __func__);
            llama_free(ctx);
            return nullptr;
        }

        {
            size_t memory_size_k = 0;
            size_t memory_size_v = 0;

            for (auto & k : ctx->kv_self.k_l) {
                memory_size_k += lm_ggml_nbytes(k);
            }

            for (auto & v : ctx->kv_self.v_l) {
                memory_size_v += lm_ggml_nbytes(v);
            }

            LLAMA_LOG_INFO("%s: KV self size  = %7.2f MiB, K (%s): %7.2f MiB, V (%s): %7.2f MiB\n", __func__,
                      (float)(memory_size_k + memory_size_v) / (1024.0f * 1024.0f),
                lm_ggml_type_name(type_k), (float)memory_size_k / (1024.0f * 1024.0f),
                lm_ggml_type_name(type_v), (float)memory_size_v / (1024.0f * 1024.0f));
        }

        // graph outputs buffer
        {
            // resized during inference when a batch uses more outputs
            if (llama_output_reserve(*ctx, params.n_seq_max) < params.n_seq_max) {
                LLAMA_LOG_ERROR("%s: failed to reserve initial output buffer\n", __func__);
                llama_free(ctx);
                return nullptr;
            }

            LLAMA_LOG_INFO("%s: %10s  output buffer size = %8.2f MiB\n", __func__,
                    lm_ggml_backend_buffer_name(ctx->buf_output.get()),
                    lm_ggml_backend_buffer_get_size(ctx->buf_output.get()) / 1024.0 / 1024.0);
        }

        // scheduler and compute buffers
        {
            // buffer types used for the compute buffer of each backend
            std::vector<lm_ggml_backend_buffer_type_t> backend_buft;
            std::vector<lm_ggml_backend_t> backend_ptrs;
            for (auto & backend : ctx->backends) {
                auto * buft = lm_ggml_backend_get_default_buffer_type(backend.get());
                if (lm_ggml_backend_is_cpu(backend.get()) && !model->devices.empty()) {
                    // use the host buffer of the first device CPU for faster transfer of the intermediate state
                    auto * dev = model->devices[0];
                    auto * host_buft = lm_ggml_backend_dev_host_buffer_type(dev);
                    if (host_buft) {
                        buft = host_buft;
                    }
                }
                backend_buft.push_back(buft);
                backend_ptrs.push_back(backend.get());
            }

            const size_t max_nodes = llama_model_max_nodes(*model);

            // buffer used to store the computation graph and the tensor meta data
            ctx->buf_compute_meta.resize(lm_ggml_tensor_overhead()*max_nodes + lm_ggml_graph_overhead_custom(max_nodes, false));

            // TODO: move these checks to lm_ggml_backend_sched
            // enabling pipeline parallelism in the scheduler increases memory usage, so it is only done when necessary
            bool pipeline_parallel =
                llama_get_device_count(*model) > 1 &&
                model->n_gpu_layers > (int)model->hparams.n_layer &&
                model->split_mode == LLAMA_SPLIT_MODE_LAYER &&
                params.offload_kqv;

            // pipeline parallelism requires support for async compute and events in all devices
            if (pipeline_parallel) {
                for (auto & backend : ctx->backends) {
                    if (lm_ggml_backend_is_cpu(backend.get())) {
                        // ignore CPU backend
                        continue;
                    }
                    auto * dev = lm_ggml_backend_get_device(backend.get());
                    lm_ggml_backend_dev_props props;
                    lm_ggml_backend_dev_get_props(dev, &props);
                    if (!props.caps.async || !props.caps.events) {
                        // device does not support async compute or events
                        pipeline_parallel = false;
                        break;
                    }
                }
            }

            ctx->sched.reset(lm_ggml_backend_sched_new(backend_ptrs.data(), backend_buft.data(), backend_ptrs.size(), max_nodes, pipeline_parallel));

            if (pipeline_parallel) {
                LLAMA_LOG_INFO("%s: pipeline parallelism enabled (n_copies=%d)\n", __func__, lm_ggml_backend_sched_get_n_copies(ctx->sched.get()));
            }

            // initialize scheduler with the worst-case graph
            uint32_t n_seqs = 1; // TODO: worst-case number of sequences
            uint32_t n_tokens = std::min(cparams.n_ctx, cparams.n_ubatch);
            llama_token token = llama_token_bos(&ctx->model); // not actually used by llama_build_graph, but required to choose between token and embedding inputs graph

            llama_ubatch ubatch_pp = { true, n_tokens, n_tokens / n_seqs, n_seqs, &token, nullptr, nullptr, nullptr, nullptr, nullptr};
            lm_ggml_cgraph * gf_pp = llama_build_graph(*ctx, ubatch_pp, true);

            // reserve pp graph first so that buffers are only allocated once
            lm_ggml_backend_sched_reserve(ctx->sched.get(), gf_pp);
            int n_splits_pp = lm_ggml_backend_sched_get_n_splits(ctx->sched.get());
            int n_nodes_pp = lm_ggml_graph_n_nodes(gf_pp);

            // reserve with tg graph to get the number of splits and nodes
            llama_ubatch ubatch_tg = { true, 1, 1, n_seqs, &token, nullptr, nullptr, nullptr, nullptr, nullptr};
            lm_ggml_cgraph * gf_tg = llama_build_graph(*ctx, ubatch_tg, true);
            lm_ggml_backend_sched_reserve(ctx->sched.get(), gf_tg);
            int n_splits_tg = lm_ggml_backend_sched_get_n_splits(ctx->sched.get());
            int n_nodes_tg = lm_ggml_graph_n_nodes(gf_tg);

            // reserve again with pp graph to avoid ggml-alloc reallocations during inference
            gf_pp = llama_build_graph(*ctx, ubatch_pp, true);
            if (!lm_ggml_backend_sched_reserve(ctx->sched.get(), gf_pp)) {
                LLAMA_LOG_ERROR("%s: failed to allocate compute buffers\n", __func__);
                llama_free(ctx);
                return nullptr;
            }

            for (size_t i = 0; i < backend_ptrs.size(); ++i) {
                lm_ggml_backend_t backend = backend_ptrs[i];
                lm_ggml_backend_buffer_type_t buft = backend_buft[i];
                size_t size = lm_ggml_backend_sched_get_buffer_size(ctx->sched.get(), backend);
                if (size > 1) {
                    LLAMA_LOG_INFO("%s: %10s compute buffer size = %8.2f MiB\n", __func__,
                            lm_ggml_backend_buft_name(buft),
                            size / 1024.0 / 1024.0);
                }
            }

            if (n_nodes_pp == n_nodes_tg) {
                LLAMA_LOG_INFO("%s: graph nodes  = %d\n", __func__, n_nodes_pp);
            } else {
                LLAMA_LOG_INFO("%s: graph nodes  = %d (with bs=%d), %d (with bs=1)\n", __func__, n_nodes_pp, n_tokens, n_nodes_tg);
            }
            if (n_splits_pp == n_splits_tg) {
                LLAMA_LOG_INFO("%s: graph splits = %d\n", __func__, n_splits_pp);
            } else {
                LLAMA_LOG_INFO("%s: graph splits = %d (with bs=%d), %d (with bs=1)\n", __func__, n_splits_pp, n_tokens, n_splits_tg);
            }
        }
    }

    return ctx;
}

void llama_free(struct llama_context * ctx) {
    delete ctx;
}

uint32_t llama_n_ctx(const struct llama_context * ctx) {
    return ctx->cparams.n_ctx;
}

uint32_t llama_n_batch(const struct llama_context * ctx) {
    return ctx->cparams.n_batch;
}

uint32_t llama_n_ubatch(const struct llama_context * ctx) {
    return ctx->cparams.n_ubatch;
}

uint32_t llama_n_seq_max(const struct llama_context * ctx) {
    return ctx->kv_self.size;
}

enum llama_vocab_type llama_vocab_type(const struct llama_model * model) {
    return model->vocab.type;
}

int32_t llama_n_vocab(const struct llama_model * model) {
    return model->hparams.n_vocab;
}

int32_t llama_n_ctx_train(const struct llama_model * model) {
    return model->hparams.n_ctx_train;
}

int32_t llama_n_embd(const struct llama_model * model) {
    return model->hparams.n_embd;
}

int32_t llama_n_layer(const struct llama_model * model) {
    return model->hparams.n_layer;
}

int32_t llama_n_head(const struct llama_model * model) {
    return model->hparams.n_head();
}

const struct llama_model * llama_get_model(const struct llama_context * ctx) {
    return &ctx->model;
}

enum llama_pooling_type llama_pooling_type(const struct llama_context * ctx) {
    return ctx->cparams.pooling_type;
}

enum llama_rope_type llama_rope_type(const struct llama_model * model) {
    switch (model->arch) {
        // these models do not use RoPE
        case LLM_ARCH_GPT2:
        case LLM_ARCH_GPTJ:
        case LLM_ARCH_MPT:
        case LLM_ARCH_REFACT:
        case LLM_ARCH_BLOOM:
        case LLM_ARCH_MAMBA:
        case LLM_ARCH_JINA_BERT_V2:
        case LLM_ARCH_T5:
        case LLM_ARCH_T5ENCODER:
        case LLM_ARCH_JAIS:
        case LLM_ARCH_RWKV6:
            return LLAMA_ROPE_TYPE_NONE;

        // use what we call a normal RoPE, operating on pairs of consecutive head values
        case LLM_ARCH_LLAMA:
        case LLM_ARCH_BAICHUAN:
        case LLM_ARCH_STARCODER:
        case LLM_ARCH_PLAMO:
        case LLM_ARCH_ORION:
        case LLM_ARCH_INTERNLM2:
        case LLM_ARCH_MINICPM:
        case LLM_ARCH_XVERSE:
        case LLM_ARCH_COMMAND_R:
        case LLM_ARCH_OLMO:
        case LLM_ARCH_ARCTIC:
        case LLM_ARCH_DEEPSEEK2:
        case LLM_ARCH_CHATGLM:
        case LLM_ARCH_GRANITE:
        case LLM_ARCH_GRANITE_MOE:
        case LLM_ARCH_CHAMELEON:
            return LLAMA_ROPE_TYPE_NORM;

        // the pairs of head values are offset by n_rot/2
        case LLM_ARCH_FALCON:
        case LLM_ARCH_GROK:
        case LLM_ARCH_DBRX:
        case LLM_ARCH_BERT:
        case LLM_ARCH_NOMIC_BERT:
        case LLM_ARCH_STABLELM:
        case LLM_ARCH_BITNET:
        case LLM_ARCH_QWEN:
        case LLM_ARCH_QWEN2:
        case LLM_ARCH_QWEN2MOE:
        case LLM_ARCH_OLMO_1124:
        case LLM_ARCH_OLMOE:
        case LLM_ARCH_PHI2:
        case LLM_ARCH_PHI3:
        case LLM_ARCH_GEMMA:
        case LLM_ARCH_GEMMA2:
        case LLM_ARCH_STARCODER2:
        case LLM_ARCH_OPENELM:
        case LLM_ARCH_GPTNEOX:
        case LLM_ARCH_CODESHELL:
        case LLM_ARCH_NEMOTRON:
        case LLM_ARCH_EXAONE:
        case LLM_ARCH_MINICPM3:
            return LLAMA_ROPE_TYPE_NEOX;

        // all model arches should be listed explicitly here
        case LLM_ARCH_UNKNOWN:
            LM_GGML_ABORT("unknown architecture");
    }

    return LLAMA_ROPE_TYPE_NONE;
}

float llama_rope_freq_scale_train(const struct llama_model * model) {
    return model->hparams.rope_freq_scale_train;
}

int32_t llama_model_meta_val_str(const struct llama_model * model, const char * key, char * buf, size_t buf_size) {
    const auto & it = model->lm_gguf_kv.find(key);
    if (it == model->lm_gguf_kv.end()) {
        if (buf_size > 0) {
            buf[0] = '\0';
        }
        return -1;
    }
    return snprintf(buf, buf_size, "%s", it->second.c_str());
}

int32_t llama_model_meta_count(const struct llama_model * model) {
    return (int)model->lm_gguf_kv.size();
}

int32_t llama_model_meta_key_by_index(const struct llama_model * model, int i, char * buf, size_t buf_size) {
    if (i < 0 || i >= (int)model->lm_gguf_kv.size()) {
        if (buf_size > 0) {
            buf[0] = '\0';
        }
        return -1;
    }
    auto it = model->lm_gguf_kv.begin();
    std::advance(it, i);
    return snprintf(buf, buf_size, "%s", it->first.c_str());
}

int32_t llama_model_meta_val_str_by_index(const struct llama_model * model, int32_t i, char * buf, size_t buf_size) {
    if (i < 0 || i >= (int)model->lm_gguf_kv.size()) {
        if (buf_size > 0) {
            buf[0] = '\0';
        }
        return -1;
    }
    auto it = model->lm_gguf_kv.begin();
    std::advance(it, i);
    return snprintf(buf, buf_size, "%s", it->second.c_str());
}

int32_t llama_model_desc(const struct llama_model * model, char * buf, size_t buf_size) {
    return snprintf(buf, buf_size, "%s %s %s",
            llama_model_arch_name(model->arch),
            llama_model_type_name(model->type),
            llama_model_ftype_name(model->ftype).c_str());
}

uint64_t llama_model_size(const struct llama_model * model) {
    return model->n_bytes;
}

uint64_t llama_model_n_params(const struct llama_model * model) {
    return model->n_elements;
}

struct lm_ggml_tensor * llama_get_model_tensor(struct llama_model * model, const char * name) {
    auto it = std::find_if(model->tensors_by_name.begin(), model->tensors_by_name.end(),
            [name](const std::pair<std::string, struct lm_ggml_tensor *> & it) {
                return it.first == name;
            });
    if (it == model->tensors_by_name.end()) {
        return nullptr;
    }
    return it->second;
}

bool llama_model_has_encoder(const struct llama_model * model) {
    switch (model->arch) {
        case LLM_ARCH_T5:        return true;
        case LLM_ARCH_T5ENCODER: return true;
        default:                 return false;
    }
}

bool llama_model_has_decoder(const struct llama_model * model) {
    switch (model->arch) {
        case LLM_ARCH_T5ENCODER: return false;
        default:                 return true;
    }
}

llama_token llama_model_decoder_start_token(const struct llama_model * model) {
    return model->hparams.dec_start_token_id;
}

bool llama_model_is_recurrent(const struct llama_model * model) {
    switch (model->arch) {
        case LLM_ARCH_MAMBA:  return true;
        case LLM_ARCH_RWKV6:  return true;
        default:              return false;
    }
}

uint32_t llama_model_quantize(
        const char * fname_inp,
        const char * fname_out,
        const llama_model_quantize_params * params) {
    try {
        llama_model_quantize_internal(fname_inp, fname_out, params);
        return 0;
    } catch (const std::exception & err) {
        LLAMA_LOG_ERROR("%s: failed to quantize: %s\n", __func__, err.what());
        return 1;
    }
}

struct llama_lora_adapter * llama_lora_adapter_init(struct llama_model * model, const char * path_lora) {
    try {
        struct llama_lora_adapter * adapter = new llama_lora_adapter(model);
        llama_lora_adapter_init_internal(model, path_lora, *adapter);
        return adapter;
    } catch (const std::exception & err) {
        LLAMA_LOG_ERROR("%s: failed to apply lora adapter: %s\n", __func__, err.what());
        return nullptr;
    }
}

static bool llama_control_vector_init(struct llama_control_vector & cvec, const llama_model & model) {
    LM_GGML_ASSERT(cvec.tensors.empty());
    LM_GGML_ASSERT(cvec.ctxs.empty());
    LM_GGML_ASSERT(cvec.bufs.empty());

    // create a context for each buffer type
    std::map<lm_ggml_backend_buffer_type_t, lm_ggml_context *> ctx_map;
    auto ctx_for_buft = [&](lm_ggml_backend_buffer_type_t buft) -> lm_ggml_context * {
        auto it = ctx_map.find(buft);
        if (it == ctx_map.end()) {
            struct lm_ggml_init_params params = {
                /*.mem_size   =*/ model.hparams.n_layer*lm_ggml_tensor_overhead(),
                /*.mem_buffer =*/ NULL,
                /*.no_alloc   =*/ true,
            };
            lm_ggml_context * ctx = lm_ggml_init(params);
            if (!ctx) {
                return nullptr;
            }
            ctx_map[buft] = ctx;
            cvec.ctxs.emplace_back(ctx);
            return ctx;
        }
        return it->second;
    };

    // make tensors
    cvec.tensors.reserve(model.hparams.n_layer);
    cvec.tensors.push_back(nullptr); // there's never a tensor for layer 0
    for (size_t il = 1; il < model.hparams.n_layer; il++) {
        lm_ggml_backend_buffer_type_t buft = select_buft(*model.dev_layer.at(il).buft_list,
            [&](lm_ggml_context * ctx) {
                lm_ggml_tensor * cur = lm_ggml_new_tensor_1d(ctx, LM_GGML_TYPE_F32, model.hparams.n_embd);
                lm_ggml_tensor * layer_dir = lm_ggml_new_tensor_1d(ctx, LM_GGML_TYPE_F32, model.hparams.n_embd);
                return lm_ggml_add(ctx, cur, layer_dir);
            });
        lm_ggml_context * ctx = ctx_for_buft(buft);
        if (!ctx) {
            LLAMA_LOG_ERROR("%s: failed to allocate context for control vector\n", __func__);
            return false;
        }
        lm_ggml_tensor * tensor = lm_ggml_new_tensor_1d(ctx, LM_GGML_TYPE_F32, model.hparams.n_embd);
        cvec.tensors.push_back(tensor);
    }

    // allocate tensors / buffers and zero
    cvec.bufs.reserve(ctx_map.size());
    for (auto it : ctx_map) {
        lm_ggml_backend_buffer_type_t buft = it.first;
        lm_ggml_context * ctx = it.second;
        lm_ggml_backend_buffer_t buf = lm_ggml_backend_alloc_ctx_tensors_from_buft(ctx, buft);
        if (!buf) {
            LLAMA_LOG_ERROR("%s: failed to allocate buffer for control vector\n", __func__);
            return false;
        }
        lm_ggml_backend_buffer_clear(buf, 0);
        cvec.bufs.emplace_back(buf);
    }

    return true;
}

int32_t llama_control_vector_apply(struct llama_context * lctx, const float * data, size_t len, int32_t n_embd, int32_t il_start, int32_t il_end) {
    const llama_model & model = lctx->model;
    llama_control_vector & cvec = lctx->cvec;

    if (data == nullptr) {
        // disable the current control vector (but leave allocated for later)
        cvec.layer_start = -1;
        cvec.layer_end   = -1;
        return 0;
    }

    if (n_embd != (int) model.hparams.n_embd) {
        LLAMA_LOG_ERROR("%s: control vector n_embd does not match model\n", __func__);
        return 1;
    }

    if (cvec.tensors.empty()) {
        if (!llama_control_vector_init(cvec, model)) {
            return 1;
        }
    }

    cvec.layer_start = il_start;
    cvec.layer_end   = il_end;

    for (size_t il = 1; il < model.hparams.n_layer; il++) {
        assert(cvec.tensors[il] != nullptr);

        const size_t off = n_embd * (il - 1); // buffer doesn't have data for layer 0, since it's never present
        if (off + n_embd <= len) {
            lm_ggml_backend_tensor_set(cvec.tensors[il], data + off, 0, n_embd * lm_ggml_element_size(cvec.tensors[il]));
        }
    }

    return 0;
}

struct llama_kv_cache_view llama_kv_cache_view_init(const struct llama_context * ctx, int32_t n_seq_max) {
    struct llama_kv_cache_view result = {
        /*.n_cells            = */ 0,
        /*.n_seq_max          = */ n_seq_max,
        /*.token_count        = */ 0,
        /*.used_cells         = */ llama_get_kv_cache_used_cells(ctx),
        /*.max_contiguous     = */ 0,
        /*.max_contiguous_idx = */ -1,
        /*.cells              = */ nullptr,
        /*.cells_sequences    = */ nullptr,
    };
    return result;
}

void llama_kv_cache_view_free(struct llama_kv_cache_view * view) {
    if (view->cells != nullptr) {
        free(view->cells);
        view->cells = nullptr;
    }
    if (view->cells_sequences != nullptr) {
        free(view->cells_sequences);
        view->cells_sequences = nullptr;
    }
}

void llama_kv_cache_view_update(const struct llama_context * ctx, struct llama_kv_cache_view * view) {
    if (uint32_t(view->n_cells) < ctx->kv_self.size || view->cells == nullptr) {
        view->n_cells = int32_t(ctx->kv_self.size);
        void * p = realloc(view->cells, sizeof(struct llama_kv_cache_view_cell) * view->n_cells);
        LM_GGML_ASSERT(p != nullptr && "Failed to alloc kv_cache_view cells");
        view->cells = (struct llama_kv_cache_view_cell *)p;
        p = realloc(view->cells_sequences, sizeof(llama_seq_id) * view->n_seq_max * view->n_cells);
        LM_GGML_ASSERT(p != nullptr && "Failed to alloc kv_cache_view cells sequences");
        view->cells_sequences = (llama_seq_id *)p;
    }

    const std::vector<llama_kv_cell> & kv_cells = ctx->kv_self.cells;
    llama_kv_cache_view_cell * c_curr = view->cells;
    llama_seq_id * cs_curr = view->cells_sequences;
    int32_t used_cells = 0;
    int32_t token_count = 0;
    int32_t curr_contig_idx = -1;
    uint32_t max_contig = 0;
    int32_t max_contig_idx = -1;

    for (int32_t i = 0; i < int32_t(ctx->kv_self.size); i++, c_curr++, cs_curr += view->n_seq_max) {
        const size_t curr_size = kv_cells[i].seq_id.size();
        token_count += curr_size;
        c_curr->pos = kv_cells[i].pos + kv_cells[i].delta;

        if (curr_size > 0) {
            if (curr_contig_idx >= 0 && uint32_t(i - curr_contig_idx) > max_contig) {
                max_contig = i - curr_contig_idx;
                max_contig_idx = curr_contig_idx;
            }
            curr_contig_idx = -1;
        } else if (curr_contig_idx < 0) {
            curr_contig_idx = i;
        }

        int seq_idx = 0;
        for (const llama_seq_id it : kv_cells[i].seq_id) {
            if (seq_idx >= view->n_seq_max) {
                break;
            }
            cs_curr[seq_idx] = it;
            seq_idx++;
        }
        if (seq_idx != 0) {
            used_cells++;
        }
        for (; seq_idx < view->n_seq_max; seq_idx++) {
            cs_curr[seq_idx] = -1;
        }
    }
    if (curr_contig_idx >= 0 && kv_cells.size() - curr_contig_idx > max_contig) {
        max_contig_idx = curr_contig_idx;
        max_contig = kv_cells.size() - curr_contig_idx;
    }
    view->max_contiguous = max_contig;
    view->max_contiguous_idx = max_contig_idx;
    view->token_count = token_count;
    view->used_cells = used_cells;
    if (uint32_t(used_cells) != ctx->kv_self.used) {
        LLAMA_LOG_ERROR("%s: used cells mismatch. kv_cache says %d but we calculated %d\n",
            __func__, ctx->kv_self.used, used_cells);
    }
}

int32_t llama_get_kv_cache_token_count(const struct llama_context * ctx) {
    int result = 0;

    for (uint32_t i = 0; i < ctx->kv_self.size; i++) {
        result += ctx->kv_self.cells[i].seq_id.size();
    }

    return result;
}

int32_t llama_get_kv_cache_used_cells(const struct llama_context * ctx) {
    return ctx->kv_self.used;
}

void llama_kv_cache_clear(struct llama_context * ctx) {
    llama_kv_cache_clear(ctx->kv_self);
}

bool llama_kv_cache_seq_rm(struct llama_context * ctx, llama_seq_id seq_id, llama_pos p0, llama_pos p1) {
    return llama_kv_cache_seq_rm(ctx->kv_self, seq_id, p0, p1);
}

void llama_kv_cache_seq_cp(struct llama_context * ctx, llama_seq_id seq_id_src, llama_seq_id seq_id_dst, llama_pos p0, llama_pos p1) {
    if (seq_id_src == seq_id_dst) {
        return;
    }
    llama_kv_cache_seq_cp(ctx->kv_self, seq_id_src, seq_id_dst, p0, p1);
}

void llama_kv_cache_seq_keep(struct llama_context * ctx, llama_seq_id seq_id) {
    llama_kv_cache_seq_keep(ctx->kv_self, seq_id);
}

void llama_kv_cache_seq_add(struct llama_context * ctx, llama_seq_id seq_id, llama_pos p0, llama_pos p1, llama_pos delta) {
    if (delta == 0) {
        return;
    }

    llama_kv_cache_seq_add(ctx->kv_self, seq_id, p0, p1, delta);
}

void llama_kv_cache_seq_div(struct llama_context * ctx, llama_seq_id seq_id, llama_pos p0, llama_pos p1, int d) {
    if (d == 1) {
        return;
    }

    llama_kv_cache_seq_div(ctx->kv_self, seq_id, p0, p1, d);
}

llama_pos llama_kv_cache_seq_pos_max(struct llama_context * ctx, llama_seq_id seq_id) {
    return llama_kv_cache_seq_pos_max(ctx->kv_self, seq_id);
}

void llama_kv_cache_defrag(struct llama_context * ctx) {
    llama_kv_cache_defrag(ctx->kv_self);
}

void llama_kv_cache_update(struct llama_context * ctx) {
    llama_kv_cache_update_internal(*ctx);
}

bool llama_kv_cache_can_shift(struct llama_context * ctx) {
    return !ctx->kv_self.recurrent && ctx->model.arch != LLM_ARCH_DEEPSEEK2; // not supported due to MLA
}

// deprecated
size_t llama_get_state_size(struct llama_context * ctx) {
    return llama_state_get_size(ctx);
}

// deprecated
size_t llama_copy_state_data(struct llama_context * ctx, uint8_t * dst) {
    return llama_state_get_data(ctx, dst, -1);
}

// deprecated
size_t llama_set_state_data(struct llama_context * ctx, const uint8_t * src) {
    return llama_state_set_data(ctx, src, -1);
}

// deprecated
bool llama_load_session_file(struct llama_context * ctx, const char * path_session, llama_token * tokens_out, size_t n_token_capacity, size_t * n_token_count_out) {
    return llama_state_load_file(ctx, path_session, tokens_out, n_token_capacity, n_token_count_out);
}

// deprecated
bool llama_save_session_file(struct llama_context * ctx, const char * path_session, const llama_token * tokens, size_t n_token_count) {
    return llama_state_save_file(ctx, path_session, tokens, n_token_count);
}

// TODO: replace all non-fatal assertions with returned errors or exceptions
struct llama_data_write {
    virtual void write(const void * src, size_t size) = 0;
    virtual void write_tensor_data(const struct lm_ggml_tensor * tensor, size_t offset, size_t size) = 0;
    virtual size_t get_size_written() = 0;
    virtual ~llama_data_write() = default;

    void write_string(const std::string & str) {
        uint32_t str_size = str.size();

        write(&str_size,  sizeof(str_size));
        write(str.data(), str_size);
    }

    void write_model_info(const struct llama_context * ctx) {
        std::string arch_str = LLM_ARCH_NAMES.at(ctx->model.arch);
        write_string(arch_str);
        // TODO: add more model-specific info which should prevent loading the session file if not identical
    }

    //void write_rng(const std::mt19937 & rng) {
    //    std::ostringstream rng_ss;
    //    rng_ss << rng;

    //    const std::string & rng_str = rng_ss.str();

    //    write_string(rng_str);
    //}

    void write_output_ids(struct llama_context * ctx) {
        llama_output_reorder(ctx);

        const uint32_t n_outputs = ctx->n_outputs;

        std::vector<int32_t> output_pos;

        const size_t    n_batch = ctx->cparams.n_batch;
        const auto & output_ids = ctx->output_ids;

        LM_GGML_ASSERT(n_outputs <= ctx->output_size);

        output_pos.resize(n_outputs);

        // build a more compact representation of the output ids
        for (size_t i = 0; i < n_batch; ++i) {
            // map an output id to a position in the batch
            int32_t pos = output_ids[i];
            if (pos >= 0) {
                LM_GGML_ASSERT((uint32_t) pos < n_outputs);
                output_pos[pos] = i;
            }
        }

        write(&n_outputs, sizeof(n_outputs));

        if (n_outputs) {
            write(output_pos.data(), n_outputs * sizeof(int32_t));
        }
    }

    void write_logits(const struct llama_context * ctx) {
        const uint64_t logits_size = std::min((uint64_t) ctx->logits_size, (uint64_t) ctx->n_outputs * ctx->model.hparams.n_vocab);

        write(&logits_size, sizeof(logits_size));

        if (logits_size) {
            write(ctx->logits, logits_size * sizeof(float));
        }
    }

    void write_embeddings(const struct llama_context * ctx) {
        const uint64_t embeddings_size = std::min((uint64_t) ctx->embd_size, (uint64_t) ctx->n_outputs * ctx->model.hparams.n_embd);

        write(&embeddings_size, sizeof(embeddings_size));

        if (embeddings_size) {
            write(ctx->embd, embeddings_size * sizeof(float));
        }
    }

    void write_kv_cache_meta(const llama_kv_cache & kv_self, const std::vector<std::pair<uint32_t, uint32_t>> & cell_ranges, llama_seq_id seq_id = -1) {

        for (const auto & range : cell_ranges) {
            for (uint32_t i = range.first; i < range.second; ++i) {
                const auto & cell = kv_self.cells[i];
                const llama_pos pos      = cell.pos;
                const uint32_t  n_seq_id = seq_id == -1 ? cell.seq_id.size() : 0;

                write(&pos,      sizeof(pos));
                write(&n_seq_id, sizeof(n_seq_id));

                if (n_seq_id) {
                    for (auto seq_id : cell.seq_id) {
                        write(&seq_id, sizeof(seq_id));
                    }
                }
            }
        }
    }

    void write_kv_cache_data(const struct llama_context * ctx, const std::vector<std::pair<uint32_t, uint32_t>> & cell_ranges) {
        const struct llama_kv_cache & kv_self = ctx->kv_self;
        const struct llama_hparams & hparams = ctx->model.hparams;

        const uint32_t v_trans = kv_self.v_trans ? 1 : 0;
        const uint32_t n_layer = hparams.n_layer;

        write(&v_trans, sizeof(v_trans));
        write(&n_layer, sizeof(n_layer));

        std::vector<uint8_t> tmp_buf;

        // Iterate and write all the keys first, each row is a cell
        // Get whole range at a time
        for (uint32_t il = 0; il < n_layer; ++il) {
            const uint32_t n_embd_k_gqa = hparams.n_embd_k_gqa(il) + hparams.n_embd_k_s();

            // Write key type
            const int32_t k_type_i = (int32_t)kv_self.k_l[il]->type;
            write(&k_type_i, sizeof(k_type_i));

            // Write row size of key
            const uint64_t k_size_row = lm_ggml_row_size(kv_self.k_l[il]->type, n_embd_k_gqa);
            write(&k_size_row, sizeof(k_size_row));

            // Read each range of cells of k_size length each into tmp_buf and write out
            for (const auto & range : cell_ranges) {
                const size_t range_size = range.second - range.first;
                const size_t buf_size = range_size * k_size_row;
                write_tensor_data(kv_self.k_l[il], range.first * k_size_row, buf_size);
            }
        }

        if (!kv_self.v_trans) {
            for (uint32_t il = 0; il < n_layer; ++il) {
                const uint32_t n_embd_v_gqa = hparams.n_embd_v_gqa(il) + hparams.n_embd_v_s();

                // Write value type
                const int32_t v_type_i = (int32_t)kv_self.v_l[il]->type;
                write(&v_type_i, sizeof(v_type_i));

                // Write row size of value
                const uint64_t v_size_row = lm_ggml_row_size(kv_self.v_l[il]->type, n_embd_v_gqa);
                write(&v_size_row, sizeof(v_size_row));

                // Read each range of cells of v_size length each into tmp_buf and write out
                for (const auto & range : cell_ranges) {
                    const size_t range_size = range.second - range.first;
                    const size_t buf_size = range_size * v_size_row;
                    write_tensor_data(kv_self.v_l[il], range.first * v_size_row, buf_size);
                }
            }
        } else {
            // When v is transposed, we also need the element size and get the element ranges from each row
            const uint32_t kv_size = kv_self.size;
            for (uint32_t il = 0; il < n_layer; ++il) {
                const uint32_t n_embd_v_gqa = hparams.n_embd_v_gqa(il) + hparams.n_embd_v_s();

                // Write value type
                const int32_t v_type_i = (int32_t)kv_self.v_l[il]->type;
                write(&v_type_i, sizeof(v_type_i));

                // Write element size
                const uint32_t v_size_el = lm_ggml_type_size(kv_self.v_l[il]->type);
                write(&v_size_el, sizeof(v_size_el));

                // Write GQA embedding size
                write(&n_embd_v_gqa, sizeof(n_embd_v_gqa));

                // For each row, we get the element values of each cell
                for (uint32_t j = 0; j < n_embd_v_gqa; ++j) {
                    // Read each range of cells of v_size_el length each into tmp_buf and write out
                    for (const auto & range : cell_ranges) {
                        const size_t range_size = range.second - range.first;
                        const size_t src_offset = (range.first + j * kv_size) * v_size_el;
                        const size_t buf_size = range_size * v_size_el;
                        write_tensor_data(kv_self.v_l[il], src_offset, buf_size);
                    }
                }
            }
        }
    }

    void write_kv_cache(const struct llama_context * ctx, llama_seq_id seq_id = -1) {
        const struct llama_kv_cache & kv_self = ctx->kv_self;
        std::vector<std::pair<uint32_t, uint32_t>> cell_ranges; // ranges, from inclusive, to exclusive
        uint32_t cell_count = 0;

        // Count the number of cells with the specified seq_id
        // Find all the ranges of cells with this seq id (or all, when -1)
        uint32_t cell_range_begin = kv_self.size;
        for (uint32_t i = 0; i < kv_self.size; ++i) {
            const auto & cell = kv_self.cells[i];
            if ((seq_id == -1 && !cell.is_empty()) || cell.has_seq_id(seq_id)) {
                ++cell_count;
                if (cell_range_begin == kv_self.size) {
                    cell_range_begin = i;
                }
            } else {
                if (cell_range_begin != kv_self.size) {
                    cell_ranges.emplace_back(cell_range_begin, i);
                    cell_range_begin = kv_self.size;
                }
            }
        }
        if (cell_range_begin != kv_self.size) {
            cell_ranges.emplace_back(cell_range_begin, kv_self.size);
        }

        // DEBUG CHECK: Sum of cell counts in ranges should equal the total cell count
        uint32_t cell_count_check = 0;
        for (const auto & range : cell_ranges) {
            cell_count_check += range.second - range.first;
        }
        LM_GGML_ASSERT(cell_count == cell_count_check);

        write(&cell_count, sizeof(cell_count));

        write_kv_cache_meta(kv_self, cell_ranges, seq_id);
        write_kv_cache_data(ctx, cell_ranges);
    }
};

struct llama_data_read {
    virtual const uint8_t * read(size_t size) = 0;
    virtual void read_to(void * dst, size_t size) = 0;
    virtual size_t get_size_read() = 0;
    virtual ~llama_data_read() = default;

    void read_string(std::string & str) {
        uint32_t str_size;
        read_to(&str_size, sizeof(str_size));

        str.assign((const char *) read(str_size), str_size);
    }

    // validate model information
    void read_model_info(const struct llama_context * ctx) {
        std::string cur_arch_str = LLM_ARCH_NAMES.at(ctx->model.arch);
        std::string arch_str;
        read_string(arch_str);
        if (cur_arch_str != arch_str) {
            throw std::runtime_error(format("wrong model arch: '%s' instead of '%s'", arch_str.c_str(), cur_arch_str.c_str()));
        }
        // TODO: add more info which needs to be identical but which is not verified otherwise
    }

    //void read_rng(std::mt19937 & rng) {
    //    std::string rng_str;
    //    read_string(rng_str);

    //    std::istringstream rng_ss(rng_str);
    //    rng_ss >> rng;

    //    if (rng_ss.fail()) {
    //        throw std::runtime_error("failed to load RNG state");
    //    }
    //}

    void read_output_ids(struct llama_context * ctx) {
        std::vector<int32_t> output_pos;

        uint32_t n_outputs;
        read_to(&n_outputs, sizeof(n_outputs));

        if (n_outputs > llama_output_reserve(*ctx, n_outputs)) {
            throw std::runtime_error("could not reserve outputs");
        }

        if (n_outputs) {
            output_pos.resize(n_outputs);
            read_to(output_pos.data(), n_outputs * sizeof(int32_t));

            for (int32_t i = 0; i < (int32_t) output_pos.size(); ++i) {
                int32_t id = output_pos[i];
                if ((uint32_t) id >= ctx->cparams.n_batch) {
                    throw std::runtime_error(format("invalid output id, %d does not fit in batch size of %u", id, ctx->cparams.n_batch));
                }
                ctx->output_ids[id] = i;
            }

            ctx->n_outputs = n_outputs;
        }
    }

    void read_logits(struct llama_context * ctx) {
        uint64_t logits_size;
        read_to(&logits_size, sizeof(logits_size));

        if (ctx->logits_size < logits_size) {
            throw std::runtime_error("logits buffer too small");
        }

        if (logits_size) {
            read_to(ctx->logits, logits_size * sizeof(float));
        }
    }

    void read_embeddings(struct llama_context * ctx) {
        uint64_t embeddings_size;
        read_to(&embeddings_size, sizeof(embeddings_size));

        if (ctx->embd_size < embeddings_size) {
            throw std::runtime_error("embeddings buffer too small");
        }

        if (embeddings_size) {
            read_to(ctx->embd, embeddings_size * sizeof(float));
        }
    }

    bool read_kv_cache_meta(struct llama_context * ctx, uint32_t cell_count, llama_seq_id dest_seq_id = -1) {
        struct llama_kv_cache & kv_self = ctx->kv_self;

        if (dest_seq_id != -1) {
            // single sequence

            llama_kv_cache_seq_rm(kv_self, dest_seq_id, -1, -1);

            llama_ubatch batch = ctx->sbatch.reserve_ubatch(cell_count, /* has_embd */ false);
            batch.n_tokens = cell_count;
            batch.n_seq_tokens = cell_count;
            batch.n_seqs = 1;

            for (uint32_t i = 0; i < cell_count; ++i) {
                llama_pos pos;
                uint32_t n_seq_id;

                read_to(&pos, sizeof(pos));
                read_to(&n_seq_id, sizeof(n_seq_id));

                if (n_seq_id != 0) {
                    LLAMA_LOG_ERROR("%s: invalid seq_id-agnostic kv cell\n", __func__);
                    return false;
                }

                batch.pos[i] = pos;
            }
            batch.n_seq_id[0] = 1;
            batch.seq_id[0] = &dest_seq_id;
            if (!llama_kv_cache_find_slot(kv_self, batch)) {
                LLAMA_LOG_ERROR("%s: failed to find available cells in kv cache\n", __func__);
                return false;
            }

            // DEBUG CHECK: kv_self.head should be our first cell, kv_self.head + cell_count - 1 should be our last cell (verify seq_id and pos values)
            // Assume that this is one contiguous block of cells
            LM_GGML_ASSERT(kv_self.head + cell_count <= kv_self.size);
            LM_GGML_ASSERT(kv_self.cells[kv_self.head].pos == batch.pos[0]);
            LM_GGML_ASSERT(kv_self.cells[kv_self.head + cell_count - 1].pos == batch.pos[cell_count - 1]);
            LM_GGML_ASSERT(kv_self.cells[kv_self.head].has_seq_id(dest_seq_id));
            LM_GGML_ASSERT(kv_self.cells[kv_self.head + cell_count - 1].has_seq_id(dest_seq_id));
        } else {
            // whole KV cache restore

            if (cell_count > kv_self.size) {
                LLAMA_LOG_ERROR("%s: not enough cells in kv cache\n", __func__);
                return false;
            }

            llama_kv_cache_clear(kv_self);

            for (uint32_t i = 0; i < cell_count; ++i) {
                llama_kv_cell & cell = kv_self.cells[i];

                llama_pos pos;
                uint32_t  n_seq_id;

                read_to(&pos,      sizeof(pos));
                read_to(&n_seq_id, sizeof(n_seq_id));

                cell.pos = pos;

                for (uint32_t j = 0; j < n_seq_id; ++j) {
                    llama_seq_id seq_id;
                    read_to(&seq_id, sizeof(seq_id));

                    if (seq_id < 0 || (uint32_t) seq_id >= llama_n_seq_max(ctx)) {
                        LLAMA_LOG_ERROR("%s: invalid seq_id, %d is out of range [0, %u)\n", __func__, seq_id, llama_n_seq_max(ctx));
                        return false;
                    }

                    cell.seq_id.insert(seq_id);

                    if (kv_self.recurrent) {
                        int32_t & tail = kv_self.cells[seq_id].tail;
                        if (tail != -1) {
                            LLAMA_LOG_ERROR("%s: duplicate tail for seq_id %d in cell %d and %d\n", __func__, seq_id, i, tail);
                            return false;
                        }
                        tail = i;
                    }
                }
            }

            kv_self.head = 0;
            kv_self.used = cell_count;
        }

        if (kv_self.recurrent) {
            for (uint32_t i = 0; i < cell_count; ++i) {
                uint32_t cell_id = kv_self.head + i;
                // make sure the recurrent states will keep their restored state
                kv_self.cells[cell_id].src = cell_id;
            }
        }

        return true;
    }

    bool read_kv_cache_data(struct llama_context * ctx, uint32_t cell_count) {
        const struct llama_hparams & hparams = ctx->model.hparams;
        struct llama_kv_cache & kv_self = ctx->kv_self;
        uint32_t v_trans;
        uint32_t n_layer;
        read_to(&v_trans, sizeof(v_trans));
        read_to(&n_layer, sizeof(n_layer));

        if (n_layer != hparams.n_layer) {
            LLAMA_LOG_ERROR("%s: mismatched layer count (%u instead of %u)\n", __func__, n_layer, hparams.n_layer);
            return false;
        }
        if (cell_count > kv_self.size) {
            LLAMA_LOG_ERROR("%s: not enough cells in kv cache to restore state (%u > %u)\n", __func__, cell_count, kv_self.size);
            return false;
        }
        if (kv_self.v_trans != (bool) v_trans) {
            LLAMA_LOG_ERROR("%s: incompatible V transposition\n", __func__);
            return false;
        }

        // For each layer, read the keys for each cell, one row is one cell, read as one contiguous block
        for (uint32_t il = 0; il < n_layer; ++il) {
            const uint32_t n_embd_k_gqa = hparams.n_embd_k_gqa(il) + hparams.n_embd_k_s();

            // Read type of key
            int32_t k_type_i_ref;
            read_to(&k_type_i_ref, sizeof(k_type_i_ref));
            const int32_t k_type_i = (int32_t)kv_self.k_l[il]->type;
            if (k_type_i != k_type_i_ref) {
                LLAMA_LOG_ERROR("%s: mismatched key type (%d != %d, layer %d)\n", __func__, k_type_i, k_type_i_ref, il);
                return false;
            }

            // Read row size of key
            uint64_t k_size_row_ref;
            read_to(&k_size_row_ref, sizeof(k_size_row_ref));
            const size_t k_size_row = lm_ggml_row_size(kv_self.k_l[il]->type, n_embd_k_gqa);
            if (k_size_row != k_size_row_ref) {
                LLAMA_LOG_ERROR("%s: mismatched key row size (%zu != %zu, layer %d)\n", __func__, k_size_row, (size_t) k_size_row_ref, il);
                return false;
            }

            if (cell_count) {
                // Read and set the keys for the whole cell range
                lm_ggml_backend_tensor_set(kv_self.k_l[il], read(cell_count * k_size_row), kv_self.head * k_size_row, cell_count * k_size_row);
            }
        }

        if (!kv_self.v_trans) {
            for (uint32_t il = 0; il < n_layer; ++il) {
                const uint32_t n_embd_v_gqa = hparams.n_embd_v_gqa(il) + hparams.n_embd_v_s();

                // Read type of value
                int32_t v_type_i_ref;
                read_to(&v_type_i_ref, sizeof(v_type_i_ref));
                const int32_t v_type_i = (int32_t)kv_self.v_l[il]->type;
                if (v_type_i != v_type_i_ref) {
                    LLAMA_LOG_ERROR("%s: mismatched value type (%d != %d, layer %d)\n", __func__, v_type_i, v_type_i_ref, il);
                    return false;
                }

                // Read row size of value
                uint64_t v_size_row_ref;
                read_to(&v_size_row_ref, sizeof(v_size_row_ref));
                const size_t v_size_row = lm_ggml_row_size(kv_self.v_l[il]->type, n_embd_v_gqa);
                if (v_size_row != v_size_row_ref) {
                    LLAMA_LOG_ERROR("%s: mismatched value row size (%zu != %zu, layer %d)\n", __func__, v_size_row, (size_t) v_size_row_ref, il);
                    return false;
                }

                if (cell_count) {
                    // Read and set the values for the whole cell range
                    lm_ggml_backend_tensor_set(kv_self.v_l[il], read(cell_count * v_size_row), kv_self.head * v_size_row, cell_count * v_size_row);
                }
            }
        } else {
            // For each layer, read the values for each cell (transposed)
            for (uint32_t il = 0; il < n_layer; ++il) {
                const uint32_t n_embd_v_gqa = hparams.n_embd_v_gqa(il) + hparams.n_embd_v_s();

                // Read type of value
                int32_t v_type_i_ref;
                read_to(&v_type_i_ref, sizeof(v_type_i_ref));
                const int32_t v_type_i = (int32_t)kv_self.v_l[il]->type;
                if (v_type_i != v_type_i_ref) {
                    LLAMA_LOG_ERROR("%s: mismatched value type (%d != %d, layer %d)\n", __func__, v_type_i, v_type_i_ref, il);
                    return false;
                }

                // Read element size of value
                uint32_t v_size_el_ref;
                read_to(&v_size_el_ref, sizeof(v_size_el_ref));
                const size_t v_size_el = lm_ggml_type_size(kv_self.v_l[il]->type);
                if (v_size_el != v_size_el_ref) {
                    LLAMA_LOG_ERROR("%s: mismatched value element size (%zu != %zu, layer %d)\n", __func__, v_size_el, (size_t) v_size_el_ref, il);
                    return false;
                }

                // Read GQA embedding size
                uint32_t n_embd_v_gqa_ref;
                read_to(&n_embd_v_gqa_ref, sizeof(n_embd_v_gqa_ref));
                if (n_embd_v_gqa != n_embd_v_gqa_ref) {
                    LLAMA_LOG_ERROR("%s: mismatched GQA embedding size (%u != %u, layer %d)\n", __func__, n_embd_v_gqa, n_embd_v_gqa_ref, il);
                    return false;
                }

                if (cell_count) {
                    // For each row in the transposed matrix, read the values for the whole cell range
                    for (uint32_t j = 0; j < n_embd_v_gqa; ++j) {
                        const size_t dst_offset = (kv_self.head + j * kv_self.size) * v_size_el;
                        lm_ggml_backend_tensor_set(kv_self.v_l[il], read(cell_count * v_size_el), dst_offset, cell_count * v_size_el);
                    }
                }
            }
        }
        return true;
    }

    void read_kv_cache(struct llama_context * ctx, llama_seq_id seq_id = -1) {
        uint32_t cell_count;
        read_to(&cell_count, sizeof(cell_count));

        bool res = read_kv_cache_meta(ctx, cell_count, seq_id) && read_kv_cache_data(ctx, cell_count);

        if (!res) {
            if (seq_id == -1) {
                llama_kv_cache_clear(ctx);
            } else {
                llama_kv_cache_seq_rm(ctx, seq_id, -1, -1);
            }
            throw std::runtime_error("failed to restore kv cache");
        }
    }
};

struct llama_data_write_dummy : llama_data_write {
    size_t size_written = 0;

    llama_data_write_dummy() {}

    void write(const void * /* src */, size_t size) override {
        size_written += size;
    }

    void write_tensor_data(const struct lm_ggml_tensor * /* tensor */, size_t /* offset */, size_t size) override {
        size_written += size;
    }

    size_t get_size_written() override {
        return size_written;
    }
};

struct llama_data_write_buffer : llama_data_write {
    uint8_t * ptr;
    size_t buf_size = 0;
    size_t size_written = 0;

    llama_data_write_buffer(uint8_t * p, size_t len) : ptr(p), buf_size(len) {}

    void write(const void * src, size_t size) override {
        if (size > buf_size) {
            throw std::runtime_error("unexpectedly reached end of buffer");
        }
        memcpy(ptr, src, size);
        ptr += size;
        size_written += size;
        buf_size -= size;
    }

    void write_tensor_data(const struct lm_ggml_tensor * tensor, size_t offset, size_t size) override {
        if (size > buf_size) {
            throw std::runtime_error("unexpectedly reached end of buffer");
        }
        lm_ggml_backend_tensor_get(tensor, ptr, offset, size);
        ptr += size;
        size_written += size;
        buf_size -= size;
    }

    size_t get_size_written() override {
        return size_written;
    }
};

struct llama_data_read_buffer : llama_data_read {
    const uint8_t * ptr;
    size_t buf_size = 0;
    size_t size_read = 0;

    llama_data_read_buffer(const uint8_t * p, size_t len) : ptr(p), buf_size(len) {}

    const uint8_t * read(size_t size) override {
        const uint8_t * base_ptr = ptr;
        if (size > buf_size) {
            throw std::runtime_error("unexpectedly reached end of buffer");
        }
        ptr += size;
        size_read += size;
        buf_size -= size;
        return base_ptr;
    }

    void read_to(void * dst, size_t size) override {
        memcpy(dst, read(size), size);
    }

    size_t get_size_read() override {
        return size_read;
    }
};

struct llama_data_write_file : llama_data_write {
    llama_file * file;
    size_t size_written = 0;
    std::vector<uint8_t> temp_buffer;

    llama_data_write_file(llama_file * f) : file(f) {}

    void write(const void * src, size_t size) override {
        file->write_raw(src, size);
        size_written += size;
    }

    void write_tensor_data(const struct lm_ggml_tensor * tensor, size_t offset, size_t size) override {
        temp_buffer.resize(size);
        lm_ggml_backend_tensor_get(tensor, temp_buffer.data(), offset, size);
        write(temp_buffer.data(), temp_buffer.size());
    }

    size_t get_size_written() override {
        return size_written;
    }
};

struct llama_data_read_file : llama_data_read {
    llama_file * file;
    size_t size_read = 0;
    std::vector<uint8_t> temp_buffer;

    llama_data_read_file(llama_file * f) : file(f) {}

    void read_to(void * dst, size_t size) override {
        file->read_raw(dst, size);
        size_read += size;
    }

    const uint8_t * read(size_t size) override {
        temp_buffer.resize(size);
        read_to(temp_buffer.data(), size);
        return temp_buffer.data();
    }

    size_t get_size_read() override {
        return size_read;
    }
};

/** copy state data into either a buffer or file depending on the passed in context
 *
 * file context:
 * llama_file file("/path", "wb");
 * llama_data_write_file data_ctx(&file);
 * llama_state_get_data_internal(ctx, data_ctx);
 *
 * buffer context:
 * std::vector<uint8_t> buf(max_size, 0);
 * llama_data_write_buffer data_ctx(buf.data(), max_size);
 * llama_state_get_data_internal(ctx, data_ctx);
 *
*/
static size_t llama_state_get_data_internal(struct llama_context * ctx, llama_data_write & data_ctx) {
    llama_synchronize(ctx);

    data_ctx.write_model_info(ctx);

    // copy outputs
    data_ctx.write_output_ids(ctx);
    data_ctx.write_logits(ctx);
    data_ctx.write_embeddings(ctx);

    data_ctx.write_kv_cache(ctx);

    return data_ctx.get_size_written();
}

size_t llama_state_get_data(struct llama_context * ctx, uint8_t * dst, size_t size) {
    llama_data_write_buffer data_ctx(dst, size);
    try {
        return llama_state_get_data_internal(ctx, data_ctx);
    } catch (const std::exception & err) {
        LLAMA_LOG_ERROR("%s: error saving state: %s\n", __func__, err.what());
        return 0;
    }
}

// Returns the *actual* size of the state.
// Intended to be used when saving to state to a buffer.
size_t llama_state_get_size(struct llama_context * ctx) {
    llama_data_write_dummy data_ctx;
    try {
        return llama_state_get_data_internal(ctx, data_ctx);
    } catch (const std::exception & err) {
        LLAMA_LOG_ERROR("%s: error getting state size: %s\n", __func__, err.what());
        return 0;
    }
}

static size_t llama_state_set_data_internal(struct llama_context * ctx, llama_data_read & data_ctx) {
    llama_synchronize(ctx);

    data_ctx.read_model_info(ctx);

    // set outputs
    data_ctx.read_output_ids(ctx);
    data_ctx.read_logits(ctx);
    data_ctx.read_embeddings(ctx);

    data_ctx.read_kv_cache(ctx);

    return data_ctx.get_size_read();
}

// Sets the state reading from the specified source address
size_t llama_state_set_data(struct llama_context * ctx, const uint8_t * src, size_t size) {
    llama_data_read_buffer data_ctx(src, size);
    try {
        return llama_state_set_data_internal(ctx, data_ctx);
    } catch (const std::exception & err) {
        LLAMA_LOG_ERROR("%s: error loading state: %s\n", __func__, err.what());
        return 0;
    }
}

static bool llama_state_load_file_internal(struct llama_context * ctx, const char * path_session, llama_token * tokens_out, size_t n_token_capacity, size_t * n_token_count_out) {
    llama_file file(path_session, "rb");

    // sanity checks
    {
        const uint32_t magic   = file.read_u32();
        const uint32_t version = file.read_u32();

        if (magic != LLAMA_SESSION_MAGIC || version != LLAMA_SESSION_VERSION) {
            LLAMA_LOG_ERROR("%s: unknown (magic, version) for session file: %08x, %08x\n", __func__, magic, version);
            return false;
        }
    }

    // load the prompt
    {
        const uint32_t n_token_count = file.read_u32();

        if (n_token_count > n_token_capacity) {
            LLAMA_LOG_ERROR("%s: token count in session file exceeded capacity! %u > %zu\n", __func__, n_token_count, n_token_capacity);
            return false;
        }

        file.read_raw(tokens_out, sizeof(llama_token) * n_token_count);
        *n_token_count_out = n_token_count;
    }

    // restore the context state
    {
        const size_t n_state_size_cur = file.size - file.tell();

        llama_data_read_file data_ctx(&file);
        const size_t n_read = llama_state_set_data_internal(ctx, data_ctx);

        if (n_read != n_state_size_cur) {
            LLAMA_LOG_ERROR("%s: did not read all of the session file data! size %zu, got %zu\n", __func__, n_state_size_cur, n_read);
            return false;
        }
    }
    return true;
}

bool llama_state_load_file(struct llama_context * ctx, const char * path_session, llama_token * tokens_out, size_t n_token_capacity, size_t * n_token_count_out) {
    try {
        return llama_state_load_file_internal(ctx, path_session, tokens_out, n_token_capacity, n_token_count_out);
    } catch (const std::exception & err) {
        LLAMA_LOG_ERROR("%s: error loading session file: %s\n", __func__, err.what());
        return false;
    }
}

static bool llama_state_save_file_internal(struct llama_context * ctx, const char * path_session, const llama_token * tokens, size_t n_token_count) {
    llama_file file(path_session, "wb");

    file.write_u32(LLAMA_SESSION_MAGIC);
    file.write_u32(LLAMA_SESSION_VERSION);

    // save the prompt
    file.write_u32((uint32_t) n_token_count);
    file.write_raw(tokens, sizeof(llama_token) * n_token_count);

    // save the context state using stream saving
    llama_data_write_file data_ctx(&file);
    llama_state_get_data_internal(ctx, data_ctx);

    return true;
}

bool llama_state_save_file(struct llama_context * ctx, const char * path_session, const llama_token * tokens, size_t n_token_count) {
    try {
        return llama_state_save_file_internal(ctx, path_session, tokens, n_token_count);
    } catch (const std::exception & err) {
        LLAMA_LOG_ERROR("%s: error saving session file: %s\n", __func__, err.what());
        return false;
    }
}

static size_t llama_state_seq_get_data_internal(struct llama_context * ctx, llama_data_write & data_ctx, llama_seq_id seq_id) {
    llama_synchronize(ctx);

    data_ctx.write_kv_cache(ctx, seq_id);

    return data_ctx.get_size_written();
}

size_t llama_state_seq_get_size(struct llama_context * ctx, llama_seq_id seq_id) {
    llama_data_write_dummy data_ctx;
    return llama_state_seq_get_data_internal(ctx, data_ctx, seq_id);
}

size_t llama_state_seq_get_data(struct llama_context * ctx, uint8_t * dst, size_t size, llama_seq_id seq_id) {
    llama_data_write_buffer data_ctx(dst, size);
    try {
        return llama_state_seq_get_data_internal(ctx, data_ctx, seq_id);
    } catch (const std::exception & err) {
        LLAMA_LOG_ERROR("%s: error saving sequence state: %s\n", __func__, err.what());
        return 0;
    }
}

static size_t llama_state_seq_set_data_internal(struct llama_context * ctx, llama_data_read & data_ctx, llama_seq_id dest_seq_id) {
    llama_synchronize(ctx);

    data_ctx.read_kv_cache(ctx, dest_seq_id);

    return data_ctx.get_size_read();
}

size_t llama_state_seq_set_data(struct llama_context * ctx, const uint8_t * src, size_t size, llama_seq_id dest_seq_id) {
    llama_data_read_buffer data_ctx(src, size);
    try {
        return llama_state_seq_set_data_internal(ctx, data_ctx, dest_seq_id);
    } catch (const std::exception & err) {
        LLAMA_LOG_ERROR("%s: error loading sequence state: %s\n", __func__, err.what());
        return 0;
    }
}

static size_t llama_state_seq_save_file_internal(struct llama_context * ctx, const char * filepath, llama_seq_id seq_id, const llama_token * tokens, size_t n_token_count) {
    llama_file file(filepath, "wb");

    file.write_u32(LLAMA_STATE_SEQ_MAGIC);
    file.write_u32(LLAMA_STATE_SEQ_VERSION);

    // save the prompt
    file.write_u32((uint32_t) n_token_count);
    file.write_raw(tokens, sizeof(llama_token) * n_token_count);

    // save the context state using stream saving
    llama_data_write_file data_ctx(&file);
    llama_state_seq_get_data_internal(ctx, data_ctx, seq_id);

    const size_t res = file.tell();
    LM_GGML_ASSERT(res == sizeof(uint32_t) * 3 + sizeof(llama_token) * n_token_count + data_ctx.get_size_written());
    return res;
}

static size_t llama_state_seq_load_file_internal(struct llama_context * ctx, const char * filepath, llama_seq_id dest_seq_id, llama_token * tokens_out, size_t n_token_capacity, size_t * n_token_count_out) {
    llama_file file(filepath, "rb");

    // version checks
    {
        const uint32_t magic   = file.read_u32();
        const uint32_t version = file.read_u32();

        if (magic != LLAMA_STATE_SEQ_MAGIC || version != LLAMA_STATE_SEQ_VERSION) {
            LLAMA_LOG_ERROR("%s: unknown (magic, version) for sequence state file: %08x, %08x\n", __func__, magic, version);
            return 0;
        }
    }

    // load the prompt
    {
        const uint32_t n_token_count = file.read_u32();

        if (n_token_count > n_token_capacity) {
            LLAMA_LOG_ERROR("%s: token count in sequence state file exceeded capacity! %u > %zu\n", __func__, n_token_count, n_token_capacity);
            return 0;
        }

        file.read_raw(tokens_out, sizeof(llama_token) * n_token_count);
        *n_token_count_out = n_token_count;
    }

    // restore the context state
    {
        const size_t state_size = file.size - file.tell();
        llama_data_read_file data_ctx(&file);
        const size_t nread = llama_state_seq_set_data_internal(ctx, data_ctx, dest_seq_id);
        if (!nread) {
            LLAMA_LOG_ERROR("%s: failed to restore sequence state\n", __func__);
            return 0;
        }
        LM_GGML_ASSERT(nread <= state_size);
        LM_GGML_ASSERT(nread + sizeof(uint32_t) * 3 + sizeof(llama_token) * *n_token_count_out == file.tell());
    }

    return file.tell();
}

size_t llama_state_seq_save_file(struct llama_context * ctx, const char * filepath, llama_seq_id seq_id, const llama_token * tokens, size_t n_token_count) {
    try {
        return llama_state_seq_save_file_internal(ctx, filepath, seq_id, tokens, n_token_count);
    } catch (const std::exception & err) {
        LLAMA_LOG_ERROR("%s: error saving sequence state file: %s\n", __func__, err.what());
        return 0;
    }
}

size_t llama_state_seq_load_file(struct llama_context * ctx, const char * filepath, llama_seq_id dest_seq_id, llama_token * tokens_out, size_t n_token_capacity, size_t * n_token_count_out) {
    try {
        return llama_state_seq_load_file_internal(ctx, filepath, dest_seq_id, tokens_out, n_token_capacity, n_token_count_out);
    } catch (const std::exception & err) {
        LLAMA_LOG_ERROR("%s: error loading sequence state file: %s\n", __func__, err.what());
        return 0;
    }
}

void llama_set_n_threads(struct llama_context * ctx, int32_t n_threads, int32_t n_threads_batch) {
    ctx->cparams.n_threads       = n_threads;
    ctx->cparams.n_threads_batch = n_threads_batch;
}

int32_t llama_n_threads(struct llama_context * ctx) {
    return ctx->cparams.n_threads;
}

int32_t llama_n_threads_batch(struct llama_context * ctx) {
    return ctx->cparams.n_threads_batch;
}

void llama_set_abort_callback(struct llama_context * ctx, bool (*abort_callback)(void * data), void * abort_callback_data) {
    ctx->abort_callback      = abort_callback;
    ctx->abort_callback_data = abort_callback_data;
}

void llama_set_embeddings(struct llama_context * ctx, bool embeddings) {
    ctx->cparams.embeddings = embeddings;
}

void llama_set_causal_attn(struct llama_context * ctx, bool causal_attn) {
    ctx->cparams.causal_attn = causal_attn;
}

struct llama_batch llama_batch_get_one(
             llama_token * tokens,
                 int32_t   n_tokens) {
    return {
        /*n_tokens       =*/ n_tokens,
        /*tokens         =*/ tokens,
        /*embd           =*/ nullptr,
        /*pos            =*/ nullptr,
        /*n_seq_id       =*/ nullptr,
        /*seq_id         =*/ nullptr,
        /*logits         =*/ nullptr,
    };
}

struct llama_batch llama_batch_init(int32_t n_tokens_alloc, int32_t embd, int32_t n_seq_max) {
    llama_batch batch = {
        /*n_tokens       =*/ 0,
        /*tokens         =*/ nullptr,
        /*embd           =*/ nullptr,
        /*pos            =*/ nullptr,
        /*n_seq_id       =*/ nullptr,
        /*seq_id         =*/ nullptr,
        /*logits         =*/ nullptr,
    };

    if (embd) {
        batch.embd = (float *) malloc(sizeof(float) * n_tokens_alloc * embd);
    } else {
        batch.token = (llama_token *) malloc(sizeof(llama_token) * n_tokens_alloc);
    }

    batch.pos      = (llama_pos *)     malloc(sizeof(llama_pos)      * n_tokens_alloc);
    batch.n_seq_id = (int32_t *)       malloc(sizeof(int32_t)        * n_tokens_alloc);
    batch.seq_id   = (llama_seq_id **) malloc(sizeof(llama_seq_id *) * (n_tokens_alloc + 1));
    for (int i = 0; i < n_tokens_alloc; ++i) {
        batch.seq_id[i] = (llama_seq_id *) malloc(sizeof(llama_seq_id) * n_seq_max);
    }
    batch.seq_id[n_tokens_alloc] = nullptr;

    batch.logits   = (int8_t *)        malloc(sizeof(int8_t)         * n_tokens_alloc);

    return batch;
}

void llama_batch_free(struct llama_batch batch) {
    if (batch.token)    free(batch.token);
    if (batch.embd)     free(batch.embd);
    if (batch.pos)      free(batch.pos);
    if (batch.n_seq_id) free(batch.n_seq_id);
    if (batch.seq_id) {
        for (int i = 0; batch.seq_id[i] != nullptr; ++i) {
            free(batch.seq_id[i]);
        }
        free(batch.seq_id);
    }
    if (batch.logits)   free(batch.logits);
}

int32_t llama_encode(
        struct llama_context * ctx,
          struct llama_batch   batch) {
    const int ret = llama_encode_internal(*ctx, batch);
    if (ret != 0) {
        LLAMA_LOG_ERROR("%s: failed to encode, ret = %d\n", __func__, ret);
    }

    return ret;
}

int32_t llama_decode(
        struct llama_context * ctx,
          struct llama_batch   batch) {
    const int ret = llama_decode_internal(*ctx, batch);
    if (ret != 0) {
        LLAMA_LOG_ERROR("%s: failed to decode, ret = %d\n", __func__, ret);
    }

    return ret;
}

void llama_synchronize(struct llama_context * ctx) {
    lm_ggml_backend_sched_synchronize(ctx->sched.get());

    // FIXME: if multiple single tokens are evaluated without a synchronization,
    // the stats will be added to the prompt evaluation stats
    // this should only happen when using batch size 1 to evaluate a batch

    // add the evaluation to the stats
    if (ctx->n_queued_tokens == 1) {
        if (!ctx->cparams.no_perf) {
            ctx->t_eval_us += lm_ggml_time_us() - ctx->t_compute_start_us;
        }
        ctx->n_eval++;
    } else if (ctx->n_queued_tokens > 1) {
        if (!ctx->cparams.no_perf) {
            ctx->t_p_eval_us += lm_ggml_time_us() - ctx->t_compute_start_us;
        }
        ctx->n_p_eval += ctx->n_queued_tokens;
    }

    // get a more accurate load time, upon first eval
    if (ctx->n_queued_tokens > 0 && !ctx->has_evaluated_once) {
        ctx->t_load_us = lm_ggml_time_us() - ctx->t_start_us;
        ctx->has_evaluated_once = true;
    }

    ctx->n_queued_tokens = 0;
    ctx->t_compute_start_us = 0;
}

float * llama_get_logits(struct llama_context * ctx) {
    llama_synchronize(ctx);

    // reorder logits for backward compatibility
    // TODO: maybe deprecate this
    llama_output_reorder(ctx);

    return ctx->logits;
}

float * llama_get_logits_ith(struct llama_context * ctx, int32_t i) {
    int32_t j = -1;
    llama_synchronize(ctx);

    try {
        if (ctx->logits == nullptr) {
            throw std::runtime_error("no logits");
        }

        if (i < 0) {
            j = ctx->n_outputs + i;
            if (j < 0) {
                throw std::runtime_error(format("negative index out of range [0, %d)", ctx->n_outputs));
            }
        } else if ((size_t) i >= ctx->output_ids.size()) {
            throw std::runtime_error(format("out of range [0, %zu)", ctx->output_ids.size()));
        } else {
            j = ctx->output_ids[i];
        }

        if (j < 0) {
            throw std::runtime_error(format("batch.logits[%d] != true", i));
        }
        if (j >= ctx->n_outputs) {
            // This should not happen
            throw std::runtime_error(format("corrupt output buffer (j=%d, n_outputs=%d)", j, ctx->n_outputs));
        }

        return ctx->logits + j*ctx->model.hparams.n_vocab;
    } catch (const std::exception & err) {
        LLAMA_LOG_ERROR("%s: invalid logits id %d, reason: %s\n", __func__, i, err.what());
#ifndef NDEBUG
        LM_GGML_ABORT("fatal error");
#else
        return nullptr;
#endif
    }
}

float * llama_get_embeddings(struct llama_context * ctx) {
    llama_synchronize(ctx);

    // reorder embeddings for backward compatibility
    // TODO: maybe deprecate this
    llama_output_reorder(ctx);

    return ctx->embd;
}

float * llama_get_embeddings_ith(struct llama_context * ctx, int32_t i) {
    int32_t j = -1;

    llama_synchronize(ctx);

    try {
        if (ctx->embd == nullptr) {
            throw std::runtime_error("no embeddings");
        }

        if (i < 0) {
            j = ctx->n_outputs + i;
            if (j < 0) {
                throw std::runtime_error(format("negative index out of range [0, %d)", ctx->n_outputs));
            }
        } else if ((size_t) i >= ctx->output_ids.size()) {
            throw std::runtime_error(format("out of range [0, %lu)", ctx->output_ids.size()));
        } else {
            j = ctx->output_ids[i];
        }

        if (j < 0) {
            throw std::runtime_error(format("batch.logits[%d] != true", i));
        }
        if (j >= ctx->n_outputs) {
            // This should not happen
            throw std::runtime_error(format("corrupt output buffer (j=%d, n_outputs=%d)", j, ctx->n_outputs));
        }

        return ctx->embd + j*ctx->model.hparams.n_embd;
    } catch (const std::exception & err) {
        LLAMA_LOG_ERROR("%s: invalid embeddings id %d, reason: %s\n", __func__, i, err.what());
#ifndef NDEBUG
        LM_GGML_ABORT("fatal error");
#else
        return nullptr;
#endif
    }
}

float * llama_get_embeddings_seq(struct llama_context * ctx, llama_seq_id seq_id) {
    llama_synchronize(ctx);

    auto it = ctx->embd_seq.find(seq_id);
    if (it == ctx->embd_seq.end()) {
        return nullptr;
    }

    return it->second.data();
}

//
// vocab
//

const char * llama_token_get_text(const struct llama_model * model, llama_token token) {
    return llama_token_get_text_impl(model->vocab, token);
}

float llama_token_get_score(const struct llama_model * model, llama_token token) {
    return llama_token_get_score_impl(model->vocab, token);
}

enum llama_token_attr llama_token_get_attr(const struct llama_model * model, llama_token token) {
    return llama_token_get_attr_impl(model->vocab, token);
}

bool llama_token_is_eog(const struct llama_model * model, llama_token token) {
    return llama_token_is_eog_impl(model->vocab, token);
}

bool llama_token_is_control(const struct llama_model * model, llama_token token) {
    return llama_token_is_control_impl(model->vocab, token);
}

llama_token llama_token_bos(const struct llama_model * model) {
    return llama_token_bos_impl(model->vocab);
}

llama_token llama_token_eos(const struct llama_model * model) {
    return llama_token_eos_impl(model->vocab);
}

llama_token llama_token_eot(const struct llama_model * model) {
    return llama_token_eot_impl(model->vocab);
}

llama_token llama_token_cls(const struct llama_model * model) {
    return llama_token_cls_impl(model->vocab);
}

llama_token llama_token_sep(const struct llama_model * model) {
    return llama_token_sep_impl(model->vocab);
}

llama_token llama_token_nl (const struct llama_model * model) {
    return llama_token_nl_impl(model->vocab);
}

llama_token llama_token_pad(const struct llama_model * model) {
    return llama_token_pad_impl(model->vocab);
}

bool llama_add_bos_token(const struct llama_model * model) {
    return llama_add_bos_token_impl(model->vocab);
}

bool llama_add_eos_token(const struct llama_model * model) {
    return llama_add_eos_token_impl(model->vocab);
}

llama_token llama_token_prefix(const struct llama_model * model) {
    return llama_token_prefix_impl(model->vocab);
}

llama_token llama_token_middle(const struct llama_model * model) {
    return llama_token_middle_impl(model->vocab);
}

llama_token llama_token_suffix(const struct llama_model * model) {
    return llama_token_suffix_impl(model->vocab);
}

llama_token llama_token_fim_pre(const struct llama_model * model) {
    return llama_token_fim_pre_impl(model->vocab);
}

llama_token llama_token_fim_suf(const struct llama_model * model) {
    return llama_token_fim_suf_impl(model->vocab);
}

llama_token llama_token_fim_mid(const struct llama_model * model) {
    return llama_token_fim_mid_impl(model->vocab);
}

llama_token llama_token_fim_pad(const struct llama_model * model) {
    return llama_token_fim_pad_impl(model->vocab);
}

llama_token llama_token_fim_rep(const struct llama_model * model) {
    return llama_token_fim_rep_impl(model->vocab);
}

llama_token llama_token_fim_sep(const struct llama_model * model) {
    return llama_token_fim_sep_impl(model->vocab);
}

//
// tokenization
//

int32_t llama_tokenize(
    const struct llama_model * model,
                  const char * text,
                     int32_t   text_len,
                 llama_token * tokens,
                     int32_t   n_tokens_max,
                        bool   add_special,
                        bool   parse_special) {
    return llama_tokenize_impl(model->vocab, text, text_len, tokens, n_tokens_max, add_special, parse_special);
}

int32_t llama_token_to_piece(
    const struct llama_model * model,
                 llama_token   token,
                        char * buf,
                     int32_t   length,
                     int32_t   lstrip,
                        bool   special) {
    return llama_token_to_piece_impl(model->vocab, token, buf, length, lstrip, special);
}

int32_t llama_detokenize(
    const struct llama_model * model,
           const llama_token * tokens,
                     int32_t   n_tokens,
                        char * text,
                     int32_t   text_len_max,
                        bool   remove_special,
                        bool   unparse_special) {
    return llama_detokenize_impl(model->vocab, tokens, n_tokens, text, text_len_max, remove_special, unparse_special);
}

//
// chat templates
//

// Simple version of "llama_apply_chat_template" that only works with strings
// This function uses heuristic checks to determine commonly used template. It is not a jinja parser.
static int32_t llama_chat_apply_template_internal(
    const std::string & tmpl,
    const std::vector<const llama_chat_message *> & chat,
    std::string & dest, bool add_ass) {
    // Taken from the research: https://github.com/ggerganov/llama.cpp/issues/5527
    std::stringstream ss;
    auto tmpl_contains = [&tmpl](std::string haystack) -> bool {
        return tmpl.find(haystack) != std::string::npos;
    };
    if (tmpl == "chatml" || tmpl_contains("<|im_start|>")) {
        // chatml template
        for (auto message : chat) {
            ss << "<|im_start|>" << message->role << "\n" << message->content << "<|im_end|>\n";
        }
        if (add_ass) {
            ss << "<|im_start|>assistant\n";
        }
    } else if (tmpl == "llama2" || tmpl == "mistral" || tmpl_contains("[INST]")) {
        // llama2 template and its variants
        // [variant] support system message
        bool support_system_message = tmpl_contains("<<SYS>>") || tmpl == "mistral";
        // [variant] space before + after response
        bool space_around_response = tmpl_contains("' ' + eos_token");
        // [variant] add BOS inside history
        bool add_bos_inside_history = tmpl_contains("bos_token + '[INST]");
        // [variant] trim spaces from the input message
        bool strip_message = tmpl_contains("content.strip()");
        // construct the prompt
        bool is_inside_turn = true; // skip BOS at the beginning
        ss << "[INST] ";
        for (auto message : chat) {
            std::string content = strip_message ? trim(message->content) : message->content;
            std::string role(message->role);
            if (!is_inside_turn) {
                is_inside_turn = true;
                ss << (add_bos_inside_history ? "<s>[INST] " : "[INST] ");
            }
            if (role == "system") {
                if (support_system_message) {
                    ss << "<<SYS>>\n" << content << "\n<</SYS>>\n\n";
                } else {
                    // if the model does not support system message, we still include it in the first message, but without <<SYS>>
                    ss << content << "\n";
                }
            } else if (role == "user") {
                ss << content << " [/INST]";
            } else {
                ss << (space_around_response ? " " : "") << content << (space_around_response ? " " : "") << "</s>";
                is_inside_turn = false;
            }
        }
        // llama2 templates seem to not care about "add_generation_prompt"
    } else if (tmpl == "phi3" || (tmpl_contains("<|assistant|>") && tmpl_contains("<|end|>"))) {
        // Phi 3
        for (auto message : chat) {
            std::string role(message->role);
            ss << "<|" << role << "|>\n" << message->content << "<|end|>\n";
        }
        if (add_ass) {
            ss << "<|assistant|>\n";
        }
    } else if (tmpl == "zephyr" || tmpl_contains("<|user|>")) {
        // zephyr template
        for (auto message : chat) {
            ss << "<|" << message->role << "|>" << "\n" << message->content << "<|endoftext|>\n";
        }
        if (add_ass) {
            ss << "<|assistant|>\n";
        }
    } else if (tmpl == "monarch" || tmpl_contains("bos_token + message['role']")) {
        // mlabonne/AlphaMonarch-7B template (the <s> is included inside history)
        for (auto message : chat) {
            std::string bos = (message == chat.front()) ? "" : "<s>"; // skip BOS for first message
            ss << bos << message->role << "\n" << message->content << "</s>\n";
        }
        if (add_ass) {
            ss << "<s>assistant\n";
        }
    } else if (tmpl == "gemma" || tmpl == "gemma2" || tmpl_contains("<start_of_turn>")) {
        // google/gemma-7b-it
        std::string system_prompt = "";
        for (auto message : chat) {
            std::string role(message->role);
            if (role == "system") {
                // there is no system message for gemma, but we will merge it with user prompt, so nothing is broken
                system_prompt = trim(message->content);
                continue;
            }
            // in gemma, "assistant" is "model"
            role = role == "assistant" ? "model" : message->role;
            ss << "<start_of_turn>" << role << "\n";
            if (!system_prompt.empty() && role != "model") {
                ss << system_prompt << "\n\n";
                system_prompt = "";
            }
            ss << trim(message->content) << "<end_of_turn>\n";
        }
        if (add_ass) {
            ss << "<start_of_turn>model\n";
        }
    } else if (tmpl == "orion" || tmpl_contains("'\\n\\nAssistant: ' + eos_token")) {
        // OrionStarAI/Orion-14B-Chat
        std::string system_prompt = "";
        for (auto message : chat) {
            std::string role(message->role);
            if (role == "system") {
                // there is no system message support, we will merge it with user prompt
                system_prompt = message->content;
                continue;
            } else if (role == "user") {
                ss << "Human: ";
                if (!system_prompt.empty()) {
                    ss << system_prompt << "\n\n";
                    system_prompt = "";
                }
                ss << message->content << "\n\nAssistant: </s>";
            } else {
                ss << message->content << "</s>";
            }
        }
    } else if (tmpl == "openchat" || tmpl_contains("GPT4 Correct ")) {
        // openchat/openchat-3.5-0106,
        for (auto message : chat) {
            std::string role(message->role);
            if (role == "system") {
                ss << message->content << "<|end_of_turn|>";
            } else {
                role[0] = toupper(role[0]);
                ss << "GPT4 Correct " << role << ": " << message->content << "<|end_of_turn|>";
            }
        }
        if (add_ass) {
            ss << "GPT4 Correct Assistant:";
        }
    } else if (tmpl == "vicuna" || tmpl == "vicuna-orca" || (tmpl_contains("USER: ") && tmpl_contains("ASSISTANT: "))) {
        // eachadea/vicuna-13b-1.1 (and Orca variant)
        for (auto message : chat) {
            std::string role(message->role);
            if (role == "system") {
                // Orca-Vicuna variant uses a system prefix
                if (tmpl == "vicuna-orca" || tmpl_contains("SYSTEM: ")) {
                    ss << "SYSTEM: " << message->content << "\n";
                } else {
                    ss << message->content << "\n\n";
                }
            } else if (role == "user") {
                ss << "USER: " << message->content << "\n";
            } else if (role == "assistant") {
                ss << "ASSISTANT: " << message->content << "</s>\n";
            }
        }
        if (add_ass) {
            ss << "ASSISTANT:";
        }
    } else if (tmpl == "deepseek" || (tmpl_contains("### Instruction:") && tmpl_contains("<|EOT|>"))) {
        // deepseek-ai/deepseek-coder-33b-instruct
        for (auto message : chat) {
            std::string role(message->role);
            if (role == "system") {
                ss << message->content;
            } else if (role == "user") {
                ss << "### Instruction:\n" << message->content << "\n";
            } else if (role == "assistant") {
                ss << "### Response:\n" << message->content << "\n<|EOT|>\n";
            }
        }
        if (add_ass) {
            ss << "### Response:\n";
        }
    } else if (tmpl == "command-r" || (tmpl_contains("<|START_OF_TURN_TOKEN|>") && tmpl_contains("<|USER_TOKEN|>"))) {
        // CohereForAI/c4ai-command-r-plus
        for (auto message : chat) {
            std::string role(message->role);
            if (role == "system") {
                ss << "<|START_OF_TURN_TOKEN|><|SYSTEM_TOKEN|>" << trim(message->content) << "<|END_OF_TURN_TOKEN|>";
            } else if (role == "user") {
                ss << "<|START_OF_TURN_TOKEN|><|USER_TOKEN|>" << trim(message->content) << "<|END_OF_TURN_TOKEN|>";
            } else if (role == "assistant") {
                ss << "<|START_OF_TURN_TOKEN|><|CHATBOT_TOKEN|>" << trim(message->content) << "<|END_OF_TURN_TOKEN|>";
            }
        }
        if (add_ass) {
            ss << "<|START_OF_TURN_TOKEN|><|CHATBOT_TOKEN|>";
        }
    } else if (tmpl == "llama3" || (tmpl_contains("<|start_header_id|>") && tmpl_contains("<|end_header_id|>"))) {
        // Llama 3
        for (auto message : chat) {
            std::string role(message->role);
            ss << "<|start_header_id|>" << role << "<|end_header_id|>\n\n" << trim(message->content) << "<|eot_id|>";
        }
        if (add_ass) {
            ss << "<|start_header_id|>assistant<|end_header_id|>\n\n";
        }
    } else if (tmpl == "chatglm3" || tmpl_contains("[gMASK]sop")) {
        // chatglm3-6b
        ss << "[gMASK]" << "sop";
        for (auto message : chat) {
            std::string role(message->role);
            ss << "<|" << role << "|>" << "\n " << message->content;
        }
        if (add_ass) {
            ss << "<|assistant|>";
        }
    } else if (tmpl == "chatglm4" || tmpl_contains("[gMASK]<sop>")) {
        ss << "[gMASK]" << "<sop>";
        for (auto message : chat) {
            std::string role(message->role);
            ss << "<|" << role << "|>" << "\n" << message->content;
        }
        if (add_ass) {
            ss << "<|assistant|>";
        }
    } else if (tmpl == "minicpm" || tmpl_contains(LU8("<用户>"))) {
        // MiniCPM-3B-OpenHermes-2.5-v2-GGUF
        for (auto message : chat) {
            std::string role(message->role);
            if (role == "user") {
                ss << LU8("<用户>");
                ss << trim(message->content);
                ss << "<AI>";
            } else {
                ss << trim(message->content);
            }
        }
    } else if (tmpl == "deepseek2" || tmpl_contains("'Assistant: ' + message['content'] + eos_token")) {
        // DeepSeek-V2
        for (auto message : chat) {
            std::string role(message->role);
            if (role == "system") {
                ss << message->content << "\n\n";
            } else if (role == "user") {
                ss << "User: " << message->content << "\n\n";
            } else if (role == "assistant") {
                ss << "Assistant: " << message->content << LU8("<｜end▁of▁sentence｜>");
            }
        }
        if (add_ass) {
            ss << "Assistant:";
        }
    } else if (tmpl == "exaone3" || (tmpl_contains("[|system|]") && tmpl_contains("[|assistant|]") && tmpl_contains("[|endofturn|]"))) {
        // ref: https://huggingface.co/LGAI-EXAONE/EXAONE-3.0-7.8B-Instruct/discussions/8#66bae61b1893d14ee8ed85bb
        // EXAONE-3.0-7.8B-Instruct
        for (auto message : chat) {
            std::string role(message->role);
            if (role == "system") {
                ss << "[|system|]" << trim(message->content) << "[|endofturn|]\n";
            } else if (role == "user") {
                ss << "[|user|]" << trim(message->content) << "\n";
            } else if (role == "assistant") {
                ss << "[|assistant|]" << trim(message->content) << "[|endofturn|]\n";
            }
        }
        if (add_ass) {
            ss << "[|assistant|]";
        }
    } else if (tmpl == "rwkv-world" || tmpl_contains("rwkv-world")) {
        // this template requires the model to have "\n\n" as EOT token
        for (auto message : chat) {
            std::string role(message->role);
            if (role == "user") {
                ss << "User: " << message->content << "\n\nAssistant:";
            } else {
                ss << message->content << "\n\n";
            }
        }
    } else if (tmpl == "granite" || tmpl_contains("<|start_of_role|>")) {
        // IBM Granite template
        for (const auto & message : chat) {
            std::string role(message->role);
            ss << "<|start_of_role|>" << role << "<|end_of_role|>";
            if (role == "assistant_tool_call") {
                ss << "<|tool_call|>";
            }
            ss << message->content << "<|end_of_text|>\n";
        }
        if (add_ass) {
            ss << "<|start_of_role|>assistant<|end_of_role|>\n";
        }
    } else {
        // template not supported
        return -1;
    }
    dest = ss.str();
    return dest.size();
}

int32_t llama_chat_apply_template(
                const struct llama_model * model,
                              const char * tmpl,
         const struct llama_chat_message * chat,
                                  size_t   n_msg,
                                    bool   add_ass,
                                    char * buf,
                                 int32_t   length) {
    std::string curr_tmpl(tmpl == nullptr ? "" : tmpl);
    if (tmpl == nullptr) {
        LM_GGML_ASSERT(model != nullptr);
        // load template from model
        std::vector<char> model_template(2048, 0); // longest known template is about 1200 bytes
        std::string template_key = "tokenizer.chat_template";
        int32_t res = llama_model_meta_val_str(model, template_key.c_str(), model_template.data(), model_template.size());
        if (res < 0) {
            // worst case: there is no information about template, we will use chatml by default
            curr_tmpl = "chatml"; // see llama_chat_apply_template_internal
        } else {
            curr_tmpl = std::string(model_template.data(), model_template.size());
        }
    }

    // format the chat to string
    std::vector<const llama_chat_message *> chat_vec;
    chat_vec.resize(n_msg);
    for (size_t i = 0; i < n_msg; i++) {
        chat_vec[i] = &chat[i];
    }

    std::string formatted_chat;
    int32_t res = llama_chat_apply_template_internal(curr_tmpl, chat_vec, formatted_chat, add_ass);
    if (res < 0) {
        return res;
    }
    if (buf && length > 0) {
        strncpy(buf, formatted_chat.c_str(), length);
    }
    return res;
}

//
// sampling
//

// TODO: remove indirection when vocab becomes accesible in llama-sampling.cpp
struct llama_sampler * llama_sampler_init_grammar(const struct llama_model * model, const char * grammar_str, const char * grammar_root) {
    return llama_sampler_init_grammar_impl(model->vocab, grammar_str, grammar_root);
}

struct llama_sampler * llama_sampler_init_infill(const struct llama_model * model) {
    return llama_sampler_init_infill_impl(model->vocab);
}

struct llama_sampler * llama_sampler_init_dry(const struct llama_model * model, float dry_multiplier, float dry_base, int32_t dry_allowed_length, int32_t dry_penalty_last_n, const char** seq_breakers, size_t num_breakers) {
    return llama_sampler_init_dry_impl(model->vocab, llama_n_ctx_train(model), dry_multiplier, dry_base, dry_allowed_length, dry_penalty_last_n, seq_breakers, num_breakers);
}

//
// model split
//

int llama_split_path(char * split_path, size_t maxlen, const char * path_prefix, int split_no, int split_count) {
    static const char * const SPLIT_PATH_FORMAT = "%s-%05d-of-%05d.gguf";
    if (snprintf(split_path, maxlen, SPLIT_PATH_FORMAT, path_prefix, split_no + 1, split_count)) {
        return strlen(split_path);
    }
    return 0;
}

int llama_split_prefix(char * dest, size_t maxlen, const char * split_path, int split_no, int split_count) {
    std::string str_split_path(split_path);
    char postfix[32];
    snprintf(postfix, 32, "-%05d-of-%05d.gguf", split_no + 1, split_count);
    std::string str_postfix(postfix);

    // check if dest ends with postfix
    int size_prefix = str_split_path.size() - str_postfix.size();
    if (size_prefix > 0 && str_split_path.find(str_postfix, size_prefix) != std::string::npos) {
        snprintf(dest, std::min((size_t) size_prefix + 1, maxlen), "%s", split_path);
        return size_prefix;
    }

    return 0;
}

const char * llama_print_system_info(void) {
    lm_ggml_cpu_init(); // some ARM features are detected at runtime

    static std::string s;

    s  = "";
    s += "AVX = "         + std::to_string(lm_ggml_cpu_has_avx())         + " | ";
    s += "AVX_VNNI = "    + std::to_string(lm_ggml_cpu_has_avx_vnni())    + " | ";
    s += "AVX2 = "        + std::to_string(lm_ggml_cpu_has_avx2())        + " | ";
    s += "AVX512 = "      + std::to_string(lm_ggml_cpu_has_avx512())      + " | ";
    s += "AVX512_VBMI = " + std::to_string(lm_ggml_cpu_has_avx512_vbmi()) + " | ";
    s += "AVX512_VNNI = " + std::to_string(lm_ggml_cpu_has_avx512_vnni()) + " | ";
    s += "AVX512_BF16 = " + std::to_string(lm_ggml_cpu_has_avx512_bf16()) + " | ";
    s += "AMX_INT8 = "    + std::to_string(lm_ggml_cpu_has_amx_int8())    + " | ";
    s += "FMA = "         + std::to_string(lm_ggml_cpu_has_fma())         + " | ";
    s += "NEON = "        + std::to_string(lm_ggml_cpu_has_neon())        + " | ";
    s += "SVE = "         + std::to_string(lm_ggml_cpu_has_sve())         + " | ";
    s += "ARM_FMA = "     + std::to_string(lm_ggml_cpu_has_arm_fma())     + " | ";
    s += "F16C = "        + std::to_string(lm_ggml_cpu_has_f16c())        + " | ";
    s += "FP16_VA = "     + std::to_string(lm_ggml_cpu_has_fp16_va())     + " | ";
    s += "RISCV_VECT = "  + std::to_string(lm_ggml_cpu_has_riscv_v())     + " | ";
    s += "WASM_SIMD = "   + std::to_string(lm_ggml_cpu_has_wasm_simd())   + " | ";
    s += "SSE3 = "        + std::to_string(lm_ggml_cpu_has_sse3())        + " | ";
    s += "SSSE3 = "       + std::to_string(lm_ggml_cpu_has_ssse3())       + " | ";
    s += "VSX = "         + std::to_string(lm_ggml_cpu_has_vsx())         + " | ";
    s += "MATMUL_INT8 = " + std::to_string(lm_ggml_cpu_has_matmul_int8()) + " | ";
    s += "LLAMAFILE = "   + std::to_string(lm_ggml_cpu_has_llamafile())   + " | ";

    return s.c_str();
}

struct llama_perf_context_data llama_perf_context(const struct llama_context * ctx) {
    struct llama_perf_context_data data = {};

    if (ctx == nullptr) {
        return data;
    }

    data.t_start_ms  = 1e-3 * ctx->t_start_us;
    data.t_load_ms   = 1e-3 * ctx->t_load_us;
    data.t_p_eval_ms = 1e-3 * ctx->t_p_eval_us;
    data.t_eval_ms   = 1e-3 * ctx->t_eval_us;
    data.n_p_eval    = std::max(1, ctx->n_p_eval);
    data.n_eval      = std::max(1, ctx->n_eval);

    return data;
}

void llama_perf_context_print(const struct llama_context * ctx) {
    const auto data = llama_perf_context(ctx);

    const double t_end_ms = 1e-3 * lm_ggml_time_us();

    LLAMA_LOG_INFO("%s:        load time = %10.2f ms\n", __func__, data.t_load_ms);
    LLAMA_LOG_INFO("%s: prompt eval time = %10.2f ms / %5d tokens (%8.2f ms per token, %8.2f tokens per second)\n",
            __func__, data.t_p_eval_ms, data.n_p_eval, data.t_p_eval_ms / data.n_p_eval, 1e3 / data.t_p_eval_ms * data.n_p_eval);
    LLAMA_LOG_INFO("%s:        eval time = %10.2f ms / %5d runs   (%8.2f ms per token, %8.2f tokens per second)\n",
            __func__, data.t_eval_ms, data.n_eval, data.t_eval_ms / data.n_eval, 1e3 / data.t_eval_ms * data.n_eval);
    LLAMA_LOG_INFO("%s:       total time = %10.2f ms / %5d tokens\n", __func__, (t_end_ms - data.t_start_ms), (data.n_p_eval + data.n_eval));
}

void llama_perf_context_reset(struct llama_context * ctx) {
    ctx->t_start_us  = lm_ggml_time_us();
    ctx->t_eval_us   = ctx->n_eval = 0;
    ctx->t_p_eval_us = ctx->n_p_eval = 0;
}

// For internal test use
const std::vector<std::pair<std::string, struct lm_ggml_tensor *>> & llama_internal_get_tensor_map(
    struct llama_context * ctx
) {
    return ctx->model.tensors_by_name;
}

void llama_log_set(lm_ggml_log_callback log_callback, void * user_data) {
    lm_ggml_log_set(log_callback, user_data);
    g_logger_state.log_callback = log_callback ? log_callback : llama_log_callback_default;
    g_logger_state.log_callback_user_data = user_data;
}

static void llama_log_internal_v(lm_ggml_log_level level, const char * format, va_list args) {
    va_list args_copy;
    va_copy(args_copy, args);
    char buffer[128];
    int len = vsnprintf(buffer, 128, format, args);
    if (len < 128) {
        g_logger_state.log_callback(level, buffer, g_logger_state.log_callback_user_data);
    } else {
        char * buffer2 = new char[len + 1];
        vsnprintf(buffer2, len + 1, format, args_copy);
        buffer2[len] = 0;
        g_logger_state.log_callback(level, buffer2, g_logger_state.log_callback_user_data);
        delete[] buffer2;
    }
    va_end(args_copy);
}

void llama_log_internal(lm_ggml_log_level level, const char * format, ...) {
    va_list args;
    va_start(args, format);
    llama_log_internal_v(level, format, args);
    va_end(args);
}

void llama_log_callback_default(lm_ggml_log_level level, const char * text, void * user_data) {
    (void) level;
    (void) user_data;
    fputs(text, stderr);
    fflush(stderr);
}
