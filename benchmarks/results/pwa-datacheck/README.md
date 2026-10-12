# Parallel Workloads Archive: data checks (#46)

Descriptive checks of 16 logs from the [Parallel Workloads Archive](https://www.cs.huji.ac.il/labs/parallel/workload/)
(cite Feitelson, Tsafrir and Krakov, JPDC 2014), run before any scheduling arm was simulated on them. They choose the
two logs and the rules of the [`v3-traces-2`](../../../docs/v3-traces-2-preregistration.md) pre-registration.

## Source

- **Logs:** the 16 cleaned logs (or the only SWF version, for DAS2) that record users' runtime estimates. They were
  downloaded on 2026-10-12 through a browser, because the archive's firewall refuses scripted downloads.
- **Hashes:** in `SHA256SUMS-source-logs`. None of the files are in git.
- **Converter:** `python -m analysis.trace_format convert pwa`.
  - Arrival is the submit time, duration the run time, and size the user's requested run time.
  - Jobs with no positive run time or a missing key field are dropped and counted.

## Which logs record what

| Log | Jobs | Months | Users | Executable recorded | Distinct executables | Jobs completed | Single-processor jobs | Single-processor share of work | Median run |
|---|---|---|---|---|---|---|---|---|---|
| CEA-Curie-2011 | 312,826 | 8 | 582 | 0% | – | 90% | 18% | 0% | 161 s |
| CTC-SP2-1996 | 77,222 | 11 | 679 | 100% | 12,303 | 78% | 42% | 6% | 1,115 s |
| DAS2-fs1-2003 | 40,315 | 12 | 35 | 98% | 833 | 86% | 2% | 0% | 55 s |
| DAS2-fs2-2003 | 66,429 | 12 | 51 | 98% | 2,468 | 92% | 5% | 6% | 63 s |
| DAS2-fs3-2003 | 66,737 | 12 | 63 | 99% | 323 | 94% | 1% | 0% | 54 s |
| DAS2-fs4-2003 | 33,795 | 11 | 39 | 98% | 460 | 87% | 1% | 0% | 286 s |
| HPC2N-2002 | 202,871 | 42 | 257 | 0% | – | 100% | 42% | 11% | 2,873 s |
| KTH-SP2-1996 | 28,476 | 11 | 214 | 0% | – | 72% | 33% | 5% | 847 s |
| LANL-CM5-1994 | 122,060 | 24 | 213 | 100% | 3,958 | 83% | 0% | 0% | 414 s |
| LLNL-Atlas-2006 | 43,778 | 6 | 130 | 99% | 4,175 | 50% | 0% | 0% | 115 s |
| LLNL-Thunder-2007 | 121,039 | 5 | 283 | 100% | 17,002 | 86% | 0% | 0% | 58 s |
| LPC-EGEE-2004 | 234,889 | 8 | 55 | 0% | – | 88% | 94% | 100% | 14 s |
| SDSC-BLUE-2000 | 243,306 | 32 | 468 | 0% | – | 80% | 0% | 0% | 219 s |
| SDSC-DS-2004 | 96,069 | 12 | 460 | 30% | 3,862 | 69% | 0% | 0% | 654 s |
| SDSC-SP2-1998 | 59,715 | 24 | 437 | 100% | 53,308 | 72% | 21% | 1% | 359 s |
| Sandia-Ross-2001 | 57,882 | 37 | 203 | 100% | 10,441 | 20% | 14% | 0% | 1,769 s |

- **Most logs are parallel work.** Only LPC-EGEE, a grid cluster, is single-processor work, matching the simulator's
  one job per worker.
- **SDSC-SP2's executable numbers are almost unique per job,** so they cannot serve as a key.

## How predictable are the keys?

`analysis/trace_analysis.py`, 50/50 split by rows, as in the Huawei check. "User estimate" is the requested run time
compared with the actual run time over the same held-out half.

| Log | Key | Jobs kept | Keys | Global median within 2× | Per-key median within 2× | Held-out jobs from unseen keys | User estimate within 2× | User estimate median \|log2 error\| | Per-key median \|log2 error\| |
|---|---|---|---|---|---|---|---|---|---|
| LPC-EGEE-2004 | user + queue | 214,322 | 219 | 43% | 32% | 11% | 2% | 7.49 | 2.33 |
| LPC-EGEE-2004 | user | 214,322 | 54 | 43% | 24% | 4% | 2% | 7.49 | 3.71 |
| LANL-CM5-1994 | user + executable | 122,058 | 4,577 | 16% | 28% | 56% | 43% | 1.32 | 2.27 |
| DAS2-fs3-2003 | user + executable | 65,618 | 385 | 55% | 59% | 84% | 2% | 3.47 | 0.27 |
| DAS2-fs2-2003 | user + executable | 64,660 | 2,516 | 73% | 71% | 8% | 4% | 3.45 | 0.51 |
| DAS2-fs1-2003 | user + executable | 38,294 | 867 | 52% | 66% | 11% | 4% | 3.45 | 0.73 |
| DAS2-fs4-2003 | user + executable | 32,626 | 489 | 6% | 9% | 92% | 9% | 3.45 | 2.45 |
| CTC-SP2-1996 | user + executable | 77,205 | 13,169 | 11% | 20% | 68% | 36% | 1.87 | 2.96 |
| LLNL-Thunder-2007 | user + executable | 113,588 | 17,238 | 31% | 42% | 51% | 7% | 8.81 | 1.43 |
| Sandia-Ross-2001 | user + executable | 55,605 | 10,727 | 8% | 15% | 76% | 26% | 2.47 | 4.16 |
| LLNL-Atlas-2006 | user + executable | 36,222 | 3,727 | 13% | 20% | 61% | 25% | 4.62 | 3.52 |

Huawei 2025 for comparison: per-key 98% within 2×, global 84%, 0.08% unseen keys.

- **Keys churn.** Over months users run new executables, so on most logs half to nine-tenths of later jobs come from
  keys never seen before. Serverless functions recur; HPC executables do not.
- **Where keys recur (DAS2-fs1, fs2), they predict little beyond a global median.**
- **LPC-EGEE's per-user keys predict worse than a global median.** Per-user durations drift between the two halves.
- **Users' own estimates are worse still on most logs.** Most requests are near the queue's limit: typical errors are
  1.3–8.8 octaves, and 2–43% fall within 2×. Per-key medians beat them except on LANL-CM5, CTC-SP2 and Sandia-Ross.

## Choice for `v3-traces-2`

Two logs, as a "limits" experiment on job queues that do not recur like serverless functions.

| Log | Key | Jobs | Why |
|---|---|---|---|
| **LPC-EGEE-2004** | user + queue | single-processor only | The one log that fits the simulator's model exactly; weak keys. |
| **DAS2-fs2-2003** | user + executable | all; a parallel job occupies one worker for its run time | The log whose keys recur best (8% unseen); a stated simplification. |

**Class thresholds:** short < 1 min ≤ medium < 10 min ≤ long, for both logs.

| Log | Long jobs: share of jobs | Long jobs: share of work | Long jobs per evaluation window |
|---|---|---|---|
| LPC-EGEE | 23.5% | 99% | ≥ 1,219 |
| DAS2-fs2 | 6.4% | 88% | ≥ 60 |

A 1-hour threshold would leave DAS2 windows with 2–13 long jobs, below the 30 required.

**Converted traces** (in `lpc-egee-2004.summary.json`, `das2-fs2-2003.summary.json`):

| Trace | Jobs | Keys | SHA-256 |
|---|---|---|---|
| LPC-EGEE | 214,322 | 219 | `f360ce21…755a1` |
| DAS2-fs2 | 64,660 | 2,516 | `f410b083…df135` |

**Window loads at mean 0.8** (time split, 7 equal-time windows, 64 workers):

| Log | Window offered loads |
|---|---|
| LPC-EGEE | 0.61, 0.44, 0.84, 0.82, 1.06, 1.48, 0.35 |
| DAS2-fs2 | 0.10, 2.01, 1.32, 1.17, 0.55, 0.23, 0.22 |
