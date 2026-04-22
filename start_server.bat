@echo off
echo Starting RL Queue Server...

:: Start SSH tunnel in a new window
start "SSH Tunnel" cmd /k "ssh -i C:\Users\andre\.ssh\do_key -R 0.0.0.0:8000:127.0.0.1:8000 root@46.101.184.78 -N"

:: Wait 2 seconds for tunnel to connect
timeout /t 2 /nobreak >nul

:: Start backend server in a new window
start "RL Queue Backend" cmd /k "F: && cd "Rl testing\backend" && start.bat"

echo Both windows started. Close this window when done.
pause
