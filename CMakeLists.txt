cmake_minimum_required(VERSION 3.16)
project(undermine VERSION 1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  raylib
  GIT_REPOSITORY https://github.com/raysan5/raylib.git
  GIT_TAG        6.0
  GIT_SHALLOW    TRUE
)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(raylib)

add_executable(undermine
  src/main.c
  src/sim.c
  src/synth.c
  src/bot1.c
  src/bot2.c
  src/mole.c
  src/filedlg.c
)
target_link_libraries(undermine PRIVATE raylib)

if(WIN32)
  target_link_libraries(undermine PRIVATE comdlg32)   # native open-file dialog
endif()

if(MSVC)
  target_compile_options(undermine PRIVATE /W3)
endif()
