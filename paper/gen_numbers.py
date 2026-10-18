#!/usr/bin/env python3
"""Generate every number and table in the paper from committed result files.

Reads benchmarks/results/**/summary.json, the replay and sensitivity summaries of v4-replay, v4-replay-aging and
v4-sensitivity (and azure2021/predictability.json) and writes
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
v2 = load("rerun-2026-10-12/v2-prodflow/summary.json")["evaluation"]["criteria"]
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
macro("EThreeWeightedPNinetyNine", f"{e3['S7_weighted_p99_vs_rabbitmq_fifo']['point']:.2f}$\\times$")
macro("EThreeFifoGap", pct(e3["S1_fifo_equivalence_rabbitmq"], ci=False))
macro("EThreeStaticLongWait", f"{e3['S8_rabbitmq_priority_long_wait_vs_fifo']['point']:.1f}$\\times$")

# ---- tables ------------------------------------------------------------------------------------
tables = []


def hb(cell):
    """Bold a plain header cell; leave \\multicolumn cells to the caller."""
    return cell if cell.startswith("\\multicolumn") or not cell else f"\\textbf{{{cell}}}"


def table(caption, label, header, rows, spec, rules=(), size="small", place="t"):
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
        f"\\begin{{table}}[{place}]\n\\centering\\{size}\n"
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
table("Production-flow benchmark: local quiet-machine re-run and its replication on AWS. "
      " Point estimates with 95\\% bootstrap intervals, paired over five seeds.",
      "tab:prodflow", [["Criterion", "Bar", "\\multicolumn{2}{c}{\\textbf{Local run}}", "\\multicolumn{2}{c}{\\textbf{AWS replication}}"]],
      rows, "Llrcrc", rules=("\\cmidrule(lr){3-4}", "\\cmidrule(lr){5-6}"), size="footnotesize", place="H")

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
table("Harbinger against RabbitMQ and Redis on the same workload, on AWS.",
      "tab:brokers", ["Criterion", "Bar", "Estimate [95\\% CI]", "Result"], rows, "Llrc", size="footnotesize", place="H")


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


def _replay_eval(rel):
    """The replay's pre-registered analysis (28 windows, block bootstrap), in the shape trace_rows expects."""
    return {"evaluation": load(rel + "/replay_summary.json")["primary_block_bootstrap"]}


rows = []
for name, summ in [("Azure 2021, replay", _replay_eval("v4-replay/azure2021")),
                   ("Huawei 2025, replay", _replay_eval("v4-replay/huawei2025-r5")),
                   ("LPC-EGEE, simulation", load("v3-traces-2/lpc-egee-2004/summary.json")),
                   ("DAS2-fs2, simulation", load("v3-traces-2/das2-fs2-2003/summary.json"))]:
    rows += trace_rows(name, summ)
table("Trace results, mean latency against FIFO and whether each pre-registered gate passed. "
      "Azure and Huawei were replayed through the real broker on AWS (28 windows, block bootstrap, Huawei pooled over three repeats); "
      "the two research-cluster logs were simulated (seven windows). "
      "A: predictive routing as built. B: with worker-time weights and pausing aging.",
      "tab:traces", [["", "", "\\multicolumn{2}{c}{\\textbf{Gate A: strict priority}}", "\\multicolumn{2}{c}{\\textbf{Gate B: weights, pausing aging}}"],
                     ["Trace", "Load", "Mean vs FIFO", "Passed", "Mean vs FIFO", "Passed"]],
      rows, "llrcrc", rules=("\\cmidrule(lr){3-4}", "\\cmidrule(lr){5-6}"), place="H")
rows = []
for name, rel in [("Azure 2021", "v3-traces/azure2021"), ("Huawei 2025", "v3-traces/huawei2025-r5")]:
    rows += trace_rows(name, load(rel + "/summary.json"))
table("The same two traces in the simulator, as first run (seven independent windows), before the replay. "
      "Gate A passed in one of six cells and gate B in five.",
      "tab:tracessim", [["", "", "\\multicolumn{2}{c}{\\textbf{Gate A: strict priority}}", "\\multicolumn{2}{c}{\\textbf{Gate B: weights, pausing aging}}"],
                        ["Trace", "Load", "Mean vs FIFO", "Passed", "Mean vs FIFO", "Passed"]],
      rows, "llrcrc", rules=("\\cmidrule(lr){3-4}", "\\cmidrule(lr){5-6}"), place="H")

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
        macro(f"{ds}BCI{word}", pct(cr["B_P1_mean_latency_vs_fifo"]).split(" ", 1)[1] if False else "[{:+.1f}, {:+.1f}]".format(*(100 * x for x in cr["B_P1_mean_latency_vs_fifo"]["ci95"])).replace("-", "$-$").replace("+", "$+$") + "\\%")
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

sb = load("rerun-2026-10-12/v3-sizebins/summary.json")["evaluation"]["criteria"]
macro("SizeBinsMean", pct(sb["P1_mean_latency_vs_predictive"], ci=False))
macro("SizeBinsMeanCI", pct(sb["P1_mean_latency_vs_predictive"]))
macro("SizeBinsInvoice", pct(sb["S3_invoice_mean_vs_predictive"], ci=False))


# ---- design-section evidence: aging, feature signal, v2 trace sim -----------------------------
ag = load("v3-aging/sim/summary.json")["evaluation"]
for ld, word in LOADS.items():
    macro(f"AgingSim{word}", pct(ag[ld]["criteria"]["P1_mean_latency_vs_fifo"], ci=False))
    macro(f"AgingSimVsToday{word}", pct(ag[ld]["criteria"]["S2_vs_todays_aging"], ci=False))
agp = load("rerun-2026-10-12/v3-aging-prodflow/summary.json")["evaluation"]["criteria"]
macro("AgingProdMeanVsPred", pct(agp["S1_mean_vs_predictive"], ci=False))
macro("AgingProdPNinetyNine", f"{(agp['G2_all_p99_vs_predictive']['point'] - 1) * 100:.0f}\\%")
macro("AgingProdLongWait", f"{agp['G1_long_class_max_wait_vs_fifo']['point']:.2f}$\\times$")
# AWS reruns of the aging and size-bin production flows (v4-cloud-reruns, E5 and E6), replicating the local results above.
e5 = load("v4-cloud-reruns/e5/summary.json")["evaluation"]["criteria"]
e6 = load("v4-cloud-reruns/e6/summary.json")["evaluation"]["criteria"]
macro("AgingAwsMeanVsPred", pct(e5["S1_mean_vs_predictive"], ci=False))
macro("AgingAwsPNinetyNine", f"{(e5['G2_all_p99_vs_predictive']['point'] - 1) * 100:.1f}\\%")
macro("AgingAwsPNinetyNineHigh", f"{(e5['G2_all_p99_vs_predictive']['ci95'][1] - 1) * 100:.1f}\\%")
macro("SizeBinsAwsMeanCI", pct(e6["P1_mean_latency_vs_predictive"]))
macro("SizeBinsAwsInvoice", pct(e6["S3_invoice_mean_vs_predictive"], ci=False))
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


# ---- setup tables (Section 4): experiments and traces ---------------------------------------
CFG = ROOT / "benchmarks" / "configs"


def cfg(name):
    return json.loads((CFG / f"{name}.json").read_text())


def thousands(n):
    return f"{n:,}".replace(",", "{,}")


c_v2, c_e1, c_e2 = cfg("v2-prodflow"), cfg("v3-cloud-e1"), cfg("v3-cloud-e2")
c_e3, c_e4 = cfg("v3-brokers-prodflow"), cfg("v3-brokers-cost")
pf = lambda c: f"{c['seconds']} s"
rows = [
    ["Local re-run (v2)",
     "Laptop, 8 cores, turbo off; broker in process", pf(c_v2), str(len(c_v2["arms"])),
     f"{len(c_v2['seeds'])} seeds, {len(c_v2['arms']) * len(c_v2['seeds'])} runs"],
    ["AWS replication (E1)",
     "c8i.2xlarge (8 vCPU); broker in process", pf(c_e1), str(len(c_e1["arms"])),
     f"{len(c_e1['seeds'])} seeds, {len(c_e1['arms']) * len(c_e1['seeds'])} runs"],
    ["Brokers (E3)",
     "c8i.2xlarge clients, c8i.xlarge broker", pf(c_e3), str(len(c_e3["arms"])),
     f"{len(c_e3['seeds'])} seeds, {len(c_e3['arms']) * len(c_e3['seeds'])} runs"],
    ["Long backlog (E2)",
     "Same pair of hosts", f"{c_e2['seconds'] // 60} min", str(len(c_e2["arms"])),
     f"{len(c_e2['seeds'])} repetitions, {len(c_e2['arms']) * len(c_e2['seeds'])} runs"],
    ["Broker cost (E4)",
     "Same pair of hosts", f"{c_e4['seconds']} s per rate", f"{len(c_e4['kinds'])} brokers",
     f"{len(c_e4['rates'])} offered rates, {c_e4['repetitions']} repetitions"],
]
table("The wall-clock experiments. The first three run the same synthetic production flow (five services, nine job types, "
      f"about thirty-five thousand messages in a five-minute run, {c_e1['workers']} workers); E2 runs it for thirty minutes and "
      "E4 uses empty handlers. Arms are the policies compared in each run.",
      "tab:setup", ["Experiment", "Hardware", "Run", "Arms", "Runs"], rows, "LLlll", size="footnotesize")

simrows = []
for name, rel, source, keyname, span, arrival in [
    ("Azure Functions 2021", "v3-traces/azure2021", "serverless invocations", "app and function", "14 days", "end time minus duration"),
    ("Huawei Cloud 2025", "v3-traces/huawei2025-r5", "serverless requests, one region", "function", "1 day", "completion minus frontend cost"),
    ("LPC-EGEE 2004", "v3-traces-2/lpc-egee-2004", "grid cluster jobs", "user and queue", "4 months", "recorded submit time"),
    ("DAS2-fs2 2003", "v3-traces-2/das2-fs2-2003", "research-cluster jobs", "user and executable", "6 months", "recorded submit time"),
]:
    sim = json.loads((RES / rel / "simulation.json").read_text())
    nkeys = {"Azure Functions 2021": 424, "Huawei Cloud 2025": 491, "LPC-EGEE 2004": 219, "DAS2-fs2 2003": 2516}[name]
    simrows.append([name, source, thousands(sim["evaluation_rows"] + sim["history_rows"]), thousands(nkeys), span, keyname, arrival])
table("The four traces. Each is split by time into a history half, which warms the predictor, and an evaluation half, replayed on "
      "64 workers at offered loads 0.5, 0.8 and 0.95, simulated for all four traces (seven windows) and, for Azure and Huawei, also replayed through the real broker (28 windows). Job and key counts are those of the converted "
      "files (the preregistrations and the converter report them).",
      "tab:traceset", ["Trace", "Content", "Jobs", "Keys", "Span", "Key", "Arrival"], simrows, "LLrrlLL", size="footnotesize")


# ---- per-class medians in E1 (Figure on per-class latency) --------------------------------------
_arms = load("v3-aws/e1/summary.json")["arms"]
_classes = list(_arms["fifo"]["11"]["per_class"])
_med = lambda arm, c, k: statistics.median(_arms[arm][s]["per_class"][c][k] for s in _arms[arm])
_svc = {c: _med("fifo", c, "median_service_ms") for c in _classes}
_mid = [c for c in _classes if 20 <= _svc[c] <= 100]
_cheap = [c for c in _classes if _svc[c] < 5]
_midchg = [100 * (_med("predictive", c, "p50_ms") / _med("fifo", c, "p50_ms") - 1) for c in _mid]
_cheapchg = [100 * (1 - _med("predictive", c, "p99_ms") / _med("fifo", c, "p99_ms")) for c in _cheap]
macro("PerClassMidMin", f"{min(_midchg):.0f}\\%")
macro("PerClassMidMax", f"{max(_midchg):.0f}\\%")
macro("PerClassCheapPNinetyNineCut", f"{statistics.mean(_cheapchg):.0f}\\%")
_midp99 = [100 * (_med("predictive", c, "p99_ms") / _med("fifo", c, "p99_ms") - 1) for c in _mid]
macro("PerClassMidPNinetyNineMin", f"{min(_midp99):.0f}\\%")
macro("PerClassMidPNinetyNineMax", f"{max(_midp99):.0f}\\%")
macro("PerClassMidCount", str(len(_mid)))


# ---- mean latency by workload phase, predictive vs FIFO (local re-run and AWS) -----------------
def _phase_rows():
    runs = [load("rerun-2026-10-12/v2-prodflow/summary.json")["arms"], load("v3-aws/e1/summary.json")["arms"]]
    names = {"steady": "Steady (about 60\\% load)", "ramp": "Ramp", "flash_sale": "Flash sale (about 125\\% load)",
             "recovery": "Recovery", "shifted": "After the cost shift"}
    out = []
    for phase, label in names.items():
        row = [label]
        for arms in runs:
            seeds = sorted(arms["fifo"])
            fifo = [arms["fifo"][s]["cohorts"][phase]["mean_ms"] for s in seeds]
            pred = [arms["predictive"][s]["cohorts"][phase]["mean_ms"] for s in seeds]
            d = [100 * (p / f - 1) for p, f in zip(pred, fifo)]
            fmt = lambda v: f"{v:+.0f}\\%".replace("-", "$-$").replace("+", "$+$")
            row += [f"{statistics.median(fifo):,.0f}".replace(",", "{,}"), fmt(statistics.mean(d)),
                    f"[{fmt(min(d))}, {fmt(max(d))}]"]
        out.append(row)
    return out


table("Mean latency by workload phase in the production flow: FIFO's mean (median over five seeds, ms), and the change in mean latency "
      "under predictive routing (mean of the per-seed paired changes, with the smallest and largest seed in brackets). "
      "Descriptive; the phases are not separate pre-registered criteria.",
      "tab:phases", [["", "\\multicolumn{3}{c}{\\textbf{Local run}}", "\\multicolumn{3}{c}{\\textbf{AWS replication}}"],
                     ["Phase", "FIFO", "Change", "Seed range", "FIFO", "Change", "Seed range"]],
      _phase_rows(), "lrrrrrr", rules=("\\cmidrule(lr){2-4}", "\\cmidrule(lr){5-7}"), size="footnotesize", place="H")
_ph = _phase_rows()
_abs = lambda cell: cell.replace("$-$", "").replace("$+$", "")
macro("PhaseSteadyAWS", _abs(_ph[0][5]))
macro("PhaseSteadyLocal", _abs(_ph[0][2]))


# ---- simulator validation (v4-simvalidation-1): the simulator against the measured production-flow runs ---------
_val = load("v4-simvalidation/summary.json")
_unsign = lambda x: f"{abs(x) * 100:.1f}\\%"
macro("SimValPredLocalReal", _unsign(_val["local"]["V1"]["predictive"]["real"]))
macro("SimValPredLocalSim", _unsign(_val["local"]["V1"]["predictive"]["sim"]))
macro("SimValPredAWSReal", _unsign(_val["aws"]["V1"]["predictive"]["real"]))
macro("SimValPredAWSSim", _unsign(_val["aws"]["V1"]["predictive"]["sim"]))
_gaps = [_val[s]["V1"][a]["gap_points"] for s in ("local", "aws") for a in ("predictive", "oracle")]
macro("SimValGapMin", f"{min(_gaps):.1f}")
macro("SimValGapMax", f"{max(_gaps):.1f}")
_ratios = [v for s in ("local", "aws") for v in _val[s]["V4"].values()]
macro("SimValRatioMin", f"{min(_ratios):.2f}")
macro("SimValRatioMax", f"{max(_ratios):.2f}")
_over = statistics.mean(_val[s]["V1"]["predictive"]["sim"] / _val[s]["V1"]["predictive"]["real"] - 1 for s in ("local", "aws"))
macro("SimValOverstatePct", f"{_over * 100:.0f}\\%")
macro("SimValSeedsGroupOrder", str(min(_val[s]["V3"]["seeds_with_group_order"] for s in ("local", "aws"))))


# ---- the real-broker replay of the Azure and Huawei traces (v4-replay-1) ------------------------------------
ARMS_W = "predictive_warm+weights+pausing"
_replay = {"Azure": load("v4-replay/azure2021/replay_summary.json"), "Huawei": load("v4-replay/huawei2025-r5/replay_summary.json")}
_ci = lambda c: "[{:+.1f}, {:+.1f}]".format(*(100 * x for x in c["ci95"])).replace("-", "$-$").replace("+", "$+$") + "\\%"
_gA = _gB = _cells = 0
for ds, rs in _replay.items():
    prim = rs["primary_block_bootstrap"]
    for ld, word in LOADS.items():
        cr = prim[ld]["criteria"]
        macro(f"Replay{ds}A{word}", pct(cr["A_P1_mean_latency_vs_fifo"], ci=False))
        macro(f"Replay{ds}B{word}", pct(cr["B_P1_mean_latency_vs_fifo"], ci=False))
        macro(f"Replay{ds}ACI{word}", _ci(cr["A_P1_mean_latency_vs_fifo"]))
        macro(f"Replay{ds}BCI{word}", _ci(cr["B_P1_mean_latency_vs_fifo"]))
        macro(f"Replay{ds}BPNinetyNine{word}", f"{cr['B_G1_all_p99_vs_fifo']['point']:.2f}$\\times$")
        macro(f"Replay{ds}BLong{word}", f"{cr['B_G2_long_max_wait_vs_fifo']['point']:.2f}$\\times$")
        macro(f"Replay{ds}BVsA{word}", pct(cr["S7_weights_pausing_vs_predictive_warm"], ci=False))
        for key, tag in (("S5_oracle_vs_fifo", "Oracle"), ("S6_oracle_weights_pausing_vs_fifo", "OracleB")):
            if key in cr:
                macro(f"Replay{ds}{tag}{word}", pct(cr[key], ci=False))
        if "S2_vs_static_history_noninferior" in cr:
            macro(f"Replay{ds}VsStatic{word}", pct(cr["S2_vs_static_history_noninferior"], ci=False))
            macro(f"Replay{ds}VsStaticCI{word}", pct(cr["S2_vs_static_history_noninferior"]))
        gp = prim[ld]["gates_passed"]
        _gA += gp["A_predictive_routing"]; _gB += gp["B_overload_design"]; _cells += 1
macro("ReplayGateAPassed", str(_gA))
macro("ReplayGateBPassed", str(_gB))
macro("ReplayCells", str(_cells))
_wh = _replay["Huawei"]["primary_block_bootstrap"]
_sec = lambda ms: f"{ms / 1000:.1f}~s"
for ld, word in (("0.8", "Eight"), ("0.95", "NinetyFive")):
    a = _wh[ld]["all"]
    macro(f"ReplayHuaweiFifoMean{word}", _sec(a["fifo"]["mean_latency_ms"]))
    macro(f"ReplayHuaweiPredMean{word}", _sec(a["predictive_warm"]["mean_latency_ms"]))
    macro(f"ReplayHuaweiWeightedMean{word}", _sec(a[ARMS_W]["mean_latency_ms"]))
    macro(f"ReplayHuaweiFifoMedian{word}", _sec(a["fifo"]["p50_latency_ms"]))
    macro(f"ReplayHuaweiWeightedMedian{word}", f"{a[ARMS_W]['p50_latency_ms']:.0f}~ms")
_probe = {ds: next(iter(rs["overhead_probe"].values())) for ds, rs in _replay.items()}
macro("ReplayProbeHuaweiMedian", f"{_probe['Huawei']['overhead_p50_ms']:.1f}~ms")
macro("ReplayProbeHuaweiPNinetyNine", f"{_probe['Huawei']['overhead_p99_ms']:.1f}~ms")
macro("ReplayProbeAzureMedian", f"{_probe['Azure']['overhead_p50_ms']:.1f}~ms")
macro("ReplayProbeAzurePNinetyNine", f"{_probe['Azure']['overhead_p99_ms']:.1f}~ms")
# the simulator against the replay: whole-run mean change against FIFO, per arm
_sr_rows = []
_gaps = []
for ds, rs in _replay.items():
    for ld in ("0.5", "0.8", "0.95"):
        sc = rs["simulator_comparison"][ld]["mean_latency_change"]
        cell = []
        for arm in ("predictive_warm", ARMS_W, "oracle+weights+pausing"):
            if arm in sc and not (arm == "predictive_warm" and ds == "Azure"):
                r_, s_ = sc[arm]["replay"], sc[arm]["simulator"]
                cell += [f"{r_ * 100:+.1f}\\%".replace("-", "$-$").replace("+", "$+$"), f"{s_ * 100:+.1f}\\%".replace("-", "$-$").replace("+", "$+$")]
                _gaps.append(abs(r_ - s_) * 100)
            else:
                cell += ["---", "---"]
        _sr_rows.append([f"{ds}", ld] + cell)
table("Mean-latency change against FIFO over each whole run, measured through the real broker and predicted by the simulator, "
      "for the same arms, traces and loads. Azure's strict-priority arm is omitted because it changes nothing in either (a few hundredths of a point), "
      "and the weighted oracle ran at load 0.8 on Azure only.",
      "tab:simreplay", [["", "", "\\multicolumn{2}{c}{\\textbf{Predictive}}", "\\multicolumn{2}{c}{\\textbf{Weighted}}", "\\multicolumn{2}{c}{\\textbf{Weighted oracle}}"],
                        ["Trace", "Load", "Replay", "Simulator", "Replay", "Simulator", "Replay", "Simulator"]],
      _sr_rows, "llrrrrrr", rules=("\\cmidrule(lr){3-4}", "\\cmidrule(lr){5-6}", "\\cmidrule(lr){7-8}"), size="footnotesize", place="H")
macro("SimReplayGapMax", f"{max(_gaps):.1f}")
macro("SimReplayGapMedian", f"{statistics.median(_gaps):.1f}")
# the original seven-window simulator verdicts and the 28-window re-estimate, to count the cells
def _gate_count(ev, gate):
    return sum(ev[ld]["gates_passed"][gate] for ld in ("0.5", "0.8", "0.95"))
_sev = {ds: load(rel + "/summary.json")["evaluation"] for ds, rel in (("Azure", "v3-traces/azure2021"), ("Huawei", "v3-traces/huawei2025-r5"))}
macro("SimSevenAPassed", str(sum(_gate_count(_sev[d], "A_predictive_routing") for d in _sev)))
macro("SimSevenBPassed", str(sum(_gate_count(_sev[d], "B_overload_design") for d in _sev)))
_blk = {ds: load(f"v4-sensitivity/{t}/block_bootstrap.json")["4"] for ds, t in (("Azure", "azure2021"), ("Huawei", "huawei2025-r5"))}
macro("SimBlockAPassed", str(sum(_gate_count(_blk[d], "A_predictive_routing") for d in _blk)))
macro("SimBlockBPassed", str(sum(_gate_count(_blk[d], "B_overload_design") for d in _blk)))


# ---- baselines, ablation and aging isolation (v4-baselines-1) ----------------------------------------------
_bs = load("v4-baselines/summary.json")
_bc, _ba = _bs["evaluation"]["criteria"], _bs["arms"]
_bmed = lambda arm, key: statistics.median(m[key] for m in _ba[arm].values())
_bchg = lambda arm, base, key="mean_latency_ms": statistics.mean(_ba[arm][s][key] / _ba[base][s][key] - 1 for s in _ba[arm])
macro("BaseQOne", pct(_bc["Q1_predictive_replicates_v2"], ci=False))
macro("BaseBTwo", pct(_bc["B2_predictive_vs_static_tuned"], ci=False))
macro("BaseBThree", pct(_bc["B3_predictive_mean_vs_predictive"], ci=False))
macro("BaseBFour", pct(_bc["B4_predictive_p75_vs_predictive"], ci=False))
macro("BasePredWorseThanPtf", pct(_bc["B1_predictive_vs_per_type_fair"], ci=False))
macro("BasePtfVsPred", pct({"point": _bchg("per_type_fair", "predictive")}, ci=False))  # the table's paired mean, per seed
macro("BasePtfPNinetyNine", pct(_bc["R_ptf_p99_vs_predictive"], ci=False))
macro("BasePtfLongWait", pct(_bc["R_ptf_long_max_wait_vs_predictive"], ci=False))
macro("BaseNoAgingVsPred", pct(_bc["C1_noaging_vs_predictive"], ci=False))
macro("BaseNoAgingVsPredCI", pct(_bc["C1_noaging_vs_predictive"]))
macro("BaseNoAgingVsFifo", pct(_bc["C2_noaging_mean_vs_fifo"], ci=False))
macro("BaseNoAgingPNinetyNine", pct(_bc["C3_noaging_p99_vs_fifo"], ci=False))
macro("BaseNoAgingLongWait", pct(_bc["C4_noaging_long_max_wait_vs_fifo"], ci=False))
rows = []
for label, arm in (("FIFO", "fifo"), ("Predictive routing", "predictive"), ("Hand-tuned static map", "static_tuned"),
                   ("Queue per job type, no prediction", "per_type_fair"), ("Predictive, aging off", "predictive_noaging"),
                   ("Predictive, mean summary", "predictive_mean"), ("Predictive, 75th-percentile summary", "predictive_p75")):
    f = lambda v: f"{v:+.1f}\\%".replace("-", "$-$").replace("+", "$+$")
    rows.append([label, f"{_bmed(arm, 'mean_latency_ms') / 1000:.2f}",
                 "---" if arm == "fifo" else f(100 * _bchg(arm, "fifo")),
                 "---" if arm == "predictive" else f(100 * _bchg(arm, "predictive")),
                 f"{_bmed(arm, 'short_p50_ms'):.0f}", f"{_bmed(arm, 'p99_latency_ms') / 1000:.1f}", f"{_bmed(arm, 'long_max_wait_ms') / 1000:.1f}"])
table("Baselines and aging isolation on the production flow, on AWS, ten seeds. Medians over seeds for the levels; "
      "the two change columns are the means of the per-seed paired changes in mean latency, each relative to the column's base, so the hand-tuned map's change against predictive routing is not the negative of predictive routing's change against the map. Descriptive; the pre-registered readings are in the text.",
      "tab:baselines", ["Arm", "Mean (s)", "vs FIFO", "vs predictive", "Short P50 (ms)", "P99 (s)", "Long wait (s)"],
      rows, "Lrrrrrr", size="footnotesize", place="H")
rows = []
_status = {"not supported here": "no effect shown", "inconclusive": "inconclusive"}
for label, key in (("Decay and idle forgetting", "decay"), ("99th-percentile spread check", "spread"),
                   ("Staleness rule", "stale"), ("Minimum of 20 observations", "nmin")):
    o, sh = _bc[f"AB_{key}_overall"], _bc[f"AB_{key}_shifted"]
    rows.append([label, pct(o), _status.get(o["status"], o["status"]), pct(sh), _status.get(sh["status"], sh["status"])])
table("Removing one predictor component on the production flow (change in mean latency against the full predictor, ten seeds, 95\\% bootstrap interval). "
      "``No effect shown'' means the whole interval lies within $\\pm 3\\%$; ``inconclusive'' means it does not.",
      "tab:ablation", [["", "\\multicolumn{2}{c}{\\textbf{Whole run}}", "\\multicolumn{2}{c}{\\textbf{After the cost shift}}"],
                       ["Component removed", "Change", "Reading", "Change", "Reading"]],
      rows, "Lrlrl", rules=("\\cmidrule(lr){2-3}", "\\cmidrule(lr){4-5}"), size="footnotesize", place="H")


# ---- aging switched off on a real trace (v4-replay-aging-1) -------------------------------------------------
_ag = load("v4-replay-aging/replay_summary.json")["primary_block_bootstrap"]
for ld, word in (("0.8", "Eight"), ("0.95", "NinetyFive")):
    cr = _ag[ld]["criteria"]
    macro(f"AgingReplayA{word}", pct(cr["A_P1_mean_latency_vs_fifo"], ci=False))
    macro(f"AgingReplayN{word}", pct(cr["N_P1_mean_latency_vs_fifo"], ci=False))
    macro(f"AgingReplayNCI{word}", _ci(cr["N_P1_mean_latency_vs_fifo"]))
    macro(f"AgingReplayNVsA{word}", pct(cr["N_vs_A_mean_latency"], ci=False))
    macro(f"AgingReplayNPNinetyNine{word}", f"{cr['N_G1_all_p99_vs_fifo']['point']:.2f}$\\times$")
    macro(f"AgingReplayNLong{word}", f"{cr['N_G2_long_max_wait_vs_fifo']['point']:.2f}$\\times$")
    a = _ag[ld]["all"]
    macro(f"AgingReplayFifoMean{word}", _sec(a["fifo"]["mean_latency_ms"]))
    macro(f"AgingReplayPredMean{word}", _sec(a["predictive_warm"]["mean_latency_ms"]))
    macro(f"AgingReplayNoAgingMean{word}", _sec(a["predictive_warm+no_aging"]["mean_latency_ms"]))
rows = []
for ld in ("0.8", "0.95"):
    cr, gp = _ag[ld]["criteria"], _ag[ld]["gates_passed"]
    rows.append([ld, pct(cr["A_P1_mean_latency_vs_fifo"]), "\\hbyes{}" if gp["A_predictive_routing"] else "\\hbno{}",
                 pct(cr["N_P1_mean_latency_vs_fifo"]), "\\hbyes{}" if gp["N_no_aging"] else "\\hbno{}",
                 pct(cr["N_vs_A_mean_latency"], ci=False), ratio(cr["N_G1_all_p99_vs_fifo"], ci=False), ratio(cr["N_G2_long_max_wait_vs_fifo"], ci=False)])
table("Aging switched off, Huawei Cloud replayed through the real broker (three repeats, 28 windows, block bootstrap). "
      "Default aging is gate A; the same predictor with aging off is gate N, with the same three rules.",
      "tab:agingreplay", [["", "\\multicolumn{2}{c}{\\textbf{Default aging (gate A)}}", "\\multicolumn{2}{c}{\\textbf{Aging off (gate N)}}", "", "\\multicolumn{2}{c}{\\textbf{Gate N guardrails}}"],
                          ["Load", "Mean vs FIFO", "Passed", "Mean vs FIFO", "Passed", "Off vs default", "P99", "Long wait"]],
      rows, "lrcrcrrr", rules=("\\cmidrule(lr){2-3}", "\\cmidrule(lr){4-5}", "\\cmidrule(lr){7-8}"), size="scriptsize", place="H")


# ---- parameter sensitivity in the simulator (v4-sensitivity-1) ------------------------------------------------
_sens = {"Azure": load("v4-sensitivity/azure2021/sim/sensitivity.json"), "Huawei": load("v4-sensitivity/huawei2025-r5/sim/sensitivity.json")}
_AK, _BK = "A_P1_mean_latency_vs_fifo", "B_P1_mean_latency_vs_fifo"
for ds, sj in _sens.items():
    macro(f"Sens{ds}APassed", str(sj["passing_variants"][_AK]))
    macro(f"Sens{ds}BPassed", str(sj["passing_variants"][_BK]))
macro("SensVariants", str(_sens["Azure"]["variants"]))
macro("SensHuaweiAFlips", str(_sens["Huawei"]["variants"] - _sens["Huawei"]["passing_variants"][_AK]))
_hv = _sens["Huawei"]["verdicts"]
for vid, word in (("aging_threshold-1s", "One"), ("default", "Five"), ("aging_threshold-30s", "Thirty")):
    macro(f"SensHuaweiAgingA{word}", pct(_hv[vid][_AK], ci=False))
    macro(f"SensAzureAgingA{word}", pct(_sens["Azure"]["verdicts"][vid][_AK], ci=False))
macro("SensHuaweiDefaultA", pct(_hv["default"][_AK], ci=False))
macro("SensAzureDefaultB", pct(_sens["Azure"]["verdicts"]["default"][_BK], ci=False))
_names = {"n_min": "Minimum observations", "kappa": "Spread ratio", "decay": "Decay", "tau": "Staleness limit",
          "aging_threshold": "Aging threshold", "weights": "Level weights", "levels": "Number of levels"}
def _flip(sj, gate, param):
    r = sj["readings"][gate][param]
    return "robust" if r["robust"] else "flips at " + ", ".join(str(v) for v in r["flips_at"])
rows = [[_names[p], _flip(_sens["Azure"], _AK, p), _flip(_sens["Azure"], _BK, p), _flip(_sens["Huawei"], _AK, p), _flip(_sens["Huawei"], _BK, p)] for p in _names]
rows.append(["Variants passing (of " + str(_sens["Azure"]["variants"]) + ")", str(_sens["Azure"]["passing_variants"][_AK]), str(_sens["Azure"]["passing_variants"][_BK]),
             str(_sens["Huawei"]["passing_variants"][_AK]), str(_sens["Huawei"]["passing_variants"][_BK])])
table("One-at-a-time parameter sensitivity in the simulator at load 0.8 (seven windows): whether each parameter moves the gate's mean-latency verdict "
      "away from its verdict at the defaults, and at which value. Simulator results only; the grid is in the text. Azure's gate A fails at every value, so ``robust'' there means it never passes.",
      "tab:sensitivity", [["", "\\multicolumn{2}{c}{\\textbf{Azure 2021}}", "\\multicolumn{2}{c}{\\textbf{Huawei Cloud}}"],
                          ["Parameter", "Gate A", "Gate B", "Gate A", "Gate B"]],
      rows, "Lllll", rules=("\\cmidrule(lr){2-3}", "\\cmidrule(lr){4-5}"), size="footnotesize", place="H")


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
      rows, "ccccccc", rules=("\\cmidrule(lr){5-7}",), place="H")
macro("CobhamExampleShort", f"{two_class(0.9, 100, 0.8)[1]:.2f}")
macro("CobhamExampleLong", f"{two_class(0.9, 100, 0.8)[2]:.2f}")
macro("CobhamExampleMean", f"{two_class(0.9, 100, 0.8)[3]:.2f}")

(OUT / "numbers.tex").write_text("% generated by gen_numbers.py; do not edit\n" + "\n".join(macros) + "\n")
for name, text in tables:
    (OUT / f"tab_{name}.tex").write_text("% generated by gen_numbers.py; do not edit\n" + text)
print(f"wrote {len(macros)} macros and {len(tables)} tables")
