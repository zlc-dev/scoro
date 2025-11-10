cmake_minimum_required(VERSION 3.25.0)

project(COROUTINE CXX C)

file(GLOB_RECURSE SOURCE "${PROJECT_SOURCE_DIR}/src/*.cpp" "${PROJECT_SOURCE_DIR}/src/*.cc" "${PROJECT_SOURCE_DIR}/src/*.c")
file(GLOB_RECURSE HEADER "${PROJECT_SOURCE_DIR}/include/*.hpp" "${PROJECT_SOURCE_DIR}/include/*.h")
list(REMOVE_ITEM SOURCE "${PROJECT_SOURCE_DIR}/src/main.cpp")

add_executable(coroutine "${PROJECT_SOURCE_DIR}/src/main.cpp" ${SOURCE} ${HEADER})
target_include_directories(coroutine PRIVATE "${PROJECT_SOURCE_DIR}/include")

enable_testing()

if(CMAKE_SYSTEM_NAME MATCHES "Linux")
    add_executable(io_uring "${PROJECT_SOURCE_DIR}/test/test_io_uring.cpp" ${SOURCE} ${HEADER})
    target_include_directories(io_uring PRIVATE "${PROJECT_SOURCE_DIR}/include")
    add_test(NAME test_io_uring COMMAND $<TARGET_FILE:io_uring> "${PROJECT_SOURCE_DIR}/tmp/test_io_uring")
endif(CMAKE_SYSTEM_NAME MATCHES "Linux")

list(APPEND IN_FILES "${CMAKE_SOURCE_DIR}/.clangd.in")

foreach(IN_FILE ${IN_FILES})
    get_filename_component(OUTPUT_FILE_NAME ${IN_FILE} NAME_WLE)
    get_filename_component(OUTPUT_FILE_PATH ${IN_FILE} DIRECTORY)
    set(OUTPUT_FILE ${OUTPUT_FILE_PATH}/${OUTPUT_FILE_NAME})
    message(${OUTPUT_FILE})
    configure_file(
        ${IN_FILE}
        ${OUTPUT_FILE}
    )
endforeach(IN_FILE)
