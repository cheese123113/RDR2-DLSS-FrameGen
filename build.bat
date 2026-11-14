@echo off
rem Builds bin\DLSSFG.asi with MSVC (Visual Studio 2022 or newer, x64).
rem Needs the NVIDIA DLSS SDK (https://github.com/NVIDIA/DLSS): set DLSS_SDK to its folder,
rem or put it at ..\..\DLSS next to this project.
setlocal
cd /d "%~dp0"
if "%DLSS_SDK%"=="" set "DLSS_SDK=%~dp0..\..\DLSS"
if not exist "%DLSS_SDK%\include\nvsdk_ngx_vk.h" (
  echo DLSS SDK not found at "%DLSS_SDK%". Set DLSS_SDK to the SDK folder.
  exit /b 1
)
where cl >nul 2>&1 && goto build
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR for %%v in (18 2022) do for %%e in (Community Professional Enterprise BuildTools) do if exist "%ProgramFiles%\Microsoft Visual Studio\%%v\%%e\VC\Auxiliary\Build\vcvars64.bat" if not defined VSDIR set "VSDIR=%ProgramFiles%\Microsoft Visual Studio\%%v\%%e"
if not defined VSDIR (
  echo Visual Studio C++ build tools not found. Run this from a "x64 Native Tools Command Prompt".
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
:build
if not exist bin mkdir bin
cl /nologo /LD /O2 /MT /EHsc /std:c++17 /utf-8 /W3 /Zi ^
   /I third_party /I "%DLSS_SDK%\include" ^
   src\dlssfg.cpp src\fg.cpp ^
   /Fo:bin\ /Fd:bin\dlssfg.pdb ^
   /link /OUT:bin\DLSSFG.asi /PDB:bin\DLSSFG.pdb /DEBUG /OPT:REF /OPT:ICF ^
   "%DLSS_SDK%\lib\Windows_x86_64\x64\nvsdk_ngx_s.lib" kernel32.lib user32.lib advapi32.lib
exit /b %errorlevel%
