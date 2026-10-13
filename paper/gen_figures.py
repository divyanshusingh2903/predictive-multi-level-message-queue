#!/usr/bin/env python3
"""Generate the two mechanism illustrations (histogram and weighted share) as TikZ.

Both are computed from the rules in Section 3, not measured: the histogram shapes are
hand-chosen examples whose medians, p99s and boundaries are then derived by the same
quantile rules the broker uses, and the share strip applies the virtual-clock rule.
"""
import collections
import pathlib
import random

OUT = pathlib.Path(__file__).resolve().parent / "figures"
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


histogram_figure()
weights_figure()
aging_figure()
print("wrote histogram, weights and aging figures")
