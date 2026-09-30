cmake_minimum_required(VERSION 3.22)

project(DiscordMusicBotNative VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(MUSICBOT_FETCH_DPP "Download DPP when it is not installed" ON)
set(MUSICBOT_BUNDLED_YT_DLP "" CACHE FILEPATH "Standalone yt-dlp binary to package")
set(MUSICBOT_BUNDLED_DENO "" CACHE FILEPATH "Standalone Deno binary to package")

find_package(dpp 10.1.6 CONFIG QUIET)
if(NOT dpp_FOUND)
    if(NOT MUSICBOT_FETCH_DPP)
        message(FATAL_ERROR "DPP 10.1.6+ was not found")
    endif()
    include(FetchContent)
    set(DPP_BUILD_TEST OFF CACHE BOOL "" FORCE)
    set(DPP_INSTALL OFF CACHE BOOL "" FORCE)
    set(DPP_NO_VCPKG ON CACHE BOOL "" FORCE)
    set(DPP_NO_CONAN ON CACHE BOOL "" FORCE)
    set(DPP_USE_EXTERNAL_JSON ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(_musicbot_library_suffixes "${CMAKE_FIND_LIBRARY_SUFFIXES}")
    FetchContent_Declare(
        dpp
        GIT_REPOSITORY https://github.com/brainboxdotcc/DPP.git
        GIT_TAG v10.1.6
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(dpp)
    set(CMAKE_FIND_LIBRARY_SUFFIXES "${_musicbot_library_suffixes}")
    unset(_musicbot_library_suffixes)
endif()
if(TARGET dppstatic)
    set(MUSICBOT_DPP_TARGET dppstatic)
elseif(TARGET dpp::dpp)
    set(MUSICBOT_DPP_TARGET dpp::dpp)
elseif(TARGET dpp)
    set(MUSICBOT_DPP_TARGET dpp)
else()
    message(FATAL_ERROR "A usable DPP target was not found")
endif()
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)
find_path(OPUS_INCLUDE_DIR NAMES opus/opus.h REQUIRED)
find_library(OPUS_LIBRARY NAMES opus REQUIRED)

add_library(musicbot_core
    src/config.cpp
    src/process.cpp
    src/http_stream.cpp
    src/youtube.cpp
    src/webm_opus.cpp
    src/opus_volume.cpp
)
target_include_directories(musicbot_core
    PUBLIC include
    PRIVATE "${OPUS_INCLUDE_DIR}"
)
target_compile_features(musicbot_core PUBLIC cxx_std_20)
target_link_libraries(musicbot_core
    PUBLIC nlohmann_json::nlohmann_json Threads::Threads
    PRIVATE "${OPUS_LIBRARY}" CURL::libcurl
)

add_executable(DiscordMusicBot
    src/main.cpp
    src/bot.cpp
    src/player.cpp
)
target_include_directories(DiscordMusicBot PRIVATE "${OPUS_INCLUDE_DIR}")
target_compile_features(DiscordMusicBot PRIVATE cxx_std_20)
target_link_libraries(DiscordMusicBot PRIVATE musicbot_core ${MUSICBOT_DPP_TARGET} "${OPUS_LIBRARY}")

if(MSVC)
    target_compile_options(musicbot_core PRIVATE /W4 /permissive- /utf-8)
    target_compile_options(DiscordMusicBot PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(musicbot_core PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(DiscordMusicBot PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(webm_opus_tests tests/webm_opus_tests.cpp)
    target_link_libraries(webm_opus_tests PRIVATE musicbot_core)
    add_test(NAME webm_opus_tests COMMAND webm_opus_tests)
endif()

set_target_properties(DiscordMusicBot PROPERTIES OUTPUT_NAME discord-music-bot)

install(TARGETS DiscordMusicBot RUNTIME DESTINATION bin)
install(FILES packaging/config.env DESTINATION /etc/discord-music-bot)
install(FILES packaging/discord-music-bot.service DESTINATION /lib/systemd/system)
install(FILES README.md DESTINATION share/doc/discord-music-bot)
install(FILES packaging/THIRD_PARTY_NOTICES.md DESTINATION share/doc/discord-music-bot)
if(DEFINED dpp_SOURCE_DIR AND EXISTS "${dpp_SOURCE_DIR}/LICENSE")
    install(FILES "${dpp_SOURCE_DIR}/LICENSE"
            DESTINATION share/doc/discord-music-bot RENAME LICENSE.DPP)
endif()
if(MUSICBOT_BUNDLED_YT_DLP)
    if(NOT EXISTS "${MUSICBOT_BUNDLED_YT_DLP}")
        message(FATAL_ERROR "MUSICBOT_BUNDLED_YT_DLP does not exist")
    endif()
    install(PROGRAMS "${MUSICBOT_BUNDLED_YT_DLP}" DESTINATION lib/discord-music-bot
            RENAME yt-dlp)
endif()
if(MUSICBOT_BUNDLED_DENO)
    if(NOT EXISTS "${MUSICBOT_BUNDLED_DENO}")
        message(FATAL_ERROR "MUSICBOT_BUNDLED_DENO does not exist")
    endif()
    install(PROGRAMS "${MUSICBOT_BUNDLED_DENO}" DESTINATION lib/discord-music-bot
            RENAME deno)
endif()

set(CPACK_GENERATOR DEB)
set(CPACK_PACKAGE_NAME discord-music-bot)
set(CPACK_PACKAGE_CONTACT "Discord Music Bot maintainers")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Native C++ Discord bot for YouTube audio")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_SET_DESTDIR ON)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SECTION sound)
set(CPACK_DEBIAN_PACKAGE_PRIORITY optional)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
if(MUSICBOT_BUNDLED_YT_DLP)
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "ca-certificates, adduser")
else()
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "ca-certificates, yt-dlp, adduser")
endif()
set(CPACK_DEBIAN_PACKAGE_CONTROL_STRICT_PERMISSION TRUE)
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/postinst"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/prerm"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/postrm"
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/conffiles"
)
set(CPACK_STRIP_FILES TRUE)
include(CPack)
