Pod::Spec.new do |s|
  s.name             = 'llms'
  s.version          = '0.0.1'
  s.summary          = 'A new Flutter plugin project.'
  s.homepage         = 'http://github.com/m9m9ra/llms'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'M9M9Ra' => 'vasa4g@gmail.com' }

  s.source_files = 'Classes/**/*'
  s.dependency 'Flutter'
  s.swift_version = '5.10'

  s.platform         = :ios, '16.0'
  s.source           = { :path => '.' }

  s.preserve_paths = 'llama.xcframework/**/*'
  s.xcconfig = { 'OTHER_LDFLAGS' => '-framework llama' }
  s.vendored_frameworks = 'llama.xcframework'
  
  s.pod_target_xcconfig = {
    'EXCLUDED_ARCHS[sdk=iphonesimulator*]' => 'i386',
    'DEFINES_MODULE' => 'YES'
  }
end
