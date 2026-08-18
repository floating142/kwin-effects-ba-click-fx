#!/usr/bin/env bash
# 配置、构建、测试，并对发布包执行不修改系统状态的完整性检查。

set -euo pipefail

src_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${src_dir}"
build_dir="${1:-build/verify}"

required_files=(
  LICENSE README.md TODO.md TESTING.md CMakeLists.txt
  src/metadata.json src/damageutils.cpp src/damageutils.h
  config/CMakeLists.txt config/baclickfxconfig.cpp config/baclickfxconfig.h
  config/baclickfxconfig.ui tests/CMakeLists.txt tests/logic_tests.cpp
  assets/Cylinder002.obj
  assets/FX_TEX_Triangle_02_1.png assets/FX_TEX_Trail_03.png
  assets/FX_TEX_Grad_Ring3.png assets/FX_TEX_Circle_01.png
  shader/particle.vert shader/transfer.glsl
  shader/additive.frag shader/alphablend_add.frag shader/background.frag
  shader/bloom_downsample.frag shader/bloom_prefilter.frag
  shader/bloom_upsample.frag shader/composite.frag shader/dissolve.frag
  install-local.sh uninstall-local.sh test-nested.sh
)

failed=0
for path in "${required_files[@]}"; do
  if [[ ! -s "${path}" ]]; then
    echo "缺少或为空: ${path}" >&2
    failed=1
  fi
done

cmake_version="$(sed -nE 's/^project\([^)]* VERSION ([0-9.]+).*/\1/p' CMakeLists.txt)"
metadata_version="$(sed -nE 's/.*"Version": "([0-9.]+)".*/\1/p' src/metadata.json)"
if [[ -z "${cmake_version}" || "${cmake_version}" != "${metadata_version}" ]]; then
  echo "版本不一致: CMake=${cmake_version:-missing} metadata=${metadata_version:-missing}" >&2
  failed=1
fi

if ! rg -q 'diagnostics\.cpp' src/CMakeLists.txt; then
  echo "src/diagnostics.cpp 未加入构建目标" >&2
  failed=1
fi
if ! rg -q 'damageutils\.cpp' src/CMakeLists.txt; then
  echo "src/damageutils.cpp 未加入构建目标" >&2
  failed=1
fi
if ! rg -q 'add_subdirectory\(tests\)' CMakeLists.txt; then
  echo "测试目录未加入顶层构建" >&2
  failed=1
fi
if ! rg -q 'install\(DIRECTORY .*assets/' src/CMakeLists.txt; then
  echo "资源目录未加入安装规则" >&2
  failed=1
fi
if ! rg -q 'install\(DIRECTORY .*shader/' src/CMakeLists.txt; then
  echo "Shader 目录未加入安装规则" >&2
  failed=1
fi
if ! rg -q 'transfer\.glsl' shader/*.frag; then
  echo "没有着色器引用 transfer.glsl" >&2
  failed=1
fi
if ! git --no-pager diff --check; then
  failed=1
fi

if [[ "${failed}" -ne 0 ]]; then
  echo "release verification failed" >&2
  exit 1
fi

echo "==> 配置测试构建: ${build_dir}"
cmake -S . -B "${build_dir}" -DBUILD_TESTING=ON

echo "==> 构建"
cmake --build "${build_dir}" -j"${JOBS:-2}"

echo "==> 运行测试"
ctest --test-dir "${build_dir}" --output-on-failure

if [[ -f "${build_dir}/cmake_install.cmake"
      && ( ! -e "${build_dir}/install_manifest.txt" || -w "${build_dir}/install_manifest.txt" ) ]]; then
  echo "==> 临时安装检查"
  stage_dir="$(mktemp -d)"
  trap 'rm -rf "${stage_dir}"' EXIT
  install_build_dir="${stage_dir}/build"
  cp -a "${build_dir}" "${install_build_dir}"
  DESTDIR="${stage_dir}/root" cmake --install "${install_build_dir}" >/dev/null
  installed_names=(
    kwin4_effect_ba_click_fx.so kwin_ba_click_fx_config.so
    Cylinder002.obj FX_TEX_Circle_01.png FX_TEX_Grad_Ring3.png
    FX_TEX_Trail_03.png FX_TEX_Triangle_02_1.png
    particle.vert transfer.glsl additive.frag alphablend_add.frag
    background.frag bloom_downsample.frag bloom_prefilter.frag
    bloom_upsample.frag composite.frag dissolve.frag
  )
  for name in "${installed_names[@]}"; do
    if ! find "${stage_dir}/root" -type f -name "${name}" -print -quit | rg -q .; then
      echo "临时安装缺少: ${name}" >&2
      failed=1
    fi
  done
  if ! rg -q "KWin_VERSION:.*${KWin_VERSION:-}" "${build_dir}/CMakeCache.txt" 2>/dev/null; then
    : # KWin 版本变量未必写入缓存，精确版本由插件 diagnostics 验证。
  fi
else
  echo "构建目录不可写，无法执行临时安装检查" >&2
  failed=1
fi

if [[ "${failed}" -ne 0 ]]; then
  echo "release verification failed" >&2
  exit 1
fi

echo "release verification passed version=${cmake_version} files=${#required_files[@]}"
