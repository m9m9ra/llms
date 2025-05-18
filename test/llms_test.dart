import 'package:flutter_test/flutter_test.dart';
import 'package:llms/llms.dart';
import 'package:llms/llms_platform_interface.dart';
import 'package:llms/llms_method_channel.dart';
import 'package:plugin_platform_interface/plugin_platform_interface.dart';

class MockLlmsPlatform
    with MockPlatformInterfaceMixin
    implements LlmsPlatform {

  @override
  Future<String?> getPlatformVersion() => Future.value('42');
}

void main() {
  final LlmsPlatform initialPlatform = LlmsPlatform.instance;

  test('$MethodChannelLlms is the default instance', () {
    expect(initialPlatform, isInstanceOf<MethodChannelLlms>());
  });

  test('getPlatformVersion', () async {
    Llms llmsPlugin = Llms();
    MockLlmsPlatform fakePlatform = MockLlmsPlatform();
    LlmsPlatform.instance = fakePlatform;

    expect(await llmsPlugin.getPlatformVersion(), '42');
  });
}
