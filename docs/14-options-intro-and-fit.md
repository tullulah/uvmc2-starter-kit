# 14 — The OPTIONS screen, the intro card and fitting the picture

What a finished game on the UVMC2 has around it, from SDK v1.4.8 on: the Vectrex Studio
card, a standard OPTIONS screen with the settings the player expects (the screen's
orientation, where the sound comes out, the refresh, the brightness, the buttons), and its
picture as large as the tube shows. None of it is required: a game that declares nothing
starts straight away, as it always did. Everything here is UVMC2-only; on the debug
cartridge the calls exist and do nothing that would change the game.

## The settings a game declares

The settings that belong to the **console** (the beam calibration) live in
`config/uvm2.cfg` and are shared by every game ([12](12-calibrating-a-console.md)). The
ones that belong to the **game** live in `config/<NAME>.CFG`, layered on top, and each game
declares which ones it has. It does so in a function the runtime calls before the game's
`main`, so they are loaded before anything is drawn:

```c
#include "uvm2_config.h"

void uvm2_game_settings(void)
{
    uvm2_config_game("ALPHAONE", UVM2_SETTING_OPTIONS | UVM2_SETTING_ROTATE | UVM2_SETTING_AUDIO);
    uvm2_config_title("ALPHA ONE");                        /* what the screen shows on top */
#ifdef UVM2_HAS_BUTTON_MAP
    uvm2_config_buttons("FIRE", "THRUST", "COIN", "START");
#endif
}
```

`name` is 8.3 without the extension. The bits:

| bit | value | the setting |
|---|---|---|
| `UVM2_SETTING_HZ` | 1 | REFRESH: the game's own rate, 50, 60 or MAX (`uvm2_setting_hz`) |
| `UVM2_SETTING_MENU` | 2 | START IN MENU / IN GAME (`uvm2_setting_menu`) |
| `UVM2_SETTING_ROTATE` | 4 | SCREEN VERTICAL / HORIZONTAL (`uvm2_setting_rotate`) |
| `UVM2_SETTING_AUDIO` | 8 | SOUND JACK / VECTREX (`uvm2_setting_audio`) |
| `UVM2_SETTING_BRIGHT` | 16 | BRIGHTNESS, a gain in percent (`uvm2_setting_bright`) |
| `UVM2_SETTING_OPTIONS` | 32 | the standard OPTIONS screen; brings HZ, MENU, BRIGHT and BUTTONS |
| `UVM2_SETTING_BUTTONS` | 64 | the CONTROLS line, the button map (`uvm2_setting_buttons`) |

Declare only what means something in your game. A vertical arcade game does not offer
SCREEN; a game with no jack sound does not offer SOUND. `ROTATE` and `AUDIO` appear on the
OPTIONS screen only when declared.

## The OPTIONS screen

With `UVM2_SETTING_OPTIONS` the runtime shows the screen before `main`:

```
        ALPHA ONE

        PLAY
        SCREEN VERTICAL          only with UVM2_SETTING_ROTATE
        SOUND JACK               only with UVM2_SETTING_AUDIO
        REFRESH 50 HZ
        BRIGHTNESS 100
        CONTROLS
        CALIBRATE
        START IN MENU
```

The stick up and down picks a line; left, right or button 1 changes it; button 1 on PLAY,
or button 4 anywhere, starts the game. A change is saved to `config/<NAME>.CFG` as it is
made, and a failed save is shown on the screen rather than lost. CALIBRATE opens the
console's calibration ([12](12-calibrating-a-console.md)).

**START IN GAME** keeps the screen closed at launch. To open it again, hold button 4 for a
second while the game starts; without that there would be no way back once it is turned
off.

**SCREEN HORIZONTAL** is the console on its side: the drawing is turned 90 degrees
(`uvm2_draw_rotate`), text included. It names how the console stands, not how the game
was built.

**REFRESH** offers the game's own rate first, the one it was built with (`UVM2_HZ`,
`uvm2_config_native_hz()`), then 50, 60 and MAX. Any rate from 20 to 100 is held: an arcade
game at 38 Hz shows "REFRESH 38 HZ" and keeps it. The screen applies the setting on its
way out; a game without the screen applies it itself, or not.

**BRIGHTNESS** is a gain applied to every intensity the game asks for, inside
`uvm2_draw_intensity`, so it reaches the games that set their own brightness vector by
vector (the calibration's BRIGHT is only the default intensity, which such a game
overrides at once). `uvm2_draw_intensity_current()` still returns what the game asked
for.

`uvm2_options_menu()` opens the same screen from inside a game, for a pause menu.

### The buttons

CONTROLS lists the game's four buttons by the names given to `uvm2_config_buttons`
(BUTTON 1..4 without it) and the console button that does each. Pick a line and press
the button you want for it; the line that had that button takes this one's, so the four
stay four. DEFAULTS puts them back as wired. The map is saved as `buttons`, four digits:
`1234` is as wired, `2134` swaps 1 and 2.

The map is applied where the game reads its buttons (`SYS_READ_BUTTONS`,
`SYS_READ_BTN_RAW`), so a game sees its own buttons, combinations included, without
knowing about it. The SDK's own screens (OPTIONS, calibration, the HUD's combination)
keep the console's buttons as wired, so a bad map can always be undone. For a game that
remaps on its own terms: `uvm2_buttons_set(map)` (0 and no change when the map is not
the four buttons once each) and `uvm2_buttons_remap(pressed)`.

`#ifdef UVM2_HAS_BUTTON_MAP` lets a game name its buttons and still build against an
SDK older than v1.4.11.

## The Vectrex Studio card

A game with the OPTIONS screen shows the Vectrex Studio card before it: the logo turns as
it fades in, holds over the wordmark, and fades out to the jingle on the jack, about 4.2 s.
Any button skips to the fade, and it follows the SCREEN setting. Games without the OPTIONS
screen never show it.

The jingle is 16 kHz IMA ADPCM, about 34 KB of the card's 36 KB of SRAM. Three ways to
build:

| `make uvm2 …` | the card | its cost |
|---|---|---|
| (default) `UVM2_INTRO=1` | with the jingle inside the image | ~36 KB of SRAM |
| `UVM2_INTRO=sd` | the jingle read off the SD card, `VSJINGLE.BIN` at its root | ~2 KB |
| `UVM2_INTRO=0` | no card: OPTIONS (or the game) comes straight away | nothing |

`UVM2_INTRO=sd` is for an image with no room left for the jingle. The file is
`sdk/uvm2-sdk/VSJINGLE.BIN`; it is checked (size and FNV-1a), and without it, or with
anything else under that name, the card plays silent. It never plays noise. Set the choice
in the game's Makefile (`UVM2_INTRO = sd`) so the IDE's builds keep it.

## The picture, as large as the tube shows

A port draws in its own units: an arcade game's screen coordinates, scaled once to the
Vectrex's. The tube is not the arcade's monitor, and its shape is the console's: wider or
narrower, and turned when the console stands on its side. `uvm2_view_field` fits the
picture to it:

```c
/* the box the game draws in, in the units v_directDraw32 takes, and the shape
 * that box had on its own monitor: 4:3 for a landscape cabinet, 0:0 for its own */
uvm2_view_field(-13000, 13000, -9750, 9750, 4, 3);
```

From then on everything `v_directDraw32` draws is scaled and centred to be as large as the
tube shows **without changing its shape**: upright with its width across the tube, with
SCREEN HORIZONTAL with its width along it. Without the call nothing changes.

"What the tube shows" is the console's calibration: WIN X, WIN Y and ASPECT
([12](12-calibrating-a-console.md)). A console that never set them keeps the stock 15500
square, which is a safe area and not the tube, and gets the tube measured on a console by
photograph instead (+-18000 x +-20500, `UVM2_VIEW_REF_WIN_X/Y`). `uvm2_view_map(&x, &y)`
gives the mapping for a game that needs it: a HUD placed in the band the fit leaves empty,
for instance. `UVM2_HAS_VIEW_FIT` says the SDK has it (v1.4.11 on).

**Measure the box, do not guess it.** The arcade's visible area is what MAME shows, not
the coordinate range the game's code can produce; a box that is too large leaves the
picture small, and one that is too small pushes it off the tube.

## The reset button, with the card and the screen

One press of the console's reset starts the game again — through the card and the OPTIONS
screen if it has them — and two presses within a second go back to the UVMC2's menu
([06](06-sound-input-sd.md)). While the second press is awaited the screen says RESETTING.

## Where they live

`uvm2_config.h` (settings, buttons, the screen, the card), `uvm2_draw.h` (the fit),
`uvm2_options.c`, `uvm2_intro.c`. The API is in [08](08-api-reference.md).
