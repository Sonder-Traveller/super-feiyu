#!/usr/bin/env bash
# 重试构建：clang 21 在序列化 std::wstring 时偶发崩溃（channels 模块），
# 同一份源码重跑即可通过。失败就清掉该模块的 pcm 再试。
cd ~/super-feiyu || exit 1
for i in 1 2 3 4 5; do
  rm -f build/pcm/fish.channels.pcm
  if ./build.sh > /tmp/build_retry.log 2>&1; then
    echo "attempt $i: OK"
    tail -10 /tmp/build_retry.log
    exit 0
  fi
  echo "attempt $i: failed -> $(grep -a 'FAILED' /tmp/build_retry.log | head -2 | tr '\n' ' ')"
done
echo "all attempts failed"
tail -20 /tmp/build_retry.log
exit 1
