# ded — Progress Log

A living document describing where the project started, what has been changed so far, and what is still left to do. See `README.md` for build instructions.

## 1. Starting point

`ded` ("Dramatic EDitor") is an unfinished single-file text editor in C11. It was already able to:

- **Open/save** a file: load via command line or the file browser, save with `F2`.
- **File browser** (`F3`): sorted file listing, directory navigation, path normalization, and a rainbow "epicness" shader.
- **Basic editing**: insert text, Backspace/Delete, Home/End (Ctrl = whole document, else line), word jumps (Ctrl+arrows), paragraph jumps (Ctrl+Up/Down), Tab inserting 4 spaces at the cursor.
- **Selection + clipboard**: Shift+arrows to select, Ctrl+A, Ctrl+C copy, Ctrl+V paste (system clipboard).
- **Incremental search**: Ctrl+F then type to search live; Enter/Esc to stop.
- **Syntax highlighting**: C/C++ lexer — keywords, strings, `//` comments, preprocessor directives, symbols, braces/parens/semicolons.
- **Rendering**: GPU glyph atlas (FreeType), blinking cursor, smooth eased camera, live shader reload on `F5`.
- **Cross-platform build**: `build.sh` (POSIX), `build_msvc.bat`, `build_msys2_mingw64.sh` (Windows), GitHub Actions CI (Linux gcc/clang, macOS).

Stack: SDL2 + OpenGL 3.3 + GLEW + FreeType, using hand-rolled data structures (dynamic arrays, string views, string builders, arena, glyph atlas, immediate-mode mesh renderer).

## 2. What we did (chronological)

### 2.1 Undo / redo — Ctrl+Z, Ctrl+Y / Ctrl+Shift+Z
`src/editor.h`, `src/editor.c`, `src/main.c`

- Every edit is recorded as an `Edit_Op` (`EDIT_INSERT` / `EDIT_DELETE` with position + affected text).
- Ops group into undo steps: rapid consecutive edits (< 500 ms) merge into one step; navigation/mode keys call `editor_flush_group` to seal the current step.
- Undo replays the group's inverse ops in reverse; redo replays forward. Cursor lands at the start of the change on undo, end on redo.
- A new edit clears the redo stack; history is freed when a new file is loaded.
- Added `Edit_History` (undo/redo stacks) and `Edit_Ops` (current group) to the `Editor` struct.

### 2.2 Selection delete, cut, type-over
`src/editor.h`, `src/editor.c`, `src/main.c`

- Backspace/Delete on a selection now deletes the whole selection and clears it.
- Typing or pasting over a selection replaces it (single undo step reverts the whole type-over).
- Ctrl+X = cut (copy then delete selection).
- Fixed an off-by-one in `editor_clipboard_copy` (`end - begin + 1`) that copied one char past the selection and could read out of bounds.

### 2.3 Indent / unindent — Tab, Shift+Tab
`src/editor.h`, `src/editor.c`, `src/main.c`

- Tab with a selection indents every selected line (selection preserved and follows the text).
- Shift+Tab with a selection dedents each selected line (removes up to 4 leading spaces).
- Shift+Tab without a selection dedents the current line; Tab without a selection still inserts 4 spaces at the cursor.
- Generalised `editor_data_insert` into `editor_data_insert_at(pos)` so text can be inserted at any position.
- Under the hood: lines are mutated bottom-up so offsets stay valid, then the selection positions are remapped using per-line deltas (`editor_apply_deltas`).
- Undo/redo now clear the selection (positions would otherwise be stale).

### 2.4 Vim modal editing (Steps 1-3)
`src/vim.c`, `src/vim.h` (new), `src/editor.c`, `src/editor.h`, `src/main.c`, `src/tes_motion.c` (new, headless test)

- **Step 1 — pure motions**: refactored the existing cursor-movement mutators (`editor_move_line_up`, `editor_move_word_left`, ...) into pure `editor_find_*(e, pos) -> pos` functions plus a thin mutator wrapper around each. Zero behavior change (`tes_motion.c` cross-checks every mutator against its pure function) — this is the shared core Vim's motions, and any future operator/visual-mode range, build on.
- **Step 2 — Normal/Insert dispatch**: `vim.c` owns a `Vim_Mode` + `Vim_State`, dispatched from `main.c` via `vim_handle_key()` before any other key handling. `editor.c` has no idea Vim exists — it only grew a `cursor_block` rendering hint. Normal-mode motions: `hjkl`, `w`/`b`/`e` (approximate word model — see the comment on `editor_find_word_end`), `0`/`$`, `gg`/`G`. `i` enters Insert; Escape returns to Normal (and steps the cursor back one column, matching Vim convention).
- **Step 3 — operators**: `d`/`c`/`y` composed with a motion (`dw`, `d$`, `cw`, ...) or doubled with themselves (`dd`/`cc`/`yy`, linewise), plus `x`, `D`, `C`, `Y`, `p`/`P`. These do **not** introduce a separate yank register — they drive the editor's existing selection + system-clipboard primitives (`editor_clipboard_copy/cut/paste`), the same ones Shift+arrow selection and Ctrl+C/X/V already use. `cc` on a line clears its content but keeps the line itself (and the buffer's line count) — unlike `dd`, which removes the line's newline along with it; this distinction is what a regression test in `tes_motion.c` locks in after being caught by it.
- Out of scope so far (tracked in `vim.h`'s header comment): counts (`3dw`), Visual mode, command-line mode (`:w`/`:q`), registers/macros/marks, text objects (`iw`, `ci(`, ...), operators composed with `j`/`k`/`gg`/`G` (`dj`, `dgg`, `dG` — these motions aren't expressible as the simple `(buffer, pos) -> pos` model `vim_resolve_motion` needs), and authentic linewise `p`/`P` (the clipboard is charwise-only, so both just insert at a cursor position).

### 2.5 Two Vim-integration bugs found and fixed
`src/vim.c`, `src/vim.h`, `src/editor.c`, `src/main.c`

- **`i` leaked into the buffer as text.** SDL fires `SDL_TEXTINPUT` right after `SDL_KEYDOWN` for every printable key — including ones Normal mode just consumed as a command. The `SDL_TEXTINPUT` handler swallowed these by checking "are we in Normal mode", but `i` flips the mode to Insert as part of handling the very keydown that TEXTINPUT event belongs to, so by the time it arrived the check no longer matched and the letter got typed. Fixed with `Vim_State::consumed_textinput`, set whenever `vim_handle_key` claims a key while *starting out* in Normal mode (captured before dispatch), and read via `vim_take_consumed_textinput()` in `main.c`'s `SDL_TEXTINPUT` handler.
- **Normal-mode cursor was a thicker bar, not a block.** `cursor_block` only changed a hardcoded width (5px → 12px); it never actually filled the glyph cell. Fixed by sizing the block to the real glyph advance (`atlas->metrics[ch].ax`) of the character under the cursor, falling back to the space glyph's width past end-of-buffer/line or on control characters.
- Incidental bug caught by the second fix's review: `vim_handle_key` was running unconditionally even while `e->searching` — meaning typing an incremental-search query while in Normal mode (the default mode) got intercepted as Vim commands instead of reaching the search box. Fixed by having Vim defer entirely to the editor's search handling while `e->searching` is true.

### 2.6 Unsaved-changes prompt, save-as, and new-file
`src/editor.c`, `src/editor.h`, `src/main.c`

- `Editor` grew a `dirty` flag, set in the one place every content mutation already funnels through (`editor_add_op`) plus `editor_undo`/`editor_redo` (which bypass that funnel by replaying recorded ops directly). Cleared on a successful save/save-as/load/new-file.
- `main.c` replaced its `file_browser` bool with an `App_Mode` enum (`APP_MODE_EDITOR` / `_FILE_BROWSER` / `_SAVE_AS` / `_CONFIRM`) — mutually exclusive by construction, unlike stacking more bools would have been.
- **F3** (open file browser), **Ctrl+N** (new file), and closing the window / **Ctrl+Q** (quit) all go through `request_action()`: if `editor.dirty`, it parks the request behind an `APP_MODE_CONFIRM` y/n prompt instead of running it immediately. Only explicit `y` confirms — Enter is deliberately *not* an alias for it, so an accidental Enter while the prompt is up (easy to hit out of habit) falls back to the safe (cancel) option like `n`/Escape, not the destructive one.
- **F2** now prompts for a path (`APP_MODE_SAVE_AS`) instead of erroring when the buffer has none yet; **Ctrl+Shift+S** always opens that prompt (pre-filled with the current path, if any) as an explicit "Save As".
- **`draw_bottom_bar`**: the renderer has no screen-space drawing mode of its own — every vertex goes through the same camera transform as the document text (`shaders/simple.vert`). Used for the confirm prompt, the save-as path entry, and now also `flash_error` messages, which used to go to stderr only.

### 2.7 Two bugs found in the above and fixed
`src/main.c`

- **The bottom bar could overflow past the window.** It originally rode the document's own live `sr->camera_pos`/`camera_scale`, so it zoomed with the document — and the document's auto-zoom goes extreme on a near-empty buffer (the target scale divides by the longest visible line's length), blowing the bar up well past the window edges. Fixed by giving `draw_bottom_bar` its own fixed camera transform (`UI_TEXT_SCALE`), saved and restored around the draw call, so the bar is now always exactly `UI_BAR_HEIGHT_PX` screen pixels tall regardless of window size or the document's zoom.
- **Confirming a prompt (e.g. `y` for "new file, discard changes?") could leak that letter into the buffer.** Same root cause as 2.5's `i` bug, in a new spot: pressing `y` exits `APP_MODE_CONFIRM` back to `APP_MODE_EDITOR` within the same keystroke, so the trailing `SDL_TEXTINPUT('y')` arrives already in editor mode — and if Vim was still in Insert mode underneath (e.g. the user hadn't pressed Escape before triggering Ctrl+N), it got typed into the freshly-created buffer. Generalized the same "did this key just get consumed by a mode I've since left" pattern one level up: `app_mode_changed_this_key` is set whenever an `SDL_KEYDOWN` changes `mode`, and `SDL_TEXTINPUT` swallows unconditionally when it's set, regardless of which mode was switched *into*. This subsumes the old `APP_MODE_CONFIRM`-specific no-op and fixes the symmetric case (`n`/Escape cancelling) too, not just `y`.

## 3. Notes / known quirks

- Undo/redo place the caret at the start (undo) or end (redo) of the change; exact caret restoration is not implemented.
- Grouping is time-based (500 ms): a long continuous typing burst without any movement key is still a single undo step.
- `SDL_GetTicks()` returns 0 until `SDL_INIT_TIMER` is initialized — headless tests must init the timer for the merge window to behave realistically.
- `editor_clipboard_copy/cut/paste` go through SDL's system clipboard (`SDL_Set/GetClipboardText`), which needs the video subsystem initialized to work reliably — `tes_motion.c` runs with no `SDL_Init` at all, so its yank/paste round-trip check is skipped (not failed) if the clipboard isn't actually functional in that environment. Delete and change are verified independently of whether the copy half succeeds.

## 4. What is left (roadmap, in rough priority order)

1. ~~**Unsaved-changes prompt**~~ / ~~**Save-as / new-file dialog**~~ — done, see §2.6.
2. **Line numbers + status bar** — line:col, filename, dirty indicator (a permanent one — §2.6's bottom bar only appears for prompts/errors, not standing status), adjustable horizontal scroll.
3. **Search improvements** — find next/prev, highlight all matches, replace / replace-all.
4. **File-browser search** — type to filter the file list (TODO in `src/main.c`).
5. **UTF-8 support** — the big correctness gap: the glyph atlas is ASCII-only (128 glyphs), and editing is byte-wise, so word jumps, Backspace, and highlighting all break on non-ASCII text.
6. **Editing polish** — delete word, replace.
7. **Multiple buffers / tabs**, split view, project-wide search, git integration.
8. **Mouse support** — click to place cursor, wheel scroll, drag to select.
9. **Config** — theme, font choice (currently hardcoded Iosevka), tab width, keybindings.

### 4.1 Vim modal editing — what's left (see `src/vim.h`'s header comment for the authoritative scope notes)

1. **Counts** — `3dw`, `5j`, `2yy`. Currently every command acts on exactly one motion/line.
2. **Visual mode** — select-by-motion (`v`, `V`), then apply an operator to the selection instead of a fresh motion.
3. **Text objects** — `iw`, `aw`, `ci(`, `di"`, etc. Needs a notion of "object under cursor", not just "motion from cursor".
4. **Linewise motions composed with operators** — `dj`, `dk`, `dgg`, `dG`. Blocked on generalizing `vim_resolve_motion` (currently only single `(buffer, pos) -> pos` motions) to express linewise ranges.
5. **Registers** — named (`"ay`) and numbered, instead of always going through the one system clipboard.
6. **Authentic linewise put** — `p`/`P` after a linewise yank should insert a new line below/above, not paste charwise at the cursor.
7. **Command-line mode** — `:w`, `:q`, `:%s/.../.../`.
8. **Macros and marks** — `qa...q`, `@a`, `` ` ``/`'`.
9. **A real Vim word model** — `w`/`b`/`e` currently use this codebase's existing alnum-vs-everything-else split; real Vim distinguishes punctuation runs from word runs too.
10. **Sticky column for `j`/`k`** — see the "known quirks" note above; matters more once Vim's vertical motions are counted/composed.

## 5. TODOs still in the code

From `src/main.c` and elsewhere:

- Font path is hardcoded; no user customization.
- Search does not auto-fill from the current selection.
- File browser has no incremental search.
- `fb->dir_path` grows indefinitely if you navigate up at the root (`src/file_browser.c`).
- No trailing-whitespace stripping / auto-save.
- `type_of_file()` is unimplemented on Windows (`#error`), and the MSVC CI job is disabled (#29).