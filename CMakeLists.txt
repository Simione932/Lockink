cmake_minimum_required(VERSION 3.21)

include(${CMAKE_CURRENT_SOURCE_DIR}/SimpleBLE/cmake/prelude.cmake)

project(
    lockink_cli
    VERSION 0.1.0
    DESCRIPTION "LockInk (AA-A10xx) BLE command-line exerciser"
    LANGUAGES CXX
)

include(${CMAKE_CURRENT_SOURCE_DIR}/SimpleBLE/cmake/epilogue.cmake)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SIMPLEBLE_LOCAL "Use local SimpleBLE tree" ON)

if(SIMPLEBLE_LOCAL)
    # Apply local patches to the vendored SimpleBLE tree before configuring it.
    set(SIMPLEBLE_PATCHES
        ${CMAKE_CURRENT_SOURCE_DIR}/patches/simpleble-winrt-cpp20.patch)
    foreach(patch IN LISTS SIMPLEBLE_PATCHES)
        execute_process(
            COMMAND git apply --reverse --check ${patch}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/SimpleBLE
            RESULT_VARIABLE _patch_already_applied)
        if(NOT _patch_already_applied EQUAL 0)
            execute_process(
                COMMAND git apply ${patch}
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/SimpleBLE
                RESULT_VARIABLE _patch_result)
            if(NOT _patch_result EQUAL 0)
                message(FATAL_ERROR "Failed to apply patch ${patch} to SimpleBLE")
            endif()
        endif()
    endforeach()

    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/SimpleBLE/simpleble ${CMAKE_BINARY_DIR}/simpleble)
else()
    find_package(simpleble CONFIG REQUIRED)
endif()

add_executable(lockink-cli src/lockink.cpp)
add_executable(lockink-diag src/diag.cpp)
# simplecble links simpleble PRIVATE, so platform backends (macOS CoreBluetooth,
# Windows WinRT/setupapi, Linux dbus) must be pulled in via simpleble directly.
target_link_libraries(lockink-cli PRIVATE simpleble::simpleble)
target_link_libraries(lockink-diag PRIVATE simpleble::simpleble)

if(SIMPLEBLE_BACKEND_LINUX)
    target_link_libraries(lockink-cli PRIVATE ${DBus1_LIBRARIES})
endif()
