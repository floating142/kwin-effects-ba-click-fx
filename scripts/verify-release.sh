#!/usr/bin/env bash
# 配置、构建、测试，并对发布包执行不修改系统状态的完整性检查。

set -euo pipefail

src_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${src_dir}"
build_dir="${1:-build/verify}"
if [[ $# -gt 0 ]]; then
  shift
fi

required_files=(
  LICENSE README.md README.en.md CMakeLists.txt
  docs/README.md docs/architecture.md docs/testing.md docs/roadmap.md
  docs/store-description.zh-CN.md docs/store-description.en.md
  po/zh_CN/kwin_ba_click_fx_config.po
  po/zh_TW/kwin_ba_click_fx_config.po
  src/metadata.json src/effect/baclickfxeffect.cpp src/effect/baclickfxeffect.h
  src/effect/kwincompat.cpp src/effect/kwincompat.h
  src/input/frameclock.h src/unity/trailpoints.h src/unity/emission.h src/unity/lineribbon.h src/unity/billboard.h
  tests/native/billboard_oracle.cpp
  tests/native/distance_oracle.cpp tests/native/trail_mesh_oracle.cpp
  tests/fixtures/unity-trail-mesh-native.json
  tests/native/birth_oracle.cpp tests/fixtures/unity-birth-native.json
  tests/native/native_oracle.h tests/native/trail_points_oracle.cpp
  tests/fixtures/unity-trail-points-native.json
  src/input/mousebuttonutils.h
  src/input/applicationfilter.cpp src/input/applicationfilter.h
  src/render/gpurenderer.cpp src/render/gpurenderer.h src/particles/clickinstance.cpp src/particles/clickinstance.h
  src/render/gputexture.cpp src/render/gputexture.h
  src/particles/trailstream.cpp src/particles/trailstream.h src/particles/curveutils.cpp src/particles/curveutils.h
  src/effect/diagnostics.cpp src/effect/diagnostics.h src/render/damageutils.cpp src/render/damageutils.h
  src/render/bloomutils.cpp src/render/bloomutils.h
  src/particles/subsystems.cpp src/particles/subsystems.h
  src/render/meshprofiles.cpp src/render/meshprofiles.h
  src/particles/curvedata.h src/particles/touchparams.h
  src/unity/random.h src/unity/initial.h src/unity/shape.h src/unity/circle.h
  src/unity/gradient.h src/unity/curve.h src/unity/uv.h src/unity/rotation.h
  src/unity/motion.h src/unity/retirement.h src/unity/timing.h
  scripts/import-unity-touch.py scripts/verify-unity-native.py docs/unity-parity.md
  tests/fixtures/unity-touch.json tests/fixtures/unity-gradient-native.json
  tests/fixtures/unity-rotation-native.json tests/native/rotation_oracle.cpp
  tests/fixtures/unity-motion-native.json
  config/CMakeLists.txt config/baclickfxconfig.cpp config/baclickfxconfig.h
  config/baclickfxconfig.ui tests/CMakeLists.txt tests/logic_tests.cpp tests/gpu_tests.cpp tests/config_tests.cpp
  assets/Cylinder002.obj
  assets/FX_TEX_Triangle_02_1.png assets/FX_TEX_Trail_03.png
  assets/FX_TEX_Grad_Ring3.png assets/FX_TEX_Circle_01.png
  shader/particle.vert shader/transfer.glsl
  shader/additive.frag shader/alphablend_add.frag shader/background.frag
  shader/bloom_downsample.frag shader/bloom_prefilter.frag
  shader/bloom_upsample.frag shader/composite.frag shader/dissolve.frag
  install-local.sh uninstall-local.sh test-nested.sh
  scripts/ci-environment-report.sh scripts/make-release.sh scripts/verify-release.sh
)

failed=0

if ! command -v rg >/dev/null 2>&1; then
  echo "缺少命令: rg (ripgrep)" >&2
  echo "Missing command: rg (ripgrep)" >&2
  exit 1
fi

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
  echo "src/effect/diagnostics.cpp 未加入构建目标" >&2
  failed=1
fi
if ! rg -q 'damageutils\.cpp' src/CMakeLists.txt; then
  echo "src/render/damageutils.cpp 未加入构建目标" >&2
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
if git -C "${src_dir}" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  if ! git -C "${src_dir}" --no-pager diff --check; then
    failed=1
  fi
else
  echo "==> 跳过 Git 差异检查（当前目录不是 Git 工作树）"
  echo "==> Skip Git diff check (not a Git worktree)"
fi

if [[ "${failed}" -ne 0 ]]; then
  echo "release verification failed" >&2
  exit 1
fi

echo "==> 配置测试构建: ${build_dir}"
# Prefer Ninja when available. Minimal Debian/Ubuntu CI containers commonly
# install Ninja but not GNU Make, while CMake otherwise defaults to Unix Makefiles.
cmake_generator_args=()
if command -v ninja >/dev/null 2>&1; then
  cmake_generator_args=(-G Ninja)
fi
cmake -S . -B "${build_dir}" "${cmake_generator_args[@]}" \
  -DBUILD_TESTING=ON -DBA_CLICK_FX_BUILD_GPU_TESTS=OFF "$@"

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
    kwin_ba_click_fx_config.mo
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
