cmake_minimum_required(VERSION 3.20)
project(POSCommander2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(POSCommander2 WIN32
  src/main.cpp
  src/panel.cpp
  src/util.cpp
  src/dialogs.cpp
  src/ops.cpp
  src/selftest.cpp
  src/settings.cpp
  res/app.rc
)

target_include_directories(POSCommander2 PRIVATE src)

target_compile_definitions(POSCommander2 PRIVATE
  UNICODE _UNICODE
  WIN32_LEAN_AND_MEAN NOMINMAX
  _WIN32_WINNT=0x0A00 WINVER=0x0A00
)

target_link_libraries(POSCommander2 PRIVATE
  comctl32 shell32 shlwapi ole32 uuid user32 gdi32
)

if(MSVC)
  target_compile_options(POSCommander2 PRIVATE /W4 /utf-8 /permissive-)
  target_link_options(POSCommander2 PRIVATE /MANIFEST:NO)
else()
  target_compile_options(POSCommander2 PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    -finput-charset=UTF-8 -fexec-charset=UTF-8
  )
  target_link_options(POSCommander2 PRIVATE
    -municode -mwindows -static -static-libgcc -static-libstdc++
  )
endif()

set_target_properties(POSCommander2 PROPERTIES OUTPUT_NAME "POSCommander2")
