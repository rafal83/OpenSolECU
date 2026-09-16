@echo off
call "%~1\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W3 /D_CRT_SECURE_NO_WARNINGS /Icomponents\inverter\include /Icomponents\aps_protocol\include tests\test_core.cpp components\inverter\core.cpp components\inverter\statistics_core.cpp components\aps_protocol\protocol.cpp components\aps_protocol\sniffer_core.cpp /Fetests\test_core.exe /Fotests\
if errorlevel 1 exit /b 1
tests\test_core.exe
exit /b %errorlevel%
