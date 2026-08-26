#!/usr/bin/env python3
"""Уход частоты часов или джиттер сети? Ищет линейный тренд в отрезках.

Постоянное смещение часов сокращается в b_jitter, а вот уход *частоты* -- нет:
за прогон он размазывает B плавной прямой. Сетевой джиттер стационарен и
прямой не даёт. a_ns и c_ns снимаются одними часами внутри хоста и служат
контролем: у них тренда быть не должно ни при каком раскладе.

R^2 отличает одно от другого лучше самого наклона: уход частоты почти идеально
линеен, а нарастающая загрузка сети -- нет.

    ./scripts/drift.py results/rx-20260826-145359/latencies.csv
    ./scripts/drift.py results/rx-*/latencies.csv --rate 200000
"""
import argparse

import numpy as np


def load(path):
    """Колонки a_ns, b_ns, c_ns. pandas быстрее, но его может не быть на
    стенде -- тогда обычный разбор, он медленнее, но работает везде."""
    try:
        import pandas as pd
        df = pd.read_csv(path)
        return {c: df[c].to_numpy(dtype=np.float64)
                for c in ("a_ns", "b_ns", "c_ns")}
    except ImportError:
        with open(path) as f:
            cols = f.readline().strip().split(",")
            idx = [cols.index(c) for c in ("a_ns", "b_ns", "c_ns")]
            rows = [[int(p[i]) for i in idx]
                    for p in (line.split(",") for line in f)]
        arr = np.array(rows, dtype=np.float64)
        return dict(zip(("a_ns", "b_ns", "c_ns"), arr.T))


def analyse(path, rate):
    data = load(path)
    n = len(data["b_ns"])
    x = np.arange(n, dtype=np.float64)

    print(f"\n{path}  ({n} замеров)")
    print(f"{'':<6}{'p50':>10}{'p99':>10}{'наклон':>12}{'разгон':>10}"
          f"{'R^2':>8}{'p99 без тренда':>16}")

    for col in ("a_ns", "b_ns", "c_ns"):
        y = data[col]
        slope, intercept = np.polyfit(x, y, 1)
        resid = y - (slope * x + intercept)

        var = y.var()
        r2 = 0.0 if var == 0 else 1.0 - resid.var() / var
        ramp = slope * (n - 1)          # насколько уехало от начала к концу
        p50, p99 = np.percentile(y, [50, 99])
        # Хвост, измеренный от медианы, до и после снятия тренда.
        spread = p99 - p50
        spread_flat = np.percentile(resid, 99) - np.percentile(resid, 50)

        print(f"{col:<6}{p50:>10.0f}{p99:>10.0f}{slope:>12.4f}"
              f"{ramp:>10.0f}{r2:>8.3f}{spread_flat:>16.0f}")

        if col == "b_ns":
            b_verdict = (ramp, spread, r2)

    verdict(*b_verdict, n, rate)


def verdict(ramp, spread, r2, n, rate):
    print()
    if abs(ramp) < spread:
        print("   => разгон B за прогон меньше его же хвоста: уходом частоты "
              "объяснять нечего, джиттер сетевой.")
    elif r2 > 0.5:
        print("   => разгон крупный И почти идеально линейный: это уход "
              "частоты часов, а не сеть. Снимать тренд перед сравнением.")
    else:
        print("   => разгон крупный, но нелинейный: похоже на плывущую "
              "загрузку сети, а не на часы.")
    if rate:
        secs = n / rate
        print(f"   разгон {ramp:.0f} нс за {secs:.1f} с = "
              f"{ramp / secs / 1000:.3f} ppm относительного ухода")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="+")
    ap.add_argument("--rate", type=float, default=0,
                    help="сообщений в секунду, чтобы перевести разгон в ppm")
    args = ap.parse_args()
    for path in args.csv:
        analyse(path, args.rate)


if __name__ == "__main__":
    main()
