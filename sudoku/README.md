# sudoku

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

In notes mode the keys toggle pencil marks, drawn in a three-by-three grid in
the cell; marks matching the selected digit are blue. Placing a correct digit
removes that digit's marks from its row, column, and box. Wrong digits count
as mistakes until erased or undone; undo restores every cell a move changed,
including removed marks, but never refunds a mistake. The third mistake ends
the game with **Game Over**; solving it shows the time. Both offer
**New Game** or **Close** to review the board. Starting a new game with
progress asks for confirmation.

The clock runs only while the Activity is resumed, the game is visible, and it
is not paused. Pausing hides the digits behind a play button. The game,
including its undo history, is saved atomically after every move and when the
Activity pauses, so it survives process death and recreation.

Portrait windows stack the controls under the board. Wider windows put them in
a side panel, with a three-by-three keypad when the panel is tall enough.
Short windows such as split screen shrink everything uniformly. The app
rotates in all four orientations, drawing edge to edge and keeping controls
out of the system bars and display cutouts, which it reads from the window's
insets. Keyboards and D-pads also work: digits enter, arrows move the
selection, Backspace or 0 erases, N toggles notes, U or Ctrl+Z undoes, P or
Space pauses, Enter confirms dialogs, and Back closes them.

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
game can be solved without guessing. Generation takes
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

Everything runs on the Activity's main thread. Choreographer paces frames,
which are drawn only when something changes: input, the clock's seconds,
animation, or the window. A 100 ms timer checks the surface for rotations and
resizes that arrive without a callback while the Activity is visible.
Window-redraw callbacks draw and wait for the frame before returning, and the
surface is released before window destruction returns.

## Build

After the shared [toolchain setup](../README.md#toolchain-setup), run from
the workspace root:

```sh
bazel build //sudoku
```

Output: `bazel-bin/sudoku/sudoku.apk`, about 66 KB. The application ID is
`dev.demo.sudoku`; minimum Android API is 26 and target API is 35. The native
library uses the same flags as `native_buttons`: the exception-free libc++
runtime from `//common:support`, link-time optimization, no unwind tables, full
stripping, and 16 KiB segment alignment. It exports only
`ANativeActivity_onCreate`.

Run the tests on the phone:

```sh
bazel test //sudoku:puzzle_test //sudoku:game_test //sudoku:app_test \
    --platforms=//:arm64-v8a --run_under=//tools:android_test_runner
```

`puzzle_test` checks the solver, the rater's deductions against known
solutions, and that generated puzzles are unique, minimal, and Extreme.
`game_test` covers entries, mistakes, notes and their automatic removal, undo,
winning, losing, and the saved format, including corrupted files. `app_test`
checks portrait, landscape, tablet, split-screen, and small layouts for
overlaps and hit targets, then plays through the real app on the GPU with
offscreen rendering: touch input, the clock and pause, dialogs, saving and
restoring, and losing. Pass a directory to save PPM screenshots:

```sh
bazel run //sudoku:app_test --platforms=//:arm64-v8a \
    --run_under=//tools:android_test_runner -- /tmp/sudoku-shots
```

Add `--define=vulkan_validation=true --run_under=//tools:vulkan_validation_runner`
to run `app_test` with Khronos validation.

## Install on this phone

Copy the APK to Termux's home and open Android's package installer, as for
[native_buttons](../native_buttons/README.md#install-on-this-phone):

```sh
mkdir -p /data/data/com.termux/files/home/sudoku
cp bazel-bin/sudoku/sudoku.apk /data/data/com.termux/files/home/sudoku/sudoku.apk
am start --user 0 -W -a android.intent.action.INSTALL_PACKAGE \
    -d content://com.termux.files/data/data/com.termux/files/home/sudoku/sudoku.apk \
    -t application/vnd.android.package-archive -f 0x10000001 \
    -p com.google.android.packageinstaller
```
