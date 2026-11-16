# Pinned source archive: no machine-installed loader or floating SDK dependency.
include(FetchContent)
set(BUILD_LOADER ON CACHE BOOL "" FORCE)
set(DYNAMIC_LOADER OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_CONFORMANCE_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SDK_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_WITH_SYSTEM_JSONCPP OFF CACHE BOOL "" FORCE)
FetchContent_Declare(openxr
  URL https://github.com/KhronosGroup/OpenXR-SDK/archive/refs/tags/release-1.1.53.zip
  URL_HASH SHA256=9c67c2d158f8b71a19d9d0b1e73043681cc4df6e90d207417a964b8d6aee1766
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(openxr)
