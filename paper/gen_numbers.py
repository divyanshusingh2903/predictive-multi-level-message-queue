#!/usr/bin/env python3
"""Generate every number and table in the paper from committed result files.

Reads benchmarks/results/**/summary.json (and azure2021/predictability.json) and writes
generated/numbers.tex (macros) and generated/tables.tex (tables). Nothing in the
prose should be typed by hand if it can come from here.
"""
import json
import pathlib
import statistics

ROOT = pathlib.Path(__file__).resolve().parent.parent
RES = ROOT / "benchmarks" / "results"
OUT = pathlib.Path(__file__).resolve().parent / "generated"
OUT.mkdir(exist_ok=True)


def load(rel):
    return json.loads((RES / rel).read_text())


def pct(c, ci=True):
    """relative_delta -> '-30.8\\%' (with interval)."""
    s = f"{c['point'] * 100:+.1f}\\%".replace("-", "$-$").replace("+", "$+$")
    if ci and c.get("ci95"):
        lo, hi = (x * 100 for x in c["ci95"])
        s += f" [{lo:+.1f}, {hi:+.1f}]".replace("-", "$-$").replace("+", "$+$")
    return s


def ratio(c, ci=True):
    s = f"{c['point']:.2f}$\\times$"
    if ci and c.get("ci95"):
        s += f" [{c['ci95'][0]:.2f}, {c['ci95'][1]:.2f}]"
    return s


def verdict(c):
    return {"pass": "pass", "fail": "fail"}.get(c["status"], c["status"])


macros = []


def macro(name, value):
    macros.append(f"\\newcommand{{\\{name}}}{{{value}}}")


# ---- production flow (v2 local, E1 AWS, E3 brokers) -------------------------------------------
v2 = load("v2-prodflow/summary.json")["evaluation"]["criteria"]
e1 = load("v3-aws/e1/summary.json")["evaluation"]["criteria"]
e3 = load("v3-aws/e3/summary.json")["evaluation"]["criteria"]
for tag, c in (("VTwo", v2), ("EOne", e1)):
    macro(f"{tag}MeanVsFifo", pct(c["P1_mean_latency_vs_fifo"], ci=False))
    macro(f"{tag}ShortPFiftyVsFifo", pct(c["P2_short_class_p50_vs_fifo"], ci=False))
    macro(f"{tag}PNinetyNine", f"{c['G1_all_p99_vs_fifo']['point']:.2f}$\\times$")
    macro(f"{tag}VsTuned", pct(c["S1_vs_static_tuned_noninferior"], ci=False))
    macro(f"{tag}VsMisconf", pct(c["S2_beats_static_misconfigured"], ci=False))
    macro(f"{tag}ZeroConfig", pct(c["S4_zero_config_vs_fifo"], ci=False))
macro("EThreeVsRabbit", pct(e3["P1_mean_vs_rabbitmq_fifo"], ci=False))
macro("EThreeVsRedis", pct(e3["P3_mean_vs_redis_fifo"], ci=False))
macro("EThreeSidecar", pct(e3["S3_transfer_rabbitmq_predictive_vs_fifo"], ci=False))
macro("EThreeWeighted", pct(e3["S6_weighted_mean_vs_rabbitmq_fifo"], ci=False))
macro("EThreeFifoGap", pct(e3["S1_fifo_equivalence_rabbitmq"], ci=False))
macro("EThreeStaticLongWait", f"{e3['S8_rabbitmq_priority_long_wait_vs_fifo']['point']:.1f}$\\times$")

# ---- tables ------------------------------------------------------------------------------------
tables = []


def table(caption, label, header, rows, spec):
    body = "\n".join(" & ".join(r) + " \\\\" for r in rows)
    tables.append((label.split(":")[1],
        "\\begin{table}[t]\n\\centering\\small\n"
        f"\\caption{{{caption}}}\\label{{{label}}}\n"
        f"\\begin{{tabular}}{{{spec}}}\n\\toprule\n{' & '.join(header)} \\\\\n\\midrule\n{body}\n"
        "\\bottomrule\n\\end{tabular}\n\\end{table}\n"
    ))


rows = []
for key, name, fn, bar in [
    ("P1_mean_latency_vs_fifo", "P1 mean vs FIFO", pct, "$\\le -15\\%$"),
    ("P2_short_class_p50_vs_fifo", "P2 short-class P50 vs FIFO", pct, "$\\le -30\\%$"),
    ("G1_all_p99_vs_fifo", "G1 all-message P99 vs FIFO", ratio, "$\\le 1.25\\times$"),
    ("G2_long_class_max_wait", "G2 long-class max wait vs FIFO", ratio, "$\\le 2\\times$"),
    ("S1_vs_static_tuned_noninferior", "S1 vs hand-tuned static", pct, "$\\le +10\\%$"),
    ("S2_beats_static_misconfigured", "S2 vs misconfigured static", pct, "---"),
    ("S4_zero_config_vs_fifo", "S4 producer-only keys vs FIFO", pct, "$\\le -5\\%$"),
]:
    rows.append([name, bar, fn(v2[key]), verdict(v2[key]), fn(e1[key]), verdict(e1[key])])
table("Production-flow benchmark: local run (\\texttt{v2-prodflow-1}) and its replication on AWS "
      "(\\texttt{v3-cloud-e1-1}). Point estimates with 95\\% bootstrap intervals, paired over five seeds.",
      "tab:prodflow", ["Criterion", "Bar", "Local", "", "AWS", ""], rows, "llrcrc")

rows = []
for key, name, fn, bar in [
    ("P1_mean_vs_rabbitmq_fifo", "P1 mean vs RabbitMQ FIFO", pct, "$\\le -15\\%$"),
    ("P3_mean_vs_redis_fifo", "P3 mean vs Redis FIFO", pct, "$\\le -15\\%$"),
    ("P2_short_p50_vs_rabbitmq_fifo", "P2 short-class P50 vs RabbitMQ FIFO", pct, "$\\le -30\\%$"),
    ("G1_all_p99_vs_rabbitmq_fifo", "G1 all-message P99 vs RabbitMQ FIFO", ratio, "$\\le 1.25\\times$"),
    ("S1_fifo_equivalence_rabbitmq", "S1 Harbinger FIFO vs RabbitMQ FIFO", pct, "$\\pm 10\\%$"),
    ("S3_transfer_rabbitmq_predictive_vs_fifo", "S3 sidecar on RabbitMQ vs RabbitMQ FIFO", pct, "$\\le -15\\%$"),
    ("S6_weighted_mean_vs_rabbitmq_fifo", "S6 weighted aging vs RabbitMQ FIFO", pct, "$\\le -15\\%$"),
    ("S8_rabbitmq_priority_long_wait_vs_fifo", "S8 RabbitMQ priority, long-class max wait", ratio, "$\\le 2\\times$"),
]:
    c = e3[key]
    rows.append([name, bar, (ratio if c["kind"] == "ratio" else pct)(c), verdict(c)])
table("Harbinger against RabbitMQ and Redis on the same workload (\\texttt{v3-brokers-prodflow-1}, AWS).",
      "tab:brokers", ["Criterion", "Bar", "Estimate [95\\% CI]", ""], rows, "llrc")


def trace_rows(name, summ):
    rows = []
    for load_ in ("0.5", "0.8", "0.95"):
        ev = summ["evaluation"][load_]
        cr = ev["criteria"]
        a, b = cr["A_P1_mean_latency_vs_fifo"], cr["B_P1_mean_latency_vs_fifo"]
        g = ev["gates_passed"]
        rows.append([name, load_, pct(a, ci=False), "yes" if g["A_predictive_routing"] else "no",
                     pct(b, ci=False), "yes" if g["B_overload_design"] else "no"])
    return rows


rows = []
for name, rel in [("Azure 2021", "v3-traces/azure2021"), ("Huawei 2025", "v3-traces/huawei2025-r5"),
                  ("LPC-EGEE", "v3-traces-2/lpc-egee-2004"), ("DAS2-fs2", "v3-traces-2/das2-fs2-2003")]:
    rows += trace_rows(name, load(rel + "/summary.json"))
table("Trace simulations, mean latency against FIFO and whether each pre-registered gate passed. "
      "A: predictive routing as built. B: with worker-time weights and pausing aging.",
      "tab:traces", ["Trace", "Load", "A: mean", "A gate", "B: mean", "B gate"], rows, "lrrcrc")

pr = load("azure2021/predictability.json")["held_out"]
rows = [
    [n, f"{pr[k]['median_abs_log2_error']:.2f}", f"{pr[k]['within_2x'] * 100:.0f}\\%",
     f"{pr[k]['tier_accuracy'] * 100:.0f}\\%"]
    for n, k in [("Global median", "global_median"), ("Per-key median", "per_key_median_all_keys"),
                 ("Per-key median, with gates", "per_key_median_with_gates")]
]
table("Predicting held-out Azure Functions durations: median absolute $\\log_2$ error, share within a factor of 2, "
      "and tier accuracy.", "tab:predictability", ["Predictor", "Median $|\\log_2|$ error", "Within 2$\\times$", "Tier acc."],
      rows, "lrrr")
macro("AzureGlobalErrFactor", f"{2 ** pr['global_median']['median_abs_log2_error']:.0f}")
macro("AzurePerKeyErrFactor", f"{2 ** pr['per_key_median_all_keys']['median_abs_log2_error']:.1f}")


# ---- trace simulations, E2, E4, size bins ------------------------------------------------------
LOADS = {"0.5": "Half", "0.8": "Eight", "0.95": "NinetyFive"}
DATASETS = {"Azure": "v3-traces/azure2021", "Huawei": "v3-traces/huawei2025-r5",
            "LPC": "v3-traces-2/lpc-egee-2004", "DAS": "v3-traces-2/das2-fs2-2003"}
for ds, rel in DATASETS.items():
    ev = load(rel + "/summary.json")["evaluation"]
    for ld, word in LOADS.items():
        cr = ev[ld]["criteria"]
        macro(f"{ds}A{word}", pct(cr["A_P1_mean_latency_vs_fifo"], ci=False))
        macro(f"{ds}B{word}", pct(cr["B_P1_mean_latency_vs_fifo"], ci=False))
        macro(f"{ds}Oracle{word}", pct(cr["S5_oracle_vs_fifo"], ci=False))
        macro(f"{ds}OracleB{word}", pct(cr["S6_oracle_weights_pausing_vs_fifo"], ci=False))
        macro(f"{ds}BPNinetyNine{word}", f"{cr['B_G1_all_p99_vs_fifo']['point']:.2f}$\\times$")
        macro(f"{ds}BPNinetyNineUpper{word}", f"{cr['B_G1_all_p99_vs_fifo']['ci95'][1]:.1f}$\\times$")
        macro(f"{ds}BLongUpper{word}", f"{cr['B_G2_long_max_wait_vs_fifo']['ci95'][1]:.1f}$\\times$")
        macro(f"{ds}BVsA{word}", pct(cr["S7_weights_pausing_vs_predictive_warm"], ci=False))
        macro(f"{ds}ShortMedian{word}", pct(cr["S1_short_p50_vs_fifo"], ci=False))
lpc = json.loads((RES / "v3-traces-2/lpc-egee-2004/simulation.json").read_text())
oc = lpc["results"]["0.8"]["predictive_warm+weights+pausing"]["outcomes"]
tot = oc["predicted"] + oc["cold_key"] + oc["high_spread"] + oc["stale_key"]
macro("LPCJobs", f"{tot:,}".replace(",", "{,}"))
macro("LPCPredicted", f"{oc['predicted']:,}".replace(",", "{,}"))
macro("LPCPredictedShare", f"{oc['predicted'] / tot * 100:.0f}\\%")
macro("LPCStaleShare", f"{oc['stale_key'] / tot * 100:.0f}\\%")
macro("LPCStaleBacklogShare", f"{oc['backlog_stale'] / oc['stale_key'] * 100:.0f}\\%")
macro("LPCColdShare", f"{oc['cold_key'] / tot * 100:.0f}\\%")

e2 = load("v3-aws/e2/summary.json")["evaluation"]["criteria"]
for name, key in (("Mean", "C1_mean_vs_fifo"), ("Short", "C2_short_p50_vs_fifo"),
                  ("Shift", "C5_shifted_phase_mean_vs_fifo"), ("Tuned", "C6_vs_static_tuned")):
    d = e2[key]["deltas"]
    macro(f"ETwo{name}A", f"${d[0] * 100:+.1f}$\\%".replace("+", ""))
    macro(f"ETwo{name}B", f"${d[1] * 100:+.1f}$\\%".replace("+", ""))

e4 = load("v3-aws/e4/summary.json")["brokers"]
for b, tag in (("harbinger", "Harbinger"), ("rabbitmq", "Rabbit"), ("redis", "Redis")):
    macro(f"EFour{tag}Peak", f"{round(e4[b]['0.0']['delivery_rate_per_s'], -2):,.0f}".replace(",", "{,}"))

sb = load("v3-sizebins/summary.json")["evaluation"]["criteria"]
macro("SizeBinsMean", pct(sb["P1_mean_latency_vs_predictive"], ci=False))
macro("SizeBinsMeanCI", pct(sb["P1_mean_latency_vs_predictive"]))
macro("SizeBinsInvoice", pct(sb["S3_invoice_mean_vs_predictive"], ci=False))

(OUT / "numbers.tex").write_text("% generated by gen_numbers.py; do not edit\n" + "\n".join(macros) + "\n")
for name, text in tables:
    (OUT / f"tab_{name}.tex").write_text("% generated by gen_numbers.py; do not edit\n" + text)
print(f"wrote {len(macros)} macros and {len(tables)} tables")
