#!/bin/sh
# run_tap.sh NAME [ARMS, default 5,6,1,2,3,4 = battery, frame bottom, arms 1-4] : erase flash, tap test with voice prompts, download, analyse against the suspended chirp
set -e
NAME=$1; ARMS=${2:-5,6,1,2,3,4}; PORT=/dev/cu.usbmodem0x80000001; PY=.venv/bin/python
cd "$(dirname "$0")"
$PY bbl_download.py $PORT --erase
$PY tap_test.py $PORT --taps 5 --window 8 --arms $ARMS
echo "=== TAPS DONE, downloading ==="
sleep 6
$PY bbl_download.py $PORT logs/${NAME}.bbl
$PY -W ignore tap_report.py logs/${NAME}.bbl --compare logs/stage3_hung_report_peaks.csv | tee logs/${NAME}_report.txt
echo "DONE $NAME"
