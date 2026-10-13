#!/usr/bin/env python3
"""Generate the two mechanism illustrations (histogram and weighted share) as TikZ.

Both are computed from the rules in Section 3, not measured: the histogram shapes are
hand-chosen examples whose medians, p99s and boundaries are then derived by the same
quantile rules the broker uses, and the share strip applies the virtual-clock rule.
"""
import pathlib

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


histogram_figure()
weights_figure()
print("wrote figures/histogram.tex and figures/weights.tex")
