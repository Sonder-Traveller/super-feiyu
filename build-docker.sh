#!/usr/bin/env bash
# 在 Ubuntu 24.04 容器里编译 super-feiyu，产出与服务器 ABI 兼容的二进制。
#
# 为什么需要这个：
#   开发机 WSL 是 Ubuntu 26.04（glibc 2.43 / OpenSSL 3.5），
#   服务器是 Ubuntu 24.04（glibc 2.39 / OpenSSL 3.0）。
#   在 26.04 上编出的二进制在 24.04 上会报
#   "GLIBC_2.43 not found" / "OPENSSL_3.2.0 not found"。
#   所以用 24.04 容器作为构建环境，产出 ABI 完全匹配的二进制。
#
# 依赖装在一个自建镜像里（只构建一次），后续编译秒级启动。
#
# 用法:
#   ./build-docker.sh            # 增量构建
#   ./build-docker.sh --clean    # 清空 build 后重建
#   ./build-docker.sh --rebuild-image  # 强制重建依赖镜像
#
# 产物: build-docker/super-feiyu
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SRC_DIR/build-docker"
IMAGE="super-feiyu-build:24.04"

case "${1:-}" in
  --clean) rm -rf "$BUILD_DIR" ;;
  --rebuild-image) docker rmi -f "$IMAGE" >/dev/null 2>&1 || true ;;
esac
mkdir -p "$BUILD_DIR"

# 依赖镜像：不存在就构建（约 2 分钟，之后复用）
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
  echo "==> 构建依赖镜像 $IMAGE（首次，约 2 分钟）"
  docker build -q -t "$IMAGE" -f "$SRC_DIR/deploy/Dockerfile.build" "$SRC_DIR"
fi

echo "==> 在容器内编译"

docker run --rm \
  -v "$SRC_DIR":/src \
  -v "$BUILD_DIR":/build \
  -w /src \
  "$IMAGE" \
  bash -c '
set -euo pipefail
CXX=clang++
INC="-Isrc -I/usr/local/include"
OUT=/build
mkdir -p "$OUT/pcm" "$OUT/obj"

MODULES=(core log config memory capability image ws qq_ws onebot_ws channels orchestrator)

echo "--- 1/4 预编译模块接口 ---"
FM=()
for m in "${MODULES[@]}"; do
  printf "    fish.%-14s" "$m"
  if "$CXX" -std=c++20 $INC "${FM[@]}" \
      --precompile "src/fish.$m.cppm" -o "$OUT/pcm/fish.$m.pcm" 2>"$OUT/pcm/fish.$m.err"; then
    echo "ok"
  else
    echo "FAILED"; head -25 "$OUT/pcm/fish.$m.err"; exit 1
  fi
  FM+=("-fmodule-file=fish.$m=$OUT/pcm/fish.$m.pcm")
done

echo "--- 2/4 生成模块目标代码 ---"
# 必须保留累加的 -fmodule-file：生成第 N 个模块时，前序模块都要能找到
FM=()
for m in "${MODULES[@]}"; do
  "$CXX" -std=c++20 $INC "${FM[@]}" -c "$OUT/pcm/fish.$m.pcm" -o "$OUT/obj/fish.$m.o"
  FM+=("-fmodule-file=fish.$m=$OUT/pcm/fish.$m.pcm")
done

echo "--- 3/4 编译主程序 ---"
"$CXX" -std=c++20 $INC "${FM[@]}" -c src/main.cpp -o "$OUT/obj/main.o"

echo "--- 4/4 静态链接 ---"
# 静态链自编的 libcurl（8.11.1，带 WebSocket）+ sqlite3，
# 让二进制在服务器上不依赖任何非标准库。
# 其余依赖来自 24.04 系统库，ABI 与服务器一致。
"$CXX" "$OUT/obj/main.o" "$OUT"/obj/fish.*.o -o "$OUT/super-feiyu" \
  -Wl,-Bstatic -L/usr/local/lib -lcurl -lsqlite3 -Wl,-Bdynamic \
  -lnghttp2 -lidn2 -lpsl -lssl -lcrypto -lgssapi_krb5 \
  -lzstd -lbrotlidec -lz \
  -lresolv -lpthread -ldl -lm

echo
ls -lh "$OUT/super-feiyu"
'

echo
echo "==> 产物: $BUILD_DIR/super-feiyu"
