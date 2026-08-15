#!/usr/bin/env bash
# 功能：在嵌套 KWin 会话里跑 BA Click FX，供手动测试。
# 用法：kwin-effects-ba-click-fx/test-nested.sh [--no-build] [--size WxH] [--debug|--profile]
#
# 为什么用嵌套会话：Wayland 下 effect 崩溃会把整个会话拖垮。嵌套会话跑在独立的
# D-Bus 与 wayland socket 上，插件崩了只死这一个窗口，主会话不受影响。
#
# 这个脚本不做任何自动断言，只负责「把环境搭好、把特效加载上」，
# 之后在弹出的窗口里手动点击/拖动即可。

set -euo pipefail

SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EFFECT_ID="kwin4_effect_ba_click_fx"
SOCKET="wayland-ba-click-fx"

BUILD=1
DEBUG=0
PROFILE=0
SIZE="1600x900"
# 嵌套输出的缩放倍数。逻辑尺寸仍是 --size，设备像素是它乘以这个值。
#
# 高分屏相关的 bug 只在 scale != 1 时才现形（合成四边形漏乘 scale 那次，
# scale=1 的屏上一切正常，scale=2 才看得出朝左上偏 + 尺寸减半 + 右下被裁）。
# 有了这个开关就不必把嵌套窗口拖到另一块物理屏上去复现，任何屏都能测。
SCALE="1"
for arg in "$@"; do
  case "${arg}" in
    --no-build) BUILD=0 ;;
    --debug) DEBUG=1 ;;
    --profile) PROFILE=1 ;;
    --size=*) SIZE="${arg#*=}" ;;
    --scale=*) SCALE="${arg#*=}" ;;
    *) echo "未知参数: ${arg}" >&2
       echo "用法: $0 [--no-build] [--size=WxH] [--scale=N] [--debug|--profile]" >&2
       exit 2 ;;
  esac
done

WIDTH="${SIZE%x*}"
HEIGHT="${SIZE#*x}"
if [[ ! "${WIDTH}" =~ ^[0-9]+$ || ! "${HEIGHT}" =~ ^[0-9]+$ ]]; then
  echo "--size 格式应为 WxH，例如 --size=1600x900" >&2
  exit 2
fi
if [[ ! "${SCALE}" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
  echo "--scale 应为正数，例如 --scale=2" >&2
  exit 2
fi

for cmd in kwin_wayland dbus-run-session konsole; do
  if ! command -v "${cmd}" >/dev/null 2>&1; then
    echo "缺少 ${cmd}，无法启动嵌套会话。" >&2
    exit 1
  fi
done

# --user 装到 ~/.local，不碰系统目录，也不影响正在跑的主会话 KWin。
if [[ "${BUILD}" -eq 1 ]]; then
  echo "==> 编译并安装到 ~/.local"
  "${SRC_DIR}/install-local.sh" --user --no-reload
fi

BUILD_DIR="${SRC_DIR}/build/plugin-user"
if [[ ! -f "${BUILD_DIR}/plugin-install-dir.txt" ]]; then
  echo "找不到 ${BUILD_DIR}/plugin-install-dir.txt，请先不带 --no-build 跑一次。" >&2
  exit 1
fi
PLUGIN_DIR="$(cat "${BUILD_DIR}/plugin-install-dir.txt")"

# qdbus 可执行文件名各发行版不统一：Fedora 44 只有 qdbus-qt6，没有 qdbus6。
# 先探测哪个存在再用，否则 loadEffect 静默失败，效果不会被加载。
QDBUS=""
for cmd in qdbus6 qdbus-qt6 qdbus; do
  if command -v "${cmd}" >/dev/null 2>&1; then
    QDBUS="${cmd}"
    break
  fi
done
if [[ -z "${QDBUS}" ]]; then
  echo "找不到 qdbus6 / qdbus-qt6 / qdbus，跳过 loadEffect。" >&2
  echo "特效可能不会自动加载，需要在「系统设置 → 桌面特效」里手动开启。" >&2
fi

if [[ "${DEBUG}" -eq 1 ]]; then
  echo "==> 打开 DebugLog 与重绘区域边框"
  kwriteconfig6 --file kwinrc --group Effect-ba-click-fx --key DebugLog true
  kwriteconfig6 --file kwinrc --group Effect-ba-click-fx --key DebugDamage true
elif [[ "${PROFILE}" -eq 1 ]]; then
  echo "==> 打开性能日志，关闭重绘区域边框"
  kwriteconfig6 --file kwinrc --group Effect-ba-click-fx --key DebugLog true
  kwriteconfig6 --file kwinrc --group Effect-ba-click-fx --key DebugDamage false
fi

echo "==> 启动嵌套会话 (${WIDTH}x${HEIGHT} @scale ${SCALE}, socket=${SOCKET})"
echo "    插件路径: ${PLUGIN_DIR}"
echo
echo "    黑色背景是正常的（嵌套 KWin 没有壁纸）。"
echo "    终端窗口可能不明显，直接在黑色区域里点击/拖动即可测试："
echo "      · 单击        → 圆环 + 旋转三角 + 4 个小三角迸发"
echo "      · 按住拖动    → ribbon 拖尾 + 沿途连发小三角"
echo "      · 快速连点    → 检查有没有异常放大的白圈"
echo "    关掉窗口即退出。"
echo

# 整个嵌套会话跑在自己的 D-Bus 上：qdbus6 在里面找到的 org.kde.KWin 才是嵌套那个，
# 不会误操作主会话的 KWin。
export QT_PLUGIN_PATH="${PLUGIN_DIR}:${QT_PLUGIN_PATH:-}"
export KWIN_EFFECT_ID="${EFFECT_ID}"
export KWIN_SOCKET="${SOCKET}"
export QDBUS

# 宿主合成器的 socket。嵌套的 kwin_wayland 要连到它上面开窗，
# 所以必须在这里存下来——子 shell 里 WAYLAND_DISPLAY 会被改成嵌套那个。
HOST_WAYLAND="${WAYLAND_DISPLAY:-}"
if [[ -z "${HOST_WAYLAND}" ]]; then
  echo "WAYLAND_DISPLAY 是空的，当前不在 Wayland 会话里，无法嵌套。" >&2
  exit 1
fi
export HOST_WAYLAND

dbus-run-session -- bash -c '
  set -uo pipefail

  # --wayland-display 是「嵌套模式」的开关：不给它（也不给 --x11-display /
  # --virtual / --drm）的话 kwin_wayland 默认走 DRM 后端，会直接抢显示器输出，
  # 表现就是一片黑屏。--width/--height/--scale 也只在窗口模式下才被读。
  kwin_wayland --wayland-display "${HOST_WAYLAND}" \
    --width "'"${WIDTH}"'" --height "'"${HEIGHT}"'" --scale "'"${SCALE}"'" \
    --socket="${KWIN_SOCKET}" --xwayland &
  kwin_pid=$!

  # 等 KWin 把 D-Bus 名字注册出来再动手，否则 loadEffect 会打在空气上。
  for _ in $(seq 1 60); do
    if [[ -n "${QDBUS}" ]] && "${QDBUS}" org.kde.KWin /Effects >/dev/null 2>&1; then
      break
    fi
    if ! kill -0 "${kwin_pid}" 2>/dev/null; then
      echo "嵌套 KWin 提前退出了。" >&2
      exit 1
    fi
    sleep 0.5
  done

  if [[ -n "${QDBUS}" ]]; then
    # 先无条件 loadEffect，再单独查一次 isEffectLoaded 来判定结果。
    #
    # 不能拿 loadEffect 的返回值当成败：它对**已经加载**的特效也返回 false。
    # 宿主 kwinrc 里一旦有 kwin4_effect_ba_click_fxEnabled=true（在系统设置里开过一次
    # 就会留下），嵌套 KWin 共用这份配置，启动时自己就把特效加载了，此时
    # loadEffect 必然返回 false —— 脚本却报「加载失败，检查 ABI」，
    # 把一次完全正常的启动误判成故障，白查半天 ABI 和 QT_PLUGIN_PATH。
    "${QDBUS}" org.kde.KWin /Effects loadEffect "${KWIN_EFFECT_ID}" >/dev/null 2>&1 || true
    if "${QDBUS}" org.kde.KWin /Effects isEffectLoaded "${KWIN_EFFECT_ID}" 2>/dev/null | grep -q true; then
      echo "[nested] 已加载 ${KWIN_EFFECT_ID}"
    else
      echo "[nested] 加载 ${KWIN_EFFECT_ID} 失败 —— 检查 QT_PLUGIN_PATH 与 ABI 版本。" >&2
    fi
  fi

  # 铺一个真正的桌面出来。裸 KWin 自己不画任何东西，只开一个 konsole 的话
  # 窗口以外全是黑的，看不出特效画在什么背景上；plasmashell 会铺壁纸和面板，
  # 整屏都可点，跟主会话里的观感一致。
  #
  # QT_QPA_PLATFORM 必须显式指定：这个 shell 里还继承着宿主的 DISPLAY=:0，
  # 不指定的话 Qt 可能挑 xcb 后端连回宿主的 X，窗口就开到嵌套会话外面去了。
  export WAYLAND_DISPLAY="${KWIN_SOCKET}"
  export QT_QPA_PLATFORM=wayland
  plasmashell >/dev/null 2>&1 &

  # 再给一个可点的窗口，方便测「特效画在窗口之上」。--separate 强制新实例，
  # 否则 konsole 的 D-Bus 单实例机制会把请求转回主会话，标签页开到外面去。
  konsole --separate >/dev/null 2>&1 &

  wait "${kwin_pid}"
'
