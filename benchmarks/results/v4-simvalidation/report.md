# Simulator validation (v4-simvalidation-1)

## Measured set: local  (seeds 11, 22, 33, 44, 55)

**Gate FAILED**: V1 False, V2 True, V3 True

### V1 effect against FIFO, mean latency

| Arm | Real | Simulator | Gap (points) | Pass |
|---|---|---|---|---|
| predictive | -25.6% | -37.0% | 11.4 | False |
| static_tuned | -16.4% | -26.3% | 9.9 | True |
| static_misconfigured | +6.6% | +10.5% | 3.9 | True |
| oracle | -25.1% | -37.9% | 12.9 | False |

### V2 predictive against FIFO, by phase

| Phase | Real | Simulator | Same sign |
|---|---|---|---|
| steady | -28.3% | -17.5% | True |
| ramp | -46.5% | -39.8% | True |
| flash_sale | -30.8% | -39.3% | True |
| recovery | -17.3% | -30.6% | True |
| shifted | -28.7% | -47.9% | True |

### V3 group order [['predictive', 'oracle'], ['static_tuned'], ['fifo'], ['static_misconfigured']]: holds in 5 of 5 simulated seeds (5 real)

- seed 11: real predictive < oracle < static_tuned < fifo < static_misconfigured (groups True); sim predictive < oracle < static_tuned < fifo < static_misconfigured (groups True)
- seed 22: real predictive < oracle < static_tuned < fifo < static_misconfigured (groups True); sim oracle < predictive < static_tuned < fifo < static_misconfigured (groups True)
- seed 33: real predictive < oracle < static_tuned < fifo < static_misconfigured (groups True); sim predictive < oracle < static_tuned < fifo < static_misconfigured (groups True)
- seed 44: real predictive < oracle < static_tuned < fifo < static_misconfigured (groups True); sim predictive < oracle < static_tuned < fifo < static_misconfigured (groups True)
- seed 55: real predictive < oracle < static_tuned < fifo < static_misconfigured (groups True); sim oracle < predictive < static_tuned < fifo < static_misconfigured (groups True)

### V4 absolute mean latency, simulator / real

| Arm | Ratio |
|---|---|
| fifo | 0.38 |
| predictive | 0.32 |
| static_tuned | 0.33 |
| static_misconfigured | 0.39 |
| oracle | 0.31 |

### V5 short-class P50 against FIFO

| Arm | Real | Simulator |
|---|---|---|
| predictive | -57.7% | -99.0% |
| static_tuned | -36.4% | -90.5% |
| static_misconfigured | +4.7% | +3.9% |
| oracle | -53.5% | -99.0% |

## Measured set: aws  (seeds 11, 22, 33, 44, 55)

**Gate FAILED**: V1 False, V2 True, V3 True

### V1 effect against FIFO, mean latency

| Arm | Real | Simulator | Gap (points) | Pass |
|---|---|---|---|---|
| predictive | -30.8% | -42.7% | 11.8 | False |
| static_tuned | -19.3% | -28.6% | 9.4 | True |
| static_misconfigured | +9.4% | +12.1% | 2.7 | True |
| oracle | -33.1% | -46.2% | 13.1 | False |

### V2 predictive against FIFO, by phase

| Phase | Real | Simulator | Same sign |
|---|---|---|---|
| steady | -19.5% | -9.0% | True |
| ramp | -47.6% | -32.1% | True |
| flash_sale | -33.9% | -44.6% | True |
| recovery | -23.3% | -38.0% | True |
| shifted | -52.8% | -28.1% | True |

### V3 group order [['predictive', 'oracle'], ['static_tuned'], ['fifo'], ['static_misconfigured']]: holds in 5 of 5 simulated seeds (5 real)

- seed 11: real oracle < predictive < static_tuned < fifo < static_misconfigured (groups True); sim oracle < predictive < static_tuned < fifo < static_misconfigured (groups True)
- seed 22: real oracle < predictive < static_tuned < fifo < static_misconfigured (groups True); sim oracle < predictive < static_tuned < fifo < static_misconfigured (groups True)
- seed 33: real predictive < oracle < static_tuned < fifo < static_misconfigured (groups True); sim predictive < oracle < static_tuned < fifo < static_misconfigured (groups True)
- seed 44: real oracle < predictive < static_tuned < fifo < static_misconfigured (groups True); sim oracle < predictive < static_tuned < fifo < static_misconfigured (groups True)
- seed 55: real oracle < predictive < static_tuned < fifo < static_misconfigured (groups True); sim oracle < predictive < static_tuned < fifo < static_misconfigured (groups True)

### V4 absolute mean latency, simulator / real

| Arm | Ratio |
|---|---|
| fifo | 0.40 |
| predictive | 0.33 |
| static_tuned | 0.36 |
| static_misconfigured | 0.41 |
| oracle | 0.33 |

### V5 short-class P50 against FIFO

| Arm | Real | Simulator |
|---|---|---|
| predictive | -94.8% | -91.1% |
| static_tuned | -83.6% | -88.2% |
| static_misconfigured | +13.5% | -41.6% |
| oracle | -97.6% | -91.8% |
