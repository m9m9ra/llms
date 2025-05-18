enum MessageRole {
  assistant,
  system,
  user,
  tool;

  String get messageRole {
    switch (this) {
      case MessageRole.assistant:
        return 'assistant';
      case MessageRole.system:
        return 'system';
      case MessageRole.user:
        return 'user';
      case MessageRole.tool:
        return 'tool';
    }
  }
}