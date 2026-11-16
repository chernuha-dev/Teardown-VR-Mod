include(FetchContent)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(minhook
  URL https://github.com/TsudaKageyu/minhook/archive/refs/tags/v1.3.4.zip
  URL_HASH SHA256=172708123daa0c98d20d3a980b16a50be14af243dc95dee6f79c24193ad010e4
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(minhook)
