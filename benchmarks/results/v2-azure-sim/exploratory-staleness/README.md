# Staleness during backlogs (issue #38, exploratory)

**Not pre-registered.** These runs diagnose why the predictor fell back so often in the Azure trace simulation. They
do not change the frozen `v2-azure-sim-1` result, and the latency numbers below are exploratory.

Everything here was produced by `harbinger_trace_sim` on the same trace (`txt_sha256` as in
`benchmarks/configs/v2-azure-sim.json`). The simulator now also reports, for the predictive arms, how many
predictions ended in each outcome, how many stale verdicts hit a key that still had work queued or running, and the
outcome split between "backlog" rows (the message itself waited over a minute, measured afterwards) and the rest.
Adding these counters did not change any latency result (`frozen-config-before-fix.json` matches the published
`simulation.json` exactly).

## Finding

Staleness ran from a key's last **completed** job. During a backlog lasting hours, a key whose jobs were stuck in the
queue got no completions, so its new messages were judged stale and sent to the default tier. The predictor switched
itself off for exactly the keys caught in the backlog.

Frozen configuration (aging on), `predictive_warm`, before the fix:

| Load | Stale share of all predictions | Stale share during backlog | Stale share otherwise | Stale verdicts with work outstanding |
|---|---|---|---|---|
| 0.50 | 9.2% | 47.6% | 0.59% | 98% |
| 0.80 | 8.5% | 37.6% | 0.52% | 98% |
| 0.95 | 8.3% | 33.1% | 0.51% | 98% |

In the evaluation window that contains the largest overload, only 14% of predictions were made from history (61% stale).

## Fix

A prediction for a key that is still fresh now counts as activity. Continuous arrivals keep the key's history even
when nothing completes. A key that is already stale stays stale until new evidence arrives: predicting it does not
revive it. After the fix (frozen configuration, `frozen-config-after-fix.json`), the stale share during backlogs falls
to 36.4% / 24.9% / 21.7%. The remainder is keys that had no traffic at all for longer than `stale_after` and then woke
up inside a backlog. Their history is old by the rule's own definition, so it is left alone.

With aging on, every policy still collapses to FIFO under these backlogs (#40), so latency does not change.

## Effect with aging disabled (exploratory)

Aging disabled (`config-no-aging.json`: threshold 10^15 ms), arms `fifo`, `predictive_warm`, `oracle`. Mean latency
relative to FIFO; the "before" row is the published `exploratory-no-aging.json`.

| Load | Before fix | After fix | `stale_after` off (`config-no-aging-no-stale.json`) | Oracle |
|---|---|---|---|---|
| 0.50 | −30.8% | −32.8% | −34.7% | −50.4% |
| 0.80 | −26.4% | −27.5% | −27.2% | −51.9% |
| 0.95 | −22.8% | −23.5% | −22.8% | −52.3% |

The fix helps slightly at every load, and long jobs' worst wait improves (1.14× → 1.12× FIFO at load 0.5). Turning
staleness off entirely is no better overall (better at 0.5, not at 0.8 or 0.95), so no "provisional" mode for keys
that went quiet is planned.

## Reproduce

```
harbinger_trace_sim TRACE.txt benchmarks/configs/v2-azure-sim.json frozen-config-after-fix.json
harbinger_trace_sim TRACE.txt config-no-aging.json no-aging-after-fix.json
harbinger_trace_sim TRACE.txt config-no-aging-no-stale.json no-aging-stale-off.json
```

`frozen-config-before-fix.json` was produced by the same simulator before the predictor change.
