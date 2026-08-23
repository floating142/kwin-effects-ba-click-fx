#!/usr/bin/env bash
# Record the effective CI distribution, toolchain, and dependency versions.

set -euo pipefail

report_file="${1:-build-environment.txt}"

report_command()
{
  local title="$1"
  shift
  echo
  echo "${title}:"
  "$@" 2>&1 || true
}

{
  echo "Build environment"
  echo "================="
  report_command "Distribution" cat /etc/os-release
  report_command "Kernel" uname -a
  report_command "CMake" cmake --version
  report_command "C++ compiler" c++ --version
  report_command "Ninja" ninja --version

  if command -v pacman >/dev/null 2>&1; then
    report_command "Packages" pacman -Q \
      cmake extra-cmake-modules kwin libdrm libepoxy \
      qt6-base qt6-declarative vulkan-headers
  elif command -v rpm >/dev/null 2>&1; then
    report_command "Packages" rpm -q \
      cmake extra-cmake-modules kwin-devel \
      kf6-kcmutils-devel kf6-ki18n-devel \
      libdrm-devel libepoxy-devel qt6-qtbase-devel \
      qt6-qtdeclarative-devel qt6-qttools-devel vulkan-headers
  elif command -v dpkg-query >/dev/null 2>&1; then
    report_command "Packages" dpkg-query -W \
      cmake extra-cmake-modules kwin-dev \
      libdrm-dev libepoxy-dev libkf6config-dev libkf6coreaddons-dev \
      libkf6i18n-dev libkf6kcmutils-dev libkf6windowsystem-dev \
      qt6-base-dev qt6-declarative-dev qt6-tools-dev libvulkan-dev
  fi
} | tee "${report_file}"

if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
  {
    echo "## Build environment"
    echo
    echo '```text'
    cat "${report_file}"
    echo '```'
  } >> "${GITHUB_STEP_SUMMARY}"
fi
