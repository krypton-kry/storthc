@echo off
if not exist build mkdir build

pushd build
    cl ..\src\frontend\comptime\*.c ..\src\middle\*.c ..\src\frontend\*.c ..\src\backend\st_nasm_x86_64_win32.c ..\src\utils\*.c ..\src\st_main.c /std:c11 /FC /Zi /Fe:storthc.exe
    set ERR=%errorlevel%
popd

exit /b %ERR%
