# ETS2 Mac controller rumble

A tiny Euro Truck Simulator 2 plugin for macOS that makes the game's own
controller rumble work with gamepads that Steam Input reports as **"Unknown"**,
such as the **8BitDo Pro 3 in D mode over Bluetooth**.

Without it, such a controller works fine for driving but never vibrates. With
it, you get the game's normal rumble (road surface, braking, collisions…),
delivered by Steam exactly as for an Xbox or PlayStation controller.

## The problem

ETS2 sends vibration only to controller types that it has a motor mapping for.
Its `steam_controller_vibration` definitions cover Steam Input types 1–5, 10 and 13
(Steam Controller, Xbox, generic gamepad, PlayStation, …). A controller that
Steam Input reports as type 0, *Unknown*, gets no vibration commands at all.
Steam itself is perfectly able to rumble it; the game just never asks.

## How the plugin fixes it

The plugin is loaded by the game through the SCS Telemetry SDK (it doesn't use
any telemetry data; that's only how it gets loaded). Inside the game, it
patches four entries of the game's `ISteamInput` (`SteamInput006`) interface:

- **`GetInputTypeForHandle`**: an *Unknown* controller is reported as a
  *Generic Gamepad* (type 4), so the game uses its standard gamepad vibration
  mapping. Steam then delivers the rumble itself.
- **`GetConnectedControllers`**: the game sets controllers up about 3 seconds
  after launch, but plugins only load at about 11 seconds. The plugin answers
  the game's next 2 polls (about 2 seconds) with "no controllers", so the game
  sets the controller up again, this time as a gamepad.
- **`TriggerVibration` / `TriggerVibrationExtended`**: passed through unchanged
  and counted, for the log.

Before patching, the plugin checks that every slot it replaces points into
Steam's own client library. If the layout is not what it expects (for example
after a game or Steam update), it logs a warning and leaves everything as it
was. The original interface is restored when the game unloads the plugin.

## Tested with

| | |
|---|---|
| Game | Euro Truck Simulator 2 1.61.1.9s (Steam, macOS, Vulkan renderer) |
| macOS | 27.0.1, Apple M3 Pro (the game runs under Rosetta) |
| Controller | 8BitDo Pro 3 in D mode, Bluetooth (`2dc8:6009`) |

Other controllers that Steam Input reports as *Unknown* should benefit in the
same way, but I haven't tested any. Controllers that Steam already recognises
(Xbox, PlayStation, Switch Pro, …) don't need this plugin. American Truck
Simulator uses the same engine and might work too; it is untested.

## Install

You need the Xcode Command Line Tools (`xcode-select --install`).

```sh
git clone https://github.com/rshrj/ets2-mac-controller-rumble.git
cd ets2-mac-controller-rumble
make
```

This builds `ets2_steam_input_fix.dylib`. Copy it into the game's `plugins`
folder:

1. In Steam, right-click **Euro Truck Simulator 2** → **Manage** → **Browse local files**.
2. Right-click **Euro Truck Simulator 2.app** → **Show Package Contents**, then
   open **Contents/MacOS**.
3. Create a folder called `plugins` there if it doesn't exist, and put
   `ets2_steam_input_fix.dylib` in it.

macOS normally blocks Terminal from writing inside other apps, which is why
Finder is the easy way. If you have given your terminal the *App Management*
permission (System Settings → Privacy & Security), `make install` does the
same copy.

Make sure vibration is enabled both in the game's controller options and in
Steam's settings for the controller.

## Check that it works

Start the game. About 11 seconds after launch the controller shows as
*missing* in the game's controller settings for 2–3 seconds, then comes back.
That's the plugin making the game set it up again.

The game log (`~/Library/Application Support/Euro Truck Simulator 2/game.log.txt`)
should contain:

```
[steam-input-fix] installed (redetect_polls=2)
[steam] Initializing device '@@steam_controller@@ …' … hw: Generic Input (4)
```

and, after you quit the game, a summary like:

```
[steam-input-fix] type queries=3 (overridden 3), vibration calls=46502, hidden polls=2
```

A non-zero number of vibration calls means the game is sending rumble to your
controller.

## Uninstall

Delete `ets2_steam_input_fix.dylib` from the game's `plugins` folder (or run
`make uninstall`).

## Limitations

- **macOS only.** The plugin is built for x86_64, like the macOS game. The
  same idea could work on Windows or Linux but would need a different build,
  and I haven't tried it.
- **Depends on the game's Steamworks version.** The plugin relies on the
  `SteamInput006` interface and its method order. If a future game update
  changes that, the plugin logs a warning and does nothing.
- **The controller drops out for 2–3 seconds** about 11 seconds after launch.

## Other ETS2-on-Mac notes

Two things I found while setting this up, in case they save someone time:

- **HDR breaks the Steam overlay.** With HDR on, ETS2 by default uses a 10-bit
  `BT.2100 PQ` swapchain, which the Steam overlay (Shift+Tab, notifications,
  the in-game store) draws as psychedelic garbage, with blacks turning bright
  blue. Adding the launch option `-color_space scrgb` makes the game use a
  16-bit floating-point scRGB swapchain instead; HDR still works and the
  overlay draws correctly. (`-color_mode` only accepts `sdr`; other values are
  ignored.)
- **`-mvk-arg-buffers` made things worse** on an M3 Pro: random frame freezes.
  The game disables Metal argument buffers on x86_64 by default for a reason.

My full launch options are `-rdevice vk -color_space scrgb`.

## Experimental: telemetry-based rumble

[`experimental/`](experimental/) has an earlier, larger approach: a plugin that
streams truck telemetry to a native helper, which generates its own rumble
effects from suspension, road surface and collisions, and can record and
replay drives for tuning. It is unfinished and not needed for the game's own
rumble; see its [README](experimental/README.md).

## Credits and licence

- `scs_sdk/` contains the [SCS Telemetry SDK](https://modding.scssoft.com/wiki/Documentation/Engine/SDK/Telemetry)
  headers, © SCS Software, MIT licence (`scs_sdk/sdk_license.txt`).
- This project is not affiliated with SCS Software, Valve or 8BitDo.

Released under the [MIT licence](LICENSE).
