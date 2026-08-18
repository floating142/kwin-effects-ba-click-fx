#!/usr/bin/env bash
# 卸载 install-local.sh 安装的 BA Click FX 文件。
# 用法：./uninstall-local.sh [--user | --system] [--purge-config]
#
# 默认保留用户配置。--purge-config 额外删除效果设置和启用状态。

set -euo pipefail

if [[ "${EUID}" -eq 0 && -n "${SUDO_USER:-}" ]]; then
  echo "请不要用 sudo 跑整个脚本，直接执行即可：" >&2
  echo "  ./uninstall-local.sh ${*:-}" >&2
  exit 2
fi

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EFFECT_ID="kwin4_effect_ba_click_fx"
CONFIG_ID="kwin_ba_click_fx_config"
CONFIG_GROUP="Effect-ba-click-fx"

MODE="system"
PURGE_CONFIG=0
for arg in "$@"; do
  case "${arg}" in
    --user) MODE="user" ;;
    --system) MODE="system" ;;
    --purge-config) PURGE_CONFIG=1 ;;
    *) echo "未知参数: ${arg}" >&2; exit 2 ;;
  esac
done

BUILD_DIR="${SRC_DIR}/build/plugin-${MODE}"
MANIFEST="${BUILD_DIR}/install_manifest.txt"
PLUGIN_DIR_FILE="${BUILD_DIR}/plugin-install-dir.txt"
DATA_DIR_FILE="${BUILD_DIR}/data-install-dir.txt"

if [[ ! -f "${MANIFEST}" || ! -f "${PLUGIN_DIR_FILE}" || ! -f "${DATA_DIR_FILE}" ]]; then
  echo "找不到 ${MODE} 安装清单，请使用执行安装时保留的构建目录：" >&2
  echo "  ${BUILD_DIR}" >&2
  exit 1
fi

PLUGIN_DIR="$(cat "${PLUGIN_DIR_FILE}")"
DATA_DIR="$(cat "${DATA_DIR_FILE}")"
ASSET_ROOT="${DATA_DIR}/kwin/effects/ba-click-fx"

# 先从当前会话移除实例。插件机器码可能仍被 KWin 缓存，注销后会彻底释放。
QDBUS=""
for cmd in qdbus6 qdbus-qt6 qdbus; do
  if command -v "${cmd}" >/dev/null 2>&1; then
    QDBUS="${cmd}"
    break
  fi
done
if [[ -n "${QDBUS}" ]]; then
  "${QDBUS}" org.kde.KWin /Effects unloadEffect "${EFFECT_ID}" >/dev/null 2>&1 || true
fi

echo "==> 卸载 ${MODE} 安装"
if [[ "${MODE}" == "system" ]]; then
  sudo -v
fi

# 删除前先验证完整清单，避免异常清单导致部分卸载。
while IFS= read -r installed; do
  [[ -n "${installed}" ]] || continue
  case "${installed}" in
    "${PLUGIN_DIR}/kwin/effects/plugins/${EFFECT_ID}.so" | \
    "${PLUGIN_DIR}/kwin/effects/configs/${CONFIG_ID}.so" | \
    "${ASSET_ROOT}/"*) ;;
    *)
      echo "安装清单包含非预期路径，已停止：${installed}" >&2
      exit 1
      ;;
  esac
done < "${MANIFEST}"

while IFS= read -r installed; do
  [[ -n "${installed}" ]] || continue
  if [[ "${MODE}" == "system" ]]; then
    sudo cmake -E rm -f "${installed}"
  else
    cmake -E rm -f "${installed}"
  fi
done < "${MANIFEST}"

# 只删除已经为空的本项目目录，不影响同级的其他 KWin effect。
if [[ "${MODE}" == "system" ]]; then
  sudo rmdir "${ASSET_ROOT}/assets" "${ASSET_ROOT}/shader" "${ASSET_ROOT}" 2>/dev/null || true
else
  rmdir "${ASSET_ROOT}/assets" "${ASSET_ROOT}/shader" "${ASSET_ROOT}" 2>/dev/null || true
fi

if [[ "${PURGE_CONFIG}" -eq 1 ]]; then
  if command -v kwriteconfig6 >/dev/null 2>&1; then
    echo "==> 删除用户配置"
    kwriteconfig6 --file kwinrc --group Plugins --key "${EFFECT_ID}Enabled" --delete
    for key in TimeScale GlobalScale EnableTrail EnableDistanceEmitter LogLevel DebugDamage; do
      kwriteconfig6 --file kwinrc --group "${CONFIG_GROUP}" --key "${key}" --delete
    done
  else
    echo "未找到 kwriteconfig6，无法清理用户配置。" >&2
  fi
fi

echo "卸载完成。若该效果此前已加载，请注销 Plasma 会话以释放已映射的插件。"
