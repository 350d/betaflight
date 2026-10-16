#!/usr/bin/env python3
"""motor_id.py PORT [--beep | --jog] [--motors 1,2] : say a motor number, then spin that motor for 2 s at 1080 (default),
three 0.2 s nudges at 1060 (--jog), or beep it (--beep)."""
import struct, subprocess, sys, time
from msp import Msp
MSP2_SEND_DSHOT_COMMAND, DSHOT_CMD_BEACON3 = 0x3003, 3
m = Msp(sys.argv[1])
beep = "--beep" in sys.argv
jog = "--jog" in sys.argv
motors = [int(x) for x in sys.argv[sys.argv.index("--motors") + 1].split(",")] if "--motors" in sys.argv else [1, 2, 3, 4]
for motor in motors:
    subprocess.run(["say", "-v", "Milena", f"Мотор {motor}"])
    if beep:
        for _ in range(3):
            m.send2(MSP2_SEND_DSHOT_COMMAND, struct.pack("<BBBB", 0, motor - 1, 1, DSHOT_CMD_BEACON3))
            time.sleep(0.6)
    elif jog:
        for k in range(3):
            m.excite_start(970 + motor * 3 + k, 1 << (motor - 1), "none", 1060, duration=0.2, ramp=0, maxcmd=1100, timeout=0.5)
            time.sleep(0.35)
            m.excite_stop()
            time.sleep(0.4)
    else:
        session = 960 + motor
        m.excite_start(session, 1 << (motor - 1), "none", 1080, duration=2.0, ramp=0.3, maxcmd=1200, timeout=1.0)
        t = time.time()
        while time.time() - t < 2.6:
            m.excite_keepalive(session)
            time.sleep(0.2)
        m.excite_stop()
    time.sleep(1.5)
m.close()
