#!/usr/bin/env bash
# ---------------------------------------------------------------------------
#  加构建锁后执行一次 cmake --build。
#
#  为什么需要锁：多个终端 / 多个 agent 共用同一个 Ninja 目录时，并发构建会互相踩
#  （重复链接、*.ilk 争用、CMake regenerate 竞争）。锁目录 build/.build-lock 里写
#  持有者 PID：持有者被超时杀掉（trap 来不及执行）时，下一个等待者能识别并清理。
#
#  用法：tools/verify/build.sh [cmake --build 参数...]
#    例：tools/verify/build.sh                      # 全量
#        tools/verify/build.sh --target tray        # 只构建一个 target
#
#  注意：调用方要给足超时（全量构建通常 > 2 分钟），建议 10 分钟。
# ---------------------------------------------------------------------------
set -u
cd "$(dirname "$0")/../.." || exit 1
LOCK=build/.build-lock

cleanup() { rm -rf "$LOCK" 2>/dev/null; }

acquire() {
    local waited=0 pid=""
    while true; do
        if mkdir "$LOCK" 2>/dev/null; then
            echo "$$" > "$LOCK/pid"
            trap cleanup EXIT TERM INT
            return 0
        fi
        pid=""
        [ -f "$LOCK/pid" ] && pid=$(cat "$LOCK/pid" 2>/dev/null || true)
        if [ -n "$pid" ] && ! kill -0 "$pid" 2>/dev/null; then
            echo "[lock] 持有者 PID $pid 已退出，清理过期锁"
            rm -rf "$LOCK"
            continue
        fi
        if [ -z "$pid" ] && [ -n "$(find "$LOCK" -maxdepth 0 -mmin +3 2>/dev/null)" ]; then
            echo "[lock] 无 PID 且超过 3 分钟的旧锁目录，清理"
            rm -rf "$LOCK"
            continue
        fi
        waited=$((waited + 1))
        if [ "$waited" -ge 600 ]; then
            echo "[lock] 等待构建锁超过 10 分钟；先检查是否有卡死的构建进程" >&2
            return 1
        fi
        sleep 1
    done
}

acquire || exit 3
# MSYS bash 的 pwd 是 /d/src/... 形式，cmd/powershell 要 D:\src\... 形式。
ROOT_WIN=$(pwd | sed -E 's#^/([A-Za-z])/#\1:/#; s#/#\\\\#g')
powershell.exe -NoProfile -Command "& '${ROOT_WIN}\\tools\\verify\\build.bat' $* 2>&1 | Select-Object -Last 40; exit \$LASTEXITCODE"
