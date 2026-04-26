@echo off
echo Starting RL Queue Server...

:: Kill any leftover process on port 9000 on the remote server before opening the tunnel
echo Clearing port 9000 on server...
ssh -i C:\Users\andre\.ssh\do_key root@46.101.184.78 "fuser -k 9000/tcp; sleep 1" 2>nul

:: Start SSH tunnel in a new window
start "SSH Tunnel" cmd /k ssh -i C:\Users\andre\.ssh\do_key -R 0.0.0.0:9000:127.0.0.1:9000 root@46.101.184.78 -N

:: Wait 3 seconds for tunnel to connect
timeout /t 3 /nobreak >nul

:: Start backend server in a new window
start "RL Queue Backend" cmd /k "cd /d F:\Rl testing\backend && python -m uvicorn main:app --host 0.0.0.0 --port 9000 --reload"

echo Done. Keep both windows open while playing.
pause
