@echo off
REM Framework-dependent single-file Release publish (the ~3.4MB stable build; NOT --self-contained=the 75MB crashy one).
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
set "PATH=C:\Program Files\LLVM\bin;C:\Program Files\CMake\bin;C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;C:\Program Files\dotnet;%PATH%"
set "DevEnvDir=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\"
cd /d "C:\Users\hurri\Downloads\Osoyoos-Launcher-main (1)\Osoyoos-Launcher-main"
echo ===== dotnet publish (Release, framework-dependent single-file) =====
dotnet publish ".\OsoyoosLauncher\OsoyoosLauncher.csproj" -c Release -r win-x64 -p:Platform=x64 -p:PublishSingleFile=true -p:PublishTrimmed=false --self-contained false -v minimal
echo ===== EXIT %errorlevel% =====
exit /b %errorlevel%
