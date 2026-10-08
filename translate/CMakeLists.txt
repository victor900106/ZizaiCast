# pm_translate: on-screen translation (PaddleOCR via ONNX Runtime, Windows.Media.Ocr
# as the fallback, + Bergamot / Firefox Translations models) drawn by pm_video's
# text overlay.  See docs/translate.md.
#
# The engine is a separate DLL (bergamot.dll, MPL-2.0, no BLAS), loaded at
# run time: build it once with translate/tools/build_bergamot.sh and ship it
# next to the executable.  PM_BERGAMOT_DLL (cache path) is copied next to the
# test tool / app when it exists.  onnxruntime.dll (MIT, the official 1.30.0
# CPU build; translate/tools/get_onnxruntime.sh) is loaded the same way:
# PM_ONNXRUNTIME_DLL.

if(NOT TARGET pm_video)
  message(FATAL_ERROR "pm_translate needs pm_video (add video before translate)")
endif()

add_library(pm_translate STATIC
  include/pm/translate.h
  src/engine.cpp
  src/label_text.cpp
  src/model_store.cpp
  src/models.inc
  src/ocr.cpp
  src/ocr_models.inc
  src/paddle_ocr.cpp
  src/screen_translator.cpp
  src/text_util.h
  src/text_util.cpp
)
target_include_directories(pm_translate PUBLIC include PRIVATE src third_party/onnxruntime/include)
target_link_libraries(pm_translate PUBLIC pm_video pm_media_api PRIVATE windowsapp winhttp bcrypt shell32 ole32)
target_compile_features(pm_translate PUBLIC cxx_std_20)
if(MSVC)
  target_compile_options(pm_translate PRIVATE /W4 /utf-8 /EHsc /bigobj)
  target_compile_definitions(pm_translate PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

set(PM_ONNXRUNTIME_DLL "${CMAKE_CURRENT_SOURCE_DIR}/../build-translate/ort/onnxruntime.dll" CACHE FILEPATH
    "onnxruntime.dll 1.30.0 (CPU) from translate/tools/get_onnxruntime.sh (copied next to the executables)")
set(PM_BERGAMOT_DLL "${CMAKE_CURRENT_SOURCE_DIR}/../build-translate/bergamot/bin/bergamot.dll" CACHE FILEPATH
    "bergamot.dll built by translate/tools/build_bergamot.sh (copied next to the executables)")

add_executable(pm_translate_test tools/pm_translate_test.cpp)
target_include_directories(pm_translate_test PRIVATE src)
target_link_libraries(pm_translate_test PRIVATE pm_translate pm_video windowscodecs ole32)
if(MSVC)
  target_compile_options(pm_translate_test PRIVATE /W4 /utf-8 /EHsc)
  target_compile_definitions(pm_translate_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()
if(EXISTS "${PM_BERGAMOT_DLL}")
  add_custom_command(TARGET pm_translate_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PM_BERGAMOT_DLL}" "$<TARGET_FILE_DIR:pm_translate_test>"
    VERBATIM)
endif()
if(EXISTS "${PM_ONNXRUNTIME_DLL}")
  add_custom_command(TARGET pm_translate_test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${PM_ONNXRUNTIME_DLL}" "$<TARGET_FILE_DIR:pm_translate_test>"
    VERBATIM)
endif()
