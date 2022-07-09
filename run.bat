@echo off

echo Running...

pushd build
copy ..\rain.mp4
copy ..\shadmehr.mp4
copy ..\docking.mp4
copy ..\interstellar.mp4
.\vp.exe .\interstellar.mp4
popd build





