@echo off
rem Shim so CMake can use the Python wrapper as GSPC_EXECUTABLE on Windows.
setlocal
set "GSPC_WRAP_PY=%~dp0gspc_depfile_fix.py"
set "PYTHON_EXE=python"
where python >nul 2>nul || set "PYTHON_EXE=py"
"%PYTHON_EXE%" "%GSPC_WRAP_PY%" %*
exit /b %ERRORLEVEL%
