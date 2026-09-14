#!/bin/sh
# OMHOST 启动器：起本地后端并自动打开界面（Ctrl+C 退出）
exec python3 "$(dirname "$0")/server.py" "$@"
