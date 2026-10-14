cmake_minimum_required(VERSION 3.20)

project(ds_player VERSION 0.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# melonDS core only: no Qt frontend, no GDB stub, software 3D.
set(BUILD_QT_SDL OFF CACHE BOOL "" FORCE)
set(ENABLE_GDBSTUB OFF CACHE BOOL "" FORCE)
set(ENABLE_OGLRENDERER OFF CACHE BOOL "" FORCE)
if(EMSCRIPTEN)
  set(ENABLE_LTO_RELEASE OFF CACHE BOOL "" FORCE)
endif()
add_subdirectory(thirdparty/melonDS EXCLUDE_FROM_ALL)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(thirdparty/SDL EXCLUDE_FROM_ALL)

add_executable(ds-player
  src/app.cpp
  src/platform.cpp
  src/save_file.cpp
)
target_link_libraries(ds-player PRIVATE core SDL3::SDL3)

if(MINGW)
  target_link_options(ds-player PRIVATE -static -mwindows)
endif()

if(EMSCRIPTEN)
  set_target_properties(ds-player PROPERTIES OUTPUT_NAME ds-player SUFFIX ".js")
  target_link_options(ds-player PRIVATE
    "-sMODULARIZE=1"
    "-sEXPORT_NAME=DSPlayerModule"
    "-sEXPORTED_RUNTIME_METHODS=[FS,callMain]"
    "-sEXPORTED_FUNCTIONS=[_main,_ds_player_flush_save,_ds_player_reload]"
    "-sINVOKE_RUN=0"
    "-sFORCE_FILESYSTEM=1"
    "-sALLOW_MEMORY_GROWTH=1"
    "-sINITIAL_MEMORY=268435456"
    "-sSTACK_SIZE=1048576"
    "-sEXIT_RUNTIME=0"
    "-sENVIRONMENT=web"
    "-lidbfs.js"
  )

  # The page and the module land together in <build>/site.
  set(web_dir "${CMAKE_BINARY_DIR}/site")
  add_custom_command(TARGET ds-player POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${web_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_SOURCE_DIR}/web" "${web_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:ds-player>" "$<TARGET_FILE_DIR:ds-player>/ds-player.wasm" "${web_dir}"
    COMMENT "Assembling the web page in ${web_dir}"
  )
endif()
