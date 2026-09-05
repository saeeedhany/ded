#ifndef VIM_H_
#define VIM_H_

#include <stdbool.h>
#include <SDL2/SDL.h>
#include "./editor.h"

// Step 3 of the modal-editing work: operators (d/c/y) composed with the
// Step 1 motions (editor_find_*), plus x/D/C/Y/p/P, on top of the
// Step 2 Normal/Insert dispatch. This module owns all Vim-specific state
// and knows nothing about SDL's event loop shape or window/rendering -
// `editor.c` stays a plain text engine, unaware that Vim exists. It also
// has no register of its own: yank/delete/put are implemented by driving
// the editor's existing selection + system-clipboard primitives
// (editor_clipboard_copy/cut/paste), the same ones Shift+arrow selection
// and Ctrl+C/X/V already use - one delete/yank/put implementation, not a
// second one bolted on beside it.
//
// Deliberately out of scope for this step (see PROGRESS.md roadmap):
//   - counts ("3dw", "5j")
//   - Visual mode
//   - Command-line mode (:w, :q)
//   - registers (named or numbered), macros, marks
//   - text objects (iw, aw, i(, ...)
//   - operators composed with linewise/multi-key motions: dj, dk, dgg, dG
//     (vim_resolve_motion below only covers motions expressible as a
//     single (buffer, pos) -> pos call with no line-boundary bookkeeping;
//     j/k/gg/G stay movement-only until that's generalized)
//   - authentic linewise p/P (pasting a yanked whole line below/above the
//     current one) - the clipboard is charwise-only here, so p/P always
//     insert at a cursor position instead
// Editing keys (Backspace/Delete/Enter/Tab/clipboard) still intentionally
// keep working in BOTH modes for now, same as Step 2.

typedef enum {
    VIM_MODE_NORMAL,
    VIM_MODE_INSERT,
} Vim_Mode;

// Which operator (if any) is waiting for a motion (or its own letter
// again, for the linewise dd/cc/yy form) to complete it. See
// Vim_State::pending_op.
typedef enum {
    VIM_OP_NONE,
    VIM_OP_DELETE, // d
    VIM_OP_CHANGE, // c
    VIM_OP_YANK,   // y
} Vim_Op;

typedef struct {
    Vim_Mode mode;

    // True immediately after a lone 'g' in Normal mode, while we wait to
    // see whether the next key completes `gg`. Any key other than a
    // second 'g' cancels this and is then processed normally - so `g`
    // followed by `j`, `Escape`, etc. never eats the second keypress.
    bool pending_g;

    // VIM_OP_NONE unless a lone d/c/y was just pressed and we're waiting
    // for the motion (or repeated letter) that completes it into a range.
    // Any key that isn't a recognized motion and isn't that same letter
    // cancels the pending operator - see vim.c's vim_dispatch_pending_op.
    Vim_Op pending_op;

    // Set by vim_handle_key whenever it claims a key while starting out
    // in Normal mode - including keys like 'i' that switch to Insert as
    // part of handling it. SDL fires SDL_TEXTINPUT for every printable
    // key right after its SDL_KEYDOWN, even ones vim_handle_key just
    // consumed, so the caller's SDL_TEXTINPUT handler must check this
    // (and clear it) to swallow that trailing text event - checking
    // "are we in Normal mode" there is not enough, since a mode-switching
    // key like 'i' has already flipped the mode by the time it arrives.
    bool consumed_textinput;
} Vim_State;

// Call once at startup. Vim conventionally starts in Normal mode.
Vim_State vim_state_init(void);

// Should this frame's pending SDL_TEXTINPUT event be swallowed instead of
// inserted as text? True exactly once after a Normal-mode key was
// claimed by vim_handle_key; calling this clears it. See
// Vim_State::consumed_textinput for why this can't just be inferred from
// the current mode.
bool vim_take_consumed_textinput(Vim_State *vs);

// Call for every SDL_KEYDOWN, in every mode, before any other key
// handling. Returns true if this key was fully handled by the modal
// layer and the caller should do nothing else with it this frame
// (movement commands, mode switches). Returns false if the caller's
// existing key handling should still run - this is how mode-agnostic
// bindings (Ctrl+Z, F2 save, arrow keys, Backspace, ...) keep working
// unchanged: vim_handle_key only ever claims the specific keys it
// recognizes as Normal-mode commands.
//
// Also updates e->cursor_block so rendering reflects the current mode.
bool vim_handle_key(Vim_State *vs, Editor *e, SDL_Keysym key);

#endif // VIM_H_
