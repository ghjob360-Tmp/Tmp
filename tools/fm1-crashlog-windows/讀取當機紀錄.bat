@echo off
chcp 65001 >nul
cd /d "%~dp0"
set "PY="
for %%v in (3.14 3.13 3.12 3.11 3.10 3.9 3.8) do (
  if not defined PY (
    py -%%v -c "import mido, rtmidi" >nul 2>nul && set "PY=py -%%v"
  )
)
if not defined PY (
  echo 找不到裝好 mido 和 python-rtmidi 的 Python 3.8 以上：先用「安裝PRISM-1.bat」裝一次（它會安裝這兩個套件）。
  pause
  exit /b 1
)
echo 讓 FM-1 停在安全模式（safe mode）、接上 USB，關掉 Chrome 和其他用 MIDI 的程式。
echo 讀取中 ...
echo.
(
  echo ===== log =====
  %PY% fm1_remote.py --elf prism-1-1.3.elf log
  echo.
  echo ===== crashes =====
  %PY% fm1_remote.py --elf prism-1-1.3.elf crashes
) > prism1-crash-log.txt 2>&1
type prism1-crash-log.txt
echo.
echo 已存成 prism1-crash-log.txt：請把這個檔案（或上面整段文字）傳回來。
pause
