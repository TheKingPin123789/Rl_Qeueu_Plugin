@echo off
echo Installing dependencies...
pip install -r requirements.txt
echo.
echo Starting RL Queue Server on http://localhost:8000
echo Press Ctrl+C to stop.
echo.
python -m uvicorn main:app --host 0.0.0.0 --port 8000 --reload
pause
