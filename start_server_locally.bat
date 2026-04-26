@echo off
title RL Queue Server (Local)
color 0A

echo ============================================================
echo   RL Custom Queue - Local Server
echo ============================================================
echo.

:: Move into the backend folder (relative to where this .bat lives)
cd /d "%~dp0backend"

:: Check Python is available
python --version >nul 2>&1
if errorlevel 1 (
    echo [ERROR] Python not found. Install Python 3.10+ and add it to PATH.
    pause
    exit /b 1
)

echo [1/2] Installing / updating dependencies...
pip install -r requirements.txt --quiet
echo       Done.
echo.

echo [2/2] Starting server on http://localhost:9000
echo       Web dashboard : http://localhost:9000
echo       Replay tester : http://localhost:9000/replay-test
echo       Leaderboard   : http://localhost:9000/leaderboard
echo.
echo       Press Ctrl+C to stop the server.
echo ============================================================
echo.

:: Open the dashboard in the default browser after a short delay
start "" /b cmd /c "timeout /t 2 >nul && start http://localhost:9000"

:: Run the server — 127.0.0.1 keeps it local-only (not exposed on LAN/internet)
:: Change to 0.0.0.0 if you want other PCs on your network to connect
python -m uvicorn main:app --host 127.0.0.1 --port 9000 --reload

echo.
echo Server stopped.
pause
