include_directories(${PROJECT_SOURCE_DIR}/src)

ecm_add_test(
    HelpMenuTest.cpp
    ../src/Gui/HelpMenu.cpp
    ../src/Gui/SpectacleMenu.cpp
    TEST_NAME "help_menu_test"
    LINK_LIBRARIES Qt::Test Qt::Widgets Qt::Quick KF6::CoreAddons KF6::XmlGui KF6::ConfigGui KF6::I18n KQuickImageEditor
)
target_include_directories(help_menu_test PRIVATE "${PROJECT_BINARY_DIR}/src")
# The core declaration includes the application's generated settings header.
add_dependencies(help_menu_test spectacle)
set_tests_properties(help_menu_test PROPERTIES LABELS "headless;branding" ENVIRONMENT "QT_QPA_PLATFORM=offscreen;LANGUAGE=en;LC_ALL=C")

SET(FILENAME_TEST_SRCS
    FilenameTest.cpp
    ../src/ShortcutActions.cpp
    ../src/ExportManager.cpp
    ../src/Platforms/ImagePlatform.cpp
    ../src/Platforms/VideoPlatform.cpp
)

ecm_qt_declare_logging_category(FILENAME_TEST_SRCS
    HEADER spectacle_debug.h
    IDENTIFIER SPECTACLE_LOG
    CATEGORY_NAME spectacle
    DESCRIPTION "spectacle (general)"
    EXPORT SPECTACLE
)

kconfig_add_kcfg_files(FILENAME_TEST_SRCS GENERATE_MOC ${PROJECT_SOURCE_DIR}/src/Gui/SettingsDialog/settings.kcfgc)

ecm_add_test(
    ${FILENAME_TEST_SRCS}
    TEST_NAME "filename_test"
    LINK_LIBRARIES  Qt::Test
        Qt::PrintSupport Qt::Qml KF6::I18n KF6::ConfigCore KF6::GlobalAccel KF6::KIOCore KF6::WindowSystem KF6::XmlGui KF6::GuiAddons KF6::PrisonScanner
)

SET(PLATFORM_STARTUP_TEST_SRCS
    PlatformStartupTest.cpp
    ../src/PlatformStartup.cpp
)

ecm_add_test(
    ${PLATFORM_STARTUP_TEST_SRCS}
    TEST_NAME "platform_startup_test"
    LINK_LIBRARIES Qt::Test
)

set(VIDEO_RECORDING_TEST_SRCS
    VideoRecordingTest.cpp
    ../src/ExportManager.cpp
    ../src/PlatformOutput.cpp
    ../src/ShortcutActions.cpp
    ../src/Platforms/VideoPlatform.cpp
    ../src/Platforms/VideoPlatformX11.cpp
    ../src/Platforms/Audio/AudioCaptureWorker.cpp
    ../src/Platforms/Audio/AudioPcmMixer.cpp
    ../src/Platforms/Encoding/AnimatedEncoderWorker.cpp
    ../src/Platforms/X11/X11Connection.cpp
    ../src/Platforms/X11/X11FrameGrabWorker.cpp
    ../src/Platforms/X11/X11TargetSelector.cpp
)

ecm_qt_declare_logging_category(VIDEO_RECORDING_TEST_SRCS
    HEADER spectacle_debug.h
    IDENTIFIER SPECTACLE_LOG
    CATEGORY_NAME spectacle.recording.test
)

kconfig_add_kcfg_files(VIDEO_RECORDING_TEST_SRCS GENERATE_MOC ${PROJECT_SOURCE_DIR}/src/Gui/SettingsDialog/settings.kcfgc)

ecm_add_test(
    ${VIDEO_RECORDING_TEST_SRCS}
    TEST_NAME "video_recording_test"
    LINK_LIBRARIES
        Qt::Test
        Qt::Gui
        Qt::Multimedia
        Qt::PrintSupport
        Qt::Qml
        KF6::ConfigCore
        KF6::GlobalAccel
        KF6::GuiAddons
        KF6::I18n
        KF6::KIOCore
        KF6::PrisonScanner
        KF6::WindowSystem
        KF6::XmlGui
        XCB::IMAGE
        XCB::CURSOR
        XCB::RANDR
        XCB::UTIL
        XCB::XFIXES
        FFmpeg::AVCODEC
        FFmpeg::AVFORMAT
        FFmpeg::AVUTIL
        FFmpeg::SWSCALE
        PulseAudio::PulseAudio
)

ecm_add_test(
    AudioPcmMixerTest.cpp
    ../src/Platforms/Audio/AudioPcmMixer.cpp
    TEST_NAME "audio_pcm_mixer_test"
    LINK_LIBRARIES Qt::Test
)
set_tests_properties(audio_pcm_mixer_test PROPERTIES LABELS "headless;audio")

ecm_add_test(
    ScreenshotTimingTest.cpp
    TEST_NAME "screenshot_timing_test"
    LINK_LIBRARIES Qt::Test
)
set_tests_properties(screenshot_timing_test PROPERTIES LABELS "headless;screenshot")

ecm_add_test(
    AnimatedEncoderWorkerTest.cpp
    ../src/Platforms/Encoding/AnimatedEncoderWorker.cpp
    TEST_NAME "animated_encoder_worker_test"
    LINK_LIBRARIES
        Qt::Test
        Qt::Gui
        FFmpeg::AVCODEC
        FFmpeg::AVFORMAT
        FFmpeg::AVUTIL
        FFmpeg::SWSCALE
)
set_tests_properties(animated_encoder_worker_test PROPERTIES LABELS "codec;integration")

ecm_add_test(
    X11TargetSelectorTest.cpp
    ../src/Platforms/X11/X11TargetSelector.cpp
    TEST_NAME "x11_target_selector_test"
    LINK_LIBRARIES
        Qt::Test
        Qt::Gui
        XCB::CURSOR
        XCB::RANDR
        XCB::UTIL
)
set_tests_properties(x11_target_selector_test PROPERTIES LABELS "x11;integration")

ecm_add_test(
    X11WindowCaptureTest.cpp
    ../src/Platforms/X11/X11WindowCapture.cpp
    TEST_NAME "x11_window_capture_test"
    LINK_LIBRARIES Qt::Test Qt::Gui XCB::IMAGE XCB::COMPOSITE XCB::SHAPE
)
set_tests_properties(x11_window_capture_test PROPERTIES LABELS "x11;integration;security")

if(BUILD_TESTING)
    find_program(DBUS_RUN_SESSION_EXECUTABLE dbus-run-session)
    if(DBUS_RUN_SESSION_EXECUTABLE)
        add_executable(dbus_activation_test DBusActivationTest.cpp)
        target_link_libraries(dbus_activation_test PRIVATE Qt::Test Qt::DBus)
        target_compile_definitions(dbus_activation_test PRIVATE
            TEST_APP_BINARY="$<TARGET_FILE:spectacle>"
            TEST_DBUS_DIRECTORY="${PROJECT_BINARY_DIR}/dbus"
            TEST_DESKTOP_FILE="${PROJECT_BINARY_DIR}/desktop/org.kde.spectacle.desktop"
        )
        add_dependencies(dbus_activation_test spectacle)
        configure_file(dbus-test.conf.in "${CMAKE_CURRENT_BINARY_DIR}/dbus-test.conf" @ONLY)
        foreach(service_name IN ITEMS org.sonicde.screenies org.kde.spectacle org.kde.Spectacle)
            # Use the actual generated service metadata, replacing only the
            # installed executable path so tests cannot launch a stale binary.
            file(READ "${PROJECT_BINARY_DIR}/dbus/${service_name}.service" _dbus_test_service)
            string(REGEX REPLACE "Exec=[^\n]*" "Exec=\"$<TARGET_FILE:spectacle>\" --dbus"
                _dbus_test_service "${_dbus_test_service}")
            file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/dbus-activation-services/${service_name}.service"
                CONTENT "${_dbus_test_service}")
        endforeach()
        foreach(directory IN ITEMS config cache data)
            file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/dbus-test-state/${directory}")
        endforeach()
        set(_dbus_test_command "${DBUS_RUN_SESSION_EXECUTABLE}"
            "--config-file=${CMAKE_CURRENT_BINARY_DIR}/dbus-test.conf"
            -- "$<TARGET_FILE:dbus_activation_test>")
        find_program(XVFB_RUN_EXECUTABLE xvfb-run)
        if(XVFB_RUN_EXECUTABLE)
            list(PREPEND _dbus_test_command "${XVFB_RUN_EXECUTABLE}" --auto-servernum)
        endif()
        add_test(NAME dbus_activation_test COMMAND ${_dbus_test_command})
        set_tests_properties(dbus_activation_test PROPERTIES
            LABELS "dbus;integration"
            ENVIRONMENT "SCREENIES_TEST_PRIVATE_BUS=1;QT_QPA_PLATFORM=xcb;XDG_SESSION_TYPE=x11;QT_QUICK_BACKEND=software;XDG_CONFIG_HOME=${CMAKE_CURRENT_BINARY_DIR}/dbus-test-state/config;XDG_CACHE_HOME=${CMAKE_CURRENT_BINARY_DIR}/dbus-test-state/cache;XDG_DATA_HOME=${CMAKE_CURRENT_BINARY_DIR}/dbus-test-state/data"
            TIMEOUT 60
        )
    endif()
endif()
