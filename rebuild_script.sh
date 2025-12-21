#!/bin/bash
# Rebuild script for cc50-ucx-llama-server-bridge

set -e

# FIX: Use the current working directory instead of a hardcoded path
PROJECT_DIR=$(pwd)
cd "$PROJECT_DIR"

echo "=== UCX-LLAMA Bridge Rebuild ==="
echo "Building in: $PROJECT_DIR"

# PROJECT_DIR="/media/waqasm86/External1/Project-CPP/Project-Nvidia/ucx-llama-infer-accel-11"

cd "$PROJECT_DIR"

echo "=== UCX-LLAMA Bridge Rebuild ==="
echo

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

# Check UCX
echo -e "${YELLOW}[1] Checking UCX installation...${NC}"
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH

if ! pkg-config --exists ucx; then
    echo -e "${RED}ERROR: UCX not found${NC}"
    echo "Make sure UCX is installed and PKG_CONFIG_PATH is set"
    exit 1
fi

echo "UCX version: $(pkg-config --modversion ucx)"
echo "UCX libs: $(pkg-config --libs ucx)"
echo

# Clean build
echo -e "${YELLOW}[2] Cleaning old build...${NC}"
rm -rf build
echo

# Configure
echo -e "${YELLOW}[3] Configuring CMake...${NC}"
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_CUDA_ARCHITECTURES=50 \
  -DCC50_ENABLE_UCX=ON \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

echo

# Deep Clean
# echo -e "${YELLOW}[2] Cleaning old build...${NC}"
rm -rf build
mkdir -p build

# Configure with AddressSanitizer FORCED
# NOTE: We add -fno-omit-frame-pointer for better stack traces
echo -e "${YELLOW}[3] Configuring CMake...${NC}"
export PKG_CONFIG_PATH=/usr/local/lib/pkgconfig:$PKG_CONFIG_PATH
cmake -S . -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCC50_ENABLE_UCX=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"

# Build
#echo -e "${YELLOW}[4] Building...${NC}"
#cmake --build build --clean-first
cmake --build build


# Build
#echo -e "${YELLOW}[4] Building...${NC}"
#ninja -C build -v

echo

# Check binaries
if [ ! -f "build/cc50_llm_server" ]; then
    echo -e "${RED}ERROR: Server binary not created${NC}"
    exit 1
fi

if [ ! -f "build/cc50_llm_client" ]; then
    echo -e "${RED}ERROR: Client binary not created${NC}"
    exit 1
fi

echo -e "${GREEN}[5] Build successful!${NC}"
echo

# Set environment
echo -e "${YELLOW}[6] Setting environment variables...${NC}"
export LD_LIBRARY_PATH="/usr/local/lib:$LD_LIBRARY_PATH"
export UCX_TLS=tcp,self
export UCX_NET_DEVICES=lo
export UCX_LOG_LEVEL=info

echo "Environment:"
echo "  LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
echo "  UCX_TLS=$UCX_TLS"
echo "  UCX_NET_DEVICES=$UCX_NET_DEVICES"
echo "  UCX_LOG_LEVEL=$UCX_LOG_LEVEL"
echo

# Test server startup
echo -e "${YELLOW}[7] Testing server startup...${NC}"
echo "Command: ./build/cc50_llm_server --transport=ucx --backend=llama_server --listen=127.0.0.1:9199 --llama-url=http://127.0.0.1:8090 --llama-endpoint=/completion"
echo

# Run with timeout to see if it starts without crashing
timeout 3s ./build/cc50_llm_server \
  --transport=ucx \
  --backend=llama_server \
  --listen=127.0.0.1:9199 \
  --llama-url=http://127.0.0.1:8090 \
  --llama-endpoint=/completion || {
  EXIT_CODE=$?
  if [ $EXIT_CODE -eq 124 ]; then
    echo -e "${GREEN}Server started successfully (timed out after 3s as expected)${NC}"
  else
    echo -e "${RED}Server crashed with exit code $EXIT_CODE${NC}"
    exit 1
  fi
}

echo
echo -e "${GREEN}=== All checks passed! ===${NC}"
echo
echo "To run the server:"
echo "  export LD_LIBRARY_PATH=/usr/local/lib:\$LD_LIBRARY_PATH"
echo "  export UCX_TLS=tcp,self"
echo "  export UCX_NET_DEVICES=lo"
echo "  ./build/cc50_llm_server --transport=ucx --backend=llama_server --listen=127.0.0.1:9199 --llama-url=http://127.0.0.1:8090 --llama-endpoint=/completion"
echo
echo "In another terminal, run the client:"
echo "  export LD_LIBRARY_PATH=/usr/local/lib:\$LD_LIBRARY_PATH"
echo "  export UCX_TLS=tcp,self"
echo "  export UCX_NET_DEVICES=lo"
echo "  ./build/cc50_llm_client --transport=ucx --server=127.0.0.1:9199 --prompt \"Hello\" --max-tokens 50 --iters 1 --print 1"
