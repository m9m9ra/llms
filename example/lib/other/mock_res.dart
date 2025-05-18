import 'package:llms_example/other/message.dart';
import 'package:llms_example/other/message_role.dart';
import 'package:uuid/uuid.dart';

class MockRes {
  MockRes._();

  static final Message assistantRes = Message(
    uuid: Uuid().v1(),
    message: '''Ох, мне очень жаль слышать, что ты так устал. Это действительно тяжело. Давай поговорим. 

Как именно ты устал? Что произошло, что вызвало это чувство усталости? Если тебе не хочется вдаваться в подробности, это тоже нормально. Просто дай мне знать, что чувствуешь.

Я здесь, чтобы выслушать тебя, поддержать и, возможно, просто немного отвлечь. Если тебе хочется, я могу:

*   Поговорить с тобой о чем-нибудь другом: Расскажи мне о своем хобби, о фильме, который ты недавно посмотрел, о чем-нибудь, что тебя радует.
*   Предложить тебе что-нибудь расслабляющее: Например, рассказать анекдот, предложить послушать музыку, или просто немного почитать.
*   Просто побыть рядом: Иногда просто знать, что кто-то рядом и готов выслушать, уже помогает.

В любом случае, я хочу, чтобы ты знал, что ты не один.  Тебе не нужно справляться с этим в одиночку.  

Что ты чувствуешь в данный момент? Хочешь ли ты просто выговориться, или тебе нужна конкретная помощь?
            ''',
    createAt: DateTime.now(),
    messageRole: MessageRole.assistant,
  );

  static final Message userRes = Message(
    uuid: Uuid().v1(),
    message: 'Как дела, я сегодня очень устал и хочу поддержки, сегодня очень устал и хочу поддержки, и хочу поддержкset',
    createAt: DateTime.now(),
    messageRole: MessageRole.user,
  );

  static final List<Message> mockedMessageList = [
    userRes,
    assistantRes,
    userRes,
    assistantRes,
    userRes,
    assistantRes,
  ];
}
