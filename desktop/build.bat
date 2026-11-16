@echo off
setlocal
cd /d "%~dp0"
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo [x] vcvars64.bat not found. Install Visual Studio 2022 C++ toolchain.
  exit /b 1
)
if not exist bin mkdir bin
if not exist bin\obj mkdir bin\obj
call "%VCVARS%" >nul
if errorlevel 1 exit /b 1

echo [*] compiling resources...
rc /nologo /fo bin\obj\app.res src\app.rc
if errorlevel 1 (
  echo [x] resource build failed
  exit /b 1
)

echo [*] compiling...
cl /nologo /std:c++17 /EHsc /O2 /MD /utf-8 /W3 /wd4996 ^
  /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS ^
  /I third_party /I third_party\webview2\include /I src ^
  src\common.cpp src\scanner.cpp src\launcher.cpp src\terminal.cpp src\search.cpp src\backend.cpp src\main.cpp ^
  /Fo:bin\obj\ /Fe:bin\cli-resume.exe ^
  /link bin\obj\app.res /SUBSYSTEM:WINDOWS /LIBPATH:third_party\webview2\lib\x64 ^
  WebView2LoaderStatic.lib user32.lib gdi32.lib ole32.lib oleaut32.lib advapi32.lib ^
  shell32.lib shlwapi.lib version.lib ws2_32.lib comctl32.lib crypt32.lib
if errorlevel 1 (
  echo [x] build failed
  exit /b 1
)
if exist web (
  if exist bin\web rmdir /s /q bin\web
  xcopy /e /i /q web bin\web >nul
)
echo [+] done: bin\cli-resume.exe
