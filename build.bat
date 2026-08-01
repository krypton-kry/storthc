@echo off

pushd build
    cl ..\src\middle\*.c ..\src\frontend\*.c ..\src\backend\st_nasm_x86_64_linux.c ..\src\utils\*.c ..\src\st_main.c /std:c11 /FC /Zi /Fe:storthc.exe Kernel32.lib
popd
