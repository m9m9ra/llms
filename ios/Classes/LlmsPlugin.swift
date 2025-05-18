import Flutter
import UIKit
import llama

public class LlmsPlugin: NSObject, FlutterPlugin {
  private var llamaContext: LlamaContext?

  public static func register(with registrar: FlutterPluginRegistrar) {
    let channel = FlutterMethodChannel(name: "llms", binaryMessenger: registrar.messenger())
    let instance = LlmsPlugin()
    registrar.addMethodCallDelegate(instance, channel: channel)
  }

  public func handle(_ call: FlutterMethodCall, result: @escaping FlutterResult) {
    switch call.method {
    default:
      result(FlutterMethodNotImplemented)
    }
  }
}