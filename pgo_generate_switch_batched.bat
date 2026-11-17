@echo off
rem fnxf_xiii (2026-10-29): the profiling build with 1 MB code files (--pgo generate --batched): its runs fit pgo_use_switch_o3_lto_batched.bat.
rem The code layout of build_switch.py --batched: a few hundred files instead of one a function (a much faster
rem full build). Switching between this layout and the per-function one (build_switch.py without --batched)
rem generates the code again and compiles all of it.
setlocal
python "%~dp0build_switch.py" --pgo generate --batched %* && (echo. & echo pgo_generate_switch_batched finished: out\fnxf_xiii\fnxf_xiii_pgo.nro & pause & exit /b 0) || (echo. & echo pgo_generate_switch_batched failed: the reason is above. & pause & exit /b 1)
