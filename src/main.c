#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <assert.h>

#include <SDL2/SDL.h>
#define GLEW_STATIC
#include <GL/glew.h>
#define GL_GLEXT_PROTOTYPES
#include <SDL2/SDL_opengl.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include "./editor.h"
#include "./file_browser.h"
#include "./la.h"
#include "./free_glyph.h"
#include "./simple_renderer.h"
#include "./common.h"
#include "./lexer.h"
#include "./sv.h"
#include "./vim.h"
#include "./command.h"
#include "./config.h"

// TODO: Delete a word

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

void MessageCallback(GLenum source,
                     GLenum type,
                     GLuint id,
                     GLenum severity,
                     GLsizei length,
                     const GLchar* message,
                     const void* userParam)
{
    (void) source;
    (void) id;
    (void) length;
    (void) userParam;
    fprintf(stderr, "GL CALLBACK: %s type = 0x%x, severity = 0x%x, message = %s\n",
            (type == GL_DEBUG_TYPE_ERROR ? "** GL ERROR **" : ""),
            type, severity, message);
}

static Free_Glyph_Atlas atlas = {0};
static Simple_Renderer sr = {0};
static Editor editor = {0};
static File_Browser fb = {0};
static Vim_State vim = {0};

// Which full-screen view is active. Mutually exclusive by construction
// (unlike the plain bools this replaced) - exactly one of these is ever
// "current" at a time, so there's no way to e.g. have both the file
// browser and a save-as prompt think they own the keyboard at once.
typedef enum {
    APP_MODE_EDITOR,
    APP_MODE_FILE_BROWSER,
    APP_MODE_SAVE_AS, // typing a destination path for F2/Ctrl+Shift+S
    APP_MODE_CONFIRM, // "you have unsaved changes, proceed anyway? (y/n)"
} App_Mode;

// What to do once APP_MODE_CONFIRM's y/n is answered. Set by
// request_action() alongside the mode switch, read back by
// perform_action() when the user confirms.
typedef enum {
    CONFIRM_ACTION_OPEN_BROWSER, // F3
    CONFIRM_ACTION_NEW_FILE,     // Ctrl+N
    CONFIRM_ACTION_QUIT,         // Ctrl+Q / closing the window
} Confirm_Action;

static App_Mode mode = APP_MODE_EDITOR;
static Confirm_Action pending_action = CONFIRM_ACTION_QUIT;
static bool quit = false;

// True for exactly one SDL_TEXTINPUT: the one immediately following a
// SDL_KEYDOWN that changed `mode` (e.g. 'y' confirming a prompt, which
// jumps straight back to APP_MODE_EDITOR). SDL fires that trailing
// SDL_TEXTINPUT no matter what - it belongs to whatever mode the
// keydown was handled in, not the new mode we just switched into, so it
// must never be typed as document text (or a save-as path, etc.)
// regardless of which mode's own SDL_TEXTINPUT rules would otherwise
// apply. This is what let a confirm keypress like 'y' leak into a
// freshly-created buffer when Vim was still in Insert mode underneath.
static bool app_mode_changed_this_key = false;

#define SAVE_AS_PATH_CAP 1024
static char save_as_path[SAVE_AS_PATH_CAP] = {0};
static size_t save_as_path_len = 0;

#define ERROR_DISPLAY_MS 4000
static char error_message[256] = {0};
static Uint32 error_message_time = 0;

// Errors used to go to stderr only, invisible once the app has its own
// window - now they're also kept around to flash in a bottom bar for a
// few seconds the next time the editor view renders (see draw_bottom_bar
// and its call site).
static void flash_error(const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");

    va_start(args, fmt);
    vsnprintf(error_message, sizeof(error_message), fmt, args);
    va_end(args);
    error_message_time = SDL_GetTicks();
}

// Actually carries out a Confirm_Action, either because there was
// nothing to lose (request_action skipped the prompt) or because the
// user just answered 'y' to it.
static void perform_action(Confirm_Action action)
{
    editor_flush_group(&editor);
    switch (action) {
    case CONFIRM_ACTION_OPEN_BROWSER:
        mode = APP_MODE_FILE_BROWSER;
        break;
    case CONFIRM_ACTION_NEW_FILE:
        editor_new_file(&editor);
        vim = vim_state_init();
        editor.cursor_block = true;
        mode = APP_MODE_EDITOR;
        break;
    case CONFIRM_ACTION_QUIT:
        quit = true;
        break;
    }
}

// Entry point for anything that would discard unsaved work (opening the
// file browser, starting a new file, quitting): runs immediately if the
// buffer isn't dirty, otherwise parks the request behind an APP_MODE_CONFIRM
// y/n prompt instead.
static void request_action(Confirm_Action action)
{
    if (editor.dirty) {
        pending_action = action;
        mode = APP_MODE_CONFIRM;
    } else {
        perform_action(action);
    }
}

static const char *confirm_message(Confirm_Action action)
{
    switch (action) {
    case CONFIRM_ACTION_OPEN_BROWSER:
        return "Unsaved changes! Discard and open the file browser? (y/n)";
    case CONFIRM_ACTION_NEW_FILE:
        return "Unsaved changes! Discard and start a new file? (y/n)";
    case CONFIRM_ACTION_QUIT:
        return "Unsaved changes! Quit without saving? (y/n)";
    }
    UNREACHABLE("confirm_message");
}

// Fixed zoom for screen-space UI overlays (the bottom bar), chosen
// independently of the document's own camera. FREE_GLYPH_FONT_SIZE (64)
// times this is the glyphs' actual on-screen pixel height.
#define UI_TEXT_SCALE 0.3125f
#define UI_BAR_HEIGHT_PX 36.0f
#define UI_TEXT_PADDING_PX 12.0f

// Draws a single-line bar pinned to the bottom edge of the window,
// always occupying exactly UI_BAR_HEIGHT_PX screen pixels regardless of
// window size OR the document's own pan/zoom. The renderer has no
// screen-space drawing mode of its own - every vertex, including this
// one, goes through the same camera_pos/camera_scale transform as the
// document text (see shaders/simple.vert) - so riding the document's
// live camera (as an earlier version of this function did) made the bar
// zoom with the document too, which could blow it up well past the
// window's edges whenever the document's own auto-zoom went extreme
// (e.g. an empty buffer zooms in a lot, since the target scale divides
// by the longest visible line's length). Fixed by using a *different*,
// fixed camera transform just for this draw call - saved and restored
// so the next document draw call this frame isn't affected.
static void draw_bottom_bar(Simple_Renderer *sr, Free_Glyph_Atlas *atlas, const char *text, Vec4f bg, Vec4f fg)
{
    Vec2f saved_camera_pos = sr->camera_pos;
    float saved_camera_scale = sr->camera_scale;

    sr->camera_scale = UI_TEXT_SCALE;
    float half_w = sr->resolution.x / (2.0f * UI_TEXT_SCALE);
    float half_h = sr->resolution.y / (2.0f * UI_TEXT_SCALE);
    sr->camera_pos = vec2f(half_w, half_h); // (0,0) now maps to the window's bottom-left corner

    float bar_h = UI_BAR_HEIGHT_PX / UI_TEXT_SCALE;
    Vec2f bar_pos = vec2f(0.0f, 0.0f);
    Vec2f bar_size = vec2f(2.0f * half_w, bar_h);

    simple_renderer_set_shader(sr, SHADER_FOR_COLOR);
    simple_renderer_solid_rect(sr, bar_pos, bar_size, bg);
    simple_renderer_flush(sr);

    simple_renderer_set_shader(sr, SHADER_FOR_TEXT);
    Vec2f text_pos = vec2f(UI_TEXT_PADDING_PX / UI_TEXT_SCALE, bar_h * 0.28f);
    free_glyph_atlas_render_line_sized(atlas, sr, text, strlen(text), &text_pos, fg);
    simple_renderer_flush(sr);

    sr->camera_pos = saved_camera_pos;
    sr->camera_scale = saved_camera_scale;
}

// ------------------------------------------------------------------------
// Commands
//
// Each of these is a registered Command (see command.h) - the thing a
// keybinding (built-in, or a config `bind` line) actually resolves to
// and runs. They're plain zero-argument functions reading/writing this
// file's static globals, same as the rest of main.c always has -
// nothing here is more "special" than a normal function, it's just
// reachable by name through command_find/keymap_resolve instead of only
// by a hardcoded switch case. This is the seam a config file's
// keybinding overrides attach to now, and where a future plugin/
// scripting layer would register its own commands alongside these.
//
// Movement commands (shift_extends_selection = true at registration,
// below) don't call editor_update_selection themselves - the dispatcher
// does that once, generically, for any command registered that way,
// right before calling it.
// ------------------------------------------------------------------------

static void cmd_move_line_begin(void)
{
    editor_flush_group(&editor);
    editor_move_to_line_begin(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_move_buffer_begin(void)
{
    editor_flush_group(&editor);
    editor_move_to_begin(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_move_line_end(void)
{
    editor_flush_group(&editor);
    editor_move_to_line_end(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_move_buffer_end(void)
{
    editor_flush_group(&editor);
    editor_move_to_end(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_move_char_left(void)
{
    editor_flush_group(&editor);
    editor_move_char_left(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_move_word_left(void)
{
    editor_flush_group(&editor);
    editor_move_word_left(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_move_char_right(void)
{
    editor_flush_group(&editor);
    editor_move_char_right(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_move_word_right(void)
{
    editor_flush_group(&editor);
    editor_move_word_right(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_move_line_up(void)
{
    editor_flush_group(&editor);
    editor_move_line_up(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_move_paragraph_up(void)
{
    editor_flush_group(&editor);
    editor_move_paragraph_up(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_move_line_down(void)
{
    editor_flush_group(&editor);
    editor_move_line_down(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_move_paragraph_down(void)
{
    editor_flush_group(&editor);
    editor_move_paragraph_down(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_backspace(void)
{
    editor_backspace(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_delete_forward(void)
{
    editor_delete(&editor);
    editor.last_stroke = SDL_GetTicks();
}

static void cmd_save(void)
{
    if (editor.file_path.count > 0) {
        editor_flush_group(&editor);
        Errno err = editor_save(&editor);
        if (err != 0) {
            flash_error("Could not save currently edited file: %s", strerror(err));
        } else {
            editor.dirty = false;
        }
    } else {
        editor_flush_group(&editor);
        save_as_path[0] = '\0';
        save_as_path_len = 0;
        mode = APP_MODE_SAVE_AS;
    }
}

static void cmd_save_as(void)
{
    editor_flush_group(&editor);
    save_as_path[0] = '\0';
    save_as_path_len = 0;
    if (editor.file_path.count > 0) {
        snprintf(save_as_path, SAVE_AS_PATH_CAP, "%s", editor.file_path.items);
        save_as_path_len = strlen(save_as_path);
    }
    mode = APP_MODE_SAVE_AS;
}

static void cmd_open_file_browser(void)
{
    request_action(CONFIRM_ACTION_OPEN_BROWSER);
}

static void cmd_new_file(void)
{
    request_action(CONFIRM_ACTION_NEW_FILE);
}

static void cmd_quit(void)
{
    request_action(CONFIRM_ACTION_QUIT);
}

static void cmd_reload_shaders(void)
{
    simple_renderer_reload_shaders(&sr);
}

static void cmd_undo(void)
{
    editor_undo(&editor);
}

static void cmd_redo(void)
{
    editor_redo(&editor);
}

static void cmd_confirm_line(void)
{
    if (editor.searching) {
        editor_flush_group(&editor);
        editor_stop_search(&editor);
    } else {
        editor_insert_char(&editor, '\n');
        editor.last_stroke = SDL_GetTicks();
    }
}

static void cmd_start_search(void)
{
    editor_flush_group(&editor);
    editor_start_search(&editor);
}

// Registered with shift_extends_selection = true, same as the movement
// commands - but unlike them, the order that matters (relative to
// editor_stop_search) doesn't quite line up with the dispatcher's
// "update selection, then run the command" sequencing: the original
// code called editor_update_selection AFTER stop_search, and
// editor_update_selection no-ops while e->searching is true. So in the
// rare case of pressing Escape+Shift while both searching AND holding a
// selection, this no longer clears that selection the way the original
// code did. Deliberately accepted: a zero-argument command can't
// replicate ordering that depends on the specific event that resolved
// to it, and this combination is obscure enough not to be worth
// breaking that abstraction over.
static void cmd_escape(void)
{
    editor_flush_group(&editor);
    editor_stop_search(&editor);
}

static void cmd_select_all(void)
{
    editor_flush_group(&editor);
    editor.selection = true;
    editor.select_begin = 0;
    editor.cursor = editor.data.count;
}

static void cmd_indent(void)
{
    editor_indent(&editor);
}

static void cmd_unindent(void)
{
    editor_unindent(&editor);
}

static void cmd_copy(void)
{
    editor_flush_group(&editor);
    editor_clipboard_copy(&editor);
}

static void cmd_cut(void)
{
    editor_flush_group(&editor);
    editor_clipboard_cut(&editor);
}

static void cmd_paste(void)
{
    editor_flush_group(&editor);
    editor_clipboard_paste(&editor);
}

typedef struct {
    const char *name;
    Command_Fn fn;
    bool shift_extends_selection;
} Command_Def;

static const Command_Def default_commands[] = {
    {"move-line-begin",     cmd_move_line_begin,     true},
    {"move-buffer-begin",   cmd_move_buffer_begin,   true},
    {"move-line-end",       cmd_move_line_end,       true},
    {"move-buffer-end",     cmd_move_buffer_end,     true},
    {"move-char-left",      cmd_move_char_left,      true},
    {"move-word-left",      cmd_move_word_left,      true},
    {"move-char-right",     cmd_move_char_right,     true},
    {"move-word-right",     cmd_move_word_right,     true},
    {"move-line-up",        cmd_move_line_up,        true},
    {"move-paragraph-up",   cmd_move_paragraph_up,   true},
    {"move-line-down",      cmd_move_line_down,      true},
    {"move-paragraph-down", cmd_move_paragraph_down, true},
    {"escape",              cmd_escape,              true},

    {"backspace",           cmd_backspace,           false},
    {"delete-forward",      cmd_delete_forward,      false},
    {"save",                cmd_save,                false},
    {"save-as",             cmd_save_as,             false},
    {"open-file-browser",   cmd_open_file_browser,   false},
    {"new-file",            cmd_new_file,            false},
    {"quit",                cmd_quit,                false},
    {"reload-shaders",      cmd_reload_shaders,      false},
    {"undo",                cmd_undo,                false},
    {"redo",                cmd_redo,                false},
    {"confirm-line",        cmd_confirm_line,        false},
    {"start-search",        cmd_start_search,        false},
    {"select-all",          cmd_select_all,          false},
    {"indent",              cmd_indent,              false},
    {"unindent",            cmd_unindent,            false},
    {"copy",                cmd_copy,                false},
    {"cut",                 cmd_cut,                 false},
    {"paste",               cmd_paste,               false},
};

typedef struct {
    const char *chord;
    const char *command;
} Keybind_Def;

// The built-in keymap. Every entry a movement/escape command needs is
// listed twice (bare and Shift-prefixed) since shift_extends_selection
// is handled generically by the dispatcher rather than by matching a
// "don't care about Shift" wildcard chord - see command.h.
//
// Three spots deliberately do NOT replicate a modifier-agnostic quirk
// the original hardcoded switch had (nobody chose it - the switch
// simply never bothered checking a modifier it didn't need):
//   - Backspace/Delete/Return/F2/F3/F5 no longer also fire when Ctrl
//     and/or Shift happen to be held alongside them.
//   - Escape no longer also fires on Ctrl+Escape (an OS-level shortcut
//     on some platforms anyway - not claiming it is arguably safer).
//   - Tab/Shift+Tab no longer also fire on Ctrl+Tab/Ctrl+Shift+Tab,
//     which frees those up for a future tab-switching feature (Ctrl+Tab
//     is the near-universal convention for it) instead of indenting.
static const Keybind_Def default_keybindings[] = {
    {"home",             "move-line-begin"},
    {"shift+home",       "move-line-begin"},
    {"ctrl+home",        "move-buffer-begin"},
    {"ctrl+shift+home",  "move-buffer-begin"},
    {"end",              "move-line-end"},
    {"shift+end",        "move-line-end"},
    {"ctrl+end",         "move-buffer-end"},
    {"ctrl+shift+end",   "move-buffer-end"},
    {"left",             "move-char-left"},
    {"shift+left",       "move-char-left"},
    {"ctrl+left",        "move-word-left"},
    {"ctrl+shift+left",  "move-word-left"},
    {"right",            "move-char-right"},
    {"shift+right",      "move-char-right"},
    {"ctrl+right",       "move-word-right"},
    {"ctrl+shift+right", "move-word-right"},
    {"up",               "move-line-up"},
    {"shift+up",         "move-line-up"},
    {"ctrl+up",          "move-paragraph-up"},
    {"ctrl+shift+up",    "move-paragraph-up"},
    {"down",             "move-line-down"},
    {"shift+down",       "move-line-down"},
    {"ctrl+down",        "move-paragraph-down"},
    {"ctrl+shift+down",  "move-paragraph-down"},
    {"escape",           "escape"},
    {"shift+escape",     "escape"},

    {"backspace",        "backspace"},
    {"delete",           "delete-forward"},
    {"f2",               "save"},
    {"ctrl+shift+s",     "save-as"},
    {"f3",               "open-file-browser"},
    {"ctrl+n",           "new-file"},
    {"ctrl+q",           "quit"},
    {"f5",               "reload-shaders"},
    {"ctrl+z",           "undo"},
    {"ctrl+shift+z",     "redo"},
    {"ctrl+y",           "redo"},
    {"return",           "confirm-line"},
    {"ctrl+f",           "start-search"},
    {"ctrl+a",           "select-all"},
    {"tab",              "indent"},
    {"shift+tab",        "unindent"},
    {"ctrl+c",           "copy"},
    {"ctrl+x",           "cut"},
    {"ctrl+v",           "paste"},
};

// Registers every built-in command and binds every built-in keybinding.
// Must run before config_load, so a `bind` line in the user's config
// overrides (rather than races) a default rather than the other way
// around - keymap_bind replaces an existing entry for the same chord in
// place (see command.h) instead of adding a shadowed duplicate.
static void register_default_commands_and_keymap(void)
{
    for (size_t i = 0; i < ARRAY_LEN(default_commands); ++i) {
        const Command_Def *def = &default_commands[i];
        command_register(def->name, def->fn, def->shift_extends_selection);
    }
    for (size_t i = 0; i < ARRAY_LEN(default_keybindings); ++i) {
        const Keybind_Def *def = &default_keybindings[i];
        bool ok = keymap_bind(def->chord, def->command);
        assert(ok && "a built-in keybinding failed to bind - this is a bug in default_keybindings/default_commands itself, not a runtime condition");
    }
}

int main(int argc, char **argv)
{
    Errno err;

    register_default_commands_and_keymap();

    Config cfg = config_default();
    err = config_load("./ded.conf", &cfg);
    if (err != 0) {
        fprintf(stderr, "WARNING: could not read ./ded.conf: %s\n", strerror(err));
    }
    editor.indent_width = cfg.tab_width;

    FT_Library library = {0};

    FT_Error error = FT_Init_FreeType(&library);
    if (error) {
        fprintf(stderr, "ERROR: Could not initialize FreeType2 library\n");
        return 1;
    }

    FT_Face face;
    error = FT_New_Face(library, cfg.font_path, 0, &face);
    if (error == FT_Err_Unknown_File_Format) {
        fprintf(stderr, "ERROR: `%s` has an unknown format\n", cfg.font_path);
        return 1;
    } else if (error) {
        fprintf(stderr, "ERROR: Could not load file `%s`\n", cfg.font_path);
        return 1;
    }

    FT_UInt pixel_size = FREE_GLYPH_FONT_SIZE;
    error = FT_Set_Pixel_Sizes(face, 0, pixel_size);
    if (error) {
        fprintf(stderr, "ERROR: Could not set pixel size to %u\n", pixel_size);
        return 1;
    }

    if (argc > 1) {
        const char *file_path = argv[1];
        err = editor_load_from_file(&editor, file_path);
        if (err != 0) {
            fprintf(stderr, "ERROR: Could not read file %s: %s\n", file_path, strerror(err));
            return 1;
        }
    }

    const char *dir_path = ".";
    err = fb_open_dir(&fb, dir_path);
    if (err != 0) {
        fprintf(stderr, "ERROR: Could not read directory %s: %s\n", dir_path, strerror(err));
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "ERROR: Could not initialize SDL: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Window *window =
        SDL_CreateWindow("ded",
                         0, 0,
                         SCREEN_WIDTH, SCREEN_HEIGHT,
                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL);
    if (window == NULL) {
        fprintf(stderr, "ERROR: Could not create SDL window: %s\n", SDL_GetError());
        return 1;
    }

    {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

        int major;
        int minor;
        SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, &major);
        SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, &minor);
        printf("GL version %d.%d\n", major, minor);
    }

    if (SDL_GL_CreateContext(window) == NULL) {
        fprintf(stderr, "ERROR: Could not create OpenGL context: %s\n", SDL_GetError());
        return 1;
    }

    GLenum glewErr = glewInit();
    if (GLEW_OK != glewErr) {
        fprintf(stderr, "ERROR: Could not initialize GLEW: %s\n", glewGetErrorString(glewErr));
        return 1;
    }

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    if (GLEW_ARB_debug_output) {
        glEnable(GL_DEBUG_OUTPUT);
        glDebugMessageCallback(MessageCallback, 0);
    } else {
        fprintf(stderr, "WARNING: GLEW_ARB_debug_output is not available");
    }

    simple_renderer_init(&sr);
    free_glyph_atlas_init(&atlas, face);

    editor.atlas = &atlas;
    editor_retokenize(&editor);

    vim = vim_state_init();
    editor.cursor_block = (vim.mode == VIM_MODE_NORMAL);

    while (!quit) {
        const Uint32 start = SDL_GetTicks();
        SDL_Event event = {0};
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT: {
                request_action(CONFIRM_ACTION_QUIT);
            }
            break;

            case SDL_KEYDOWN: {
                App_Mode mode_before_key = mode;
                switch (mode) {
                case APP_MODE_FILE_BROWSER: {
                    switch (event.key.keysym.sym) {
                    case SDLK_F3: {
                        mode = APP_MODE_EDITOR;
                    }
                    break;

                    case SDLK_UP: {
                        if (fb.cursor > 0) fb.cursor -= 1;
                    }
                    break;

                    case SDLK_DOWN: {
                        if (fb.cursor + 1 < fb.files.count) fb.cursor += 1;
                    }
                    break;

                    case SDLK_RETURN: {
                        const char *file_path = fb_file_path(&fb);
                        if (file_path) {
                            File_Type ft;
                            err = type_of_file(file_path, &ft);
                            if (err != 0) {
                                flash_error("Could not determine type of file %s: %s", file_path, strerror(err));
                            } else {
                                switch (ft) {
                                case FT_DIRECTORY: {
                                    err = fb_change_dir(&fb);
                                    if (err != 0) {
                                        flash_error("Could not change directory to %s: %s", file_path, strerror(err));
                                    }
                                }
                                break;

                                case FT_REGULAR: {
                                    // F3 already confirmed discarding unsaved changes.
                                    err = editor_load_from_file(&editor, file_path);
                                    if (err != 0) {
                                        flash_error("Could not open file %s: %s", file_path, strerror(err));
                                    } else {
                                        vim = vim_state_init();
                                        editor.cursor_block = true;
                                        mode = APP_MODE_EDITOR;
                                    }
                                }
                                break;

                                case FT_OTHER: {
                                    flash_error("%s is neither a regular file nor a directory. We can't open it.", file_path);
                                }
                                break;

                                default:
                                    UNREACHABLE("unknown File_Type");
                                }
                            }
                        }
                    }
                    break;
                    }
                } break;

                case APP_MODE_CONFIRM: {
                    switch (event.key.keysym.sym) {
                    case SDLK_y:
                        perform_action(pending_action);
                        break;

                    // Only explicit 'y' confirms discarding work. Enter is
                    // deliberately NOT an alias for it - this prompt exists
                    // specifically to catch a moment where Enter is easy to
                    // hit out of habit (mid-edit, or closing the window),
                    // so it has to fall back to the safe (cancel) option
                    // like 'n'/Escape do, not the destructive one.
                    case SDLK_n:
                    case SDLK_RETURN:
                    case SDLK_ESCAPE:
                        mode = APP_MODE_EDITOR;
                        break;

                    default:
                        break;
                    }
                } break;

                case APP_MODE_SAVE_AS: {
                    switch (event.key.keysym.sym) {
                    case SDLK_RETURN: {
                        if (save_as_path_len > 0) {
                            editor_flush_group(&editor);
                            err = editor_save_as(&editor, save_as_path);
                            if (err != 0) {
                                flash_error("Could not save as %s: %s", save_as_path, strerror(err));
                                // stay in the prompt so the path can be fixed
                            } else {
                                mode = APP_MODE_EDITOR;
                            }
                        }
                    }
                    break;

                    case SDLK_ESCAPE: {
                        mode = APP_MODE_EDITOR;
                    }
                    break;

                    case SDLK_BACKSPACE: {
                        if (save_as_path_len > 0) {
                            save_as_path_len -= 1;
                            save_as_path[save_as_path_len] = '\0';
                        }
                    }
                    break;

                    default:
                        break;
                    }
                } break;

                case APP_MODE_EDITOR: {
                    if (vim_handle_key(&vim, &editor, event.key.keysym)) {
                        editor.last_stroke = SDL_GetTicks();
                    } else {
                        const Command *cmd = keymap_resolve(event.key.keysym);
                        if (cmd != NULL) {
                            if (cmd->shift_extends_selection) {
                                bool extend = (event.key.keysym.mod & KMOD_SHIFT) || vim.mode == VIM_MODE_VISUAL;
                                editor_update_selection(&editor, extend);
                            }
                            cmd->fn();
                        }
                    }
                } break;
                }
                app_mode_changed_this_key = (mode != mode_before_key);
            }
            break;

            case SDL_TEXTINPUT: {
                if (app_mode_changed_this_key) {
                    app_mode_changed_this_key = false;
                    // See the flag's own comment: this key just switched
                    // which app-level mode we're in, so its trailing text
                    // event belongs to the mode we left and must be
                    // swallowed no matter which mode we're now in.
                } else
                switch (mode) {
                case APP_MODE_FILE_BROWSER:
                    // Nothing for now
                    // Once we have incremental search in the file browser this may become useful
                    break;

                case APP_MODE_CONFIRM:
                    // y/n is handled entirely in SDL_KEYDOWN above.
                    break;

                case APP_MODE_SAVE_AS: {
                    const char *text = event.text.text;
                    size_t text_len = strlen(text);
                    for (size_t i = 0; i < text_len && save_as_path_len + 1 < SAVE_AS_PATH_CAP; ++i) {
                        save_as_path[save_as_path_len++] = text[i];
                    }
                    save_as_path[save_as_path_len] = '\0';
                }
                break;

                case APP_MODE_EDITOR:
                    if (vim_take_consumed_textinput(&vim)) {
                        // key already claimed by vim_handle_key; don't also type it
                    } else if (!editor.searching && (vim.mode == VIM_MODE_NORMAL || vim.mode == VIM_MODE_VISUAL)) {
                        // unbound letters in these modes are commands, not text
                    } else {
                        const char *text = event.text.text;
                        size_t text_len = strlen(text);
                        for (size_t i = 0; i < text_len; ++i) {
                            editor_insert_char(&editor, text[i]);
                        }
                        editor.last_stroke = SDL_GetTicks();
                    }
                    break;
                }
            }
            break;
            }
        }

        {
            int w, h;
            SDL_GetWindowSize(window, &w, &h);
            // TODO(#19): update the viewport and the resolution only on actual window change
            glViewport(0, 0, w, h);
        }

        Vec4f bg = hex_to_vec4f(0x181818FF);
        glClearColor(bg.x, bg.y, bg.z, bg.w);
        glClear(GL_COLOR_BUFFER_BIT);

        if (mode == APP_MODE_FILE_BROWSER) {
            fb_render(&fb, window, &atlas, &sr);
        } else {
            editor_render(window, &atlas, &sr, &editor);

            Vec4f bar_fg = vec4fs(1.0f);
            if (mode == APP_MODE_CONFIRM) {
                draw_bottom_bar(&sr, &atlas, confirm_message(pending_action), hex_to_vec4f(0x4A1D1DFF), bar_fg);
            } else if (mode == APP_MODE_SAVE_AS) {
                char prompt[SAVE_AS_PATH_CAP + 32];
                snprintf(prompt, sizeof(prompt), "Save as: %s_", save_as_path);
                draw_bottom_bar(&sr, &atlas, prompt, hex_to_vec4f(0x1D2E4AFF), bar_fg);
            } else if (error_message[0] != '\0' && SDL_GetTicks() - error_message_time < ERROR_DISPLAY_MS) {
                draw_bottom_bar(&sr, &atlas, error_message, hex_to_vec4f(0x4A1D1DFF), bar_fg);
            }
        }

        SDL_GL_SwapWindow(window);

        const Uint32 duration = SDL_GetTicks() - start;
        const Uint32 delta_time_ms = 1000 / FPS;
        if (duration < delta_time_ms) {
            SDL_Delay(delta_time_ms - duration);
        }
    }

    return 0;
}

// TODO: ability to search within file browser
// Very useful when you have a lot of files
// TODO: ability to remove trailing whitespaces
