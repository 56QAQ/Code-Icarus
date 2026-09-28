#!/usr/bin/env python3
"""Sum up third-version acceptance runs (tools/v3_acceptance.sh) as a Markdown table.

Usage: tools/v3_acceptance.py DIR
DIR holds seed_<N>.txt written by `icarus_cli trend` (a line a day, then TOTAL deaths).

A run trends upward when the island's civilisation index ends above where it started
and its linear trend over the second half of the run is rising. The trend over the whole
run and the second half's slope are listed too, with population, births, deaths by cause,
wars and the days some people spent at war.
"""
import glob
import os
import re
import sys

WAR_CAUSES = re.compile(r"(被[^×]*所伤|被魔法[^×]*击中|中箭)×(\d+)")


def slope(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    den = sum((x - mx) ** 2 for x in xs)
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den if den else 0.0


def summarise(path):
    days, isl, pops = [], [], []
    births = wars = war_days = 0
    total = ""
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("D "):
            parts = line.split()
            days.append(int(parts[1]))
            isl.append(float(parts[3]))
            births += int(re.search(r"births (\d+)", line).group(1))
            wars += int(re.search(r"wars (\d+)", line).group(1))
            pops.append(sum(int(x) for x in re.findall(r":(\d+)p ", line)))
            war_days += "⚔" in line
        elif line.startswith("TOTAL deaths:"):
            total = line.strip()[len("TOTAL deaths:"):].strip()
    if len(days) < 4:
        return None
    half = len(days) // 2
    causes = dict((k, int(v)) for k, v in re.findall(r"(\S+)×(\d+)", total))
    return {
        "seed": int(re.search(r"seed_(\d+)", path).group(1)),
        "days": days[-1],
        "first": isl[0], "mid": isl[half], "last": isl[-1],
        "slope_all": slope(days, isl), "slope_half": slope(days[half:], isl[half:]),
        "pop0": pops[0], "pop1": pops[-1], "pop_max": max(pops),
        "births": births, "wars": wars, "war_days": war_days,
        "deaths": sum(causes.values()),
        "old": causes.get("年老", 0), "starved": causes.get("饿死", 0), "thirst": causes.get("渴死", 0),
        "war_dead": sum(int(v) for _, v in WAR_CAUSES.findall(total)),
        "wounds": causes.get("伤势过重", 0),
    }


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    runs = [r for r in (summarise(p) for p in glob.glob(os.path.join(sys.argv[1], "seed_*.txt"))) if r]
    runs.sort(key=lambda r: r["seed"])
    if not runs:
        print("no runs found")
        return 1
    print("| 种子 | 全岛文明指数 起→中→末 | 全程趋势 | 后半程趋势 | 态势 | 人口 起→末（最多） | 出生 | 死亡（年老/饿死/渴死/战死/伤重） | 宣战 | 有战事的天数 |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    up = 0
    for r in runs:
        rising = r["last"] > r["first"] and r["slope_half"] > 0
        up += rising
        print(f"| {r['seed']} | {r['first']:.0f} → {r['mid']:.0f} → {r['last']:.0f} | {r['slope_all']:+.2f}/天 | "
              f"{r['slope_half']:+.2f}/天 | {'上升' if rising else '回落'} | {r['pop0']} → {r['pop1']}（{r['pop_max']}） | "
              f"{r['births']} | {r['deaths']}（{r['old']}/{r['starved']}/{r['thirst']}/{r['war_dead']}/{r['wounds']}） | "
              f"{r['wars']} | {r['war_days']} |")
    n = len(runs)
    ends_higher = sum(r["last"] > r["first"] for r in runs)
    whole = sum(r["slope_all"] > 0 for r in runs)
    print()
    print(f"上升态势（末值高于起点且后半程趋势上升）：{up}/{n}（{100 * up / n:.0f}%）；"
          f"末值高于起点：{ends_higher}/{n}；全程趋势上升：{whole}/{n}")
    avg = lambda k: sum(r[k] for r in runs) / n
    print(f"平均每局：出生 {avg('births'):.0f}，饿死 {avg('starved'):.1f}，渴死 {avg('thirst'):.1f}，"
          f"战死 {avg('war_dead'):.1f}，伤重不治 {avg('wounds'):.1f}，宣战 {avg('wars'):.1f} 次，有战事 {avg('war_days'):.0f} 天")
    return 0


if __name__ == "__main__":
    sys.exit(main())
