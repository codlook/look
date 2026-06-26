@echo off
if "%1"=="-c" (
    shift
    build\Release\look -c %*
) else (
    build\Release\look %1
)
