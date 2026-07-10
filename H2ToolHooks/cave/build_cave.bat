@echo off
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
cd /d "%~dp0"
echo === compile ===
cl /nologo /c /O2 /GS- /Gd /Fo:objfix_cave.obj objfix_cave.cpp
if errorlevel 1 (echo COMPILE FAILED & exit /b 1)
echo === link (base 0x17B0000, no CRT, keep relocs) ===
link /nologo /DLL /BASE:0x17B0000 /FIXED:NO /DYNAMICBASE:NO /NODEFAULTLIB /ENTRY:_DllMainCRTStartup /SUBSYSTEM:WINDOWS,5.01 /MAP:objfix_cave.map /OUT:objfix_cave.dll objfix_cave.obj
if errorlevel 1 (echo LINK FAILED & exit /b 1)
echo === extract .bin / .reloc / stub RVAs ===
py extract_cave.py
exit /b %errorlevel%
