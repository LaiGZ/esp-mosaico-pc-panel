@echo off
rem Double-click to start the PC status panel sender (window + tray icon).
rem
rem First run on a new PC:
rem     python -m pip install psutil pythonnet pystray
rem     python fetch_lhm.py          (downloads the sensor library into ./lhm)
rem Then fill in the device IP shown on the panel's screen and press start.
rem Closing the window keeps it running in the tray.
setlocal
cd /d "%~dp0"
python pc_status_gui.py %*
if errorlevel 1 (
    echo.
    echo Failed to start. Check Python 3.10+ and the dependencies:
    echo     python -m pip install psutil pythonnet pystray
    pause
)
endlocal
