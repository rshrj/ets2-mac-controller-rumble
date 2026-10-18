# Experimental: telemetry-based rumble

> **Status: unfinished.** This was the first approach to controller rumble in
> ETS2 on macOS, before it turned out that the
> [small Steam Input plugin](../README.md) is enough to get the game's own
> rumble. It is kept here for anyone who wants to build custom rumble
> effects. The effects model is not tuned, and its effects are off by default.

## What it does

```
ETS2 ──► ets2_rumble_plugin.dylib ──UDP (localhost)──► rumble_helper ──SDL3──► controller
          (x86_64, inside the game)                      (native)
```

- **`ets2_rumble_plugin.dylib`** is an SCS telemetry plugin. Every physics
  frame, it sends truck state (suspension, wheel substances, speed,
  acceleration, damage, …) to the helper. It starts the helper when the game
  starts. It also contains the same Steam Input hook as the main plugin, and
  forwards the game's own vibration commands to the helper.
- **`rumble_helper`** opens the controller with SDL3 and turns the telemetry
  into two motor levels using the model in `effects.c`: bumps (fast suspension
  movement), surface texture (gravel, dirt, rumble strips, scaled by speed),
  collisions and a faint engine hum, layered with the game's own vibration.
  It re-reads `rumble.conf` within a second of each change, even mid-drive.
- **Recordings.** With `record = 1`, every drive is saved to `recordings/`.
  The controller's back paddles then stamp markers into the recording:
  **PL** for "should have rumbled here" and **PR** for "rumbled when it
  shouldn't have". A short tick confirms each marker.
- **`replay`** runs a recording through the current model and writes CSVs.
  **`analyze.py`** summarises a drive and plots the moments around each
  marker, so the model can be tuned against real drives.

Don't install this plugin together with the main `ets2_steam_input_fix.dylib`:
both patch the same Steam Input calls.

## Build

```sh
brew install sdl3 pkg-config
cd experimental
make
```

The plugin stores the helper's absolute path at build time, so build it where
you'll keep it and rebuild if you move the folder. Install the plugin the same
way as the main one (see the [main README](../README.md#install)), or run
`make install`.

## Use

| Command | What it does |
|---|---|
| `make demo` | Feel the effects without the game: plays a scripted drive (smooth road, braking, pothole, gravel, rumble strip, crash). |
| `make calibrate` | Finds the weakest level each motor can be felt at and writes `low_min` / `high_min` to `rumble.conf`. |
| `./replay recordings/drive-….rec` | Writes `.frames.csv` and `.events.csv` for a recording. |
| `.venv/bin/python analyze.py` | Summary and plots for the newest recording (set up the venv with `python3 -m venv .venv && .venv/bin/pip install -r requirements.txt`). |

Settings:

- **`rumble.conf`**: overall strength, the game's own vibration, each effect's
  strength, motor minimums, and recording on/off.
- **`plugin.conf`**: the Steam Input hook and the re-detection at startup.
  Read each time the game starts telemetry, so there's no need to reinstall
  the plugin after editing it.

The helper logs to `rumble_helper.log` next to itself, and exits when the game
shuts telemetry down or after two minutes without data.

## Files

| File | |
|---|---|
| `plugin.cpp` | Telemetry plugin and Steam Input hook (x86_64) |
| `protocol.h` | UDP packet layout shared by the plugin and the helper |
| `rumble_helper.c` | Live helper: SDL3 output, config reload, recording, markers, calibration |
| `effects.c`, `effects.h` | The rumble model, shared by the helper and `replay` |
| `recording.h` | Recording file format |
| `replay.c` | Offline replay of recordings to CSV |
| `fake_telemetry.c` | Scripted test drive for `make demo` |
| `analyze.py` | Recording summary and plots |
| `hashfs_grep.py` | Searches the game's `.scs` archives (HashFS v2) for text, e.g. to read the game's own vibration and substance definitions |

## Notes

- Steam starts the game with SDL environment variables such as
  `SDL_GAMECONTROLLER_IGNORE_DEVICES`, which hide physical controllers from
  SDL. The helper inherits them from the game, so it removes them before
  starting SDL.
- On the 8BitDo Pro 3, SDL reports the back paddles PL / PR as
  `LEFT_PADDLE2` / `RIGHT_PADDLE2`.
- The model's starting values (per-substance bump strength, the game's own
  force-feedback parameters) came from the game's data files, extracted with
  `hashfs_grep.py`. Those extracted files are SCS Software's content and are
  not included here.
- `hashfs_grep.py` follows the HashFS v2 format as implemented in
  [sk-zk/TruckLib](https://github.com/sk-zk/TruckLib) (`TruckLib.HashFs`).
