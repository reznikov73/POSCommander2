cmake_minimum_required(VERSION 3.20)
# Версия продукта: меняется здесь и только здесь (попадает в exe, «О программе»
# и в подпись на главном окне). Номер сборки — из git, см. ниже.
project(PosFiles VERSION 1.0.0 LANGUAGES CXX)

# Номер сборки — количество коммитов в git; без git — 0.
set(POSFILES_BUILD 0)
find_package(Git QUIET)
if(Git_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-list --count HEAD
                  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                  OUTPUT_VARIABLE POSFILES_BUILD
                  OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET)
endif()
if(NOT POSFILES_BUILD MATCHES "^[0-9]+$")
  set(POSFILES_BUILD 0)
endif()
message(STATUS "PosFiles ${PROJECT_VERSION}.${POSFILES_BUILD}")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(PosFiles WIN32
  src/main.cpp
  src/panel.cpp
  src/util.cpp
  src/dialogs.cpp
  src/ops.cpp
  src/selftest.cpp
  src/settings.cpp
  src/search.cpp
  src/dupes.cpp
  src/rename.cpp
  src/undo.cpp
  src/diff.cpp
  src/prefs.cpp
  src/archive.cpp
  src/zip.cpp
  src/help.cpp
  src/sync.cpp
  src/ftp.cpp
  src/plugins.cpp
  src/wcx.cpp
  ${CMAKE_CURRENT_BINARY_DIR}/app.rc
)

# version.h — из шаблона (версия + номер сборки + реквизиты разработчика).
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/generated)
configure_file(src/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/version.h @ONLY)

# app.rc — из шаблона: VERSIONINFO с версией и путями к иконкам.
configure_file(res/app.rc.in ${CMAKE_CURRENT_BINARY_DIR}/app.rc @ONLY)

# Кириллица в VERSIONINFO: ресурсы в UTF-8.
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -c 65001")

target_include_directories(PosFiles PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated)

target_compile_definitions(PosFiles PRIVATE
  UNICODE _UNICODE
  WIN32_LEAN_AND_MEAN NOMINMAX
  _WIN32_WINNT=0x0A00 WINVER=0x0A00
)

target_link_libraries(PosFiles PRIVATE
  comctl32 shell32 shlwapi ole32 uuid mpr iphlpapi user32 gdi32 ws2_32
)

if(MSVC)
  target_compile_options(PosFiles PRIVATE /W4 /utf-8 /permissive-)
  target_link_options(PosFiles PRIVATE /MANIFEST:NO)
else()
  target_compile_options(PosFiles PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -finput-charset=UTF-8 -fexec-charset=UTF-8
  )
  target_link_options(PosFiles PRIVATE
    -municode -mwindows -static -static-libgcc -static-libstdc++
  )
endif()

# Ресурсы ссылаются на .ico/.manifest по имени — добавляем их в зависимости,
# иначе после перерисовки иконок ninja не пересобирает .rc.
set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/app.rc PROPERTIES OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/res/app.ico;${CMAKE_CURRENT_SOURCE_DIR}/res/folder.ico;${CMAKE_CURRENT_SOURCE_DIR}/res/folder_hidden.ico;${CMAKE_CURRENT_SOURCE_DIR}/res/arrow_up.ico;${CMAKE_CURRENT_SOURCE_DIR}/res/app.manifest;${CMAKE_CURRENT_SOURCE_DIR}/res/app.rc.in"
)

set_target_properties(PosFiles PROPERTIES OUTPUT_NAME "PosFiles")
