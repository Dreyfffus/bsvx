@echo off

pushd ..
bsxv\premake\premake5.exe --file=bsxv\Build.lua vs2022
popd
pause