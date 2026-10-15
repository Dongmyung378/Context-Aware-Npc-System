"""Render NPC portfolio charts from retained actual-engine JSON reports.

Default language is English. Korean SVG text is converted to paths using Malgun
Gothic so the exported figure does not depend on the viewer's installed fonts.
Only Matplotlib and the standard library are required. No engine is launched.

Optional --profile-batch expects a JSON object with:
  schema: npc-figure-profile-batch/v1
  release_scope: final-navigation-validated
  navigation_report: relative path to a passing actual navigation JSON report
  runs: [{report: relative path to a final profile JSON report}, ...]
There must be three final runs each for 5, 20 and 50 NPCs. Movement evidence and
live controllers/brains are required; original idle NPCs need not all walk.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
from statistics import median

import matplotlib

matplotlib.use("Agg")
from matplotlib import font_manager, pyplot as plt  # noqa: E402
from matplotlib.patches import Patch  # noqa: E402
from matplotlib.ticker import MaxNLocator  # noqa: E402


PALETTE = {
    "cream": "#FBF8F1",
    "forest": "#245942",
    "deep": "#183D30",
    "copper": "#AE6945",
    "clay": "#DCC3AF",
    "sage": "#91A291",
    "ink": "#293D34",
    "muted": "#526358",
    "grid": "#DDDCD1",
}

TEXT = {
    "en": {
        "state_title": "State contracts in the actual game",
        "state_subtitle": "The same expected behaviors, before and after targeted fixes",
        "initial": "Initial implementation",
        "retained": "Retained implementation",
        "initial_short": "Initial",
        "retained_short": "Retained",
        "state_axis": "State contracts (count)",
        "passed": "Passed",
        "failed": "Failed",
        "state_note": "Pass counts from the same {state_total} state contracts in the original-map PIE world.",
        "delta": "{delta:+d} passing contracts",
        "prob_title": "Probability measured in the actual engine",
        "prob_subtitle": "Actual Blueprint function calls, engine RNG seed {seed}",
        "rare": "Rare item selection",
        "quest": "Quest proposal*",
        "prob_axis": "Observed rate (%)",
        "threshold": "Configured threshold: {rate:g}%",
        "prob_note": "Whiskers: Wilson 95% intervals.\n*{quest_trials:,} Talk calls: stamina {stamina:g}, money {money:,.0f}, no active quest.",
        "count": "{count:,} / {trials:,}  ({rate:.2f}%)",
        "ci": "95% CI  {lower:.2f}% to {upper:.2f}%",
        "overview_title": "NPC interaction: verified behavior and probability",
        "profile_title": "Engine workload by NPC count",
        "profile_subtitle": "After navigation validation; three runs per condition",
        "world_tick": "PIE world actor-tick span",
        "editor_interval": "Editor end-frame interval",
        "profile_x": "NPC count",
        "profile_y": "Milliseconds",
        "profile_note": "Dots: median of three run medians. Whiskers: minimum to maximum run median.\nWorld tick includes all actors and intervening work. Editor intervals include editor/PIE/waits; not packaged-game FPS.",
    },
    "ko": {
        "state_title": "실제 게임의 상태 규칙 검증",
        "state_subtitle": "같은 기대 동작을 초기 구현과 수정 후 구현에서 비교",
        "initial": "초기 구현",
        "retained": "수정 후 구현",
        "initial_short": "초기 구현",
        "retained_short": "수정 후 구현",
        "state_axis": "상태 검증 항목 수",
        "passed": "통과",
        "failed": "실패",
        "state_note": "원래 맵의 PIE에서 같은 상태 검증 {state_total}개 항목을 실행한 통과 건수입니다.",
        "delta": "통과 항목 {delta:+d}개",
        "prob_title": "실제 엔진에서 측정한 발생 확률",
        "prob_subtitle": "실제 블루프린트 함수 호출, 엔진 난수 시드 {seed}",
        "rare": "희귀 아이템 선택",
        "quest": "퀘스트 제안*",
        "prob_axis": "관측 비율 (%)",
        "threshold": "설정 기준: {rate:g}%",
        "prob_note": "오차 막대는 Wilson 95% 구간입니다.\n*스태미나 {stamina:g}, 소지금 {money:,.0f}, 진행 중 퀘스트 없음에서 Talk {quest_trials:,}회 호출",
        "count": "{count:,} / {trials:,}회  ({rate:.2f}%)",
        "ci": "95% 구간  {lower:.2f}% ~ {upper:.2f}%",
        "overview_title": "NPC 상호작용의 동작 검증과 확률 측정",
        "profile_title": "NPC 수에 따른 엔진 작업 시간",
        "profile_subtitle": "내비게이션 검증 후 조건별 3회 실행",
        "world_tick": "PIE 월드 Actor Tick 구간",
        "editor_interval": "에디터 프레임 종료 간격",
        "profile_x": "NPC 수",
        "profile_y": "밀리초",
        "profile_note": "점은 실행별 중앙값 3개의 중앙값, 오차 막대는 실행별 중앙값의 최솟값과 최댓값입니다.\n월드 Tick은 모든 Actor와 중간 작업을 포함합니다. 에디터 간격은 대기 등을 포함하며 패키징 게임의 FPS가 아닙니다.",
    },
}


def read_json(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8-sig"))
    if not isinstance(value, dict):
        raise ValueError(f"JSON object required: {path.name}")
    return value


def finite_number(value, label: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
        raise ValueError(f"Finite numeric field required: {label}")
    return float(value)


def integer(value, label: str) -> int:
    number = finite_number(value, label)
    if not number.is_integer() or number < 0:
        raise ValueError(f"Nonnegative integer required: {label}")
    return int(number)


def state_record(report: dict) -> dict:
    total = integer(report["case_count"], "case_count")
    passed = integer(report["passed_cases"], "passed_cases")
    failed = integer(report["failed_cases"], "failed_cases")
    cases = report["cases"]
    if not isinstance(cases, list) or total != len(cases) or passed + failed != total or total == 0:
        raise ValueError("State summary does not match its actual case list")
    if any(type(case.get("passed")) is not bool for case in cases):
        raise ValueError("Every state case requires an explicit passed boolean")
    if sum(case["passed"] for case in cases) != passed:
        raise ValueError("State pass count differs from case results")
    ids = [case["id"] for case in cases]
    if len(set(ids)) != total:
        raise ValueError("State case IDs must be unique")
    return {"total": total, "passed": passed, "failed": failed, "ids": ids}


def probability_record(report: dict, count_key: str, seed_key: str) -> dict:
    trials = integer(report["trials"], "trials")
    count = integer(report[count_key], count_key)
    if trials == 0 or count > trials:
        raise ValueError("Invalid probability denominator or success count")
    rate = count / trials
    lower = finite_number(report["wilson_95_lower"], "wilson_95_lower")
    upper = finite_number(report["wilson_95_upper"], "wilson_95_upper")
    z = 1.959963984540054
    denominator = 1 + z * z / trials
    centre = (rate + z * z / (2 * trials)) / denominator
    radius = z * math.sqrt(rate * (1 - rate) / trials + z * z / (4 * trials * trials)) / denominator
    if not (math.isclose(lower, centre - radius, abs_tol=1e-9)
            and math.isclose(upper, centre + radius, abs_tol=1e-9)):
        raise ValueError("Reported Wilson interval does not match the actual counts")
    return {
        "count": count, "trials": trials, "rate": rate,
        "lower": lower, "upper": upper,
        "threshold": finite_number(report["configured_threshold"], "configured_threshold"),
        "seed": integer(report[seed_key], seed_key),
    }


def configure_style(language: str, font_path: Path | None) -> None:
    family = "DejaVu Sans"
    if language == "ko":
        selected = font_path or Path("C:/Windows/Fonts/malgun.ttf")
        if not selected.is_file():
            raise ValueError("Korean rendering requires --font pointing to Malgun Gothic")
        font_manager.fontManager.addfont(str(selected))
        family = font_manager.FontProperties(fname=selected).get_name()
    plt.rcParams.update({
        "font.family": family,
        "font.size": 11,
        "axes.labelsize": 11,
        "axes.titlesize": 15,
        "axes.titleweight": "bold",
        "text.color": PALETTE["ink"],
        "axes.labelcolor": PALETTE["muted"],
        "xtick.color": PALETTE["muted"],
        "ytick.color": PALETTE["ink"],
        "axes.edgecolor": PALETTE["grid"],
        "figure.facecolor": PALETTE["cream"],
        "axes.facecolor": PALETTE["cream"],
        "savefig.facecolor": PALETTE["cream"],
        "svg.fonttype": "path",
        "svg.hashsalt": "npc-portfolio-figures-v1",
        "axes.unicode_minus": False,
        "lines.solid_capstyle": "round",
    })


def clean_axis(axis) -> None:
    for side in ("top", "right", "left"):
        axis.spines[side].set_visible(False)
    axis.tick_params(axis="both", length=0, pad=9)
    axis.grid(axis="x", color=PALETTE["grid"], linewidth=0.8, alpha=0.7)
    axis.set_axisbelow(True)


def draw_states(axis, initial: dict, corrected: dict, text: dict, compact: bool = False) -> None:
    clean_axis(axis)
    total = corrected["total"]
    for y, row in ((1, initial), (0, corrected)):
        axis.barh(y, row["passed"], height=0.34, color=PALETTE["forest"],
                  alpha=0.68 if y else 1, zorder=3)
        if row["failed"]:
            axis.barh(y, row["failed"], left=row["passed"], height=0.34,
                      color=PALETTE["clay"], hatch="///", edgecolor=PALETTE["copper"], linewidth=0.0, zorder=3)
        axis.text(total + total * 0.035, y, f"{row['passed']} / {total}",
                  va="center", ha="left", fontsize=12 if compact else 14,
                  color=PALETTE["deep"], fontweight="bold")
    labels = [text["initial_short"], text["retained_short"]] if compact else [text["initial"], text["retained"]]
    axis.set_yticks([1, 0], labels)
    axis.set_ylim(-0.62, 1.66)
    axis.set_xlim(0, total * 1.23)
    ticks = sorted({int(value) for value in MaxNLocator(nbins=4, integer=True).tick_values(0, total)
                    if 0 <= value < total} | {total})
    axis.set_xticks(ticks)
    axis.set_xlabel(text["state_axis"], labelpad=12)
    axis.set_title(text["state_title"], loc="left", pad=24)
    axis.text(0, 1.04, text["delta"].format(delta=corrected["passed"] - initial["passed"]),
              transform=axis.transAxes, fontsize=11, color=PALETTE["forest"], va="bottom")
    axis.legend(handles=[Patch(facecolor=PALETTE["forest"], label=text["passed"]),
                         Patch(facecolor=PALETTE["clay"], edgecolor=PALETTE["copper"], hatch="///", label=text["failed"])],
                loc="lower left", bbox_to_anchor=(0, -0.34), ncol=2, frameon=False,
                borderaxespad=0, handlelength=1.4, fontsize=10)


def draw_probability(axis, records: list[dict], text: dict, compact: bool = False) -> None:
    clean_axis(axis)
    threshold = records[0]["threshold"] * 100
    axis.axvline(threshold, color=PALETTE["muted"], linewidth=1.3, linestyle=(0, (3, 4)), zorder=1)
    lower = min(row["lower"] * 100 for row in records)
    upper = max(row["upper"] * 100 for row in records)
    margin = max((upper - lower) * 0.42, 0.35)
    axis.set_xlim(lower - margin, upper + margin)
    axis.set_ylim(-0.58, 1.65)
    for y, row, color in ((1, records[0], PALETTE["copper"]), (0, records[1], PALETTE["forest"])):
        rate = row["rate"] * 100
        errors = [[rate - row["lower"] * 100], [row["upper"] * 100 - rate]]
        axis.errorbar(rate, y, xerr=errors, fmt="o", color=color, markersize=8,
                       markeredgecolor=PALETTE["cream"], markeredgewidth=1.2,
                       elinewidth=2.2, capsize=6, capthick=1.7, zorder=4)
        axis.text(0.02, y + 0.27, text["count"].format(
            count=row["count"], trials=row["trials"], rate=row["rate"] * 100),
                  transform=axis.get_yaxis_transform(), va="bottom", fontsize=11 if compact else 12,
                  color=color, fontweight="bold")
        axis.text(0.02, y - 0.29, text["ci"].format(lower=row["lower"] * 100, upper=row["upper"] * 100),
                  transform=axis.get_yaxis_transform(), va="top", fontsize=9 if compact else 10,
                  color=PALETTE["muted"])
    axis.set_yticks([1, 0], [text["rare"], text["quest"]])
    axis.set_xlabel(text["prob_axis"], labelpad=12)
    axis.set_title(text["prob_title"], loc="left", pad=24)
    axis.text(0, 1.04, text["threshold"].format(rate=threshold), transform=axis.transAxes,
              fontsize=10, color=PALETTE["muted"], va="bottom")


def save_figure(figure, output: Path, stem: str, title: str, description: str) -> list[Path]:
    paths = []
    for extension in ("svg", "png"):
        path = output / f"{stem}.{extension}"
        metadata = {"Title": title, "Description": description}
        if extension == "svg":
            metadata.update({"Date": None, "Creator": "NPC project figure generator"})
        figure.savefig(path, dpi=160, metadata=metadata)
        paths.append(path)
    plt.close(figure)
    return paths


def render_validation(output: Path, language: str, initial: dict, corrected: dict,
                      probabilities: list[dict]) -> list[Path]:
    text = TEXT[language]
    seed = probabilities[0]["seed"]
    state_note = text["state_note"].format(state_total=corrected["total"])
    probability_note = text["prob_note"].format(
        quest_trials=probabilities[1]["trials"], stamina=probabilities[1]["stamina"],
        money=probabilities[1]["money"])
    description = "Figures derived from actual-engine JSON reports; state contract counts and conditional event sampling are separate measurements."
    paths = []

    figure, axis = plt.subplots(figsize=(10.6, 5.5))
    figure.subplots_adjust(left=0.23, right=0.93, top=0.77, bottom=0.27)
    draw_states(axis, initial, corrected, text)
    figure.text(0.06, 0.94, text["state_subtitle"], color=PALETTE["muted"], fontsize=11)
    figure.text(0.06, 0.055, state_note, color=PALETTE["muted"], fontsize=10)
    paths += save_figure(figure, output, "npc-state-contracts", text["state_title"], description)

    figure, axis = plt.subplots(figsize=(10.6, 5.8))
    figure.subplots_adjust(left=0.24, right=0.94, top=0.76, bottom=0.25)
    draw_probability(axis, probabilities, text)
    figure.text(0.06, 0.94, text["prob_subtitle"].format(seed=seed), color=PALETTE["muted"], fontsize=11)
    figure.text(0.06, 0.045, probability_note, color=PALETTE["muted"], fontsize=10, linespacing=1.65)
    paths += save_figure(figure, output, "npc-probability", text["prob_title"], description)

    figure, axes = plt.subplots(1, 2, figsize=(16, 6.4))
    figure.subplots_adjust(left=0.115, right=0.965, top=0.71, bottom=0.28, wspace=0.58)
    draw_states(axes[0], initial, corrected, text, compact=True)
    draw_probability(axes[1], probabilities, text, compact=True)
    figure.text(0.045, 0.925, text["overview_title"], fontsize=20, fontweight="bold", color=PALETTE["deep"])
    figure.text(0.045, 0.855, text["prob_subtitle"].format(seed=seed), fontsize=11, color=PALETTE["muted"])
    figure.text(0.045, 0.05, state_note + "\n" + probability_note,
                 fontsize=9, color=PALETTE["muted"], linespacing=1.65)
    paths += save_figure(figure, output, "npc-validation-overview", text["overview_title"], description)
    return paths


def resolve_relative(base: Path, value: str) -> Path:
    path = Path(value)
    return path if path.is_absolute() else base / path


def profile_records(batch_path: Path) -> tuple[dict, list[Path]]:
    batch = read_json(batch_path)
    if batch.get("schema") != "npc-figure-profile-batch/v1" or batch.get("release_scope") != "final-navigation-validated":
        raise ValueError("Only an explicitly final-navigation-validated profile batch is accepted")
    nav_path = resolve_relative(batch_path.parent, batch["navigation_report"])
    navigation = read_json(nav_path)
    if navigation.get("assertions_passed") is not True or navigation.get("status", "passed") != "passed":
        raise ValueError("The referenced actual navigation report did not pass")
    sources = [batch_path, nav_path]
    records = {count: {"world": [], "editor": []} for count in (5, 20, 50)}
    observed_paths = set()
    for entry in batch["runs"]:
        path = resolve_relative(batch_path.parent, entry["report"])
        resolved = path.resolve()
        if resolved in observed_paths:
            raise ValueError("Repeated copies of one report cannot stand in for three runs")
        observed_paths.add(resolved)
        if entry.get("sha256") and hashlib.sha256(path.read_bytes()).hexdigest() != entry["sha256"]:
            raise ValueError(f"Profile report digest differs from its manifest: {path.name}")
        if path.stem.lower().startswith(("initial", "baseline", "lookup-candidate")):
            raise ValueError("Initial or rejected-candidate profiles cannot be plotted as final results")
        report = read_json(path)
        if report.get("status") != "passed" or report.get("assertions_passed") is not True:
            raise ValueError(f"Final profile failed: {path.name}")
        count = integer(report["requested_npc_count"], "requested_npc_count")
        if count not in records:
            raise ValueError("Final figure expects exactly 5, 20 and 50 NPC conditions")
        movement = report["npc_movement"]
        final = report["final_counts"]
        if (integer(movement["observed_actor_count"], "observed_actor_count") != count
                or integer(movement["moved_over_25uu_count"], "moved_over_25uu_count") < 1
                or integer(movement["moving_path_sample_count"], "moving_path_sample_count") < 1
                or integer(final["npc_ai_controller_count"], "npc_ai_controller_count") != count
                or integer(final["npc_brain_running_count"], "npc_brain_running_count") != count):
            raise ValueError(f"Missing live-controller/brain/movement evidence: {path.name}")
        for metric, field, raw_field in (
            ("world", "world_actor_tick_ms", "raw_world_actor_tick_ms"),
            ("editor", "editor_end_frame_interval_ms", "raw_editor_end_frame_interval_ms"),
        ):
            observed = finite_number(report[field]["median_ms"], field)
            raw = report[raw_field]
            raw_values = [finite_number(sample["milliseconds"], raw_field) for sample in raw]
            if (not raw_values or observed <= 0
                    or len(raw_values) != integer(report[field]["samples"], "samples")
                    or not math.isclose(observed, median(raw_values), abs_tol=1e-6)):
                raise ValueError(f"Profile median does not match retained raw samples: {path.name}")
            records[count][metric].append(observed)
        sources.append(path)
    for count, metrics in records.items():
        if any(len(values) != 3 for values in metrics.values()):
            raise ValueError(f"Exactly three final runs are required for {count} NPCs")
    return records, sources


def render_profile(output: Path, language: str, records: dict) -> list[Path]:
    text = TEXT[language]
    figure, axes = plt.subplots(1, 2, figsize=(13.8, 6.4))
    figure.subplots_adjust(left=0.08, right=0.96, top=0.72, bottom=0.29, wspace=0.25)
    for axis, metric, title, color in (
        (axes[0], "world", text["world_tick"], PALETTE["forest"]),
        (axes[1], "editor", text["editor_interval"], PALETTE["copper"]),
    ):
        clean_axis(axis)
        axis.grid(axis="x", visible=False)
        axis.grid(axis="y", color=PALETTE["grid"], linewidth=0.8, alpha=0.7)
        counts = sorted(records)
        centres = [median(records[count][metric]) for count in counts]
        lower = [centre - min(records[count][metric]) for count, centre in zip(counts, centres)]
        upper = [max(records[count][metric]) - centre for count, centre in zip(counts, centres)]
        axis.plot(range(len(counts)), centres, color=color, linewidth=1.6, alpha=0.75)
        axis.errorbar(range(len(counts)), centres, yerr=[lower, upper], fmt="o", color=color,
                       markersize=8, capsize=6, elinewidth=1.8, capthick=1.5)
        for index, value in enumerate(centres):
            axis.annotate(f"{value:.2f}", (index, value), xytext=(0, 13),
                          textcoords="offset points", ha="center", color=color, fontweight="bold")
        axis.set_xticks(range(len(counts)), [str(value) for value in counts])
        axis.set_xlim(-0.35, len(counts) - 0.65)
        axis.set_ylim(0, max(max(records[count][metric]) for count in counts) * 1.22)
        axis.set_xlabel(text["profile_x"], labelpad=12)
        axis.set_ylabel(text["profile_y"], labelpad=9)
        axis.set_title(title, loc="left", pad=20)
    figure.text(0.045, 0.93, text["profile_title"], fontsize=20, fontweight="bold", color=PALETTE["deep"])
    figure.text(0.045, 0.86, text["profile_subtitle"], color=PALETTE["muted"], fontsize=11)
    figure.text(0.045, 0.06, text["profile_note"], color=PALETTE["muted"], fontsize=9, linespacing=1.7)
    return save_figure(figure, output, "npc-runtime-profile", text["profile_title"],
                       "Final navigation-validated engine workload; three run medians and min-max range, not standalone FPS.")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reports-dir", type=Path, default=Path(__file__).resolve().parents[1] / "reports")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--language", choices=("en", "ko"), default="en")
    parser.add_argument("--font", type=Path)
    parser.add_argument("--profile-batch", type=Path)
    args = parser.parse_args()
    paths = {
        "initial": args.reports_dir / "initial-state-contracts-valid.json",
        "corrected": args.reports_dir / "corrected-state-contracts.json",
        "interaction": args.reports_dir / "corrected-interaction-contracts.json",
        "quest": args.reports_dir / "corrected-quest-probability.json",
    }
    raw = {key: read_json(path) for key, path in paths.items()}
    initial, corrected = state_record(raw["initial"]), state_record(raw["corrected"])
    if set(initial["ids"]) != set(corrected["ids"]):
        raise ValueError("Initial and corrected state figures must compare the same contract IDs")
    if raw["corrected"].get("assertions_passed") is not True or raw["interaction"].get("assertions_passed") is not True or raw["quest"].get("assertions_passed") is not True:
        raise ValueError("A retained actual-engine source report did not pass")
    probabilities = [probability_record(raw["interaction"]["rare_item_sampling"], "rare_count", "seed"),
                     probability_record(raw["quest"], "quest_count", "rng_seed")]
    # These fixture conditions are currently recorded in the engine report's
    # explicit scope string, rather than separate numeric fields. Parse that
    # evidence so a changed source cannot silently retain an old caption.
    quest_conditions = re.search(r"stamina(\d+(?:\.\d+)?), money(\d+(?:\.\d+)?), no active quest", raw["quest"]["scope"])
    if not quest_conditions:
        raise ValueError("Actual quest report must specify stamina, money and no-active-quest fixture conditions")
    probabilities[1]["stamina"] = float(quest_conditions.group(1))
    probabilities[1]["money"] = float(quest_conditions.group(2))
    if any(row["threshold"] != probabilities[0]["threshold"] or row["seed"] != probabilities[0]["seed"] for row in probabilities):
        raise ValueError("These probability panels require matching reported thresholds and seeds")
    configure_style(args.language, args.font)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    outputs = render_validation(args.output_dir, args.language, initial, corrected, probabilities)
    profile = None
    sources = list(paths.values())
    if args.profile_batch:
        profile, profile_sources = profile_records(args.profile_batch)
        outputs += render_profile(args.output_dir, args.language, profile)
        sources += profile_sources
    manifest = {
        "schema": "npc-portfolio-figures/v1",
        "language": args.language,
        "renderer": f"Matplotlib {matplotlib.__version__}",
        "generator_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "svg_text": "portable paths",
        "state_contracts": {"initial_passed": initial["passed"], "corrected_passed": corrected["passed"],
                            "total": corrected["total"], "scope": "same actual PIE state contracts, not model accuracy"},
        "probability": probabilities,
        "final_profile_run_medians": profile,
        "sources": [{"name": path.name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()} for path in sources],
        "artifacts": [{"name": path.name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()} for path in outputs],
    }
    (args.output_dir / "figures-manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"language": args.language, "artifacts": [str(path.resolve()) for path in outputs]}, ensure_ascii=False))


if __name__ == "__main__":
    main()
