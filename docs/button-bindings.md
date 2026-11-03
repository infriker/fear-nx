# Button bindings

Close the game and add one `[input.bindings]` table to `fear-nx.toml` beside the
NRO. Bindings are loaded at startup and require a full restart to change.
Keys name physical Switch buttons; values name the Xbox 360 inputs sent
to F.E.A.R. Prompts and the game's selected control preset still use Xbox inputs.

```toml
[input.bindings]
zl = "rs"
r3 = "lt"
```

This exchanges ZL and right-stick click. With a game preset where RS aims and
LT throws grenades, it gives ZL aim and R3 grenade. Both bindings are needed for
a swap; setting only one deliberately allows two buttons to perform one action.

Physical keys: `a`, `b`, `x`, `y`, `l`, `r`, `zl`, `zr`, `l3`, `r3`, `plus`,
`minus`, `up`, `down`, `left`, `right`.

| Value | Xbox input |
|---|---|
| `a`, `b`, `x`, `y` | Face button |
| `lb`, `rb` | Shoulder button |
| `lt`, `rt` | Trigger (0 or 255, as on Switch) |
| `ls`, `rs` | Stick click |
| `start`, `back` | Start / Back |
| `up`, `down`, `left`, `right` | D-pad |
| `none` | Disable this physical button |
| `default` | Inherit the normal layout |

Omitted, empty or unknown values also inherit the normal layout. Destination
names are case-insensitive; key names are lowercase. `xbox_layout` controls
inherited face buttons; an explicit override always wins. Sticks, vibration,
and physical ReXGlue menu shortcuts are unchanged. Remaps affect the original
game's menus too; port settings keep their normal Switch navigation.

The SDK's Save to config may flatten a binding to `input_bindings_zl = "rs"`
at the TOML root. Both forms are supported. Avoid defining the same binding in
both forms. Removing the override or setting it to `default` restores it.

Host checks (no NRO execution):

```sh
c++ -std=c++23 -O2 -Ithird_party/rexglue-sdk/include src/input/button_bindings_test.cpp -o /tmp/bindings-test
/tmp/bindings-test
```
