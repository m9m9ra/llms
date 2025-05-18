import 'package:equatable/equatable.dart';
import 'package:llms_example/other/message_role.dart';

// ignore: must_be_immutable
class Message extends Equatable {
  Message({
    required this.uuid,
    required this.message,
    required this.createAt,
    required this.messageRole,
    this.deleate = false
  });

  String uuid;
  String message;
  DateTime createAt;
  MessageRole messageRole;

  bool deleate;

  @override
  List<Object> get props => [
    uuid,
    message,
    createAt,
    messageRole,
    deleate
  ];

  Map<String, dynamic> toMap () => {
    'uuid': uuid,
    'message': message,
    'createAt': createAt.toIso8601String(),
    'messageRole': messageRole,
    'deleate': deleate
  };

  factory Message.fromMap(Map<String, dynamic> json) => Message(
    uuid: json['uuid'],
    message: json['message'],
    createAt: DateTime.parse(json['createAt']),
    messageRole: json['messageRole'],
    deleate: json['deleate']
  );
}
