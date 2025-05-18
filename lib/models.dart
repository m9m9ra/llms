class LlamaContextInfo {
  final int contextId;
  final bool gpuEnabled;
  final String modelDescription;

  LlamaContextInfo({
    required this.contextId,
    required this.gpuEnabled,
    required this.modelDescription,
  });

  factory LlamaContextInfo.fromMap(Map<String, dynamic> map) {
    return LlamaContextInfo(
      contextId: map['contextId'] as int,
      gpuEnabled: map['gpu'] as bool,
      modelDescription: map['model']['desc'] as String,
    );
  }
}

class CompletionResult {
  final String text;
  final int tokensPredicted;
  final int tokensEvaluated;

  CompletionResult({
    required this.text,
    required this.tokensPredicted,
    required this.tokensEvaluated,
  });

  factory CompletionResult.fromMap(Map<String, dynamic> map) {
    return CompletionResult(
      text: map['text'] as String,
      tokensPredicted: map['tokens_predicted'] as int,
      tokensEvaluated: map['tokens_evaluated'] as int,
    );
  }
}