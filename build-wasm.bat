@echo off
rem ============================================================
rem  shya -> WebAssembly build (Emscripten)
rem  usage:  build-wasm.bat
rem  output: build\wasm\shya.mjs + build\wasm\shya.wasm
rem  note:   emcc.exe must be present at the path below
rem          the embedded macro library is the whole lib\ folder,
rem          mounted at /lib inside the module
rem ============================================================
setlocal
set "EMCC=C:\emsdk\upstream\emscripten\emcc.exe"
if not exist "%EMCC%" ( echo [build-wasm] ERROR: emcc.exe not found at %EMCC% & exit /b 1 )
set "ROOT=%~dp0"
if not exist "%ROOT%build\wasm" mkdir "%ROOT%build\wasm"

"%EMCC%" -sDEFAULT_TO_CXX -std=c++20 -O2 -I"%ROOT%src" ^
  "%ROOT%src\main.cpp" "%ROOT%src\diag.cpp" "%ROOT%src\lexer.cpp" "%ROOT%src\parser.cpp" ^
  "%ROOT%src\macro.cpp" "%ROOT%src\modules.cpp" "%ROOT%src\typecheck.cpp" ^
  "%ROOT%src\codegen.cpp" "%ROOT%src\stdlib.cpp" "%ROOT%src\wasm_api.cpp" ^
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sALLOW_MEMORY_GROWTH=1 -sINVOKE_RUN=0 -sSTACK_SIZE=8388608 ^
  -sEXPORTED_FUNCTIONS=_shya_compile,_shya_free,_malloc,_free ^
  -sEXPORTED_RUNTIME_METHODS=UTF8ToString,stringToUTF8,lengthBytesUTF8,FS,ccall,cwrap ^
  --embed-file "%ROOT%lib@/lib" ^
  -o "%ROOT%build\wasm\shya.mjs"
if errorlevel 1 ( echo [build-wasm] FAILED & exit /b 1 )
echo [build-wasm] OK
endlocal
