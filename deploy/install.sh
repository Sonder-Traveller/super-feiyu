#!/usr/bin/env bash
# 生产部署脚本：从源码到 systemd 服务。
#
# 用法：
#   ./deploy/install.sh              # 装到 ~/super-feiyu 并注册 systemd 用户服务
#   ./deploy/install.sh --no-service # 只构建部署，不装服务
#
# 前置条件（脚本会检查）：
#   - clang++ 16+（需 C++20 模块支持）
#   - libcurl 8.10+、sqlite3、nlohmann/json 的开发文件
#   - Python 3.9+（仅桥接层需要，可选）
set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
INSTALL_DIR="${INSTALL_DIR:-$HOME/super-feiyu}"
PREFIX="${PREFIX:-$HOME/local}"
WITH_SERVICE=1

for arg in "$@"; do
  case "$arg" in
    --no-service) WITH_SERVICE=0 ;;
    *) echo "未知参数: $arg" >&2; exit 2 ;;
  esac
done

say() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[!]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[x]\033[0m %s\n' "$*" >&2; exit 1; }

# ── 1. 环境检查 ─────────────────────────────────────────
say "检查依赖"
command -v clang++ >/dev/null || die "缺少 clang++（需要 16+ 以支持 C++20 模块）"

CLANG_MAJOR=$(clang++ -dM -E -x c++ /dev/null 2>/dev/null | awk '/__clang_major__/{print $3}')
if [ -n "$CLANG_MAJOR" ] && [ "$CLANG_MAJOR" -lt 16 ]; then
  die "clang++ 版本过低（$CLANG_MAJOR），需要 16+"
fi

missing=()
[ -f "$PREFIX/include/nlohmann/json.hpp" ] || \
  [ -f "/usr/include/nlohmann/json.hpp" ] || missing+=("nlohmann/json.hpp")
[ -f "$PREFIX/include/sqlite3.h" ] || [ -f "/usr/include/sqlite3.h" ] || missing+=("sqlite3.h")
{ [ -f "$PREFIX/include/x86_64-linux-gnu/curl/curl.h" ] || \
  [ -f "$PREFIX/include/curl/curl.h" ] || \
  [ -f "/usr/include/curl/curl.h" ]; } || missing+=("curl/curl.h")

if [ ${#missing[@]} -gt 0 ]; then
  warn "缺少依赖: ${missing[*]}"
  cat <<EOF
安装方式（二选一）：

A) 有 root：
   sudo apt install libcurl4-openssl-dev libsqlite3-dev nlohmann-json3-dev

B) 无 root（装到 $PREFIX）：
   mkdir -p $PREFIX && cd /tmp
   apt-get download libcurl4-openssl-dev libcurl4t64 libsqlite3-dev libsqlite3-0
   for f in *.deb; do dpkg -x "\$f" $PREFIX; done
   mkdir -p $PREFIX/include/nlohmann
   curl -fsSL https://raw.githubusercontent.com/nlohmann/json/v3.11.3/single_include/nlohmann/json.hpp \\
     -o $PREFIX/include/nlohmann/json.hpp
EOF
  die "依赖不全，先装好再跑"
fi

# ── 2. 构建 ─────────────────────────────────────────────
say "构建（clang C++20 模块）"
cd "$SRC_DIR"
PREFIX="$PREFIX" ./build.sh 2>&1 | tail -8

# ── 3. 部署到 install dir ───────────────────────────────
say "部署到 $INSTALL_DIR"
mkdir -p "$INSTALL_DIR"
rsync -a --delete \
  --exclude '.git/' --exclude 'build/' --exclude 'build_obj/' --exclude 'build_pcm/' \
  --exclude '*.o' --exclude '*.pcm' --exclude 'data/' --exclude '.env.local' \
  "$SRC_DIR"/ "$INSTALL_DIR"/

cd "$INSTALL_DIR"

# 配置模板：首次部署时生成
if [ ! -f config.toml ]; then
  cp config.example.toml config.toml
  say "已生成 config.toml，请填入通道与密钥"
fi

# 环境变量文件
if [ ! -f .env ]; then
  cat > .env <<'EOF'
# 密钥与凭据。不要提交到版本控制。
# DEEPSEEK_API_KEY=sk-xxx
# SILICONFLOW_API_KEY=sk-xxx
# QQ_APP_ID=
# QQ_APP_SECRET=
# BOT_QQ=
# ONEBOT_TOKEN=
EOF
  chmod 600 .env
  say "已生成 .env 模板（权限 600）"
fi

mkdir -p data/memory/notes
say "重新构建（部署目录内）"
PREFIX="$PREFIX" ./build.sh 2>&1 | tail -5

# ── 4. 安装 systemd 用户服务 ────────────────────────────
if [ "$WITH_SERVICE" -eq 1 ]; then
  say "安装 systemd 用户服务"
  if ! command -v systemctl >/dev/null; then
    warn "没有 systemctl，跳过服务安装"
  else
    UNIT_DIR="$HOME/.config/systemd/user"
    mkdir -p "$UNIT_DIR"
    # 用模板实例：%i 替换成当前用户名
    sed "s/%i/$(whoami)/g" deploy/super-feiyu@.service > "$UNIT_DIR/super-feiyu.service"
    systemctl --user daemon-reload || true
    systemctl --user enable super-feiyu.service || true

    # 让用户服务在未登录时也运行（需要一次 sudo）
    if command -v loginctl >/dev/null; then
      say "启用 linger（让服务在登出后继续运行）"
      loginctl enable-linger "$(whoami)" 2>/dev/null || \
        warn "无法启用 linger，可能需要手动执行: sudo loginctl enable-linger $(whoami)"
    fi
  fi
fi

# ── 5. 自检 ─────────────────────────────────────────────
say "自检"
LD_LIBRARY_PATH="$PREFIX/lib" ./build/super-feiyu --self-test 2>&1 | tail -8

cat <<EOF

\033[1;32m部署完成\033[0m

  目录:     $INSTALL_DIR
  二进制:   $INSTALL_DIR/build/super-feiyu
  配置:     $INSTALL_DIR/config.toml
  密钥:     $INSTALL_DIR/.env

下一步：
  1. 编辑 $INSTALL_DIR/config.toml，至少启用一个通道
  2. 编辑 $INSTALL_DIR/.env，填入密钥
  3. 启动：
       systemctl --user start super-feiyu
       journalctl --user -u super-feiyu -f
     或前台运行排查：
       cd $INSTALL_DIR && set -a && . ./.env && set +a && \\
       LD_LIBRARY_PATH=$PREFIX/lib ./build/super-feiyu --config config.toml
EOF
