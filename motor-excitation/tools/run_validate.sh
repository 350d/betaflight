#!/bin/sh
# run_validate.sh NAME : erase flash, random-order tap prompts (one tap each), download, confusion matrix
set -e
NAME=$1; PORT=/dev/cu.usbmodem0x80000001; PY=.venv/bin/python
cd "$(dirname "$0")"
$PY bbl_download.py $PORT --erase
$PY tap_validate.py $PORT --per 5 --seed ${2:-1}
sleep 6
$PY bbl_download.py $PORT logs/${NAME}.bbl
$PY -W ignore tap_validate.py --report logs/${NAME}.bbl 2>&1 | grep -v "Header not found" | tee logs/${NAME}_report.txt
echo "DONE $NAME"
