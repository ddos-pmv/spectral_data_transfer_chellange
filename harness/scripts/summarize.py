#!/usr/bin/env python3
"""Перцентили по CSV из consumer'а.

    ./scripts/summarize.py results/rx-*/latencies.csv
    ./scripts/summarize.py results/rx-*/latencies.csv --offset -12222
    ./scripts/summarize.py results/rx-*/latencies.csv --offset -12222 --offset-end -12261

Отрезки:
    a = tx_ts - send_ts    кольцо -> отправитель   одни часы, точен
    b = rx_ts - tx_ts      провод                  РАЗНЫЕ часы
    c = recv_ts - rx_ts    приёмник -> consumer    одни часы, точен

b равен настоящему времени провода плюс смещение между хостами, поэтому без
--offset абсолютные b и e2e недостоверны. a и c достоверны всегда.

Оба смещения печатает tx.sh в конце прогона (он же выводит готовую строку
вызова). С двумя поправка интерполируется по времени каждого отсчёта, с одним --
берётся константой.

И то и другое законно только при ОСТАНОВЛЕННОМ chrony: тогда часы расходятся
линейно, а его поправки каждые 2 с делают смещение непредсказуемым. Мерить надо
для каждого прогона -- между прогонами смещение уходит заметно (6.4 мкс за шесть
минут, 0.018 ppm).

Константы хватает, пока уход за прогон мал против измеряемого: на 13 с это
39..675 нс при b порядка 17 мкс. На многоминутном прогоне накопится микросекунды,
и тогда нужен --offset-end.

Несколько файлов объединяются в ОДНУ выборку, а не усредняются по файлам:
перцентиль от N*M отсчётов и медиана из N перцентилей -- разные величины, и на
дальнем хвосте расходятся в разы.
"""
import argparse
import glob

import numpy as np

COLS = ["send_ts_ns", "a_ns", "b_ns", "c_ns"]
LEVELS = [("min", None), ("p50", .50), ("p99", .99), ("p99.9", .999),
          ("p99.99", .9999), ("p99.999", .99999), ("max", None)]


def load(paths):
    try:
        import pandas as pd
        f = [pd.read_csv(p, usecols=COLS) for p in paths]
        df = pd.concat(f, ignore_index=True) if len(f) > 1 else f[0]
        return {c: df[c].to_numpy(dtype=np.int64) for c in COLS}
    except ImportError:
        import csv
        out = {c: [] for c in COLS}
        for p in paths:
            op = __import__("gzip").open if p.endswith(".gz") else open
            with op(p, "rt", newline="") as fh:
                for r in csv.DictReader(fh):
                    for c in COLS:
                        out[c].append(int(r[c]))
        return {c: np.asarray(v, dtype=np.int64) for c, v in out.items()}


def pct(srt, p):
    """Ранг ближайшего, как в metrics.h -- чтобы числа сходились с consumer'ом."""
    n = srt.size
    rank = int(p * n)
    if rank < p * n:
        rank += 1
    return int(srt[max(1, min(rank, n)) - 1])


def row(name, vals):
    srt = np.sort(vals)
    cells = "".join(
        f"{(int(srt[0]) if l == 'min' else int(srt[-1]) if l == 'max' else pct(srt, p)):>12,}"
        for l, p in LEVELS)
    print(f"  {name:<14}{cells}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv", nargs="+")
    ap.add_argument("--offset", type=int,
                    help="смещение часов в нс до прогона, из вывода tx.sh")
    ap.add_argument("--offset-end", type=int,
                    help="смещение после прогона, из вывода tx.sh; "
                         "с ним поправка интерполируется по времени, без него постоянна")
    a = ap.parse_args()
    if a.offset_end is not None and a.offset is None:
        ap.error("--offset-end без --offset")

    paths = sorted({p for pat in a.csv for p in (glob.glob(pat) or [pat])})
    d = load(paths)
    ts, A, B, C = d["send_ts_ns"], d["a_ns"], d["b_ns"], d["c_ns"]

    for p in paths:
        print(f"  {p}")
    print(f"  {ts.size:,} отсчётов, {(int(ts.max()) - int(ts.min())) / 1e9:.1f} с")
    if a.offset is not None:
        if a.offset_end is None:
            print(f"  смещение часов: {a.offset:,} нс (постоянное)")
        else:
            span = int(ts.max()) - int(ts.min())
            drift = a.offset_end - a.offset
            ppm = drift / span * 1e6 if span else 0.0
            print(f"  смещение часов: {a.offset:,} -> {a.offset_end:,} нс "
                  f"(уход {drift:+,} нс = {ppm:.3f} ppm)")
            if abs(ppm) > 2.0:
                print("  ВНИМАНИЕ: больше 2 ppm -- проверьте, что chrony остановлен на ОБЕИХ машинах.")
    print()

    print(f"  {'':<14}" + "".join(f"{l:>12}" for l, _ in LEVELS))
    row("a", A)
    row("c", C)
    print("  " + "-" * (14 + 12 * len(LEVELS)))
    if a.offset is None:
        row("b", B)
        row("e2e", A + B + C)
        print("\n  b и e2e без --offset несут смещение часов и абсолютными не являются.")
    else:
        if a.offset_end is None:
            off = a.offset
        else:
            t0 = int(ts.min())
            span = int(ts.max()) - t0
            off = a.offset + (a.offset_end - a.offset) * (ts - t0) / span if span else a.offset
        Bc = np.rint(B - off).astype(np.int64)
        row("b", Bc)
        row("e2e", A + Bc + C)
        neg = int((Bc < 0).sum())
        if neg:
            print(f"\n  ВНИМАНИЕ: {neg:,} отсчётов ({neg / Bc.size * 100:.2f}%) с отрицательным b --"
                  f" смещение снято неверно.")


if __name__ == "__main__":
    main()
