#!/usr/bin/env python3
"""Перцентили по CSV из consumer'а, с необязательной коррекцией часов.

    ./scripts/summarize.py results/rx-*/latencies.csv
    ./scripts/summarize.py latencies.csv --offset-before 18608 --offset-after -1335

Отрезки:
    a = tx_ts - send_ts    кольцо -> отправитель   одни часы, точен
    b = rx_ts - tx_ts      провод                  РАЗНЫЕ часы
    c = recv_ts - rx_ts    приёмник -> consumer    одни часы, точен

b равен настоящему времени провода плюс смещение между хостами, поэтому
абсолютные e2e и b без коррекции недостоверны. a и c достоверны всегда.

Несколько файлов объединяются в ОДНУ выборку, а не усредняются по файлам:
перцентиль от N*M отсчётов и медиана из N перцентилей -- разные величины, и на
дальнем хвосте расходятся в разы, потому что усреднение выбрасывает как раз те
файлы, где лежат редкие события.
"""
import argparse
import glob
import sys

import numpy as np

COLS = ["send_ts_ns", "a_ns", "b_ns", "c_ns"]
LEVELS = [("min", None), ("p50", .50), ("p99", .99), ("p99.9", .999),
          ("p99.99", .9999), ("p99.999", .99999), ("max", None)]


def load(paths):
    """pandas быстрее на миллионах строк, но его может не быть на стенде."""
    try:
        import pandas as pd
        frames = [pd.read_csv(p, usecols=COLS) for p in paths]
        df = pd.concat(frames, ignore_index=True) if len(frames) > 1 else frames[0]
        return {c: df[c].to_numpy(dtype=np.int64) for c in COLS}
    except ImportError:
        out = {c: [] for c in COLS}
        for p in paths:
            opener = __import__("gzip").open if p.endswith(".gz") else open
            with opener(p, "rt") as f:
                head = f.readline().strip().split(",")
                idx = [head.index(c) for c in COLS]
                for line in f:
                    v = line.split(",")
                    for c, i in zip(COLS, idx):
                        out[c].append(int(v[i]))
        return {c: np.asarray(v, dtype=np.int64) for c, v in out.items()}


def pct(srt, p):
    """Ранг ближайшего, как в metrics.h -- чтобы числа сходились с выводом consumer."""
    n = srt.size
    rank = int(p * n)
    if rank < p * n:
        rank += 1
    return int(srt[max(1, min(rank, n)) - 1])


def row(name, vals):
    srt = np.sort(vals)
    cells = []
    for lbl, p in LEVELS:
        v = int(srt[0]) if lbl == "min" else int(srt[-1]) if lbl == "max" else pct(srt, p)
        cells.append(f"{v:>12,}")
    print(f"  {name:<16}" + "".join(cells))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="+", help="один или несколько файлов; объединяются в одну выборку")
    ap.add_argument("--offset-before", type=int, help="offset_ns от clock_probe ДО прогона")
    ap.add_argument("--offset-after", type=int, help="offset_ns от clock_probe ПОСЛЕ прогона")
    args = ap.parse_args()

    paths = sorted({p for pat in args.csv for p in (glob.glob(pat) or [pat])})
    d = load(paths)
    ts, a, b, c = d["send_ts_ns"], d["a_ns"], d["b_ns"], d["c_ns"]
    n = ts.size
    t0, t1 = int(ts.min()), int(ts.max())

    print(f"файлов       : {len(paths)}" + ("" if len(paths) == 1 else f"  (объединены в одну выборку)"))
    for p in paths:
        print(f"               {p}")
    print(f"отсчётов     : {n:,}")
    print(f"длительность : {(t1 - t0) / 1e9:.1f} с")

    have = args.offset_before is not None and args.offset_after is not None
    if (args.offset_before is None) != (args.offset_after is None):
        sys.exit("нужны обе границы: --offset-before и --offset-after")

    if have:
        span = t1 - t0
        if span <= 0:
            sys.exit("все отсчёты с одним send_ts_ns -- прогон слишком короткий")
        drift = args.offset_after - args.offset_before
        ppm = drift / span * 1e6
        print(f"коррекция    : {args.offset_before:,} -> {args.offset_after:,} нс "
              f"(уход {drift:,} нс = {ppm:.3f} ppm)")
        if abs(ppm) > 2.0:
            print("  ВНИМАНИЕ: больше 2 ppm. Для остановленного chrony это много --")
            print("            проверьте, что он остановлен на ОБЕИХ машинах.")
        off = args.offset_before + drift * (ts - t0) / span
        b_cor = np.rint(b - off).astype(np.int64)
    print()

    hdr = "".join(f"{lbl:>12}" for lbl, _ in LEVELS)
    print(f"  {'':<16}{hdr}")
    row("a", a)
    row("c", c)
    print("  " + "-" * (16 + 12 * len(LEVELS)))
    row("b", b)
    row("e2e", a + b + c)
    if have:
        print("  " + "-" * (16 + 12 * len(LEVELS)))
        row("b испр.", b_cor)
        row("e2e испр.", a + b_cor + c)

    print()
    bj = b - b.min()
    row("b_jitter", bj)
    print("  (b_jitter = b - min(b): постоянное смещение сокращается, коррекция не нужна)")

    if have:
        neg = int((b_cor < 0).sum())
        if neg:
            print(f"\n  ВНИМАНИЕ: {neg:,} отсчётов ({neg / n * 100:.2f}%) дали отрицательный b.")
            print("            Коррекция завышена: часы разъехались не так, как описывает прямая.")


if __name__ == "__main__":
    main()
