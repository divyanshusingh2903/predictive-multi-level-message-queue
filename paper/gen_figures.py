#!/usr/bin/env python3
"""Generate the paper's figures as TikZ: three mechanism illustrations (histogram, weighted share, aging)
and the measured production-flow, broker and trace charts (prodflow, brokers, traces, perclass), which read benchmarks/results,
and the closed-form queueing curves (cobham).

Both are computed from the rules in Section 3, not measured: the histogram shapes are
hand-chosen examples whose medians, p99s and boundaries are then derived by the same
quantile rules the broker uses, and the share strip applies the virtual-clock rule.
"""
import collections
import json
import pathlib
import random

OUT = pathlib.Path(__file__).resolve().parent / "figures"
RES = pathlib.Path(__file__).resolve().parent.parent / "benchmarks" / "results"
NB = 14  # bins [2^i, 2^(i+1)) ms for i = 0..13


def quantile(h, q):
    """Position (in bins, fractional) of the q-quantile, interpolating inside a bin."""
    total = sum(h)
    target, acc = q * total, 0.0
    for i, m in enumerate(h):
        if acc + m >= target:
            return i + (target - acc) / m if m else i
        acc += m
    return float(len(h))


def histogram_figure():
    # The global histogram is the pooled history of every key: here a four-key mix.
    keys = {
        "send\\_sms":        [0, 4, 14, 18, 8, 2, 0, 0, 0, 0, 0, 0, 0, 0],
        "resize\\_image":    [0, 0, 0, 0, 0, 2, 8, 16, 14, 6, 1, 0, 0, 0],
        "export\\_report":   [0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 5, 12, 14, 5],
        "mixed (92/8)":     [0, 0, 92, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8],
    }
    weights = {"send\\_sms": 36, "resize\\_image": 34, "export\\_report": 22, "mixed (92/8)": 8}
    glob = [sum(weights[k] * keys[k][i] / sum(keys[k]) for k in keys) for i in range(NB)]
    L = 3
    beta = [quantile(glob, j / L) for j in range(1, L)]
    w, hgt = 0.62, 1.0  # bin width (cm) and panel height (cm)
    lines = ["\\resizebox{\\textwidth}{!}{%", "\\begin{tikzpicture}[font=\\small]"]

    def panel(y, name, h, show_stats):
        top = max(h)
        lines.append(f"  \\node[anchor=east] at (-0.2,{y + hgt / 2:.2f}) {{{name}}};")
        lines.append(f"  \\draw[hbslate!60] (0,{y:.2f}) -- ({NB * w:.2f},{y:.2f});")
        for i, m in enumerate(h):
            if m:
                lines.append(f"  \\fill[hbink!85] ({i * w + 0.04:.2f},{y:.2f}) rectangle ({(i + 1) * w - 0.04:.2f},{y + hgt * m / top:.2f});")
        if show_stats:
            med, p99 = quantile(h, 0.5), quantile(h, 0.99)
            level = sum(1 for b in beta if b <= med)
            spread = 2 ** (p99 - med)
            ok = spread <= 16
            lines.append(f"  \\fill[hbsignal] ({med * w:.2f},{y - 0.12:.2f}) -- ({med * w - 0.09:.2f},{y - 0.3:.2f}) -- ({med * w + 0.09:.2f},{y - 0.3:.2f}) -- cycle;")
            lines.append(f"  \\draw[hbslate, line width=0.8pt] ({p99 * w:.2f},{y - 0.05:.2f}) -- ({p99 * w:.2f},{y - 0.3:.2f});")
            if ok:
                note = f"median $\\to$ level {level}"
                color = "hbsignal"
            else:
                note = f"p99 $\\approx 2^{{{round(p99 - med)}}}\\times$ the median (limit 16$\\times$): default level"
                color = "hbslate"
            lines.append(f"  \\node[anchor=west, font=\\footnotesize, text={color}] at ({NB * w + 0.25:.2f},{y + hgt / 2:.2f}) {{{note}}};")

    y = 0.0
    panel(y, "all keys ($G$)", glob, False)
    for b in beta:
        pass
    ys = [y]
    for name, h in keys.items():
        y -= 1.55
        panel(y, name, h, True)
        ys.append(y)
    bottom = ys[-1] - 0.35
    for j, b in enumerate(beta, start=1):
        lines.append(f"  \\draw[hbslate, dashed] ({b * w:.2f},{bottom:.2f}) -- ({b * w:.2f},{hgt + 0.25:.2f});")
        lines.append(f"  \\node[font=\\footnotesize, text=hbslate, anchor=south] at ({b * w:.2f},{hgt + 0.25:.2f}) {{$\\beta_{j}$}};")
    for i in range(0, NB + 1, 2):
        label = f"{2 ** i} ms" if i < 10 else f"{2 ** (i - 10)} s"
        lines.append(f"  \\node[font=\\scriptsize, text=hbslate, anchor=north] at ({i * w:.2f},{bottom:.2f}) {{{label}}};")
    lines.append(f"  \\node[font=\\footnotesize, text=hbslate, anchor=north] at ({NB * w / 2:.2f},{bottom - 0.45:.2f}) {{handler duration (log scale)}};")
    lines.append("\\end{tikzpicture}}")
    (OUT / "histogram.tex").write_text("\n".join(lines) + "\n")


def weights_figure():
    w = [8, 3, 1]
    v = [0.0, 0.0, 0.0]
    served = []
    for _ in range(48):  # every level always has work, every message costs 1
        l = min(range(3), key=lambda i: (v[i], i))
        served.append(l)
        v[l] += 1.0 / w[l]
    shades = ["hbink", "hbslate", "hbslate!35"]
    lines = ["\\resizebox{\\textwidth}{!}{%", "\\begin{tikzpicture}[font=\\small]"]
    lines.append("  \\node[anchor=east] at (-0.2,0.0) {pulls in order};")
    for i, l in enumerate(served):
        lines.append(f"  \\fill[{shades[l]}] ({i * 0.3 + 0.02:.2f},-0.2) rectangle ({(i + 1) * 0.3 - 0.02:.2f},0.2);")
    for l in range(3):
        lines.append(f"  \\fill[{shades[l]}] ({l * 4.2:.2f},-0.95) rectangle ({l * 4.2 + 0.3:.2f},-0.65);")
        lines.append(f"  \\node[anchor=west, font=\\footnotesize] at ({l * 4.2 + 0.4:.2f},-0.8) {{level {l}, weight {w[l]}}};")
    counts = [served.count(l) / len(served) for l in range(3)]
    ideal = [x / sum(w) for x in w]
    lines.append("  \\node[anchor=east] at (-0.2,-1.9) {share of pulls};")
    for l in range(3):
        y = -1.55 - l * 0.5
        lines.append(f"  \\fill[{shades[l]}] (0,{y - 0.15:.2f}) rectangle ({counts[l] * 8:.2f},{y + 0.15:.2f});")
        lines.append(f"  \\draw[hbsignal, line width=1.6pt] ({ideal[l] * 8:.2f},{y - 0.28:.2f}) -- ({ideal[l] * 8:.2f},{y + 0.28:.2f});")
        lines.append(f"  \\node[anchor=west, font=\\footnotesize] at ({max(counts[l], ideal[l]) * 8 + 0.2:.2f},{y:.2f}) {{{counts[l] * 100:.0f}\\% of pulls (ideal {ideal[l] * 100:.0f}\\%)}};")
    lines.append("\\end{tikzpicture}}")
    (OUT / "weights.tex").write_text("\n".join(lines) + "\n")


def run(mode, seed=7, T=150.0, dt=0.05, lam=24.0, probs=(0.5, 0.3, 0.2), theta=5.0, interval=0.5, w=(8, 3, 1)):
    rng = random.Random(seed)
    lv = [collections.deque() for _ in range(3)]
    v = [0.0, 0.0, 0.0]
    t, nxt_age = 0.0, interval
    comp, waits = [], collections.defaultdict(list)
    next_sample = 0.0
    while t < T:
        # arrivals (Bernoulli approximation of Poisson at this dt)
        n = 0
        p = lam * dt
        while p > 0:
            if rng.random() < min(p, 1.0): n += 1
            p -= 1.0
        for _ in range(n):
            o = rng.choices((0, 1, 2), probs)[0]
            lv[o].append({"arr": t, "enq": t, "orig": o, "prom": False})
        # aging pass
        if t >= nxt_age:
            nxt_age += interval
            for L in (2, 1):
                T_ = L - 1
                if mode == "plain":
                    keep = collections.deque()
                    for m in lv[L]:
                        if t - m["enq"] >= theta:
                            m["enq"], m["prom"] = t, True
                            lv[T_].append(m)
                        else:
                            keep.append(m)
                    lv[L] = keep
                else:
                    behind = any(m["prom"] for m in lv[T_]) and lv[T_] and (t - lv[T_][0]["arr"] >= theta)
                    if behind:
                        continue
                    for i, m in enumerate(lv[L]):
                        if t - m["enq"] >= theta:
                            del lv[L][i]
                            m["enq"], m["prom"] = t, True
                            lv[T_].append(m)
                            break
        # one pull per dt (service time dt)
        nonempty = [i for i in range(3) if lv[i]]
        if nonempty:
            if mode == "plain":
                l = nonempty[0]
            else:
                vt = min(v[i] for i in nonempty)
                for i in range(3):
                    if not lv[i]: v[i] = max(v[i], vt)
                l = min(nonempty, key=lambda i: (v[i], i))
                v[l] += 1.0 / w[l]
            m = lv[l].popleft()
            waits[int(t // 5)].append((m["orig"], t - m["arr"]))
        if t >= next_sample:
            next_sample += 1.0
            tot = len(lv[0])
            promoted = sum(1 for m in lv[0] if m["orig"] > 0)
            comp.append((t, promoted / tot if tot else 0.0, [len(q) for q in lv]))
        t += dt
    return comp, waits



def aging_figure():
    """Toy model of the two promotion rules under sustained overload (120% load, 150 s)."""
    plain, wp = run("plain")
    paus, wq = run("pausing")
    T = 150.0

    def smooth(series, k=5):
        out = []
        for i in range(len(series)):
            seg = series[max(0, i - k + 1): i + 1]
            out.append(sum(seg) / len(seg))
        return out

    def waits_series(waits):
        pts = []
        for b in range(int(T // 5)):
            o0 = [x for o, x in waits.get(b, []) if o == 0]
            if o0:
                pts.append((b * 5 + 2.5, sum(o0) / len(o0)))
        return pts

    a_plain = list(zip([c[0] for c in plain], smooth([c[1] for c in plain])))
    a_paus = list(zip([c[0] for c in paus], smooth([c[1] for c in paus])))
    c_plain = [(c[0], c[2][2]) for c in plain]
    c_paus = [(c[0], c[2][2]) for c in paus]
    panels = [
        ("(a) promoted work in the top level", a_plain, a_paus, 1.0, "share"),
        ("(b) wait of jobs predicted short (s)", waits_series(wp), waits_series(wq), None, "s"),
        ("(c) messages in the lowest level", c_plain, c_paus, None, "n"),
    ]
    W, H, gap = 4.6, 2.6, 1.5
    lines = ["\\resizebox{\\textwidth}{!}{%", "\\begin{tikzpicture}[font=\\small]"]
    for k, (title, d1, d2, ymax, kind) in enumerate(panels):
        x0 = k * (W + gap)
        top = ymax or max(max(y for _, y in d1), max(y for _, y in d2)) * 1.1
        step = {"share": 0.25, "s": 5, "n": 100}[kind]
        top = (int(top / step) + 1) * step if ymax is None else ymax
        lines.append(f"  \\node[font=\\footnotesize\\bfseries, anchor=south west] at ({x0:.2f},{H + 0.15:.2f}) {{{title}}};")
        lines.append(f"  \\draw[hbslate!60] ({x0:.2f},0) -- ({x0 + W:.2f},0); \\draw[hbslate!60] ({x0:.2f},0) -- ({x0:.2f},{H:.2f});")
        for xt in (0, 50, 100, 150):
            lines.append(f"  \\node[font=\\scriptsize, text=hbslate, anchor=north] at ({x0 + xt / T * W:.2f},-0.05) {{{xt}}};")
        nt = int(round(top / step))
        for i in range(0, nt + 1, max(1, nt // 4)):
            yv = i * step
            lab = f"{yv:.2f}".rstrip("0").rstrip(".") if kind == "share" else f"{yv:.0f}"
            lines.append(f"  \\node[font=\\scriptsize, text=hbslate, anchor=east] at ({x0 - 0.05:.2f},{yv / top * H:.2f}) {{{lab}}};")
        for data, col, wd in ((d1, "hbink", "0.9pt"), (d2, "hbsignal", "1.2pt")):
            pts = " -- ".join(f"({x0 + x / T * W:.3f},{min(y, top) / top * H:.3f})" for x, y in data)
            lines.append(f"  \\draw[{col}, line width={wd}] {pts};")
        lines.append(f"  \\node[font=\\scriptsize, text=hbslate, anchor=north] at ({x0 + W / 2:.2f},-0.4) {{time (s)}};")
    lx = 0.0
    lines.append(f"  \\draw[hbink, line width=0.9pt] ({lx:.2f},-1.2) -- ({lx + 0.7:.2f},-1.2); \\node[anchor=west, font=\\footnotesize] at ({lx + 0.85:.2f},-1.2) {{strict priority with plain aging (today's default)}};")
    lines.append(f"  \\draw[hbsignal, line width=1.2pt] ({lx + 8.2:.2f},-1.2) -- ({lx + 8.9:.2f},-1.2); \\node[anchor=west, font=\\footnotesize] at ({lx + 9.05:.2f},-1.2) {{weights 8:3:1 with pausing aging}};")
    lines.append("\\end{tikzpicture}}")
    (OUT / "aging.tex").write_text("\n".join(lines) + "\n")


def prodflow_figure():
    """Grouped horizontal bars: change against FIFO for each production-flow criterion, local run beside AWS replication.

    Every quantity is a relative change in latency, so ratio criteria (P99 and max wait) are shown as ratio - 1.
    Whiskers are the 95% bootstrap intervals the table reports.
    """
    ev = lambda rel: json.loads((RES / rel).read_text())["evaluation"]["criteria"]
    arms = lambda rel: json.loads((RES / rel).read_text())["arms"]
    runs = (("Local run", "hbslate!55", ev("rerun-2026-10-12/v2-prodflow/summary.json"), arms("rerun-2026-10-12/v2-prodflow/summary.json")),
            ("AWS replication", "hbink", ev("v3-aws/e1/summary.json"), arms("v3-aws/e1/summary.json")))
    # (arm, baseline, metric) behind each criterion, to draw every seed's own paired value
    seeds_of = {"P1_mean_latency_vs_fifo": ("predictive", "fifo", "mean_latency_ms"),
                "P2_short_class_p50_vs_fifo": ("predictive", "fifo", "short_p50_ms"),
                "G1_all_p99_vs_fifo": ("predictive", "fifo", "p99_latency_ms"),
                "G2_long_class_max_wait": ("predictive", "fifo", "long_max_wait_ms"),
                "S1_vs_static_tuned_noninferior": ("predictive", "static_tuned", "mean_latency_ms"),
                "S2_beats_static_misconfigured": ("predictive", "static_misconfigured", "mean_latency_ms"),
                "S4_zero_config_vs_fifo": ("predictive_producer", "fifo", "mean_latency_ms")}
    rows = [("P1_mean_latency_vs_fifo", "Mean latency vs FIFO"),
            ("P2_short_class_p50_vs_fifo", "Short-job median vs FIFO"),
            ("G1_all_p99_vs_fifo", "All-message P99 vs FIFO"),
            ("G2_long_class_max_wait", "Long-job maximum wait vs FIFO"),
            ("S1_vs_static_tuned_noninferior", "Mean vs hand-tuned static map"),
            ("S2_beats_static_misconfigured", "Mean vs misconfigured static map"),
            ("S4_zero_config_vs_fifo", "Mean, one key per producer, vs FIFO")]

    def est(c):
        sh = 1.0 if c["kind"] == "ratio" else 0.0
        return [100 * (v - sh) for v in (c["point"], *c["ci95"])]

    lo_ax, hi_ax = -100, 15
    x0, sc = 4.9, 0.062  # plot origin (cm) and cm per percentage point
    zx = x0 + (0 - lo_ax) * sc
    gh, bh = 1.0, 0.3  # group pitch and bar height
    n = len(rows)
    top = n * gh
    lines = ["\\resizebox{\\textwidth}{!}{%", "\\begin{tikzpicture}[font=\\small]"]
    for xt in range(-100, 20, 25):
        x = x0 + (xt - lo_ax) * sc
        lines.append(f"  \\draw[hbslate!25] ({x:.3f},0) -- ({x:.3f},{top:.2f});")
        tick = "0\\%" if xt == 0 else f"$-${abs(xt)}\\%"
        lines.append(f"  \\node[font=\\scriptsize, text=hbslate, anchor=north] at ({x:.3f},-0.05) {{{tick}}};")
    lines.append(f"  \\draw[hbslate, line width=0.7pt] ({zx:.3f},0) -- ({zx:.3f},{top:.2f});")
    vx = x0 + (hi_ax - lo_ax) * sc + 0.25
    for i, (key, label) in enumerate(rows):
        yc = top - (i + 0.5) * gh
        lines.append(f"  \\node[anchor=east, align=right] at ({x0 - 0.15:.2f},{yc:.2f}) {{{label}}};")
        for j, (rname, col, crit, arm_tab) in enumerate(runs):
            p, lo, hi = est(crit[key])
            y = yc + (0.5 - j) * (bh + 0.04)
            xa, xb = sorted((zx, zx + p * sc))
            lines.append(f"  \\fill[{col}] ({xa:.3f},{y - bh / 2:.3f}) rectangle ({xb:.3f},{y + bh / 2:.3f});")
            lines.append(f"  \\draw[hbink, line width=0.6pt] ({zx + lo * sc:.3f},{y:.3f}) -- ({zx + hi * sc:.3f},{y:.3f});")
            for e in (lo, hi):
                lines.append(f"  \\draw[hbink, line width=0.6pt] ({zx + e * sc:.3f},{y - 0.07:.3f}) -- ({zx + e * sc:.3f},{y + 0.07:.3f});")
            arm, base, metric = seeds_of[key]
            for sd in arm_tab[arm]:
                v = arm_tab[arm][sd][metric] / arm_tab[base][sd][metric] - 1
                if lo_ax <= 100 * v <= hi_ax:
                    lines.append(f"  \\draw[hbink, line width=0.45pt, fill=white] ({zx + 100 * v * sc:.3f},{y:.3f}) circle (0.045);")
            txt = f"{p:+.1f}\\% [{lo:+.1f}, {hi:+.1f}]".replace("-", "$-$").replace("+", "$+$")
            lines.append(f"  \\node[anchor=west, font=\\scriptsize, text={'hbslate' if j == 0 else 'hbink'}] at ({vx:.2f},{y:.3f}) {{{txt}}};")
    ly = top + 0.45
    for j, (rname, col, _, _a) in enumerate(runs):
        lx = x0 + j * 3.4
        lines.append(f"  \\fill[{col}] ({lx:.2f},{ly - 0.1:.2f}) rectangle ({lx + 0.3:.2f},{ly + 0.1:.2f}); \\node[anchor=west, font=\\footnotesize] at ({lx + 0.4:.2f},{ly:.2f}) {{{rname}}};")
    lines.append(f"  \\node[anchor=west, font=\\scriptsize, text=hbslate] at ({vx:.2f},{ly:.2f}) {{estimate [95\\% interval]}};")
    lines.append(f"  \\draw[hbink, line width=0.45pt, fill=white] ({x0 + 2.4:.2f},-1.0) circle (0.045); \\node[anchor=west, font=\\footnotesize] at ({x0 + 2.55:.2f},-1.0) {{one seed's paired value}};")
    lines.append(f"  \\node[font=\\footnotesize, anchor=north] at ({x0 + (hi_ax - lo_ax) * sc / 2:.2f},-0.45) {{change in latency relative to the comparison (negative is better)}};")
    lines.append("\\end{tikzpicture}}")
    (OUT / "prodflow.tex").write_text("\n".join(lines) + "\n")


def brokers_figure():
    """Bars: change against RabbitMQ or Redis FIFO for the broker experiment, with each criterion's pre-registered bar.

    Bars that met their bar are ink, bars that missed it are orange. Ratios are shown as ratio - 1. The RabbitMQ
    priority bar runs off the axis and is clipped, with its value printed.
    """
    c = json.loads((RES / "v3-aws/e3/summary.json").read_text())["evaluation"]["criteria"]
    # (key, label, bar as list of x positions in percent or None)
    rows = [("P1_mean_vs_rabbitmq_fifo", "Mean vs RabbitMQ FIFO", [-15]),
            ("P3_mean_vs_redis_fifo", "Mean vs Redis FIFO", [-15]),
            ("P2_short_p50_vs_rabbitmq_fifo", "Short-job median vs RabbitMQ FIFO", [-30]),
            ("G1_all_p99_vs_rabbitmq_fifo", "All-message P99 vs RabbitMQ FIFO", [25]),
            ("S1_fifo_equivalence_rabbitmq", "Harbinger FIFO vs RabbitMQ FIFO (mean)", [-10, 10]),
            ("S3_transfer_rabbitmq_predictive_vs_fifo", "Predictor as RabbitMQ sidecar, mean, vs RabbitMQ FIFO", [-15]),
            ("S6_weighted_mean_vs_rabbitmq_fifo", "Harbinger with weighted aging, mean, vs RabbitMQ FIFO", [-15]),
            ("S8_rabbitmq_priority_long_wait_vs_fifo", "RabbitMQ priority: long-job maximum wait vs FIFO", [100])]
    lo_ax, hi_ax = -100, 40
    x0, sc = 6.1, 0.0535
    zx = x0 + (0 - lo_ax) * sc
    gh, bh = 0.72, 0.36
    n = len(rows)
    top = n * gh
    xr = x0 + (hi_ax - lo_ax) * sc
    lines = ["\\resizebox{\\textwidth}{!}{%", "\\begin{tikzpicture}[font=\\small]"]
    for xt in range(-100, 50, 25):
        x = x0 + (xt - lo_ax) * sc
        if xt > hi_ax:
            continue
        tick = "0\\%" if xt == 0 else (f"$-${abs(xt)}\\%" if xt < 0 else f"$+${xt}\\%")
        lines.append(f"  \\draw[hbslate!25] ({x:.3f},0) -- ({x:.3f},{top:.2f});")
        lines.append(f"  \\node[font=\\scriptsize, text=hbslate, anchor=north] at ({x:.3f},-0.05) {{{tick}}};")
    lines.append(f"  \\draw[hbslate, line width=0.7pt] ({zx:.3f},0) -- ({zx:.3f},{top:.2f});")
    vx = xr + 0.3
    for i, (key, label, bars) in enumerate(rows):
        crit = c[key]
        sh = 1.0 if crit["kind"] == "ratio" else 0.0
        p, lo, hi = (100 * (v - sh) for v in (crit["point"], *crit["ci95"]))
        y = top - (i + 0.5) * gh
        col = "hbink" if crit["status"] == "pass" else "hbsignal"
        lines.append(f"  \\node[anchor=east, align=right] at ({x0 - 0.15:.2f},{y:.2f}) {{{label}}};")
        for b in (b for b in bars if b <= hi_ax):
            bx = zx + b * sc
            lines.append(f"  \\draw[hbslate, densely dashed, line width=0.7pt] ({bx:.3f},{y - gh * 0.42:.3f}) -- ({bx:.3f},{y + gh * 0.42:.3f});")
        clip = hi_ax - 2
        pe, loe, hie = min(p, clip), min(lo, clip), min(hi, clip)
        xa, xb = sorted((zx, zx + pe * sc))
        lines.append(f"  \\fill[{col}] ({xa:.3f},{y - bh / 2:.3f}) rectangle ({xb:.3f},{y + bh / 2:.3f});")
        if p > clip:
            bx = zx + clip * sc
            lines.append(f"  \\fill[white] ({bx - 0.22:.3f},{y - bh / 2 - 0.02:.3f}) -- ({bx - 0.02:.3f},{y - bh / 2 - 0.02:.3f}) -- ({bx + 0.18:.3f},{y + bh / 2 + 0.02:.3f}) -- ({bx - 0.02:.3f},{y + bh / 2 + 0.02:.3f}) -- cycle;")
        lines.append(f"  \\draw[hbink, line width=0.6pt] ({zx + loe * sc:.3f},{y:.3f}) -- ({zx + hie * sc:.3f},{y:.3f});")
        if p <= clip:
            for e in (loe, hie):
                lines.append(f"  \\draw[hbink, line width=0.6pt] ({zx + e * sc:.3f},{y - 0.07:.3f}) -- ({zx + e * sc:.3f},{y + 0.07:.3f});")
        txt = f"{p:+.1f}\\% [{lo:+.1f}, {hi:+.1f}]".replace("-", "$-$").replace("+", "$+$")
        lines.append(f"  \\node[anchor=west, font=\\scriptsize, text=hbink] at ({vx:.2f},{y:.3f}) {{{txt}}};")
    ly = top + 0.45
    lines.append(f"  \\fill[hbink] ({x0 - 5.0:.2f},{ly - 0.1:.2f}) rectangle ({x0 - 4.7:.2f},{ly + 0.1:.2f}); \\node[anchor=west, font=\\footnotesize] at ({x0 - 4.6:.2f},{ly:.2f}) {{met its bar}};")
    lines.append(f"  \\fill[hbsignal] ({x0 - 2.6:.2f},{ly - 0.1:.2f}) rectangle ({x0 - 2.3:.2f},{ly + 0.1:.2f}); \\node[anchor=west, font=\\footnotesize] at ({x0 - 2.2:.2f},{ly:.2f}) {{missed its bar}};")
    lines.append(f"  \\draw[hbslate, densely dashed, line width=0.7pt] ({x0 + 0.3:.2f},{ly - 0.12:.2f}) -- ({x0 + 0.3:.2f},{ly + 0.12:.2f}); \\node[anchor=west, font=\\footnotesize] at ({x0 + 0.45:.2f},{ly:.2f}) {{pre-registered bar}};")
    lines.append(f"  \\node[anchor=west, font=\\scriptsize, text=hbslate] at ({vx:.2f},{ly:.2f}) {{estimate [95\\% interval]}};")
    lines.append(f"  \\node[font=\\footnotesize, anchor=north] at ({x0 + (hi_ax - lo_ax) * sc / 2:.2f},-0.45) {{change relative to the comparison (negative is better)}};")
    lines.append("\\end{tikzpicture}}")
    (OUT / "brokers.tex").write_text("\n".join(lines) + "\n")


def traces_figure():
    """Forest plot: mean latency against FIFO for each trace and load, gate A beside gate B.

    One dot per cell with its 95% interval. A filled dot passed its pre-registered gate, a hollow one did not. The
    dashed line is the -15% bar and the solid line is no change. Intervals are clipped at the axis ends, with an arrowhead.
    """
    # Azure and Huawei come from the real-broker replay (v4-replay-1); the two HPC logs are simulated.
    traces = [("Azure 2021", "replay", "v4-replay/azure2021"), ("Huawei 2025", "replay", "v4-replay/huawei2025-r5"),
              ("LPC-EGEE", "simulation", "v3-traces-2/lpc-egee-2004"), ("DAS2-fs2", "simulation", "v3-traces-2/das2-fs2-2003")]
    loads = ("0.5", "0.8", "0.95")
    gates = (("A_P1_mean_latency_vs_fifo", "A_predictive_routing", "Gate A: strict priority"),
             ("B_P1_mean_latency_vs_fifo", "B_overload_design", "Gate B: weights, pausing aging"))
    lo_ax, hi_ax = -100, 100
    sc = 0.03                      # cm per percentage point
    pw = (hi_ax - lo_ax) * sc      # panel width
    gap = 1.9
    x_lab = 3.1                    # left of the first panel, where the labels go
    pitch, tgap = 0.42, 0.3
    n = len(traces) * len(loads)
    H = n * pitch + (len(traces) - 1) * tgap
    lines = ["\\resizebox{\\textwidth}{!}{%", "\\begin{tikzpicture}[font=\\small]"]
    for k, (ckey, gkey, title) in enumerate(gates):
        px = x_lab + k * (pw + gap)
        zx = px + (0 - lo_ax) * sc
        bx = px + (-15 - lo_ax) * sc
        for xt in range(lo_ax, hi_ax + 1, 50):
            x = px + (xt - lo_ax) * sc
            lines.append(f"  \\draw[hbslate!22] ({x:.3f},0) -- ({x:.3f},{H:.2f});")
            tick = "0\\%" if xt == 0 else (f"$-${abs(xt)}\\%" if xt < 0 else f"$+${xt}\\%")
            lines.append(f"  \\node[font=\\scriptsize, text=hbslate, anchor=north] at ({x:.3f},-0.05) {{{tick}}};")
        lines.append(f"  \\draw[hbslate, line width=0.7pt] ({zx:.3f},0) -- ({zx:.3f},{H:.2f});")
        lines.append(f"  \\draw[hbslate, dashed, line width=0.8pt] ({bx:.3f},0) -- ({bx:.3f},{H + 0.12:.2f});")
        lines.append(f"  \\node[font=\\scriptsize, text=hbslate, anchor=south] at ({bx:.3f},{H + 0.12:.2f}) {{bar $-$15\\%}};")
        lines.append(f"  \\node[font=\\footnotesize\\bfseries, anchor=south] at ({px + pw / 2:.2f},{H + 0.55:.2f}) {{{title}}};")
        lines.append(f"  \\node[font=\\scriptsize, anchor=north] at ({px + pw / 2:.2f},-0.6) {{mean latency vs FIFO (negative is better)}};")
    for ti, (tname, source, rel) in enumerate(traces):
        if source == "replay":
            ev = json.loads((RES / (rel + "/replay_summary.json")).read_text())["primary_block_bootstrap"]
        else:
            ev = json.loads((RES / (rel + "/summary.json")).read_text())["evaluation"]
        top = H - ti * (len(loads) * pitch + tgap)
        for li, ld in enumerate(loads):
            y = top - (li + 0.5) * pitch
            lines.append(f"  \\node[anchor=east, font=\\scriptsize, text=hbslate] at ({x_lab - 0.15:.2f},{y:.3f}) {{load {ld}}};")
            for k, (ckey, gkey, _) in enumerate(gates):
                px = x_lab + k * (pw + gap)
                c = ev[ld]["criteria"][ckey]
                passed = ev[ld]["gates_passed"][gkey]
                p, lo, hi = (100 * v for v in (c["point"], *c["ci95"]))
                X = lambda v: px + (max(lo_ax, min(hi_ax, v)) - lo_ax) * sc
                if hi - lo > 0.4:
                    lines.append(f"  \\draw[hbink, line width=0.7pt] ({X(lo):.3f},{y:.3f}) -- ({X(hi):.3f},{y:.3f});")
                    for e, past in ((lo, lo < lo_ax), (hi, hi > hi_ax)):
                        if past:
                            d = 1 if e > hi_ax else -1
                            lines.append(f"  \\fill[hbink] ({X(e) + d * 0.1:.3f},{y:.3f}) -- ({X(e) - d * 0.02:.3f},{y + 0.07:.3f}) -- ({X(e) - d * 0.02:.3f},{y - 0.07:.3f}) -- cycle;")
                        else:
                            lines.append(f"  \\draw[hbink, line width=0.7pt] ({X(e):.3f},{y - 0.06:.3f}) -- ({X(e):.3f},{y + 0.06:.3f});")
                if passed:
                    lines.append(f"  \\fill[hbink] ({X(p):.3f},{y:.3f}) circle (0.085);")
                else:
                    lines.append(f"  \\draw[hbink, line width=0.8pt, fill=white] ({X(p):.3f},{y:.3f}) circle (0.085);")
        mid = top - len(loads) * pitch / 2
        lines.append(f"  \\node[anchor=east, font=\\footnotesize\\bfseries, align=right] at (1.2,{mid + 0.05:.3f}) {{{tname}}};")
        lines.append(f"  \\node[anchor=east, font=\\tiny, text=hbslate] at (1.2,{mid - 0.22:.3f}) {{{source}}};")
        if ti:
            lines.append(f"  \\draw[hbslate!35, dashed] (-0.2,{top + tgap / 2:.3f}) -- ({x_lab + 2 * pw + gap:.2f},{top + tgap / 2:.3f});")
    ly = -1.4
    lines.append(f"  \\fill[hbink] ({x_lab + 0.2:.2f},{ly:.2f}) circle (0.085); \\node[anchor=west, font=\\footnotesize] at ({x_lab + 0.4:.2f},{ly:.2f}) {{gate passed}};")
    lines.append(f"  \\draw[hbink, line width=0.8pt, fill=white] ({x_lab + 3.6:.2f},{ly:.2f}) circle (0.085); \\node[anchor=west, font=\\footnotesize] at ({x_lab + 3.8:.2f},{ly:.2f}) {{gate not passed}};")
    lines.append(f"  \\draw[hbink, line width=0.7pt] ({x_lab + 7.2:.2f},{ly:.2f}) -- ({x_lab + 7.8:.2f},{ly:.2f}); \\node[anchor=west, font=\\footnotesize] at ({x_lab + 7.95:.2f},{ly:.2f}) {{95\\% interval (arrowhead: runs past the axis)}};")
    lines.append("\\end{tikzpicture}}")
    (OUT / "traces.tex").write_text("\n".join(lines) + "\n")


def cobham_figure():
    """Mean wait against FIFO for the two-class non-preemptive priority queue, from the closed forms of Section 5.1.

    A model illustration, not a measurement. Same formulas as gen_numbers.py (Cobham; Kleinrock's conservation law).
    """
    def two_class(p_short, size_ratio, rho):
        pi = p_short / (p_short + (1 - p_short) * size_ratio)
        rho_s = pi * rho
        return pi, (1 - rho) / (1 - rho_s), 1 / (1 - rho_s), (1 - p_short * rho) / (1 - rho_s)

    panels = [(0.9, 10), (0.9, 100), (0.5, 100)]
    series = (("Short jobs", "hbink", ""), ("Long jobs", "hbslate!70", "densely dashed"), ("All jobs", "hbsignal", ""))
    W, Hh, gap = 4.6, 4.0, 1.15
    ymax = 2.0
    r0, r1 = 0.3, 0.97
    lines = ["\\resizebox{\\textwidth}{!}{%", "\\begin{tikzpicture}[font=\\small]"]
    for pi_, (p, r) in enumerate(panels):
        x0 = pi_ * (W + gap)
        pi = two_class(p, r, 0.5)[0]
        lines.append(f"  \\node[font=\\footnotesize, anchor=south, align=center] at ({x0 + W / 2:.2f},{Hh + 0.12:.2f}) {{{round(p * 100)}\\% short, long jobs {r}$\\times$ larger\\\\[-1pt]\\scriptsize\\color{{hbslate}} (short jobs: {pi * 100:.0f}\\% of the work)}};")
        for yt in (0, 0.5, 1.0, 1.5, 2.0):
            y = yt / ymax * Hh
            lines.append(f"  \\draw[{'hbslate' if yt == 1.0 else 'hbslate!25'}{', dashed' if yt == 1.0 else ''}] ({x0:.2f},{y:.3f}) -- ({x0 + W:.2f},{y:.3f});")
            if pi_ == 0:
                lines.append(f"  \\node[font=\\scriptsize, text=hbslate, anchor=east] at ({x0 - 0.1:.2f},{y:.3f}) {{{yt:g}}};")
        for xt in (0.3, 0.5, 0.7, 0.9):
            x = x0 + (xt - r0) / (r1 - r0) * W
            lines.append(f"  \\node[font=\\scriptsize, text=hbslate, anchor=north] at ({x:.3f},-0.05) {{{xt:g}}};")
        lines.append(f"  \\node[font=\\footnotesize, anchor=north] at ({x0 + W / 2:.2f},-0.5) {{offered load $\\rho$}};")
        n = 60
        for k, (name, col, dash) in enumerate(series):
            pts = []
            for i in range(n + 1):
                rho = r0 + (r1 - r0) * i / n
                v = two_class(p, r, rho)[1 + k]
                pts.append(f"({x0 + (rho - r0) / (r1 - r0) * W:.3f},{min(v, ymax) / ymax * Hh:.3f})")
            lines.append(f"  \\draw[{col}, line width=1.1pt{', ' + dash if dash else ''}] " + " -- ".join(pts) + ";")
            for rho in (0.5, 0.8, 0.95):
                v = two_class(p, r, rho)[1 + k]
                lines.append(f"  \\fill[{col}] ({x0 + (rho - r0) / (r1 - r0) * W:.3f},{v / ymax * Hh:.3f}) circle (0.05);")
        # direct label at the right end of each line
        for k, (name, col, dash) in enumerate(series):
            v = two_class(p, r, r1)[1 + k]
            if pi_ == 2 and k == 1:
                continue
    if True:
        ly = Hh + 1.15
        for k, (name, col, dash) in enumerate(series):
            lx = 0.0 + k * 3.6
            lines.append(f"  \\draw[{col}, line width=1.1pt{', ' + dash if dash else ''}] ({lx:.2f},{ly:.2f}) -- ({lx + 0.7:.2f},{ly:.2f}); \\node[anchor=west, font=\\footnotesize] at ({lx + 0.85:.2f},{ly:.2f}) {{{name}}};")
        lines.append(f"  \\node[anchor=west, font=\\scriptsize, text=hbslate] at ({11.2:.2f},{ly:.2f}) {{dots mark loads 0.5, 0.8 and 0.95}};")
    lines.append(f"  \\node[font=\\footnotesize, rotate=90, anchor=south] at (-0.75,{Hh / 2:.2f}) {{mean wait relative to FIFO}};")
    lines.append("\\end{tikzpicture}}")
    (OUT / "cobham.tex").write_text("\n".join(lines) + "\n")


def perclass_figure():
    """Per-class latency in the AWS production flow: FIFO beside predictive routing, median P50 and P99 over five seeds.

    Classes are ordered by median service time, cheapest first. Whiskers span the five seeds (minimum to maximum).
    The axis is logarithmic because the classes' latencies differ by three orders of magnitude.
    """
    import math
    import statistics
    d = json.loads((RES / "v3-aws/e1/summary.json").read_text())["arms"]
    classes = list(d["fifo"]["11"]["per_class"])
    svc = {c: statistics.median(d["fifo"][s]["per_class"][c]["median_service_ms"] for s in d["fifo"]) for c in classes}
    order = sorted(classes, key=svc.get)
    fmt_ms = lambda v: f"{v / 1000:.2f} s" if v >= 1000 else f"{v:.3g} ms"
    names = {c: c.replace("_", "\\_") for c in classes}
    arms = (("fifo", "FIFO", "hbslate!55"), ("predictive", "Predictive", "hbink"))
    lo, hi = 5.0, 100000.0
    W, x_lab = 5.4, 5.6
    gap = 0.9
    gh, bh = 0.78, 0.28
    n = len(order)
    top = n * gh
    L = lambda v: math.log10(max(v, lo) / lo) / math.log10(hi / lo) * W
    lines = ["\\resizebox{\\textwidth}{!}{%", "\\begin{tikzpicture}[font=\\small]"]
    for pi_, (key, title) in enumerate((("p50_ms", "Median latency (P50)"), ("p99_ms", "P99 latency"))):
        x0 = x_lab + pi_ * (W + gap)
        lines.append(f"  \\node[font=\\footnotesize\\bfseries, anchor=south] at ({x0 + W / 2:.2f},{top + 0.1:.2f}) {{{title}}};")
        for v, lab in ((10, "10 ms"), (100, "100 ms"), (1000, "1 s"), (10000, "10 s"), (100000, "100 s")):
            x = x0 + L(v)
            lines.append(f"  \\draw[hbslate!25] ({x:.3f},0) -- ({x:.3f},{top:.2f});")
            lines.append(f"  \\node[font=\\scriptsize, text=hbslate, anchor=north] at ({x:.3f},-0.05) {{{lab}}};")
        lines.append(f"  \\draw[hbslate, line width=0.7pt] ({x0:.2f},0) -- ({x0:.2f},{top:.2f});")
        for i, c in enumerate(order):
            yc = top - (i + 0.5) * gh
            if pi_ == 0:
                lines.append(f"  \\node[anchor=east, align=right] at ({x_lab - 0.15:.2f},{yc:.2f}) {{{names[c]}\\\\[-2pt]\\scriptsize\\color{{hbslate}} service {fmt_ms(svc[c])}}};")
            for j, (arm, _, col) in enumerate(arms):
                vals = [d[arm][sd]["per_class"][c][key] for sd in d[arm]]
                med, vmin, vmax = statistics.median(vals), min(vals), max(vals)
                y = yc + (0.5 - j) * (bh + 0.03)
                lines.append(f"  \\fill[{col}] ({x0:.3f},{y - bh / 2:.3f}) rectangle ({x0 + L(med):.3f},{y + bh / 2:.3f});")
                lines.append(f"  \\draw[hbink, line width=0.5pt] ({x0 + L(vmin):.3f},{y:.3f}) -- ({x0 + L(vmax):.3f},{y:.3f});")
                for e in (vmin, vmax):
                    lines.append(f"  \\draw[hbink, line width=0.5pt] ({x0 + L(e):.3f},{y - 0.06:.3f}) -- ({x0 + L(e):.3f},{y + 0.06:.3f});")
    ly = top + 0.95
    for k, (_, name, col) in enumerate(arms):
        lx = x_lab + k * 2.6
        lines.append(f"  \\fill[{col}] ({lx:.2f},{ly - 0.1:.2f}) rectangle ({lx + 0.3:.2f},{ly + 0.1:.2f}); \\node[anchor=west, font=\\footnotesize] at ({lx + 0.4:.2f},{ly:.2f}) {{{name}}};")
    lines.append(f"  \\node[anchor=west, font=\\scriptsize, text=hbslate] at ({x_lab + 5.4:.2f},{ly:.2f}) {{bars: median over five seeds; whiskers: seed minimum to maximum}};")
    lines.append("\\end{tikzpicture}}")
    (OUT / "perclass.tex").write_text("\n".join(lines) + "\n")


histogram_figure()
weights_figure()
aging_figure()
prodflow_figure()
brokers_figure()
traces_figure()
cobham_figure()
perclass_figure()
print("wrote histogram, weights, aging, prodflow, brokers, traces, cobham and perclass figures")
