@echo off
chcp 65001 >nul
cd /d "%~dp0"
where python >nul 2>nul
if errorlevel 1 (
  echo 找不到 Python。請先從 https://www.python.org/downloads/ 安裝 Python 3，
  echo 安裝畫面第一頁記得勾選「Add python.exe to PATH」，裝好後再雙擊這個檔案。
  pause
  exit /b 1
)
python -c "import usb, libusb_package" >nul 2>nul
if errorlevel 1 (
  echo 第一次使用：安裝 pyusb 和 libusb ...
  python -m pip install --disable-pip-version-check pyusb libusb-package
  if errorlevel 1 (
    echo 安裝失敗，請把上面的訊息貼出來。
    pause
    exit /b 1
  )
)
start "" pythonw fm1_rescue_gui.pyw
