cmake_minimum_required(VERSION 3.15)
project(LlamaFlow)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- 1. ImGui & JSON Downloads ---
set(DEPS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/deps)
set(IMGUI_DIR ${DEPS_DIR}/imgui)
set(JSON_DIR ${DEPS_DIR}/json)

if(NOT EXISTS ${IMGUI_DIR}/imgui.h)
    message(STATUS "Downloading ImGui...")
    execute_process(COMMAND git clone --depth 1 https://github.com/ocornut/imgui.git ${IMGUI_DIR})
endif()

if(NOT EXISTS ${JSON_DIR}/json.hpp)
    message(STATUS "Downloading nlohmann/json...")
    file(MAKE_DIRECTORY ${JSON_DIR})
    file(DOWNLOAD https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp ${JSON_DIR}/json.hpp)
endif()

# --- 2. Fetch GLFW via CMake (No Homebrew required!) ---
include(FetchContent)
message(STATUS "Fetching GLFW from source...")

FetchContent_Declare(
    glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4  # Latest stable release
)

# Turn off GLFW tests and examples so it builds instantly
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(glfw)

# --- 3. Add source files ---
set(SOURCES
    main.cpp
    LlamaManager.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

if(APPLE)
    list(APPEND SOURCES mac_icon.mm)
    set_source_files_properties(mac_icon.mm PROPERTIES COMPILE_FLAGS "-x objective-c++")
endif()

add_executable(${PROJECT_NAME} ${SOURCES})

# --- 4. Include directories ---
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${JSON_DIR}
)

# --- 5. Link libraries ---
find_package(Threads REQUIRED)

if(APPLE)
    find_library(COCOA_FRAMEWORK Cocoa)
    find_library(OPENGL_FRAMEWORK OpenGL)
    find_library(IOKIT_FRAMEWORK IOKit)
    find_library(COREVIDEO_FRAMEWORK CoreVideo)
    
    # We now link to the target created by FetchContent, not a system library
    target_link_libraries(${PROJECT_NAME} PRIVATE 
        glfw 
        Threads::Threads
        ${COCOA_FRAMEWORK} 
        ${OPENGL_FRAMEWORK} 
        ${IOKIT_FRAMEWORK} 
        ${COREVIDEO_FRAMEWORK}
    )
else()
    target_link_libraries(${PROJECT_NAME} PRIVATE glfw Threads::Threads)
endif()