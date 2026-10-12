# Real traces under common rules (`v3-traces-1`, issue #46)

Pre-registered in [docs/v3-traces-preregistration.md](../../../docs/v3-traces-preregistration.md), frozen in commit
`52020be` before any scheduling arm was simulated on the Huawei trace, and before any Azure run under these rules.

| | |
|---|---|
| Simulator | `harbinger_trace_sim` built from `52020be` (binary SHA-256 `b272b6d4…09d5`) |
| Inputs | SHA-256 matched the configs: Huawei `e860c7a6…656b`, Azure `07e6c5c1…d33c` |
| Run time | 21 s (Huawei) and 22 s (Azure) on one machine |

**Run note.** The first Azure run wrote nowhere: a shell variable was unset, and the simulator does not report a
failed write. It was re-run with the correct path. The simulation is deterministic, and no output from the lost run
was seen.

## Result

| Gate | Huawei 0.5 | Huawei 0.8 | Huawei 0.95 | Azure 0.5 | Azure 0.8 | Azure 0.95 |
|---|---|---|---|---|---|---|
| **A**: predictive routing as built (strict priority, today's aging) | not passed | **passed** | not passed | not passed | not passed | not passed |
| **B**: predictive routing + weights `[8, 3, 1]` + pausing aging | not passed | **passed** | **passed** | **passed** | **passed** | **passed** |

### Mean latency vs FIFO (P1: ≤ −15% with interval below 0)

| Arm | Huawei 0.5 | Huawei 0.8 | Huawei 0.95 | Azure 0.5 | Azure 0.8 | Azure 0.95 |
|---|---|---|---|---|---|---|
| A `predictive_warm` | −4.8% | **−15.0%** [−27.0, −4.2] | −7.9% | −0.7% | −0.9% | −1.2% |
| B `predictive_warm+weights+pausing` | −4.3% | **−46.5%** [−78.3, −13.3] | **−49.9%** [−77.4, −21.1] | **−15.4%** [−25.8, −5.4] | **−22.8%** [−37.2, −9.3] | **−25.3%** [−40.3, −10.6] |
| `oracle` (strict, today's aging) | −3.2% | −17.5% | −11.8% | −1.2% | −1.6% | −1.8% |
| `oracle+weights+pausing` | −3.2% | −37.8% | −50.0% | −26.5% | −33.3% | −36.7% |

**Gate A at Huawei 0.8 is borderline.** The point estimate is −15.01% against a −15% bar.

### Guardrails, primary arms vs FIFO (G1 P99 ≤ 2×; G2 long-job max wait ≤ 2×, upper bounds)

| | Huawei 0.5 | Huawei 0.8 | Huawei 0.95 | Azure 0.5 | Azure 0.8 | Azure 0.95 |
|---|---|---|---|---|---|---|
| A, P99 | 0.98× | 0.97× | 0.98× | 1.00× | 1.00× | 1.00× |
| A, long max wait | 1.12× [≤1.37] | 1.03× | 1.01× | 1.00× | 1.00× | 1.00× |
| B, P99 | 0.98× | 1.05× [≤1.16] | 1.06× [≤1.12] | 1.00× | 1.06× [≤1.14] | 1.10× [≤1.19] |
| B, long max wait | 1.11× [≤1.34] | 1.16× [≤1.28] | 1.08× [≤1.16] | 1.08× [≤1.13] | 1.11× [≤1.23] | 1.14× [≤1.27] |

Every guardrail passed everywhere. Every window had at least 348 long jobs (≥ 1 s), so no G2 was unevaluable.

Full tables, secondary criteria and the without-extreme-window view are in `huawei2025-r5/report.md` and
`azure2021/report.md`. The views without the extreme window give the same gate outcomes.

## What the results show

**The overload design generalises; predictive routing alone mostly does not.**
- With strict priority and today's 5 s aging, predictive routing beat FIFO by ≥ 15% only once, and only just: Huawei
  at 0.8.
- With worker-time weights and pausing aging, the same predictor passed 5 of 6 cells. It cut mean latency 15–25% on
  Azure and 47–50% on Huawei at 0.8–0.95.
- This repeats `v3-aging-1` on a second provider. Under backlogs, today's aging lifts everything to the top level and
  erases the predictor's ordering (S7: B vs A, −41% on Huawei at 0.8, −22% on Azure at 0.8).

**Where the gain comes from (Huawei, load 0.8, mean latency per window in seconds):**

| Arm | W1 | W2 | W3 | W4 | W5 | W6 | W7 |
|---|---|---|---|---|---|---|---|
| FIFO | 27.6 | 46.8 | 6.5 | 0.21 | 0.24 | 0.20 | 3.9 |
| A | 22.4 | 41.9 | 4.1 | 0.21 | 0.24 | 0.20 | 2.4 |
| B | 2.5 | 3.4 | 0.6 | 0.21 | 0.24 | 0.20 | 1.9 |

- Queueing happens in the busy windows (1–3 and 7). In windows 4–6 every arm is at the service time, about 0.2 s, and
  nothing can be gained.
- At 0.5 load most windows are like that, so gate A and gate B both fail there. Their −4% to −5% is real but small.

**Short jobs win, long jobs pay a little.** On Huawei at 0.8, B moved:

| Measure | FIFO | B |
|---|---|---|
| Short-job median | 8.0 s | 54 ms |
| Long-job mean | 30.4 s | 35.5 s (+17%) |
| Long-job max wait | | 1.16× FIFO |

This is the shortest-job-first trade-off the project's metrics were chosen for. All-message P99 stays within 1.10×.

**A hand-tuned static map does as well as the predictor here.**
- `static_history` gives each key its tier from the history half's medians. It matched or beat `predictive_warm` on
  Huawei at 0.8 and 0.95 (S2 non-inferiority failed: +10.6% and +6.4%).
- Huawei's keys are stable within a day, so a correct map set by an operator works. The predictor's case is that it
  needs no map: it beat a random map (S3) at five of six cells, and it starts from empty within a few percent (S4).

**The strict-priority oracle is no better than the predictor.**
- On Huawei it gained 17.5% at 0.8, against the predictor's 15.0%; on Azure 1–2%.
- Under today's aging, better predictions would not help. The ordering is lost to aging, not to prediction error.

**With weights, the predictor beats the oracle on Huawei** (1.8 s vs 6.6 s mean at 0.8).
- The oracle attaches no predicted duration, so its worker-time charges fall back to level averages, as stated in the
  `v3-aging` pre-registration. The predictor charges each message its key's estimate.
- This compares charging rules, not prediction quality. It should not be read as "prediction beats perfect
  knowledge".

**Azure is dominated by one backlog.**
- Window 6 was offered 3.46× capacity at mean load 0.8. Its backlog carries into window 7, so FIFO's mean latency is
  2.4 h in window 6 and 9.3 h in window 7, against 6–61 s elsewhere.
- B's gains come from windows 2, 4, 5 and 7. Window 6 is unchanged or slightly worse.

## Against the stated expectations

| Expectation | Outcome |
|---|---|
| Huawei A_P1 passes at 0.8 and 0.95; uncertain at 0.5 | Passed at 0.8 (borderline); failed at 0.95 (−7.9%) and 0.5 |
| Huawei overload window may collapse under today's aging, favouring B | Yes: windows 1–2 |
| Huawei G2 passes; 0.95 is the risk | Passed at every load (≤ 1.16× upper bound at 0.95) |
| Huawei oracle may be no better than the predictor | Yes (strict: −17.5% vs −15.0% at 0.8) |
| Azure A fails P1 | Yes, at every load |
| Azure B uncertain | Passed at every load |

## Files

- `<trace>/simulation.json`: simulator output.
  - SHA-256 Huawei `69cd87c4dda50ea0d1149e2cdc979dfc8c735bb7cb4fa7f0166757f11ee9e4fe`.
  - SHA-256 Azure `e093e90c780666d7a7460415c229c090ceacc61008ce93a81d0e40604ea2e155`.
- `<trace>/report.md`, `<trace>/summary.json`: output of `python -m benchmarks.sim_report CONFIG SIMULATION.json`.

Reproduce: convert the traces as in the configs, then run
`harbinger_trace_sim TRACE.csv benchmarks/configs/v3-traces-<trace>.json OUT.json` and the report command.
