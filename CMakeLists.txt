cmake_minimum_required(VERSION 3.24)
project(MediaEngineFoundation VERSION 0.4.0 LANGUAGES C CXX)

add_library(media_core STATIC src/linear_memory_arena.cpp src/platform/OutputFile.cpp)
target_include_directories(media_core PUBLIC include)
target_compile_features(media_core PUBLIC cxx_std_20)
if(WIN32)
    target_sources(media_core PRIVATE src/platform/windows/SystemMemory.cpp)
    target_compile_definitions(media_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(media_core PRIVATE kernel32)
    add_library(media_windows STATIC src/platform/windows/Win32IocpBlockSource.cpp src/platform/windows/HttpBlockSource.cpp)
    target_link_libraries(media_windows PUBLIC media_core winhttp PRIVATE kernel32 mmdevapi) # mmdevapi: ActivateAudioInterfaceAsync (process loopback).
    target_compile_definitions(media_windows PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX)
else()
    target_sources(media_core PRIVATE src/platform/posix/SystemMemory.cpp)
endif()

add_library(media_demux STATIC src/demux/IsobmffParser.cpp src/demux/MatroskaReader.cpp src/demux/MatroskaDemuxer.cpp src/demux/Demuxer.cpp)
target_link_libraries(media_demux PUBLIC media_core)

add_library(media_timing STATIC src/clock/PredictiveMasterClock.cpp src/audio/PcmRingBuffer.cpp src/audio/DspProcessor.cpp src/audio/Loudness.cpp src/audio/MicroVad.cpp
    src/audio/FilterbankFeatures.cpp src/audio/PcmConverter.cpp src/audio/TimeStretcher.cpp src/audio/SpectrumAnalyser.cpp src/audio/MasteringEq.cpp src/audio/SoundLocator.cpp)
target_link_libraries(media_timing PUBLIC media_core)
# Preserve separate multiply/add rounding in the SSE, NEON and scalar DSP implementations.
# SSE2 is baseline on x64; do not require AVX or impose x86 flags on ARM.
if(MSVC)
    set_source_files_properties(src/audio/DspProcessor.cpp src/audio/MasteringEq.cpp src/audio/SoundLocator.cpp src/video/Perception.cpp src/video/FlowRefiner.cpp PROPERTIES COMPILE_OPTIONS /fp:precise)
else()
    set_source_files_properties(src/audio/DspProcessor.cpp src/audio/MasteringEq.cpp src/audio/SoundLocator.cpp src/video/Perception.cpp src/video/FlowRefiner.cpp PROPERTIES COMPILE_OPTIONS -ffp-contract=off)
endif()
add_library(media_codec STATIC src/codec/Rbsp.cpp src/codec/H264SyntaxParser.cpp src/codec/HevcSyntaxParser.cpp src/codec/AacConfig.cpp
    src/codec/BitstreamPerception.cpp)
target_link_libraries(media_codec PUBLIC media_core)
add_library(media_neural STATIC src/video/NeuralUpscale.cpp)
target_link_libraries(media_neural PUBLIC media_core)
# Reconstruction-tier policy from measured GPU time (docs/picture.md, "Quality governor"); no GPU or platform dependency.
add_library(media_quality STATIC src/render/QualityGovernor.cpp)
target_link_libraries(media_quality PUBLIC media_core)
add_library(media_ui STATIC src/ui/KineticHud.cpp src/ui/CommandPalette.cpp src/ui/StatusCue.cpp src/ui/TelemetryReadout.cpp src/ui/PictureAudioCures.cpp)
target_link_libraries(media_ui PUBLIC media_core media_text media_quality)
# UTF-8 decoding and search folding, shared by the subtitle search and the command grammar.
add_library(media_text STATIC src/text/Utf8.cpp)
target_link_libraries(media_text PUBLIC media_core)
# Chapters and scenes: navigation, structural synthesis, live cut detection (docs/chapters.md); no platform dependency.
add_library(media_timeline STATIC src/timeline/ChapterTimeline.cpp src/timeline/LoopRegion.cpp)
target_link_libraries(media_timeline PUBLIC media_core media_text)
add_library(media_subtitle STATIC src/subtitle/SubtitleTrack.cpp src/subtitle/SubtitleSearch.cpp src/subtitle/EmbeddedText.cpp)
target_link_libraries(media_subtitle PUBLIC media_core media_text)
# Offline command grammar (nine languages) to typed engine actions; no engine or platform dependency.
add_library(media_ai STATIC src/ai/LocalIntentDispatcher.cpp src/ai/AcousticIntentRecognizer.cpp src/ai/FormantSynthesizer.cpp src/ai/AvionWakeGate.cpp src/ai/CustomWakeWord.cpp
    src/ai/ConceptSearch.cpp)
target_link_libraries(media_ai PUBLIC media_core media_text media_timing media_subtitle)
# Sessions across files: the resume ledger and the folder playlist (portable; the platform reads and lists the files).
add_library(media_session STATIC src/session/MediaSession.cpp)
target_link_libraries(media_session PUBLIC media_core)
# Target tracking for the zoom and the audio focus (docs/picture.md, "Target tracking"); portable.
add_library(media_tracker STATIC src/video/TargetTracker.cpp src/video/Perception.cpp src/video/Stabilizer.cpp src/video/Orientation.cpp src/video/HorizonLevel.cpp src/video/Matte.cpp
    src/video/Snapshot.cpp src/video/Vibrometry.cpp src/video/VirtualImu.cpp src/video/EventCamera.cpp src/video/TensorExport.cpp src/video/FlowRefiner.cpp)
target_link_libraries(media_tracker PUBLIC media_core)
# Host-driven playback (docs/sdk.md, "Host-driven playback"): the portable engine, its file source, the H.264 software bridge.
add_library(media_host STATIC src/engine/HostEngine.cpp src/platform/FileBlockSource.cpp src/codec/H264PcmDecoder.cpp)
target_link_libraries(media_host PUBLIC media_demux media_codec media_ai media_timing media_timeline media_subtitle media_ui media_tracker)
if(WIN32)
    target_compile_definitions(media_host PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()
if(WIN32)
    target_sources(media_windows PRIVATE src/platform/windows/H264Dxva.cpp src/platform/windows/HevcDxva.cpp src/platform/windows/SessionFiles.cpp)
    target_link_libraries(media_windows PUBLIC media_codec media_ui media_timeline media_session)
    find_program(MEDIA_FXC fxc.exe HINTS "C:/Program Files (x86)/Windows Kits/10/bin/${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}/x64" REQUIRED)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/Nv12Shader.hpp"
        COMMAND "${MEDIA_FXC}" /nologo /WX /T cs_5_0 /E CS /Vn g_Nv12Shader
        /Fh "${CMAKE_CURRENT_BINARY_DIR}/generated/Nv12Shader.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/render/Nv12ToRgb.hlsl"
        DEPENDS src/render/Nv12ToRgb.hlsl src/render/Reconstruct.hlsli VERBATIM)
    # The split-comparison variant of the conversion (Reconstruct.hlsli, SPLIT_COMPARE): a pipeline of its own, so the
    # production shader carries none of its code.
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/Nv12CompareShader.hpp"
        COMMAND "${MEDIA_FXC}" /nologo /WX /T cs_5_0 /E CS /D SPLIT_COMPARE=1 /Vn g_Nv12CompareShader
        /Fh "${CMAKE_CURRENT_BINARY_DIR}/generated/Nv12CompareShader.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/render/Nv12ToRgb.hlsl"
        DEPENDS src/render/Nv12ToRgb.hlsl src/render/Reconstruct.hlsli VERBATIM)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/HdrCompareShader.hpp"
        COMMAND "${MEDIA_FXC}" /nologo /WX /T cs_5_0 /E CS /D SPLIT_COMPARE=1 /Vn g_HdrCompareShader
        /Fh "${CMAKE_CURRENT_BINARY_DIR}/generated/HdrCompareShader.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/render/HdrToneMap.hlsl"
        DEPENDS src/render/HdrToneMap.hlsl src/render/Reconstruct.hlsli VERBATIM)
    # Picture controls and micro-motion (Reconstruct.hlsli, PICTURE_CONTROLS): variants of their own, alone and with the split
    # comparison, so the production shaders carry none of their code.
    foreach(entry IN ITEMS "Nv12ToRgb;Nv12PictureShader;PICTURE_CONTROLS=1;" "Nv12ToRgb;Nv12ComparePictureShader;PICTURE_CONTROLS=1;SPLIT_COMPARE=1"
                           "HdrToneMap;HdrPictureShader;PICTURE_CONTROLS=1;" "HdrToneMap;HdrComparePictureShader;PICTURE_CONTROLS=1;SPLIT_COMPARE=1")
        list(GET entry 0 source)
        list(GET entry 1 header)
        list(GET entry 2 first)
        list(GET entry 3 second)
        set(defines /D ${first})
        if(second)
            list(APPEND defines /D ${second})
        endif()
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/${header}.hpp"
            COMMAND "${MEDIA_FXC}" /nologo /WX /T cs_5_0 /E CS ${defines} /Vn g_${header}
            /Fh "${CMAKE_CURRENT_BINARY_DIR}/generated/${header}.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/render/${source}.hlsl"
            DEPENDS src/render/${source}.hlsl src/render/Reconstruct.hlsli VERBATIM)
    endforeach()
    foreach(entry IN ITEMS "CS;g_HdrShader;HdrShader" "Peak;g_HdrPeakShader;HdrPeakShader" "Detect;g_HdrDetectShader;HdrDetectShader"
                           "Stats;g_SceneStatsShader;SceneStatsShader" "StatsReset;g_StatsResetShader;StatsResetShader" "Grid;g_GridShader;GridShader"
                           "Vibro;g_VibroShader;VibroShader" "Tensor;g_TensorShader;TensorShader")
        list(GET entry 0 entry_point)
        list(GET entry 1 variable)
        list(GET entry 2 header)
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/${header}.hpp"
            COMMAND "${MEDIA_FXC}" /nologo /WX /T cs_5_0 /E ${entry_point} /Vn ${variable}
            /Fh "${CMAKE_CURRENT_BINARY_DIR}/generated/${header}.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/render/HdrToneMap.hlsl"
            DEPENDS src/render/HdrToneMap.hlsl src/render/Reconstruct.hlsli VERBATIM)
    endforeach()
    foreach(stage IN ITEMS Vertex Pixel)
        if(stage STREQUAL "Vertex")
            set(profile vs_5_0)
            set(entry_point VS)
        else()
            set(profile ps_5_0)
            set(entry_point PS)
        endif()
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/Hud${stage}Shader.hpp"
            COMMAND "${MEDIA_FXC}" /nologo /WX /T ${profile} /E ${entry_point} /Vn g_Hud${stage}Shader
            /Fh "${CMAKE_CURRENT_BINARY_DIR}/generated/Hud${stage}Shader.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/render/HudComposite.hlsl"
            DEPENDS src/render/HudComposite.hlsl VERBATIM)
    endforeach()
    target_sources(media_windows PRIVATE src/render/HudPipeline.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/generated/HudVertexShader.hpp" "${CMAKE_CURRENT_BINARY_DIR}/generated/HudPixelShader.hpp")
    # Music mode: the sound as analytical geometry (spectrum ring, orb, waveform ribbon) from root constants.
    foreach(stage IN ITEMS Vertex Pixel)
        if(stage STREQUAL "Vertex")
            set(profile vs_5_0)
            set(entry_point VS)
        else()
            set(profile ps_5_0)
            set(entry_point PS)
        endif()
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/Visual${stage}Shader.hpp"
            COMMAND "${MEDIA_FXC}" /nologo /WX /T ${profile} /E ${entry_point} /Vn g_Visual${stage}Shader
            /Fh "${CMAKE_CURRENT_BINARY_DIR}/generated/Visual${stage}Shader.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/render/VisualComposite.hlsl"
            DEPENDS src/render/VisualComposite.hlsl VERBATIM)
    endforeach()
    target_sources(media_windows PRIVATE src/render/VisualPipeline.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/generated/VisualVertexShader.hpp" "${CMAKE_CURRENT_BINARY_DIR}/generated/VisualPixelShader.hpp")
    # Command bar: analytical glass and caret, DirectWrite text coverage read from a buffer.
    foreach(stage IN ITEMS Vertex Pixel)
        if(stage STREQUAL "Vertex")
            set(profile vs_5_0)
            set(entry_point VS)
        else()
            set(profile ps_5_0)
            set(entry_point PS)
        endif()
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/Palette${stage}Shader.hpp"
            COMMAND "${MEDIA_FXC}" /nologo /WX /T ${profile} /E ${entry_point} /Vn g_Palette${stage}Shader
            /Fh "${CMAKE_CURRENT_BINARY_DIR}/generated/Palette${stage}Shader.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/render/PaletteComposite.hlsl"
            DEPENDS src/render/PaletteComposite.hlsl VERBATIM)
    endforeach()
    target_sources(media_windows PRIVATE src/render/PalettePipeline.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/generated/PaletteVertexShader.hpp" "${CMAKE_CURRENT_BINARY_DIR}/generated/PalettePixelShader.hpp")
    # The HUD's status pill (render::StatusPipeline): glass, icon and a DirectWrite text raster.
    foreach(stage IN ITEMS Vertex Pixel)
        if(stage STREQUAL "Vertex")
            set(profile vs_5_0)
            set(entry_point VS)
        else()
            set(profile ps_5_0)
            set(entry_point PS)
        endif()
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/Status${stage}Shader.hpp"
            COMMAND "${MEDIA_FXC}" /nologo /WX /T ${profile} /E ${entry_point} /Vn g_Status${stage}Shader
            /Fh "${CMAKE_CURRENT_BINARY_DIR}/generated/Status${stage}Shader.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/render/StatusComposite.hlsl"
            DEPENDS src/render/StatusComposite.hlsl VERBATIM)
    endforeach()
    target_sources(media_windows PRIVATE src/render/StatusPipeline.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/generated/StatusVertexShader.hpp" "${CMAKE_CURRENT_BINARY_DIR}/generated/StatusPixelShader.hpp")
    target_link_libraries(media_windows PRIVATE gdi32 imm32)
    foreach(stage IN ITEMS Vertex Pixel Atlas)
        if(stage STREQUAL "Vertex")
            set(profile vs_5_0)
            set(entry_point VS)
            set(shader SubtitleComposite)
        elseif(stage STREQUAL "Pixel")
            set(profile ps_5_0)
            set(entry_point PS)
            set(shader SubtitleComposite)
        else()
            set(profile cs_5_0)
            set(entry_point BuildAtlas)
            set(shader SubtitleSdf)
        endif()
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/Subtitle${stage}Shader.hpp"
            COMMAND "${MEDIA_FXC}" /nologo /WX /T ${profile} /E ${entry_point} /Vn g_Subtitle${stage}Shader
            /Fh "${CMAKE_CURRENT_BINARY_DIR}/generated/Subtitle${stage}Shader.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/render/${shader}.hlsl"
            DEPENDS "src/render/${shader}.hlsl" VERBATIM)
        target_sources(media_windows PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/Subtitle${stage}Shader.hpp")
    endforeach()
    foreach(stage IN ITEMS Vertex Pixel)
        if(stage STREQUAL "Vertex")
            set(profile vs_5_0)
            set(entry_point VS)
        else()
            set(profile ps_5_0)
            set(entry_point PS)
        endif()
        add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/Blit${stage}Shader.hpp"
            COMMAND "${MEDIA_FXC}" /nologo /WX /T ${profile} /E ${entry_point} /Vn g_Blit${stage}Shader
            /Fh "${CMAKE_CURRENT_BINARY_DIR}/generated/Blit${stage}Shader.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/render/PictureBlit.hlsl"
            DEPENDS src/render/PictureBlit.hlsl VERBATIM)
        target_sources(media_windows PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/Blit${stage}Shader.hpp")
    endforeach()
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/NeuralShader.hpp"
        COMMAND "${MEDIA_FXC}" /nologo /WX /O3 /T cs_5_0 /E CS /Vn g_NeuralShader
        /Fh "${CMAKE_CURRENT_BINARY_DIR}/generated/NeuralShader.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/render/NeuralSuperRes.hlsl"
        DEPENDS src/render/NeuralSuperRes.hlsl VERBATIM)
    target_sources(media_windows PRIVATE src/render/NeuralUpscalePass.cpp "${CMAKE_CURRENT_BINARY_DIR}/generated/NeuralShader.hpp")
    target_link_libraries(media_windows PUBLIC media_neural media_quality)
    target_sources(media_windows PRIVATE src/render/SdfSubtitlePipeline.cpp)
    target_link_libraries(media_windows PUBLIC media_subtitle PRIVATE dwrite)
    target_sources(media_windows PRIVATE src/render/D3D12Renderer.cpp src/render/ColorPipeline.cpp src/render/LiveGpuPipeline.cpp src/render/SharedOutput.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/generated/Nv12Shader.hpp" "${CMAKE_CURRENT_BINARY_DIR}/generated/HdrShader.hpp"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/HdrPeakShader.hpp" "${CMAKE_CURRENT_BINARY_DIR}/generated/HdrDetectShader.hpp"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/SceneStatsShader.hpp" "${CMAKE_CURRENT_BINARY_DIR}/generated/StatsResetShader.hpp"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/GridShader.hpp"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/VibroShader.hpp"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/TensorShader.hpp"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/Nv12CompareShader.hpp"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/Nv12PictureShader.hpp" "${CMAKE_CURRENT_BINARY_DIR}/generated/Nv12ComparePictureShader.hpp"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/HdrPictureShader.hpp" "${CMAKE_CURRENT_BINARY_DIR}/generated/HdrComparePictureShader.hpp"
        "${CMAKE_CURRENT_BINARY_DIR}/generated/HdrCompareShader.hpp")
    target_include_directories(media_windows PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(media_windows PRIVATE user32 PUBLIC media_tracker)
    add_library(media_engine STATIC src/engine/PlaybackCoordinator.cpp)
    target_link_libraries(media_engine PUBLIC media_windows media_demux media_ai media_timeline)
    add_executable(media_player_demo apps/media_player_demo.cpp tests/allocation_probe.cpp)
    target_include_directories(media_player_demo PRIVATE tests)
    target_link_libraries(media_player_demo PRIVATE media_engine ws2_32)
    target_sources(media_windows PRIVATE src/platform/windows/Win32WasapiAudioSink.cpp
        src/platform/windows/D3D12VideoDecoder.cpp src/platform/windows/Win32AacDecoder.cpp src/platform/windows/Win32VideoMftDecoder.cpp
        src/platform/windows/Win32WasapiCapture.cpp)
    target_link_libraries(media_windows PUBLIC media_timing PRIVATE ole32 uuid d3d12 dxgi dxguid dwmapi mfplat mfuuid wmcodecdspuuid)
    # The system's voices for custom wake words (docs/voice.md, "Custom wake words").
    add_library(media_speech STATIC src/platform/windows/SapiSpeech.cpp)
    target_link_libraries(media_speech PUBLIC media_ai PRIVATE ole32 sapi)
    target_compile_definitions(media_speech PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(media_engine PUBLIC media_speech)
endif()

# The C99 SDK (docs/sdk.md): configuration, budgets and host-driven playback everywhere, the windowed engine on Windows.
add_library(media_sdk STATIC src/sdk/CApi.cpp src/sdk/Probe.cpp) # Probe: media_probe (API 15).
target_link_libraries(media_sdk PUBLIC media_core media_host media_demux media_codec)
if(WIN32)
    target_sources(media_sdk PRIVATE src/sdk/CApiEngine.cpp src/sdk/HostGpu.cpp) # API 14: host mode's GPU decoding.
    target_link_libraries(media_sdk PUBLIC media_engine)
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Threads REQUIRED)
    add_executable(media_subtitle_tests tests/subtitle_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_subtitle_tests PRIVATE media_subtitle)
    add_test(NAME media_subtitle_tests COMMAND media_subtitle_tests)
    add_executable(media_quality_governor_tests tests/quality_governor_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_quality_governor_tests PRIVATE media_quality)
    add_test(NAME media_quality_governor_tests COMMAND media_quality_governor_tests)
    set_tests_properties(media_quality_governor_tests PROPERTIES TIMEOUT 30)
    add_executable(media_neural_reference_tests tests/neural_reference_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_neural_reference_tests PRIVATE media_neural)
    add_test(NAME media_neural_reference_tests COMMAND media_neural_reference_tests)
    set_tests_properties(media_neural_reference_tests PROPERTIES TIMEOUT 60)
    add_executable(media_aac_config_tests tests/aac_config_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_aac_config_tests PRIVATE media_codec)
    add_test(NAME media_aac_config_tests COMMAND media_aac_config_tests)
    set_tests_properties(media_aac_config_tests PROPERTIES TIMEOUT 60)
    set_tests_properties(media_subtitle_tests PROPERTIES TIMEOUT 30)
    add_executable(media_subtitle_search_tests tests/subtitle_search_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_subtitle_search_tests PRIVATE media_subtitle)
    add_test(NAME media_subtitle_search_tests COMMAND media_subtitle_search_tests)
    set_tests_properties(media_subtitle_search_tests PROPERTIES TIMEOUT 120)
    add_executable(media_intent_dispatcher_tests tests/intent_dispatcher_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_intent_dispatcher_tests PRIVATE media_ai)
    add_test(NAME media_intent_dispatcher_tests COMMAND media_intent_dispatcher_tests)
    set_tests_properties(media_intent_dispatcher_tests PROPERTIES TIMEOUT 60)
    # Concept search over subtitles: the lexicon, word readings, ranking in Hebrew and English, and the real film's lines.
    add_executable(media_semantic_search_tests tests/semantic_search_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_semantic_search_tests PRIVATE media_ai)
    add_test(NAME media_semantic_search_tests COMMAND media_semantic_search_tests "${CMAKE_CURRENT_SOURCE_DIR}/assets/test_fixtures/big_buck_bunny.srt")
    set_tests_properties(media_semantic_search_tests PROPERTIES TIMEOUT 60)
    # Sessions: resume rules, the 64-entry ledger saved and loaded, path keys, the folder in natural order.
    add_executable(media_session_tests tests/session_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_session_tests PRIVATE media_session)
    add_test(NAME media_session_tests COMMAND media_session_tests)
    set_tests_properties(media_session_tests PROPERTIES TIMEOUT 30)
    # Picture description: viewport arithmetic (crop, sample aspect ratio, letterboxing) and colour description
    # resolution (container against bitstream). Header-only and portable.
    add_executable(media_picture_geometry_tests tests/picture_geometry_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_picture_geometry_tests PRIVATE media_core)
    add_test(NAME media_picture_geometry_tests COMMAND media_picture_geometry_tests)
    set_tests_properties(media_picture_geometry_tests PROPERTIES TIMEOUT 60)
    # Voice activity detection (energy and zero crossings) and its junction with the command grammar, on synthetic
    # speech, silence and noise with known boundaries.
    add_executable(media_vad_tests tests/vad_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_vad_tests PRIVATE media_timing media_ai)
    add_test(NAME media_vad_tests COMMAND media_vad_tests)
    set_tests_properties(media_vad_tests PROPERTIES TIMEOUT 120)
    # The acoustic end of the voice path (filterbank features, template matching) on synthetic words and on the corpus
    # of voices that are in no template (tests/data/voice_corpus.bin, from tools/make_voice_templates.cpp).
    add_executable(media_acoustic_intent_tests tests/acoustic_intent_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_acoustic_intent_tests PRIVATE media_ai media_timing)
    add_test(NAME media_acoustic_intent_tests COMMAND media_acoustic_intent_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/voice_corpus.bin"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/voice_sentences.bin")
    set_tests_properties(media_acoustic_intent_tests PROPERTIES TIMEOUT 300) # 134 s in Debug with 2,367 templates.
    add_executable(media_avion_wake_tests tests/avion_wake_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_avion_wake_tests PRIVATE media_ai media_timing)
    add_test(NAME media_avion_wake_tests COMMAND media_avion_wake_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/voice_wake.bin"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/voice_sentences.bin")
    set_tests_properties(media_avion_wake_tests PROPERTIES TIMEOUT 600) # 60 s Release, 246 s Debug.
    # Custom wake words (docs/voice.md): spelling to sound, synthesised templates, the path on Windows and synthetic voices.
    add_executable(media_custom_wake_tests tests/custom_wake_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_custom_wake_tests PRIVATE media_ai media_timing)
    if(WIN32)
        target_link_libraries(media_custom_wake_tests PRIVATE media_speech)
    endif()
    add_test(NAME media_custom_wake_tests COMMAND media_custom_wake_tests "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/voice_custom_wake.bin")
    set_tests_properties(media_custom_wake_tests PROPERTIES TIMEOUT 900) # About 75 s Release, 7 minutes Debug.
    add_executable(media_formant_synth_tests tests/formant_synth_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_formant_synth_tests PRIVATE media_ai)
    add_test(NAME media_formant_synth_tests COMMAND media_formant_synth_tests)
    set_tests_properties(media_formant_synth_tests PROPERTIES TIMEOUT 120)
    add_executable(media_pcm_converter_tests tests/pcm_converter_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_pcm_converter_tests PRIVATE media_timing)
    add_test(NAME media_pcm_converter_tests COMMAND media_pcm_converter_tests)
    set_tests_properties(media_pcm_converter_tests PROPERTIES TIMEOUT 120)
    add_executable(media_audio_dsp_tests tests/audio_dsp_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_audio_dsp_tests PRIVATE media_timing)
    add_test(NAME media_audio_dsp_tests COMMAND media_audio_dsp_tests)
    set_tests_properties(media_audio_dsp_tests PROPERTIES TIMEOUT 60)
    # The command bar's model: multilingual typing, input-method compositions, motion.
    add_executable(media_command_palette_tests tests/command_palette_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_command_palette_tests PRIVATE media_ui)
    add_test(NAME media_command_palette_tests COMMAND media_command_palette_tests)
    set_tests_properties(media_command_palette_tests PROPERTIES TIMEOUT 30)
    add_executable(media_hud_tests tests/hud_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_hud_tests PRIVATE media_ui)
    add_test(NAME media_hud_tests COMMAND media_hud_tests)
    set_tests_properties(media_hud_tests PROPERTIES TIMEOUT 30)
    add_executable(media_codec_tests tests/codec_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_codec_tests PRIVATE media_codec)
    add_test(NAME media_codec_tests COMMAND media_codec_tests)
    set_tests_properties(media_codec_tests PROPERTIES TIMEOUT 30)
    add_executable(media_hevc_codec_tests tests/hevc_codec_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_hevc_codec_tests PRIVATE media_codec)
    add_test(NAME media_hevc_codec_tests COMMAND media_hevc_codec_tests)
    set_tests_properties(media_hevc_codec_tests PROPERTIES TIMEOUT 60)
    add_executable(media_timing_tests tests/timing_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_timing_tests PRIVATE media_timing Threads::Threads)
    add_test(NAME media_timing_tests COMMAND media_timing_tests)
    # Pitch-preserving playback speed: exact 1x, tones and voiced sounds at 0.25x-2x, changes of speed, no allocation.
    add_executable(media_time_stretch_tests tests/time_stretch_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_time_stretch_tests PRIVATE media_timing)
    add_test(NAME media_time_stretch_tests COMMAND media_time_stretch_tests)
    set_tests_properties(media_time_stretch_tests PROPERTIES TIMEOUT 120)
    # BS.1770 loudness and night mode (docs/audio-dsp.md).
    add_executable(media_loudness_tests tests/loudness_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_loudness_tests PRIVATE media_timing)
    add_test(NAME media_loudness_tests COMMAND media_loudness_tests)
    set_tests_properties(media_loudness_tests PROPERTIES TIMEOUT 120)
    # Mastering EQ, notch killer, ducking, iso-loudness, audio zoom, cures; on Windows also the picture controls and
    # micro-motion through the colour pipeline (docs/audio-dsp.md, docs/picture.md).
    add_executable(media_eq_picture_tests tests/eq_picture_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_eq_picture_tests PRIVATE media_timing media_ai media_ui)
    if(WIN32)
        target_sources(media_eq_picture_tests PRIVATE tests/eq_picture_gpu.cpp)
        target_link_libraries(media_eq_picture_tests PRIVATE media_windows d3d12 dxgi dxguid)
    endif()
    add_test(NAME media_eq_picture_tests COMMAND media_eq_picture_tests)
    set_tests_properties(media_eq_picture_tests PROPERTIES TIMEOUT 120)
    # The portable fixture (I_PCM H.264, two PCM tracks, chapters), written by a portable tool on every platform.
    add_executable(media_make_portable_fixture tests/make_portable_fixture.cpp)
    target_compile_features(media_make_portable_fixture PRIVATE cxx_std_20)
    add_test(NAME media_make_portable_fixture COMMAND media_make_portable_fixture "${CMAKE_CURRENT_BINARY_DIR}/portable_fixture.mp4")
    set_tests_properties(media_make_portable_fixture PROPERTIES FIXTURES_SETUP portable_fixture TIMEOUT 30)
    # The C99 SDK from a pure C translation unit: host-driven playback of the portable fixture everywhere; on Windows it also
    # plays the chapters fixture through the windowed engine.
    # GPU live ingest through the C SDK (docs/sdk.md, "GPU live ingest"): picture controls, micro-motion, stabilisation and 2x.
    add_executable(media_gpu_live_ingest_tests tests/gpu_live_ingest_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_gpu_live_ingest_tests PRIVATE media_sdk)
    target_include_directories(media_gpu_live_ingest_tests PRIVATE tests)
    add_test(NAME media_gpu_live_ingest_tests COMMAND media_gpu_live_ingest_tests)
    set_tests_properties(media_gpu_live_ingest_tests PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    # Host-mode parity (API 17): tracks and their controls, subtitles after open, snapshots into memory, Matroska subtitles.
    add_executable(media_host_tracks_tests tests/host_tracks_tests.c tests/allocation_probe_c.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_host_tracks_tests PRIVATE media_sdk)
    target_include_directories(media_host_tracks_tests PRIVATE tests)
    set_target_properties(media_host_tracks_tests PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(WIN32)
        target_compile_definitions(media_host_tracks_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(media_host_tracks_tests PRIVATE shell32)
    endif()
    add_test(NAME media_host_tracks_tests COMMAND media_host_tracks_tests "${CMAKE_CURRENT_BINARY_DIR}/portable_fixture.mp4" "${CMAKE_CURRENT_SOURCE_DIR}/assets/test_fixtures/mkv/h264_aac_srt_chapters.mkv"
             WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(media_host_tracks_tests PROPERTIES FIXTURES_REQUIRED portable_fixture TIMEOUT 120)
    add_executable(media_c_api_tests tests/c_api_tests.c tests/allocation_probe_c.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_c_api_tests PRIVATE media_sdk)
    target_include_directories(media_c_api_tests PRIVATE tests)
    set_target_properties(media_c_api_tests PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    if(WIN32)
        target_compile_definitions(media_c_api_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_link_libraries(media_c_api_tests PRIVATE shell32)
        add_test(NAME media_c_api_tests COMMAND media_c_api_tests "${CMAKE_CURRENT_BINARY_DIR}/portable_fixture.mp4"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/voice_custom_wake.bin" "${CMAKE_CURRENT_BINARY_DIR}/playback_chapters.mp4")
        set_tests_properties(media_c_api_tests PROPERTIES FIXTURES_REQUIRED "chapters_fixture;portable_fixture" TIMEOUT 240)
    else()
        add_test(NAME media_c_api_tests COMMAND media_c_api_tests "${CMAKE_CURRENT_BINARY_DIR}/portable_fixture.mp4"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/voice_custom_wake.bin")
        set_tests_properties(media_c_api_tests PROPERTIES FIXTURES_REQUIRED portable_fixture TIMEOUT 240)
    endif()
    # Target tracking (docs/picture.md, "Target tracking"): the tracker on synthetic sequences everywhere, the renderer's grid
    # pass on Windows.
    add_executable(media_target_tracking_tests tests/target_tracking_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_target_tracking_tests PRIVATE media_tracker media_ai)
    add_test(NAME media_target_tracking_tests COMMAND media_target_tracking_tests)
    set_tests_properties(media_target_tracking_tests PROPERTIES TIMEOUT 120)
    # Physical-world perception (docs/picture.md, "Perception"; docs/audio-dsp.md, "Sound localisation and the audio saccade"):
    # flow, looming and time to contact, ego-motion, saliency, the sound locator and the saccade's commands; portable.
    # Rotation (docs/picture.md, "Rotation"): tkhd matrices, video::Orientation, the rotation commands. Portable.
    add_executable(media_rotation_tests tests/rotation_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_rotation_tests PRIVATE media_tracker media_demux media_ai)
    target_include_directories(media_rotation_tests PRIVATE tests)
    add_test(NAME media_rotation_tests COMMAND media_rotation_tests)
    set_tests_properties(media_rotation_tests PROPERTIES TIMEOUT 60)
    # Auto-leveling the horizon (docs/picture.md, "Auto-leveling the horizon"): the estimator on the CPU reference of the
    # histograms; on Windows the statistics pass's histograms through the colour pipeline, and the leveled output.
    add_executable(media_auto_horizon_tests tests/auto_horizon_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_auto_horizon_tests PRIVATE media_tracker)
    target_include_directories(media_auto_horizon_tests PRIVATE tests)
    if(WIN32)
        target_link_libraries(media_auto_horizon_tests PRIVATE media_windows d3d12 dxgi dxguid mfplat mfreadwrite mfuuid ole32)
    endif()
    add_test(NAME media_auto_horizon_tests COMMAND media_auto_horizon_tests "${CMAKE_CURRENT_SOURCE_DIR}/assets/test_fixtures")
    set_tests_properties(media_auto_horizon_tests PROPERTIES TIMEOUT 120)
    add_executable(media_perception_tests tests/perception_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_perception_tests PRIVATE media_tracker media_timing media_ai)
    add_test(NAME media_perception_tests COMMAND media_perception_tests)
    set_tests_properties(media_perception_tests PROPERTIES TIMEOUT 120)
    # Several objects at once (docs/picture.md, "Several objects at once"; SDK API 7): perception's object table and track ids,
    # "next target" through the SDK (the tracker, the view and the audio focus's beam), the GPU image's quarter turns (Windows).
    add_executable(media_multi_target_tests tests/multi_target_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_multi_target_tests PRIVATE media_sdk media_tracker media_ai)
    target_include_directories(media_multi_target_tests PRIVATE tests)
    add_test(NAME media_multi_target_tests COMMAND media_multi_target_tests)
    set_tests_properties(media_multi_target_tests PROPERTIES TIMEOUT 180)
    # Forensic snapshots (suite #200; docs/picture.md, "Forensic snapshots"; SDK API 8): the BMP and its sidecar, the commands,
    # host mode's software view and (Windows) the GPU image and the windowed engine's composed picture on the two-targets fixture.
    add_executable(media_snapshot_tests tests/snapshot_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_snapshot_tests PRIVATE media_sdk media_tracker media_ai)
    target_include_directories(media_snapshot_tests PRIVATE tests)
    add_test(NAME media_snapshot_tests COMMAND media_snapshot_tests "${CMAKE_CURRENT_BINARY_DIR}/playback_two_targets.mp4")
    set_tests_properties(media_snapshot_tests PROPERTIES TIMEOUT 180)
    if(WIN32)
        set_tests_properties(media_snapshot_tests PROPERTIES FIXTURES_REQUIRED two_targets_fixture)
    endif()
    # Remote vibrometry and vitals (suite #201; docs/picture.md, "Remote vibrometry"; SDK API 9): video::Vibrometry on synthetic
    # pulses, flicker and vibration with and without camera shake, and through the SDK with the shake from the IMU.
    add_executable(media_vibrometry_tests tests/vibrometry_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_vibrometry_tests PRIVATE media_sdk media_tracker media_ai)
    target_include_directories(media_vibrometry_tests PRIVATE tests)
    add_test(NAME media_vibrometry_tests COMMAND media_vibrometry_tests)
    set_tests_properties(media_vibrometry_tests PROPERTIES TIMEOUT 180)
    # The neuromorphic event camera (suite #202; docs/picture.md, "Event camera"; SDK API 9): ON / OFF edges, the IMU-stabilised
    # shake, pans and turns, the ring, and media_poll_event_spikes.
    add_executable(media_neuromorphic_spike_tests tests/neuromorphic_spike_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_neuromorphic_spike_tests PRIVATE media_sdk media_tracker media_ai)
    target_include_directories(media_neuromorphic_spike_tests PRIVATE tests)
    add_test(NAME media_neuromorphic_spike_tests COMMAND media_neuromorphic_spike_tests "${CMAKE_CURRENT_BINARY_DIR}/playback_two_targets.mp4")
    set_tests_properties(media_neuromorphic_spike_tests PROPERTIES TIMEOUT 180)
    if(WIN32)
        set_tests_properties(media_neuromorphic_spike_tests PROPERTIES FIXTURES_REQUIRED two_targets_fixture)
    endif()
    # Pre-decode bitstream perception (suite #203; docs/picture.md, "Bitstream perception"; SDK API 9): headers to their QP, a
    # conditional-replenishment stream's cuts and bursts, the SDK's inspector, and (Windows) the platform's H.264 and HEVC encoders.
    add_executable(media_bitstream_perception_tests tests/bitstream_perception_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_bitstream_perception_tests PRIVATE media_sdk media_codec)
    target_include_directories(media_bitstream_perception_tests PRIVATE tests)
    if(WIN32)
        target_link_libraries(media_bitstream_perception_tests PRIVATE mfplat mfuuid ole32)
    endif()
    add_test(NAME media_bitstream_perception_tests COMMAND media_bitstream_perception_tests)
    set_tests_properties(media_bitstream_perception_tests PROPERTIES TIMEOUT 180)
    # Tensor export (suite #204; docs/picture.md, "Tensor export"; SDK API 9): binary16, exact NCHW planes, and
    # media_export_foveated_tensor's regions.
    add_executable(media_tensor_export_tests tests/tensor_export_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_tensor_export_tests PRIVATE media_sdk media_tracker media_ai)
    target_include_directories(media_tensor_export_tests PRIVATE tests)
    add_test(NAME media_tensor_export_tests COMMAND media_tensor_export_tests "${CMAKE_CURRENT_BINARY_DIR}/playback_two_targets.mp4" "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc10_4k_pq.mp4")
    set_tests_properties(media_tensor_export_tests PROPERTIES TIMEOUT 180)
    if(WIN32)
        set_tests_properties(media_tensor_export_tests PROPERTIES FIXTURES_REQUIRED "two_targets_fixture;hevc_pq_fixture")
    endif()
    add_executable(media_spectrum_tests tests/spectrum_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_spectrum_tests PRIVATE media_timing)
    add_test(NAME media_spectrum_tests COMMAND media_spectrum_tests)
    set_tests_properties(media_spectrum_tests PROPERTIES TIMEOUT 60)
    set_tests_properties(media_timing_tests PROPERTIES TIMEOUT 90)
    add_executable(media_demux_tests tests/demux_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_demux_tests PRIVATE media_demux)
    add_test(NAME media_demux_tests COMMAND media_demux_tests)
    set_tests_properties(media_demux_tests PROPERTIES TIMEOUT 90)
    # Multi-hour sample indexes (synthetic multi-GiB files, virtual payload): parse, full passes, seeks.
    add_executable(media_longform_demux_tests tests/longform_demux_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_longform_demux_tests PRIVATE media_demux)
    add_test(NAME media_longform_demux_tests COMMAND media_longform_demux_tests)
    set_tests_properties(media_longform_demux_tests PROPERTIES TIMEOUT 300)
    # Several tracks: catalogue, per-track refusal, emission choice, audio switching, subtitle sample lookup.
    # Chapters and scenes (docs/chapters.md): parsing, navigation, synthesis, cut detection, dock marks, grammar.
    add_executable(media_chapter_scene_tests tests/chapter_scene_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_chapter_scene_tests PRIVATE media_demux media_timeline media_ui media_ai)
    add_test(NAME media_chapter_scene_tests COMMAND media_chapter_scene_tests)
    set_tests_properties(media_chapter_scene_tests PROPERTIES TIMEOUT 60)
    add_executable(media_multitrack_demux_tests tests/multitrack_demux_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_multitrack_demux_tests PRIVATE media_demux)
    add_test(NAME media_multitrack_demux_tests COMMAND media_multitrack_demux_tests)
    set_tests_properties(media_multitrack_demux_tests PROPERTIES TIMEOUT 120)
endif()
if(BUILD_TESTING AND WIN32)
    add_executable(media_hud_window_tests tests/hud_window_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_hud_window_tests PRIVATE media_windows)
    add_test(NAME media_hud_window_tests COMMAND media_hud_window_tests)
    set_tests_properties(media_hud_window_tests PROPERTIES TIMEOUT 30)
    # Command bar: real key, character and input-method messages through the window procedure; and its GPU pass with
    # text in several scripts, checked on the pixels.
    add_executable(media_palette_window_tests tests/palette_window_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_palette_window_tests PRIVATE media_windows imm32)
    add_test(NAME media_palette_window_tests COMMAND media_palette_window_tests)
    set_tests_properties(media_palette_window_tests PROPERTIES TIMEOUT 60)
    add_executable(media_palette_gpu_tests tests/palette_gpu_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_palette_gpu_tests PRIVATE media_windows)
    add_test(NAME media_palette_gpu_tests COMMAND media_palette_gpu_tests)
    set_tests_properties(media_palette_gpu_tests PROPERTIES TIMEOUT 60)
    # The status pill: model (text, layout, fades, labels) and its pass rendered offscreen.
    # The A–B loop (docs/seeking.md): the model, the commands, the dock's bracket drawn offscreen.
    add_executable(media_ab_loop_tests tests/ab_loop_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_ab_loop_tests PRIVATE media_windows media_ai d3d12)
    add_test(NAME media_ab_loop_tests COMMAND media_ab_loop_tests)
    set_tests_properties(media_ab_loop_tests PROPERTIES TIMEOUT 60)
    # Bookmarks: the list, the ledger (version 2, version 1 migrated), the commands; portable.
    add_executable(media_bookmark_compare_tests tests/bookmark_compare_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_bookmark_compare_tests PRIVATE media_session media_ai)
    add_test(NAME media_bookmark_compare_tests COMMAND media_bookmark_compare_tests)
    set_tests_properties(media_bookmark_compare_tests PROPERTIES TIMEOUT 60)
    # The flight recorder and the telemetry readout (docs/session.md); portable.
    add_executable(media_telemetry_explain_tests tests/telemetry_explain_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_telemetry_explain_tests PRIVATE media_ui)
    add_test(NAME media_telemetry_explain_tests COMMAND media_telemetry_explain_tests)
    set_tests_properties(media_telemetry_explain_tests PROPERTIES TIMEOUT 60)
    add_executable(media_hud_status_tests tests/hud_status_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_hud_status_tests PRIVATE media_windows d3d12)
    add_test(NAME media_hud_status_tests COMMAND media_hud_status_tests)
    set_tests_properties(media_hud_status_tests PROPERTIES TIMEOUT 60)
    add_executable(media_hud_gpu_tests tests/hud_gpu_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_hud_gpu_tests PRIVATE media_windows)
    add_test(NAME media_hud_gpu_tests COMMAND media_hud_gpu_tests)
    set_tests_properties(media_hud_gpu_tests PROPERTIES TIMEOUT 30)
    add_executable(media_subtitle_gpu_tests tests/subtitle_gpu_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_subtitle_gpu_tests PRIVATE media_windows)
    add_test(NAME media_subtitle_gpu_tests COMMAND media_subtitle_gpu_tests)
    set_tests_properties(media_subtitle_gpu_tests PROPERTIES TIMEOUT 60)
    add_executable(media_layout_gpu_tests tests/layout_gpu_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_layout_gpu_tests PRIVATE media_windows)
    add_test(NAME media_layout_gpu_tests COMMAND media_layout_gpu_tests)
    set_tests_properties(media_layout_gpu_tests PROPERTIES TIMEOUT 120)
    add_executable(media_make_playback_fixture tests/make_playback_fixture.cpp)
    target_link_libraries(media_make_playback_fixture PRIVATE media_windows)
    add_test(NAME media_make_tracking_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_tracking.mp4" --tracking)
    set_tests_properties(media_make_tracking_fixture PROPERTIES FIXTURES_SETUP tracking_fixture TIMEOUT 30)
    add_test(NAME media_playback_tracking_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_tracking.mp4" --test-tracking)
    set_tests_properties(media_playback_tracking_test PROPERTIES FIXTURES_REQUIRED tracking_fixture TIMEOUT 30)
    # Several objects at once (docs/picture.md, "Several objects at once"): Tab and "cible suivante" move the lock between two
    # approaching squares.
    add_test(NAME media_make_two_targets_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_two_targets.mp4" --two-targets)
    set_tests_properties(media_make_two_targets_fixture PROPERTIES FIXTURES_SETUP two_targets_fixture TIMEOUT 60)
    add_test(NAME media_playback_next_target_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_two_targets.mp4" --test-next-target)
    set_tests_properties(media_playback_next_target_test PROPERTIES FIXTURES_REQUIRED two_targets_fixture TIMEOUT 30)
    # Remote vibrometry in the windowed engine (docs/picture.md, "Remote vibrometry", 2026-11-02): the HUD's pulse readout while
    # tracking (T, a grid every picture) and with micro-motion (M, the warm grids).
    add_test(NAME media_make_pulse_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_pulse.mp4" --pulse)
    set_tests_properties(media_make_pulse_fixture PROPERTIES FIXTURES_SETUP pulse_fixture TIMEOUT 60)
    add_test(NAME media_playback_vibrometry_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_pulse.mp4" --test-vibrometry)
    set_tests_properties(media_playback_vibrometry_test PROPERTIES FIXTURES_REQUIRED pulse_fixture TIMEOUT 40)
    # The window's view stabilised (2026-11-04, docs/picture.md, "Stabilising the window"): a shaken clip, the shake measured on screen.
    add_test(NAME media_make_shaken_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_shaken.mp4" --shaken)
    set_tests_properties(media_make_shaken_fixture PROPERTIES FIXTURES_SETUP shaken_fixture TIMEOUT 60)
    add_test(NAME media_playback_stabilize_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_shaken.mp4" --test-stabilize)
    set_tests_properties(media_playback_stabilize_test PROPERTIES FIXTURES_REQUIRED shaken_fixture TIMEOUT 40)
    # The GPU's full-resolution samples (2026-11-03, "Vibrometry in the window"): a patch whose luma is still and whose colour
    # pulses (Cr), found both ways in Cr.
    add_test(NAME media_make_chroma_pulse_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_chroma_pulse.mp4" --chroma-pulse)
    set_tests_properties(media_make_chroma_pulse_fixture PROPERTIES FIXTURES_SETUP chroma_pulse_fixture TIMEOUT 60)
    add_test(NAME media_playback_vibrometry_chroma_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_chroma_pulse.mp4" --test-vibrometry-chroma)
    set_tests_properties(media_playback_vibrometry_chroma_test PROPERTIES FIXTURES_REQUIRED chroma_pulse_fixture TIMEOUT 40)
    # The audio saccade in the windowed engine (docs/audio-dsp.md, "Sound localisation and the audio saccade").
    add_test(NAME media_make_saccade_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_saccade.mp4" --saccade)
    set_tests_properties(media_make_saccade_fixture PROPERTIES FIXTURES_SETUP saccade_fixture TIMEOUT 30)
    add_test(NAME media_playback_saccade_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_saccade.mp4" --test-saccade)
    set_tests_properties(media_playback_saccade_test PROPERTIES FIXTURES_REQUIRED saccade_fixture TIMEOUT 30)
    # Low-power pacing (docs/session.md, "Low-power pacing"): 24, 30 and 60 fps, playing, paused for 4 s, playing again; the
    # presentation thread's CPU share while playing and while paused, and no picture dropped.
    foreach(fps IN ITEMS 24 30 60)
        add_test(NAME media_make_pacing${fps}_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_pacing${fps}.mp4" --pacing${fps})
        set_tests_properties(media_make_pacing${fps}_fixture PROPERTIES FIXTURES_SETUP pacing${fps}_fixture TIMEOUT 30)
        add_test(NAME media_playback_pacing${fps}_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_pacing${fps}.mp4" --test-pacing)
        set_tests_properties(media_playback_pacing${fps}_test PROPERTIES FIXTURES_REQUIRED pacing${fps}_fixture TIMEOUT 45)
    endforeach()
    # Rotation in playback (docs/picture.md, "Rotation"): each tkhd orientation opens upright; R while playing and paused,
    # leveling and a spoken turn, every picture against video::Orientation's prediction.
    # The window thread (docs/session.md, "The window thread"): the window's procedure blocked 120 ms three times at 60 fps,
    # no picture dropped, input handled within 4 ms of being queued.
    add_test(NAME media_playback_window_stall_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_pacing60.mp4" --test-window-stall)
    # The audio lead (docs/session.md, "The audio lead"): a 10 ms and a 15 ms gap in the stream, bridged without stopping the clock.
    add_test(NAME media_playback_hiccup_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_pacing60.mp4" --test-hiccup)
    set_tests_properties(media_playback_hiccup_test PROPERTIES FIXTURES_REQUIRED pacing60_fixture TIMEOUT 45)
    set_tests_properties(media_playback_window_stall_test PROPERTIES FIXTURES_REQUIRED pacing60_fixture TIMEOUT 45)
    foreach(turn rotate90 rotate180 rotate270 mirror)
        add_test(NAME media_make_${turn}_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_${turn}.mp4" --${turn})
        set_tests_properties(media_make_${turn}_fixture PROPERTIES FIXTURES_SETUP ${turn}_fixture TIMEOUT 30)
        add_test(NAME media_playback_${turn}_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_${turn}.mp4" --test-rotation)
        set_tests_properties(media_playback_${turn}_test PROPERTIES FIXTURES_REQUIRED ${turn}_fixture TIMEOUT 60)
    endforeach()
    # Auto-leveling in playback (docs/picture.md, "Auto-leveling the horizon"): Shift+R on a picture tilted +4.5 degrees, then
    # -6.0; the leveling turn found from the GPU's histograms, the shown picture's own lines level.
    add_test(NAME media_make_horizon_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_horizon.mp4" --horizon)
    set_tests_properties(media_make_horizon_fixture PROPERTIES FIXTURES_SETUP horizon_fixture TIMEOUT 30)
    add_test(NAME media_playback_auto_horizon_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_horizon.mp4" --test-auto-horizon)
    set_tests_properties(media_playback_auto_horizon_test PROPERTIES FIXTURES_REQUIRED horizon_fixture TIMEOUT 45)
    # Black bars (docs/picture.md, "Black bars"): the same pictures letterboxed; auto-leveling through the bars, then "recadrer
    # les bandes noires".
    add_test(NAME media_make_horizon_letterbox_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_horizon_letterbox.mp4" --horizon-letterbox)
    set_tests_properties(media_make_horizon_letterbox_fixture PROPERTIES FIXTURES_SETUP horizon_letterbox_fixture TIMEOUT 30)
    add_test(NAME media_playback_crop_bars_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_horizon_letterbox.mp4" --test-crop-bars)
    set_tests_properties(media_playback_crop_bars_test PROPERTIES FIXTURES_REQUIRED horizon_letterbox_fixture TIMEOUT 45)
    add_test(NAME media_make_playback_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_fixture.mp4")
    set_tests_properties(media_make_playback_fixture PROPERTIES FIXTURES_SETUP playback_fixture TIMEOUT 30)
    add_test(NAME media_playback_demo_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_fixture.mp4" --test)
    set_tests_properties(media_playback_demo_test PROPERTIES FIXTURES_REQUIRED playback_fixture TIMEOUT 30)
    add_test(NAME media_playback_dsp_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_fixture.mp4" --test-dsp)
    set_tests_properties(media_playback_dsp_test PROPERTIES FIXTURES_REQUIRED playback_fixture TIMEOUT 30)
    add_test(NAME media_playback_volume_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_fixture.mp4" --test-volume)
    set_tests_properties(media_playback_volume_test PROPERTIES FIXTURES_REQUIRED playback_fixture TIMEOUT 30)
    # Spoken-style commands (Hebrew and English) through the intent dispatcher during playback: volume and mute with
    # the dock's volume pill, a refused seek, quote searches landing on their cues, a relative seek while paused.
    add_test(NAME media_playback_intent_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_fixture.mp4" --test-intents)
    set_tests_properties(media_playback_intent_test PROPERTIES FIXTURES_REQUIRED playback_fixture TIMEOUT 30)
    # Follow-ups (docs/intents.md): next/previous/nth match, undo, again, the line again, the split comparison by command.
    add_test(NAME media_playback_followup_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_fixture.mp4" --test-followups)
    # Paused inspection (docs/picture.md): V, N, C while paused change the picture on screen at once, as their settings say.
    add_test(NAME media_playback_paused_quality_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_fixture.mp4" --test-paused-quality)
    set_tests_properties(media_playback_paused_quality_test PROPERTIES FIXTURES_REQUIRED playback_fixture TIMEOUT 40)
    set_tests_properties(media_playback_followup_test PROPERTIES FIXTURES_REQUIRED playback_fixture TIMEOUT 30)
    # The same, typed into the window's command bar as key and character messages, in seven languages: commands,
    # a quote searched because it is no command, a refused text dismissed with Escape.
    add_test(NAME media_playback_palette_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_fixture.mp4" --test-palette)
    set_tests_properties(media_playback_palette_test PROPERTIES FIXTURES_REQUIRED playback_fixture TIMEOUT 30)
    add_test(NAME media_playback_subtitle_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_fixture.mp4" --test-subtitles)
    set_tests_properties(media_playback_subtitle_test PROPERTIES FIXTURES_REQUIRED playback_fixture TIMEOUT 30)
    add_test(NAME media_playback_subtitle_scrub_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_fixture.mp4" --test-subtitle-scrub)
    # Hardware fault recovery: a real D3D12 device removal while playing, and an audio endpoint invalidated in-process
    # (the sink's injection hook; the machine's audio device and other applications' streams are not touched).
    add_test(NAME media_playback_device_loss_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_fixture.mp4" --test-device-loss)
    set_tests_properties(media_playback_device_loss_test PROPERTIES FIXTURES_REQUIRED playback_fixture TIMEOUT 30)
    add_test(NAME media_playback_audio_loss_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_fixture.mp4" --test-audio-loss)
    set_tests_properties(media_playback_audio_loss_test PROPERTIES FIXTURES_REQUIRED playback_fixture TIMEOUT 30)
    # The same recovery against a real exclusive-mode steal of the default speaker. It takes the device from every
    # application on the machine (their playback fails with 0xC00D4E85), so it is registered only on request.
    option(MEDIA_TEST_EXCLUSIVE_STEAL "Register the test that takes the default audio endpoint in exclusive mode" OFF)
    if(MEDIA_TEST_EXCLUSIVE_STEAL)
        add_test(NAME media_playback_audio_exclusive_steal_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_fixture.mp4" --test-exclusive-steal)
        set_tests_properties(media_playback_audio_exclusive_steal_test PROPERTIES FIXTURES_REQUIRED playback_fixture TIMEOUT 30 RUN_SERIAL TRUE)
    endif()
    set_tests_properties(media_playback_subtitle_scrub_test PROPERTIES FIXTURES_REQUIRED playback_fixture TIMEOUT 30)
    # H.264 with B-frames from the platform encoders: a hardware B-pyramid (reference B-pictures, MMCO, list
    # modification) and a software CAVLC IBBP stream, with ctts and an edit list; skipped when no encoder exists.
    add_test(NAME media_make_h264_bframes_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_h264_bframes.mp4" --h264-bframes)
    add_test(NAME media_make_h264_bframes_sw_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_h264_bframes_sw.mp4" --h264-bframes-sw)
    set_tests_properties(media_make_h264_bframes_fixture PROPERTIES FIXTURES_SETUP h264_bframes_fixture TIMEOUT 120 SKIP_RETURN_CODE 77)
    set_tests_properties(media_make_h264_bframes_sw_fixture PROPERTIES FIXTURES_SETUP h264_bframes_sw_fixture TIMEOUT 120 SKIP_RETURN_CODE 77)
    foreach(mode IN ITEMS test test-seek test-scrub)
        string(REPLACE "-" "_" name "${mode}")
        add_test(NAME media_playback_h264_bframes_${name} COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_h264_bframes.mp4" --${mode})
        set_tests_properties(media_playback_h264_bframes_${name} PROPERTIES FIXTURES_REQUIRED h264_bframes_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    endforeach()
    add_test(NAME media_playback_h264_bframes_http_seek COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_h264_bframes.mp4" --test-seek --http)
    set_tests_properties(media_playback_h264_bframes_http_seek PROPERTIES FIXTURES_REQUIRED h264_bframes_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    foreach(mode IN ITEMS test test-seek)
        string(REPLACE "-" "_" name "${mode}")
        add_test(NAME media_playback_h264_bframes_sw_${name} COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_h264_bframes_sw.mp4" --${mode})
        set_tests_properties(media_playback_h264_bframes_sw_${name} PROPERTIES FIXTURES_REQUIRED h264_bframes_sw_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    endforeach()
    # A 3 h 30 min file (302,496 pictures, 590,812 AAC units) whose chunks reuse one GOP cycle: seeks across hours.
    add_test(NAME media_make_longform_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_longform.mp4" --longform)
    set_tests_properties(media_make_longform_fixture PROPERTIES FIXTURES_SETUP longform_fixture TIMEOUT 60)
    add_test(NAME media_playback_longform_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_longform.mp4" --test-longform)
    set_tests_properties(media_playback_longform_test PROPERTIES FIXTURES_REQUIRED longform_fixture TIMEOUT 90)
    add_test(NAME media_playback_longform_http_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_longform.mp4" --test-longform --http)
    set_tests_properties(media_playback_longform_http_test PROPERTIES FIXTURES_REQUIRED longform_fixture TIMEOUT 90)
    # Sessions (docs/session.md): a folder of fixtures played through Ctrl+arrows and spoken next/previous commands, resumed
    # from a fresh ledger, tracks restored, an unplayable file passed over, one window handed from file to file.
    add_test(NAME media_playback_resume_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_longform.mp4" --test-resume)
    set_tests_properties(media_playback_resume_test PROPERTIES FIXTURES_REQUIRED "longform_fixture;multitrack_fixture;playback_fixture" TIMEOUT 120 SKIP_RETURN_CODE 77)
    # GPU deadline governor (docs/picture.md): Auto steps up to Neural on headroom, down to Fast under real extra GPU load
    # at 90 % of the frame interval and stays there, then N presses fix Neural and return to Auto. 64x64 shown at 640x640.
    add_test(NAME media_playback_quality_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_longform.mp4" --test-quality)
    set_tests_properties(media_playback_quality_test PROPERTIES FIXTURES_REQUIRED longform_fixture TIMEOUT 60)
    # Voice commands said into the engine (scripted PCM of voices that are in no template): pause, Hebrew play, mute,
    # and a word that is no command, executed through Command() while the 3.5-hour fixture plays.
    add_test(NAME media_playback_voice_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_longform.mp4" --test-voice
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/data/voice_corpus.bin")
    set_tests_properties(media_playback_voice_test PROPERTIES FIXTURES_REQUIRED longform_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    # Surround fixtures: native 5.1 AAC-LC, and 7.1 AAC-LC spliced from native mono/stereo encodes (PCE layout).
    add_test(NAME media_make_surround51_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_51.mp4" --surround51)
    add_test(NAME media_make_surround71_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_71.mp4" --surround71)
    set_tests_properties(media_make_surround51_fixture PROPERTIES FIXTURES_SETUP surround51_fixture TIMEOUT 60)
    set_tests_properties(media_make_surround71_fixture PROPERTIES FIXTURES_SETUP surround71_fixture TIMEOUT 60)
    foreach(output IN ITEMS auto passthrough stereo)
        add_test(NAME media_playback_surround_${output}_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_51.mp4" --test-surround --surround ${output})
        set_tests_properties(media_playback_surround_${output}_test PROPERTIES FIXTURES_REQUIRED surround51_fixture TIMEOUT 60)
    endforeach()
    add_test(NAME media_playback_surround71_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_71.mp4" --test-surround71)
    set_tests_properties(media_playback_surround71_test PROPERTIES FIXTURES_REQUIRED surround71_fixture TIMEOUT 60)
    # Anamorphic H.264: a 60x44 crop with sample aspect 4:3 in the VUI, BT.601 in the stream and BT.709 in the container.
    # Several audio and subtitle tracks (docs/tracks.md): catalogue, conversational track commands, PCM splices, cues.
    # Chapters and scenes (docs/chapters.md): Nero chpl and a QuickTime chapter track, with hard cuts at the chapters.
    add_test(NAME media_make_chapters_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_chapters.mp4" --chapters)
    add_test(NAME media_make_chapters_qt_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_chapters_qt.mp4" --chapters-qt)
    set_tests_properties(media_make_chapters_fixture media_make_chapters_qt_fixture PROPERTIES FIXTURES_SETUP chapters_fixture TIMEOUT 60)
    add_test(NAME media_playback_chapters_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_chapters.mp4" --test-chapters)
    add_test(NAME media_playback_chapters_qt_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_chapters_qt.mp4" --test-chapters)
    set_tests_properties(media_playback_chapters_test media_playback_chapters_qt_test PROPERTIES FIXTURES_REQUIRED chapters_fixture TIMEOUT 60)
    add_test(NAME media_make_multitrack_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_multitrack.mp4" --multitrack)
    set_tests_properties(media_make_multitrack_fixture PROPERTIES FIXTURES_SETUP multitrack_fixture TIMEOUT 60)
    # Playback speed (docs/audio-dsp.md): 1x, ] (1.25x), "speed 2x", "half speed", "מהירות רגילה": the clock at each speed, the tone's
    # pitch kept, the audio marks where the clock puts them; then frame steps, three forward and two back.
    add_test(NAME media_playback_speed_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_multitrack.mp4" --test-speed)
    set_tests_properties(media_playback_speed_test PROPERTIES FIXTURES_REQUIRED multitrack_fixture TIMEOUT 60)
    add_test(NAME media_playback_zoom_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc.mp4" --test-zoom)
    set_tests_properties(media_playback_zoom_test PROPERTIES FIXTURES_REQUIRED hevc_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    # Music mode (docs/audio-dsp.md): an M4A with AAC alone, three chapters of three tones.
    add_test(NAME media_make_music_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_music.m4a" --audio-only)
    set_tests_properties(media_make_music_fixture PROPERTIES FIXTURES_SETUP music_fixture TIMEOUT 30)
    add_test(NAME media_playback_music_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_music.m4a" --test-music)
    set_tests_properties(media_playback_music_test PROPERTIES FIXTURES_REQUIRED music_fixture TIMEOUT 60)
    add_test(NAME media_playback_music_session_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_music.m4a" --test-music-session)
    set_tests_properties(media_playback_music_session_test PROPERTIES FIXTURES_REQUIRED "music_fixture;playback_fixture" TIMEOUT 60 SKIP_RETURN_CODE 77)
    # Live zoom while playing (docs/picture.md): the queued pictures shown through each change, then native.
    add_test(NAME media_playback_zoom_live_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_longform.mp4" --test-zoom-live)
    set_tests_properties(media_playback_zoom_live_test PROPERTIES FIXTURES_REQUIRED longform_fixture TIMEOUT 60)
    # The A–B loop (docs/seeking.md): L, L, three turns, L; the sentence on screen, three turns; off.
    add_test(NAME media_playback_ab_loop_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_fixture.mp4" --test-ab-loop)
    set_tests_properties(media_playback_ab_loop_test PROPERTIES FIXTURES_REQUIRED playback_fixture TIMEOUT 60)
    # The telemetry readout (docs/session.md): the panel, an overload, the explanations in English and Hebrew.
    add_test(NAME media_playback_telemetry_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_longform.mp4" --test-telemetry)
    set_tests_properties(media_playback_telemetry_test PROPERTIES FIXTURES_REQUIRED longform_fixture TIMEOUT 60)
    # The pinned loop turnaround on long-GOP 4K (docs/seeking.md): A 24 pictures after the only sync sample.
    add_test(NAME media_playback_ab_loop_longgop_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc10_4k_longgop.mp4" --test-ab-loop)
    set_tests_properties(media_playback_ab_loop_longgop_test PROPERTIES FIXTURES_REQUIRED hevc10_4k_longgop_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    # Bookmarks, the pinned frame and Pinned | Live on screen; kept in the ledger (docs/session.md).
    add_test(NAME media_playback_bookmark_compare_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_longform.mp4" --test-bookmarks)
    set_tests_properties(media_playback_bookmark_compare_test PROPERTIES FIXTURES_REQUIRED longform_fixture TIMEOUT 60)
    add_test(NAME media_playback_tracks_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_multitrack.mp4" --test-tracks)
    set_tests_properties(media_playback_tracks_test PROPERTIES FIXTURES_REQUIRED multitrack_fixture TIMEOUT 60)
    add_test(NAME media_playback_tracks_surround_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_multitrack.mp4" --test-tracks-surround)
    set_tests_properties(media_playback_tracks_surround_test PROPERTIES FIXTURES_REQUIRED multitrack_fixture TIMEOUT 60)
    add_test(NAME media_make_anamorphic_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_anamorphic.mp4" --anamorphic)
    set_tests_properties(media_make_anamorphic_fixture PROPERTIES FIXTURES_SETUP anamorphic_fixture TIMEOUT 30)
    add_test(NAME media_playback_anamorphic_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_anamorphic.mp4" --test-anamorphic)
    set_tests_properties(media_playback_anamorphic_test PROPERTIES FIXTURES_REQUIRED anamorphic_fixture TIMEOUT 30)
    add_test(NAME media_playback_neural_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_fixture.mp4" --test-neural)
    set_tests_properties(media_playback_neural_test PROPERTIES FIXTURES_REQUIRED playback_fixture TIMEOUT 60)
    add_test(NAME media_playback_hevc_neural_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc.mp4" --test-neural)
    set_tests_properties(media_playback_hevc_neural_test PROPERTIES FIXTURES_REQUIRED hevc_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    foreach(mode IN ITEMS subtitle-overflow subtitle-missing resize)
        string(REPLACE "-" "_" name "${mode}")
        add_test(NAME media_playback_${name}_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_fixture.mp4" --test-${mode})
        set_tests_properties(media_playback_${name}_test PROPERTIES FIXTURES_REQUIRED playback_fixture TIMEOUT 60)
    endforeach()
    add_test(NAME media_playback_hud_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_fixture.mp4" --test-hud)
    set_tests_properties(media_playback_hud_test PROPERTIES FIXTURES_REQUIRED playback_fixture TIMEOUT 30)
    # HEVC fixtures come from the platform's HEVC encoder MFT (B-pictures, ctts, single-entry elst); skip when absent.
    add_test(NAME media_make_hevc_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc.mp4" --hevc)
    add_test(NAME media_make_hevc10_4k_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc10_4k.mp4" --hevc10-4k)
    set_tests_properties(media_make_hevc_fixture PROPERTIES FIXTURES_SETUP hevc_fixture TIMEOUT 120 SKIP_RETURN_CODE 77)
    set_tests_properties(media_make_hevc10_4k_fixture PROPERTIES FIXTURES_SETUP hevc10_4k_fixture TIMEOUT 180 SKIP_RETURN_CODE 77)
    add_test(NAME media_playback_hevc_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc.mp4" --test)
    add_test(NAME media_playback_hevc10_4k_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc10_4k.mp4" --test)
    set_tests_properties(media_playback_hevc_test PROPERTIES FIXTURES_REQUIRED hevc_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    set_tests_properties(media_playback_hevc10_4k_test PROPERTIES FIXTURES_REQUIRED hevc10_4k_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    # Device removal while paused (the held picture must come back exactly), and while playing 4K Main10.
    add_test(NAME media_playback_hevc_device_loss_paused_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc.mp4" --test-device-loss-paused)
    add_test(NAME media_playback_hevc10_4k_device_loss_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc10_4k.mp4" --test-device-loss)
    set_tests_properties(media_playback_hevc_device_loss_paused_test PROPERTIES FIXTURES_REQUIRED hevc_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    set_tests_properties(media_playback_hevc10_4k_device_loss_test PROPERTIES FIXTURES_REQUIRED hevc10_4k_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    # Edit list starting one picture later: that picture decodes as a reference but is never presented.
    add_test(NAME media_make_hevc_trim_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc_trim.mp4" --hevc-trim)
    set_tests_properties(media_make_hevc_trim_fixture PROPERTIES FIXTURES_SETUP hevc_trim_fixture TIMEOUT 120 SKIP_RETURN_CODE 77)
    add_test(NAME media_playback_hevc_trim_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc_trim.mp4" --test-trim)
    set_tests_properties(media_playback_hevc_trim_test PROPERTIES FIXTURES_REQUIRED hevc_trim_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    # HDR: platform-encoded Main10 with a BT.2020 PQ / HLG colour description spliced into the SPS VUI.
    add_test(NAME media_make_hevc10_4k_pq_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc10_4k_pq.mp4" --hevc10-4k-pq)
    add_test(NAME media_make_hevc10_hlg_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc10_hlg.mp4" --hevc10-hlg)
    set_tests_properties(media_make_hevc10_4k_pq_fixture PROPERTIES FIXTURES_SETUP hevc_pq_fixture TIMEOUT 180 SKIP_RETURN_CODE 77)
    set_tests_properties(media_make_hevc10_hlg_fixture PROPERTIES FIXTURES_SETUP hevc_hlg_fixture TIMEOUT 120 SKIP_RETURN_CODE 77)
    add_test(NAME media_playback_hevc10_4k_pq_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc10_4k_pq.mp4" --test-hdr)
    add_test(NAME media_playback_hevc10_hlg_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc10_hlg.mp4" --test-hdr)
    set_tests_properties(media_playback_hevc10_4k_pq_test PROPERTIES FIXTURES_REQUIRED hevc_pq_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    set_tests_properties(media_playback_hevc10_hlg_test PROPERTIES FIXTURES_REQUIRED hevc_hlg_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    # Resilience: a forced endpoint underrun and a scripted pause/resume must not end or stall the session.
    add_test(NAME media_playback_underrun_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc.mp4" --test-underrun)
    add_test(NAME media_playback_pause_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc.mp4" --test-pause)
    set_tests_properties(media_playback_underrun_test media_playback_pause_test PROPERTIES FIXTURES_REQUIRED hevc_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    # Seek: forward while playing and backward while paused, each landing on the exact target frame.
    add_test(NAME media_playback_seek_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_fixture.mp4" --test-seek)
    set_tests_properties(media_playback_seek_test PROPERTIES FIXTURES_REQUIRED playback_fixture TIMEOUT 30)
    add_test(NAME media_playback_hevc_seek_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc.mp4" --test-seek)
    set_tests_properties(media_playback_hevc_seek_test PROPERTIES FIXTURES_REQUIRED hevc_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    add_test(NAME media_playback_hevc10_4k_seek_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc10_4k.mp4" --test-seek)
    set_tests_properties(media_playback_hevc10_4k_seek_test PROPERTIES FIXTURES_REQUIRED hevc10_4k_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    # Network: the same fixtures served by the in-process loopback HTTP range server (--http-slow: 15 ms per
    # response, 50 Mbit/s) and streamed through HttpBlockSource, with seeking and scrubbing.
    add_test(NAME media_playback_http_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_fixture.mp4" --test --http)
    add_test(NAME media_playback_http_seek_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_fixture.mp4" --test-seek --http)
    set_tests_properties(media_playback_http_test media_playback_http_seek_test PROPERTIES FIXTURES_REQUIRED playback_fixture TIMEOUT 30)
    add_test(NAME media_playback_hevc_http_seek_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc.mp4" --test-seek --http)
    add_test(NAME media_playback_hevc_http_scrub_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc.mp4" --test-scrub --http-slow)
    set_tests_properties(media_playback_hevc_http_seek_test media_playback_hevc_http_scrub_test PROPERTIES FIXTURES_REQUIRED hevc_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    add_test(NAME media_playback_hevc10_4k_http_seek_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc10_4k.mp4" --test-seek --http-slow)
    set_tests_properties(media_playback_hevc10_4k_http_seek_test PROPERTIES FIXTURES_REQUIRED hevc10_4k_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    # Scrub: a 120 Hz drag coalesced to one seek in flight, then a burst of four seeks drained and applied as one.
    add_test(NAME media_playback_scrub_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_fixture.mp4" --test-scrub)
    set_tests_properties(media_playback_scrub_test PROPERTIES FIXTURES_REQUIRED playback_fixture TIMEOUT 30)
    add_test(NAME media_playback_hevc_scrub_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc.mp4" --test-scrub)
    set_tests_properties(media_playback_hevc_scrub_test PROPERTIES FIXTURES_REQUIRED hevc_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    add_test(NAME media_playback_hevc10_4k_scrub_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc10_4k.mp4" --test-scrub --expect-previews)
    set_tests_properties(media_playback_hevc10_4k_scrub_test PROPERTIES FIXTURES_REQUIRED hevc10_4k_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    # Scrub preview (docs/seeking.md): the same drag with previews off lands every scrub seek exactly; on a single-GOP clip
    # the backward half previews the one sync sample.
    add_test(NAME media_playback_hevc10_4k_scrub_exact_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc10_4k.mp4" --test-scrub --no-scrub-preview)
    set_tests_properties(media_playback_hevc10_4k_scrub_exact_test PROPERTIES FIXTURES_REQUIRED hevc10_4k_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    add_test(NAME media_playback_hevc10_4k_longgop_scrub_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc10_4k_longgop.mp4" --test-scrub --expect-previews)
    set_tests_properties(media_playback_hevc10_4k_longgop_scrub_test PROPERTIES FIXTURES_REQUIRED hevc10_4k_longgop_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    # Test windows take no desktop input: stray clicks, drags, keys and the wheel posted to the window from another thread
    # change nothing, while the harness's tagged posts and SendMessage keys still work.
    add_test(NAME media_playback_isolation_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_fixture.mp4" --test-isolation)
    set_tests_properties(media_playback_isolation_test PROPERTIES FIXTURES_REQUIRED playback_fixture TIMEOUT 30)
    # Forward continuation: one group of pictures for the whole 3 s clip (a single IDR, as long-GOP files have). The same
    # 0.25 s -> 2.5 s seek is made carrying on from the decode position, then restarting from the sync sample.
    add_test(NAME media_make_hevc_longgop_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc_longgop.mp4" --hevc-longgop)
    add_test(NAME media_make_hevc10_4k_longgop_fixture COMMAND media_make_playback_fixture "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc10_4k_longgop.mp4" --hevc10-4k-longgop)
    set_tests_properties(media_make_hevc_longgop_fixture PROPERTIES FIXTURES_SETUP hevc_longgop_fixture TIMEOUT 120 SKIP_RETURN_CODE 77)
    set_tests_properties(media_make_hevc10_4k_longgop_fixture PROPERTIES FIXTURES_SETUP hevc10_4k_longgop_fixture TIMEOUT 180 SKIP_RETURN_CODE 77)
    add_test(NAME media_playback_hevc_seek_forward_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc_longgop.mp4" --test-seek-forward)
    add_test(NAME media_playback_hevc10_4k_seek_forward_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc10_4k_longgop.mp4" --test-seek-forward)
    set_tests_properties(media_playback_hevc_seek_forward_test PROPERTIES FIXTURES_REQUIRED hevc_longgop_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    set_tests_properties(media_playback_hevc10_4k_seek_forward_test PROPERTIES FIXTURES_REQUIRED hevc10_4k_longgop_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    # Queue seeks: forward targets among pictures already decoded land on them without decoding anything again;
    # the same seek with queue seeks off restarts from the sync sample, for comparison.
    add_test(NAME media_playback_hevc_seek_micro_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc.mp4" --test-seek-micro)
    add_test(NAME media_playback_hevc10_4k_seek_micro_test COMMAND media_player_demo "${CMAKE_CURRENT_BINARY_DIR}/playback_hevc10_4k_longgop.mp4" --test-seek-micro)
    set_tests_properties(media_playback_hevc_seek_micro_test PROPERTIES FIXTURES_REQUIRED hevc_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    set_tests_properties(media_playback_hevc10_4k_seek_micro_test PROPERTIES FIXTURES_REQUIRED hevc10_4k_longgop_fixture TIMEOUT 60 SKIP_RETURN_CODE 77)
    add_executable(media_windows_media_tests tests/windows_media_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_windows_media_tests PRIVATE media_windows)
    add_test(NAME media_wasapi_tests COMMAND media_windows_media_tests audio)
    add_test(NAME media_d3d12_decode_tests COMMAND media_windows_media_tests video)
    add_test(NAME media_hevc_prepare_tests COMMAND media_windows_media_tests hevc-prepare)
    set_tests_properties(media_wasapi_tests media_d3d12_decode_tests media_hevc_prepare_tests PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/HevcCompareShader.hpp"
        COMMAND "${MEDIA_FXC}" /nologo /WX /T cs_5_0 /E CS /Vn g_HevcCompareShader
        /Fh "${CMAKE_CURRENT_BINARY_DIR}/generated/HevcCompareShader.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/tests/HevcPatternCompare.hlsl"
        DEPENDS tests/HevcPatternCompare.hlsl VERBATIM)
    add_executable(media_hevc_decode_tests tests/hevc_decode_tests.cpp tests/allocation_probe.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/generated/HevcCompareShader.hpp")
    target_include_directories(media_hevc_decode_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(media_hevc_decode_tests PRIVATE media_windows d3d12 dxgi dxguid mfplat mfuuid ole32)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/ColorCompareShader.hpp"
        COMMAND "${MEDIA_FXC}" /nologo /WX /T cs_5_0 /E CS /Vn g_ColorCompareShader
        /Fh "${CMAKE_CURRENT_BINARY_DIR}/generated/ColorCompareShader.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ColorCompare.hlsl"
        DEPENDS tests/ColorCompare.hlsl VERBATIM)
    add_executable(media_tonemap_tests tests/tonemap_tests.cpp "${CMAKE_CURRENT_BINARY_DIR}/generated/ColorCompareShader.hpp")
    target_include_directories(media_tonemap_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(media_tonemap_tests PRIVATE media_windows d3d12 dxgi dxguid)
    add_test(NAME media_tonemap_tests COMMAND media_tonemap_tests)
    # The dynamic HDR peak per epoch: a seek's landing tone-mapped exactly as the same picture decoded cold, scene cuts,
    # presentation-time smoothing (docs/picture.md, "HDR peak per epoch").
    add_test(NAME media_hdr_tonemap_epoch_tests COMMAND media_tonemap_tests epoch)
    set_tests_properties(media_hdr_tonemap_epoch_tests PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/EdgeMetricsShader.hpp"
        COMMAND "${MEDIA_FXC}" /nologo /WX /T cs_5_0 /E CS /Vn g_EdgeMetricsShader
        /Fh "${CMAKE_CURRENT_BINARY_DIR}/generated/EdgeMetricsShader.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/tests/EdgeMetrics.hlsl"
        DEPENDS tests/EdgeMetrics.hlsl VERBATIM)
    add_executable(media_upscale_tests tests/upscale_tests.cpp tests/allocation_probe.cpp "${CMAKE_CURRENT_BINARY_DIR}/generated/EdgeMetricsShader.hpp")
    target_include_directories(media_upscale_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(media_upscale_tests PRIVATE media_windows d3d12 dxgi dxguid)
    add_test(NAME media_upscale_tests COMMAND media_upscale_tests)
    # Crop exactness (nothing outside the crop reaches the output, in any conversion mode or in the scaled blit) and
    # display shape (sample aspect ratio, viewport) on synthetic surfaces with odd crops and poisoned padding.
    add_executable(media_aspect_ratio_crop_tests tests/aspect_ratio_crop_tests.cpp)
    target_include_directories(media_aspect_ratio_crop_tests PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(media_aspect_ratio_crop_tests PRIVATE media_windows d3d12 dxgi dxguid)
    add_test(NAME media_aspect_ratio_crop_tests COMMAND media_aspect_ratio_crop_tests)
    set_tests_properties(media_aspect_ratio_crop_tests PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    set_tests_properties(media_upscale_tests PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    add_executable(media_http_source_tests tests/http_source_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_http_source_tests PRIVATE media_windows ws2_32)
    add_test(NAME media_http_source_tests COMMAND media_http_source_tests)
    set_tests_properties(media_http_source_tests PROPERTIES TIMEOUT 60)
    add_executable(media_aac_preroll_tests tests/aac_preroll_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_aac_preroll_tests PRIVATE media_windows mfplat mfuuid wmcodecdspuuid ole32)
    add_test(NAME media_aac_preroll_tests COMMAND media_aac_preroll_tests)
    set_tests_properties(media_aac_preroll_tests PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
    # Offline trainer for the neural upscaler's weights (writes include/media/render/NeuralWeights.hpp); not a test.
    add_executable(media_train_neural tools/train_neural.cpp)
    target_link_libraries(media_train_neural PRIVATE media_neural windowscodecs ole32 mfplat mfuuid wmcodecdspuuid strmiids)
    # Downscale prefilter and dither (docs/picture.md): aliasing at 2x/2.5x/3x, crop safety under poison padding, banding.
    add_executable(media_downscale_dither_tests tests/downscale_dither_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_downscale_dither_tests PRIVATE media_windows d3d12 dxgi dxguid)
    add_test(NAME media_downscale_dither_tests COMMAND media_downscale_dither_tests)
    set_tests_properties(media_downscale_dither_tests PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    add_executable(media_neural_upscale_tests tests/neural_upscale_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_neural_upscale_tests PRIVATE media_windows)
    add_test(NAME media_neural_upscale_tests COMMAND media_neural_upscale_tests)
    set_tests_properties(media_neural_upscale_tests PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    add_executable(media_aac_surround_tests tests/aac_surround_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_aac_surround_tests PRIVATE media_windows mfplat mfuuid wmcodecdspuuid ole32)
    add_test(NAME media_aac_surround_tests COMMAND media_aac_surround_tests)
    set_tests_properties(media_aac_surround_tests PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    set_tests_properties(media_tonemap_tests PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    add_test(NAME media_hevc_dxva_tests COMMAND media_hevc_decode_tests dxva)
    add_test(NAME media_decode_reference_tests COMMAND media_hevc_decode_tests reference)
    add_test(NAME media_hevc_decode_tests COMMAND media_hevc_decode_tests decode)
    set_tests_properties(media_hevc_dxva_tests media_decode_reference_tests media_hevc_decode_tests PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77)
    # H.264 B-frames: pixel-exact reference lists (MMCO, long-term, list modification, POC types) and encoder streams.
    add_test(NAME media_h264_lists_tests COMMAND media_hevc_decode_tests h264-lists)
    add_test(NAME media_h264_decode_tests COMMAND media_hevc_decode_tests h264-decode)
    set_tests_properties(media_h264_lists_tests media_h264_decode_tests PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
    add_executable(media_windows_io_tests tests/windows_io_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_windows_io_tests PRIVATE media_windows media_demux)
    add_test(NAME media_windows_io_tests COMMAND media_windows_io_tests)
    set_tests_properties(media_windows_io_tests PROPERTIES TIMEOUT 90)
    # Real-world files (assets/test_fixtures). Third-party footage (the LG, Costa Rica and Dolby demo reels, Big Buck Bunny 4K, the
    # 45-minute clock) is not in the public repository: its tests are registered only when the file is there (2026-11-10; a
    # clean clone runs no test it cannot run). The synthetic ones (tools/realworld/make_*_fixtures.py) are tracked. The audit
    # reads each container, describes its streams, demuxes 60 pictures and decodes two seconds of audio; the playable
    # file is then played through the engine, alone and with the multilingual subtitle track and quote searches; the
    # AV1 files must be refused at the codec stage (4) until an AV1 frontend exists.
    set(MEDIA_FIXTURES "${CMAKE_CURRENT_SOURCE_DIR}/assets/test_fixtures")
    add_executable(media_fixture_audit tests/fixture_audit.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_fixture_audit PRIVATE media_windows media_demux d3d12 dxguid mfplat mfuuid wmcodecdspuuid ole32)
    if(EXISTS "${MEDIA_FIXTURES}/costa_rica_4k60_hdr.mp4")
        add_test(NAME media_fixture_audit_test COMMAND media_fixture_audit "${MEDIA_FIXTURES}")
        set_tests_properties(media_fixture_audit_test PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77)
    endif()
    # The same audit on the everyday-MP4 variants the repository generates (make_compat_matrix.py).
    add_test(NAME media_fixture_audit_compat_test COMMAND media_fixture_audit "${MEDIA_FIXTURES}/compat")
    set_tests_properties(media_fixture_audit_compat_test PROPERTIES TIMEOUT 180)
    foreach(name IN ITEMS costa_rica_4k60_hdr big_buck_bunny_4k60 dolby_surround_51_test av_sync_clock_45m_converted)
        if(EXISTS "${MEDIA_FIXTURES}/${name}.mp4")
            add_test(NAME media_fixture_playback_${name}_test COMMAND media_player_demo "${MEDIA_FIXTURES}/${name}.mp4" --test-fixture)
            set_tests_properties(media_fixture_playback_${name}_test PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
        endif()
    endforeach()
    # Temporal tile reuse on decoded video (costa_rica is 720p60 H.264): the same 5 s through the neural pass, fullscreen,
    # with reuse off, exact and tolerant (docs/neural.md).
    if(EXISTS "${MEDIA_FIXTURES}/costa_rica_4k60_hdr.mp4")
        add_test(NAME media_fixture_neural_reuse_test COMMAND media_player_demo "${MEDIA_FIXTURES}/costa_rica_4k60_hdr.mp4" --test-neural-reuse)
        set_tests_properties(media_fixture_neural_reuse_test PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    endif()
    # Perception and sound localisation on real footage (costa_rica's pictures and soundtrack, decoded by Media Foundation here only).
    add_executable(media_real_perception_tests tests/real_perception_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_real_perception_tests PRIVATE media_tracker media_timing mfplat mfreadwrite mfuuid ole32)
    if(EXISTS "${MEDIA_FIXTURES}/costa_rica_4k60_hdr.mp4")
        add_test(NAME media_real_perception_tests COMMAND media_real_perception_tests "${MEDIA_FIXTURES}")
        set_tests_properties(media_real_perception_tests PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77)
        # Low-power pacing on decoded film: 720p60 H.264 here, 1080p24 HEVC below.
        add_test(NAME media_fixture_pacing_60_test COMMAND media_player_demo "${MEDIA_FIXTURES}/costa_rica_4k60_hdr.mp4" --test-pacing)
        set_tests_properties(media_fixture_pacing_60_test PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    endif()
    if(EXISTS "${MEDIA_FIXTURES}/dolby_surround_51_test.mp4")
        add_test(NAME media_fixture_pacing_24_test COMMAND media_player_demo "${MEDIA_FIXTURES}/dolby_surround_51_test.mp4" --test-pacing)
        set_tests_properties(media_fixture_pacing_24_test PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    endif()
    # Everyday x264 + AAC files as ffmpeg writes them (tools/realworld/make_compat_matrix.py, 2026-11-04): the AAC's last access
    # unit is short (188 x 1024 + 512), which failed the end of every such file (stage 29, its last pictures unshown); and the
    # pictures start 0.5 s after the sound (an empty edit), which was refused at the open (stage 4) and then dead-locked the
    # pre-roll. Both play every picture to the end.
    add_test(NAME media_fixture_x264_aac_test COMMAND media_player_demo "${MEDIA_FIXTURES}/compat/x264_high_default.mp4" --test-pacing)
    add_test(NAME media_fixture_x264_delayed_video_test COMMAND media_player_demo "${MEDIA_FIXTURES}/compat/x264_start_offset.mp4" --test-pacing)
    set_tests_properties(media_fixture_x264_aac_test media_fixture_x264_delayed_video_test PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    # The reported file (mpv's encoder: the last video sample of no duration, refused at stage 4 before): plays to the end. The
    # file is a user's and stays private; stts_last_zero.mp4 is the same structure made synthetically (make_compat_matrix.py:
    # B-pictures in a 24000 timescale, 44.1 kHz mono AAC, moov after mdat, stts ending (1, 0)) and is the test everyone runs.
    add_test(NAME media_fixture_stts_last_zero_test COMMAND media_player_demo "${MEDIA_FIXTURES}/compat/stts_last_zero.mp4" --test-pacing)
    set_tests_properties(media_fixture_stts_last_zero_test PROPERTIES TIMEOUT 60)
    if(EXISTS "${MEDIA_FIXTURES}/compat/mpv_last_frame_zero.mp4")
        add_test(NAME media_fixture_mpv_last_frame_test COMMAND media_player_demo "${MEDIA_FIXTURES}/compat/mpv_last_frame_zero.mp4" --test-pacing)
        set_tests_properties(media_fixture_mpv_last_frame_test PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    endif()
    # The Matroska reader (API 16): a real file's metadata, every corruption and truncation of its head.
    # The Matroska demuxer (API 17): packets, times, seeks, switches and a corruption sweep on the fixtures.
    add_executable(media_matroska_demux_tests tests/matroska_demux_tests.cpp)
    target_link_libraries(media_matroska_demux_tests PRIVATE media_demux)
    target_compile_definitions(media_matroska_demux_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    add_test(NAME media_matroska_demux_tests COMMAND media_matroska_demux_tests "${MEDIA_FIXTURES}")
    set_tests_properties(media_matroska_demux_tests PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
    add_executable(media_matroska_tests tests/matroska_tests.cpp)
    target_link_libraries(media_matroska_tests PRIVATE media_demux)
    target_compile_definitions(media_matroska_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    add_test(NAME media_matroska_tests COMMAND media_matroska_tests "${MEDIA_FIXTURES}")
    set_tests_properties(media_matroska_tests PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    # media_probe and the failure texts (API 15): playable or not, and why, on the fixtures.
    add_executable(media_probe_tests tests/probe_tests.cpp)
    target_link_libraries(media_probe_tests PRIVATE media_sdk)
    target_compile_definitions(media_probe_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    add_test(NAME media_probe_tests COMMAND media_probe_tests "${MEDIA_FIXTURES}")
    set_tests_properties(media_probe_tests PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    # Matroska / WebM and AV1 (API 17; docs/matroska.md): what tools/realworld/make_mkv_fixtures.py writes (and its --large stream
    # copies), played through the windowed engine's pacing run: nothing dropped, nothing allocated, the sound decoded.
    foreach(case IN ITEMS "mkv_h264:mkv/h264_aac_srt_chapters.mkv" "mkv_hevc10:mkv/hevc10_aac.mkv" "mkv_h264_opus:mkv/h264_opus.mkv"
                          "webm_av1_opus:mkv/av1_opus.webm" "webm_av1_live:mkv/av1_opus_live.webm" "mkv_av1_aac:mkv/av1_aac.mkv"
                          "av1_4k60:av_sync_clock_45m.mp4" "mkv_av1_4k60:mkv/av_sync_clock_45m.mkv" "mkv_hevc_4k60:mkv/big_buck_bunny_4k60.mkv")
        string(REPLACE ":" ";" parts "${case}")
        list(GET parts 0 name)
        list(GET parts 1 file)
        if(EXISTS "${MEDIA_FIXTURES}/${file}")
            add_test(NAME media_fixture_${name}_test COMMAND media_player_demo "${MEDIA_FIXTURES}/${file}" --test-pacing)
            set_tests_properties(media_fixture_${name}_test PROPERTIES TIMEOUT 90)
        endif()
    endforeach()
    # Matroska seeks with B-pictures (2026-11-09): --test-seek on the everyday .mkv, from the file and over HTTP. Times there are
    # rounded to 1 ms, and DefaultDuration made two pictures cover a target (one presented out of order after the seek); each
    # landing must be exact, nothing dropped or out of order. Then media_http_mkv_seek_test: a 10-minute 720p B-picture .mkv with
    # its Cues at the end, over HTTP with the headers the server requires (401 without): five cue seeks, 0 dropped, 0 out of
    # order, 0 allocations, less than half the file read.
    foreach(case IN ITEMS "mkv_bframes_seek:mkv/h264_aac_srt_chapters.mkv:" "mkv_bframes_http_seek:mkv/h264_aac_srt_chapters.mkv:--http"
                          "mkv_hevc_bframes_seek:mkv/hevc_aac.mkv:" "mkv_hevc10_bframes_seek:mkv/hevc10_aac.mkv:")
        string(REPLACE ":" ";" parts "${case}")
        list(GET parts 0 name)
        list(GET parts 1 file)
        list(LENGTH parts count)
        set(extra "")
        if(count GREATER 2)
            list(GET parts 2 extra)
        endif()
        if(EXISTS "${MEDIA_FIXTURES}/${file}")
            add_test(NAME media_fixture_${name}_test COMMAND media_player_demo "${MEDIA_FIXTURES}/${file}" --test-seek ${extra})
            set_tests_properties(media_fixture_${name}_test PROPERTIES TIMEOUT 60)
        endif()
    endforeach()
    if(EXISTS "${MEDIA_FIXTURES}/mkv/h264_bframes_720p_10m.mkv")
        add_test(NAME media_http_mkv_seek_test COMMAND media_player_demo "${MEDIA_FIXTURES}/mkv/h264_bframes_720p_10m.mkv" --test-remote-seek --http)
        set_tests_properties(media_http_mkv_seek_test PROPERTIES TIMEOUT 120)
    endif()
    # lg_black_4k60_hdr (4K Main10 PQ at 59.94 fps, tone-mapped): continuous from the first picture, nothing dropped.
    # It used to drop pictures in its first 0.3 s in a third of the runs (docs/fixtures.md), so the start is also
    # tested as such: twenty new processes in a row, none of which may drop a picture.
    if(EXISTS "${MEDIA_FIXTURES}/lg_black_4k60_hdr.mp4")
        add_test(NAME media_fixture_lg_hdr_playback_test COMMAND media_player_demo "${MEDIA_FIXTURES}/lg_black_4k60_hdr.mp4" --test-fixture-hdr)
        set_tests_properties(media_fixture_lg_hdr_playback_test PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
        add_test(NAME media_fixture_lg_hdr_cold_start_test COMMAND "${CMAKE_COMMAND}" -DCOUNT=20 "-DEXE=$<TARGET_FILE:media_player_demo>"
            "-DARG1=${MEDIA_FIXTURES}/lg_black_4k60_hdr.mp4" -DARG2=--test-fixture-hdr -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/repeat_command.cmake")
        set_tests_properties(media_fixture_lg_hdr_cold_start_test PROPERTIES TIMEOUT 240 SKIP_REGULAR_EXPRESSION "SKIP fixture not generated")
    endif()
    # Big Buck Bunny with its own multilingual track: quotes in four scripts, each landing on its cue.
    if(EXISTS "${MEDIA_FIXTURES}/big_buck_bunny_4k60.mp4" AND EXISTS "${MEDIA_FIXTURES}/big_buck_bunny.srt")
        add_test(NAME media_fixture_subtitle_search_test COMMAND media_player_demo "${MEDIA_FIXTURES}/big_buck_bunny_4k60.mp4" --test-fixture-search
            --subtitles "${MEDIA_FIXTURES}/big_buck_bunny.srt")
        set_tests_properties(media_fixture_subtitle_search_test PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    endif()
    # Subtitles after open in the window (API 17): attached at 1 s from another thread, replaced at 3 s; drawn, 0 dropped, 0 allocations.
    foreach(case IN ITEMS "mp4:compat/x264_high_default.mp4" "mkv:mkv/h264_aac_srt_chapters.mkv")
        string(REPLACE ":" ";" parts "${case}")
        list(GET parts 0 name)
        list(GET parts 1 file)
        if(EXISTS "${MEDIA_FIXTURES}/${file}")
            add_test(NAME media_fixture_attach_${name}_test COMMAND media_player_demo "${MEDIA_FIXTURES}/${file}" --test-attach WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
            set_tests_properties(media_fixture_attach_${name}_test PROPERTIES TIMEOUT 60)
        endif()
    endforeach()
    # A file still refused at stage 4 (no usable video track): VP9 in WebM. The AV1 clock used to be this test; since API 17 it
    # plays (media_fixture_av1_4k60_test).
    foreach(name IN ITEMS vp9_opus)
        add_test(NAME media_fixture_refused_${name}_test COMMAND media_player_demo "${MEDIA_FIXTURES}/compat/${name}.webm" --expect-stage 4)
        set_tests_properties(media_fixture_refused_${name}_test PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    endforeach()
    # Voice ingest: WASAPI capture into the lock-free PCM ring, verified on a loopback of a tone the sink plays
    # (serial: it listens to the endpoint, so other tests' audio would be in it). `media_capture_tests microphone`
    # opens the real microphone and is run by hand.
    add_executable(media_capture_tests tests/capture_tests.cpp tests/allocation_probe.cpp)
    target_link_libraries(media_capture_tests PRIVATE media_windows ole32)
    add_test(NAME media_capture_tests COMMAND media_capture_tests loopback)
    set_tests_properties(media_capture_tests PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
    # The voice activity detector draining that ring: a synthetic word between silences, played by the sink.
    add_test(NAME media_capture_vad_test COMMAND media_capture_tests vad)
    set_tests_properties(media_capture_vad_test PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
    # Offline tool (not a test): voice command templates and the held-out test corpus, from Windows' speech synthesiser.
    add_executable(media_make_voice_templates tools/make_voice_templates.cpp)
    target_link_libraries(media_make_voice_templates PRIVATE media_ai ole32 sapi)
    # Offline tool (not a test): the platform's AV1 decoders and the software one's speed (API 17, the CPU-MFT fallback).
    add_executable(media_av1_mft_probe tools/av1_mft_probe.cpp)
    target_link_libraries(media_av1_mft_probe PRIVATE media_host media_demux)
    target_compile_definitions(media_av1_mft_probe PRIVATE _CRT_SECURE_NO_WARNINGS)
    # Offline tool (not a test): the platform's Opus decoder on a file's Opus track (API 17).
    add_executable(media_opus_mft_probe tools/opus_mft_probe.cpp)
    target_link_libraries(media_opus_mft_probe PRIVATE media_host media_demux)
    target_compile_definitions(media_opus_mft_probe PRIVATE _CRT_SECURE_NO_WARNINGS)
    # Offline tool (not a test): AVION's side of the head-to-head benchmark (docs/benchmarks.md, tools/bench/run_headtohead.py).
    add_executable(media_bench_headtohead tools/bench_headtohead.cpp)
    target_link_libraries(media_bench_headtohead PRIVATE media_host media_codec media_demux media_tracker)
    # Offline tool (not a test): the virtual IMU over recorded luma pictures, for the real-world check against a gyroscope
    # (tools/realworld/eval_tumvi.py, docs/realworld.md).
    add_executable(media_eval_vimu tools/eval_vimu.cpp)
    target_link_libraries(media_eval_vimu PRIVATE media_tracker)
    target_compile_definitions(media_eval_vimu PRIVATE _CRT_SECURE_NO_WARNINGS)
    # Offline tool (not a test): AVION's tile flow on recorded pictures, against dense ground truth (tools/realworld/eval_flow.py).
    add_executable(media_eval_flow tools/eval_flow.cpp)
    target_link_libraries(media_eval_flow PRIVATE media_tracker)
    target_compile_definitions(media_eval_flow PRIVATE _CRT_SECURE_NO_WARNINGS)
    # Its diagnostic twin (2026-11-05): the coarse level's search traced tile by tile, from its own copy of Perception.cpp.
    add_executable(media_flow_trace tools/flow_trace.cpp src/video/Perception.cpp)
    target_link_libraries(media_flow_trace PRIVATE media_tracker)
    target_compile_definitions(media_flow_trace PRIVATE _CRT_SECURE_NO_WARNINGS MEDIA_PERCEPTION_TRACE)
    # Offline tool (not a test): the site's showcase pictures, made through the C SDK (tools/showcase/make_showcase.py).
    add_executable(media_showcase tools/showcase/avion_showcase.c)
    target_link_libraries(media_showcase PRIVATE media_sdk)
    set_target_properties(media_showcase PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_definitions(media_showcase PRIVATE _CRT_SECURE_NO_WARNINGS)
    # The site's quickstart, built so the snippet it shows compiles (docs/site, "Download").
    add_executable(media_quickstart tools/showcase/quickstart.c)
    target_link_libraries(media_quickstart PRIVATE media_sdk)
    set_target_properties(media_quickstart PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    # Embedding (API 12; docs/sdk.md, "Embedding: API 12"): avion.dll, the C99 SDK for any language (its exports are every
    # function c_api.h declares, read from the header at configure time), and a C99 host that plays inside its own window.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/include/media/c_api.h")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/include/media/c_api.h" avion_declarations REGEX "^(void|media_status|uint64_t|uint32_t|size_t|int) +\\*? *media_[a-z0-9_]+\\(")
    set(avion_exports "LIBRARY avion\nEXPORTS\n")
    foreach(declaration IN LISTS avion_declarations)
        if(declaration MATCHES "(media_[a-z0-9_]+)\\(")
            string(APPEND avion_exports "    ${CMAKE_MATCH_1}\n")
        endif()
    endforeach()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/avion.def" "${avion_exports}")
    add_library(avion SHARED src/sdk/AvionDll.cpp "${CMAKE_CURRENT_BINARY_DIR}/avion.def")
    target_link_libraries(avion PRIVATE media_sdk)
    target_include_directories(avion PUBLIC include)
    foreach(embed media_embed_tests media_embed_dll_tests)
        add_executable(${embed} tests/embed_tests.c tests/allocation_probe_c.cpp tests/allocation_probe.cpp)
        target_include_directories(${embed} PRIVATE tests)
        set_target_properties(${embed} PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
        target_compile_definitions(${embed} PRIVATE _CRT_SECURE_NO_WARNINGS)
        # Relative paths: a C program's argv is the ANSI code page on Windows (the build folder's name may not be).
        add_test(NAME ${embed} COMMAND ${embed} playback_fixture.mp4 portable_fixture.mp4 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
        set_tests_properties(${embed} PROPERTIES FIXTURES_REQUIRED "playback_fixture;portable_fixture" TIMEOUT 120 RUN_SERIAL TRUE)
    endforeach()
    target_link_libraries(media_embed_tests PRIVATE media_sdk)
    target_link_libraries(media_embed_dll_tests PRIVATE avion) # The same host through the DLL's exports.
    # Texture output (API 13, 14): Direct3D 11 and Direct3D 12 host renderers reading the engine's shared textures, in the 8-bit,
    # 10-bit PQ and FP16 scRGB formats; host mode's hardware decoding into them (API 14).
    add_executable(media_texture_output_tests tests/texture_output_tests.cpp tests/allocation_probe.cpp)
    target_include_directories(media_texture_output_tests PRIVATE tests)
    target_link_libraries(media_texture_output_tests PRIVATE media_sdk d3d11 d3d12 dxgi)
    add_test(NAME media_texture_output_tests COMMAND media_texture_output_tests playback_chapters.mp4 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME media_texture_output_d3d12_tests COMMAND media_texture_output_tests playback_chapters.mp4 --d3d12 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME media_texture_output_pq10_tests COMMAND media_texture_output_tests playback_chapters.mp4 --d3d12 --format pq10 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME media_texture_output_scrgb_tests COMMAND media_texture_output_tests playback_chapters.mp4 --format scrgb16 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME media_texture_output_hdr_tests COMMAND media_texture_output_tests playback_hevc10_4k_pq.mp4 --d3d12 --format pq10 --hdr WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME media_host_gpu_decode_tests COMMAND media_texture_output_tests playback_chapters.mp4 --host-mode WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME media_host_gpu_decode_hdr_tests COMMAND media_texture_output_tests playback_hevc10_4k_pq.mp4 --host-mode --d3d12 --format pq10 --hdr WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(media_texture_output_tests media_texture_output_d3d12_tests media_texture_output_pq10_tests media_texture_output_scrgb_tests
                         media_host_gpu_decode_tests PROPERTIES FIXTURES_REQUIRED chapters_fixture TIMEOUT 120 RUN_SERIAL TRUE)
    set_tests_properties(media_texture_output_hdr_tests media_host_gpu_decode_hdr_tests PROPERTIES FIXTURES_REQUIRED hevc_pq_fixture TIMEOUT 120 RUN_SERIAL TRUE SKIP_RETURN_CODE 77)
    # Developer packages (API 14; docs/sdk.md, "Packages"): the Python wheel and the NuGet package, from avion.dll, into
    # artifacts/packages (cmake --build build --config Release --target avion_packages). The Python test builds the wheel itself,
    # unpacks it and imports it from there.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME media_python_binding_test COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/tests/python_binding_test.py"
                 "$<TARGET_FILE:avion>" "${CMAKE_CURRENT_BINARY_DIR}")
        set_tests_properties(media_python_binding_test PROPERTIES FIXTURES_REQUIRED "pulse_fixture;two_targets_fixture;h264_bframes_fixture;chapters_fixture"
                             TIMEOUT 300 RUN_SERIAL TRUE)
        find_program(MEDIA_DOTNET dotnet)
        if(MEDIA_DOTNET)
            add_test(NAME media_nuget_package_test COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/tests/nuget_package_test.py"
                     "$<TARGET_FILE:avion>" "$<TARGET_LINKER_FILE:avion>" "${CMAKE_CURRENT_BINARY_DIR}")
            set_tests_properties(media_nuget_package_test PROPERTIES FIXTURES_REQUIRED "pulse_fixture;chapters_fixture" TIMEOUT 600 RUN_SERIAL TRUE)
        endif()
        add_custom_target(avion_packages
            COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/tools/python/build_wheel.py" "$<TARGET_FILE:avion>" "${CMAKE_CURRENT_SOURCE_DIR}/artifacts/packages"
            COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/tools/nuget/build_nuget.py" "$<TARGET_FILE:avion>" "$<TARGET_LINKER_FILE:avion>"
                    "${CMAKE_CURRENT_SOURCE_DIR}/artifacts/packages"
            DEPENDS avion VERBATIM)
    endif()
    add_executable(media_core_tests tests/core_tests.cpp)
    target_link_libraries(media_core_tests PRIVATE media_core)
    target_compile_definitions(media_core_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    add_test(NAME media_core_tests COMMAND media_core_tests)
    set_tests_properties(media_core_tests PROPERTIES TIMEOUT 90)
endif()

# Unattended suites (docs/session.md, "Staying awake"): media_stay_awake holds a power request from the first test to the end of
# CTest, between the test processes too; every test requires it, so any subset starts it.
if(WIN32)
    add_executable(media_stay_awake tests/stay_awake.cpp)
    get_property(media_tests DIRECTORY PROPERTY TESTS)
    add_test(NAME media_stay_awake COMMAND media_stay_awake)
    set_tests_properties(media_stay_awake PROPERTIES FIXTURES_SETUP stay_awake TIMEOUT 30)
    set_property(TEST ${media_tests} APPEND PROPERTY FIXTURES_REQUIRED stay_awake)
endif()
get_property(media_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(media_target IN LISTS media_targets)
    if(media_target MATCHES "^media_" OR media_target STREQUAL "avion")
        if(MSVC)
            target_compile_options(${media_target} PRIVATE /W4 /WX /utf-8 $<$<COMPILE_LANGUAGE:CXX>:/permissive- /Zc:__cplusplus>)
            set_property(TARGET ${media_target} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        else()
            target_compile_options(${media_target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
        endif()
    endif()
endforeach()
