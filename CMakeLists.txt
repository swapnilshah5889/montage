cmake_minimum_required(VERSION 3.21)
project(Montage VERSION 0.1.0 LANGUAGES CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Quick QuickControls2 Multimedia Network Qml WebSockets)
qt_standard_project_setup(REQUIRES 6.8)

# FFmpeg: libavformat, libavcodec, libavutil, libswscale, libswresample.
# Point FFMPEG_ROOT at a custom (e.g. LGPL-only) build if needed.
set(FFMPEG_ROOT "/opt/homebrew/opt/ffmpeg" CACHE PATH "FFmpeg install prefix")
find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h HINTS ${FFMPEG_ROOT}/include REQUIRED)
set(FFMPEG_LIBRARIES "")
foreach(lib avformat avcodec avutil swscale swresample)
    find_library(FFMPEG_${lib}_LIBRARY ${lib} HINTS ${FFMPEG_ROOT}/lib REQUIRED)
    list(APPEND FFMPEG_LIBRARIES ${FFMPEG_${lib}_LIBRARY})
endforeach()

# Engine: document model, decoders, renderer, preview and export.
add_library(montage_engine STATIC
    src/model/Draft.h src/model/Draft.cpp
    src/engine/FFmpeg.h
    src/engine/VideoDecoder.h src/engine/VideoDecoder.cpp
    src/engine/AudioDecoder.h src/engine/AudioDecoder.cpp
    src/engine/MediaProbe.h src/engine/MediaProbe.cpp
    src/engine/Animation.h src/engine/Animation.cpp
    src/engine/Effects.h src/engine/Effects.mm
    src/engine/Reframe.h src/engine/Reframe.mm
    src/engine/Beats.h src/engine/Beats.cpp
    src/engine/Speech.h src/engine/Speech.cpp
    src/engine/Scenes.h src/engine/Scenes.mm
    src/engine/Renderer.h src/engine/Renderer.cpp
    src/engine/PreviewEngine.h src/engine/PreviewEngine.cpp
    src/engine/ExportJob.h src/engine/ExportJob.cpp
)
target_include_directories(montage_engine PUBLIC src ${FFMPEG_INCLUDE_DIR})
target_link_libraries(montage_engine PUBLIC Qt6::Core Qt6::Gui Qt6::Multimedia ${FFMPEG_LIBRARIES}
    "-framework CoreImage" "-framework Metal" "-framework Foundation" "-framework CoreGraphics" "-framework Vision" "-framework CoreVideo")
set_source_files_properties(src/engine/Effects.mm src/engine/Reframe.mm src/engine/Scenes.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")

# AI layer: model-agnostic chat providers, edit plans, prompts.
add_library(montage_ai STATIC
    src/ai/ChatProvider.h src/ai/ChatProvider.cpp
    src/ai/Keychain.h src/ai/Keychain.cpp
    src/ai/EditPlan.h src/ai/EditPlan.cpp
    src/ai/PlanGuards.h src/ai/PlanGuards.cpp
    src/ai/AdStyles.h src/ai/AdStyles.cpp
    src/ai/Brand.h src/ai/Brand.cpp
    src/ai/Finishing.h src/ai/Finishing.cpp
    src/ai/MediaIndexer.h src/ai/MediaIndexer.cpp
    src/ai/Prompts.h src/ai/Prompts.cpp
    src/ai/Generators.h src/ai/Generators.cpp
)
target_link_libraries(montage_ai PUBLIC montage_engine Qt6::Network Qt6::WebSockets)
# Helper scripts the generators run with each model's own Python.
qt_add_resources(montage_ai "generate_scripts" PREFIX "/generate" BASE scripts/generate
    FILES scripts/generate/kokoro_tts.py scripts/generate/acestep_music.py)
if(APPLE)
    target_link_libraries(montage_ai PUBLIC "-framework Security" "-framework CoreFoundation")
endif()

qt_add_executable(Montage MACOSX_BUNDLE src/main.cpp
    src/ai/EditorTools.h src/ai/EditorTools.cpp
    src/app/AgentCli.h src/app/AgentCli.cpp)

set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
qt_add_qml_module(Montage
    URI Montage
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/FlatButton.qml
        qml/TopBar.qml
        qml/MediaPanel.qml
        qml/PlayerPanel.qml
        qml/PropertySlider.qml
        qml/InspectorPanel.qml
        qml/TimeRuler.qml
        qml/SegmentItem.qml
        qml/TimelinePanel.qml
        qml/ChatPanel.qml
        qml/ModelSettings.qml
        qml/BrandSettings.qml
    SOURCES
        src/app/Agent.h src/app/Agent.cpp
        src/app/Project.h src/app/Project.cpp
        src/app/Player.h src/app/Player.cpp
        src/app/Exporter.h src/app/Exporter.cpp
        src/app/VideoSurface.h src/app/VideoSurface.cpp
)

target_include_directories(Montage PRIVATE src/app)
target_link_libraries(Montage PRIVATE montage_ai Qt6::Quick Qt6::QuickControls2)
set_target_properties(Montage PROPERTIES
    MACOSX_BUNDLE_GUI_IDENTIFIER dev.montage.editor
    MACOSX_BUNDLE_BUNDLE_NAME Montage
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
)

# Editing-model tests: ctest, or build/project_test <media dir>
enable_testing()
add_executable(plan_test tests/plan_test.cpp)
target_include_directories(plan_test PRIVATE src)
target_link_libraries(plan_test PRIVATE montage_ai)
add_test(NAME plan COMMAND plan_test)

add_executable(project_test tests/project_test.cpp src/app/Project.h src/app/Project.cpp)
target_include_directories(project_test PRIVATE src)
target_link_libraries(project_test PRIVATE montage_engine Qt6::Qml)
set(MONTAGE_TEST_MEDIA "" CACHE PATH "Directory with clipA.mp4, clipB.mov, music.mp3, photo.png")
if(MONTAGE_TEST_MEDIA)
    add_test(NAME project COMMAND project_test ${MONTAGE_TEST_MEDIA})
endif()
