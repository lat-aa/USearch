#!/bin/sh
set -e
cmake --build /root/usearch-build --config Release --target api -j"$(nproc)"
cp -f /root/usearch-build/api /mnt/e/data/USearch/.local/api
chmod +x /mnt/e/data/USearch/.local/api
echo BUILD_OK
