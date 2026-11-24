#!/bin/bash
# Extra verification: ThreadSanitizer run + LD_PRELOAD smoke tests on real programs.
set -e
cd "$(dirname "$0")/.."
make -s build/libqalloc.so build/test_qalloc_tsan

echo "--- tsan"
./build/test_qalloc_tsan 2>&1 | tail -8

echo "--- LD_PRELOAD smoke tests"
export LD_PRELOAD=$PWD/build/libqalloc.so
ls /usr | head -3
python3 -c "import json,sys; print(len(json.dumps({str(i):[i]*5 for i in range(100000)})))"
g++ --version | head -1
echo 'int main(){return 0;}' | g++ -x c++ - -o /tmp/qa_smoke && /tmp/qa_smoke && echo "g++ compile ok"
