#!/usr/bin/env sh
set -u

NAME="${1:-Plugin-shot}"

# 精确匹配进程名（comm 字段）。注意 Linux 的 comm 只有 15 字符，
# 若项目名 > 15 字符，改用: pgrep -f "/bin/${NAME}$" / pkill -f "/bin/${NAME}$"
if ! pgrep -x "$NAME" >/dev/null 2>&1; then
    echo "[kill_running] $NAME is not running."
    exit 0
fi

echo "[kill_running] Stopping $NAME ..."
pkill -x "$NAME" >/dev/null 2>&1 || true

# 最多等 5 秒让进程释放文件句柄；超时则 SIGKILL
i=0
while pgrep -x "$NAME" >/dev/null 2>&1; do
    i=$((i + 1))
    if [ "$i" -ge 50 ]; then
        echo "[kill_running] Force killing $NAME ..."
        pkill -9 -x "$NAME" >/dev/null 2>&1 || true
        break
    fi
    sleep 0.1
done

echo "[kill_running] Done."
exit 0