cmake_minimum_required(VERSION 3.24)
project(cc50_ucx_llama_server_bridge LANGUAGES CXX CUDA)

option(CC50_ENABLE_UCX "Enable UCX transport (requires ucx.pc via pkg-config)" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 20)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)
find_package(PkgConfig QUIET)

# ---- warnings ----
add_library(cc50_warnings INTERFACE)
target_compile_options(cc50_warnings INTERFACE
  $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>
  $<$<COMPILE_LANGUAGE:CUDA>:-Xcompiler=-Wall,-Wextra,-Wpedantic>
)

# ---- headers ----
add_library(cc50_headers INTERFACE)
target_include_directories(cc50_headers INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

# ---- transports ----
add_library(cc50_transport_tcp src/transport/tcp_transport.cpp)
target_link_libraries(cc50_transport_tcp PRIVATE cc50_headers cc50_warnings Threads::Threads)

add_library(cc50_transport_ucx src/transport/ucx_transport.cpp)
target_link_libraries(cc50_transport_ucx PRIVATE cc50_headers cc50_warnings Threads::Threads)

if (CC50_ENABLE_UCX)
  if (PkgConfig_FOUND)
    pkg_check_modules(UCX_PKG QUIET IMPORTED_TARGET ucx)
  endif()

  if (TARGET PkgConfig::UCX_PKG)
    target_compile_definitions(cc50_transport_ucx PRIVATE CC50_ENABLE_UCX=1)
    target_link_libraries(cc50_transport_ucx PRIVATE PkgConfig::UCX_PKG)
    message(STATUS "UCX enabled via pkg-config module: ucx")
  else()
    target_compile_definitions(cc50_transport_ucx PRIVATE CC50_ENABLE_UCX=0)
    message(WARNING
      "UCX requested but 'ucx' pkg-config module not found.\n"
      "If you installed UCX under /usr/local, try:\n"
      "  export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}\n"
      "  rm -rf build && cmake -S . -B build -G Ninja ...\n"
      "Building UCX transport as stub."
    )
  endif()
else()
  target_compile_definitions(cc50_transport_ucx PRIVATE CC50_ENABLE_UCX=0)
endif()

# ---- backends ----
add_library(cc50_backend_toy src/backend/toy_backend.cu)
target_link_libraries(cc50_backend_toy PRIVATE cc50_headers cc50_warnings)
# CUDA runtime
find_package(CUDAToolkit REQUIRED)
target_link_libraries(cc50_backend_toy PRIVATE CUDA::cudart)

add_library(cc50_backend_llama_server src/backend/llama_server_backend.cpp)
target_link_libraries(cc50_backend_llama_server PRIVATE cc50_headers cc50_warnings Threads::Threads)

# ---- executables ----
add_executable(cc50_llm_server src/server.cpp)
target_link_libraries(cc50_llm_server PRIVATE
  cc50_headers cc50_warnings
  cc50_transport_tcp cc50_transport_ucx
  cc50_backend_toy cc50_backend_llama_server
  Threads::Threads
)

add_executable(cc50_llm_client src/client.cpp)
target_link_libraries(cc50_llm_client PRIVATE
  cc50_headers cc50_warnings
  cc50_transport_tcp cc50_transport_ucx
  Threads::Threads
)
