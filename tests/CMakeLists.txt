include_directories(${PROJECT_SOURCE_DIR}/src)

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
