# Sudoku

A native C++23 Android Sudoku app modeled on sudoku.com. Every game is Extreme.
It requests no permissions, works offline, and uses Android's built-in
`NativeActivity`, so the APK has no Java sources or DEX code.

The screen follows sudoku.com: a title with **New Game**, the difficulty,
**Mistakes: n/3**, and a clock with a pause button above the board, then
**Undo**, **Erase**, and **Notes** with an ON/OFF badge, and a keypad of
digits 1-9. There are no hints. Selecting a cell highlights its row, column,
and box and every copy of its digit. Given digits are navy, correct entries
blue, and wrong entries red; selecting a wrong entry marks the peers it
conflicts with. Correct digits are final, and a digit's key disappears once all
nine copies are placed. Completing a row, column, or box sends a brief wave
across it from the last cell.

In notes mode the keys toggle pencil marks in empty cells, drawn in a
three-by-three grid in the cell; marks matching the selected digit are blue.
Placing a correct digit removes that digit's marks from its row, column, and
box. Each wrong digit adds a mistake and stays red until it is replaced,
erased, or undone. Undo restores every cell a move changed, including removed
marks, but never refunds a mistake; it reaches back up to 2,000 moves. The third
mistake ends the game with **Game Over**; solving it shows the time. Either
dialog waits for a running completion wave and offers **New Game** or
**Close** to review the board. Starting a new game with progress asks for
confirmation.

The clock runs only while the Activity is resumed with a window, the game is in
progress, and it is neither paused nor showing a dialog. Pausing hides the
digits, notes, and highlights behind a play button; tapping the board resumes.
The game, including its undo history, is saved atomically after every move and
when the Activity pauses, so it survives process death and recreation. A failed
save or an unreadable saved game appears once as an Android toast.

Portrait windows stack the controls under the board. Wider windows put them in
a side panel, with a three-by-three keypad when the panel is tall enough.
Short windows such as split screen shrink everything uniformly. The app
rotates in all four orientations, drawing edge to edge and keeping controls
out of the system bars and display cutouts, which it reads from the window's
insets. Keyboards and D-pads also work: digits enter, arrows move the
selection and wrap at the edges, Backspace or 0 erases, N toggles notes, U or
Ctrl+Z undoes, P or Space pauses and resumes, Enter confirms dialogs or resumes
a paused game, and Back closes dialogs. The manifest opts
out of predictive back so Back still arrives as a key event.

## Puzzles

A background thread generates each puzzle and keeps the next one ready. It
fills a random grid, then removes clues in random order while the solution
stays unique, leaving a minimal puzzle with about 24 clues. A human-style
solver rates it, always applying the easiest technique that progresses:
singles, locked candidates, naked and hidden subsets up to quads, X-wings,
swordfish, jellyfish, XY- and XYZ-wings, and alternating inference chains of
up to 12 links over bivalue cells and bilocal units. A puzzle is Extreme when
the solver finishes it and needs at least four chain deductions, about one in
ten minimal puzzles. Puzzles the solver cannot finish are rejected, so every
game can be solved without guessing. The board shows "Generating puzzle..."
while it waits for one. Generation takes
about 20 ms on this phone's performance cores and 90 ms on its efficiency
cores.

## Rendering

The app reuses `//common/gpu`'s Vulkan 1.4 renderer in overlay mode, which
draws 4x multisampled rectangles, triangles, lines, and text straight to the
swapchain, without the scene, shadow, particle, or post-processing passes.
Linking only `create_overlay_renderer` lets the linker drop the scene
pipelines, their shaders, and the mesh code. Icons are drawn from these
primitives. Glyphs are rasterized at 128 px and mipmapped, so the large board
digits stay sharp. Cells and grid lines are snapped to whole pixels.

Everything except puzzle generation runs on the Activity's main thread.
Choreographer paces frames,
which are drawn only when something changes: input, the clock's seconds,
animation, or the window. A 100 ms timer checks the surface for rotations and
resizes that arrive without a callback while the Activity is visible.
Window-redraw callbacks draw and wait for the frame before returning, and the
surface is released before window destruction returns. A renderer error
appears as a toast and closes the Activity.

## Build

After the shared [toolchain setup](../README.md#toolchain-setup), run from
the workspace root:

```sh
bazel build //sudoku
```

Output: `bazel-bin/sudoku/sudoku.apk`, about 66 KB. The application ID is
`dev.demo.sudoku`; minimum and target Android API are 36, and the manifest
requires Vulkan 1.4. The native
library links the exception-free libc++ runtime from `//common:support` and uses
the shared flags from `.bazelrc`: link-time optimization, no unwind tables, full
stripping, and 16 KiB segment alignment. It exports only
`ANativeActivity_onCreate`.

Run the tests on the phone:

```sh
bazel test //sudoku:puzzle_test //sudoku:game_test //sudoku:app_test
```

`puzzle_test` checks the solver, the rater's deductions against known
solutions, and that generated puzzles are unique, minimal, and Extreme.
`game_test` covers entries, mistakes, notes and their automatic removal, undo,
winning, losing, and the saved format, including corrupted, truncated, and
empty data. `app_test` checks six layouts, including landscape, tablet,
split-screen, and small windows, for overlaps, safe areas, pixel alignment, and
hit targets. It then plays through the real app on the GPU with offscreen
rendering: touch input and slop, notes, the clock and pause, dialogs, saving and
restoring, losing, and a pixel readback. Pass an existing directory to save PPM
screenshots of the portrait, paused, confirm, landscape, lost, and tablet
frames:

```sh
mkdir -p /tmp/sudoku-shots
bazel run //sudoku:app_test -- /tmp/sudoku-shots
```

Add `--config=vulkan_validation` to run `app_test` with Khronos API and
synchronization validation; it then fails on any validation error.

## Install on this phone

Copy the APK to Termux's home and open Android's package installer, as for
[Native Buttons](../native_buttons/README.md#install-on-this-phone):

```sh
mkdir -p /data/data/com.termux/files/home/sudoku
cp bazel-bin/sudoku/sudoku.apk /data/data/com.termux/files/home/sudoku/sudoku.apk
chmod 0400 /data/data/com.termux/files/usr/libexec/termux-am/am.apk
am start --user 0 -W -a android.intent.action.INSTALL_PACKAGE \
    -d content://com.termux.files/data/data/com.termux/files/home/sudoku/sudoku.apk \
    -t application/vnd.android.package-archive -f 0x10000001 \
    -p com.google.android.packageinstaller
```

Termux needs the same temporary `allow-external-apps = true` setting in
`/data/data/com.termux/files/home/.termux/termux.properties` as for Native
Buttons.
