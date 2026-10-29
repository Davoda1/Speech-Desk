cmake_minimum_required(VERSION 3.21)
project(SpeechDesk VERSION 1.0.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Network Multimedia Svg)
add_executable(SpeechDesk WIN32
    src/main.cpp src/mainwindow.cpp src/mainwindow.h
    src/numberededitor.cpp src/numberededitor.h
    src/settingsdialog.cpp src/settingsdialog.h
    src/textsegments.cpp src/textsegments.h
    src/audiomerger.cpp src/audiomerger.h
    src/ttsclient.cpp src/ttsclient.h
    src/sttservice.cpp src/sttservice.h src/modelpacks.cpp src/modelpacks.h src/speechtypes.h
    resources/app.qrc)
target_link_libraries(SpeechDesk PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Multimedia Qt6::Svg)
if(WIN32)
    enable_language(RC)
    target_include_directories(SpeechDesk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/resources)
    target_sources(SpeechDesk PRIVATE resources/windows.rc)
else()
    find_package(Qt6 REQUIRED COMPONENTS DBus)
    target_link_libraries(SpeechDesk PRIVATE Qt6::DBus)
    set_target_properties(SpeechDesk PROPERTIES OUTPUT_NAME speechdesk BUILD_RPATH "$ORIGIN/lib" INSTALL_RPATH "$ORIGIN/../lib/speechdesk/lib")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/assets/models")
    if(WIN32)
        set(SPEECHDESK_NATIVE_PLATFORM windows)
    else()
        set(SPEECHDESK_NATIVE_PLATFORM linux)
    endif()
    add_custom_target(stage_resources
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/vendor/assets/models" "$<TARGET_FILE_DIR:SpeechDesk>/resources/models"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/vendor/assets/engines/${SPEECHDESK_NATIVE_PLATFORM}" "$<TARGET_FILE_DIR:SpeechDesk>/resources/bin"
        DEPENDS SpeechDesk
        COMMENT "Stage bundled offline recognition engines and models")
endif()
option(SPEECHDESK_BUILD_TESTS "Build focused transport and text tests" OFF)
if(SPEECHDESK_BUILD_TESTS)
    enable_testing()
    find_package(Qt6 REQUIRED COMPONENTS Test)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(textsegments_test tests/textsegments_test.cpp src/textsegments.cpp)
    target_include_directories(textsegments_test PRIVATE src)
    target_link_libraries(textsegments_test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME textsegments COMMAND textsegments_test)
    add_executable(ttsclient_test tests/ttsclient_test.cpp src/ttsclient.cpp)
    target_include_directories(ttsclient_test PRIVATE src)
    target_link_libraries(ttsclient_test PRIVATE Qt6::Core Qt6::Network Qt6::Test)
    add_test(NAME ttsclient COMMAND ttsclient_test)
    set_tests_properties(ttsclient PROPERTIES ENVIRONMENT "SPEECHDESK_FIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/tests/tts_fixture_server.py;SPEECHDESK_PYTHON=${Python3_EXECUTABLE}")
    add_executable(sttservice_test tests/sttservice_test.cpp src/sttservice.cpp src/modelpacks.cpp)
    target_include_directories(sttservice_test PRIVATE src)
    target_link_libraries(sttservice_test PRIVATE Qt6::Core)
    add_executable(modelpacks_test tests/modelpacks_test.cpp src/modelpacks.cpp)
    target_include_directories(modelpacks_test PRIVATE src)
    target_link_libraries(modelpacks_test PRIVATE Qt6::Core Qt6::Test)
    add_test(NAME modelpacks COMMAND modelpacks_test)
    add_executable(providerprofiles_test tests/providerprofiles_test.cpp
        src/settingsdialog.cpp src/settingsdialog.h src/modelpacks.cpp src/modelpacks.h
        src/ttsclient.cpp src/ttsclient.h)
    target_include_directories(providerprofiles_test PRIVATE src)
    target_link_libraries(providerprofiles_test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Test)
    add_test(NAME providerprofiles COMMAND providerprofiles_test)
    set_tests_properties(providerprofiles PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    if(NOT WIN32)
        add_executable(ui_flow_test tests/ui_flow_test.cpp
            src/mainwindow.cpp src/mainwindow.h src/numberededitor.cpp src/numberededitor.h
            src/settingsdialog.cpp src/settingsdialog.h src/textsegments.cpp
            src/audiomerger.cpp src/audiomerger.h src/ttsclient.cpp src/ttsclient.h
            src/sttservice.cpp src/sttservice.h src/modelpacks.cpp src/modelpacks.h resources/app.qrc)
        target_include_directories(ui_flow_test PRIVATE src)
        target_link_libraries(ui_flow_test PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Multimedia Qt6::Svg Qt6::DBus Qt6::Test)
        add_test(NAME ui_profiles COMMAND ui_flow_test
            openRouterVoicesFollowSelectedModel unifiedEditorFitsMinimumSizeWithDictation
            profileEditsSurviveSwitching cleanStartupAndThemesPreserveTextModificationState)
        set_tests_properties(ui_profiles PROPERTIES ENVIRONMENT
            "QT_QPA_PLATFORM=offscreen;QT_MEDIA_BACKEND=ffmpeg;SPEECHDESK_FIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/tests/tts_fixture_server.py;SPEECHDESK_PYTHON=${Python3_EXECUTABLE}")
        # Playback integration cases require an actual or virtual audio output.
    endif()
endif()
