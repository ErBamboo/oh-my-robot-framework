@echo off
chcp 65001 >nul
REM OMHOST 启动器：起本地后端并自动打开界面（Ctrl+C 退出）
setlocal
set "S=%~dp0server.py"
python "%S%" %*
set "RC=%ERRORLEVEL%"
if not "%RC%"=="9009" goto run_done
py "%S%" %*
set "RC=%ERRORLEVEL%"
:run_done
if "%RC%"=="0" goto end
echo.
echo omhost 退出，代码 %RC%
pause
:end
endlocal & exit /b %RC%
