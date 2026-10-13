#!/usr/bin/env python3
"""Generate every number and table in the paper from committed result files.

Reads benchmarks/results/**/summary.json (and azure2021/predictability.json) and writes
generated/numbers.tex (macros) and generated/tables.tex (tables). Nothing in the
prose should be typed by hand if it can come from here.
"""
import json
import re
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
    v = c["point"] * 100
    s = "$0.0$\\%" if abs(v) < 0.05 else f"{v:+.1f}\\%".replace("-", "$-$").replace("+", "$+$")
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
    return {"pass": "\\hbpass{}", "fail": "\\hbfail{}"}.get(c["status"], c["status"])


macros = []


def macro(name, value):
    macros.append(f"\\newcommand{{\\{name}}}{{{value}}}")
    # percentage macros also get a magnitude form (NameAbs) for sentences that already say "reduced by" or "lower".
    m = re.fullmatch(r"\$[-+]\$(\d+\.\d)\\%", value) or re.fullmatch(r"\$[-+](\d+\.\d)\$\\%", value)
    if m:
        macros.append(f"\\newcommand{{\\{name}Abs}}{{{m.group(1)}\\%}}")


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


def hb(cell):
    """Bold a plain header cell; leave \\multicolumn cells to the caller."""
    return cell if cell.startswith("\\multicolumn") or not cell else f"\\textbf{{{cell}}}"


def table(caption, label, header, rows, spec, rules=(), size="small"):
    """header: list of header rows (each a list of cells). rules: cmidrule strings after the first header row."""
    if header and isinstance(header[0], str):
        header = [header]
    head_rows = []
    for i, hr in enumerate(header):
        line = "\\rowcolor{hbslate!14} " + " & ".join(hb(c) for c in hr) + " \\\\"
        if i == 0 and rules:
            line += "\n" + "".join(rules)
        head_rows.append(line)
    body = "\n".join(" & ".join(r) + " \\\\" for r in rows)
    env = "tabularx" if "L" in spec else "tabular"
    width = "{\\textwidth}" if env == "tabularx" else ""
    tables.append((label.split(":")[1],
        f"\\begin{{table}}[t]\n\\centering\\{size}\n"
        f"\\caption{{{caption}}}\\label{{{label}}}\n"
        f"\\begin{{{env}}}{width}{{{spec}}}\n\\toprule\n" + "\n".join(head_rows) + f"\n\\midrule\n{body}\n"
        f"\\bottomrule\n\\end{{{env}}}\n\\end{{table}}\n"
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
      "tab:prodflow", [["Criterion", "Bar", "\\multicolumn{2}{c}{\\textbf{Local run}}", "\\multicolumn{2}{c}{\\textbf{AWS replication}}"]],
      rows, "Llrcrc", rules=("\\cmidrule(lr){3-4}", "\\cmidrule(lr){5-6}"), size="footnotesize")

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
      "tab:brokers", ["Criterion", "Bar", "Estimate [95\\% CI]", "Result"], rows, "Llrc", size="footnotesize")


def trace_rows(name, summ):
    rows = []
    for load_ in ("0.5", "0.8", "0.95"):
        ev = summ["evaluation"][load_]
        cr = ev["criteria"]
        a, b = cr["A_P1_mean_latency_vs_fifo"], cr["B_P1_mean_latency_vs_fifo"]
        g = ev["gates_passed"]
        rows.append([name, load_, pct(a, ci=False), "\\hbyes{}" if g["A_predictive_routing"] else "\\hbno{}",
                     pct(b, ci=False), "\\hbyes{}" if g["B_overload_design"] else "\\hbno{}"])
    return rows


rows = []
for name, rel in [("Azure 2021", "v3-traces/azure2021"), ("Huawei 2025", "v3-traces/huawei2025-r5"),
                  ("LPC-EGEE", "v3-traces-2/lpc-egee-2004"), ("DAS2-fs2", "v3-traces-2/das2-fs2-2003")]:
    rows += trace_rows(name, load(rel + "/summary.json"))
table("Trace simulations, mean latency against FIFO and whether each pre-registered gate passed. "
      "A: predictive routing as built. B: with worker-time weights and pausing aging.",
      "tab:traces", [["", "", "\\multicolumn{2}{c}{\\textbf{Gate A: strict priority}}", "\\multicolumn{2}{c}{\\textbf{Gate B: weights, pausing aging}}"],
                     ["Trace", "Load", "Mean vs FIFO", "Passed", "Mean vs FIFO", "Passed"]],
      rows, "llrcrc", rules=("\\cmidrule(lr){3-4}", "\\cmidrule(lr){5-6}"))

pr = load("azure2021/predictability.json")["held_out"]
rows = [
    [n, f"{pr[k]['median_abs_log2_error']:.2f}", f"{pr[k]['within_2x'] * 100:.0f}\\%",
     f"{pr[k]['tier_accuracy'] * 100:.0f}\\%"]
    for n, k in [("Global median", "global_median"), ("Per-key median", "per_key_median_all_keys"),
                 ("Per-key median, with gates", "per_key_median_with_gates")]
]
table("Predicting held-out Azure Functions durations: median absolute $\\log_2$ error, share within a factor of 2, "
      "and tier accuracy.", "tab:predictability", ["Predictor", "Median $|\\log_2|$ error", "Within 2$\\times$", "Tier acc."],
      rows, "Lrrr")
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


# ---- design-section evidence: aging, feature signal, v2 trace sim -----------------------------
ag = load("v3-aging/sim/summary.json")["evaluation"]
for ld, word in LOADS.items():
    macro(f"AgingSim{word}", pct(ag[ld]["criteria"]["P1_mean_latency_vs_fifo"], ci=False))
    macro(f"AgingSimVsToday{word}", pct(ag[ld]["criteria"]["S2_vs_todays_aging"], ci=False))
agp = load("v3-aging/prodflow/summary.json")["evaluation"]["criteria"]
macro("AgingProdMeanVsPred", pct(agp["S1_mean_vs_predictive"], ci=False))
macro("AgingProdPNinetyNine", f"{(agp['G2_all_p99_vs_predictive']['point'] - 1) * 100:.0f}\\%")
macro("AgingProdLongWait", f"{agp['G1_long_class_max_wait_vs_fifo']['point']:.2f}$\\times$")
v2az = load("v2-azure-sim/summary.json")["evaluation"]
macro("VTwoAzurePassed", str(sum(1 for l in v2az if v2az[l].get("gate_passed"))))
fs = load("feature-signal-v1/analysis.json")["runs"]
def avg(model, key):
    vals = [r["models"][model][key] for r in fs.values()]
    return sum(vals) / len(vals)
macro("FSPerKeyFactor", f"{2 ** avg('per_key_median', 'median_abs_log2_error'):.1f}")
macro("FSBinnedFactor", f"{2 ** avg('per_key_size_bin_median', 'median_abs_log2_error'):.1f}")
macro("FSPerKeyTier", f"{avg('per_key_median', 'tier_accuracy') * 100:.0f}\\%")
macro("FSBinnedTier", f"{avg('per_key_size_bin_median', 'tier_accuracy') * 100:.0f}\\%")
macro("FSBoostTier", f"{avg('gradient_boosting', 'tier_accuracy') * 100:.0f}\\%")


# ---- queueing illustration (Cobham / Kleinrock), computed here, not measured -----------------
def two_class(p_short, size_ratio, rho):
    """Closed forms for non-preemptive priority M/G/1, short class first.
    p_short: share of jobs that are short; size_ratio: mean long / mean short size; rho: total load."""
    pi = p_short / (p_short + (1 - p_short) * size_ratio)   # short class's share of the work
    rho_s = pi * rho
    short_vs_fifo = (1 - rho) / (1 - rho_s)                  # W_s / W_fifo
    long_vs_fifo = 1 / (1 - rho_s)                           # W_l / W_fifo
    mean_vs_fifo = (1 - p_short * rho) / (1 - rho_s)         # mean wait / W_fifo
    return pi, short_vs_fifo, long_vs_fifo, mean_vs_fifo

rows = []
for p_short, ratio in ((0.9, 10), (0.9, 100), (0.5, 100)):
    for rho in (0.5, 0.8, 0.95):
        pi, ws, wl, wm = two_class(p_short, ratio, rho)
        rows.append([f"{p_short:.1f}", f"{ratio}", f"{rho}", f"{pi * 100:.0f}\\%",
                     f"{ws:.2f}", f"{wl:.2f}", f"{wm:.2f}"])
table("Mean waiting time under non-preemptive priority, relative to FIFO, from the closed forms of "
      "Section~\\ref{sec:analysis}. An illustration of the model, not a measurement. Short-job share $p$, "
      "long-to-short size ratio $r$, load $\\rho$, and the short class's share of the work $\\pi$.",
      "tab:cobham", [["", "", "", "", "\\multicolumn{3}{c}{\\textbf{Mean wait relative to FIFO}}"],
                     ["$p$", "$r$", "$\\rho$", "$\\pi$", "Short class", "Long class", "All jobs"]],
      rows, "ccccccc", rules=("\\cmidrule(lr){5-7}",))
macro("CobhamExampleShort", f"{two_class(0.9, 100, 0.8)[1]:.2f}")
macro("CobhamExampleLong", f"{two_class(0.9, 100, 0.8)[2]:.2f}")
macro("CobhamExampleMean", f"{two_class(0.9, 100, 0.8)[3]:.2f}")

(OUT / "numbers.tex").write_text("% generated by gen_numbers.py; do not edit\n" + "\n".join(macros) + "\n")
for name, text in tables:
    (OUT / f"tab_{name}.tex").write_text("% generated by gen_numbers.py; do not edit\n" + text)
print(f"wrote {len(macros)} macros and {len(tables)} tables")
