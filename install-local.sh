#!/usr/bin/env bash
# 编译并安装 BA Click FX，然后重建当前 KWin 中的 effect 实例。
# 用法：./install-local.sh [--user | --system] [--no-reload] [--help]
#
# 默认 --system，安装步骤会按需请求管理员权限；--user 安装到 ~/.local，主要用于
# test-nested.sh。请以当前用户运行脚本，不要为整个脚本加 sudo。

set -euo pipefail

usage() {
  cat <<'EOF'
用法：./install-local.sh [选项]
Usage: ./install-local.sh [options]

  --system       安装到 /usr（默认）
                 Install to /usr (default)
  --user         安装到 ~/.local
                 Install to ~/.local
  --no-reload    不重载当前 KWin 特效
                 Do not reload the current KWin effect
  -h, --help     显示帮助
                 Show help

JOBS=N 可指定编译并行数。
JOBS=N sets the number of build jobs.
EOF
}

die() {
  echo "安装失败：$*" >&2
  echo "Installation failed: $*" >&2
  exit 1
}

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    echo "未找到命令：$1" >&2
    echo "Missing command: $1" >&2
    exit 1
  fi
}

if [[ "${EUID}" -eq 0 ]]; then
  echo "请以普通用户运行；脚本会在安装时请求权限。" >&2
  echo "Run as your normal user; the script requests access when installing." >&2
  exit 2
fi

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Effect id 由插件文件名派生，同时用于配置键和 D-Bus 调用。
EFFECT_ID="kwin4_effect_ba_click_fx"

MODE="system"
RELOAD=1
for arg in "$@"; do
  case "${arg}" in
    --user) MODE="user" ;;
    --system) MODE="system" ;;
    --no-reload) RELOAD=0 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "未知参数：${arg}" >&2; echo "Unknown option: ${arg}" >&2; usage >&2; exit 2 ;;
  esac
done

require_command cmake
require_command strings
if [[ "${MODE}" == "system" ]]; then
  require_command sudo
fi

if [[ -n "${JOBS:-}" && ! "${JOBS}" =~ ^[1-9][0-9]*$ ]]; then
  echo "安装失败：JOBS 必须是正整数。" >&2
  echo "Installation failed: JOBS must be a positive integer." >&2
  exit 1
fi
if [[ -n "${JOBS:-}" ]]; then
  BUILD_JOBS="${JOBS}"
elif command -v nproc >/dev/null 2>&1; then
  BUILD_JOBS="$(nproc)"
else
  BUILD_JOBS=2
fi

# KWin native effect 的工厂 IID 包含 KWin 版本，不匹配时插件会被忽略。
check_abi() {
  local running built
  running="$(kwin_wayland --version 2>/dev/null | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1 || true)"
  built="$(strings "${BUILD_DIR}/bin/${EFFECT_ID}.so" 2>/dev/null \
           | grep -oE 'EffectPluginFactory[0-9]+\.[0-9]+\.[0-9]+' \
           | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1 || true)"

  if [[ -n "${running}" && -n "${built}" && "${running}" != "${built}" ]]; then
    echo "警告：插件是针对 KWin ${built} 编译的，当前运行的是 ${running}。" >&2
    echo "      请删除 ${BUILD_DIR} 后重新编译。" >&2
    echo "Warning: built for KWin ${built}, but the running version is ${running}." >&2
    echo "Remove ${BUILD_DIR} and rebuild." >&2
  fi
}

echo "==> 配置"
echo "==> Configure"
if [[ "${MODE}" == "user" ]]; then
  PREFIX="${HOME}/.local"
else
  PREFIX="/usr"
fi
# KDEInstallDirs 会把安装目录写入 CMake 缓存，因此两种模式使用独立构建目录。
BUILD_DIR="${SRC_DIR}/build/plugin-${MODE}"

# 旧版本曾用 root 创建构建文件。只清理当前模式的构建目录，保留其他构建目录。
if [[ -d "${BUILD_DIR}" ]] && find "${BUILD_DIR}" -uid 0 -print -quit 2>/dev/null | grep -q .; then
  echo "清理旧的管理员构建目录：${BUILD_DIR}" >&2
  echo "Removing old root-owned build: ${BUILD_DIR}" >&2
  require_command sudo
  sudo rm -rf -- "${BUILD_DIR}"
fi

if ! cmake -B "${BUILD_DIR}" -S "${SRC_DIR}" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=OFF \
  -DCMAKE_INSTALL_PREFIX="${PREFIX}"; then
  echo >&2
  echo "CMake 配置失败。请确认已安装 README.md 中列出的 Qt、KF6、ECM 和 KWin 开发包。" >&2
  echo "CMake configuration failed. Install the Qt, KF6, ECM and KWin development packages listed in README.en.md." >&2
  exit 1
fi

if [[ ! -s "${BUILD_DIR}/plugin-install-dir.txt" ]]; then
  echo "构建系统没有生成插件安装目录。" >&2
  echo "The build system did not generate the plugin install directory." >&2
  exit 1
fi
PLUGIN_DIR="$(<"${BUILD_DIR}/plugin-install-dir.txt")"

echo "==> 编译"
echo "==> Build"
cmake --build "${BUILD_DIR}" -j"${BUILD_JOBS}"

check_abi

echo "==> 安装到 ${PREFIX}"
echo "==> Install to ${PREFIX}"
if [[ "${MODE}" == "system" ]]; then
  sudo cmake --install "${BUILD_DIR}"
else
  cmake --install "${BUILD_DIR}"
  echo "提示：--user 安装的插件不在 Qt 默认插件路径中。"
  echo "Note: --user plugins are outside Qt's default plugin path."
  echo "启动 KWin 前请设置 QT_PLUGIN_PATH："
  echo "Export QT_PLUGIN_PATH before starting KWin:"
  echo "      export QT_PLUGIN_PATH=\"${PLUGIN_DIR}:\${QT_PLUGIN_PATH}\""
fi

EFFECT_PATH="${PLUGIN_DIR}/kwin/effects/plugins/${EFFECT_ID}.so"
CONFIG_PATH="${PLUGIN_DIR}/kwin/effects/configs/kwin_ba_click_fx_config.so"
if [[ ! -f "${EFFECT_PATH}" || ! -f "${CONFIG_PATH}" ]]; then
  echo "安装失败：安装完成但未在 ${PLUGIN_DIR} 找到模块。" >&2
  echo "Installation failed: installed modules not found under ${PLUGIN_DIR}." >&2
  exit 1
fi
echo "Effect: ${EFFECT_PATH}"
echo "Config: ${CONFIG_PATH}"

if [[ "${RELOAD}" -eq 0 ]]; then
  exit 0
fi

# qdbus 的可执行文件名因发行版而异。
QDBUS=""
for cmd in qdbus6 qdbus-qt6 qdbus; do
  if command -v "${cmd}" >/dev/null 2>&1; then
    QDBUS="${cmd}"
    break
  fi
done
if [[ -z "${QDBUS}" ]]; then
  echo "未找到 qdbus6 / qdbus-qt6 / qdbus，跳过重载。"
  echo "qdbus not found; effect reload skipped. Toggle the effect in System Settings."
  exit 0
fi

was_loaded="$("${QDBUS}" org.kde.KWin /Effects isEffectLoaded "${EFFECT_ID}" 2>/dev/null || true)"

echo "==> 重载特效"
echo "==> Reload effect"
# unload/load 只重建 Effect 对象；已映射的 native plugin 机器码仍会被进程缓存。
"${QDBUS}" org.kde.KWin /Effects unloadEffect "${EFFECT_ID}" >/dev/null 2>&1 || true
if "${QDBUS}" org.kde.KWin /Effects loadEffect "${EFFECT_ID}" 2>/dev/null | grep -q true; then
  echo "已创建特效实例：${EFFECT_ID}"
  echo "Effect instance created: ${EFFECT_ID}"
else
  echo "当前 KWin 尚未加载新插件，请注销并重新登录 Plasma。" >&2
  echo "The current KWin did not load the new plugin. Log out and back in, then enable BA Click FX." >&2
  exit 0
fi

runtime_status="$("${QDBUS}" org.kde.KWin /Effects debug "${EFFECT_ID}" status 2>/dev/null || true)"
if [[ -n "${runtime_status}" ]]; then
  echo
  echo "运行中实例：${runtime_status}"
  echo "Runtime status: ${runtime_status}"
fi

if [[ "${MODE}" == "system" && "${was_loaded}" == "true" ]]; then
  echo >&2
  echo "注意：安装前该 native effect 已经加载。" >&2
  echo "KWin 已经重新创建了对象，但 Qt 仍缓存了旧的映射 .so。" >&2
  echo "请重启 KWin Wayland 会话以使新二进制生效。" >&2
  echo
  echo "Note: the native effect was already loaded before installation." >&2
  echo "KWin recreated the object, but Qt still caches the old mapped .so." >&2
  echo "Restart the KWin Wayland session for the new binary to take effect." >&2

fi

echo
echo "日志和诊断请使用特效设置页。"
echo "命令行排查：README.md 和 TESTING.md"
echo "运行状态：${QDBUS} org.kde.KWin /Effects debug ${EFFECT_ID} status"
echo
echo "Use the effect settings page for logs and diagnostics."
echo "CLI troubleshooting: README.md and TESTING.md"
echo "Runtime status: ${QDBUS} org.kde.KWin /Effects debug ${EFFECT_ID} status"
