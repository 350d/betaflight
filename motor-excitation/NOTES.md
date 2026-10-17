# Motor + frame test: plan and status (2026-10-16)

Lab notes of the bench session, in the order the tests were run.

## Mounting stages (decided with the user)

All stages without props, same excitation protocol, same analysis.

1. **Table**: drone lying on the table. Running now. Cheap, but the table couples into the frame modes.
2. **In hand**: the user holds the drone by the battery, props off, low command. Firmware caps stay on
   (max command 1800 hard, per-session `--max`, keepalive timeout). Keep fingers and wires away from the bells.
3. **Suspended**: hung on strings / elastic, closest to free flight.

Then compare the three stages per motor and axis: peak frequency, Q, gain, coherence, and repeatability
over 2-3 runs each. Pick the most accurate (the one whose peaks repeat and least depend on the support),
then decide which is simplest for a normal user (expected: cable only, in hand or on the table).

## Per-stage protocol

- baseline per motor (`--wave none`, 1100 and 1200)
- chirp 20-900 Hz, amp 40, base 1200, 20 s, each motor alone (`--wave chirp`)
- repeat the chirp 2-3 times
- optional: sine at the identified peak, two amplitudes (linearity), white noise (out-of-sample check)

## Release architecture (user decision)

The firmware does the work and sends only results to the Configurator, not the whole log.
Candidate: on-board synchronous detection during the sweep: multiply raw gyro (3 axes) and motor rpm by
sin/cos of the excitation and accumulate per frequency step → gain, phase, coherence per point,
a few hundred bytes per sweep over MSP. The bench MSP blackbox stream is only for checking the theory.

## Status

- Firmware branch `feat/motor-excitation` (local, on top of #15848):
  `a9fd3fddc` excitation module, `b6923f713` debug state fix, `c824c79e7` blackbox device MSP.
- Bench 2: HAKRCF411D in the frame, 4 motors, ESCs flashed BLHeli_S 16.7 → Bluejay 0.21.0 C_H_30_48
  (backup + restore hex in `firmware/esc_backup_f411`), bidirectional DShot on, PID 4 kHz, log 1/2 (~1950 Hz).
- Table, chirp per motor (`logs/bj_chirp.bbl`, report `logs/bj_chirp_report*`):
  - coherent peak ~208-212 Hz on all motors, strongest on pitch (0.23 deg/s per unit): structure or table,
    not a motor property;
  - narrow lines at 170 / 340 Hz = rotation (10 400 rpm) and 2nd harmonic, not coherent with the input;
  - command → rpm 13.7-14.5 rpm per unit on all four motors, coherent up to ~130 Hz.
- Log over the USB cable (blackbox device MSP), F411:

  | setup | lost frames | max loop delay | pitch peak |
  |---|---|---|---|
  | flash, 4 kHz | 0 % | 0.39 ms | 207.9 Hz |
  | stream, 4 kHz, 1 KiB chunks | 1.5 % | 2.4 ms | 207.5 Hz |
  | stream, 2 kHz, 512 B chunks | 0.5 % | 1.4 ms | 206.8 Hz |

  One 1 KiB MSP reply holds the serial task ~2 ms on F411, longer than a 250 µs PID loop. With resampling by
  real timestamps (now in `bbl_report.py`) the stream gives the same peaks, but wider (bw 6.3 vs 4.1 Hz).
  Flash stays the default for theory checks.

## Stage 1 result: table, 3 repeats x 4 motors (logs/rep_table.bbl, logs/stage1_table_repeatability.txt)

Frequency resolution 1.9 Hz (1953 Hz log, 1024-point segments). 46 peaks repeat in 2-3 of 3 runs, 17 appear once.

| mode | where | frequency spread between repeats | gain spread |
|---|---|---|---|
| ~208-212 Hz, Q 30-50 | roll and yaw on all motors, pitch on motors 1-3 (strongest, 0.23-0.27 deg/s/unit) | <= 1.2 Hz | 2-7 % |
| ~242 Hz, broad (Q 6-20) | pitch, motors 2 and 4 only (front arms in quad-X) | <= 0.2 Hz | 3-7 % |
| ~124-130 Hz, Q 6-8 | yaw on all motors | <= 3 Hz | 2-5 % |
| command -> rpm | 13.7-14.4 rpm/unit, coherent to 124-151 Hz | - | <= 2 % |

Reading: the 210 Hz mode belongs to the frame/FC stack (every motor excites it); 242 Hz on pitch only from the
front motors points to the front of the frame; the broad, low-Q 125 Hz yaw mode is a candidate for the table
support and should move or vanish in stages 2 and 3.

## Stage 2 result: in hand, 3 repeats x 4 motors (logs/stage2_hand.bbl, logs/compare_table_hand.txt)

Repeatability as on the table: frequency spread <= 2 Hz, gain spread mostly < 10 %. Motor response unchanged
(13.7-14.3 rpm/unit), as expected without props.

| mode | table | in hand | reading |
|---|---|---|---|
| ~208-210 Hz | all motors, roll/yaw (+ pitch on 1-3) | all motors, all axes | same frequency on both supports: belongs to the craft (frame / FC stack) |
| front arms, pitch, motors 2 and 4 | 242 Hz, gain 0.10-0.13 | 267-268 Hz, gain 0.40 | the table loads the front arms; free, the mode is 26 Hz higher and 3x stronger |
| yaw ~125-130 Hz, Q 6-8, and 160-195 Hz yaw peaks | all motors | gone | table support |
| ~434-456 Hz | not seen | all motors, several axes | damped by the table contact, or a hand/battery mode: stage 3 decides |
| yaw at 19 Hz (band edge) | - | all motors, coherence 0.99 | rigid-body yaw from the motor reaction torque, not a resonance (excluded below 30 Hz) |

So far: the table hides or shifts modes, the hand frees them. Accuracy needs the suspended reference (stage 3).

## Stage 3 result and comparison (logs/stage3_hung.bbl, logs/compare_3stages.txt)

Suspended is the reference (closest to free flight). Repeatable peaks = seen in >= 2 of 3 runs, >= 30 Hz.

| stage | repeatable peaks | one-run peaks | freq sd mean | gain CV mean | suspended peaks reproduced | median freq error | median gain ratio | strongest mode per motor |
|---|---|---|---|---|---|---|---|---|
| table | 46 | 17 | 0.94 Hz | 3.7 % | 16 / 51 (31 %) | 1.9 Hz | 0.77 | 1 / 4 |
| in hand | 49 | 17 | 0.75 Hz | 6.3 % | 39 / 51 (76 %) | 0.7 Hz | 1.01 | 4 / 4 |
| suspended | 55 | 7 | 0.86 Hz | 4.6 % | reference | - | - | - |

- All three supports repeat equally well run to run, so repeatability alone does not tell which is right.
- The table is wrong where it matters: it moves the front-arm pitch mode 268 → 242 Hz, hides the 434-456 Hz
  group, and adds support modes (yaw 125-130 Hz, 160-195 Hz). It finds the strongest mode on only 1 of 4 motors.
- In hand matches the suspension: same frequencies within ~1 Hz, same gains, strongest mode on every motor.
- Strongest suspended mode per motor: m1 pitch 433 Hz, m2 pitch 267 Hz, m3 pitch 210 Hz, m4 pitch 269 Hz.
  Craft modes common to all motors: ~209 Hz (all axes), ~268 Hz (pitch, front motors strongest), ~434/456 Hz.
- Rigid-body yaw at the band edge: 19 Hz in hand, 18 Hz suspended (reaction torque on a free body), not a mode.
- Motor response is the same on all supports: 13.6-14.4 rpm/unit, coherent to 118-151 Hz.

Accuracy: suspended ≈ in hand >> table. Simplest for a user: in hand (nothing to build), but the handoff
safety note says not to suggest holding a running craft; a soft support (foam block, rubber bands) is the
user-facing candidate to test next against the suspension.

## Tap test 1 (logs/tap_hand1.bbl, logs/tap_hand1_report.txt)

Firmware `ccb924943`: RECORD session (motors stopped, blackbox MOTOR_TEST logs while the session runs), host
markers per arm. Log 1/1 (3910 Hz) to keep tap energy above 1 kHz from aliasing. Voice prompts by motor number
(`tap_test.py`, `run_tap.sh`), 5 taps per arm, analysis `tap_report.py` (free-decay spectra averaged over taps,
Q from the band envelope; self-test on synthetic modes recovers f and Q within 1 %).

| mode | tap | chirp, suspended |
|---|---|---|
| front arms pitch | 270 Hz, Q 33-35, strongest from arms 2 and 4 | 267-269 Hz, Q 39-40 |
| ~210 Hz | arm 3 roll/pitch, arm 4 pitch | all motors |
| yaw ~335 Hz | arms 1, 3, 4 | 337-339 Hz |
| 434-456 Hz group | not excited | all motors |
| 74 Hz yaw (Q ~27), 133 Hz pitch (Q ~9) | all arms | not seen |

- Structural modes confirmed by an excitation that bypasses motor and ESC: 210, 268-270, 335 Hz.
- 434-456 Hz is excited only through the motor: likely motor/bell-local.
- 74 / 133 Hz: candidates for the hold (battery grip) — check the mounting used.
- Q: tap envelope ~0.6-0.8x chirp; chirp Q near 100 is limited by the 1.9 Hz resolution, the envelope fit is the
  better estimate but beats of close modes (203/209 Hz) pull it down.
- Arm 1 window: 10 taps detected for 5 requested (bounces or extra taps).

## Tap tests 2-3: hand repeat and suspended (logs/tap_hand2*, logs/tap_hung1*, logs/tap_hung1_m4*)

The first tap test was held by the battery; tap_hand2 repeats it, tap_hung1 is the suspended run (battery
connected; the arm 4 window overlapped a Bluejay ESC idle beacon), tap_hung1_m4 repeats arm 4 suspended with the
battery disconnected (gyro on USB only; no ESC beacon possible).

| mode | tap in hand (2 runs) | tap suspended | chirp suspended | reading |
|---|---|---|---|---|
| 74-77 Hz, Q 15-30 | all arms, roll/pitch/yaw | all arms | not seen | craft mode (FC stack on grommets or battery on its strap); chirp without props has no force at low frequency, the tap does |
| 133 Hz pitch, Q 5-10 | all arms, both runs | gone from arm taps (weak 117-118 Hz on arms 3, 4) | not seen | centre mode coupled by the grip (see tap points below) |
| 267-271 Hz pitch, Q 26-36 | arms 1, 2, 4 | arms 2, 3, 4 | 267-269 Hz, Q 39-56 | confirmed structural (front arms) |
| 332-336 Hz yaw | arms 1, 3, 4 | arms 1, 3 | 337-339 Hz | confirmed |
| ~206-210 Hz | weak, arms 3, 4 | weak, arms 1, 4 | strongest chirp mode | excited mostly through the motor |
| 434-456 Hz | not excited | not excited | all motors | motor/bell-local |
| < 70 Hz comb, Q 4 | - | all arms | - | leakage of the suspension swing in the 0.25 s window, not modes |

- Beacon window vs battery-off repeat for arm 4: same 75-76 and 268 Hz; the beacon tone sits above the band and
  taps are found by the jump, so the analysis tolerated it. Battery off is still the clean way for tap tests.
- The two methods complement each other: chirp (motor-driven) finds what the motors excite, including the
  motor-local 434-456 Hz group and the strong 210 Hz mode; the tap finds low structural modes (75 Hz) that a
  propless motor cannot drive. A hand grip adds a false 133 Hz mode to the tap test but not to the chirp test.
- For a user procedure: chirp in hand matched the suspension (76 % of peaks, all strongest modes); the tap test
  needs no spinning motors at all but wants a free support, not a hand.

## Tap points on the suspension: battery, frame bottom under the stack (logs/tap_hung_points*)

Point 6 was tapped on the bottom of the frame under the FC stack (the report label said "top plate").

| mode | arm taps (suspended) | battery taps | frame bottom taps | reading |
|---|---|---|---|---|
| 74-77 Hz | all arms, strong | weak (roll) | absent | arm / whole-frame bending, not excited at the centre |
| 99-100 Hz pitch | - | Q 11 | Q 12 | centre mode (battery + stack) |
| 131-133 Hz | weak 117-118 Hz only | yaw, Q 5 | pitch, Q 17 | centre mode: exists without the hand; the battery grip made it visible from the arms |
| 212-224 Hz | (209 Hz arm mode) | pitch 212, roll/yaw 222-224 | roll 221 | centre mode next to the 209 Hz arm mode |
| 268-272 Hz pitch | arms 2-4 | - | Q 31-36 (frame bottom) | same mode as chirp 267-269 Hz |

Correction to the in-hand tap result: 133 Hz is not created by the hand; it is a centre mode the grip couples to the
arms. Rule "a mode seen in hand but not on the suspension is the grip" is therefore not safe; compare taps at the
same point, or use two different grips.

## Same tap points in hand vs suspended (logs/tap_hand_points*, logs/tap_hung_points*)

| mode | battery, hand | battery, suspended | frame bottom, hand | frame bottom, suspended |
|---|---|---|---|---|
| 95-100 Hz pitch | 95 Hz Q 5 | 99 Hz Q 11 | 95 Hz Q 8 | 100 Hz Q 12 |
| 130-134 Hz | 132 Hz Q 7 | 131 Hz Q 5 | 134 Hz Q 7 | 132 Hz Q 17 |
| 219-224 Hz | 219 Hz Q 20, weak | 224 Hz Q 31, strong | 220-222 Hz Q 8-23 | 221 Hz Q 16 |
| 272 Hz pitch | - | - | 273 Hz Q 30 | 272 Hz Q 31-36 |
| 56 / 86 / 90 Hz, Q 7-10 | yes | no | yes | no |

Grip signature: (1) extra low-Q (<= 10) modes below ~100 Hz, only when tapping near the grip; (2) centre modes
4-5 Hz lower and 2-3x more damped; (3) modes away from the grip unchanged (272 Hz within 1 Hz, same Q).
For filters (modes >= ~200 Hz) the hand grip is fine and a suspension is not needed; below ~150 Hz flag low-Q
peaks as "possibly grip" and expect centre modes a few percent low.

## Agreed user procedure (draft, 2026-10-16)

1. Battery disconnected, hold the craft by the battery.
2. Taps, 5 each, fixed order: battery → frame bottom under the stack → arms 1-4 (prompted by motor number).
   Battery taps give the grip signature, frame bottom the centre modes, arms the frame modes and per-arm comparison.
3. Battery connected, chirp per motor in the same grip (what the motors actually excite: 209 Hz, 434-456 Hz).

Default of `run_tap.sh` / `tap_test.py` is now this order (5,6,1,2,3,4). Not yet checked on another craft or by
another person.

## Which arm was tapped, from the gyro (tap_locate.py)

Direction of the roll/pitch rate impulse integrated over the first 4 ms after each tap (taps from above):

| point | in hand, run 1 | in hand, run 2 | suspended | impulse |
|---|---|---|---|---|
| arm 1 rear right | +133..+136 deg | +133..+135 | +131..+139 | 0.14-0.19 deg |
| arm 2 front right | -122..-127 | -122..-125 | -128..-134 | 0.14-0.25 |
| arm 3 rear left | +36..+38 | +36..+42 | +34..+40 | 0.16-0.23 |
| arm 4 front left | -50..-57 | -54..-58 | -35..-50 | 0.15-0.22 |
| battery / frame bottom | -79..-96 (hand), scattered (suspended) | | | 0.015-0.042 |

Four clusters ~90 deg apart, spread +-5 deg, independent of grip vs suspension; centre taps have a 4-10x smaller
rotational impulse. A wizard can accept taps in any order and label them, or check the requested arm. The direction
follows from the mixer motor positions and board alignment, so no calibration should be needed; taps must come from
above (a tap from below mirrors to the opposite arm).

## Automatic tap labelling (tap_classify.py)

Vectors only (no frequencies, no frame constants; decided with the user): arm = mixer motor whose (roll, pitch)
coefficients point closest to the 4 ms rotational impulse (quad-X expects +135, -135, +45, -45 deg for motors 1-4;
measured 131-139, -122..-134, 34-42, -35..-58); arm vs centre by the widest gap of the impulse magnitudes within the
session; centre point by the sign of the vertical acc impulse: < 0 = tapped from above (battery), > 0 = from below
(frame bottom), |acc z| < 0.03 g = unsure. (A frequency ratio also separated battery and frame bottom on this craft,
but it depends on the frame and is not used.)

Score on all tap logs: 85/87 (98 %). Misses: a weak extra tap (bounce) on arm 1, and a frame-bottom tap with
|acc z| ~ 0.01 g. A wizard should answer "not sure, tap again" when |acc z| < ~0.03 g or the impulse is near the
threshold. With this the tap phase needs no prompts: tap the six points in any order, from above for battery and
arms, from below for the frame bottom.

## Quick full test, free mode (logs/full_free2*, `run_full.sh NAME "--free 40"`)

Battery connected, held by the battery, ~2.5 min: 40 s free tap window (any order, labelled from the vectors),
then all four motors with independent noise (firmware `2beef11b8`), 15 s at base 1150 and 15 s at 1300.
(full_free1 lost the taps: the long spoken instruction blocked the keepalive and the FC closed the session;
fixed by keeping the session alive while speaking.)

- 57 taps found, labels arm 1/2/3/4 = 12/12/10/14, battery 3, frame bottom 6, 0 unsure (ground truth for free
  mode: ask the operator).
- Mode summary, automatic: frame (tap + motors) 207.5, 268, 330.7 Hz; frame (tap only) 74 Hz; centre 218.6 Hz;
  possibly grip 89 Hz (Q 5) and 133 Hz (Q 8, also near the 1150 rotation frequency 135 Hz);
  motor-driven only on both speeds 433-445, 505 Hz (+ 227, 257 Hz).
- Command → rpm: 11.0-11.3 rpm/unit at 1150, 12.6-13.0 at 1300; motors within 3 %.
- At base 1150 the 2nd rotation harmonic (270 Hz) lands on the 268 Hz frame mode; the second base separates them.
- The 422-450 Hz group sits 5-10 Hz lower at 1300 than at 1150: rpm dependent, consistent with a rotor/bell mode.

## Stage 4: kitchen sponge, chirp per motor x 3 (logs/stage4_sponge*, 2026-10-17)

| support | suspended peaks reproduced | median freq error | median gain ratio | extra peaks | strongest mode |
|---|---|---|---|---|---|
| table | 31 % | 1.9 Hz | 0.77 | 30 | 1/4 |
| held by the battery | 76 % | 0.7 Hz | 1.01 | 8 | 4/4 |
| sponge | 71 % | 1.3 Hz | 0.92 | 17 | 4/4 |

Same repeatability as the hand (freq sd 0.8 Hz, gain CV ~6 %). The front-arm mode moves 268 → 272-274 Hz and its
gain halves (0.18 vs 0.39): the sponge loads the front arms. Decision rule from the user (sponge becomes the main
support only if it beats the hand): not met, hand stays main, sponge is a fallback. Open: sponge under the body only,
arms free. On a sponge the frame-bottom tap is replaced by a tap on the tail from above (point 7).

## Stage 5: inverted, battery on the sponge, one pass (logs/stage5_sponge_inv*)

Single-pass comparison (repeat 1 only for every support, `score_single.py`), reference = repeatable suspended peaks:

| support | reproduced | median freq error | gain ratio | extra | front arms pitch |
|---|---|---|---|---|---|
| suspended, 1 pass vs 3 (ceiling) | 98 % | 0.4 Hz | 1.00 | 1 | 267-269 Hz, 0.37-0.38 |
| held by the battery | 80 % | 1.0 Hz | 0.98 | 10 | 267-269 Hz, 0.38-0.41 |
| inverted, battery on the sponge | 71 % | 1.5 Hz | 1.11 | 11 | 269 Hz, 0.25-0.26 |
| upright on the sponge | 69 % | 1.7 Hz | 0.92 | 19 | 272-273 Hz, 0.18 |
| table | 31 % | 2.0 Hz | 0.81 | 30 | - |

Inverted beats upright (front-arm mode back at 269 Hz, half the extra peaks) but stays below the hand. User rule:
hand stays the main support. Inverted-on-sponge is the hands-free option: nobody holds a craft with spinning
motors, at the cost of ~9 % of the peaks and 0.5 Hz. One pass only (spread of this metric between passes ~4 %).
(An idle ESC beacon after 10 min and a USB replug aborted the first attempt before any run.)

## Open

- all three stages done; next candidate: soft support (foam / rubber bands) as the user-facing method,
  scored against the suspension the same way
- on-board synchronous detection prototype, compare with the offline report
- thermal camera sees the whole drone: thermal runs per motor are possible on this bench too
