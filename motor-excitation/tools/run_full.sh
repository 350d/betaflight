#!/bin/sh
# run_full.sh NAME : erase flash, quick full test (taps + all motors at two speeds), download, report
set -e
NAME=$1; FULL_ARGS=${2:-}; PORT=/dev/cu.usbmodem0x80000001; PY=.venv/bin/python
cd "$(dirname "$0")"
$PY bbl_download.py $PORT --erase
$PY full_test.py $PORT $FULL_ARGS
echo "=== TEST DONE, downloading ==="
sleep 6
$PY bbl_download.py $PORT logs/${NAME}.bbl
$PY -W ignore full_report.py logs/${NAME}.bbl 2>&1 | grep -v "Header not found" | tee logs/${NAME}_report.txt
$PY -W ignore tap_classify.py logs/${NAME}.bbl 2>&1 | grep -v "Header not found" | tee -a logs/${NAME}_report.txt
echo "DONE $NAME"
