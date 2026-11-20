#!/usr/bin/env bash
# 默认安装到 /usr。请以普通用户运行，安装时按需授权。
set -euo pipefail

SRC_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "${SRC_DIR}/scripts/kwin-abi.sh"
EFFECT_ID="${BA_CLICK_FX_EFFECT_ID}"

usage() {
  ba_click_fx_message \
    '用法：./install-local.sh [--system | --user] [--no-reload]
  --system     安装到 /usr（默认）
  --user       安装到 ~/.local
  --no-reload  跳过特效重载
  -h, --help   显示帮助
设置 JOBS=N 可指定编译并行数。' \
    'Usage: ./install-local.sh [--system | --user] [--no-reload]
  --system     Install to /usr (default)
  --user       Install to ~/.local
  --no-reload  Skip reloading the effect
  -h, --help   Show help
Set JOBS=N to choose the number of build jobs.'
}

die() {
  ba_click_fx_message "安装失败：$1" "Installation failed: $2" >&2
  exit 1
}

require_command() {
  command -v "$1" >/dev/null || die "缺少命令 $1。" "Missing command $1."
}

MODE=system
RELOAD=1
REPAIR=0
for arg in "$@"; do
  case "${arg}" in
    --user) MODE=user ;;
    --system) MODE=system ;;
    --no-reload) RELOAD=0 ;;
    --repair) REPAIR=1 ;; # Invoked by scripts/rebuild-on-login.sh.
    -h|--help) usage; exit 0 ;;
    *) ba_click_fx_message "未知参数：${arg}" "Unknown option: ${arg}" >&2; usage >&2; exit 2 ;;
  esac
done

if (( EUID == 0 )); then
  ba_click_fx_message "请以普通用户运行，安装时会请求授权。" "Run as a regular user. Installation will request authorization." >&2
  exit 2
fi

require_command cmake
if (( REPAIR )); then
  [[ "${MODE}" == system ]] ||
    die "自动重建只支持系统安装。" "Automatic rebuild supports system source installs only."
  require_command flock
  require_command kdialog
  require_command pkexec
  RELOAD=0
else
  require_command strings
  if [[ "${MODE}" == system ]]; then
    require_command sudo
  fi
fi

if [[ -n "${JOBS:-}" && ! "${JOBS}" =~ ^[1-9][0-9]*$ ]]; then
  die "JOBS 必须是正整数。" "JOBS must be a positive integer."
fi
if [[ -n "${JOBS:-}" ]]; then
  BUILD_JOBS="${JOBS}"
elif command -v nproc >/dev/null; then
  BUILD_JOBS="$(nproc)"
else
  BUILD_JOBS=2
fi

# KWin requires the full version in the effect plugin's factory IID.
warn_abi_mismatch() {
  local installed built
  installed="$(ba_click_fx_kwin_version 2>/dev/null || true)"
  built="$(strings "${BUILD_DIR}/bin/${EFFECT_ID}.so" 2>/dev/null \
           | grep -oE 'EffectPluginFactory[0-9]+\.[0-9]+\.[0-9]+' \
           | grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1 || true)"

  if [[ -n "${installed}" && -n "${built}" && "${installed}" != "${built}" ]]; then
    ba_click_fx_message \
      "警告：插件按 KWin ${built} 构建，本机是 ${installed}。请安装匹配的开发包后重建。" \
      "Warning: plugin built for KWin ${built}; installed KWin is ${installed}. Use matching development files and rebuild." >&2
  fi
}

reload_effect() {
  local qdbus cmd was_loaded
  for cmd in qdbus6 qdbus-qt6 qdbus; do
    if command -v "${cmd}" >/dev/null; then
      qdbus="${cmd}"
      break
    fi
  done
  if [[ -z "${qdbus:-}" ]]; then
    ba_click_fx_message "找不到 qdbus。请在系统设置中启用特效。" "qdbus is unavailable. Enable the effect in System Settings."
    return 0
  fi

  was_loaded="$("${qdbus}" org.kde.KWin /Effects isEffectLoaded "${EFFECT_ID}" 2>/dev/null || true)"
  # Recreating the effect can reuse the binary already mapped in KWin.
  "${qdbus}" org.kde.KWin /Effects unloadEffect "${EFFECT_ID}" >/dev/null 2>&1 || true
  if ! "${qdbus}" org.kde.KWin /Effects loadEffect "${EFFECT_ID}" 2>/dev/null | grep -q true; then
    ba_click_fx_message "当前会话未加载特效。请重新登录 Plasma 后启用。" \
      "The effect did not load. Log out and back in to Plasma, then enable it." >&2
  elif [[ "${MODE}" == system && "${was_loaded}" == true ]]; then
    ba_click_fx_message "请重新登录 Plasma，让 KWin 加载新版本。" "Log out and back in to Plasma to load the new version."
  else
    ba_click_fx_message "特效已加载。" "Effect loaded."
  fi
}

PREFIX=/usr
if [[ "${MODE}" == user ]]; then
  PREFIX="${HOME}/.local"
fi
# KDE install paths are cached separately for the system and user prefixes.
BUILD_DIR="${SRC_DIR}/build/plugin-${MODE}"
if command -v flock >/dev/null && ! ba_click_fx_lock_build "${BUILD_DIR}"; then
  die "构建目录正被其他安装或卸载使用。" "Another install or uninstall is using this build directory."
fi

if (( REPAIR )); then
  original_plugin="$(ba_click_fx_installed_plugin "${BUILD_DIR}")" || exit 0
  installed_version="$(ba_click_fx_kwin_version)" ||
    die "无法读取 KWin 版本。" "Could not read the KWin version."
  old_version="$(ba_click_fx_plugin_version "${original_plugin}")" ||
    die "无法读取已安装插件的 IID。" "Could not read the installed plugin IID."
  [[ "${installed_version}" != "${old_version}" ]] || exit 0
  ba_click_fx_message "本机 KWin ${installed_version}，插件 IID ${old_version}。" \
    "Installed KWin ${installed_version}, plugin IID version ${old_version}."
fi

ba_click_fx_message "==> 配置" "==> Configure"

# Older installs could leave root-owned build files. The install manifest is safe to keep.
if (( ! REPAIR )) && [[ -d "${BUILD_DIR}" ]] &&
    find "${BUILD_DIR}" -uid 0 ! -path "${BUILD_DIR}/install_manifest.txt" -print -quit 2>/dev/null | grep -q .; then
  ba_click_fx_message "清理旧的 root 构建目录：${BUILD_DIR}" "Removing the old root-owned build: ${BUILD_DIR}" >&2
  require_command sudo
  sudo rm -rf -- "${BUILD_DIR}"
fi

if ! configure_output="$(cmake -B "${BUILD_DIR}" -S "${SRC_DIR}" \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=OFF \
  -DCMAKE_INSTALL_MESSAGE=NEVER \
  -DCMAKE_INSTALL_PREFIX="${PREFIX}")"; then
  printf '%s\n' "${configure_output}" >&2
  die "CMake 配置失败。请核对 README.md 中的构建依赖。" \
    "CMake configuration failed. Check the build dependencies in README.en.md."
fi

[[ -s "${BUILD_DIR}/plugin-install-dir.txt" ]] ||
  die "找不到插件安装目录记录。" "The plugin install directory is missing."
PLUGIN_DIR="$(<"${BUILD_DIR}/plugin-install-dir.txt")"
EFFECT_PATH="${PLUGIN_DIR}/kwin/effects/plugins/${EFFECT_ID}.so"
CONFIG_PATH="${PLUGIN_DIR}/kwin/effects/configs/kwin_ba_click_fx_config.so"
if [[ -s "${BUILD_DIR}/kwin-build-version.txt" ]]; then
  build_version="$(<"${BUILD_DIR}/kwin-build-version.txt")"
  build_id_pattern='BA Click FX build id: ([[:xdigit:]]+)'
  if [[ "${configure_output}" =~ ${build_id_pattern} ]]; then
    printf 'KWin %s | Build ID %s\n' "${build_version}" "${BASH_REMATCH[1]}"
  else
    printf 'KWin %s\n' "${build_version}"
  fi
fi

if (( REPAIR )); then
  [[ "${EFFECT_PATH}" == "${original_plugin}" ]] ||
    die "安装路径已变更，请手动安装。" "The plugin path changed. Run a manual install."
  [[ "${build_version:-}" == "${installed_version}" ]] ||
    die "KWin 开发包与已安装版本不匹配。" "KWin development files do not match the installed version."
fi

ba_click_fx_message "==> 编译" "==> Build"
cmake --build "${BUILD_DIR}" -j"${BUILD_JOBS}"

if (( REPAIR )); then
  built_version="$(ba_click_fx_plugin_version "${BUILD_DIR}/bin/${EFFECT_ID}.so")" ||
    die "无法读取构建产物的 IID。" "Could not read the rebuilt plugin IID."
  [[ "${built_version}" == "${installed_version}" ]] ||
    die "构建产物的 IID 与 KWin 不匹配。" "The rebuilt plugin IID does not match KWin."
  [[ -f "${BUILD_DIR}/bin/kwin_ba_click_fx_config.so" ]] ||
    die "构建产物缺少设置模块。" "The rebuilt settings module is missing."

  if kdialog --title 'BA Click FX' --yesno "$(ba_click_fx_message \
      "已为 KWin ${installed_version} 重建 BA Click FX。现在安装？" \
      "BA Click FX was rebuilt for KWin ${installed_version}. Install it now?")"; then
    :
  else
    status=$?
    [[ "${status}" == 1 ]] || die "无法显示安装确认。" "Could not show the installation prompt."
    ba_click_fx_message "安装已取消。" "Installation cancelled."
    exit 0
  fi
  [[ "$(ba_click_fx_kwin_version)" == "${installed_version}" ]] ||
    die "KWin 版本再次变化，请重新构建。" "KWin changed again. Rebuild before installing."
else
  warn_abi_mismatch
fi

ba_click_fx_message "==> 安装到 ${PREFIX}" "==> Install to ${PREFIX}"
if (( REPAIR )); then
  if pkexec --disable-internal-agent "$(command -v cmake)" --install "${BUILD_DIR}"; then
    :
  else
    status=$?
    [[ "${status}" == 126 ]] || exit "${status}"
    ba_click_fx_message "安装已取消。" "Installation cancelled."
    exit 0
  fi
elif [[ "${MODE}" == system ]]; then
  sudo cmake --install "${BUILD_DIR}"
else
  cmake --install "${BUILD_DIR}"
fi

[[ -f "${EFFECT_PATH}" && -f "${CONFIG_PATH}" ]] ||
  die "找不到已安装的模块。" "The installed modules could not be found."
if (( REPAIR )); then
  [[ "$(ba_click_fx_plugin_version "${EFFECT_PATH}")" == "${installed_version}" ]] ||
    die "安装后的 IID 校验失败。" "The installed plugin IID could not be verified."
fi
ba_click_fx_message "BA Click FX 安装完成。" "BA Click FX installed."

if (( REPAIR )); then
  kdialog --title 'BA Click FX' --msgbox "$(ba_click_fx_message \
    "安装完成，请重新登录 Plasma。" "Installed. Log out and back in to Plasma.")" || true
  exit 0
fi

if [[ "${MODE}" == user ]]; then
  ba_click_fx_message "启动 KWin 前请设置插件路径：" "Set the plugin path before starting KWin:"
  printf '  export QT_PLUGIN_PATH="%s:${QT_PLUGIN_PATH:-}"\n' "${PLUGIN_DIR}"
fi

if (( RELOAD )); then
  reload_effect
fi

if [[ "${MODE}" == system ]] && ! "${SRC_DIR}/scripts/rebuild-on-login.sh" --offer; then
  ba_click_fx_message "登录检查未能开启。" "Could not enable the login check." >&2
fi
