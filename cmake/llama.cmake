include(FetchContent)

find_package(CUDAToolkit QUIET)
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

FetchContent_Declare(
    llama
    GIT_REPOSITORY https://github.com/ggml-org/llama.cpp.git
    GIT_TAG b10429
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(llama)
