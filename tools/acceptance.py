#!/usr/bin/env python3
"""Summarise v2 acceptance runs (icarus_cli run ... --events) as Markdown tables.

Usage: tools/acceptance.py DIR
DIR holds three_realms_<seed>.txt and wild_<seed>.txt, written by tools/acceptance.sh.
Days are counted from the start of the game (a year has 32 days).
"""
import glob
import os
import re
import sys

SEASONS = {"春": 0, "夏": 1, "秋": 2, "冬": 3}
EVENT = re.compile(r"^\s+>> \[第(\d+)年 (.)·第(\d+)日 (\d+):(\d+)\] (.*)$")


def events(path):
    out = []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = EVENT.match(line)
        if m:
            y, s, d, hh, mm, text = m.groups()
            day = (int(y) - 1) * 32 + SEASONS[s] * 8 + int(d) - 1 + (int(hh) + int(mm) / 60) / 24
            out.append((day, text))
    return out


def populations(path):
    lines = [l for l in open(path, encoding="utf-8", errors="replace") if l.startswith("第")]
    if not lines:
        return 0, 0, 0
    first = [int(x) for x in re.findall(r"pop (\d+)", lines[0])]
    last = [int(x) for x in re.findall(r"pop (\d+)", lines[-1])]
    return sum(first), sum(last), len(last)


def buildings(path):
    """Beds and finished buildings of every people at the end of the run."""
    beds, built = 0, {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"buildings [^:]*: beds (\d+) \|(.*)$", line)
        if m:
            beds += int(m.group(1))
            for name, n in re.findall(r"(\S+)×(\d+)", m.group(2)):
                built[name] = built.get(name, 0) + int(n)
    return beds, built


def first(ev, pattern):
    return next((d for d, t in ev if re.search(pattern, t)), None)


def count(ev, pattern):
    return sum(1 for _, t in ev if re.search(pattern, t))


def fmt(v):
    return "—" if v is None else (f"{v:.1f}" if isinstance(v, float) else str(v))


def seeds(d, prefix):
    files = glob.glob(os.path.join(d, prefix + "_*.txt"))
    return sorted(files, key=lambda f: int(re.findall(r"_(\d+)\.txt$", f)[0]))


def main():
    d = sys.argv[1]
    print("| 种子 | 首次宣战（天） | 宣战 | 会战 | 溃退 | 议和/停战 | 称臣 | 征服 | 阵亡 | 对决 | 随军出征 | 施法 | 源动力转变 | 觉醒 | 饿死 | 人口 始→终 | 终局国家数 |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for f in seeds(d, "three_realms"):
        ev = events(f)
        p0, p1, n = populations(f)
        seed = re.findall(r"_(\d+)\.txt$", f)[0]
        row = [seed, fmt(first(ev, r"宣战（")), count(ev, r"宣战（"), count(ev, r"的军队交战") // 2,
               count(ev, r"撤退了"), count(ev, r"^「[^」]*」与「[^」]*」(议和|.*停战|.*平息)"),
               count(ev, r"成为其附庸|被迫称臣"), count(ev, r"征服了"), count(ev, r"死亡：被"),
               count(ev, r"展开对决"), count(ev, r"随军出征"), count(ev, r"施展|掷出|击中"),
               count(ev, r"源动力由"), count(ev, r"觉醒为魔法少女"), count(ev, r"饿死"), f"{p0}→{p1}", n]
        print("| " + " | ".join(str(x) for x in row) + " |")
    print()
    print("| 种子 | 石器 | 狩猎 | 窝棚（科技） | 文字 | 书写室 | 农耕 | 第一片田 | 茅草屋（科技） | 储藏 | 农耕时代 | 青铜时代 | 铁器时代 | 捕鱼 | 饿死 | 人口 始→终 | 终局国家数 | 床位 | 建成 |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    for f in seeds(d, "wild"):
        ev = events(f)
        p0, p1, n = populations(f)
        seed = re.findall(r"_(\d+)\.txt$", f)[0]
        beds, built = buildings(f)
        built.pop("篝火", None)
        row = [seed] + [fmt(first(ev, p)) for p in (
            r"掌握了打制石器", r"掌握了狩猎", r"掌握了窝棚", r"掌握了文字", r"竣工：.*书写室", r"掌握了农耕",
            r"开垦第一片田地", r"掌握了茅草屋", r"掌握了储藏", r"迈入了农耕时代", r"迈入了青铜时代",
            r"迈入了铁器时代", r"掌握了捕鱼")] + [count(ev, r"饿死"), f"{p0}→{p1}", n, beds,
                                   "、".join(f"{k}×{v}" for k, v in sorted(built.items())) or "—"]
        print("| " + " | ".join(str(x) for x in row) + " |")


if __name__ == "__main__":
    main()
