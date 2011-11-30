/*
 * Audacious
 * Copyright (c) 2006-2011 Audacious team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; under version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses>.
 *
 * The Audacious team does not consider modular code linking to
 * Audacious or using our public API to be a derived work.
 */
/**
 * @file tuple.c
 * @brief Basic Tuple handling API.
 */

#include <glib.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include <audacious/i18n.h>

#include "audstrings.h"
#include "config.h"
#include "strpool.h"
#include "tuple.h"

typedef struct {
    gchar *name;
    TupleValueType type;
} TupleBasicType;

/**
 * Structure for holding and passing around miscellaneous track
 * metadata. This is not the same as a playlist entry, though.
 */
struct _Tuple {
    gint refcount;
    gint64 setmask;

    union {
        gint x;
        gchar * s;
    } vals[TUPLE_FIELDS];

    gint nsubtunes;                 /**< Number of subtunes, if any. Values greater than 0
                                         mean that there are subtunes and #subtunes array
                                         may be set. */
    gint *subtunes;                 /**< Array of gint containing subtune index numbers.
                                         Can be NULL if indexing is linear or if
                                         there are no subtunes. */
};

#define BIT(i) ((gint64) 1 << (i))

/** Ordered table of basic #Tuple field names and their #TupleValueType.
 */
static const TupleBasicType tuple_fields[TUPLE_FIELDS] = {
    { "artist",         TUPLE_STRING },
    { "title",          TUPLE_STRING },
    { "album",          TUPLE_STRING },
    { "comment",        TUPLE_STRING },
    { "genre",          TUPLE_STRING },

    { "track-number",   TUPLE_INT },
    { "length",         TUPLE_INT },
    { "year",           TUPLE_INT },
    { "quality",        TUPLE_STRING },

    { "codec",          TUPLE_STRING },
    { "file-name",      TUPLE_STRING },
    { "file-path",      TUPLE_STRING },
    { "file-ext",       TUPLE_STRING },

    { "song-artist",    TUPLE_STRING },
    { "composer",       TUPLE_STRING },
    { "performer",      TUPLE_STRING },
    { "copyright",      TUPLE_STRING },
    { "date",           TUPLE_STRING },

    { "subsong-id",     TUPLE_INT },
    { "subsong-num",    TUPLE_INT },
    { "mime-type",      TUPLE_STRING },
    { "bitrate",        TUPLE_INT },

    { "segment-start",  TUPLE_INT },
    { "segment-end",    TUPLE_INT },

    { "gain-album-gain", TUPLE_INT },
    { "gain-album-peak", TUPLE_INT },
    { "gain-track-gain", TUPLE_INT },
    { "gain-track-peak", TUPLE_INT },
    { "gain-gain-unit", TUPLE_INT },
    { "gain-peak-unit", TUPLE_INT },
};

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;


gint tuple_field_by_name (const gchar * name)
{
    if (! name)
        return -1;

    for (gint i = 0; i < TUPLE_FIELDS; i ++)
    {
        if (! strcmp (tuple_fields[i].name, name))
            return i;
    }

    fprintf (stderr, "Unknown tuple field name \"%s\".\n", name);
    return -1;
}

const gchar * tuple_field_get_name (gint field)
{
    if (field < 0 || field >= TUPLE_FIELDS)
        return NULL;

    return tuple_fields[field].name;
}

TupleValueType tuple_field_get_type (gint field)
{
    if (field < 0 || field >= TUPLE_FIELDS)
        return TUPLE_UNKNOWN;

    return tuple_fields[field].type;
}

static void tuple_destroy_unlocked (Tuple * tuple)
{
    gint i;
    gint64 bit = 1;

    for (i = 0; i < TUPLE_FIELDS; i++)
    {
        if ((tuple->setmask & bit) && tuple_fields[i].type == TUPLE_STRING)
            str_unref (tuple->vals[i].s);

        bit <<= 1;
    }

    g_free(tuple->subtunes);

    memset (tuple, 0, sizeof (Tuple));
    g_slice_free (Tuple, tuple);
}

Tuple * tuple_new (void)
{
    Tuple * tuple = g_slice_new0 (Tuple);
    tuple->refcount = 1;
    return tuple;
}

Tuple * tuple_ref (Tuple * tuple)
{
    pthread_mutex_lock (& mutex);

    tuple->refcount ++;

    pthread_mutex_unlock (& mutex);
    return tuple;
}

void tuple_unref (Tuple * tuple)
{
    pthread_mutex_lock (& mutex);

    if (! -- tuple->refcount)
        tuple_destroy_unlocked (tuple);

    pthread_mutex_unlock (& mutex);
}

/**
 * Sets filename/URI related fields of a #Tuple structure, based
 * on the given filename argument. The fields set are:
 * #FIELD_FILE_PATH, #FIELD_FILE_NAME and #FIELD_FILE_EXT.
 *
 * @param[in] filename Filename URI.
 * @param[in,out] tuple Tuple structure to manipulate.
 */
void tuple_set_filename (Tuple * tuple, const gchar * name)
{
    const gchar * slash;
    gchar * temp, * c;

    if ((slash = strrchr (name, '/')))
    {
        gchar path[slash - name + 2];
        memcpy (path, name, slash - name + 1);
        path[slash - name + 1] = 0;

        temp = uri_to_display (path);
        tuple_copy_str (tuple, FIELD_FILE_PATH, NULL, temp);
        g_free (temp);

        name = slash + 1;
    }

    gchar buf[strlen (name) + 1];
    strcpy (buf, name);

    if ((c = strrchr (buf, '?')))
    {
        gint sub;
        gchar junk;

        if (sscanf (c + 1, "%d%c", & sub, & junk) == 1)
        {
            tuple_set_int (tuple, FIELD_SUBSONG_ID, NULL, sub);
            * c = 0;
        }
    }

    temp = uri_to_display (buf);

    if ((c = strrchr (temp, '.')))
        tuple_copy_str (tuple, FIELD_FILE_EXT, NULL, c + 1);

    tuple_copy_str (tuple, FIELD_FILE_NAME, NULL, temp);
    g_free (temp);
}

/**
 * Creates a copy of given Tuple structure, with copied data.
 *
 * @param[in] src Tuple structure to be made a copy of.
 * @return Pointer to newly allocated Tuple.
 */
Tuple *
tuple_copy(const Tuple *src)
{
    Tuple *dst;
    gint i;

    pthread_mutex_lock (& mutex);

    dst = g_memdup (src, sizeof (Tuple));

    gint64 bit = 1;

    for (i = 0; i < TUPLE_FIELDS; i++)
    {
        if ((dst->setmask & bit) && tuple_fields[i].type == TUPLE_STRING)
            str_ref (dst->vals[i].s);

        bit <<= 1;
    }

    if (dst->subtunes)
        dst->subtunes = g_memdup (dst->subtunes, sizeof (gint) * dst->nsubtunes);

    pthread_mutex_unlock (& mutex);
    return dst;
}

/**
 * Allocates a new #Tuple structure, setting filename/URI related
 * fields based on the given filename argument by calling #tuple_set_filename.
 *
 * @param[in] filename Filename URI.
 * @return Pointer to newly allocated Tuple.
 */
Tuple *
tuple_new_from_filename(const gchar *filename)
{
    Tuple *tuple = tuple_new();

    tuple_set_filename(tuple, filename);
    return tuple;
}

void tuple_set_int (Tuple * tuple, gint nfield, const gchar * field, gint x)
{
    if (nfield < 0)
        nfield = tuple_field_by_name (field);
    if (nfield < 0 || nfield >= TUPLE_FIELDS || tuple_fields[nfield].type != TUPLE_INT)
        return;

    pthread_mutex_lock (& mutex);

    tuple->setmask |= BIT (nfield);
    tuple->vals[nfield].x = x;

    pthread_mutex_unlock (& mutex);
}

void tuple_set_str (Tuple * tuple, gint nfield, const gchar * field, gchar * str)
{
    if (nfield < 0)
        nfield = tuple_field_by_name (field);
    if (nfield < 0 || nfield >= TUPLE_FIELDS || tuple_fields[nfield].type != TUPLE_STRING)
        return;

    STR_CHECK (str);

    pthread_mutex_lock (& mutex);

    if ((tuple->setmask & BIT (nfield)))
        str_unref (tuple->vals[nfield].s);

    tuple->setmask |= BIT (nfield);
    tuple->vals[nfield].s = str;

    pthread_mutex_unlock (& mutex);
}

void tuple_copy_str (Tuple * tuple, gint nfield, const gchar * field, const gchar * str)
{
    tuple_set_str (tuple, nfield, field, str_get (str));
}

void tuple_unset (Tuple * tuple, gint nfield, const gchar * field)
{
    if (nfield < 0)
        nfield = tuple_field_by_name (field);
    if (nfield < 0 || nfield >= TUPLE_FIELDS || tuple_fields[nfield].type != TUPLE_STRING)
        return;

    pthread_mutex_lock (& mutex);

    if ((tuple->setmask & BIT (nfield)))
    {
        tuple->setmask &= ~BIT (nfield);

        if (tuple_fields[nfield].type == TUPLE_STRING)
        {
            str_unref (tuple->vals[nfield].s);
            tuple->vals[nfield].s = NULL;
        }
        else /* TUPLE_INT */
            tuple->vals[nfield].x = 0;
    }

    pthread_mutex_unlock (& mutex);
}

/**
 * Returns #TupleValueType of given #Tuple field.
 * Desired field can be specified either by key name or if it is
 * one of basic fields, by #TupleBasicType index.
 *
 * @param[in] tuple #Tuple structure pointer.
 * @param[in] cnfield #TupleBasicType index or -1 if key name is to be used instead.
 * @param[in] field String acting as key name or NULL if nfield is used.
 * @return #TupleValueType of the field or TUPLE_UNKNOWN if there was an error.
 */
TupleValueType tuple_get_value_type (const Tuple * tuple, gint nfield, const gchar * field)
{
    if (nfield < 0)
        nfield = tuple_field_by_name (field);
    if (nfield < 0 || nfield >= TUPLE_FIELDS)
        return TUPLE_UNKNOWN;

    pthread_mutex_lock (& mutex);

    TupleValueType type = TUPLE_UNKNOWN;
    if ((tuple->setmask & BIT (nfield)))
        type = tuple_fields[nfield].type;

    pthread_mutex_unlock (& mutex);
    return type;
}

gchar * tuple_get_str (const Tuple * tuple, gint nfield, const gchar * field)
{
    if (nfield < 0)
        nfield = tuple_field_by_name (field);
    if (nfield < 0 || nfield >= TUPLE_FIELDS || tuple_fields[nfield].type != TUPLE_STRING)
        return NULL;

    pthread_mutex_lock (& mutex);

    gchar * str = str_ref (tuple->vals[nfield].s);

    pthread_mutex_unlock (& mutex);
    return str;
}

/**
 * Returns integer associated to #Tuple field.
 * Desired field can be specified either by key name or if it is
 * one of basic fields, by #TupleBasicType index.
 *
 * @param[in] tuple #Tuple structure pointer.
 * @param[in] cnfield #TupleBasicType index or -1 if key name is to be used instead.
 * @param[in] field String acting as key name or NULL if nfield is used.
 * @return Integer value or 0 if the field/key did not exist.
 *
 * @bug There is no way to distinguish error situations if the associated value is zero.
 */
gint tuple_get_int (const Tuple * tuple, gint nfield, const gchar * field)
{
    if (nfield < 0)
        nfield = tuple_field_by_name (field);
    if (nfield < 0 || nfield >= TUPLE_FIELDS || tuple_fields[nfield].type != TUPLE_INT)
        return 0;

    pthread_mutex_lock (& mutex);

    gint x = tuple->vals[nfield].x;

    pthread_mutex_unlock (& mutex);
    return x;
}

#define APPEND(b, ...) snprintf (b + strlen (b), sizeof b - strlen (b), \
 __VA_ARGS__)

void tuple_set_format (Tuple * t, const gchar * format, gint chans, gint rate,
 gint brate)
{
    if (format)
        tuple_copy_str (t, FIELD_CODEC, NULL, format);

    gchar buf[32];
    buf[0] = 0;

    if (chans > 0)
    {
        if (chans == 1)
            APPEND (buf, _("Mono"));
        else if (chans == 2)
            APPEND (buf, _("Stereo"));
        else
            APPEND (buf, dngettext (PACKAGE, "%d channel", "%d channels",
             chans), chans);

        if (rate > 0)
            APPEND (buf, ", ");
    }

    if (rate > 0)
        APPEND (buf, "%d kHz", rate / 1000);

    if (buf[0])
        tuple_copy_str (t, FIELD_QUALITY, NULL, buf);

    if (brate > 0)
        tuple_set_int (t, FIELD_BITRATE, NULL, brate);
}

void tuple_set_subtunes (Tuple * tuple, gint n_subtunes, const gint * subtunes)
{
    pthread_mutex_lock (& mutex);

    g_free (tuple->subtunes);
    tuple->subtunes = NULL;

    tuple->nsubtunes = n_subtunes;
    if (subtunes)
        tuple->subtunes = g_memdup (subtunes, sizeof (gint) * n_subtunes);

    pthread_mutex_unlock (& mutex);
}

gint tuple_get_n_subtunes (Tuple * tuple)
{
    pthread_mutex_lock (& mutex);

    gint n_subtunes = tuple->nsubtunes;

    pthread_mutex_unlock (& mutex);
    return n_subtunes;
}

gint tuple_get_nth_subtune (Tuple * tuple, gint n)
{
    pthread_mutex_lock (& mutex);

    gint subtune = -1;
    if (n >= 0 && n < tuple->nsubtunes)
        subtune = tuple->subtunes ? tuple->subtunes[n] : 1 + n;

    pthread_mutex_unlock (& mutex);
    return subtune;
}

/* deprecated */
void tuple_free (Tuple * tuple)
{
    tuple_unref (tuple);
}

/* deprecated */
gboolean tuple_associate_string (Tuple * tuple, const gint nfield,
 const gchar * field, const gchar * str)
{
    tuple_copy_str (tuple, nfield, field, str);
    return TRUE;
}

gboolean tuple_associate_string_rel (Tuple * tuple, gint nfield,
 const gchar * field, gchar * str)
{
    tuple_copy_str (tuple, nfield, field, str);
    g_free (str);
    return TRUE;
}

gboolean tuple_associate_int (Tuple * tuple, gint nfield, const gchar * field, gint x)
{
    tuple_set_int (tuple, nfield, field, x);
    return TRUE;
}

void tuple_disassociate (Tuple * tuple, const gint nfield, const gchar * field)
{
    tuple_unset (tuple, nfield, field);
}

const gchar * tuple_get_string (const Tuple * tuple, gint nfield, const gchar * field)
{
    gchar * str = tuple_get_str (tuple, nfield, field);
    str_unref (str);
    return str;
}
