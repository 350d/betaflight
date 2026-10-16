#!/usr/bin/env python3
"""Download the onboard dataflash blackbox over MSP.
  bbl_download.py PORT out.bbl [--resume] download the used part (--resume: continue an interrupted file)
  bbl_download.py PORT --erase            erase the flash (waits until done)
"""
import struct, sys, time
from msp import Msp

MSP_DATAFLASH_SUMMARY, MSP_DATAFLASH_READ, MSP_DATAFLASH_ERASE = 70, 71, 72

def summary(m):
    p = m.query2(MSP_DATAFLASH_SUMMARY)
    flags, sectors, total, used = struct.unpack_from("<BIII", p, 0)
    return {"ready": bool(flags & 1), "supported": bool(flags & 2), "sectors": sectors, "total": total, "used": used}

def main():
    m = Msp(sys.argv[1], timeout=0.5)
    if sys.argv[2] == "--erase":
        m.query2(MSP_DATAFLASH_ERASE)
        t0 = time.time()
        while not summary(m)["ready"]:
            time.sleep(1)
        print(f"erased in {time.time() - t0:.0f} s", summary(m)); return
    s = summary(m); print(s)
    import os
    resume = "--resume" in sys.argv and os.path.exists(sys.argv[2])
    out = open(sys.argv[2], "ab" if resume else "wb")
    addr = out.tell() if resume else 0
    if resume:
        print("resuming at", addr)
    chunk = 4096; t0 = time.time(); start = addr; fails = 0
    while addr < s["used"]:
        n = min(chunk, s["used"] - addr)
        try:
            p = m.query2(MSP_DATAFLASH_READ, struct.pack("<IHB", addr, n, 0), deadline=1.0)
        except Exception as e:
            print("read error:", e); p = None
        if not p:
            fails += 1
            print("retry at", addr, "fail", fails)
            if fails % 5 == 0:   # a stuck port does not recover by retrying: reopen it
                try:
                    m.close()
                except Exception:
                    pass
                time.sleep(1.0)
                m = Msp(sys.argv[1], timeout=0.5)
            if fails > 30:
                out.close(); sys.exit(f"giving up at {addr}; rerun with --resume")
            continue
        fails = 0
        raddr, size, comp = struct.unpack_from("<IHB", p, 0)
        data = p[7:7 + size]
        if raddr != addr or comp or not size:
            print("unexpected chunk", raddr, size, comp); continue
        out.write(data); addr += len(data)
        if len(data) < n:
            chunk = len(data)   # FC capped the reply size
    out.close()
    dt = time.time() - t0
    print(f"{addr} bytes ({addr - start} this run) in {dt:.1f} s ({(addr - start) / dt / 1024:.0f} KiB/s)")

main()
