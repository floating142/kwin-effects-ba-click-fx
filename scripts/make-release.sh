#!/usr/bin/env bash
# 验证源码并生成可发布的源码压缩包和 SHA-256 校验文件。

set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${root_dir}"

version="$(sed -nE 's/^project\([^)]* VERSION ([0-9.]+).*/\1/p' CMakeLists.txt)"
if [[ -z "${version}" ]]; then
  echo "无法读取版本号 / Unable to read version from CMakeLists.txt" >&2
  exit 1
fi

name="kwin-effects-ba-click-fx-${version}"
out_dir="${root_dir}/dist"
archive="${out_dir}/${name}.tar.xz"
checksums="${out_dir}/${name}-SHA256SUMS.txt"
work_dir="$(mktemp -d)"
trap 'rm -rf "${work_dir}"' EXIT

# 只将构建、安装及用户说明所需的文件放入源码包；
# CI、测试、发布脚本和商店素材不属于源码安装包。
archive_paths=(
  CMakeLists.txt LICENSE README.md README.en.md TESTING.md TODO.md
  src config
  assets shader po
  install-local.sh uninstall-local.sh test-nested.sh
  preview/logo.gif
)

in_git_worktree=0
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  in_git_worktree=1
  if ! git diff --quiet || ! git diff --cached --quiet; then
    echo "请先提交当前修改再生成发布包 / Commit the current changes before creating a release archive" >&2
    exit 1
  fi
fi

"${root_dir}/scripts/verify-release.sh"

mkdir -p "${out_dir}"
if [[ "${in_git_worktree}" -eq 1 ]]; then
  git archive --format=tar --prefix="${name}/" HEAD -- "${archive_paths[@]}" \
    | tar -xf - -C "${work_dir}"
  source_date_epoch="${SOURCE_DATE_EPOCH:-$(git log -1 --format=%ct)}"
else
  echo "==> 当前目录没有 Git 元数据，按发布白名单打包源码"
  echo "==> No Git metadata found; packaging the source allowlist"
  mkdir -p "${work_dir}/${name}"
  cp -a --parents "${archive_paths[@]}" "${work_dir}/${name}"
  source_date_epoch="${SOURCE_DATE_EPOCH:-$(date +%s)}"
fi

tar --sort=name \
  --mtime="@${source_date_epoch}" \
  --owner=0 --group=0 --numeric-owner \
  --mode='u+rwX,go+rX,go-w' \
  -C "${work_dir}" -cJf "${archive}" "${name}"

(
  cd "${out_dir}"
  sha256sum "${name}.tar.xz" > "$(basename "${checksums}")"
)

echo "发布产物已生成 / Release artifacts created:"
echo "  ${archive}"
echo "  ${checksums}"
