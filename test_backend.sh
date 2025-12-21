#!/bin/bash
cd /media/waqasm86/External1/Project-CPP/Project-Nvidia/ucx-llama-infer-accel-10

# Add debug output to see what's happening
export LLAMA_SERVER_URL="http://127.0.0.1:8090"
export LLAMA_SERVER_ENDPOINT="/completion"

echo "Testing llama_server backend with debug..."

# Run with strace to see system calls
strace -e trace=network,read,write -o /tmp/backend_trace.log \
  timeout 30s ./build/cc50_llm_server \
    --transport=tcp \
    --backend=llama_server \
    --listen=127.0.0.1:9199 \
    --llama-url=http://127.0.0.1:8090 \
    --llama-endpoint=/completion &

SERVER_PID=$!
sleep 2

# Send a request
echo "Sending client request..."
timeout 30s ./build/cc50_llm_client \
  --transport=tcp \
  --server=127.0.0.1:9199 \
  --prompt "Hi" \
  --max-tokens 10 \
  --iters 1 \
  --print 1

echo "Checking trace log..."
tail -50 /tmp/backend_trace.log

kill $SERVER_PID 2>/dev/null
