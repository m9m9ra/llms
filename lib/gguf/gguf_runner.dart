// gguf_runner.dart
import 'package:flutter/services.dart';

class GGUFModel {
  final String modelPath;
  final Map<String, dynamic>? params;
  
  GGUFModel({required this.modelPath, this.params});
}

class GGUFRunner {
  static const MethodChannel _channel = MethodChannel('gguf_runner');
  
  /// Загружает модель в память
  static Future<void> loadModel(GGUFModel model) async {
    try {
      await _channel.invokeMethod('loadModel', {
        'modelPath': model.modelPath,
        'params': model.params ?? {},
      });
    } on PlatformException catch (e) {
      throw Exception("Failed to load model: ${e.message}");
    }
  }
  
  /// Выполняет инференс с заданным промптом
  static Future<String> infer(String prompt, {Map<String, dynamic>? options}) async {
    try {
      final result = await _channel.invokeMethod('infer', {
        'prompt': prompt,
        'options': options ?? {},
      });
      return result as String;
    } on PlatformException catch (e) {
      throw Exception("Inference failed: ${e.message}");
    }
  }
  
  /// Освобождает ресурсы модели
  static Future<void> unloadModel() async {
    try {
      await _channel.invokeMethod('unloadModel');
    } on PlatformException catch (e) {
      throw Exception("Failed to unload model: ${e.message}");
    }
  }
  
  /// Проверяет доступность GGUF runner на устройстве
  static Future<bool> isAvailable() async {
    try {
      final result = await _channel.invokeMethod('isAvailable');
      return result as bool;
    } on PlatformException {
      return false;
    }
  }
  
  /// Возвращает информацию о загруженной модели
  static Future<Map<String, dynamic>> getModelInfo() async {
    try {
      final result = await _channel.invokeMethod('getModelInfo');
      return Map<String, dynamic>.from(result);
    } on PlatformException catch (e) {
      throw Exception("Failed to get model info: ${e.message}");
    }
  }
}