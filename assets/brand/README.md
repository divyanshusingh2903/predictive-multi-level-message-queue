# Harbinger brand

The mark is three queue levels and one dot. The bars are MLFQ levels, with the
shortest (predicted-fast) job on top at L0. The dot sits ahead of the stack:
the message whose cost was predicted, dispatched first. That's the harbinger.

## Files

| File | Use |
|---|---|
| `harbinger-mark.svg` / `-dark.svg` / `-mono.svg` | Mark alone, light / dark backgrounds / one-colour print |
| `harbinger-lockup.svg` / `-dark.svg` / `-mono.svg` | Mark + wordmark (text is outlined, no font needed) |
| `harbinger-icon.svg`, `png/harbinger-icon-{16,32,180,512}.png` | App icon, favicon, avatar |
| `harbinger-social.svg`, `png/harbinger-social.png` | GitHub social preview (1280×640) |
| `pdf/harbinger-{mark,lockup}{,-mono}.pdf` | Vector for LaTeX: `\includegraphics[height=1.2em]{harbinger-mark-mono}` |

## Colour

| Token | Light | Dark | Role |
|---|---|---|---|
| Ink | `#15171C` | `#ECEBE6` | Bars, wordmark, text |
| Paper | `#F5F4EF` | `#0F1115` | Background |
| Signal | `#E4572E` | `#F0643C` | The dot only |
| Slate | `#5B616E` | `#9AA0AC` | Secondary text, taglines |

Signal is reserved for the dot. In figures, use it for Harbinger's series and
keep the baselines (FIFO, static priority, round-robin) in greys.

## Type

- **Wordmark:** Fira Code Medium (500), lowercase, tracking −0.045 em. Outlined in the SVGs.
- **Supporting text:** Fira Code for code and labels; the paper keeps its LaTeX template fonts.

## Rules

- Clear space around the mark: at least the height of one bar.
- Minimum size: 16 px for the icon, 96 px wide for the lockup.
- Don't recolour the bars, rotate the mark, add effects, or move the dot.
- The name is written **Harbinger** in prose and `harbinger` in the wordmark.

## README header

```html
<picture>
  <source media="(prefers-color-scheme: dark)" srcset="assets/brand/harbinger-lockup-dark.svg">
  <img alt="Harbinger" src="assets/brand/harbinger-lockup.svg" height="56">
</picture>
```
