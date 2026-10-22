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

# Optional engines implementing pm/translator.h (local LLM), found by
# file name; without them the library's default factories return nullptr.
option(PM_TRANSLATE_ENGINES "build the optional engines (src/llm_engine*.cpp)" ON)
set(PM_TRANSLATE_EXTRA_ENGINES "")
if(PM_TRANSLATE_ENGINES)
  file(GLOB PM_TRANSLATE_EXTRA_ENGINES CONFIGURE_DEPENDS
       ${CMAKE_CURRENT_SOURCE_DIR}/src/llm_engine*.cpp)
endif()

add_library(pm_translate STATIC
  include/pm/translate.h
  include/pm/translator.h
  ${PM_TRANSLATE_EXTRA_ENGINES}
  src/downloader.cpp
  src/downloader.h
  src/engine.cpp
  src/label_text.cpp
  src/layout_rules.cpp
  src/model_store.cpp
  src/models.inc
  src/ocr.cpp
  src/ocr_models.inc
  src/paddle_ocr.cpp
  src/preprocess.cpp
  src/qe.cpp
  src/screen_translator.cpp
  src/tech_text.cpp
  src/text_util.h
  src/text_util.cpp
  src/translator.cpp
  src/tr_log.cpp
  src/tr_log.h
)
target_include_directories(pm_translate PUBLIC include PRIVATE src third_party/onnxruntime/include)
target_link_libraries(pm_translate PUBLIC pm_video pm_media_api PRIVATE windowsapp winhttp bcrypt shell32 ole32)
if(PM_TEST_HTTP_DL)
  target_compile_definitions(pm_translate PRIVATE PM_DL_TEST_HTTP)
endif()
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

# Local LLM engine (src/llm_engine*.cpp, llama.dll downloaded on demand):
# evaluation / speed tool, see tools/llm_eval.py.
if(PM_TRANSLATE_ENGINES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/llm_engine.cpp")
  add_executable(pm_llm_eval tools/pm_llm_eval.cpp)
  target_include_directories(pm_llm_eval PRIVATE src)
  target_link_libraries(pm_llm_eval PRIVATE pm_translate psapi)
  if(MSVC)
    target_compile_options(pm_llm_eval PRIVATE /W4 /utf-8 /EHsc)
    target_compile_definitions(pm_llm_eval PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
  endif()

  # 0.8.3 whole-picture timing, cold / warm, RAM / VRAM, idle release
  # (translate/testdata/make_bench_083.py scenes; off-screen, no input).
  add_executable(pm_tr_bench tools/pm_tr_bench.cpp)
  target_link_libraries(pm_tr_bench PRIVATE pm_translate pm_video windowscodecs ole32 dxgi psapi)
  if(MSVC)
    target_compile_options(pm_tr_bench PRIVATE /W4 /utf-8 /EHsc)
    target_compile_definitions(pm_tr_bench PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
  endif()

  # Per-CPU runtime download (src/llm_engine_cpu.cpp + the ranged fetch in
  # src/llm_engine_store.cpp): offline CPUID-pick tests, and --fetch DIR
  # against a local server (PM_DL_TEST_HTTP: plain HTTP to 127.0.0.1 only in
  # this test build) or the real mirror.  Its own copies of the store /
  # downloader objects (with the test hooks) take precedence over pm_translate's.
  add_executable(pm_llm_runtime_test tools/llm_runtime_test.cpp
    src/llm_engine_cpu.cpp src/llm_engine_store.cpp src/downloader.cpp)
  target_include_directories(pm_llm_runtime_test PRIVATE include src)
  target_compile_definitions(pm_llm_runtime_test PRIVATE PM_DL_TEST_HTTP)
  target_link_libraries(pm_llm_runtime_test PRIVATE pm_translate winhttp bcrypt shell32)
  if(MSVC)
    target_compile_options(pm_llm_runtime_test PRIVATE /W4 /utf-8 /EHsc)
    target_compile_definitions(pm_llm_runtime_test PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
  endif()
endif()
