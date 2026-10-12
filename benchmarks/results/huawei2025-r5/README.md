# Huawei Cloud 2025, Region 5: data checks (#47)

Descriptive checks of the second real trace, run before any scheduling arm is simulated on it. They inform the
`v3-traces` pre-registration (#46). No policy comparison is reported here.

## Source

- **Dataset:** Huawei Public Cloud Trace 2025 ([sir-lab/data-release](https://github.com/sir-lab/data-release),
  `README_data_release_2025.md`), CC BY 4.0. The request-level tables cover day 30.
- **Region:** 5. This is the smallest region and is released whole; Region 1 keeps only the top 100 pods per
  function, and Region 3 is a subsample.
- **Download:** `R5.zip` from the dataset's Google Drive link, 269,637,125 bytes. It holds 200 Spark part files,
  730 MB of CSV.
  - SHA-256 of `R5.zip`: `5825f74443cf6e4f5df264f8c94d439fc9eae576399f9786a50ca4cb3316e60d`.
- **Converted trace** (`python -m analysis.trace_format convert huawei2025 R5/ huawei2025-r5.csv`):
  - 3,443,079 rows and 491 keys, over 24.0 h, with a size on every row.
  - SHA-256: `e860c7a699670af200e09b8a6d97b194fd87ecc5a15833f039af88bc7a64656b`.
  - 5,180 of 3,448,259 source rows (0.15%) were dropped because they lack frontend fields.
  - Neither file is in git.

## Conversion decisions

- **Timestamps are completion times.** The dataset does not document this; the data show it.
  - `time_frontend` is 0–1 ms after `time_worker`, and that gap does not grow with the frontend's extra time, even
    at 1–100 s.
  - Consecutive requests on the same pod overlap 1.0% of the time if the stamps are end times. If they were start
    times, they would overlap 11.4%, and 23.7% where the two requests' durations differ by over 10×.
  - So arrival at the frontend is `time_frontend − totalCost_frontend`, as Azure 2021's arrival is `end − duration`.
- **Duration is `totalCost_worker`, not `runtimeCost`.**
  - `runtimeCost` is 0 on 40,953 rows (1.2%) whose `totalCost_worker` is set, about 1.1 s on average. 40,571 of
    them belong to one function, which always logs 0. This is a logging gap.
  - Where both are set, `totalCost_worker` is never smaller, and exceeds `runtimeCost` by 0.12 ms at the median (0.4%
    of the duration; 16% at p99).
  - Using `runtimeCost` would understate total work by 15%.
- **Key** is the publishers' funcID, `funcName---userID---poolName`. **Size** is `requestBodySize` in bytes.

## What the trace looks like

**Durations:** p50 26.4 ms, p90 37.4 ms, p99 971 ms, max 774 s.
- Azure 2021: p50 31 ms, p90 1.6 s, p99 72 s.

**One key dominates the count, not the work.**
- `296---224---pool22-300-128` is 82% of requests and 25% of work: p50 28 ms, p99 44 ms.
- The top 10 keys are 97% of requests.

**The work is in a heavy tail.**

| Duration | Requests | Work |
|---|---|---|
| < 10 ms | 7.8% | 0.1% |
| 10–100 ms | 89.7% | 27% |
| 100 ms – 1 s | 1.6% | 10% |
| ≥ 1 s | 0.9% | 63% |

The 30,263 requests of at least 1 s come from 251 keys. Two keys carry 70% of them: `49---128---pool22-300-128`
(the function that logs `runtimeCost` as 0) and `322---134---pool30-1800-2048`.

**Per-key medians predict well** (`predictability.json`; `trace_analysis.py`, 50/50 time split):

| Held-out prediction | Within 2× | Median \|log2 error\| | Tier accuracy |
|---|---|---|---|
| Global median | 83.6% | 0.39 | 28.6% |
| Per-key median | 97.7% | 0.24 | 48.8% |

- 97.6% of held-out requests have a routable key, and 0.08% have a key unseen in the first half.
- Tier accuracy is low because the global terciles (22.7 ms and 30.4 ms) cut through the dominant key's narrow band.
  The terciles separate requests that differ by a few milliseconds, not the long tail that holds the work.
- Azure 2021 for comparison: within 2× 62% (per key) vs 10% (global); boundaries 1 ms and 171 ms.

**The size hint carries no signal here.**
- Request bodies are nearly constant within a key (busiest key: 1,021 bytes median, 1,045 at p99).
- Spearman ρ between size and duration is between −0.03 and +0.04 in the five busiest keys.
- A size-binned arm on this trace would be a negative control at best.

**Load swings within the day.**
- Arrivals range from 504k per hour (early in the day) to 16k per hour (late).
- With the simulator's rules (`history_fraction` 0.5 by rows, 7 equal-row windows, arrivals compressed to a mean
  load), the history half covers only the first 5.0 h.
- At a mean load of 0.8 (64 workers), the evaluation windows' offered loads are 1.26, 1.70, 1.89, 1.47, 0.93, 0.66
  and 0.43. Four of the seven windows are in sustained overload.
- Azure 2021 under the same rules: 0.27, 0.44, 0.43, 0.28, 0.42, 0.53 and 3.6, with one extreme window.

## For the `v3-traces` pre-registration

1. **Tier boundaries.** With global terciles, most tier moves happen inside the dominant key. Decide before any run
   whether the registered arms keep the broker's tercile rule, as on Azure, or also test another boundary rule.
   Any other rule would be a new, separately registered arm.
2. **Load scaling.** A mean-load scaling puts most windows in overload, where aging behaviour dominates (`v3-aging`).
   Decide the loads and windows, for example by time instead of by row count, before running.
3. **History split.** A 50% row split leaves the first 5 hours as history. Decide whether that is acceptable or
   whether to split by time.
4. **Size arm.** Leave it out, or register it explicitly as a control expected to show nothing.
