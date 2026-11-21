#!/usr/bin/env bash
# Virtual Switch demo: runs every feature step by step in one terminal.
#   ./demo.sh          pause before each step (press Enter)
#   ./demo.sh --auto   run straight through
# Hosts run in the background; after each step the script prints what each
# host received and what the switch logged.

set -u
cd "$(dirname "$0")" || exit 1

AUTO=0
[ "${1:-}" = "--auto" ] && AUTO=1
PORT=9600
D=$(mktemp -d /tmp/vswitch-demo.XXXXXX)
declare -A FD SEEN
LOGSEEN=0
SWPID=""

if [ -t 1 ]; then
    B=$'\e[1m'; C=$'\e[36m'; G=$'\e[32m'; Y=$'\e[33m'; R=$'\e[31m'; N=$'\e[0m'
else
    B=""; C=""; G=""; Y=""; R=""; N=""
fi

cleanup() {
    for f in "${FD[@]}"; do eval "exec $f>&-" 2>/dev/null; done
    [ -n "$SWPID" ] && kill -INT "$SWPID" 2>/dev/null
    kill $(jobs -p) 2>/dev/null
    wait 2>/dev/null
    rm -rf "$D"
}
trap cleanup EXIT
trap 'echo; exit 130' INT

step() {
    echo
    echo "${B}${C}=== $1 ===${N}"
    shift
    for line in "$@"; do echo "  $line"; done
    if [ "$AUTO" = 1 ]; then sleep 0.5; else read -r -p "  ${Y}[Enter]${N} " _; fi
}

say()  { echo "${G}\$ $*${N}"; }
run()  { say "$@"; "$@" | sed 's/^/    /'; }
pause() { sleep "${1:-0.4}"; }

wait_for_file() {
    for _ in $(seq 50); do [ -e "$1" ] && return 0; sleep 0.1; done
    echo "${R}timed out waiting for $1${N}"; exit 1
}

# start_host NAME TCP_PORT MAC : host reads commands from a fifo, output to a file
start_host() {
    mkfifo "$D/$1.in"
    ./host 127.0.0.1 "$2" "$3" < "$D/$1.in" > "$D/$1.out" 2>&1 &
    local fd
    exec {fd}> "$D/$1.in"
    FD[$1]=$fd
    SEEN[$1]=2
    sleep 0.3
}

send() {
    local h=$1; shift
    echo "${G}$h> $*${N}"
    echo "$*" >&"${FD[$h]}"
}

# show NAME... : print the new lines each host received
show() {
    pause 0.5
    for h in "$@"; do
        local total
        total=$(wc -l < "$D/$h.out")
        if [ "$total" -le "${SEEN[$h]}" ]; then
            echo "    $h received: ${Y}nothing${N}"
        else
            tail -n +"$((SEEN[$h] + 1))" "$D/$h.out" | sed "s/^/    $h received: /"
        fi
        SEEN[$h]=$total
    done
}

# switch log lines since last time (without the timestamp date)
swlog() {
    pause 0.3
    local total
    total=$(wc -l < "$D/switch.log")
    if [ "$total" -gt "$LOGSEEN" ]; then
        tail -n +"$((LOGSEEN + 1))" "$D/switch.log" | grep -v "admin command" | cut -d' ' -f2- | sed "s/^/    ${C}switch log:${N} /"
    fi
    LOGSEEN=$total
}

# raw frame with any source MAC, written with bash /dev/tcp (for port security)
raw_frame() {
    local last
    last=$(printf '\\x%02x' "$2")
    printf "\x00\x05\x02\x00\x00\x00\x00${last}\xff\xff\xff\xff\xff\xff\x00\x01\x01hello" >&"$1"
}

echo "${B}Virtual Switch demo${N}  (Umair Abbas, bsai24077)"
make -s || exit 1
rm -f /tmp/vswitch.sock

cat > "$D/switch.conf" <<EOF
weights 5 3 1
max_wait_ms 200
wait_seconds 3
port_max_macs 8
log_file $D/switch.log
trunk 3
vlan 0 10
vlan 1 10
vlan 2 20
vlan 4 99
EOF

# ---------------------------------------------------------------- Tier 1
step "1. Start the switch" \
     "6 ports.  Port 0, 1: VLAN 10.  Port 2: VLAN 20.  Port 3: trunk." \
     "Port 4: VLAN 99, used later as the sniffer.  Port 5: free."
say ./switch $PORT 6 switch.conf
./switch $PORT 6 "$D/switch.conf" > /dev/null 2>&1 &
SWPID=$!
wait_for_file /tmp/vswitch.sock
start_host A $PORT 00:00:00:00:00:0a
start_host B $PORT 00:00:00:00:00:0b
start_host C $PORT 00:00:00:00:00:0c
start_host T $PORT 00:00:00:00:00:77
start_host S $PORT 00:00:00:00:00:55
swlog
run ./swctl show ports

step "2. MAC learning" \
     "A sends to B. The switch does not know B yet, so it floods (inside VLAN 10)." \
     "Then B answers: now both are learned, so the reply goes only to A."
send A 00:00:00:00:00:0b hello B
show B T C
send B 00:00:00:00:00:0a hi A, only you get this
show A T
run ./swctl show mac

step "3. Broadcast stays inside its VLAN" \
     "A (VLAN 10) broadcasts: B and the trunk get it, C (VLAN 20) does not."
send A all hello everyone
show B T C

step "4. Trunk port" \
     "The trunk carries every VLAN and keeps the vlan field." \
     "T sends a broadcast tagged VLAN 20: only C gets it."
send T vlan 20
send T all from the trunk, VLAN 20
show C A B

step "5. Port mirroring" \
     "Copy all traffic in and out of port 0 (A) to the sniffer on port 4."
run ./swctl mirror 0 4
send A 00:00:00:00:00:0b A to B, watched
pause
send B 00:00:00:00:00:0a B to A, watched
show B A S
run ./swctl mirror 0 off
swlog

step "6. Rate limit and priority queues" \
     "Limit port 1 (B) to 5 frames/s. A quickly sends 8 low then 8 high frames." \
     "High frames jump ahead (weights 5:3:1), low ones still get through."
run ./swctl rate 1 5
send A prio 2
echo "${G}A> 00:00:00:00:00:0b low 1 ... low 8${N}"
for i in 1 2 3 4 5 6 7 8; do echo "00:00:00:00:00:0b low $i" >&"${FD[A]}"; done
send A prio 0
echo "${G}A> 00:00:00:00:00:0b HIGH 1 ... HIGH 8${N}"
for i in 1 2 3 4 5 6 7 8; do echo "00:00:00:00:00:0b HIGH $i" >&"${FD[A]}"; done
send A prio 1
echo "    (waiting 4 s for the rate-limited port...)"
sleep 4
show B
run ./swctl rate 1 0

step "7. Live reconfiguration" \
     "Move C into VLAN 10 while the switch runs: now A's broadcast reaches C."
run ./swctl vlan 2 10
send A all C can hear me now
show C B
run ./swctl vlan 2 20

step "8. Shut a port down and bring it back" \
     "shutdown 1: B is disconnected, its queue is drained, its threads joined."
run ./swctl shutdown 1
show B
run ./swctl show ports
run ./swctl up 1
start_host B2 $PORT 00:00:00:00:00:b2
swlog

step "9. Port security (MAC flooding defence)" \
     "An attacker on port 5 sends frames from 9 different MACs (limit is 8)."
exec {ATK}<>/dev/tcp/127.0.0.1/$PORT
sleep 0.3
for i in 1 2 3 4 5 6 7 8 9; do raw_frame "$ATK" "$i"; done
swlog
exec {ATK}>&-
SEEN[T]=$(wc -l < "$D/T.out")

step "10. Port limit (semaphore)" \
     "Every port is now used or down. A new host waits 3 s, then gets 'switch full'."
say "connect a new host"
exec {X}<>/dev/tcp/127.0.0.1/$PORT
timeout 6 head -c 12 <&"$X" | sed 's/^/    reply: /'
exec {X}>&-
swlog
run ./swctl up 5

# ---------------------------------------------------------------- Tier 2 signals
step "11. Signals: SIGUSR1 dumps the MAC table"
say kill -USR1 $SWPID
kill -USR1 "$SWPID"
pause
sed 's/^/    /' mac_dump.txt

step "12. Signals: SIGHUP reloads the config file" \
     "Change port 2 to VLAN 30 and rate to 50 in the file, then send SIGHUP."
sed -i 's/^vlan 2 20/vlan 2 30/' "$D/switch.conf"
echo "rate 50" >> "$D/switch.conf"
say kill -HUP $SWPID
kill -HUP "$SWPID"
swlog
run ./swctl show ports

step "13. Counters in shared memory" \
     "swctl stats reads shared memory directly (process-shared mutex)." \
     "Live view in another terminal: ./swctl stats --watch"
run ./swctl stats

step "14. Clean shutdown with Ctrl+C (SIGINT)" \
     "Every thread joined, sockets closed, memory freed, shared memory removed."
for f in "${FD[@]}"; do eval "exec $f>&-"; done
FD=()
say kill -INT $SWPID
kill -INT "$SWPID"
wait "$SWPID"
echo "    exit code: $?"
SWPID=""
swlog
echo "    leftover shared memory: $(ls /dev/shm | grep -c vswitch) objects"

# ---------------------------------------------------------------- Tier 3
step "15. Bonus: two switches linked by two pipes" \
     "Switch A and switch B; the last port of each is a trunk over the pipes."
say ./twoswitch 9601 9602 4
./twoswitch 9601 9602 4 > "$D/pair.out" 2>&1 &
PAIR=$!
wait_for_file /tmp/vswitch-B.sock
start_host X 9601 00:00:00:00:00:a1
start_host Y 9602 00:00:00:00:00:b1
send X all hello from switch A
show Y
send Y 00:00:00:00:00:a1 hello back from switch B
show X
run ./swctl -n A show mac
run ./swctl -n B show mac
for h in X Y; do eval "exec ${FD[$h]}>&-"; unset "FD[$h]"; done
kill -INT "$PAIR"
wait "$PAIR"
sed 's/^/    /' "$D/pair.out" | grep -E "exited|pid"

step "16. Bonus: supervisor restarts a crashed switch" \
     "The MAC table is in shared memory, so it survives the crash."
say ./supervisor 9603 4
./supervisor 9603 4 > "$D/sup.out" 2>&1 &
SUP=$!
wait_for_file /tmp/vswitch.sock
start_host P 9603 00:00:00:00:00:c1
start_host Q 9603 00:00:00:00:00:c2
send P 00:00:00:00:00:c2 hi
send Q 00:00:00:00:00:c1 hi
pause
run ./swctl show mac
CHILD=$(pgrep -P "$SUP" -x switch)
say kill -9 $CHILD "   (crash the switch)"
kill -9 "$CHILD"
sleep 2.5
grep supervisor "$D/sup.out" | sed 's/^/    /'
run ./swctl show mac
for h in P Q; do eval "exec ${FD[$h]}>&-"; unset "FD[$h]"; done
kill -INT "$SUP"
wait "$SUP"

step "17. Bonus: lock benchmark" \
     "Million MAC table operations per second (run ./bench to measure again)."
if [ -f bench.csv ]; then
    column -t -s, bench.csv | sed 's/^/    /'
else
    ./bench | sed 's/^/    /'
fi

echo
echo "${B}${G}Demo finished.${N}"
