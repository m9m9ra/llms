// llamajni.cpp
#include <jni.h>
#include <map>
#include <string>
#include "llama.h"

// Глобальные переменные для контекста модели
static llama_model *model = nullptr;
static llama_context *ctx = nullptr;

extern "C" {

JNIEXPORT jlong JNICALL
Java_com_example_GGUFModelWrapper_nativeLoadModel(
    JNIEnv *env,
    jobject /* this */,
    jstring modelPath,
    jint numThreads,
    jint numGpuLayers) {
    
    const char *path = env->GetStringUTFChars(modelPath, nullptr);
    
    // Инициализация параметров модели
    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = numGpuLayers;
    
    model = llama_load_model_from_file(path, model_params);
    env->ReleaseStringUTFChars(modelPath, path);
    
    if (!model) {
        return 0;
    }
    
    // Инициализация контекста
    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.seed = 1234;
    ctx_params.n_ctx = 2048;
    ctx_params.n_threads = numThreads;
    ctx_params.n_threads_batch = numThreads;
    
    ctx = llama_new_context_with_model(model, ctx_params);
    if (!ctx) {
        llama_free_model(model);
        model = nullptr;
        return 0;
    }
    
    return reinterpret_cast<jlong>(ctx);
}

JNIEXPORT jstring JNICALL
Java_com_example_GGUFModelWrapper_nativeInfer(
    JNIEnv *env,
    jobject /* this */,
    jlong modelPtr,
    jstring prompt,
    jobject params) {
    
    if (!modelPtr) return env->NewStringUTF("");
    
    const char *promptStr = env->GetStringUTFChars(prompt, nullptr);
    
    // Парсинг параметров из Java Map
    jclass mapClass = env->GetObjectClass(params);
    jmethodID getMethod = env->GetMethodID(mapClass, "get", "(Ljava/lang/Object;)Ljava/lang/Object;");
    
    // Получаем параметры инференса
    jstring maxTokensKey = env->NewStringUTF("max_tokens");
    jobject maxTokensObj = env->CallObjectMethod(params, getMethod, maxTokensKey);
    int maxTokens = 256;
    if (maxTokensObj) {
        maxTokens = env->CallIntMethod(maxTokensObj, env->GetMethodID(env->FindClass("java/lang/Integer"), "intValue"));
    }
    
    jstring tempKey = env->NewStringUTF("temperature");
    jobject tempObj = env->CallObjectMethod(params, getMethod, tempKey);
    float temperature = 0.7f;
    if (tempObj) {
        temperature = env->CallFloatMethod(tempObj, env->GetMethodID(env->FindClass("java/lang/Float"), "floatValue"));
    }
    
    // Подготовка промпта
    llama_batch batch = llama_batch_init(512, 0);
    int n_past = 0;
    
    // Токенизация промпта
    std::vector<llama_token> tokens;
    tokens = llama_tokenize(ctx, promptStr, true);
    
    // Обработка токенов
    for (size_t i = 0; i < tokens.size(); i++) {
        llama_batch_add(batch, tokens[i], i, { 0 }, false);
    }
    
    // Инференс
    std::string output;
    int n_cur = batch.n_tokens;
    int n_decode = 0;
    
    while (n_cur <= maxTokens) {
        if (llama_decode(ctx, batch)) {
            break;
        }
        
        // Генерация следующего токена
        llama_token new_token_id = llama_sample_token(ctx, batch, temperature);
        
        if (new_token_id == llama_token_eos(model)) {
            break;
        }
        
        // Добавление токена в вывод
        output += llama_token_to_piece(ctx, new_token_id);
        
        // Подготовка следующего батча
        llama_batch_clear(batch);
        llama_batch_add(batch, new_token_id, n_cur, { 0 }, true);
        
        n_cur++;
        n_decode++;
    }
    
    llama_batch_free(batch);
    env->ReleaseStringUTFChars(prompt, promptStr);
    
    return env->NewStringUTF(output.c_str());
}

JNIEXPORT void JNICALL
Java_com_example_GGUFModelWrapper_nativeUnloadModel(
    JNIEnv * /* env */,
    jobject /* this */,
    jlong modelPtr) {
    
    if (ctx) {
        llama_free(ctx);
        ctx = nullptr;
    }
    if (model) {
        llama_free_model(model);
        model = nullptr;
    }
}

JNIEXPORT jobject JNICALL
Java_com_example_GGUFModelWrapper_nativeGetModelInfo(
    JNIEnv *env,
    jobject /* this */,
    jlong modelPtr) {
    
    if (!modelPtr) return nullptr;
    
    // Создаем Java HashMap
    jclass hashMapClass = env->FindClass("java/util/HashMap");
    jmethodID hashMapInit = env->GetMethodID(hashMapClass, "<init>", "()V");
    jobject hashMap = env->NewObject(hashMapClass, hashMapInit);
    jmethodID putMethod = env->GetMethodID(hashMapClass, "put", 
        "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    
    // Добавляем информацию о модели
    env->CallObjectMethod(hashMap, putMethod,
        env->NewStringUTF("n_vocab"),
        env->NewStringUTF(std::to_string(llama_n_vocab(model)).c_str()));
    
    env->CallObjectMethod(hashMap, putMethod,
        env->NewStringUTF("context_size"),
        env->NewStringUTF(std::to_string(llama_n_ctx(ctx)).c_str()));
    
    return hashMap;
}

} // extern "C"