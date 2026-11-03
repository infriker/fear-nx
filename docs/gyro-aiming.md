# Gyroscope aiming

Gyro aiming is on by default throughout gameplay, including hip fire. Set
`gyro_always_on = false` under `[input]` to restore aim-only behavior. That mode
follows F.E.A.R.'s actual aim state, including toggle aim and remapped buttons.
There is no separate hold-ZL requirement. Menus and focus changes still suppress
motion in both modes.

## Turn off and save

1. Open the port's settings with **L + R + D-pad Right**. Prefer doing this
   from the game's pause menu, since the shoulder buttons retain their normal
   game actions.
2. Select **Input**, uncheck **input_gyro_enabled**, and choose **Save to config**.
3. Close the settings with the same combination and return to the game.

To enable gyro again, check **input_gyro_enabled** and save. Motion is applied
during gameplay, or only while aiming when `gyro_always_on = false`.

The checkbox applies immediately. Saving writes `fear-nx.toml` beside the NRO.
Installing this NRO alone is sufficient: an older TOML need not be replaced.
When `gyro_enabled` is absent, gyro is enabled by default. An explicit saved
`gyro_enabled = false` remains disabled after updating; enable it in settings
if desired.

To disable without the settings menu, close the game, then set
`gyro_enabled = false` in the existing `[input]` section of
`sdmc:/switch/fear-nx/fear-nx.toml`. Restart the game. Do not replace the rest
of your configuration.

Default values in the existing `[input]` section:

```toml
gyro_enabled = true
gyro_always_on = true
gyro_sensitivity = 1.5
gyro_smoothing_ms = 16.0
gyro_deadzone_dps = 0.15
gyro_invert_y = false
gyro_invert_x = false
```

The default sensitivity is 1.5: one degree of controller rotation produces
1.5 degrees of camera rotation, apart from smoothing and the small drift
deadzone. Sensitivity 1.0 gives a one-to-one ratio. Sensitivity is
independent of the game's right-stick sensitivity. Zero smoothing disables the
filter. Horizon's standard stationary drift correction is enabled; hold the
controller still briefly if needed. Do not add a second `[input]` table.

Supported motion sources are the handheld console, player one's Pro Controller,
and a detached Joy-Con pair. The pair uses the right Joy-Con and falls back to
the left only if the right is disconnected. A controller with no usable motion
sensor retains normal buttons and sticks. A single sideways Joy-Con is not
supported for gyro aiming.

## Implementation and evidence

The design was studied in [EchoPatch](https://github.com/Wemino/EchoPatch/tree/ee12ec9c2d27cc014356b6ecc687dda123d591b8):

- `src/Client/Controller/SDLGamepadSupport.cpp` restricts local camera rotation
  to player movement and gates it with the actual `BeginAim`/`EndAim` state.
- `src/Controller/Controller.cpp` applies sensitivity, inversion and exponential
  smoothing to gyro rates before adding pitch/yaw angles.

The Switch implementation uses libnx HID directly and Horizon drift correction.
It does not require SDL, Win32, PC hooks or a ReXGlue upgrade. The SDK base snapshot remains pinned. The separate `input-release.patch`
overlay adds button bindings and build-time profiler/debug switches. It is original project code inspired
by that approach; no EchoPatch source files are bundled.

The supported Xbox executable was inspected locally. The relevant functions are:

| Address | Observed behavior |
|---|---|
| `0x8237F648` | Player movement update; calls look calculation and passes a stack-local pitch/yaw/roll vector to the camera, unless its own menu/camera gate blocks it. |
| `0x8237DC88` | Reads look commands 22/23 and stores the calculated pitch/yaw deltas at player offsets 500/504. |
| `0x823B70A8` | Applies local rotation to the player camera. This is the gyro injection point, only during the movement update above. |
| `0x82387DB8` | Returns the aim singleton at `0x83371764`. |
| `0x8246D7A8` / `0x8246D858` | Begin/end aim; update the singleton's first byte only when the game's checks allow the transition. |

The player pointer at `0x833708DC` and its camera at offset 40 are checked at the
hook. The aim byte is read each time rather than mirrored in a button toggle.
The application already validates the full XEX SHA-256; the gyro setup also
checks function signatures after loading. Strong hook symbols delegate all
original behavior to the generated `__imp__` functions.

HID samples are processed oldest first, once per sampling number, using their
nanosecond intervals. Angular velocities are converted from turns/second to
radians/second. Pitch uses local X. Horizontal rotation projects all three
angular-velocity components onto the smoothed gravity direction, following the
principle used by [BLACK-NX-PORT](https://github.com/gerisson1996/BLACK-NX-PORT/blob/main/switch/gyro.cpp).
Thus turning an upright handheld around world up moves the view horizontally;
banking its screen around the screen normal does not. The gravity filter uses
sample time and rejects strong linear acceleration. `gyro_invert_x` reverses
horizontal motion independently of vertical inversion. The
official [libnx example](https://github.com/switchbrew/switch-examples/blob/master/hid/sixaxis/source/main.c)
provides the handle and controller-style selection pattern.

Menu input capture and application focus suppress motion. Leaving aim mode
when `gyro_always_on = false`,
changing settings/controllers, stale samples, buffer overruns and long gaps
reset the integrator. Motion from before aiming or from a pause is discarded.

## Verification

`src/input/gyro_math_test.cpp` checks units, axes, inversion, sensitivity,
deadzone, invalid samples, equal results at 25/30/60 FPS, duplicate rejection,
aim transitions, reconnection, gaps, buffer overruns, upright/flat/tilted
gravity projection, and rejection of upright screen banking. Run it with a C++23
host compiler without `NDEBUG`:

```sh
g++ -std=c++23 -Wall -Wextra -Werror -O2 src/input/gyro_math_test.cpp -o /tmp/gyro-test
/tmp/gyro-test
```

An explicit `gyro_sensitivity` value in an existing configuration overrides
the default. Set it to `1.5` to use the v0.1.5 default with an older TOML.

Hardware regression checks:

- Off: moving the controller never affects the camera.
- Always on: both hip fire and aimed fire accept gyro.
- Aim-only: entering aim activates motion; exiting aim stops it.
- Upright handheld: rotate left/right around world up; banking the screen should
  not substitute for horizontal aim.
- Pause/settings/HOME: no accumulated camera jump when returning.
- Right stick and gyro can be used together.
- Save the setting, restart, and confirm it persists.
- If available, repeat in handheld mode, with detached Joy-Cons and with a Pro
  Controller, including disconnect/reconnect.
