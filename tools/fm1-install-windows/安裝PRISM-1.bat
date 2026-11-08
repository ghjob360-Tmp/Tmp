@echo off
chcp 65001 >nul
cd /d "%~dp0"
where python >nul 2>nul
if errorlevel 1 (
  echo 找不到 Python。請先安裝 Python 3（安裝時勾選 Add python.exe to PATH）。
  pause
  exit /b 1
)
python -c "import mido, rtmidi" >nul 2>nul
if errorlevel 1 (
  echo 第一次使用：安裝 mido 和 python-rtmidi ...
  python -m pip install --disable-pip-version-check mido python-rtmidi
  if errorlevel 1 (
    echo 安裝失敗，請把上面的訊息貼出來。
    pause
    exit /b 1
  )
)
echo.
echo 請先關掉 Chrome 的安裝頁和其他用 MIDI 的程式（DAW、M-VAVE 的 App）。
echo FM-1 在「Felucca Update」更新模式或正常開機都可以；它會自動接續。
echo.
python fm1_install.py prism-1-1.2.fwsc --classic
echo.
pause
