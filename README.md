# Cave Story frame pacing + frame interpolation fix (for the 2004 freeware Doukutsu.exe and mods built on it)

## INSTALL

  Copy dinput.dll into the same folder as the game's .exe (works for mods
  too, whatever the exe is called). To uninstall, delete dinput.dll.

## WHAT IT DOES

  1. Steady game speed
     The game limits its speed with GetTickCount() and Sleep(1), which on
     modern Windows only tick every ~15.6 ms, so frames came out at 16 / 31 ms
     instead of a steady 20 ms. This is replaced by a precise timer. The game
     logic runs at exactly its intended rate (50 per second in vanilla).

  2. Frame interpolation (on by default)
     The game logic still runs at 50 per second, but a frame is shown on
     every refresh of your monitor (60, 120, 144, 240 Hz...), timed to the
     display. Each shown frame is drawn by the game's own drawing code with the
     camera, Quote, enemies, bullets, effects, backgrounds and water level
     placed part-way between their positions in the last two game ticks, so
     motion is smooth and evenly spaced. Game speed, physics and timing are
     not affected: positions are only changed temporarily while a frame is
     drawn, then restored exactly.

     The scrolling cloud backgrounds (bkMoon / bkFog: Outer Wall, Balcony,
     Throne Room, Clock Room...) are redrawn so each cloud layer moves
     smoothly at its own speed. If a mod has changed how those backgrounds
     are drawn, the game's own drawing is used instead.

     The HUD, text boxes, menus and fades are shown as the game drew them.
     Things that jump far in one tick (teleports, room changes) aren't
     blended. Screens with no moving world (title, inventory, map) are shown
     as-is, but still in sync with the display.

     Interpolation shows each game tick spread over the following 20 ms, so
     it adds up to one game tick (about 10 ms on average) of display delay.
     If you'd rather have the lowest possible input delay, turn it off (below).

  3. Crisp text (no ClearType)
     The game asks Windows for its default text quality, so with ClearType on
     (the Windows default) the text box letters get blurry, coloured edges.
     The fix makes the game's text render as sharp, single-colour pixels, the
     way it looked on 2004-era Windows. Only the game is affected; your
     Windows ClearType setting is not changed.

## SETTINGS (optional)

  Create framefix.ini next to the exe (an example file is included):

      [FrameFix]
      FPS=0
      Interpolation=1
      ClearType=0
FPS=0 use the game's own rate (50 in vanilla, or whatever a mod sets). Any other value forces that logic rate, e.g. FPS=60 for a mod meant to run at 60 (decimals like 59.94 work).
  Interpolation=1  smooth motion at your monitor's refresh rate.
  Interpolation=0  steady game speed only; the game shows its own frames.
  ClearType=0      crisp, non-smoothed text (default).
  ClearType=1      leave the game's text to Windows' font smoothing setting.

## MODS

  The frame interval is read from the game's code every tick, so mods that
  change the frame rate keep their speed. Mods that add their own drawing
  are still shown (that drawing just isn't interpolated). If a mod draws
  world objects with its own custom code, those objects may look slightly
  jittery next to the smooth camera; set Interpolation=0 for that mod.

  The fix assumes the standard Doukutsu.exe layout (mods made with the usual
  tools keep it). Engine ports such as CSE2, NXEngine, doukutsu-rs or
  Cave Story+ don't use it.

## NOTES

  * Works alongside DirectDraw wrappers such as dgVoodoo2 or DDrawCompat
    (it doesn't replace ddraw.dll).
  * Display sync uses the Windows compositor (DWM). If that isn't available it
    falls back to DirectDraw's vertical-blank wait, then to a timer at the
    monitor's refresh rate.

## BUILD FROM SOURCE (MinGW, 32-bit)

  i686-w64-mingw32-gcc -O2 -shared -s -static-libgcc -o dinput.dll framefix.c dinput.def -lwinmm -luser32 -lgdi32 -Wl,--enable-stdcall-fixup

