/*  Audacious - Cross-platform multimedia player
 *  Copyright (C) 2005-2007  Audacious development team
 *
 *  Based on BMP:
 *  Copyright (C) 2003-2004  BMP development team.
 *
 *  Based on XMMS:
 *  Copyright (C) 1998-2003  XMMS development team.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; under version 3 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses>.
 *
 *  The Audacious team does not consider modular code linking to
 *  Audacious or using our public API to be a derived work.
 */

/* #define AUD_DEBUG 1 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "util.h"

#include <glib.h>
#include <glib/gi18n.h>
#include <gtk/gtk.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "platform/smartinclude.h"
#include <errno.h>

#ifdef HAVE_FTS_H
#  include <sys/types.h>
#  include <sys/stat.h>
#  include <fts.h>
#endif

#include "input.h"
#include "main.h"
#include "playback.h"
#include "strings.h"
#include "ui_playlist.h"

#ifdef USE_CHARDET
    #include "../libguess/libguess.h"
#ifdef HAVE_UDET
    #include <libudet_c.h>
#endif
#endif

/*
 * find <file> in directory <dirname> or subdirectories.  return
 * pointer to complete filename which has to be freed by calling
 * "g_free()" after use. Returns NULL if file could not be found.
 */

typedef struct {
    const gchar *to_match;
    gchar *match;
    gboolean found;
} FindFileContext;

static gboolean
find_file_func(const gchar * path, const gchar * basename, gpointer data)
{
    FindFileContext *context = data;

    if (strlen(path) > FILENAME_MAX) {
        g_warning("Ignoring path: name too long (%s)", path);
        return TRUE;
    }

    if (vfs_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        if (!strcasecmp(basename, context->to_match)) {
            context->match = g_strdup(path);
            context->found = TRUE;
            return TRUE;
        }
    }
    else if (vfs_file_test(path, G_FILE_TEST_IS_DIR)) {
        dir_foreach(path, find_file_func, context, NULL);
        if (context->found)
            return TRUE;
    }

    return FALSE;
}

gchar *
find_file_recursively(const gchar * path, const gchar * filename)
{
    FindFileContext context;
    gchar *out = NULL;

    context.to_match = filename;
    context.match = NULL;
    context.found = FALSE;

    dir_foreach(path, find_file_func, &context, NULL);

    if (context.match)
    {
        out = g_filename_to_uri(context.match, NULL, NULL);
        g_free(context.match);
    }

    return out;
}

gchar *
find_path_recursively(const gchar * path, const gchar * filename)
{
    FindFileContext context;

    context.to_match = filename;
    context.match = NULL;
    context.found = FALSE;

    dir_foreach(path, find_file_func, &context, NULL);

    return context.match;
}


typedef enum {
    ARCHIVE_UNKNOWN = 0,
    ARCHIVE_DIR,
    ARCHIVE_TAR,
    ARCHIVE_TGZ,
    ARCHIVE_ZIP,
    ARCHIVE_TBZ2
} ArchiveType;

typedef gchar *(*ArchiveExtractFunc) (const gchar *, const gchar *);

typedef struct {
    ArchiveType type;
    const gchar *ext;
} ArchiveExtensionType;

static ArchiveExtensionType archive_extensions[] = {
    {ARCHIVE_TAR, ".tar"},
    {ARCHIVE_ZIP, ".wsz"},
    {ARCHIVE_ZIP, ".zip"},
    {ARCHIVE_TGZ, ".tar.gz"},
    {ARCHIVE_TGZ, ".tgz"},
    {ARCHIVE_TBZ2, ".tar.bz2"},
    {ARCHIVE_TBZ2, ".bz2"},
    {ARCHIVE_UNKNOWN, NULL}
};

static gchar *archive_extract_tar(const gchar * archive, const gchar * dest);
static gchar *archive_extract_zip(const gchar * archive, const gchar * dest);
static gchar *archive_extract_tgz(const gchar * archive, const gchar * dest);
static gchar *archive_extract_tbz2(const gchar * archive, const gchar * dest);

static ArchiveExtractFunc archive_extract_funcs[] = {
    NULL,
    NULL,
    archive_extract_tar,
    archive_extract_tgz,
    archive_extract_zip,
    archive_extract_tbz2
};


/* FIXME: these functions can be generalised into a function using a
 * command lookup table */

static const gchar *
get_tar_command(void)
{
    static const gchar *command = NULL;

    if (!command) {
        if (!(command = getenv("TARCMD")))
            command = "tar";
    }

    return command;
}

static const gchar *
get_unzip_command(void)
{
    static const gchar *command = NULL;

    if (!command) {
        if (!(command = getenv("UNZIPCMD")))
            command = "unzip";
    }

    return command;
}


static gchar *
archive_extract_tar(const gchar * archive, const gchar * dest)
{
    return g_strdup_printf("%s >/dev/null xf \"%s\" -C %s",
                           get_tar_command(), archive, dest);
}

static gchar *
archive_extract_zip(const gchar * archive, const gchar * dest)
{
    return g_strdup_printf("%s >/dev/null -o -j \"%s\" -d %s",
                           get_unzip_command(), archive, dest);
}

static gchar *
archive_extract_tgz(const gchar * archive, const gchar * dest)
{
    return g_strdup_printf("%s >/dev/null xzf \"%s\" -C %s",
                           get_tar_command(), archive, dest);
}

static gchar *
archive_extract_tbz2(const gchar * archive, const gchar * dest)
{
    return g_strdup_printf("bzip2 -dc \"%s\" | %s >/dev/null xf - -C %s",
                           archive, get_tar_command(), dest);
}


ArchiveType
archive_get_type(const gchar * filename)
{
    gint i = 0;

    if (g_file_test(filename, G_FILE_TEST_IS_DIR))
        return ARCHIVE_DIR;

    while (archive_extensions[i].ext) {
        if (g_str_has_suffix(filename, archive_extensions[i].ext)) {
            return archive_extensions[i].type;
        }
        i++;
    }

    return ARCHIVE_UNKNOWN;
}

gboolean
file_is_archive(const gchar * filename)
{
    return (archive_get_type(filename) > ARCHIVE_DIR);
}

gchar *
archive_basename(const gchar * str)
{
    gint i = 0;

    while (archive_extensions[i].ext) {
        if (str_has_suffix_nocase(str, archive_extensions[i].ext)) {
            const gchar *end = g_strrstr(str, archive_extensions[i].ext);
            if (end) {
                return g_strndup(str, end - str);
            }
            break;
        }
        i++;
    }

    return NULL;
}

/*
   decompress_archive

   Decompresses the archive "filename" to a temporary directory,
   returns the path to the temp dir, or NULL if failed,
   watch out tho, doesn't actually check if the system command succeeds :-|
*/

gchar *
archive_decompress(const gchar * filename)
{
    gchar *tmpdir, *cmd, *escaped_filename;
    ArchiveType type;
#ifndef HAVE_MKDTEMP
    mode_t mode755 = S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH;
#endif

    if ((type = archive_get_type(filename)) <= ARCHIVE_DIR)
        return NULL;

#ifdef HAVE_MKDTEMP
    tmpdir = g_build_filename(g_get_tmp_dir(), "audacious.XXXXXXXX", NULL);
    if (!mkdtemp(tmpdir)) {
        g_free(tmpdir);
        g_message("Unable to load skin: Failed to create temporary "
                  "directory: %s", g_strerror(errno));
        return NULL;
    }
#else
    tmpdir = g_strdup_printf("%s/audacious.%ld", g_get_tmp_dir(), (long) rand());
    make_directory(tmpdir, mode755);
#endif

    escaped_filename = escape_shell_chars(filename);
    cmd = archive_extract_funcs[type] (escaped_filename, tmpdir);
    g_free(escaped_filename);

    if (!cmd) {
        g_message("extraction function is NULL!");
        g_free(tmpdir);
        return NULL;
    }

    if(system(cmd) == -1)
    {
        g_message("could not execute cmd %s",cmd);
        g_free(cmd);
        return NULL;
    }
    g_free(cmd);

    return tmpdir;
}


#ifdef HAVE_FTS_H

void
del_directory(const gchar * dirname)
{
    gchar *const argv[2] = { (gchar *) dirname, NULL };
    FTS *fts;
    FTSENT *p;

    fts = fts_open(argv, FTS_PHYSICAL, (gint(*)())NULL);
    while ((p = fts_read(fts))) {
        switch (p->fts_info) {
        case FTS_D:
            break;
        case FTS_DNR:
        case FTS_ERR:
            break;
        case FTS_DP:
            rmdir(p->fts_accpath);
            break;
        default:
            unlink(p->fts_accpath);
            break;
        }
    }
    fts_close(fts);
}

#else                           /* !HAVE_FTS */

gboolean
del_directory_func(const gchar * path, const gchar * basename,
                   gpointer params)
{
    if (!strcmp(basename, ".") || !strcmp(path, ".."))
        return FALSE;

    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
        dir_foreach(path, del_directory_func, NULL, NULL);
        rmdir(path);
        return FALSE;
    }

    unlink(path);

    return FALSE;
}

void
del_directory(const gchar * path)
{
    dir_foreach(path, del_directory_func, NULL, NULL);
    rmdir(path);
}

#endif                          /* ifdef HAVE_FTS */

static void
strip_string(GString *string)
{
    while (string->len > 0 && string->str[0] == ' ')
        g_string_erase(string, 0, 1);

    while (string->len > 0 && string->str[string->len - 1] == ' ')
        g_string_erase(string, string->len - 1, 1);
}

static void
strip_lower_string(GString *string)
{
    gchar *lower;
    strip_string(string);

    lower = g_ascii_strdown(string->str, -1);
    g_free(string->str);
    string->str = lower;
}

INIFile *
open_ini_file(const gchar *filename)
{
    GHashTable *ini_file = g_hash_table_new(NULL, NULL);
    GHashTable *section = g_hash_table_new(NULL, NULL);
    GString *section_name, *key_name, *value;
    gpointer section_hash, key_hash;
    gchar *buffer = NULL;
    gsize off = 0;
    gsize filesize = 0;

    unsigned char x[] = { 0xff, 0xfe, 0x00 };


    g_return_val_if_fail(filename, NULL);

    section_name = g_string_new("");
    key_name = g_string_new(NULL);
    value = g_string_new(NULL);

    /* make a nameless section which should store all entries that are not
     * embedded in a section */
    section_hash = GINT_TO_POINTER(g_string_hash(section_name));
    g_hash_table_insert(ini_file, section_hash, section);

    vfs_file_get_contents(filename, &buffer, &filesize);
    if (buffer == NULL)
        return NULL;


    /*
     * Convert UTF-16 into something useful. Original implementation
     * by incomp@#audacious. Cleanups \nenolod
     * FIXME: can't we use a GLib function for that? -- 01mf02
     */
    if (filesize > 2 && !memcmp(&buffer[0],&x,2))
    {
        gchar *outbuf = g_malloc (filesize);   /* it's safe to waste memory. */
        guint counter;

        for (counter = 2; counter < filesize; counter += 2)
        {
            if (!memcmp(&buffer[counter+1], &x[2], 1))
                outbuf[(counter-2)/2] = buffer[counter];
            else
                return NULL;
        }

        outbuf[(counter-2)/2] = '\0';

        if ((filesize - 2) / 2 == (counter - 2) / 2)
        {
            g_free(buffer);
            buffer = outbuf;
        }
        else
        {
            g_free(outbuf);
            return NULL;    /* XXX wrong encoding */
        }
    }

    while (off < filesize)
    {
        /* ignore the following characters */
        if (buffer[off] == '\r' || buffer[off] == '\n' ||
            buffer[off] == ' '  || buffer[off] == '\t')
        {
            if (buffer[off] == '\n')
            {
                g_string_free(key_name, TRUE);
                g_string_free(value, TRUE);
                key_name = g_string_new(NULL);
                value = g_string_new(NULL);
            }

            off++;
            continue;
        }

        /* if we encounter a possible section statement */
        if (buffer[off] == '[')
        {
            g_string_free(section_name, TRUE);
            section_name = g_string_new(NULL);
            off++;

            if (off >= filesize)
                goto return_sequence;

            while (buffer[off] != ']')
            {
                /* if the section statement has not been closed before a
                 * linebreak */
                if (buffer[off] == '\n')
                    break;

                g_string_append_c(section_name, buffer[off]);
                off++;
                if (off >= filesize)
                    goto return_sequence;
            }
            if (buffer[off] == '\n')
                continue;
            if (buffer[off] == ']')
            {
                off++;
                if (off >= filesize)
                    goto return_sequence;

                strip_lower_string(section_name);
                section_hash = GINT_TO_POINTER(g_string_hash(section_name));

                /* if this section already exists, we don't make a new one,
                 * but reuse the old one */
                if (g_hash_table_lookup(ini_file, section_hash) != NULL)
                    section = g_hash_table_lookup(ini_file, section_hash);
                else
                {
                    section = g_hash_table_new(NULL, NULL);
                    g_hash_table_insert(ini_file, section_hash, section);
                }

                continue;
            }
        }

        if (buffer[off] == '=')
        {
            off++;
            if (off >= filesize)
                goto return_sequence;

            while (buffer[off] != '\n' && buffer[off] != '\r')
            {
                g_string_append_c(value, buffer[off]);
                off++;
                if (off >= filesize)
                    break;
            }

            strip_lower_string(key_name);
            key_hash = GINT_TO_POINTER(g_string_hash(key_name));
            strip_string(value);

            if (key_name->len > 0 && value->len > 0)
                g_hash_table_insert(section, key_hash, g_strdup(value->str));
        }
        else
        {
            g_string_append_c(key_name, buffer[off]);
            off++;
            if (off >= filesize)
                goto return_sequence;
        }
    }

return_sequence:
    g_string_free(section_name, TRUE);
    g_string_free(key_name, TRUE);
    g_string_free(value, TRUE);
    g_free(buffer);
    return ini_file;
}

void
close_ini_file(INIFile *inifile)
{
    g_return_if_fail(inifile);

    /* we don't have to destroy anything in the hash table manually, as the
     * keys are represented as integers and the string values may be used in
     * functions which have read the strings from the hash table
     */
    g_hash_table_destroy(inifile);
}

gchar *
read_ini_string(INIFile *inifile, const gchar *section, const gchar *key)
{
    GString *section_string;
    GString *key_string;
    gchar *value = NULL;
    gpointer section_hash, key_hash;
    GHashTable *section_table;
    
    g_return_val_if_fail(inifile, NULL);

    section_string = g_string_new(section);
    key_string = g_string_new(key);
    value = NULL;

    strip_lower_string(section_string);
    strip_lower_string(key_string);
    section_hash = GINT_TO_POINTER(g_string_hash(section_string));
    key_hash = GINT_TO_POINTER(g_string_hash(key_string));
    g_string_free(section_string, FALSE);
    g_string_free(key_string, FALSE);

    section_table = g_hash_table_lookup(inifile, section_hash);
    g_return_val_if_fail(section_table, NULL);

    value = g_hash_table_lookup(section_table, GINT_TO_POINTER(key_hash));
    return value;
}

GArray *
read_ini_array(INIFile *inifile, const gchar *section, const gchar *key)
{
    gchar *temp;
    GArray *a;

    g_return_val_if_fail((temp = read_ini_string(inifile, section, key)), NULL);

    a = string_to_garray(temp);
    g_free(temp);
    return a;
}

GArray *
string_to_garray(const gchar * str)
{
    GArray *array;
    gint temp;
    const gchar *ptr = str;
    gchar *endptr;

    array = g_array_new(FALSE, TRUE, sizeof(gint));
    for (;;) {
        temp = strtol(ptr, &endptr, 10);
        if (ptr == endptr)
            break;
        g_array_append_val(array, temp);
        ptr = endptr;
        while (!isdigit((int) *ptr) && (*ptr) != '\0')
            ptr++;
        if (*ptr == '\0')
            break;
    }
    return (array);
}

void
glist_movedown(GList * list)
{
    gpointer temp;

    if (g_list_next(list)) {
        temp = list->data;
        list->data = list->next->data;
        list->next->data = temp;
    }
}

void
glist_moveup(GList * list)
{
    gpointer temp;

    if (g_list_previous(list)) {
        temp = list->data;
        list->data = list->prev->data;
        list->prev->data = temp;
    }
}


void
util_menu_position(GtkMenu * menu, gint * x, gint * y,
                   gboolean * push_in, gpointer data)
{
    GtkRequisition requisition;
    gint screen_width;
    gint screen_height;
    MenuPos *pos = data;

    gtk_widget_size_request(GTK_WIDGET(menu), &requisition);

    screen_width = gdk_screen_width();
    screen_height = gdk_screen_height();

    *x = CLAMP(pos->x - 2, 0, MAX(0, screen_width - requisition.width));
    *y = CLAMP(pos->y - 2, 0, MAX(0, screen_height - requisition.height));
}

GdkFont *
util_font_load(const gchar * name)
{
    GdkFont *font;
    PangoFontDescription *desc;

    desc = pango_font_description_from_string(name);
    font = gdk_font_from_description(desc);

    return font;
}

/* text_get_extents() taken from The GIMP (C) Spencer Kimball, Peter
 * Mattis et al */
gboolean
text_get_extents(const gchar * fontname,
                 const gchar * text,
                 gint * width, gint * height, gint * ascent, gint * descent)
{
    PangoFontDescription *font_desc;
    PangoLayout *layout;
    PangoRectangle rect;

    g_return_val_if_fail(fontname != NULL, FALSE);
    g_return_val_if_fail(text != NULL, FALSE);

    /* FIXME: resolution */
    layout = gtk_widget_create_pango_layout(GTK_WIDGET(mainwin), text);

    font_desc = pango_font_description_from_string(fontname);
    pango_layout_set_font_description(layout, font_desc);
    pango_font_description_free(font_desc);
    pango_layout_get_pixel_extents(layout, NULL, &rect);

    if (width)
        *width = rect.width;
    if (height)
        *height = rect.height;

    if (ascent || descent) {
        PangoLayoutIter *iter;
        PangoLayoutLine *line;

        iter = pango_layout_get_iter(layout);
        line = pango_layout_iter_get_line(iter);
        pango_layout_iter_free(iter);

        pango_layout_line_get_pixel_extents(line, NULL, &rect);

        if (ascent)
            *ascent = PANGO_ASCENT(rect);
        if (descent)
            *descent = -PANGO_DESCENT(rect);
    }

    g_object_unref(layout);

    return TRUE;
}

/* counts number of digits in a gint */
guint
gint_count_digits(gint n)
{
    guint count = 0;

    n = ABS(n);
    do {
        count++;
        n /= 10;
    } while (n > 0);

    return count;
}

gboolean
dir_foreach(const gchar * path, DirForeachFunc function,
            gpointer user_data, GError ** error)
{
    GError *error_out = NULL;
    GDir *dir;
    const gchar *entry;
    gchar *entry_fullpath;

    if (!(dir = g_dir_open(path, 0, &error_out))) {
        g_propagate_error(error, error_out);
        return FALSE;
    }

    while ((entry = g_dir_read_name(dir))) {
        entry_fullpath = g_build_filename(path, entry, NULL);

        if ((*function) (entry_fullpath, entry, user_data)) {
            g_free(entry_fullpath);
            break;
        }

        g_free(entry_fullpath);
    }

    g_dir_close(dir);

    return TRUE;
}

GtkWidget *
make_filebrowser(const gchar *title, gboolean save)
{
    GtkWidget *dialog;
    GtkWidget *button;

    g_return_val_if_fail(title != NULL, NULL);

    dialog = gtk_file_chooser_dialog_new(title, GTK_WINDOW(mainwin),
                                         save ?
                                         GTK_FILE_CHOOSER_ACTION_SAVE :
                                         GTK_FILE_CHOOSER_ACTION_OPEN,
                                         NULL, NULL);

    button = gtk_dialog_add_button(GTK_DIALOG(dialog), GTK_STOCK_CANCEL,
                                   GTK_RESPONSE_REJECT);

    gtk_button_set_use_stock(GTK_BUTTON(button), TRUE);
    GTK_WIDGET_SET_FLAGS(button, GTK_CAN_DEFAULT);

    button = gtk_dialog_add_button(GTK_DIALOG(dialog), save ?
                                   GTK_STOCK_SAVE : GTK_STOCK_OPEN,
                                   GTK_RESPONSE_ACCEPT);

    gtk_button_set_use_stock(GTK_BUTTON(button), TRUE);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    gtk_window_set_position(GTK_WINDOW(dialog), GTK_WIN_POS_CENTER); /* centering */

    return dialog;
}

/*
 * Resizes a GDK pixmap.
 */
GdkPixmap *audacious_pixmap_resize(GdkWindow *src, GdkGC *src_gc, GdkPixmap *in, gint width, gint height)
{
    GdkPixmap *out;
    gint owidth, oheight;

    g_return_val_if_fail(src != NULL, NULL);
    g_return_val_if_fail(src_gc != NULL, NULL);
    g_return_val_if_fail(in != NULL, NULL);
    g_return_val_if_fail(width > 0 && height > 0, NULL);

    gdk_drawable_get_size(in, &owidth, &oheight);

    if (oheight == height && owidth == width)
        return NULL;

    out = gdk_pixmap_new(src, width, height, -1);

    gdk_draw_rectangle(out, src_gc, TRUE, 0, 0, width, height);

    gdk_window_copy_area(out, src_gc, 0, 0, in, 0, 0, owidth, oheight);
    g_object_unref(src);

    return out;
}

GdkPixmap *create_dblsize_pixmap(GdkPixmap *pix) {
    int w, h;
    gdk_drawable_get_size(pix, &w, &h);
    GdkGC* gc = gdk_gc_new(pix);
    GdkPixbuf *img, *img2x;
    GdkColormap *colormap = gdk_colormap_get_system();
    img = gdk_pixbuf_new(GDK_COLORSPACE_RGB, FALSE, 8, w, h);
    gdk_pixbuf_get_from_drawable(img, pix, colormap, 0, 0, 0, 0, w, h);
    img2x = gdk_pixbuf_scale_simple(img, w*2, h*2, GDK_INTERP_NEAREST);

    GdkPixmap *image;
    image = gdk_pixmap_new(NULL, w*2, h*2, gdk_rgb_get_visual()->depth);
    gdk_draw_pixbuf(image, gc, img2x, 0, 0, 0, 0, w*2, h*2, GDK_RGB_DITHER_NONE, 0, 0);
    g_object_unref(img);
    g_object_unref(img2x);
    g_object_unref(gc);
    return image;
}

/**
 * xmms_show_message:
 * @title: The title of the message to show.
 * @text: The text of the message to show.
 * @button_text: The text of the button which will close the messagebox.
 * @modal: Whether or not the messagebox should be modal.
 * @button_action: Code to execute on when the messagebox is closed, or %NULL.
 * @action_data: Optional opaque data to pass to @button_action.
 *
 * Displays a message box.
 *
 * Return value: A GTK widget handle for the message box.
 **/
GtkWidget *
util_info_dialog(const gchar * title, const gchar * text,
                 const gchar * button_text, gboolean modal,
                 GCallback button_action, gpointer action_data)
{
  GtkWidget *dialog;
  GtkWidget *dialog_vbox, *dialog_hbox, *dialog_bbox;
  GtkWidget *dialog_bbox_b1;
  GtkWidget *dialog_textlabel;
  GtkWidget *dialog_icon;

  dialog = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_type_hint( GTK_WINDOW(dialog) , GDK_WINDOW_TYPE_HINT_DIALOG );
  gtk_window_set_modal( GTK_WINDOW(dialog) , modal );
  gtk_window_set_title( GTK_WINDOW(dialog) , title );
  gtk_container_set_border_width( GTK_CONTAINER(dialog) , 10 );

  dialog_vbox = gtk_vbox_new( FALSE , 0 );
  dialog_hbox = gtk_hbox_new( FALSE , 0 );

  /* icon */
  dialog_icon = gtk_image_new_from_stock( GTK_STOCK_DIALOG_INFO , GTK_ICON_SIZE_DIALOG );
  gtk_box_pack_start( GTK_BOX(dialog_hbox) , dialog_icon , FALSE , FALSE , 2 );

  /* label */
  dialog_textlabel = gtk_label_new( text );
  /* gtk_label_set_selectable( GTK_LABEL(dialog_textlabel) , TRUE ); */
  gtk_box_pack_start( GTK_BOX(dialog_hbox) , dialog_textlabel , TRUE , TRUE , 2 );

  gtk_box_pack_start( GTK_BOX(dialog_vbox) , dialog_hbox , FALSE , FALSE , 2 );
  gtk_box_pack_start( GTK_BOX(dialog_vbox) , gtk_hseparator_new() , FALSE , FALSE , 4 );

  dialog_bbox = gtk_hbutton_box_new();
  gtk_button_box_set_layout( GTK_BUTTON_BOX(dialog_bbox) , GTK_BUTTONBOX_END );
  dialog_bbox_b1 = gtk_button_new_with_label( button_text );
  g_signal_connect_swapped( G_OBJECT(dialog_bbox_b1) , "clicked" ,
                            G_CALLBACK(gtk_widget_destroy) , dialog );
  if ( button_action )
    g_signal_connect( G_OBJECT(dialog_bbox_b1) , "clicked" ,
                      button_action , action_data );

  gtk_container_add( GTK_CONTAINER(dialog_bbox) , dialog_bbox_b1 );
  gtk_box_pack_start( GTK_BOX(dialog_vbox) , dialog_bbox , FALSE , FALSE , 0 );

  gtk_container_add( GTK_CONTAINER(dialog) , dialog_vbox );

  GTK_WIDGET_SET_FLAGS( dialog_bbox_b1 , GTK_CAN_DEFAULT);
  gtk_widget_grab_default( dialog_bbox_b1 );

  gtk_widget_show_all(dialog);

  return dialog;
}


/**
 * util_get_localdir:
 *
 * Returns a string with the full path of Audacious local datadir (where config files are placed).
 * It's useful in order to put in the right place custom config files for audacious plugins.
 *
 * Return value: a string with full path of Audacious local datadir (should be freed after use)
 **/
gchar*
util_get_localdir(void)
{
  gchar *datadir;
  gchar *tmp;

  if ( (tmp = getenv("XDG_CONFIG_HOME")) == NULL )
    datadir = g_build_filename( g_get_home_dir() , ".config" , "audacious" ,  NULL );
  else
    datadir = g_build_filename( tmp , "audacious" , NULL );

  return datadir;
}


gchar *
construct_uri(gchar *string, const gchar *playlist_name) // uri, path and anything else
{
    gchar *filename = g_strdup(string);
    gchar *tmp, *path;
    gchar *uri = NULL;

    /* try to translate dos path */
    convert_dos_path(filename); /* in place replacement */

    /* convert backslash to slash */
    while ((tmp = strchr(filename, '\\')) != NULL)
        *tmp = '/';

    // make full path uri here
    // case 1: filename is raw full path or uri
    if (filename[0] == '/' || strstr(filename, "://")) {
        uri = g_filename_to_uri(filename, NULL, NULL);
        if(!uri) {
            uri = g_strdup(filename);
        }
        g_free(filename);
    }
    // case 2: filename is not raw full path nor uri, playlist path is full path
    // make full path by replacing last part of playlist path with filename. (using g_build_filename)
    else if (playlist_name[0] == '/' || strstr(playlist_name, "://")) {
        path = g_strdup(playlist_name);
        tmp = strrchr(path, '/'); *tmp = '\0';
        tmp = g_build_filename(path, filename, NULL);
        g_free(path); g_free(filename);
        uri = g_filename_to_uri(tmp, NULL, NULL);
        g_free(tmp);
    }
    // case 3: filename is not raw full path nor uri, playlist path is not full path
    // just abort.
    else {
        g_free(filename);
        return NULL;
    }

    AUDDBG("uri=%s\n", uri);
    return uri;
}
