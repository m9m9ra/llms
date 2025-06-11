Pod::Spec.new do |s|
  s.name             = 'llms'
  s.version          = '0.0.1'
  s.summary          = 'A new Flutter plugin project.'
  s.homepage         = 'http://github.com/m9m9ra/llms'
  s.license          = { :file => '../LICENSE' }
  s.author           = { 'M9M9Ra' => 'vasa4g@gmail.com' }

  
  s.dependency 'Flutter'
  s.swift_version = '5.10'

  s.platform         = :ios, '16.0'
  s.source           = { :path => '.' }
  s.source_files = "Classes/**/*.{h,m,mm}", "Cpp/**/*.{h,cpp,hpp,c,m,mm}", "Cpp/**/*.{metal}"
  s.resources = "Cpp/**/*.{metallib}"

  s.compiler_flags = "-fno-objc-arc -DLM_GGML_USE_ACCELERATE -DLM_GGML_USE_METAL -DLM_GGML_METAL_NDEBUG -Wno-shorten-64-to-32 -Wno-comma"
  # including C++ library
  s.library = "c++"
  s.pod_target_xcconfig = {
     "MAKEFLAGS" => "-j8",
     "ENABLE_BITCODE" => "NO",
     "DEFINES_MODULE" => "YES",
     "LDPLUSPLUSFLAGS" => "-flto",
     "OTHER_LDFLAGS" => "-flto=thin -framework Accelerate -framework Foundation -framework Metal -framework MetalKit -lc++",
     "CLANG_CXX_LIBRARY" => "libc++",
     "CLANG_CXX_LANGUAGE_STANDARD" => "c++20",
     "OTHER_CFLAGS" => "-O3 -DNDEBUG -funroll-loops -fomit-frame-pointer -fvisibility-inlines-hidden -ffunction-sections -fdata-sections",
     "OTHER_CPLUSPLUSFLAGS" => "-O3 -DNDEBUG -funroll-loops -fomit-frame-pointer -fvisibility-inlines-hidden -ffunction-sections -fdata-sections",
     # Flutter.framework does not contain a i386 slice.
     "EXCLUDED_ARCHS[sdk=iphonesimulator*]" => "i386"
  }
  s.swift_version = '5.0'
  s.dependency 'Flutter'
  # Set as a static lib
  # s.static_framework = true
  # module_map is needed so this module can be used as a framework
  s.module_map = 'llms.modulemap'
  s.resource_bundles = {'llms_flutter_libs_apple_privacy' => ['Resources/PrivacyInfo.xcprivacy']}
end
