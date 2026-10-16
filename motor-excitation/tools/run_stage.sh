#!/bin/sh
# run_stage.sh NAME [REPEATS] : erase flash, chirp each motor REPEATS times (default 3), download, report, repeatability.
# Ctrl-C stops the current motor safely (excite.py stops the session); the FC also stops after 1 s without keepalive.
set -e
NAME=$1; REPS=${2:-3}; PORT=/dev/cu.usbmodem0x80000001; PY=.venv/bin/python
cd "$(dirname "$0")"
$PY -c "from msp import Msp; print('battery', Msp('$PORT').analog())"
$PY bbl_download.py $PORT --erase
for R in $(seq 1 $REPS); do
  for M in 1 2 3 4; do
    S=$((700 + R*10 + M))
    echo ">>> repeat $R motor $M (session $S)"
    $PY excite.py $PORT --motor $M --wave chirp --base 1200 --amp 40 --f0 20 --f1 900 --duration 20 --ramp 1.5 \
        --max-amps 3 --min-vbat 13.2 --session $S --log logs/${NAME}_r${R}_m$M.csv | grep -E "^ended|STOP|rejected|interrupted"
    sleep 6.5
  done
done
echo "=== ALL RUNS DONE: motors stopped, you can put the drone down ==="
$PY bbl_download.py $PORT logs/${NAME}.bbl
$PY -W ignore bbl_report.py logs/${NAME}.bbl --out logs/${NAME}_report > logs/${NAME}_report.txt 2>&1
$PY repeatability.py logs/${NAME}_report_peaks.csv | tee logs/${NAME}_repeatability.txt
echo "DONE $NAME"
