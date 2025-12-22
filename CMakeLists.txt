cmake_minimum_required(VERSION 3.24)
project("cuda-tcp-llama.cpp" LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CUDA_STANDARD 20)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(Threads REQUIRED)

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
  cc50_transport_tcp
  cc50_backend_toy cc50_backend_llama_server
  Threads::Threads
)

add_executable(cc50_llm_client src/client.cpp)
target_link_libraries(cc50_llm_client PRIVATE
  cc50_headers cc50_warnings
  cc50_transport_tcp
  Threads::Threads
)
