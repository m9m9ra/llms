import 'package:flutter/services.dart';
import 'llms_platform_interface.dart';

class MethodChannelLlms extends LlmsPlatform {
  static const MethodChannel _channel = MethodChannel('llms');

  @override
  Future<Map<String, dynamic>> initContext({
    required String modelPath,
    required int nGpuLayers,
    required bool isModelAsset,
    required int nCtx,
    required bool useMlock,
    required bool useMmap,
  }) async {
    final result = await _channel.invokeMethod<Map>('initContext', {
      'model': modelPath,
      'n_gpu_layers': nGpuLayers,
      'is_model_asset': isModelAsset,
      'n_ctx': nCtx,
      'use_mlock': useMlock,
      'use_mmap': useMmap,
    });
    return Map<String, dynamic>.from(result!);
  }

  @override
  Future<Map<String, dynamic>> completion({
    required int contextId,
    required String prompt,
    required int nPredict,
    required double temperature,
  }) async {
    final result = await _channel.invokeMethod<Map>('completion', {
      'contextId': contextId,
      'prompt': prompt,
      'n_predict': nPredict,
      'temperature': temperature,
    });
    return Map<String, dynamic>.from(result!);
  }

  @override
  Future<void> stopCompletion(int contextId) async {
    await _channel.invokeMethod('stopCompletion', {'contextId': contextId});
  }

  @override
  Future<void> releaseContext(int contextId) async {
    await _channel.invokeMethod('releaseContext', {'contextId': contextId});
  }

  @override
  Future<List<int>> tokenize({
    required int contextId,
    required String text,
  }) async {
    final result = await _channel.invokeMethod<List>('tokenize', {
      'contextId': contextId,
      'text': text,
    });
    return result!.cast<int>();
  }

  @override
  Future<String> detokenize({
    required int contextId,
    required List<int> tokens,
  }) async {
    return await _channel.invokeMethod<String>('detokenize', {
      'contextId': contextId,
      'tokens': tokens,
    }) ?? '';
  }

  @override
  Future<List<double>> embedding({
    required int contextId,
    required String text,
  }) async {
    final result = await _channel.invokeMethod<List>('embedding', {
      'contextId': contextId,
      'text': text,
    });
    return result!.cast<double>();
  }
}