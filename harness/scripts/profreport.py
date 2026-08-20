"""Сборка results/<ts>/prof/index.html из того, что оставили профайлеры.

Задача отчёта — поставить рядом две вещи, которые иначе лежат в разных файлах:
хвост латентности точки и то, чем в этот момент занимался каждый бинарник.
Только stdlib: bench.py ничего не требует сверх компилятора и python3.
"""
import html
import re
from pathlib import Path

# В логах перемешаны обычные пробелы, NBSP и тонкие пробелы как разделители
# разрядов (локаль ru_RU), плюс запятая как десятичный разделитель. Читаем их
# явно как utf-8: локальная кодировка зависит от LANG, под LANG=C это ASCII и
# те же NBSP превратились бы в U+FFFD ещё до парсера.
_SPACES = dict.fromkeys(map(ord, " \xa0\u202f\u2009"), None)
_COLS = re.compile(r"\s{2,}")
_ELAPSED = re.compile(r"([\d,.]+)\s+seconds time elapsed")
_TAG = re.compile(r"^(r\d+-f\d+-\d+)(?:-(.+))?$")

ROLE_ORDER = {"producer": 0, "sender": 1, "receiver": 2, "rx": 3}


def _num(s):
    s = s.strip()
    if s.startswith("<"):            # <not counted> / <not supported>
        return None
    s = s.translate(_SPACES).replace(",", ".")
    try:
        return float(s)
    except ValueError:
        return None


def parse_stat(path):
    """perf stat --stdio -> {событие: число}. Имена PMU срезаются: cpu_core/cycles/
    и cpu_atom/cycles/ — это один и тот же счётчик, просто на разных ядрах."""
    out = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if m := _ELAPSED.search(line):
            out["elapsed"] = _num(m.group(1))
            continue
        parts = _COLS.split(line.strip())
        if len(parts) < 2 or line.lstrip().startswith("#"):
            continue
        val, ev = _num(parts[0]), parts[1]
        if val is None:
            continue
        out[ev.removeprefix("cpu_core/").removeprefix("cpu_atom/").rstrip("/")] = val
    return out


def parse_topdown(path):
    """Двухстрочная таблица tma_*: шапка с именами, следом строка процентов.

    Порядок колонок у cpu_core и cpu_atom разный, поэтому имена берутся из шапки,
    а не из фиксированного списка."""
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    head = next((i for i, l in enumerate(lines) if "tma_" in l), None)
    if head is None or head + 1 >= len(lines):
        return {}
    names = re.findall(r"tma_\w+", lines[head])
    vals = [v for v in (_num(x) for x in _COLS.split(lines[head + 1].strip()))
            if v is not None]
    return dict(zip(names, vals))


def _rate(v):
    """0.00 у редкого события читается как сбой парсера, поэтому мелочь — в мантиссе."""
    return f"{v:.2f}" if v >= 0.01 else f"{v:.2g}"


def metrics(stat):
    """Производные величины — то, ради чего счётчики и собирались."""
    g = stat.get
    cyc, ins = g("cycles"), g("instructions")
    out = {}
    if cyc and ins:
        out["IPC"] = f"{ins / cyc:.2f}"
    if ins:
        for label, ev in (("LLC-miss/Kinsn", "LLC-load-misses"),
                          ("br-miss/Kinsn", "branch-misses"),
                          ("dTLB-miss/Kinsn", "dTLB-load-misses")):
            if g(ev) is not None:
                out[label] = _rate(g(ev) / ins * 1000)
    if g("cache-references"):
        out["cache-miss %"] = f"{g('cache-misses', 0) / g('cache-references') * 100:.1f}"
    for ev, label in (("context-switches", "ctx-sw"), ("cpu-migrations", "migr"),
                      ("page-faults", "pgflt")):
        if g(ev) is not None:
            out[label] = f"{int(g(ev)):,}".replace(",", " ")
    return out


def collect(profdir):
    """Файлы prof/ -> {tag: {роль или '' для shared: {суффикс: путь}}}."""
    points = {}
    for f in sorted(profdir.iterdir()):
        if f.name == "index.html" or f.suffix in (".err", ".folded"):
            continue
        base, _, suffix = f.name.partition(".")
        if not (m := _TAG.match(base)):
            continue
        tag, role = m.group(1), m.group(2) or ""
        points.setdefault(tag, {}).setdefault(role, {})[suffix] = f
    return points


def _h(s):
    return html.escape(str(s))


def _excerpt(path, limit=120):
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    text = "\n".join(lines[:limit])
    if len(lines) > limit:
        text += f"\n... ещё {len(lines) - limit} строк, см. {path.name}"
    return text


def _flame(f, title):
    # <object>, а не inline <svg>: у каждого SVG от flamegraph.pl свой встроенный
    # скрипт с фиксированными id, в одном документе они бы передрались.
    return (f'<details open><summary>{_h(title)}</summary>'
            f'<object type="image/svg+xml" data="{_h(f.name)}" class="flame"></object>'
            f'</details>')


def _role_block(role, art):
    parts = []
    if stat := art.get("stat.txt"):
        m = metrics(parse_stat(stat))
        if td := art.get("topdown.txt"):
            m |= {k.removeprefix("tma_"): f"{v:.1f}%" for k, v in parse_topdown(td).items()}
        parts.append("<table class=m><tr>" +
                     "".join(f"<th>{_h(k)}</th>" for k in m) + "</tr><tr>" +
                     "".join(f"<td>{_h(v)}</td>" for v in m.values()) + "</tr></table>")
    for key, title in (("perf.flame.svg", "флейм-граф по циклам"),
                       ("cachemiss.flame.svg", "флейм-граф по промахам кэша")):
        if f := art.get(key):
            parts.append(_flame(f, title))
    for key, title in (("perf.report.txt", "perf report (циклы)"),
                       ("cachemiss.report.txt", "perf report (промахи кэша)")):
        if f := art.get(key):
            parts.append(f"<details><summary>{_h(title)}</summary>"
                         f"<pre>{_h(_excerpt(f, 40))}</pre></details>")
    raw = [f"<code>perf report -i {f.name}</code>"
           for k, f in sorted(art.items()) if k.endswith(".data")]
    if raw:
        parts.append("<p class=raw>сырьё: " + " · ".join(raw) + "</p>")
    return f"<h4>{_h(role or 'на точку целиком')}</h4>" + "".join(parts) if parts else ""


def _shared_block(art):
    parts = []
    for key, title in (("c2c.txt", "perf c2c — false sharing, HITM по строкам кэша"),
                       ("mem.txt", "perf mem — латентность обращений к памяти")):
        if f := art.get(key):
            parts.append(f"<details><summary>{_h(title)}</summary>"
                         f"<pre>{_h(_excerpt(f))}</pre></details>")
    return "".join(parts)


CSS = """
:root{--bg:#fff;--fg:#1a1a1a;--dim:#666;--line:#e0e0e0;--accent:#2a78d6;--code:#f6f6f6}
@media (prefers-color-scheme:dark){
:root{--bg:#16181c;--fg:#e6e6e6;--dim:#9aa0a6;--line:#2c3038;--accent:#5b9dff;--code:#1e2126}}
*{box-sizing:border-box}
body{background:var(--bg);color:var(--fg);margin:0 auto;padding:2rem 1.5rem;max-width:76rem;
font:15px/1.55 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
h1{font-size:1.5rem;margin:0 0 .25rem}h2{font-size:1.15rem;margin:2.5rem 0 .5rem;
padding-top:.75rem;border-top:2px solid var(--line)}
h3{font-size:1rem;margin:1.25rem 0 .4rem;color:var(--dim);font-weight:600}
h4{font-size:.95rem;margin:1rem 0 .35rem;color:var(--accent)}
table{border-collapse:collapse;margin:.4rem 0;font-size:.87rem;display:block;
overflow-x:auto;max-width:100%}
th,td{border:1px solid var(--line);padding:.28rem .6rem;text-align:right;white-space:nowrap}
th{background:var(--code);font-weight:600;color:var(--dim)}
td:first-child,th:first-child{text-align:left}
table.m th{font-size:.78rem}
pre{background:var(--code);padding:.75rem;overflow-x:auto;font-size:.8rem;
border-radius:4px;margin:.4rem 0}
code{background:var(--code);padding:.1rem .35rem;border-radius:3px;font-size:.82rem}
details{margin:.5rem 0}summary{cursor:pointer;color:var(--accent);font-size:.9rem}
.flame{width:100%;height:min(70vh,540px);border:1px solid var(--line);
border-radius:4px;background:#fff;margin-top:.4rem}
.raw{color:var(--dim);font-size:.85rem;margin:.4rem 0}
.note{color:var(--dim);font-size:.87rem}
"""


def build(rundir, rows):
    """Собирает prof/index.html. Возвращает путь (или None, если профилей нет)."""
    profdir = Path(rundir) / "prof"
    if not profdir.is_dir():
        return None
    points = collect(profdir)
    env = (Path(rundir) / "env.txt")
    lat = {}
    for r in rows:
        lat.setdefault(r.get("tag"), []).append(r)

    # charset — первой строкой и обязательно: файл открывают по file://, где нет
    # заголовка Content-Type, и без объявления браузер берёт кодировку из локали
    # (в ru — windows-1251), отчего весь русский текст выходит крокозябрами.
    out = ['<meta charset="utf-8">',
           f"<title>Профили прогона {_h(Path(rundir).name)}</title>",
           f"<style>{CSS}</style>",
           f"<h1>Профили прогона {_h(Path(rundir).name)}</h1>",
           "<p class=note>Точки, помеченные в <code>summary.csv</code> колонкой "
           "<code>profiled</code>, сняты под сэмплирующим профайлером: хвост в них "
           "завышен, сравнивать их с чистыми повторами нельзя.</p>"]
    if env.exists():
        out.append(f"<pre>{_h(env.read_text(encoding='utf-8').strip())}</pre>")

    for tag in sorted(points):
        out.append(f"<h2>{_h(tag)}</h2>")
        if rs := lat.get(tag):
            out.append("<h3>латентность, нс</h3><table><tr><th>получатель</th><th>n</th>"
                       "<th>drops</th><th>p50</th><th>p99</th><th>p99.9</th>"
                       "<th>p99.99</th></tr>")
            for r in sorted(rs, key=lambda x: x["receiver"]):
                out.append("<tr>" + "".join(
                    f"<td>{_h(r[c])}</td>" for c in
                    ("receiver", "n", "drop_rate", "p50", "p99", "p99.9", "p99.99")) +
                    "</tr>")
            out.append("</table>")
        roles = points[tag]
        for role in sorted(roles, key=lambda r: (ROLE_ORDER.get(r.split("-")[0], 9), r)):
            if role:
                out.append(_role_block(role, roles[role]))
        if "" in roles:
            out.append("<h3>обе стороны строки кэша</h3>" + _shared_block(roles[""]))

    index = profdir / "index.html"
    index.write_text("\n".join(out), encoding="utf-8")
    return index
