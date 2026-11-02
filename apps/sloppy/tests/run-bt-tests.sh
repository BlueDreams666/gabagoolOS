#!/bin/sh
# Bluetooth backend and Bluetooth volume against fake BlueZ/BlueALSA tools.
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$test_dir/../../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/sloppy-bt.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM
mkdir -p "$work/src" "$work/bin" "$work/state"
cp "$repo/apps/hiby_bluetooth.c" "$repo/apps/hiby_bluetooth.h" \
   "$repo/firmware/target/hosted/hiby/hibylinux_codec.c" \
   "$repo/firmware/target/hosted/hiby/pcm-alsa-hiby.c" \
   "$repo/firmware/target/hosted/hiby/pcm-alsa-hiby.h" "$work/src/"

cat > "$work/bin/bluetoothctl" <<'EOF'
#!/bin/bash
S="$BT_STATE"
PHONE=AA:BB:CC:DD:EE:01
case "${1:-}" in
show) if [ -f "$S/powered" ]; then printf '\tPowered: yes\n'; else printf '\tPowered: no\n'; fi
      printf '\tAlias: %s\n\tPairable: yes\n\tDiscoverable: no\n' "$(cat "$S/alias" 2>/dev/null || echo 'HiBy R1')" ;;
power) if [ "$2" = on ]; then touch "$S/powered"; echo "Changing power on succeeded";
       else rm -f "$S/powered"; echo "Changing power off succeeded"; fi ;;
paired-devices|devices) echo "Device 24:58:7C:8A:10:20 Studio headphones" ;;
info) if [ "$2" = "$PHONE" ]; then printf "\tName: Tai's iPhone\n\tAlias: Tai's iPhone\n"
      else printf '\tName: Studio headphones\n\tAlias: Studio headphones\n'; fi ;;
connect) [ -f "$S/hang" ] && sleep 60   # a bluetoothctl that never answers
         n=$(cat "$S/fail" 2>/dev/null || echo 0)
         if [ "$n" -gt 0 ]; then echo $((n - 1)) > "$S/fail"; echo "Failed to connect"; exit 1; fi
         touch "$S/connected"; echo "Connection successful" ;;
disconnect) if [ "$2" = "$PHONE" ]; then rm -f "$S/phone"; echo "$2" > "$S/phone_dropped"
            else rm -f "$S/connected"; fi; echo "Successful disconnected" ;;
remove) echo "$2" >> "$S/removed"; echo ok ;;
trust|pair) echo ok ;;
"") exec python3 -I "$BT_BIN/agent.py" ;;   # interactive: GabagoolOS's scan, the receiver's agent
discoverable) echo "$2" > "$S/discoverable"; echo "Changing discoverable $2 succeeded" ;;
pairable) echo "$2" > "$S/pairable"; echo "Changing pairable $2 succeeded" ;;
system-alias) echo "$2" > "$S/alias"; echo "Changing $2 succeeded" ;;
reset-alias) echo "HiBy R1" > "$S/alias"; echo "Changing  succeeded" ;;
*) cat > /dev/null ;;
esac
EOF
cat > "$work/bin/bluealsa-cli" <<'EOF'
#!/bin/sh
S="$BT_STATE"
SINK=/org/bluealsa/hci0/dev_AA_BB_CC_DD_EE_01/a2dpsnk/source
case "$1" in
list-pcms) # "flaky": every other answer comes back empty, as BlueALSA does while busy
           if [ -f "$S/flaky" ]; then n=$(cat "$S/flaky"); echo $((n + 1)) > "$S/flaky"
               [ $((n % 2)) -eq 1 ] && exit 0; fi
           [ -f "$S/connected" ] && echo "/org/bluealsa/hci0/dev_24_58_7C_8A_10_20/a2dpsrc/sink"
           [ -f "$S/phone" ] && grep -q sink "$S/bluealsa" 2>/dev/null && echo "$SINK" ;;
info) case "$2" in
      *a2dpsnk*) printf 'Device: /org/bluez/hci0/dev_AA_BB_CC_DD_EE_01\nTransport: A2DP-sink\nMode: source\n'
                 printf 'Running: true\nFormat: %s\nChannels: 2\nSampling: 48000 Hz\n' "$(cat "$S/format" 2>/dev/null || echo S16_LE)"
                 printf 'Available codecs: SBC AAC\nSelected codec: AAC\nSoftVolume: false\n' ;;
      *) printf 'Selected codec: AAC\nSampling: 44100 Hz\nDelay: 152.0 ms\n' ;;
      esac ;;
codec) echo "Available codecs: SBC AAC" ;;
status) [ -f "$S/bluealsa" ] || { echo "Couldn't get BlueALSA service status" >&2; exit 1; }
        printf 'Service: org.bluealsa\nVersion: v4.1.1\nProfiles:\n'
        if grep -q sink "$S/bluealsa"; then echo "  A2DP-sink"; else echo "  A2DP-source"; fi ;;
volume) echo "Volume: L: $(cat "$S/volume" 2>/dev/null || echo 127) R: $(cat "$S/volume" 2>/dev/null || echo 127)" ;;
open) if [ -f "$S/open_fails" ]; then echo "CMD \"open\": Cannot open PCM: Not supported" >&2; exit 1; fi
      exec python3 -I "$BT_BIN/pcmgen.py" "$S" ;;
esac
exit 0
EOF
cat > "$work/bin/agent.py" <<'EOF'
# bluetoothctl kept open: answers the receiver's commands, asks the
# pairing question when the test wants one, ends on quit or EOF
import os, select, sys, time
s = os.environ['BT_STATE']; phone = 'AA:BB:CC:DD:EE:01'
def put(name, text): open(os.path.join(s, name), 'w').write(text + '\n')
def has(name): return os.path.exists(os.path.join(s, name))
def say(text, end='\n'): sys.stdout.write(text + end); sys.stdout.flush()
buf = b''
pending = ''
while True:
    while b'\n' not in buf:
        if has('pair_request') and not has('asked'):
            put('asked', ''); say('[agent] Accept pairing (yes/no): ', end='')
        if select.select([0], [], [], 0.2)[0]:
            more = os.read(0, 4096)
            if not more: sys.exit(0)
            buf += more
    line, buf = buf.split(b'\n', 1)
    line = line.decode()
    open(os.path.join(s, 'agent_said'), 'a').write(line + '\n')
    if line == 'agent NoInputNoOutput': say('Agent registered')
    elif line == 'default-agent': say('Default agent request successful')
    elif line.startswith('system-alias '): put('alias', line[13:]); say('Changing %s succeeded' % line[13:])
    elif line.startswith('pairable '): put('pairable', line[9:]); say('Changing pairable %s succeeded' % line[9:])
    elif line.startswith('discoverable '):
        if has('no_discoverable'): say('Failed to set discoverable on: org.bluez.Error.Failed')
        else: put('discoverable', line[13:]); say('\033[0;94m[bluetooth]\033[0m# Changing discoverable %s succeeded\r' % line[13:])
    elif line == 'yes' and not pending: put('answered', 'yes'); say('[CHG] Device %s Paired: yes' % phone)
    elif line.startswith('trust '): put('trusted', line[6:]); say('Changing %s trust succeeded' % line[6:])
    elif line.startswith('disconnect '):
        if has('phone'): os.unlink(os.path.join(s, 'phone'))
        put('phone_dropped', line[11:]); say('Successful disconnected')
    elif line.startswith('agent '): say('Agent registered')
    elif line.startswith('pair '):
        mac = line[5:]; mode = open(os.path.join(s, 'pair_mode')).read().strip() if has('pair_mode') else ''
        open(os.path.join(s, 'paired_with'), 'a').write(mac + '\n')
        if mode == 'confirm': pending = 'confirm'; say('\033[0;94m[agent]\033[0m Confirm passkey 123456 (yes/no): ', end='')
        elif mode == 'pin': pending = 'pin'; say('[agent] Enter PIN code: ', end='')
        elif mode == 'display': say('[agent] Passkey: 654321'); time.sleep(0.5); say('Pairing successful')
        elif mode == 'stale' and not (has('removed') and mac in open(os.path.join(s, 'removed')).read()):
            say('Failed to pair: org.bluez.Error.AuthenticationFailed')
        else: say('Pairing successful')
    elif line in ('quit', 'exit'): break
    elif pending:
        p, pending = pending, ''
        if p == 'confirm': say('Pairing successful' if line == 'yes' else 'Failed to pair: org.bluez.Error.AuthenticationRejected')
        elif p == 'pin':
            want = open(os.path.join(s, 'pin_expect')).read().strip()
            say('Pairing successful' if line == want else 'Failed to pair: org.bluez.Error.AuthenticationFailed')
EOF
cat > "$work/bin/pcmgen.py" <<'EOF'
# the phone's audio: 48 kHz S16_LE stereo at its own pace, left counting up
# and right its negative; nothing while "paused"; ends with the phone
import os, struct, sys, time
s = sys.argv[1]; n = 0
fmt = open(os.path.join(s, 'format')).read().strip() if os.path.exists(os.path.join(s, 'format')) else 'S16_LE'
try:
    while os.path.exists(os.path.join(s, 'phone')):
        if not os.path.exists(os.path.join(s, 'paused')):
            out = bytearray()
            for i in range(480):
                v = (n + i) & 0x3fff
                if fmt == 'S32_LE': out += struct.pack('<ii', v << 16, -v << 16)
                else: out += struct.pack('<hh', v, -v)
            n += 480
            sys.stdout.buffer.write(out); sys.stdout.buffer.flush()
        time.sleep(0.01)
except BrokenPipeError:
    pass
EOF
cat > "$work/bin/arecord" <<'EOF'
#!/bin/sh
echo "$*" > "$BT_STATE/arecord"
exec python3 -I "$BT_BIN/pcmgen.py" "$BT_STATE"
EOF
cat > "$work/bin/bluealsa" <<'EOF'
#!/bin/sh
S="$BT_STATE"
echo "$*" >> "$S/bluealsa_runs"
[ -f "$S/no_bluealsa" ] && exit 127
case "$*" in *a2dp-sink*) [ -f "$S/no_sink" ] && { echo "bluealsa: E: Couldn't register A2DP sink" >&2; exit 1; } ;; esac
echo "$*" > "$S/bluealsa"
EOF
cat > "$work/bin/killall" <<'EOF'
#!/bin/sh
case "$*" in *bluealsa*) rm -f "$BT_STATE/bluealsa" ;; esac
EOF
cat > "$work/bin/pgrep" <<'EOF'
#!/bin/sh
case "$*" in
*"-a -x bluealsa"*) [ -f "$BT_STATE/bluealsa" ] && echo "321 bluealsa $(cat "$BT_STATE/bluealsa")" ;;
*"-x bluealsa"*) [ -f "$BT_STATE/bluealsa" ] ;;
*) exit 1 ;;
esac
EOF
cat > "$work/bin/dbus-send" <<'EOF'
#!/bin/sh
S="$BT_STATE"
case "$*" in
*GetAll*) [ -f "$S/title" ] || { echo "Error org.freedesktop.DBus.Error.UnknownObject" >&2; exit 1; }
  printf 'method return time=1 sender=:1.3 -> destination=:1.9 serial=7 reply_serial=2\n   array [\n'
  printf '      dict entry(\n         string "Name"\n         variant             string "Music"\n      )\n'
  printf '      dict entry(\n         string "Track"\n         variant             array [\n'
  printf '               dict entry(\n                  string "Title"\n                  variant                      string "%s"\n               )\n' "$(cat "$S/title")"
  printf '               dict entry(\n                  string "Artist"\n                  variant                      string "%s"\n               )\n' "$(cat "$S/artist")"
  printf '               dict entry(\n                  string "Duration"\n                  variant                      uint32 215000\n               )\n            ]\n      )\n'
  printf '      dict entry(\n         string "Status"\n         variant             string "%s"\n      )\n   ]\n' "$(cat "$S/status" 2>/dev/null || echo playing)" ;;
*MediaPlayer1.*) all="$*"; m="${all##*MediaPlayer1.}"; echo "${m%% *}" > "$S/ctl" ;;
*Properties.Set*) printf '%s\n' "$@" > "$S/dbus_set" ;;
esac
EOF
chmod +x "$work/bin/"*

${CC:-cc} -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function \
    -Wno-deprecated-declarations -g -fsanitize=${SANITIZERS:-address,undefined} \
    -DROCKBOX_DIR="\"$work\"" -DHIBY_SYS_SERVER_SOCKET="\"sys.sock\"" \
    -I"$work/src" -I"$test_dir/bt_stubs" "$test_dir/bt_test.c" -o "$work/bt-test" -lpthread
cd "$work"   # the socket path is relative: temp paths outgrow sun_path
PATH="$work/bin:$PATH" BT_BIN="$work/bin" BT_STATE="$work/state" ASAN_OPTIONS=detect_leaks=0 "$work/bt-test" ||
    { rc=$?; echo "bluetooth.log:"; sed 's/^/  /' "$work/sloppy/bluetooth.log" | tail -60; exit $rc; }
echo "bluetooth.log:"; sed 's/^/  /' "$work/sloppy/bluetooth.log" | tail -${BT_LOG_LINES:-25}

# The ALSA output on a pretend Bluetooth PCM: pause, resume, seek, delay
${CC:-cc} -std=gnu99 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -Wno-pointer-sign \
    -g -fsanitize=${SANITIZERS:-address,undefined} -DHIBY_LINUX \
    -I"$test_dir/pcm_stubs" -I"$repo/firmware/target/hosted" -I"$repo/firmware/target/hosted/hiby" \
    "$test_dir/pcm_bt_test.c" -o "$work/pcm-bt-test" -lpthread
ASAN_OPTIONS=detect_leaks=0 "$work/pcm-bt-test" | grep -v '^  log:'
