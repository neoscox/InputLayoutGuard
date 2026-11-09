cmake_minimum_required(VERSION 3.26)

set(PROJECT_AUTHOR "neoscox")
project(
  InputLayoutGuard
  VERSION 1.0.0
  LANGUAGES CXX)

if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
  message(FATAL_ERROR "In-source builds not allowed.")
endif()

# CommonLibSSE-NG is the git submodule in extern/CommonLibSSE-NG (or the path in the CommonLibSSE-NGPath environment variable).
list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(CLIBNGPlugin)
add_simple_commonlibsse_ng_plugin(AUTHOR "${PROJECT_AUTHOR}" LICENSE "GPL-3.0-or-later")

find_package(minhook CONFIG REQUIRED)
target_link_libraries(${PROJECT_NAME} PRIVATE minhook::minhook)
