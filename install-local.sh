#!/usr/bin/env bash
# 编译并安装 BA Click FX，然后重建当前 KWin 中的 effect 实例。
# 用法：./install-local.sh [--user | --system] [--no-reload]
#
# 默认 --system，安装步骤会按需调用 sudo。--user 安装到 ~/.local，主要用于
# test-nested.sh；普通 KWin 会话默认不会从该目录加载 native effect。
#
# 不要用 sudo 启动整个脚本，否则后续 D-Bus 调用无法连接当前用户的 KWin。

set -euo pipefail

if [[ "${EUID}" -eq 0 && -n "${SUDO_USER:-}" ]]; then
  echo "请不要用 sudo 跑整个脚本，直接执行即可：" >&2
  echo "  ./install-local.sh ${*:-}" >&2
  echo >&2
  echo "只有安装到 /usr 那一步需要 root，脚本内部会自行调用 sudo。" >&2
  echo "整脚本提权会让 D-Bus 连错会话总线，导致「加载失败」。" >&2
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
    *) echo "未知参数: ${arg}" >&2; exit 2 ;;
  esac
done

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
  fi
}

echo "==> 配置"
if [[ "${MODE}" == "user" ]]; then
  PREFIX="${HOME}/.local"
else
  PREFIX="/usr"
fi
# KDEInstallDirs 会把安装目录写入 CMake 缓存，因此两种模式使用独立构建目录。
BUILD_DIR="${SRC_DIR}/build/plugin-${MODE}"

# 拒绝复用含 root 文件的构建目录，避免 CMake 给出不直观的写入错误。
if [[ -d "${BUILD_DIR}" ]] && ! find "${BUILD_DIR}" -uid 0 -print -quit 2>/dev/null | grep -q .; then
  : # 目录干净，继续
elif [[ -d "${BUILD_DIR}" ]]; then
  echo "${BUILD_DIR} 里有 root 所属的文件，普通用户无法写入。" >&2
  echo "这是之前 sudo 整脚本留下的，清掉再跑：" >&2
  echo >&2
  echo "  sudo rm -rf ${SRC_DIR}/build" >&2
  exit 2
fi

cmake -B "${BUILD_DIR}" -S "${SRC_DIR}" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX="${PREFIX}" >/dev/null

PLUGIN_DIR="$(cat "${BUILD_DIR}/plugin-install-dir.txt")"

echo "==> 编译"
cmake --build "${BUILD_DIR}" -j"$(nproc)"

check_abi

echo "==> 安装到 ${PREFIX}"
if [[ "${MODE}" == "system" ]]; then
  sudo cmake --install "${BUILD_DIR}"
else
  cmake --install "${BUILD_DIR}"
  echo "提示：--user 安装的插件不在 Qt 默认插件路径里。"
  echo "      启动 KWin 前需要 export QT_PLUGIN_PATH=\"${PLUGIN_DIR}:\${QT_PLUGIN_PATH}\""
fi

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
  echo "未找到 qdbus6 / qdbus-qt6 / qdbus，跳过 effect 实例重建。"
  echo "请手动在「系统设置 → 桌面特效」里开关一次。"
  exit 0
fi

was_loaded="$("${QDBUS}" org.kde.KWin /Effects isEffectLoaded "${EFFECT_ID}" 2>/dev/null || true)"

echo "==> 重建 effect 实例"
# unload/load 只重建 Effect 对象；已映射的 native plugin 机器码仍会被进程缓存。
"${QDBUS}" org.kde.KWin /Effects unloadEffect "${EFFECT_ID}" >/dev/null 2>&1 || true
if "${QDBUS}" org.kde.KWin /Effects loadEffect "${EFFECT_ID}" 2>/dev/null | grep -q true; then
  echo "已创建 ${EFFECT_ID} effect 实例。"
else
  echo "加载失败。若是首次安装，KWin 可能需要重启才能发现新插件：" >&2
  echo "  kwin_wayland --replace &" >&2
  exit 1
fi

runtime_status="$("${QDBUS}" org.kde.KWin /Effects debug "${EFFECT_ID}" status 2>/dev/null || true)"
if [[ -n "${runtime_status}" ]]; then
  echo "运行中实例: ${runtime_status}"
fi

if [[ "${MODE}" == "system" && "${was_loaded}" == "true" ]]; then
  echo >&2
  echo "注意：安装前该 native effect 已加载。KWin 只重建了对象，Qt 仍复用进程中" >&2
  echo "      已映射的旧 .so；本次新机器码要在 KWin Wayland 会话重新启动后生效。" >&2
  echo "      reconfigureEffect 也只重读配置，不能重新加载二进制。开发迭代请使用：" >&2
  echo "        ./test-nested.sh --profile" >&2
fi

echo
echo "日志与诊断请在特效设置页操作。命令行故障排查见 README.md 和 TESTING.md。"
echo "运行状态： ${QDBUS} org.kde.KWin /Effects debug ${EFFECT_ID} status"
