#!/bin/bash
# usage: simctl.sh start [wait] | shot NAME | tap X Y | drag X1 Y1 X2 Y2 [steps] | stop   (coords in LCD pixels)
export DISPLAY=:9
pos() { xdotool search --name "" getwindowgeometry %@ 2>/dev/null | grep -B1 "480x800" | head -1 | sed 's/.*Position: \([0-9]*\),\([0-9]*\).*/\1 \2/'; }
case "$1" in
  start) pgrep Xvfb >/dev/null || (Xvfb :9 -screen 0 1024x1024x24 >/dev/null 2>&1 &); sleep 1
         cd ${SIMDIR:-build-sim} && (SDL_AUDIODRIVER=dummy setsid nohup ./rockboxui --nobackground > sim.log 2>&1 &); sleep ${2:-6};;
  shot)  read X Y < <(pos); import -window root /tmp/full.png && convert /tmp/full.png -crop 480x800+$X+$Y +repage ${SHOTS:-shots}/$2.png;;
  tap)   read X Y < <(pos); xdotool mousemove $((X+$2)) $((Y+$3)) mousedown 1; sleep 0.08; xdotool mouseup 1;;
  drag)  read X Y < <(pos); steps=${6:-12}; xdotool mousemove $((X+$2)) $((Y+$3)) mousedown 1
         for i in $(seq 1 $steps); do xdotool mousemove $((X + $2 + ($4-$2)*i/steps)) $((Y + $3 + ($5-$3)*i/steps)); sleep 0.016; done
         xdotool mouseup 1;;
  stop)  pkill rockboxui; sleep 1;;
esac
