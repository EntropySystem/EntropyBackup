@echo off
rem Renders the built-in background videos into assets\backgrounds (needs ffmpeg on PATH).
rem They are seamless 20-second loops at 1280x720, 30 fps, embedded in EntropyBackup.exe by src\backgrounds.rc.
setlocal
cd /d "%~dp0.."
cmake --build build --config Release --target bggen || exit /b 1
if not exist assets\backgrounds mkdir assets\backgrounds
set "ENC=-c:v libx264 -preset slow -crf 22 -pix_fmt yuv420p -g 60 -movflags +faststart -an"
build\Release\bggen.exe 1 | ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 -s 1280x720 -r 30 -i - %ENC% assets\backgrounds\nebula-lattice.mp4 || exit /b 1
build\Release\bggen.exe 2 | ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 -s 1280x720 -r 30 -i - %ENC% assets\backgrounds\tidal-fractal.mp4 || exit /b 1
build\Release\bggen.exe 3 | ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 -s 1280x720 -r 30 -i - %ENC% assets\backgrounds\sacred-geometry.mp4 || exit /b 1
echo Done: assets\backgrounds
