#!/usr/bin/env python3
"""test_perf JSON 对照 cpp/baseline.json：绝对地板 + 相对上一历史点；写 JSONL 并出 SVG。"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path


def load_got(path: Path) -> dict:
    lines = path.read_text(encoding="utf-8").strip().splitlines()
    if not lines:
        raise SystemExit(f"empty perf output: {path}")
    return json.loads(lines[-1])


def floor_check(got: dict, base: dict) -> list[str]:
    tol = float(base.get("tolerance", 0.15))
    floors = base["metrics"]
    mapping = {
        "add_qps": "add_qps_min",
        "search_qps": "search_qps_min",
        "batch_qps": "batch_qps_min",
    }
    failed = []
    for gk, fk in mapping.items():
        need = floors[fk] * (1.0 - tol)
        val = float(got[gk])
        print(f"{gk}={val:.1f} floor={need:.1f} (min={floors[fk]} tol={tol})")
        if val < need:
            failed.append(gk)
    return failed


def last_hist(hist: Path) -> dict | None:
    if not hist.is_file():
        return None
    lines = [ln for ln in hist.read_text(encoding="utf-8").splitlines() if ln.strip()]
    if not lines:
        return None
    return json.loads(lines[-1])


def relative_check(got: dict, prev: dict | None, tol: float) -> list[str]:
    if prev is None:
        print("relative: no history, skip")
        return []
    failed = []
    for k in ("add_qps", "search_qps", "batch_qps"):
        if k not in prev or k not in got:
            continue
        old = float(prev[k])
        new = float(got[k])
        if old <= 0:
            continue
        drop = (old - new) / old
        print(f"relative {k}: {old:.1f} -> {new:.1f} drop={drop:.1%}")
        if drop > tol:
            failed.append(k)
    return failed


def append_hist(hist: Path, got: dict, sha: str, ref: str) -> None:
    row = {
        "ts": int(time.time()),
        "sha": sha,
        "ref": ref,
        "add_qps": float(got["add_qps"]),
        "search_qps": float(got["search_qps"]),
        "batch_qps": float(got["batch_qps"]),
    }
    hist.parent.mkdir(parents=True, exist_ok=True)
    with hist.open("a", encoding="utf-8") as f:
        f.write(json.dumps(row, separators=(",", ":")) + "\n")


def write_svg(hist: Path, out: Path) -> None:
    rows = []
    if hist.is_file():
        for ln in hist.read_text(encoding="utf-8").splitlines():
            if ln.strip():
                rows.append(json.loads(ln))
    if len(rows) < 1:
        out.write_text("<svg xmlns='http://www.w3.org/2000/svg' width='400' height='80'>"
                       "<text x='12' y='40'>no perf history</text></svg>\n", encoding="utf-8")
        return
    w, h, pad = 720, 240, 36
    keys = [("add_qps", "#1f77b4"), ("search_qps", "#ff7f0e"), ("batch_qps", "#2ca02c")]
    ymax = max(float(r[k]) for r in rows for k, _ in keys) * 1.1 or 1.0
    n = max(len(rows) - 1, 1)

    def xy(i: int, v: float) -> tuple[float, float]:
        x = pad + (w - 2 * pad) * (i / n)
        y = h - pad - (h - 2 * pad) * (v / ymax)
        return x, y

    parts = [
        f"<svg xmlns='http://www.w3.org/2000/svg' width='{w}' height='{h}'>",
        f"<rect width='100%' height='100%' fill='#fafafa'/>",
        f"<text x='{pad}' y='20' font-size='14'>test_perf QPS trend (n={len(rows)})</text>",
    ]
    for k, color in keys:
        pts = []
        for i, r in enumerate(rows):
            x, y = xy(i, float(r[k]))
            pts.append(f"{x:.1f},{y:.1f}")
        parts.append(f"<polyline fill='none' stroke='{color}' stroke-width='2' points='{' '.join(pts)}'/>")
        parts.append(f"<text x='{w - pad - 120}' y='{20 + 16 * keys.index((k, color))}' "
                     f"font-size='12' fill='{color}'>{k}</text>")
    parts.append("</svg>\n")
    out.write_text("\n".join(parts), encoding="utf-8")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--perf", type=Path, required=True, help="test_perf stdout file")
    ap.add_argument("--baseline", type=Path, required=True)
    ap.add_argument("--hist", type=Path, required=True)
    ap.add_argument("--svg", type=Path, required=True)
    ap.add_argument("--sha", default="")
    ap.add_argument("--ref", default="")
    ap.add_argument("--append", action="store_true")
    ap.add_argument("--relative", action="store_true", help="fail if drop vs last hist > tolerance")
    args = ap.parse_args()

    got = load_got(args.perf)
    base = json.loads(args.baseline.read_text(encoding="utf-8"))
    tol = float(base.get("tolerance", 0.15))

    failed = floor_check(got, base)
    if args.relative:
        failed.extend(relative_check(got, last_hist(args.hist), tol))

    if args.append:
        append_hist(args.hist, got, args.sha, args.ref)
    write_svg(args.hist if args.hist.is_file() or args.append else args.hist, args.svg)

    if failed:
        print("FAIL:", ", ".join(sorted(set(failed))), file=sys.stderr)
        return 1
    print("perfcheck ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
