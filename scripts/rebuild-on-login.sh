#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later

set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "${script_dir}/kwin-abi.sh"
source_dir="${script_dir%/*}"
build_dir="${source_dir}/build/plugin-system"
entry="${XDG_CONFIG_HOME:-${HOME}/.config}/autostart/org.kde.ba-click-fx.rebuild.desktop"

check() {
  local plugin installed_version plugin_version
  if ! plugin="$(ba_click_fx_installed_plugin "${build_dir}")"; then
    echo "找不到此源码目录的系统安装 / No system installation recorded for this checkout." >&2
    return 2
  fi
  if ! installed_version="$(ba_click_fx_kwin_version)" ||
      ! plugin_version="$(ba_click_fx_plugin_version "${plugin}")"; then
    echo "无法读取 KWin 或插件版本。请确认已安装 Qt 6 的 qtplugininfo。" >&2
    echo "Could not read the KWin or plugin version. Make sure Qt 6 qtplugininfo is installed." >&2
    return 2
  fi
  printf 'KWin: %s\nPlugin IID version: %s\nPlugin: %s\n' "${installed_version}" "${plugin_version}" "${plugin}"
  [[ "${installed_version}" == "${plugin_version}" ]]
}

enable() {
  local status=0 path temp_entry
  check || status=$?
  (( status < 2 )) || return "${status}"
  for tool in flock kdialog pkexec; do
    if ! command -v "${tool}" >/dev/null; then
      echo "缺少命令 / Missing command: ${tool}" >&2
      return 2
    fi
  done

  path="${script_dir}/rebuild-on-login.sh"
  if [[ "${path}" == *$'\n'* || "${path}" == *$'\r'* || "${path}" == *$'\t'* ]]; then
    echo "源码路径不能包含控制字符 / The checkout path must not contain control characters." >&2
    return 2
  fi
  # Exec uses both desktop-entry string escaping and command-line quoting.
  path="${path//\\/\\\\\\\\}"
  path="${path//\"/\\\\\"}"
  path="${path//\$/\\\\\$}"
  path="${path//\`/\\\\\`}"
  path="${path//%/%%}"

  mkdir -p -- "${entry%/*}"
  temp_entry="$(mktemp "${entry}.XXXXXX")"
  if cat >"${temp_entry}" <<DESKTOP
[Desktop Entry]
Type=Application
Name=BA Click FX KWin compatibility check
Name[zh_CN]=BA Click FX KWin 兼容性检查
Exec=bash "${path}" --run
OnlyShowIn=KDE;
Terminal=false
DESKTOP
  then
    if mv -f -- "${temp_entry}" "${entry}"; then
      echo "已启用登录检查 / Enabled the login check: ${entry}"
      return 0
    fi
  fi
  rm -f -- "${temp_entry}"
  return 1
}

run() {
  local status=0 log_dir log
  # If the user has uninstalled the effect, leave it uninstalled.
  ba_click_fx_installed_plugin "${build_dir}" >/dev/null || return 0
  check >/dev/null || status=$?
  if (( status == 0 )); then
    return 0
  elif (( status != 1 )); then
    return "${status}"
  fi

  log_dir="${XDG_STATE_HOME:-${HOME}/.local/state}/ba-click-fx"
  (umask 077; mkdir -p -- "${log_dir}")
  log="$(mktemp "${log_dir}/rebuild.XXXXXX.log")"
  if ! bash "${source_dir}/install-local.sh" --repair >"${log}" 2>&1; then
    printf 'Rebuild failed. Log: %s\n' "${log}" >&2
    if command -v kdialog >/dev/null; then
      kdialog --title 'BA Click FX' --error \
        "自动重建失败 / Automatic rebuild failed.\n\nLog: ${log}" || true
    fi
    return 1
  fi
  printf 'Log: %s\n' "${log}"
}

if (( $# != 1 )); then
  echo "Usage: $0 --enable | --disable | --check | --run" >&2
  exit 2
fi

case "$1" in
  --enable) enable ;;
  --disable) rm -f -- "${entry}"; echo "已关闭登录检查 / Disabled the login check." ;;
  --check) check ;;
  --run) run ;;
  --help|-h) echo "Usage: $0 --enable | --disable | --check | --run" ;;
  *) echo "Usage: $0 --enable | --disable | --check | --run" >&2; exit 2 ;;
esac
