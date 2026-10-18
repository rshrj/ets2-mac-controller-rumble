"""Summarize a drive recording and plot the moments around each marker.

Usage: .venv/bin/python analyze.py [recordings/drive-*.rec]   (default: newest)

Runs ./replay with rumble.conf, prints a summary, and writes plots to
recordings/<drive>-plots/.
"""

import subprocess
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402
import pandas as pd  # noqa: E402

ROOT = Path(__file__).resolve().parent
WINDOW_BEFORE_S = 5.0
WINDOW_AFTER_S = 2.0


def load(rec: Path) -> tuple[pd.DataFrame, pd.DataFrame]:
    subprocess.run([str(ROOT / "replay"), str(rec), str(ROOT / "rumble.conf")], check=True)
    frames = pd.read_csv(f"{rec}.frames.csv", low_memory=False)
    events = pd.read_csv(f"{rec}.events.csv")
    return frames, events


def fmt_time(s: float) -> str:
    return f"{int(s // 60):d}:{s % 60:05.2f}"


def summarize(frames: pd.DataFrame, events: pd.DataFrame) -> None:
    dt = frames.sim_s.diff()
    dt = dt[(dt > 0) & (dt < 0.25)]
    duration = frames.host_s.iloc[-1] - frames.host_s.iloc[0]
    moving = frames[frames.speed_kmh.abs() > 2]
    print(f"\nDrive: {fmt_time(duration)} wall time, {len(frames)} frames, "
          f"physics step median {dt.median() * 1000:.1f} ms (~{1 / dt.median():.0f} Hz)")
    print(f"Speed: max {frames.speed_kmh.abs().max():.0f} km/h, moving {len(moving) / max(len(frames), 1):.0%} of the time")

    surf = frames.loc[frames.speed_kmh.abs() > 2, "surfaces"].value_counts(normalize=True).head(8)
    print("Surfaces while moving: " + ", ".join(f"{k} {v:.0%}" for k, v in surf.items()))

    for name, col in [("bump", "env_bump"), ("collision", "env_collision"), ("surface", "env_surface")]:
        active = (frames[col] > 0.05).mean()
        print(f"  {name:9s} active {active:.1%} of frames, max {frames[col].max():.2f}")
    print(f"  motors on: low {(frames.sent_low > 0).mean():.1%}, high {(frames.sent_high > 0).mean():.1%} of frames (as sent live)")

    hits = frames[frames.hit == 1]
    print(f"\nCollisions detected: {len(hits)}")
    for _, r in hits.iterrows():
        print(f"  {fmt_time(r.host_s)}  {r.speed_kmh:5.1f} km/h  crash wear +{r.crash_wear_delta:.4f}  jolt {r.jolt:.1f}")
    resets = frames[frames.reset == 1]
    print(f"State resets (loads/teleports): {len(resets)} at " + ", ".join(fmt_time(t) for t in resets.host_s))

    other = events[~events.type.isin(["marker", "substances"])]
    if len(other):
        print("\nEvents:")
        for _, e in other.iterrows():
            detail = str(e.detail) if pd.notna(e.detail) else ""
            print(f"  {fmt_time(e.host_s)}  {e.type} {detail[:110]}")


def marker_report(frames: pd.DataFrame, events: pd.DataFrame, out_dir: Path) -> None:
    markers = events[events.type == "marker"].reset_index(drop=True)
    print(f"\nMarkers: {len(markers)}")
    if not len(markers):
        return
    out_dir.mkdir(exist_ok=True)
    defl_cols = [c for c in frames.columns if c.startswith("defl") and frames[c].notna().any()]

    for i, m in markers.iterrows():
        t = m.host_s
        w = frames[(frames.host_s >= t - WINDOW_BEFORE_S) & (frames.host_s <= t + WINDOW_AFTER_S)]
        # What was happening in the 2 s before the press (reaction time).
        lead = w[(w.host_s >= t - 2.0) & (w.host_s <= t)]
        if not len(lead):
            continue
        print(f"  #{i + 1} {m.detail:11s} at {fmt_time(t)}: {lead.speed_kmh.mean():5.1f} km/h, "
              f"surfaces {sorted(set(lead.surfaces))}, "
              f"bump_metric max {lead.bump_metric.max():.4f}, jolt max {lead.jolt.max():.1f}, "
              f"crash wear +{lead.crash_wear_delta.sum():.4f}, "
              f"sent low/high max {lead.sent_low.max():.2f}/{lead.sent_high.max():.2f}")

        x = w.host_s - t
        fig, ax = plt.subplots(6, 1, figsize=(11, 13), sharex=True)
        ax[0].plot(x, w.speed_kmh, label="speed km/h")
        ax[0].plot(x, w.brake * 100, label="brake %", alpha=0.6)
        ax[0].plot(x, w.throttle * 100, label="throttle %", alpha=0.6)
        ax[0].legend(loc="upper left", fontsize=8)
        for c in defl_cols:
            ax[1].plot(x, w[c] * 100, lw=0.8, label=c)
        ax[1].set_ylabel("deflection cm")
        ax[1].legend(loc="upper left", fontsize=6, ncol=6)
        ax[2].plot(x, w.bump_metric * 100, label="fast deflection cm (bump_metric)")
        ax[2].plot(x, w.acc_y, label="vertical accel m/s²", alpha=0.6)
        ax[2].legend(loc="upper left", fontsize=8)
        ax[3].plot(x, w.jolt, label="horizontal jolt m/s²")
        ax[3].plot(x, w.crash_wear_delta * 1000, label="crash wear ×1000")
        ax[3].plot(x, w.cab_angacc_x, label="cabin pitch accel", alpha=0.6)
        ax[3].legend(loc="upper left", fontsize=8)
        ax[4].plot(x, w.env_bump, label="bump")
        ax[4].plot(x, w.env_collision, label="collision")
        ax[4].plot(x, w.env_surface, label="surface")
        ax[4].set_ylim(-0.05, 1.05)
        ax[4].legend(loc="upper left", fontsize=8)
        ax[5].plot(x, w.sent_low, label="sent low (heavy)")
        ax[5].plot(x, w.sent_high, label="sent high (buzz)")
        ax[5].plot(x, w.model_low, "--", lw=0.8, label="model low (current conf)")
        ax[5].plot(x, w.model_high, "--", lw=0.8, label="model high (current conf)")
        ax[5].set_ylim(-0.05, 1.05)
        ax[5].legend(loc="upper left", fontsize=8)
        ax[5].set_xlabel("seconds relative to marker")
        # Surface changes as text along the top plot.
        changes = w[w.surfaces != w.surfaces.shift()]
        for _, r in changes.iterrows():
            ax[0].annotate(r.surfaces, (r.host_s - t, ax[0].get_ylim()[1]), fontsize=6, rotation=30, va="top")
        for a in ax:
            a.axvline(0, color="red", lw=1)
            a.grid(alpha=0.3)
        fig.suptitle(f"Marker #{i + 1}: {m.detail} at {fmt_time(t)}")
        fig.tight_layout()
        fig.savefig(out_dir / f"marker_{i + 1:02d}_{m.detail}.png", dpi=90)
        plt.close(fig)


def overview(frames: pd.DataFrame, events: pd.DataFrame, out_dir: Path) -> None:
    out_dir.mkdir(exist_ok=True)
    fig, ax = plt.subplots(3, 1, figsize=(14, 8), sharex=True)
    t = frames.host_s / 60
    ax[0].plot(t, frames.speed_kmh, lw=0.7)
    ax[0].set_ylabel("km/h")
    ax[1].plot(t, frames.bump_metric * 100, lw=0.5, label="fast deflection cm")
    ax[1].plot(t, np.clip(frames.jolt, 0, 100) / 10, lw=0.5, label="jolt /10")
    ax[1].legend(loc="upper left", fontsize=8)
    ax[2].plot(t, frames.sent_low, lw=0.5, label="sent low")
    ax[2].plot(t, frames.sent_high, lw=0.5, label="sent high")
    ax[2].legend(loc="upper left", fontsize=8)
    ax[2].set_xlabel("minutes")
    for _, m in events[events.type == "marker"].iterrows():
        for a in ax:
            a.axvline(m.host_s / 60, color="red" if m.detail == "false_alarm" else "green", lw=0.8)
    fig.tight_layout()
    fig.savefig(out_dir / "overview.png", dpi=80)
    plt.close(fig)


def main() -> None:
    if len(sys.argv) > 1:
        rec = Path(sys.argv[1])
    else:
        recs = sorted((ROOT / "recordings").glob("drive-*.rec"))
        if not recs:
            sys.exit("no recordings found")
        rec = recs[-1]
    print(f"Recording: {rec}")
    frames, events = load(rec)
    if frames.empty:
        sys.exit("recording has no frames")
    out_dir = rec.with_name(rec.stem + "-plots")
    summarize(frames, events)
    marker_report(frames, events, out_dir)
    overview(frames, events, out_dir)
    print(f"\nPlots in {out_dir}")


if __name__ == "__main__":
    main()
