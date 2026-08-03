# QT Cluster (appCluster)

Mercedes EQ-style instrument cluster for the motor test rig. Displays live
motor telemetry (speed, power, currents, vibration) from the
`motor_data_producer` shared memory, plus AI verdicts (anomaly / fault class /
RUL) published by `motor_ai_client`.

## Build (QNX)

```sh
rm -rf build-qnx
source ~/qnx-rpi5/repo/qnx800/qnxsdp-env.sh
echo "$QNX_TARGET"      # should now point into ~/qnx-rpi5/repo/qnx800/target/qnx
which qcc               # should agree with $QNX_HOST

~/qt6-qnx/bin/qt-cmake -S . -B build-qnx -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DQNX_TARGET_ARCH=gcc_ntoaarch64le
cmake --build build-qnx
```

## Data pipeline

```
motor_data_producer ──SHM /motor_ctrl──→ SpiReader (thread) ──newBlock()──→ VehicleBackend ──QML──→ gauges
motor_ai_client     ──SHM /motor_ai_result──→ AiResultReader (thread) ──newResult()──→ VehicleBackend ──QML──→ status signs
```

- **SpiReader** mirrors `shm_region_t` from `motor_shm.h` (block ring, 16 slots
  × 200 rows @ 20 kHz) and emits one `MotorBlock` per producer block (~100 Hz).
  The mirrored layout is compile-time verified with `offsetof` static asserts.
  Keep it in sync with `motor_wire.h` / `motor_shm.h` (vendored copies) if the
  producer changes.
- **AiResultReader** mirrors `ai_result_shm.h` and emits only when the
  producer seq changes (results arrive once per 200-row AI window, ~2 s).

## Channel map (motor_wire.h v4, 8 ADC channels)

| Index | Signal |
|-------|--------|
| 0..2  | Phase currents A/B/C (raw counts) |
| 3..5  | Phase voltages A/B/C (raw counts) |
| 6     | DC-bus voltage |
| 7     | Speed voltage |

## Derived values

### Speed (km/h) — left gauge

```
v_kmh = (rpm * 2π * R * 3.6) / (60 * G)
```

- `rpm`: motor rpm from the STM32 tach input capture (last row of the block).
- `WHEEL_RADIUS_M` (default 0.33 m) and `GEAR_RATIO` (default 9.0) are
  vehicle constants — set once in `cluster.h` and tune to the rig.

### Power (kW) — right gauge

```
P_kW = (1 / (1000 * N)) * Σ (Va·Ia + Vb·Ib + Vc·Ic)   over the window
```

- Per-row instantaneous three-phase power, averaged over a sliding window of
  `POWER_WINDOW_SAMPLES = 1000` rows (~50 ms @ 20 kHz), recomputed per block.
- Scaling: `I = (raw − 2048) * AMPS_PER_COUNT` (0.05 A/count),
  `V = (raw − 2048) * VOLTS_PER_COUNT` (0.01 V/count default — tune to your
  voltage divider).

## Noise / human-eye handling

The raw stream is 100 Hz — faster than the eye can follow, and noisy:

1. **Window average** — the power window (1000 samples) averages out
   mains/motor ripple.
2. **EWMA smoothing** — `EWMA_ALPHA = 0.1` (~100 ms time constant) on rpm,
   km/h and kW.
3. **Emit throttle** — signals fire at most every `EMIT_MS = 100` ms and only
   when the smoothed value moved more than `SPEED_EPS_KMH` / `POWER_EPS_KW`.
4. **QML `SmoothedAnimation`** — needle glides instead of jumping.

All tunables live in `cluster.h`.

## AI status signs

`motor_ai_client` publishes per-window verdicts to `/motor_ai_result`:
`anomaly_result` ("normal" / "anomaly"), `fault_class_result`
("none" / "electrical" / "mechanical"), `pred_maint_result` (RUL string).

The center panel shows three EQ-styled signs:

- **NORMAL** (green) — lit when the verdict is normal / no fault.
- **ELECTRICAL** (amber) — lit when the fault class contains "electrical".
- **MECHANICAL** (red) — lit when the fault class contains "mechanical".

Active signs blink; the RUL string is shown beneath the row, and the footer
status text follows the live anomaly verdict.

## QML properties (Vehicle backend)

| Property | Meaning |
|----------|---------|
| `speed` / `speedRpm` | Smoothed km/h / rpm |
| `power` | Smoothed kW (windowed three-phase power) |
| `currents`, `currentPhaseA/B/C`, `currentMean`, `currentMax` | Scaled amps |
| `vibX/Y/Z`, `vibTotal` | MPU6050 in g (total includes ~1 g gravity) |
| `aiStatus` / `aiFaultClass` / `aiRul` | AI verdict strings |
| `aiNormal` / `aiElectrical` / `aiMechanical` | Sign state booleans |
