import 'package:llms/llms_method_channel.dart';
import 'package:plugin_platform_interface/plugin_platform_interface.dart';

abstract class LlmsPlatform extends PlatformInterface {
  LlmsPlatform() : super(token: _token);

  static final Object _token = Object();
  static LlmsPlatform _instance = MethodChannelLlms();

  static LlmsPlatform get instance => _instance;
  static set instance(LlmsPlatform instance) {
    PlatformInterface.verifyToken(instance, _token);
    _instance = instance;
  }

  // Инициализация контекста модели
  Future<Map<String, dynamic>> initContext({
    required String modelPath,
    required int nGpuLayers,
    required bool isModelAsset,
    required int nCtx,
    required bool useMlock,
    required bool useMmap,
  });

  // Генерация текста
  Future<Map<String, dynamic>> completion({
    required int contextId,
    required String prompt,
    required int nPredict,
    required double temperature,
  });

  // Остановка генерации
  Future<void> stopCompletion(int contextId);

  // Освобождение ресурсов
  Future<void> releaseContext(int contextId);

  // Токенизация текста
  Future<List<int>> tokenize({
    required int contextId,
    required String text,
  });

  // Детокенизация в текст
  Future<String> detokenize({
    required int contextId,
    required List<int> tokens,
  });

  // Получение эмбеддингов
  Future<List<double>> embedding({
    required int contextId,
    required String text,
  });
}