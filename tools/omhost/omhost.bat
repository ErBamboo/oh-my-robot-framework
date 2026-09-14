@echo off
REM OMHOST 启动器：起本地后端并自动打开界面（Ctrl+C 退出）
setlocal
set "S=%~dp0server.py"
python "%S%" %*
if errorlevel 9009 py "%S%" %*
endlocal
