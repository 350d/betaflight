# Motor thermal estimator: datasheet validation and high-load calibration (2026-10-21)

Firmware: betaflight/betaflight#15848, commit "Motor thermal: prop wash cooling, climb and descent airflow".
Data: [datasheets/motor_datasheets.json](datasheets/motor_datasheets.json) (169 motors, 146 with a published temperature, with source URLs).
Scripts: [datasheet_check/](datasheet_check/) (Python replica of `flight/motor_thermal.c`; it matches a host build of the firmware file within 0.3 °C on the bench sets).

## Why

The bench fit (camera on the windings, RESULTS.md) has no prop on the test motor and runs at 7-12k rpm and 1-5 W.
The first datasheet points with a prop and a temperature (GEPRC GR1408 3500KV, iFlight XING 1204 4500/6500KV,
full throttle, 43-62 °C published) came out at 185-255 °C in the model. A collection of published motor data then gave
220 usable full-throttle points from 9 brands (stator 7-31 mm, 1S-6S): the model's temperature rise over ambient was
**2.72x** the published one (median, IQR 1.25-4.50), up to 9x for some brands.

## Winding resistance: from the datasheet

- The reference resistance was a multimeter reading (0.4 Ω minus 0.2 Ω leads, ±0.1). The X-NOVA 2203.5 2800KV datasheet
  gives **0.070 Ω**; 2400KV 0.105 and 1800KV 0.19 Ω follow R ∝ 1/KV² within 10 %.
- With 0.070 Ω the bench runs of the reference motor fit better (RMSE 0.96-1.38 °C instead of 1.34-1.45, bias ~0) and
  the derived R of a 2207 1950KV is 0.072 Ω, in line with common datasheets.
- The size law underestimates R of small motors 1.6-2.1x (GEPRC 1408 98 mΩ, XING 1204 4500KV 172 mΩ, 6500KV 108 mΩ):
  `motor_resistance` (Configurator: Advanced motor settings) takes the datasheet "internal resistance". A multimeter
  reads too high on 5" and larger motors (lead resistance is comparable to the winding).

## Idle current: tried and dropped

The datasheet no-load current, as a ratio to the reference motor, made the bench fit of the 1408 worse (RMSE 1.5 -> 9.5 °C):
the model's no-load term is an effective quantity fitted on the bench (2.5-5x the physical no-load power), not watts.
Datasheets also quote it at different voltages (10 V or 5.5 V). No input for it.

## Prop wash: the missing cooling

The wash speed over the motor, v = n·D·0.252 (the induced velocity the model already used), adds forced convection on the
windings and the stator core, Newton cooling with the turbulent Nusselt exponent:

    extra conductance = (v / 1.0 m/s)^0.8 / R_core-air   (on both nodes, capped at 100 for the 10 ms step)

- The bench motor had no prop, so the bench fit (low throttle) is unchanged to the hundredth of a degree.
- The exponent is checked by the data: with 0.8 the model/datasheet rise ratio shows no trend with wash speed; steeper
  exponents do (each refitted on all points):

|---|---|---|---|---|
| 0-8 m/s | 19 | 1.04 | 1.56 | 0.06 |
| 8-12 m/s | 55 | 0.94 | 1.18 | 0.09 |
| 12-16 m/s | 66 | 0.85 | 0.84 | 0.12 |
| 16-25 m/s | 80 | 1.10 | 0.93 | 0.16 |

(vk refitted per exponent on all points: n 0.8: 0.97 m/s, n 2.0: 4.82 m/s, n 4.0: 0.69 m/s)

- The reference speed 1.0 m/s centres the 220 points. Per brand:

| brand | points | how the temperature was taken | rise ratio before | rise ratio now |
|---|---|---|---|---|
| T-Motor | 72 | motor surface, 1 min at 100 % | 1.40 (0.97-2.43) | 0.54 (0.39-0.83) |
| GEPRC | 33 | not stated / 3-15 s, one 10 min | 1.42 (1.11-2.04) | 0.74 (0.64-0.92) |
| RCinPower | 30 | 3-5 s at 100 % | 2.27 (1.26-3.21) | 1.33 (0.81-1.88) |
| VCI | 25 | shaft, after a 0-100 % sweep | 4.50 (4.01-5.89) | 1.24 (1.08-1.52) |
| iFlight | 24 | not stated (1 min on some) | 6.78 (5.74-8.40) | 1.71 (1.40-2.21) |
| BrotherHobby | 17 | inside copper wire, 10 s-2 min at 100 % | 5.05 (4.28-6.06) | 1.39 (1.07-1.98) |
| Axisflying | 8 | not stated | 8.66 (7.48-10.42) | 2.05 (1.80-2.36) |
| AMAX | 6 | 10 s at 100 % | 4.32 (4.07-4.81) | 2.03 (1.97-2.16) |
| Flywoo | 5 | 3 s at 100 % | 1.14 (1.01-1.15) | 0.70 (0.67-0.77) |
| **all** | **220** | | **2.72 (1.25-4.50)** | **0.99 (0.64-1.57)** |

  Leaving one brand out of the fit, its median stays within 0.47-2.09. What is left is mostly how each brand measures:
  T-Motor the case surface after 1 min (the model's stator core is compared), BrotherHobby the copper inside, VCI the
  shaft after a sweep, RCinPower 3-5 s, GEPRC / iFlight / Axisflying without a duration.

![model vs published](pr-assets/datasheet_validation_model_vs_published.png)

## Craft speed

Airflow from the craft: GPS ground speed (as before) and now the altitude rate of the position estimate (baro / GPS), so
climbs and descents count without GPS.

## Classic flight scenarios (model, ambient 25 °C, peak winding rise)

Inputs: per-motor current from the datasheet prop tables at full throttle and a typical hover current, DShot rpm,
command noise index 10-20 (filtered tune). Hover 3 min, cruise 3 min at 15 m/s, freestyle 4 min (3 s punch every 20 s),
race 2.5 min (70 % average, 25 m/s), full-throttle climb 20 s at 18 m/s then descent.

| motor | hover | cruise 15 m/s (GPS) | freestyle (punch 3 s / 20 s) | race (70 % avg, 25 m/s) | full-throttle climb 20 s, then descend |
|---|---|---|---|---|---|
| 1204 4500KV 3S 2.5" (XING, R 172 mOhm) | +17 K | +19 K | +50 K | +50 K | +56 K |
| 1408 3500KV 4S 3" (GEPRC, R 98 mOhm) | +13 K | +15 K | +38 K | +39 K | +44 K |
| 2203.5 2800KV 4S 4" (X-NOVA, R 70 mOhm) | +9 K | +10 K | +38 K | +43 K | +47 K |
| 2207 1950KV 6S 5" (R auto) | +11 K | +11 K | +59 K | +66 K | +73 K |
| 2806.5 1300KV 6S 7" (R auto) | +18 K | +20 K | +76 K | +91 K | +101 K |

## Limits

- Full throttle: unbiased over 220 points, but one point can be off by 2x (IQR 0.64-1.57); brands that measure the copper
  or report no duration sit 1.4-2x above, T-Motor's 1-min surface 0.54x below.
- Partial throttle with a prop has no ground truth yet: the datasheets give the temperature only at full throttle, the
  bench had no prop. The exponent comes from physics and the wash-speed check above, not from hover data.
- The no-load term is the bench-fitted effective one (above the physical no-load power).
- Cheapest flight check: hover 3 min, land, measure the motors within 10 s (IR thermometer or thermocouple), with
  `debug_mode = MOTOR_THERMAL` in the log.

## Reproduce

    python3 datasheet_check/dsdoc.py datasheets/motor_datasheets.json     # tables above
    python3 datasheet_check/dsplot.py datasheets/motor_datasheets.json out.png
