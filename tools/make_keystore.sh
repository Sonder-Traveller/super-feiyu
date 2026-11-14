#!/usr/bin/env bash
# 生成自签名 keystore（用于发布 APK）
#
# 密码写到本地文件，不进 git（.gitignore 已排除）
set -euo pipefail

KEYDIR="$HOME/super-feiyu/keys"
mkdir -p "$KEYDIR"
chmod 700 "$KEYDIR"

KS="$KEYDIR/release.keystore"
PROPS="$KEYDIR/keystore.properties"

if [ -f "$KS" ]; then
  echo "keystore 已存在: $KS"
  echo "（要重新生成请先删除它）"
  exit 0
fi

# 生成随机密码：32 位，只含安全字符
PASS=$(head -c 48 /dev/urandom | base64 | tr -d '/+=\n' | head -c 32)

echo "=== 生成 keystore"
keytool -genkeypair \
  -keystore "$KS" \
  -alias super-feiyu \
  -keyalg RSA \
  -keysize 4096 \
  -validity 10000 \
  -storepass "$PASS" \
  -keypass "$PASS" \
  -dname "CN=Sonder-Traveller, OU=super-feiyu, O=Sonder, L=Unknown, ST=Unknown, C=CN" \
  2>&1 | grep -v "^$" | head -5

echo
echo "=== 写入 keystore.properties"
cat > "$PROPS" <<EOF
# APK 签名配置（本地文件，不要提交到 git）
storeFile=$KS
storePassword=$PASS
keyAlias=super-feiyu
keyPassword=$PASS
EOF
chmod 600 "$PROPS"
chmod 600 "$KS"

echo "  $PROPS (600)"
echo "  $KS (600)"

echo
echo "=== 验证 keystore"
keytool -list -keystore "$KS" -storepass "$PASS" 2>&1 | grep -E 'Keystore type|Your keystore|super-feiyu' | head -3

echo
echo "=== 文件"
ls -la "$KEYDIR" | sed 's/^/  /'
