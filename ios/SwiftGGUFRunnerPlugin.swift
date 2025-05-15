// SwiftGGUFRunnerPlugin.swift

public class SwiftGGUFRunnerPlugin: NSObject, FlutterPlugin {
  private var modelWrapper: GGUFModelWrapper?
  
  public static func register(with registrar: FlutterPluginRegistrar) {
    let channel = FlutterMethodChannel(name: "gguf_runner", binaryMessenger: registrar.messenger())
    let instance = SwiftGGUFRunnerPlugin()
    registrar.addMethodCallDelegate(instance, channel: channel)
  }

  public func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
    switch call.method {
    case "loadModel":
      guard let args = call.arguments as? [String: Any],
            let modelPath = args["modelPath"] as? String else {
        result(FlutterError(code: "INVALID_ARGUMENTS", message: "Invalid arguments", details: nil))
        return
      }
      let params = args["params"] as? [String: Any] ?? [:]
      
      do {
        modelWrapper = try GGUFModelWrapper()
        try modelWrapper?.loadModel(modelPath: modelPath, params: params)
        result(nil)
      } catch {
        result(FlutterError(code: "LOAD_ERROR", message: error.localizedDescription, details: nil))
      }
      
    case "infer":
      guard let args = call.arguments as? [String: Any],
            let prompt = args["prompt"] as? String else {
        result(FlutterError(code: "INVALID_ARGUMENTS", message: "Invalid arguments", details: nil))
        return
      }
      let options = args["options"] as? [String: Any] ?? [:]
      
      do {
        let output = try modelWrapper?.infer(prompt: prompt, options: options) ?? ""
        result(output)
      } catch {
        result(FlutterError(code: "INFER_ERROR", message: error.localizedDescription, details: nil))
      }
      
    case "unloadModel":
      do {
        try modelWrapper?.unloadModel()
        modelWrapper = nil
        result(nil)
      } catch {
        result(FlutterError(code: "UNLOAD_ERROR", message: error.localizedDescription, details: nil))
      }
      
    case "isAvailable":
      result(true)
      
    case "getModelInfo":
      do {
        let info = try modelWrapper?.getModelInfo() ?? [:]
        result(info)
      } catch {
        result(FlutterError(code: "INFO_ERROR", message: error.localizedDescription, details: nil))
      }
      
    default:
      result(FlutterMethodNotImplemented)
    }
  }
}

class GGUFModelWrapper {
    private var model: OpaquePointer?
    private var context: OpaquePointer?
    
    func loadModel(modelPath: String, params: [String: Any]) throws {
        let numThreads = params["n_threads"] as? Int32 ?? 4
        let numGpuLayers = params["n_gpu_layers"] as? Int32 ?? 0
        
        var modelParams = llama_model_default_params()
        modelParams.n_gpu_layers = numGpuLayers
        
        model = llama_load_model_from_file(modelPath, modelParams)
        guard model != nil else {
            throw NSError(domain: "GGUF", code: 1, userInfo: [NSLocalizedDescriptionKey: "Failed to load model"])
        }
        
        var contextParams = llama_context_default_params()
        contextParams.seed = 1234
        contextParams.n_ctx = 2048
        contextParams.n_threads = numThreads
        contextParams.n_threads_batch = numThreads
        
        context = llama_new_context_with_model(model, contextParams)
        guard context != nil else {
            llama_free_model(model)
            model = nil
            throw NSError(domain: "GGUF", code: 2, userInfo: [NSLocalizedDescriptionKey: "Failed to create context"])
        }
    }
    
    func infer(prompt: String, options: [String: Any]) throws -> String {
        guard let context = context else {
            throw NSError(domain: "GGUF", code: 3, userInfo: [NSLocalizedDescriptionKey: "Model not loaded"])
        }
        
        let maxTokens = options["max_tokens"] as? Int32 ?? 256
        let temperature = options["temperature"] as? Float ?? 0.7
        
        var batch = llama_batch_init(512, 0)
        defer { llama_batch_free(batch) }
        
        // Токенизация промпта
        let tokens = llama_tokenize(context, prompt, true)
        
        // Обработка токенов
        for (i, token) in tokens.enumerated() {
            llama_batch_add(batch, token, Int32(i), [0], false)
        }
        
        // Инференс
        var output = ""
        var nCur = batch.n_tokens
        var nDecode = 0
        
        while nCur <= maxTokens {
            if llama_decode(context, batch) != 0 {
                break
            }
            
            // Генерация следующего токена
            let newTokenId = llama_sample_token(context, batch, temperature)
            
            if newTokenId == llama_token_eos(model) {
                break
            }
            
            // Добавление токена в вывод
            if let tokenStr = llama_token_to_piece(context, newTokenId) {
                output += tokenStr
            }
            
            // Подготовка следующего батча
            llama_batch_clear(batch)
            llama_batch_add(batch, newTokenId, nCur, [0], true)
            
            nCur += 1
            nDecode += 1
        }
        
        return output
    }
    
    func unloadModel() {
        if let context = context {
            llama_free(context)
            self.context = nil
        }
        if let model = model {
            llama_free_model(model)
            self.model = nil
        }
    }
    
    func getModelInfo() -> [String: Any] {
        guard let model = model, let context = context else {
            return [:]
        }
        
        return [
            "n_vocab": llama_n_vocab(model),
            "context_size": llama_n_ctx(context)
        ]
    }
}