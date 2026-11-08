@echo off
chcp 65001 >nul
cd /d "%~dp0"
rem the newest Python 3.8+ there is: the py launcher first (python.org's installs), then "python"
set "PY="
for %%v in (3.14 3.13 3.12 3.11 3.10 3.9 3.8) do (
  if not defined PY (
    py -%%v -c "import sys" >nul 2>nul && set "PY=py -%%v"
  )
)
if not defined PY (
  python -c "import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)" >nul 2>nul && set "PY=python"
)
if not defined PY (
  echo 需要 Python 3.8 以上（你的電腦上沒有，或只有太舊的 3.7）。
  echo.
  echo 現在會打開 python.org 的下載頁：
  echo   1. 下載並安裝 Python 3.12（Windows installer 64-bit）
  echo   2. 安裝畫面第一頁勾選「Add python.exe to PATH」
  echo   3. 裝好後再雙擊這個檔案
  echo.
  start "" https://www.python.org/downloads/windows/
  pause
  exit /b 1
)
echo 使用的 Python：
%PY% --version
%PY% -c "import mido, rtmidi" >nul 2>nul
if errorlevel 1 (
  echo 第一次使用：安裝 mido 和 python-rtmidi（只用預先編譯好的版本，不需要編譯器）...
  %PY% -m pip install --disable-pip-version-check --only-binary python-rtmidi "python-rtmidi>=1.5.8" mido
  if errorlevel 1 (
    echo.
    echo 安裝失敗。請改裝 python.org 的 Python 3.12 再試，或把上面的訊息貼出來。
    start "" https://www.python.org/downloads/windows/
    pause
    exit /b 1
  )
)
echo.
echo 請先關掉 Chrome 的安裝頁和其他用 MIDI 的程式（DAW、M-VAVE 的 App）。
echo FM-1 在「Felucca Update」更新模式或正常開機都可以；它會自動接續。
echo.
%PY% fm1_install.py prism-1-1.2.fwsc --classic
echo.
pause
