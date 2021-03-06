/*
   Editor initialisation and callback handler.

   Copyright (C) 1996, 1997, 1998, 2001, 2002, 2003, 2004, 2005, 2006,
   2007, 2011, 2012
   The Free Software Foundation, Inc.

   Written by:
   Paul Sheer, 1996, 1997
   Andrew Borodin <aborodin@vmail.ru> 2012

   This file is part of the Midnight Commander.

   The Midnight Commander is free software: you can redistribute it
   and/or modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation, either version 3 of the License,
   or (at your option) any later version.

   The Midnight Commander is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/** \file
 *  \brief Source: editor initialisation and callback handler
 *  \author Paul Sheer
 *  \date 1996, 1997
 */

#include <config.h>

#include <stdio.h>
#include <stdarg.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdlib.h>

#include "lib/global.h"

#include "lib/tty/tty.h"        /* LINES, COLS */
#include "lib/tty/key.h"        /* is_idle() */
#include "lib/tty/color.h"      /* tty_setcolor() */
#include "lib/skin.h"
#include "lib/strutil.h"        /* str_term_trim() */
#include "lib/util.h"           /* mc_build_filename() */
#include "lib/widget.h"
#include "lib/mcconfig.h"
#include "lib/event.h"          /* mc_event_raise() */

#include "src/keybind-defaults.h"
#include "src/main.h"           /* home_dir */
#include "src/filemanager/cmd.h"        /* view_other_cmd(), save_setup_cmd()  */
#include "src/learn.h"          /* learn_keys() */
#include "src/args.h"           /* mcedit_arg_t */

#include "edit-impl.h"
#include "editwidget.h"
#ifdef HAVE_ASPELL
#include "spell.h"
#endif

/*** global variables ****************************************************************************/

char *edit_window_state_char = NULL;
char *edit_window_close_char = NULL;

/*** file scope macro definitions ****************************************************************/

#define WINDOW_MIN_LINES (2 + 2)
#define WINDOW_MIN_COLS (2 + LINE_STATE_WIDTH + 2)

/*** file scope type declarations ****************************************************************/

/*** file scope variables ************************************************************************/
static unsigned int edit_dlg_init_refcounter = 0;

/*** file scope functions ************************************************************************/

static cb_ret_t edit_callback (Widget * w, widget_msg_t msg, int parm);
static cb_ret_t edit_dialog_callback (Dlg_head * h, Widget * sender, dlg_msg_t msg, int parm,
                                      void *data);

/* --------------------------------------------------------------------------------------------- */
/**
 * Init the 'edit' subsystem
 */

static void
edit_dlg_init (void)
{
    if (edit_dlg_init_refcounter == 0)
    {
        edit_window_state_char = mc_skin_get ("editor", "window-state-char", "*");
        edit_window_close_char = mc_skin_get ("editor", "window-close-char", "X");

#ifdef HAVE_ASPELL
        aspell_init ();
#endif
    }

    edit_dlg_init_refcounter++;
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Deinit the 'edit' subsystem
 */

static void
edit_dlg_deinit (void)
{
    if (edit_dlg_init_refcounter != 0)
        edit_dlg_init_refcounter--;

    if (edit_dlg_init_refcounter == 0)
    {
        g_free (edit_window_state_char);
        g_free (edit_window_close_char);

#ifdef HAVE_ASPELL
        aspell_clean ();
#endif
    }
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Show info about editor
 */

static void
edit_about (void)
{
    const char *header = N_("About");
    const char *button_name = N_("&OK");
    const char *const version = "MCEdit " VERSION;
    char text[BUF_LARGE];

    int win_len, version_len, button_len;
    int cols, lines;

    Dlg_head *about_dlg;

#ifdef ENABLE_NLS
    header = _(header);
    button_name = _(button_name);
#endif

    button_len = str_term_width1 (button_name) + 5;
    version_len = str_term_width1 (version);

    g_snprintf (text, sizeof (text),
                _("Copyright (C) 1996-2012 the Free Software Foundation\n\n"
                  "            A user friendly text editor\n"
                  "         written for the Midnight Commander"));

    win_len = str_term_width1 (header);
    win_len = max (win_len, version_len);
    win_len = max (win_len, button_len);

    /* count width and height of text */
    str_msg_term_size (text, &lines, &cols);
    lines += 9;
    cols = max (win_len, cols) + 6;

    /* dialog */
    about_dlg = create_dlg (TRUE, 0, 0, lines, cols, dialog_colors, NULL, NULL,
                            "[Internal File Editor]", header, DLG_CENTER | DLG_TRYUP);

    add_widget (about_dlg, label_new (3, (cols - version_len) / 2, version));
    add_widget (about_dlg, label_new (5, 3, text));
    add_widget (about_dlg, button_new (lines - 3, (cols - button_len) / 2,
                                       B_ENTER, NORMAL_BUTTON, button_name, NULL));

    run_dlg (about_dlg);
    destroy_dlg (about_dlg);
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Show a help window
 */

static void
edit_help (void)
{
    ev_help_t event_data = { NULL, "[Internal File Editor]" };
    mc_event_raise (MCEVENT_GROUP_CORE, "help", &event_data);
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Callback for the iteration of objects in the 'editors' array.
 * Resize the editor window.
 *
 * @param data      probably WEdit object
 * @param user_data unused
 */

static void
edit_dialog_resize_cb (void *data, void *user_data)
{
    Widget *w = (Widget *) data;

    (void) user_data;

    if (edit_widget_is_editor (w) && ((WEdit *) w)->fullscreen)
    {
        Dlg_head *h = w->owner;

        w->lines = h->lines - 2;
        w->cols = h->cols;
    }
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Restore saved window size.
 *
 * @param edit editor object
 */

static void
edit_restore_size (WEdit * edit)
{
    edit->drag_state = MCEDIT_DRAG_NORMAL;
    widget_set_size ((Widget *) edit, edit->y_prev, edit->x_prev,
                     edit->lines_prev, edit->cols_prev);
    dlg_redraw (((Widget *) edit)->owner);
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Move window by one row or column in any direction.
 *
 * @param edit    editor object
 * @param command direction (CK_Up, CK_Down, CK_Left, CK_Right)
 */

static void
edit_window_move (WEdit * edit, unsigned long command)
{
    Widget *w = (Widget *) edit;
    Dlg_head *h = w->owner;

    switch (command)
    {
    case CK_Up:
        if (w->y > h->y + 1)    /* menubar */
            w->y--;
        break;
    case CK_Down:
        if (w->y < h->y + h->lines - 2) /* buttonbar */
            w->y++;
        break;
    case CK_Left:
        if (w->x + w->cols > h->x)
            w->x--;
        break;
    case CK_Right:
        if (w->x < h->x + h->cols)
            w->x++;
        break;
    default:
        return;
    }

    edit->force |= REDRAW_PAGE;
    dlg_redraw (h);
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Resize window by one row or column in any direction.
 *
 * @param edit    editor object
 * @param command direction (CK_Up, CK_Down, CK_Left, CK_Right)
 */

static void
edit_window_resize (WEdit * edit, unsigned long command)
{
    Widget *w = (Widget *) edit;
    Dlg_head *h = w->owner;

    switch (command)
    {
    case CK_Up:
        if (w->lines > WINDOW_MIN_LINES)
            w->lines--;
        break;
    case CK_Down:
        if (w->y + w->lines < h->y + h->lines - 1)      /* buttonbar */
            w->lines++;
        break;
    case CK_Left:
        if (w->cols > WINDOW_MIN_COLS)
            w->cols--;
        break;
    case CK_Right:
        if (w->x + w->cols < h->x + h->cols)
            w->cols++;
        break;
    default:
        return;
    }

    edit->force |= REDRAW_COMPLETELY;
    dlg_redraw (h);
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Get hotkey by number.
 *
 * @param n number
 * @return hotkey
 */

static unsigned char
get_hotkey (int n)
{
    return (n <= 9) ? '0' + n : 'a' + n - 10;
}

/* --------------------------------------------------------------------------------------------- */

static void
edit_window_list (const Dlg_head * h)
{
    const size_t offset = 2;    /* skip menu and buttonbar */
    const size_t dlg_num = g_list_length (h->widgets) - offset;
    int lines, cols;
    Listbox *listbox;
    GList *w;
    int i = 0;
    int rv;

    lines = min ((size_t) (LINES * 2 / 3), dlg_num);
    cols = COLS * 2 / 3;

    listbox = create_listbox_window (lines, cols, _("Open files"), "[Open files]");

    for (w = h->widgets; w != NULL; w = g_list_next (w))
        if (edit_widget_is_editor ((Widget *) w->data))
        {
            WEdit *e = (WEdit *) w->data;
            char *fname;

            if (e->filename_vpath == NULL)
                fname = g_strdup_printf ("%c [%s]", e->modified ? '*' : ' ', _("NoName"));
            else
            {
                char *fname2;

                fname2 = vfs_path_to_str (e->filename_vpath);
                fname = g_strdup_printf ("%c%s", e->modified ? '*' : ' ', fname2);
                g_free (fname2);
            }

            listbox_add_item (listbox->list, LISTBOX_APPEND_AT_END, get_hotkey (i++),
                              str_term_trim (fname, listbox->list->widget.cols - 2), NULL);
            g_free (fname);
        }

    rv = g_list_position (h->widgets, h->current) - offset;
    listbox_select_entry (listbox->list, rv);
    rv = run_listbox (listbox);
    if (rv >= 0)
    {
        w = g_list_nth (h->widgets, rv + offset);
        dlg_set_top_widget (w->data);
    }
}

/* --------------------------------------------------------------------------------------------- */

static char *
edit_get_shortcut (unsigned long command)
{
    const char *ext_map;
    const char *shortcut = NULL;

    shortcut = keybind_lookup_keymap_shortcut (editor_map, command);
    if (shortcut != NULL)
        return g_strdup (shortcut);

    ext_map = keybind_lookup_keymap_shortcut (editor_map, CK_ExtendedKeyMap);
    if (ext_map != NULL)
        shortcut = keybind_lookup_keymap_shortcut (editor_x_map, command);
    if (shortcut != NULL)
        return g_strdup_printf ("%s %s", ext_map, shortcut);

    return NULL;
}

/* --------------------------------------------------------------------------------------------- */

static char *
edit_get_title (const Dlg_head * h, size_t len)
{
    const WEdit *edit = find_editor (h);
    const char *modified = edit->modified ? "(*) " : "    ";
    const char *file_label;
    char *filename;

    len -= 4;

    filename = vfs_path_to_str (edit->filename_vpath);
    if (filename == NULL)
        filename = g_strdup (_("[NoName]"));
    file_label = str_term_trim (filename, len - str_term_width1 (_("Edit: ")));
    g_free (filename);

    return g_strconcat (_("Edit: "), modified, file_label, (char *) NULL);
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Handle mouse events of editor window
 *
 * @param event mouse event
 * @param data editor window
 * @return MOU_NORMAL if event was handled, MOU_UNHANDLED otherwise
 */

static int
edit_event (Gpm_Event * event, void *data)
{
    WEdit *edit = (WEdit *) data;
    Widget *w = (Widget *) data;
    Gpm_Event local;

    if (!mouse_global_in_widget (event, w))
        return MOU_UNHANDLED;

    local = mouse_get_local (event, w);

    /* Unknown event type */
    if ((event->type & (GPM_DOWN | GPM_DRAG | GPM_UP)) == 0)
        return MOU_NORMAL;

    dlg_set_top_widget (w);

    edit_update_curs_row (edit);
    edit_update_curs_col (edit);

    if (edit->fullscreen || (local.buttons & GPM_B_LEFT) == 0 || (local.type & GPM_UP) != 0)
        edit->drag_state = MCEDIT_DRAG_NORMAL;
    else if (local.y == 1 && edit->drag_state != MCEDIT_DRAG_RESIZE)
    {
        /* click on the top line (move) */
        int dx = edit->fullscreen ? 0 : 2;

        if (local.x == edit->widget.cols - dx - 1)
        {
            edit_dialog_callback (w->owner, NULL, DLG_ACTION, CK_Close, NULL);
            return MOU_NORMAL;
        }

        if (local.x == edit->widget.cols - dx - 4)
        {
            edit_toggle_fullscreen (edit);
            return MOU_NORMAL;
        }

        if ((local.type & (GPM_DOWN | GPM_DRAG)) != 0)
        {
            /* move if not fullscreen */
            edit->drag_state_start = local.x;
            edit->drag_state = MCEDIT_DRAG_MOVE;
            edit->force |= REDRAW_COMPLETELY;
            edit_update_screen (edit);
        }
    }
    else if (!edit->fullscreen && local.y == w->lines && local.x == w->cols)
    {
        /* click on bottom-right corner (resize) */
        if ((local.type & (GPM_DOWN | GPM_DRAG)) != 0)
        {
            edit->drag_state = MCEDIT_DRAG_RESIZE;
            edit->force |= REDRAW_COMPLETELY;
            edit_update_screen (edit);
        }
    }

    if (edit->drag_state == MCEDIT_DRAG_NORMAL)
    {
        gboolean done = TRUE;

        /* Double click */
        if ((local.type & (GPM_DOUBLE | GPM_UP)) == (GPM_UP | GPM_DOUBLE))
        {
            edit_mark_current_word_cmd (edit);
            goto update;
        }
#if 0
        /* Triple click */
        if ((local.type & (GPM_TRIPLE | GPM_UP)) == (GPM_UP | GPM_TRIPLE))
        {
            edit_mark_current_line_cmd (edit);
            goto update;
        }
#endif
        /* Wheel events */
        if ((local.buttons & GPM_B_UP) != 0 && (local.type & GPM_DOWN) != 0)
        {
            edit_move_up (edit, 2, 1);
            goto update;
        }
        if ((local.buttons & GPM_B_DOWN) != 0 && (local.type & GPM_DOWN) != 0)
        {
            edit_move_down (edit, 2, 1);
            goto update;
        }

        /* continue handle current event */
        goto cont;

        /* handle DRAG mouse event, don't use standard way to continue
         * event handling outside of widget */
        do
        {
            int c;

            c = tty_get_event (event, FALSE, TRUE);
            if (c == EV_NONE || c != EV_MOUSE)
                break;

            local = mouse_get_local (event, w);

          cont:
            /* A lone up mustn't do anything */
            if (edit->mark2 != -1 && (local.type & (GPM_UP | GPM_DRAG)) != 0)
                return MOU_NORMAL;

            if ((local.type & (GPM_DOWN | GPM_UP)) != 0)
                edit_push_key_press (edit);

            if (!edit->fullscreen)
                local.x--;
            if (!option_cursor_beyond_eol)
                edit->prev_col = local.x - edit->start_col - option_line_state_width - 1;
            else
            {
                long line_len = edit_move_forward3 (edit, edit_bol (edit, edit->curs1), 0,
                                                    edit_eol (edit, edit->curs1));

                if (local.x > line_len)
                {
                    edit->over_col =
                        local.x - line_len - edit->start_col - option_line_state_width - 1;
                    edit->prev_col = line_len;
                }
                else
                {
                    edit->over_col = 0;
                    edit->prev_col = local.x - option_line_state_width - edit->start_col - 1;
                }
            }

            if (!edit->fullscreen)
                local.y--;
            if (local.y > (edit->curs_row + 1))
                edit_move_down (edit, local.y - (edit->curs_row + 1), 0);
            else if (local.y < (edit->curs_row + 1))
                edit_move_up (edit, (edit->curs_row + 1) - local.y, 0);
            else
                edit_move_to_prev_col (edit, edit_bol (edit, edit->curs1));

            if ((local.type & GPM_DOWN) != 0)
            {
                edit_mark_cmd (edit, 1);        /* reset */
                edit->highlight = 0;
            }

            done = (local.type & GPM_DRAG) == 0;
            if (done)
                edit_mark_cmd (edit, 0);

          update:
            edit_find_bracket (edit);
            edit->force |= REDRAW_COMPLETELY;
            edit_update_curs_row (edit);
            edit_update_curs_col (edit);
            edit_update_screen (edit);
        }
        while (!edit->fullscreen && !done);
    }
    else
        while (edit->drag_state != MCEDIT_DRAG_NORMAL)
        {
            int c;
            int y;

            c = tty_get_event (event, FALSE, TRUE);
            y = event->y - 1;

            if (c == EV_NONE || c != EV_MOUSE)
            {
                /* redraw frame */
                edit->drag_state = MCEDIT_DRAG_NORMAL;
                edit->force |= REDRAW_COMPLETELY;
                edit_update_screen (edit);
            }
            else if (y == w->y && (event->type & (GPM_DOUBLE | GPM_UP)) == (GPM_DOUBLE | GPM_UP))
            {
                /* double click on top line (toggle fullscreen) */
                edit_toggle_fullscreen (edit);
                edit->drag_state = MCEDIT_DRAG_NORMAL;
                edit->force |= REDRAW_COMPLETELY;
                edit_update_screen (edit);
            }
            else if ((event->type & (GPM_DRAG | GPM_DOWN)) == 0)
            {
                /* redraw frame */
                edit->drag_state = MCEDIT_DRAG_NORMAL;
                edit->force |= REDRAW_COMPLETELY;
                edit_update_screen (edit);
            }
            else if (!edit->fullscreen)
            {
                Dlg_head *h = w->owner;

                if (edit->drag_state == MCEDIT_DRAG_MOVE)
                {
                    int x = event->x - 1;

                    y = max (y, h->y + 1);      /* status line */
                    y = min (y, h->y + h->lines - 2);   /* buttonbar */
                    x = max (x, h->x);
                    x = min (x, h->x + h->cols - 1);
                    /* don't use widget_set_size() here to avoid double draw  */
                    w->y = y;
                    w->x = x - edit->drag_state_start;
                    edit->force |= REDRAW_COMPLETELY;
                }
                else if (edit->drag_state == MCEDIT_DRAG_RESIZE)
                {
                    event->y = min (event->y, h->y + h->lines - 1);     /* buttonbar */
                    event->x = min (event->x, h->x + h->cols);
                    local = mouse_get_local (event, w);

                    /* don't use widget_set_size() here to avoid double draw  */
                    w->lines = max (WINDOW_MIN_LINES, local.y);
                    w->cols = max (WINDOW_MIN_COLS, local.x);
                    edit->force |= REDRAW_COMPLETELY;
                }

                dlg_redraw (h);
            }
        }

    return MOU_NORMAL;
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Handle mouse events of editor screen.
 *
 * @param event mouse event
 * @param data editor screen
 * @return MOU_NORMAL if event was handled, MOU_UNHANDLED otherwise
 */

static int
edit_dialog_event (Gpm_Event * event, void *data)
{
    Dlg_head *h = (Dlg_head *) data;
    Widget *w;
    int ret = MOU_UNHANDLED;

    w = (Widget *) find_menubar (h);

    if (event->y == h->y + 1 && (event->type & GPM_DOWN) != 0 && !((WMenuBar *) w)->is_active)
    {
        /* menubar */

        GList *l;
        GList *top = NULL;
        int x;

        /* Try find top fullscreen window */
        for (l = h->widgets; l != NULL; l = g_list_next (l))
            if (edit_widget_is_editor ((Widget *) l->data) && ((WEdit *) l->data)->fullscreen)
                top = l;

        /* Handle fullscreen/close buttons in the top line */
        x = h->x + h->cols + 1 - 6;

        if (top != NULL && event->x >= x)
        {
            WEdit *e;

            e = (WEdit *) top->data;
            x = event->x - x;

            if (top != h->current)
            {
                /* Window is not active. Activate it */
                dlg_set_top_widget (e);
            }

            /* Handle buttons */
            if (x <= 2)
                edit_toggle_fullscreen (e);
            else
                edit_dialog_callback (h, NULL, DLG_ACTION, CK_Close, NULL);

            ret = MOU_NORMAL;
        }

        if (ret == MOU_UNHANDLED)
            dlg_select_widget (w);
    }
    else if (event->y == h->y + h->lines)
    {
        /* buttonbar */

        /* In general, this can be handled in default way (dlg_mouse_event)
         * but let make it here to avoid walking in widget list */
        w = (Widget *) find_buttonbar (h);
        ret = w->mouse (event, w);
    }

    return ret;
}

/* --------------------------------------------------------------------------------------------- */

static cb_ret_t
edit_dialog_command_execute (Dlg_head * h, unsigned long command)
{
    gboolean ret = MSG_HANDLED;

    switch (command)
    {
    case CK_EditNew:
        edit_add_window (h, h->y + 1, h->x, h->lines - 2, h->cols, NULL, 0);
        break;
    case CK_EditFile:
        edit_load_cmd (h);
        break;
    case CK_EditSyntaxFile:
        edit_load_syntax_file (h);
        break;
    case CK_EditUserMenu:
        edit_load_menu_file (h);
        break;
    case CK_Close:
        /* if there are no opened files anymore, close MC editor */
        if (edit_widget_is_editor ((Widget *) h->current->data) &&
            edit_close_cmd ((WEdit *) h->current->data) && find_editor (h) == NULL)
            dlg_stop (h);
        break;
    case CK_Help:
        edit_help ();
        /* edit->force |= REDRAW_COMPLETELY; */
        break;
    case CK_Menu:
        edit_menu_cmd (h);
        break;
    case CK_Quit:
    case CK_Cancel:
        {
            Widget *w = (Widget *) h->current->data;

            if (!edit_widget_is_editor (w) || ((WEdit *) w)->drag_state == MCEDIT_DRAG_NORMAL)
                dlg_stop (h);
            else
                edit_restore_size ((WEdit *) w);
        }
        break;
    case CK_About:
        edit_about ();
        break;
    case CK_SyntaxOnOff:
        edit_syntax_onoff_cmd (h);
        break;
    case CK_ShowTabTws:
        edit_show_tabs_tws_cmd (h);
        break;
    case CK_ShowMargin:
        edit_show_margin_cmd (h);
        break;
    case CK_ShowNumbers:
        edit_show_numbers_cmd (h);
        break;
    case CK_Refresh:
        edit_refresh_cmd ();
        break;
    case CK_Shell:
        view_other_cmd ();
        break;
    case CK_LearnKeys:
        learn_keys ();
        break;
    case CK_WindowMove:
    case CK_WindowResize:
        if (edit_widget_is_editor ((Widget *) h->current->data))
            edit_handle_move_resize ((WEdit *) h->current->data, command);
        break;
    case CK_WindowList:
        edit_window_list (h);
        break;
    case CK_WindowNext:
        dlg_one_down (h);
        dlg_set_top_widget (h->current->data);
        break;
    case CK_WindowPrev:
        dlg_one_up (h);
        dlg_set_top_widget (h->current->data);
        break;
    case CK_Options:
        edit_options_dialog (h);
        break;
    case CK_OptionsSaveMode:
        edit_save_mode_cmd ();
        break;
    case CK_SaveSetup:
        save_setup_cmd ();
        break;
    default:
        ret = MSG_NOT_HANDLED;
        break;
    }

    return ret;
}

/* --------------------------------------------------------------------------------------------- */

static inline void
edit_quit (Dlg_head * h)
{
    GList *l;
    WEdit *e = NULL;

    h->state = DLG_ACTIVE;      /* don't stop the dialog before final decision */

    for (l = h->widgets; l != NULL; l = g_list_next (l))
        if (edit_widget_is_editor ((Widget *) l->data))
        {
            e = (WEdit *) l->data;

            if (e->drag_state != MCEDIT_DRAG_NORMAL)
            {
                edit_restore_size (e);
                return;
            }

            if (e->modified)
            {
                dlg_select_widget (e);

                if (!edit_ok_to_exit (e))
                    return;
            }
        }

    /* no editors in dialog at all or no any file required to be saved */
    if (e == NULL || l == NULL)
        h->state = DLG_CLOSED;
}

/* --------------------------------------------------------------------------------------------- */

static inline void
edit_set_buttonbar (WEdit * edit, WButtonBar * bb)
{
    buttonbar_set_label (bb, 1, Q_ ("ButtonBar|Help"), editor_map, NULL);
    buttonbar_set_label (bb, 2, Q_ ("ButtonBar|Save"), editor_map, (Widget *) edit);
    buttonbar_set_label (bb, 3, Q_ ("ButtonBar|Mark"), editor_map, (Widget *) edit);
    buttonbar_set_label (bb, 4, Q_ ("ButtonBar|Replac"), editor_map, (Widget *) edit);
    buttonbar_set_label (bb, 5, Q_ ("ButtonBar|Copy"), editor_map, (Widget *) edit);
    buttonbar_set_label (bb, 6, Q_ ("ButtonBar|Move"), editor_map, (Widget *) edit);
    buttonbar_set_label (bb, 7, Q_ ("ButtonBar|Search"), editor_map, (Widget *) edit);
    buttonbar_set_label (bb, 8, Q_ ("ButtonBar|Delete"), editor_map, (Widget *) edit);
    buttonbar_set_label (bb, 9, Q_ ("ButtonBar|PullDn"), editor_map, NULL);
    buttonbar_set_label (bb, 10, Q_ ("ButtonBar|Quit"), editor_map, NULL);
}

/* --------------------------------------------------------------------------------------------- */
/** Callback for the edit dialog */

static cb_ret_t
edit_dialog_callback (Dlg_head * h, Widget * sender, dlg_msg_t msg, int parm, void *data)
{
    WMenuBar *menubar;
    WButtonBar *buttonbar;

    switch (msg)
    {
    case DLG_INIT:
        edit_dlg_init ();
        return MSG_HANDLED;

    case DLG_DRAW:
        /* don't use common_dialog_repaint() -- we don't need a frame */
        tty_setcolor (EDITOR_BACKGROUND);
        dlg_erase (h);
        return MSG_HANDLED;

    case DLG_RESIZE:
        menubar = find_menubar (h);
        buttonbar = find_buttonbar (h);
        /* dlg_set_size() is surplus for this case */
        h->lines = LINES;
        h->cols = COLS;
        widget_set_size (&buttonbar->widget, h->lines - 1, h->x, 1, h->cols);
        widget_set_size (&menubar->widget, h->y, h->x, 1, h->cols);
        menubar_arrange (menubar);
        g_list_foreach (h->widgets, (GFunc) edit_dialog_resize_cb, NULL);
        return MSG_HANDLED;

    case DLG_ACTION:
        /* shortcut */
        if (sender == NULL)
            return edit_dialog_command_execute (h, parm);
        /* message from menu */
        menubar = find_menubar (h);
        if (sender == (Widget *) menubar)
        {
            if (edit_dialog_command_execute (h, parm) == MSG_HANDLED)
                return MSG_HANDLED;
            /* try send command to the current window */
            return send_message ((Widget *) h->current->data, WIDGET_COMMAND, parm);
        }
        /* message from buttonbar */
        buttonbar = find_buttonbar (h);
        if (sender == (Widget *) buttonbar)
        {
            if (data != NULL)
                return send_message ((Widget *) data, WIDGET_COMMAND, parm);
            return edit_dialog_command_execute (h, parm);
        }
        return MSG_NOT_HANDLED;

    case DLG_KEY:
        {
            Widget *w = h->current->data;
            cb_ret_t ret = MSG_NOT_HANDLED;

            if (edit_widget_is_editor (w))
            {
                WEdit *e = (WEdit *) w;
                unsigned long command;

                if (!e->extmod)
                    command = keybind_lookup_keymap_command (editor_map, parm);
                else
                {
                    e->extmod = FALSE;
                    command = keybind_lookup_keymap_command (editor_x_map, parm);
                }

                if (command != CK_IgnoreKey)
                    ret = edit_dialog_command_execute (h, command);
            }

            return ret;
        }

        /* hardcoded menu hotkeys (see edit_drop_hotkey_menu) */
    case DLG_UNHANDLED_KEY:
        return edit_drop_hotkey_menu (h, parm) ? MSG_HANDLED : MSG_NOT_HANDLED;

    case DLG_VALIDATE:
        edit_quit (h);
        return MSG_HANDLED;

    case DLG_END:
        edit_dlg_deinit ();
        return MSG_HANDLED;

    default:
        return default_dlg_callback (h, sender, msg, parm, data);
    }
}

/* --------------------------------------------------------------------------------------------- */

static cb_ret_t
edit_callback (Widget * w, widget_msg_t msg, int parm)
{
    WEdit *e = (WEdit *) w;

    switch (msg)
    {
    case WIDGET_FOCUS:
        edit_set_buttonbar (e, find_buttonbar (e->widget.owner));
        /* fall through */

    case WIDGET_DRAW:
        e->force |= REDRAW_COMPLETELY;
        edit_update_screen (e);
        return MSG_HANDLED;

    case WIDGET_UNFOCUS:
        /* redraw frame and status */
        edit_status (e, FALSE);
        return MSG_HANDLED;

    case WIDGET_KEY:
        {
            int cmd, ch;
            cb_ret_t ret = MSG_NOT_HANDLED;

            /* The user may override the access-keys for the menu bar. */
            if (macro_index == -1 && edit_execute_macro (e, parm))
            {
                edit_update_screen (e);
                ret = MSG_HANDLED;
            }
            else if (edit_translate_key (e, parm, &cmd, &ch))
            {
                edit_execute_key_command (e, cmd, ch);
                edit_update_screen (e);
                ret = MSG_HANDLED;
            }

            return ret;
        }

    case WIDGET_COMMAND:
        /* command from menubar or buttonbar */
        edit_execute_key_command (e, parm, -1);
        edit_update_screen (e);
        return MSG_HANDLED;

    case WIDGET_CURSOR:
        {
            int y, x;

            y = (e->fullscreen ? 0 : 1) + EDIT_TEXT_VERTICAL_OFFSET + e->curs_row;
            x = (e->fullscreen ? 0 : 1) + EDIT_TEXT_HORIZONTAL_OFFSET + option_line_state_width +
                e->curs_col + e->start_col + e->over_col;

            widget_move (w, y, x);
            return MSG_HANDLED;
        }

    case WIDGET_DESTROY:
        edit_clean (e);
        return MSG_HANDLED;

    default:
        return default_proc (msg, parm);
    }
}

/* --------------------------------------------------------------------------------------------- */
/*** public functions ****************************************************************************/
/* --------------------------------------------------------------------------------------------- */
/**
 * Edit one file.
 *
 * @param file_vpath file object
 * @param line       line number
 * @return TRUE if no errors was occured, FALSE otherwise
 */

gboolean
edit_file (const vfs_path_t * file_vpath, int line)
{
    mcedit_arg_t arg = { (vfs_path_t *) file_vpath, line };
    GList *files;
    gboolean ok;

    files = g_list_prepend (NULL, &arg);
    ok = edit_files (files);
    g_list_free (files);

    return ok;
}

/* --------------------------------------------------------------------------------------------- */

gboolean
edit_files (const GList * files)
{
    static gboolean made_directory = FALSE;
    Dlg_head *edit_dlg;
    WMenuBar *menubar;
    const GList *file;
    gboolean ok = FALSE;

    if (!made_directory)
    {
        char *dir;

        dir = mc_build_filename (mc_config_get_cache_path (), EDIT_DIR, NULL);
        made_directory = (mkdir (dir, 0700) != -1 || errno == EEXIST);
        g_free (dir);

        dir = mc_build_filename (mc_config_get_path (), EDIT_DIR, NULL);
        made_directory = (mkdir (dir, 0700) != -1 || errno == EEXIST);
        g_free (dir);

        dir = mc_build_filename (mc_config_get_data_path (), EDIT_DIR, NULL);
        made_directory = (mkdir (dir, 0700) != -1 || errno == EEXIST);
        g_free (dir);
    }

    /* Create a new dialog and add it widgets to it */
    edit_dlg =
        create_dlg (FALSE, 0, 0, LINES, COLS, NULL, edit_dialog_callback, edit_dialog_event,
                    "[Internal File Editor]", NULL, DLG_WANT_TAB);

    edit_dlg->get_shortcut = edit_get_shortcut;
    edit_dlg->get_title = edit_get_title;

    menubar = menubar_new (0, 0, COLS, NULL);
    add_widget (edit_dlg, menubar);
    edit_init_menu (menubar);

    add_widget (edit_dlg, buttonbar_new (TRUE));

    for (file = files; file != NULL; file = g_list_next (file))
    {
        mcedit_arg_t *f = (mcedit_arg_t *) file->data;
        gboolean f_ok;

        f_ok = edit_add_window (edit_dlg, edit_dlg->y + 1, edit_dlg->x,
                                edit_dlg->lines - 2, edit_dlg->cols, f->file_vpath, f->line_number);
        /* at least one file has been opened succefully */
        ok = ok || f_ok;
    }

    if (ok)
        run_dlg (edit_dlg);

    if (!ok || edit_dlg->state == DLG_CLOSED)
        destroy_dlg (edit_dlg);

    return ok;
}

/* --------------------------------------------------------------------------------------------- */

char *
edit_get_file_name (const WEdit * edit)
{
    return vfs_path_to_str (edit->filename_vpath);
}

/* --------------------------------------------------------------------------------------------- */

WEdit *
find_editor (const Dlg_head * h)
{
    if (edit_widget_is_editor ((Widget *) h->current->data))
        return (WEdit *) h->current->data;
    return (WEdit *) find_widget_type (h, edit_callback);
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Check if widget is an WEdit class.
 *
 * @param w probably editor object
 * @return TRUE if widget is an WEdit class, FALSE otherwise
 */

gboolean
edit_widget_is_editor (const Widget * w)
{
    return (w != NULL && w->callback == edit_callback);
}

/* --------------------------------------------------------------------------------------------- */

void
edit_update_screen (WEdit * e)
{
    edit_scroll_screen_over_cursor (e);
    edit_update_curs_col (e);

    edit_status (e, (e->force & REDRAW_COMPLETELY) != 0 &&
                 (void *) e == ((Widget *) e)->owner->current->data);

    /* pop all events for this window for internal handling */
    if (!is_idle ())
        e->force |= REDRAW_PAGE;
    else
    {
        if ((e->force & REDRAW_COMPLETELY) != 0)
            e->force |= REDRAW_PAGE;
        edit_render_keypress (e);
    }

    buttonbar_redraw (find_buttonbar (((Widget *) e)->owner));
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Save current window size.
 *
 * @param edit editor object
 */

void
edit_save_size (WEdit * edit)
{
    edit->y_prev = edit->widget.y;
    edit->x_prev = edit->widget.x;
    edit->lines_prev = edit->widget.lines;
    edit->cols_prev = edit->widget.cols;
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Create new editor window and insert it into editor screen.
 *
 * @param h     editor dialog (screen)
 * @param y     y coordinate
 * @param x     x coordinate
 * @param lines window height
 * @param cols  window width
 * @param f     file object
 * @param fline line number in file
 * @return TRUE if new window was successfully created and inserted into editor screen,
 *         FALSE otherwise
 */

gboolean
edit_add_window (Dlg_head * h, int y, int x, int lines, int cols, const vfs_path_t * f, int fline)
{
    WEdit *edit;
    Widget *w;

    edit = edit_init (NULL, y, x, lines, cols, f, fline);
    if (edit == NULL)
        return FALSE;

    w = (Widget *) edit;
    w->callback = edit_callback;
    w->mouse = edit_event;
    widget_want_cursor (*w, TRUE);

    add_widget (h, w);
    dlg_redraw (h);

    return TRUE;
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Handle move/resize events.
 *
 * @param edit    editor object
 * @param command action id
 * @return TRUE if mouse actions was handled, FALSE otherwise
 */

gboolean
edit_handle_move_resize (WEdit * edit, unsigned long command)
{
    gboolean ret = FALSE;

    if (edit->fullscreen)
    {
        edit->drag_state = MCEDIT_DRAG_NORMAL;
        return ret;
    }

    switch (edit->drag_state)
    {
    case MCEDIT_DRAG_NORMAL:
        /* possible start move/resize */
        switch (command)
        {
        case CK_WindowMove:
            edit->drag_state = MCEDIT_DRAG_MOVE;
            edit_save_size (edit);
            ret = TRUE;
            break;
        case CK_WindowResize:
            edit->drag_state = MCEDIT_DRAG_RESIZE;
            edit_save_size (edit);
            ret = TRUE;
            break;
        default:
            break;
        }
        break;

    case MCEDIT_DRAG_MOVE:
        switch (command)
        {
        case CK_WindowResize:
            edit->drag_state = MCEDIT_DRAG_RESIZE;
            ret = TRUE;
            break;
        case CK_Up:
        case CK_Down:
        case CK_Left:
        case CK_Right:
            edit_window_move (edit, command);
            ret = TRUE;
            break;
        case CK_Enter:
        case CK_WindowMove:
            edit->drag_state = MCEDIT_DRAG_NORMAL;
            /* redraw frame and status */
            edit_status (edit, TRUE);
        default:
            ret = TRUE;
            break;
        }
        break;

    case MCEDIT_DRAG_RESIZE:
        switch (command)
        {
        case CK_WindowMove:
            edit->drag_state = MCEDIT_DRAG_MOVE;
            ret = TRUE;
            break;
        case CK_Up:
        case CK_Down:
        case CK_Left:
        case CK_Right:
            edit_window_resize (edit, command);
            ret = TRUE;
            break;
        case CK_Enter:
        case CK_WindowResize:
            edit->drag_state = MCEDIT_DRAG_NORMAL;
            /* redraw frame and status */
            edit_status (edit, TRUE);
        default:
            ret = TRUE;
            break;
        }
        break;
    }

    return ret;
}

/* --------------------------------------------------------------------------------------------- */
/**
 * Toggle window fuulscreen mode.
 *
 * @param edit editor object
 */

void
edit_toggle_fullscreen (WEdit * edit)
{
    Dlg_head *h = ((Widget *) edit)->owner;

    edit->fullscreen = !edit->fullscreen;
    edit->force = REDRAW_COMPLETELY;

    if (!edit->fullscreen)
        edit_restore_size (edit);
    else
    {
        edit_save_size (edit);
        widget_set_size ((Widget *) edit, h->y + 1, h->x, h->lines - 2, h->cols);
        edit->force |= REDRAW_PAGE;
        edit_update_screen (edit);
    }
}

/* --------------------------------------------------------------------------------------------- */
