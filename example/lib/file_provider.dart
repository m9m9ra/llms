import 'package:file_picker/file_picker.dart';
import 'package:file_selector/file_selector.dart';
import 'package:flutter/foundation.dart';

class FileProvider {
  Future<String?> openGgufFile() async {
    final filePath = await _pickGgufPath();
    if (filePath == null) {
      return null;
    } else {
      return filePath;
    }
  }

  Future<String?> _pickGgufPath() async {
    if (!kIsWeb && TargetPlatform.android == defaultTargetPlatform) {
      FilePickerResult? result = await FilePicker.platform.pickFiles(
        type: FileType.any,
      );

      return result?.files.first.path;
    }

    final file = await openFile(
      acceptedTypeGroups: <XTypeGroup>[
        XTypeGroup(
          // Only on iOS, macOS is fine.
          // [ERROR:flutter/runtime/dart_vm_initializer.cc(40)] Unhandled Exception: Invalid argument(s): The provided type group Instance of 'XTypeGroup' should either allow all files, or have a non-empty "uniformTypeIdentifiers"
          label: defaultTargetPlatform == TargetPlatform.iOS ? '' : '.gguf',
          extensions:
              defaultTargetPlatform == TargetPlatform.iOS ? [] : ['gguf'],
          // UTIs are required for iOS, which does not have a .gguf UTI.
          uniformTypeIdentifiers: const [],
        ),
      ],
    );

    if (file == null) {
      return null;
    }
    final filePath = file.path;
    return filePath;
  }
}