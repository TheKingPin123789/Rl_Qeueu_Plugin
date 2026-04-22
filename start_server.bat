@echo off
echo Starting RL Queue Server...

:: Start SSH tunnel in a new window
start "SSH Tunnel" cmd /k ssh -i C:\Users\andre\.ssh\do_key -R 0.0.0.0:8000:127.0.0.1:8000 root@46.101.184.78 -N

:: Wait 3 seconds for tunnel to connect
timeout /t 3 /nobreak >nul

:: Start backend server in a new window
start "RL Queue Backend" cmd /k "cd /d F:\Rl testing\backend && python -m uvicorn main:app --host 0.0.0.0 --port 8000 --reload"

echo Done. Keep both windows open while playing.
pause
