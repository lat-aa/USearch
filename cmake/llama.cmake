include(FetchContent)

# WSL PATH often exposes Windows nvcc.exe; that cannot compile the Linux api.
find_program(_usearch_nvcc nvcc)
if (_usearch_nvcc MATCHES "\\.exe$")
    message(STATUS "llama.cpp: ignore Windows nvcc.exe (${_usearch_nvcc})")
else ()
    find_package(CUDAToolkit QUIET)
endif ()
if (CUDAToolkit_FOUND)
    set(GGML_CUDA
        ON
        CACHE BOOL "" FORCE
    )
    message(STATUS "llama.cpp: GGML_CUDA=ON (CUDAToolkit found)")
else ()
    set(GGML_CUDA
        OFF
        CACHE BOOL "" FORCE
    )
    message(STATUS "llama.cpp: GGML_CUDA=OFF (CPU)")
endif ()

set(LLAMA_BUILD_COMMON
    OFF
    CACHE BOOL "" FORCE
)
set(LLAMA_BUILD_TESTS
    OFF
    CACHE BOOL "" FORCE
)
set(LLAMA_BUILD_EXAMPLES
    OFF
    CACHE BOOL "" FORCE
)
set(LLAMA_BUILD_SERVER
    OFF
    CACHE BOOL "" FORCE
)
set(LLAMA_BUILD_TOOLS
    OFF
    CACHE BOOL "" FORCE
)
set(BUILD_SHARED_LIBS
    OFF
    CACHE BOOL "" FORCE
)
set(GGML_NATIVE
    OFF
    CACHE BOOL "" FORCE
)

# Prefer an unpacked tree (Linux FS). Git clone of llama.cpp is the slow path.
if(DEFINED ENV{LLAMA_SRC} AND EXISTS "$ENV{LLAMA_SRC}/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_LLAMA "$ENV{LLAMA_SRC}")
elseif(NOT FETCHCONTENT_SOURCE_DIR_LLAMA AND EXISTS "${CMAKE_BINARY_DIR}/_deps/llama-src/CMakeLists.txt")
    set(FETCHCONTENT_SOURCE_DIR_LLAMA "${CMAKE_BINARY_DIR}/_deps/llama-src")
endif()
if(FETCHCONTENT_SOURCE_DIR_LLAMA)
    message(STATUS "llama.cpp: SOURCE_DIR ${FETCHCONTENT_SOURCE_DIR_LLAMA} (skip git clone)")
endif()

FetchContent_Declare(
    llama
    GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
    GIT_TAG b10429
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(llama)
