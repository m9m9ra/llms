import 'dart:async';
import 'package:flutter/services.dart';

class Llms {
  static const MethodChannel _channel = MethodChannel('llms');

  /// Initializes the LLM model
  static Future<bool> initialize({required String modelPath}) async {
    return await _channel.invokeMethod('initialize', {'modelPath': modelPath});
  }

  /// Generates text completion
  static Future<String> complete(String prompt) async {
    return await _channel.invokeMethod('complete', {'prompt': prompt});
  }

  /// Runs a benchmark test
  static Future<String> benchmark() async {
    return await _channel.invokeMethod('benchmark');
  }

  /// Clears the current context
  static Future<void> clear() async {
    await _channel.invokeMethod('clear');
  }

  /// Gets available models
  static Future<List<Map<String, dynamic>>> getAvailableModels() async {
    final models = await _channel.invokeMethod('getAvailableModels');
    return List<Map<String, dynamic>>.from(models);
  }

  /// Downloads a model
  static Future<void> downloadModel(String url, String filename) async {
    await _channel.invokeMethod('downloadModel', {
      'url': url,
      'filename': filename,
    });
  }
}