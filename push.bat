@echo off

git add .

git status

echo.
set /p msg=Commit message:

git commit -m "%msg%"

if errorlevel 1 (
    pause
    exit /b
)

git push

pause