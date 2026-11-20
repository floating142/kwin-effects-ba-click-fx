# SPDX-License-Identifier: GPL-3.0-or-later
# Shared by the source installer, uninstaller and login check.

BA_CLICK_FX_EFFECT_ID=kwin4_effect_ba_click_fx

ba_click_fx_message() {
  case "${LC_ALL:-${LC_MESSAGES:-${LANG:-C}}}" in
    zh*) printf '%s\n' "$1" ;;
    *) printf '%s\n' "$2" ;;
  esac
}

ba_click_fx_installed_plugin() {
  local build_dir="$1" plugin_dir plugin
  [[ -s "${build_dir}/plugin-install-dir.txt" && -r "${build_dir}/install_manifest.txt" ]] || return 1
  plugin_dir="$(<"${build_dir}/plugin-install-dir.txt")"
  [[ "${plugin_dir}" == /* ]] || return 1
  plugin="${plugin_dir}/kwin/effects/plugins/${BA_CLICK_FX_EFFECT_ID}.so"
  [[ -f "${plugin}" ]] && grep -Fxq -- "${plugin}" "${build_dir}/install_manifest.txt" || return 1
  printf '%s\n' "${plugin}"
}

ba_click_fx_kwin_version() {
  local output
  # KWin prints its version on stdout. Keep Qt startup diagnostics out of the check.
  output="$(kwin_wayland --version 2>/dev/null)" || return 1
  if [[ "${output}" =~ ^kwin([_-]wayland)?[[:space:]]+([0-9]+\.[0-9]+\.[0-9]+)($|[[:space:]]) ]]; then
    printf '%s\n' "${BASH_REMATCH[2]}"
  else
    return 1
  fi
}

ba_click_fx_plugin_version() {
  local plugin="$1" tool qtpaths qtbin output
  for tool in qtplugininfo6 qtplugininfo-qt6 qtplugininfo; do
    if output="$("${tool}" --print iid "${plugin}" 2>/dev/null)" &&
        ba_click_fx_read_iid "${output}"; then
      return 0
    fi
  done
  # Some distributions keep Qt tools outside PATH.
  for qtpaths in qtpaths6 qtpaths-qt6 qtpaths; do
    if qtbin="$("${qtpaths}" --query QT_INSTALL_BINS 2>/dev/null)" &&
        [[ "${qtbin}" == /* ]] &&
        output="$("${qtbin}/qtplugininfo" --print iid "${plugin}" 2>/dev/null)" &&
        ba_click_fx_read_iid "${output}"; then
      return 0
    fi
  done
  return 1
}

ba_click_fx_read_iid() {
  local iid_pattern='^IID[[:space:]]+"org\.kde\.kwin\.EffectPluginFactory([0-9]+\.[0-9]+\.[0-9]+)"[[:space:]]*$'
  [[ "$1" =~ ${iid_pattern} ]] || return 1
  printf '%s\n' "${BASH_REMATCH[1]}"
}

# Keep the lock outside the per-mode build directory, which the installer may replace.
# The caller keeps the file descriptor open for the duration of the operation.
ba_click_fx_lock_build() {
  local build_dir="$1"
  mkdir -p -- "${build_dir%/*}" || return 1
  exec {BA_CLICK_FX_LOCK_FD}>"${build_dir}.lock" || return 1
  flock -n "${BA_CLICK_FX_LOCK_FD}"
}
