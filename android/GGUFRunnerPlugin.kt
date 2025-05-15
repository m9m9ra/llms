// GGUFRunnerPlugin.kt

class GGUFRunnerPlugin : FlutterPlugin, MethodCallHandler {
  private lateinit var channel: MethodChannel
  private var modelWrapper: GGUFModelWrapper? = null

  override fun onAttachedToEngine(flutterPluginBinding: FlutterPlugin.FlutterPluginBinding) {
    channel = MethodChannel(flutterPluginBinding.binaryMessenger, "gguf_runner")
    channel.setMethodCallHandler(this)
  }

  override fun onMethodCall(call: MethodCall, result: Result) {
    when (call.method) {
      "loadModel" -> {
        val modelPath = call.argument<String>("modelPath") ?: ""
        val params = call.argument<Map<String, Any>>("params") ?: emptyMap()
        
        try {
          modelWrapper = GGUFModelWrapper()
          modelWrapper?.loadModel(modelPath, params)
          result.success(null)
        } catch (e: Exception) {
          result.error("LOAD_ERROR", e.message, null)
        }
      }
      "infer" -> {
        val prompt = call.argument<String>("prompt") ?: ""
        val options = call.argument<Map<String, Any>>("options") ?: emptyMap()
        
        try {
          val output = modelWrapper?.infer(prompt, options) ?: ""
          result.success(output)
        } catch (e: Exception) {
          result.error("INFER_ERROR", e.message, null)
        }
      }
      "unloadModel" -> {
        try {
          modelWrapper?.unloadModel()
          modelWrapper = null
          result.success(null)
        } catch (e: Exception) {
          result.error("UNLOAD_ERROR", e.message, null)
        }
      }
      "isAvailable" -> result.success(true)
      "getModelInfo" -> {
        try {
          val info = modelWrapper?.getModelInfo() ?: emptyMap<String, Any>()
          result.success(info)
        } catch (e: Exception) {
          result.error("INFO_ERROR", e.message, null)
        }
      }
      else -> result.notImplemented()
    }
  }

  override fun onDetachedFromEngine(binding: FlutterPlugin.FlutterPluginBinding) {
    channel.setMethodCallHandler(null)
  }
}
class GGUFModelWrapper {
    // Загружаем нативную библиотеку
    init {
        System.loadLibrary("llamajni")
    }

    // Нативные методы
    private external fun nativeLoadModel(modelPath: String, numThreads: Int, numGpuLayers: Int): Long
    private external fun nativeInfer(modelPtr: Long, prompt: String, params: Map<String, Any>): String
    private external fun nativeUnloadModel(modelPtr: Long)
    private external fun nativeGetModelInfo(modelPtr: Long): Map<String, Any>

    private var modelPtr: Long = 0

    fun loadModel(modelPath: String, params: Map<String, Any>) {
        val numThreads = params["n_threads"] as? Int ?: 4
        val numGpuLayers = params["n_gpu_layers"] as? Int ?: 0
        
        modelPtr = nativeLoadModel(modelPath, numThreads, numGpuLayers)
        if (modelPtr == 0L) {
            throw Exception("Failed to load model")
        }
    }

    fun infer(prompt: String, options: Map<String, Any>): String {
        if (modelPtr == 0L) throw Exception("Model not loaded")
        return nativeInfer(modelPtr, prompt, options)
    }

    fun unloadModel() {
        if (modelPtr != 0L) {
            nativeUnloadModel(modelPtr)
            modelPtr = 0
        }
    }

    fun getModelInfo(): Map<String, Any> {
        if (modelPtr == 0L) throw Exception("Model not loaded")
        return nativeGetModelInfo(modelPtr)
    }
}