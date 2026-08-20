#!/usr/bin/env bash
# 验证源码并生成可发布的源码压缩包和 SHA-256 校验文件。

set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
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

"${root_dir}/verify-release.sh"

mkdir -p "${out_dir}" "${work_dir}/${name}"
tar -cf - \
  --exclude='./.git' \
  --exclude='./build' \
  --exclude='./dist' \
  --exclude='./.cache' \
  . | tar -xf - -C "${work_dir}/${name}"

source_date_epoch="${SOURCE_DATE_EPOCH:-$(git log -1 --format=%ct 2>/dev/null || date +%s)}"
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
