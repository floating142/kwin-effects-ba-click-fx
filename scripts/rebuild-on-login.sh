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
    ba_click_fx_message \
      "找不到此源码目录的系统安装。" "No system installation recorded for this checkout." >&2
    return 2
  fi
  if ! installed_version="$(ba_click_fx_kwin_version)" ||
      ! plugin_version="$(ba_click_fx_plugin_version "${plugin}")"; then
    ba_click_fx_message "无法读取版本。请确认已安装 KWin 和 Qt 6 的 qtplugininfo。" \
      "Could not read versions. Check that KWin and Qt 6 qtplugininfo are installed." >&2
    return 2
  fi
  printf 'KWin: %s\nPlugin IID version: %s\nPlugin: %s\n' "${installed_version}" "${plugin_version}" "${plugin}"
  [[ "${installed_version}" == "${plugin_version}" ]]
}

enable() {
  local status=0 path temp_entry
  check >/dev/null || status=$?
  (( status < 2 )) || return "${status}"
  for tool in flock kdialog pkexec; do
    if ! command -v "${tool}" >/dev/null; then
      ba_click_fx_message "缺少命令：${tool}" "Missing command: ${tool}" >&2
      return 2
    fi
  done

  path="${script_dir}/rebuild-on-login.sh"
  if [[ "${path}" == *$'\n'* || "${path}" == *$'\r'* || "${path}" == *$'\t'* ]]; then
    ba_click_fx_message "源码路径不能包含控制字符。" "The checkout path must not contain control characters." >&2
    return 2
  fi
  # Exec uses both desktop-entry string escaping and command-line quoting.
  path="${path//\\/\\\\\\\\}"
  path="${path//\"/\\\\\"}"
  path="${path//\$/\\\\\$}"
  path="${path//\`/\\\\\`}"
  path="${path//%/%%}"

  mkdir -p -- "${entry%/*}" || return 1
  temp_entry="$(mktemp "${entry}.XXXXXX")" || return 1
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
      ba_click_fx_message "已开启登录检查。" "Login check enabled."
      return 0
    fi
  fi
  rm -f -- "${temp_entry}"
  return 1
}

offer() {
  local answer
  # Preserve existing Plasma Autostart settings and skip unattended installs.
  [[ -t 0 && -t 1 && ! -e "${entry}" ]] || return 0
  read -rp "$(ba_click_fx_message '登录 Plasma 时检查插件是否需要重建？[y/N] ' \
    'Check plugin compatibility at Plasma login? [y/N] ')" answer || return 0
  case "${answer}" in
    y|Y|[yY][eE][sS]|是) enable ;;
  esac
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
  --disable) rm -f -- "${entry}"; ba_click_fx_message "已关闭登录检查。" "Login check disabled." ;;
  --offer) offer ;; # Invoked after an interactive system install.
  --check) check ;;
  --run) run ;;
  --help|-h) echo "Usage: $0 --enable | --disable | --check | --run" ;;
  *) echo "Usage: $0 --enable | --disable | --check | --run" >&2; exit 2 ;;
esac
