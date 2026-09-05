#include "./vim.h"

Vim_State vim_state_init(void)
{
    return (Vim_State) {
        .mode = VIM_MODE_NORMAL,
        .pending_g = false,
        .pending_op = VIM_OP_NONE,
        .consumed_textinput = false,
    };
}

bool vim_take_consumed_textinput(Vim_State *vs)
{
    bool result = vs->consumed_textinput;
    vs->consumed_textinput = false;
    return result;
}

static void vim_enter_insert(Vim_State *vs, Editor *e)
{
    vs->mode = VIM_MODE_INSERT;
    e->cursor_block = false;
}

static void vim_enter_normal(Vim_State *vs, Editor *e)
{
    vs->mode = VIM_MODE_NORMAL;
    vs->pending_g = false;
    vs->pending_op = VIM_OP_NONE;
    e->cursor_block = true;

    // Vim convention: leaving Insert mode steps the cursor back one
    // column (but never past the start of the line), so it lands ON the
    // last character you typed instead of one past it.
    size_t line_begin = editor_find_line_begin(e, e->cursor);
    if (e->cursor > line_begin) {
        e->cursor = editor_find_char_left(e, e->cursor);
    }
}

// ------------------------------------------------------------------------
// Range primitives
//
// Everything below drives the editor's existing selection + clipboard
// machinery instead of reimplementing delete/yank/put: set up a
// [begin, end) range as a selection, then call the same
// editor_clipboard_copy/cut/paste the mouse-selection and Ctrl+C/X/V
// paths already use.
// ------------------------------------------------------------------------

static void vim_select_range(Editor *e, size_t begin, size_t end)
{
    e->selection = true;
    e->select_begin = begin;
    e->cursor = end;
}

static void vim_delete_range(Editor *e, size_t begin, size_t end)
{
    vim_select_range(e, begin, end);
    editor_clipboard_cut(e); // copies then deletes; leaves e->cursor == begin
}

static void vim_yank_range(Editor *e, size_t begin, size_t end)
{
    vim_select_range(e, begin, end);
    editor_clipboard_copy(e);
    e->selection = false;
    e->cursor = begin; // Vim leaves the cursor at the start of what was yanked
}

// Range for 'dd'/'yy' on the line containing `pos`: the whole line
// INCLUDING its trailing newline, so deleting it removes the line
// entirely rather than leaving a blank one behind - except the buffer's
// very last line, which has no trailing newline of its own to take (it
// eats the *previous* line's newline instead), and the buffer's only
// line, which has no newline to take at all and is just cleared.
static void vim_linewise_range(const Editor *e, size_t pos, size_t *out_begin, size_t *out_end)
{
    size_t row = editor_row_at(e, pos);
    Line line = e->lines.items[row];
    if (row + 1 < e->lines.count) {
        *out_begin = line.begin;
        *out_end = e->lines.items[row + 1].begin;
    } else if (row > 0) {
        *out_begin = e->lines.items[row - 1].end;
        *out_end = line.end;
    } else {
        *out_begin = line.begin;
        *out_end = line.end;
    }
}

// Range for 'cc' on the line containing `pos`: just the line's content,
// EXCLUDING any newline - unlike 'dd', changing a line must leave it in
// place (and the buffer's line count unchanged) so the cursor lands on a
// still-existing blank line ready for new text, rather than merging it
// into its neighbor the way consuming a newline would.
static void vim_line_content_range(const Editor *e, size_t pos, size_t *out_begin, size_t *out_end)
{
    Line line = e->lines.items[editor_row_at(e, pos)];
    *out_begin = line.begin;
    *out_end = line.end;
}

// Maps a Normal-mode motion key to the position it moves to, for reuse by
// both plain cursor movement and operator-pending range resolution below
// - one mapping, two consumers, so a motion added here works in both
// immediately. `shift` matters only for '0' (never) and '$' (always);
// h/l/w/b/e ignore it, matching this file's pre-existing plain-movement
// behavior. `*out_inclusive` says whether the char the motion lands ON is
// itself part of an operator's range (true for `e`/`$`, matching Vim's
// inclusive-motion rules; plain movement ignores it).
//
// Scope: only motions expressible as a single (buffer, pos) -> pos call
// with no line-boundary bookkeeping. j/k/gg/G stay hand-rolled in
// vim_dispatch_key's switch and are movement-only for now - not usable
// with a pending operator (dj, dgg, dG), since a correct linewise range
// needs more than this simple model gives.
static bool vim_resolve_motion(const Editor *e, SDL_Keysym key, bool shift, size_t *out_pos, bool *out_inclusive)
{
    *out_inclusive = false;
    switch (key.sym) {
    case SDLK_h: *out_pos = editor_find_char_left(e, e->cursor);  return true;
    case SDLK_l: *out_pos = editor_find_char_right(e, e->cursor); return true;
    case SDLK_w: *out_pos = editor_find_word_right(e, e->cursor); return true;
    case SDLK_b: *out_pos = editor_find_word_left(e, e->cursor);  return true;

    case SDLK_e:
        *out_pos = editor_find_word_end(e, e->cursor);
        *out_inclusive = true;
        return true;

    case SDLK_0:
        if (shift) return false;
        *out_pos = editor_find_line_begin(e, e->cursor);
        return true;

    case SDLK_4: // Shift+4 == '$' on a US layout.
        if (!shift) return false;
        *out_pos = editor_find_line_end(e, e->cursor);
        *out_inclusive = true;
        return true;

    default:
        return false;
    }
}

// 'x': delete the single character under the cursor (never the line's
// own newline - Vim's `x` on an empty, or already-at-end, line is a
// no-op).
static void vim_delete_char_under_cursor(Editor *e)
{
    size_t line_end = editor_find_line_end(e, e->cursor);
    if (e->cursor >= line_end) return;
    vim_delete_range(e, e->cursor, editor_find_char_right(e, e->cursor));
}

// 'D': delete from the cursor to the end of the current line.
static void vim_delete_to_line_end(Editor *e)
{
    size_t end = editor_find_line_end(e, e->cursor);
    if (e->cursor < end) vim_delete_range(e, e->cursor, end);
}

// 'C': same range as 'D', then drop into Insert mode right where the
// deleted text was - Vim's "change" convention.
static void vim_change_to_line_end(Vim_State *vs, Editor *e)
{
    vim_delete_to_line_end(e);
    vim_enter_insert(vs, e);
}

// 'Y': yank the whole current line (Vim treats capital Y as an alias for
// `yy`, not "yank to end of line" the way D/C work for delete/change).
static void vim_yank_line(Editor *e)
{
    size_t begin, end;
    vim_linewise_range(e, e->cursor, &begin, &end);
    vim_yank_range(e, begin, end);
}

// 'p' pastes after the cursor (Vim's charwise put), 'P' before it. The
// clipboard here is charwise-only (no linewise register flag), so both
// just insert the system clipboard's text at a cursor position rather
// than reproducing Vim's linewise put-a-whole-line-below/above behavior.
static void vim_paste(Editor *e, bool after)
{
    if (after && e->cursor < e->data.count) {
        e->cursor = editor_find_char_right(e, e->cursor);
    }
    editor_clipboard_paste(e);
}

// Finishes whatever operator is pending against [begin, end) (order does
// not matter - callers may pass either endpoint first). Delete/change
// treat an empty range as "nothing to delete" rather than skipping the
// operator entirely, since `cc`/`cw` on an empty line must still drop
// into Insert mode with nothing to remove first.
static void vim_apply_pending_op(Vim_State *vs, Editor *e, size_t begin, size_t end)
{
    if (begin > end) SWAP(size_t, begin, end);
    Vim_Op op = vs->pending_op;
    vs->pending_op = VIM_OP_NONE;

    switch (op) {
    case VIM_OP_DELETE:
        if (begin < end) vim_delete_range(e, begin, end);
        else e->cursor = begin;
        break;

    case VIM_OP_YANK:
        if (begin < end) vim_yank_range(e, begin, end);
        break;

    case VIM_OP_CHANGE:
        if (begin < end) vim_delete_range(e, begin, end);
        else e->cursor = begin;
        vim_enter_insert(vs, e);
        break;

    case VIM_OP_NONE:
        break;
    }
}

// Resolves the second key of a pending d/c/y: either the operator's own
// letter again (linewise dd/cc/yy), a recognized motion (charwise range),
// or - Escape aside - anything else, which cancels the operator the way
// Vim silently drops an unrecognized operator-pending key. Always called
// with vs->pending_op != VIM_OP_NONE.
static bool vim_dispatch_pending_op(Vim_State *vs, Editor *e, SDL_Keysym key, bool shift)
{
    bool is_same_letter =
        (vs->pending_op == VIM_OP_DELETE && key.sym == SDLK_d && !shift) ||
        (vs->pending_op == VIM_OP_CHANGE && key.sym == SDLK_c && !shift) ||
        (vs->pending_op == VIM_OP_YANK   && key.sym == SDLK_y && !shift);
    if (is_same_letter) {
        size_t begin, end;
        if (vs->pending_op == VIM_OP_CHANGE) {
            vim_line_content_range(e, e->cursor, &begin, &end); // 'cc': keep the line, clear its content
        } else {
            vim_linewise_range(e, e->cursor, &begin, &end); // 'dd'/'yy': whole line + newline
        }
        vim_apply_pending_op(vs, e, begin, end);
        return true;
    }

    size_t target;
    bool inclusive;
    if (vim_resolve_motion(e, key, shift, &target, &inclusive)) {
        size_t begin = e->cursor, end = target;
        if (begin > end) SWAP(size_t, begin, end);
        if (inclusive && end < e->data.count) end += 1;
        vim_apply_pending_op(vs, e, begin, end);
        return true;
    }

    if (key.sym == SDLK_ESCAPE) {
        vs->pending_op = VIM_OP_NONE;
        // Don't consume: let main.c's existing Escape handling still run
        // too (flush undo group, clear selection), same as Insert mode's
        // Escape handling above.
        return false;
    }

    // Any other key: cancel the pending operator without doing anything
    // else with it this keypress, same as Vim waiting-then-dropping an
    // unbound operator-pending key.
    vs->pending_op = VIM_OP_NONE;
    return true;
}

// Actual key dispatch, split out of vim_handle_key so the public entry
// point can uniformly record consumed_textinput around every return
// path below (including the ones that switch mode) without repeating
// that bookkeeping at each `return`.
static bool vim_dispatch_key(Vim_State *vs, Editor *e, SDL_Keysym key)
{
    // Vim has no notion of this editor's incremental search - defer to
    // main.c's existing (already search-aware) key/text handling
    // entirely while it's active, rather than having Normal-mode
    // commands like hjkl hijack the search query as it's being typed.
    if (e->searching) return false;

    if (vs->mode == VIM_MODE_INSERT) {
        if (key.sym == SDLK_ESCAPE) {
            vim_enter_normal(vs, e);
            // Don't consume: main.c's existing Escape handling still
            // flushes the undo group, stops search, and clears selection.
        }
        return false;
    }

    // vs->mode == VIM_MODE_NORMAL from here down.
    bool shift = key.mod & KMOD_SHIFT;
    bool ctrl  = key.mod & KMOD_CTRL;

    // A pending `g` completes only into `gg`. Any other key cancels the
    // pending state and is then handled normally below - so `g` followed
    // by, say, `j` still moves down instead of silently vanishing.
    if (vs->pending_g) {
        vs->pending_g = false;
        if (key.sym == SDLK_g && !shift && !ctrl) {
            editor_move_to_begin(e); // gg -> start of buffer (approximates "first line")
            return true;
        }
        // fall through: process `key` as an ordinary Normal-mode command
    }

    // Never hijack Ctrl-combinations - undo/redo/copy/cut/paste/select-all
    // /search all go through main.c's existing mode-agnostic handling.
    // Also drop any pending operator: Vim abandons d/c/y the same way on
    // an unrelated Ctrl-combo.
    if (ctrl) {
        vs->pending_op = VIM_OP_NONE;
        return false;
    }

    if (vs->pending_op != VIM_OP_NONE) {
        return vim_dispatch_pending_op(vs, e, key, shift);
    }

    switch (key.sym) {
    case SDLK_i:
        if (!shift) {
            vim_enter_insert(vs, e);
            return true;
        }
        break;

    case SDLK_h:
    case SDLK_l:
    case SDLK_w:
    case SDLK_b:
    case SDLK_e:
    case SDLK_0:
    case SDLK_4: {
        size_t target;
        bool inclusive;
        if (vim_resolve_motion(e, key, shift, &target, &inclusive)) {
            (void) inclusive; // only meaningful to operator-pending ranges
            e->cursor = target;
            return true;
        }
        break; // e.g. '0' with shift, or '4' without shift: not a motion here
    }

    case SDLK_k: editor_move_line_up(e);    return true;
    case SDLK_j: editor_move_line_down(e);  return true;

    case SDLK_g:
        if (shift) {
            editor_move_to_end(e); // G -> end of buffer (approximates "last line")
        } else {
            vs->pending_g = true;  // wait for the second g of `gg`
        }
        return true;

    case SDLK_x:
        if (!shift) {
            vim_delete_char_under_cursor(e);
            return true;
        }
        break; // 'X' (delete before cursor) not implemented yet

    case SDLK_d:
        if (shift) {
            vim_delete_to_line_end(e); // 'D'
        } else {
            vs->pending_op = VIM_OP_DELETE; // 'd': wait for its motion/repeat
        }
        return true;

    case SDLK_c:
        if (shift) {
            vim_change_to_line_end(vs, e); // 'C'
        } else {
            vs->pending_op = VIM_OP_CHANGE; // 'c': wait for its motion/repeat
        }
        return true;

    case SDLK_y:
        if (shift) {
            vim_yank_line(e); // 'Y' == yy
        } else {
            vs->pending_op = VIM_OP_YANK; // 'y': wait for its motion/repeat
        }
        return true;

    case SDLK_p:
        vim_paste(e, !shift); // 'p' after cursor, 'P' before
        return true;

    default:
        break;
    }

    return false;
}

bool vim_handle_key(Vim_State *vs, Editor *e, SDL_Keysym key)
{
    // Every printable key SDL delivers as SDL_KEYDOWN is followed by an
    // SDL_TEXTINPUT event carrying the same character, even when Normal
    // mode just consumed it as a command (e.g. 'i' entering Insert mode,
    // or plain hjkl movement). Record whether *this* call claimed a key
    // while starting out in Normal mode, so the caller can swallow that
    // trailing SDL_TEXTINPUT instead of typing the command letter into
    // the buffer. Must be captured before dispatch, since a command like
    // 'i' changes vs->mode as part of handling the key.
    bool was_insert = (vs->mode == VIM_MODE_INSERT);
    bool handled = vim_dispatch_key(vs, e, key);
    if (!was_insert && handled) {
        vs->consumed_textinput = true;
    }
    return handled;
}
