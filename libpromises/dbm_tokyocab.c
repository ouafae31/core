/*
   Copyright (C) CFEngine AS

   This file is part of CFEngine 3 - written and maintained by CFEngine AS.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; version 3.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of CFEngine, the applicable Commerical Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/

/*
 * Implementation using Tokyo Cabinet hash API.
 */

#include "cf3.defs.h"

#include "dbm_priv.h"
#include "logging.h"
#include "string_lib.h"

#ifdef TCDB

# include <tcutil.h>
# include <tchdb.h>

struct DBPriv_
{
    /*
     * This mutex prevents destructive modifications of the database (removing
     * records) while the cursor is active on it.
     */
    pthread_mutex_t cursor_lock;

    TCHDB *hdb;
};

struct DBCursorPriv_
{
    DBPriv *db;

    char *current_key;
    int current_key_size;
    char *curval;

    /*
     * Removing a key underneath the active cursor stops the database iteration,
     * so if key needs to be deleted while database is iterated, this fact is
     * remembered and once iterator advances to next key, this pending delete is
     * executed.
     *
     * Writes to key underneath the active cursor are safe, so only deletes are
     * tracked.
     */
    bool pending_delete;
};

/******************************************************************************/

static bool LockCursor(DBPriv *db)
{
    int ret = pthread_mutex_lock(&db->cursor_lock);
    if (ret != 0)
    {
        errno = ret;
        Log(LOG_LEVEL_ERR, "Unable to obtain cursor lock for Tokyo Cabinet database. (pthread_mutex_lock: %s)", GetErrorStr());
        return false;
    }
    return true;
}

static void UnlockCursor(DBPriv *db)
{
    int ret = pthread_mutex_unlock(&db->cursor_lock);
    if (ret != 0)
    {
        errno = ret;
        Log(LOG_LEVEL_ERR, "Unable to release cursor lock for Tokyo Cabinet database. (pthread_mutex_unlock: %s)",
            GetErrorStr());
    }
}

const char *DBPrivGetFileExtension(void)
{
    return "tcdb";
}

static const char *ErrorMessage(TCHDB *hdb)
{
    return tchdberrmsg(tchdbecode(hdb));
}

static bool OpenTokyoDatabase(const char *filename, TCHDB **hdb)
{
    *hdb = tchdbnew();

    if (!tchdbsetmutex(*hdb))
    {
        return false;
    }

    if (!tchdbopen(*hdb, filename, HDBOWRITER | HDBOCREAT))
    {
        return false;
    }

    if (!tchdboptimize(*hdb, -1, -1, -1, false))
    {
        tchdbclose(*hdb);
        return false;
    }

    return true;
}

DBPriv *DBPrivOpenDB(const char *dbpath)
{
    DBPriv *db = xcalloc(1, sizeof(DBPriv));

    pthread_mutex_init(&db->cursor_lock, NULL);

    if (!OpenTokyoDatabase(dbpath, &db->hdb))
    {
        Log(LOG_LEVEL_ERR, "Could not open database %s: %s",
              dbpath, ErrorMessage(db->hdb));

        int errcode = tchdbecode(db->hdb);

        if(errcode != TCEMETA && errcode != TCEREAD)
        {
            goto err;
        }

        tchdbdel(db->hdb);

        return DB_PRIV_DATABASE_BROKEN;
    }

    return db;

err:
    pthread_mutex_destroy(&db->cursor_lock);
    tchdbdel(db->hdb);
    free(db);
    return NULL;
}

void DBPrivCloseDB(DBPriv *db)
{
    int ret;

    if ((ret = pthread_mutex_destroy(&db->cursor_lock)) != 0)
    {
        errno = ret;
        Log(LOG_LEVEL_ERR, "Unable to destroy mutex during Tokyo Cabinet database handle close. (pthread_mutex_destroy: %s)",
            GetErrorStr());
    }

    if (!tchdbclose(db->hdb))
    {
        Log(LOG_LEVEL_ERR, "Closing database failed. (tchdbclose: %s)", ErrorMessage(db->hdb));
    }

    tchdbdel(db->hdb);
    free(db);
}

bool DBPrivHasKey(DBPriv *db, const void *key, int key_size)
{
    // FIXME: distinguish between "entry not found" and "error occured"

    return tchdbvsiz(db->hdb, key, key_size) != -1;
}

int DBPrivGetValueSize(DBPriv *db, const void *key, int key_size)
{
    return tchdbvsiz(db->hdb, key, key_size);
}

bool DBPrivRead(DBPriv *db, const void *key, int key_size, void *dest, int dest_size)
{
    if (tchdbget3(db->hdb, key, key_size, dest, dest_size) == -1)
    {
        if (tchdbecode(db->hdb) != TCENOREC)
        {
            Log(LOG_LEVEL_ERR, "ReadComplexKeyDB(%s): Could not read: %s", (const char *)key, ErrorMessage(db->hdb));
        }
        return false;
    }

    return true;
}

bool DBPrivReadInv(DBPriv *db, const char *key_list[], const char *value, int *iterator)
{   
    const  char *key;
    char dest[CF_BUFSIZE];
    tchdbiterinit(db->hdb);
    while((key=tchdbiternext2(db->hdb))!=NULL)
    {
        if (tchdbget3(db->hdb, key, strlen(key)+1, dest, sizeof(dest)) == -1)
        {
            if (tchdbecode(db->hdb) != TCENOREC)
            {  
                CfOut(OUTPUT_LEVEL_ERROR, "", "ReadComplexKeyDB(%s): Could not read: %s\n", (const char *)key, ErrorMessage(db->hdb));
            } 
            return false;
        }
        if(strcmp(value,dest)==0) 
            {
                key_list[*iterator]=xstrdup(key);
                (*iterator)++;
            }

            free(key);
    }

    return true;
}

static bool Write(TCHDB *hdb, const void *key, int key_size, const void *value, int value_size)
{
    if (!tchdbput(hdb, key, key_size, value, value_size))
    {
        Log(LOG_LEVEL_ERR, "tchdbput: Could not write key to DB \"%s\": %s",
              tchdbpath(hdb), ErrorMessage(hdb));
        return false;
    }
    return true;
}

static bool Delete(TCHDB *hdb, const void *key, int key_size)
{
    if (!tchdbout(hdb, key, key_size) && tchdbecode(hdb) != TCENOREC)
    {
        Log(LOG_LEVEL_ERR, "tchdbout: Could not delete key: %s",
              ErrorMessage(hdb));
        return false;
    }

    return true;
}

/*
 * This one has to be locked against cursor, or interaction between
 * write/pending delete might yield surprising results.
 */
bool DBPrivWrite(DBPriv *db, const void *key, int key_size, const void *value, int value_size)
{
    /* FIXME: get a cursor and see what is the current key */

    int ret = Write(db->hdb, key, key_size, value, value_size);

    return ret;
}

/*
 * This one has to be locked against cursor -- deleting entries might interrupt
 * iteration.
 */
bool DBPrivDelete(DBPriv *db, const void *key, int key_size)
{
    if (!LockCursor(db))
    {
        return false;
    }

    int ret = Delete(db->hdb, key, key_size);

    UnlockCursor(db);
    return ret;
}

DBCursorPriv *DBPrivOpenCursor(DBPriv *db)
{
    if (!LockCursor(db))
    {
        return false;
    }

    DBCursorPriv *cursor = xcalloc(1, sizeof(DBCursorPriv));
    cursor->db = db;

    /* Cursor remains locked */
    return cursor;
}

bool DBPrivAdvanceCursor(DBCursorPriv *cursor, void **key, int *key_size,
                     void **value, int *value_size)
{
    *key = tchdbgetnext3(cursor->db->hdb,
                         cursor->current_key, cursor->current_key_size,
                         key_size, (const char **)value, value_size);

    /*
     * If there is pending delete on the key, apply it
     */
    if (cursor->pending_delete)
    {
        Delete(cursor->db->hdb, cursor->current_key, cursor->current_key_size);
    }

    /* This will free the value as well: tchdbgetnext3 returns single allocated
     * chunk of memory */

    free(cursor->current_key);

    cursor->current_key = *key;
    cursor->current_key_size = *key_size;
    cursor->pending_delete = false;

    return *key != NULL;
}

bool DBPrivDeleteCursorEntry(DBCursorPriv *cursor)
{
    cursor->pending_delete = true;
    return true;
}

bool DBPrivWriteCursorEntry(DBCursorPriv *cursor, const void *value, int value_size)
{
    /*
     * If a pending deletion of entry has been requested, cancel it
     */
    cursor->pending_delete = false;

    return Write(cursor->db->hdb, cursor->current_key, cursor->current_key_size,
                 value, value_size);
}

void DBPrivCloseCursor(DBCursorPriv *cursor)
{
    DBPriv *db = cursor->db;

    if (cursor->pending_delete)
    {
        Delete(db->hdb, cursor->current_key, cursor->current_key_size);
    }

    free(cursor->current_key);
    free(cursor);

    /* Cursor lock was obtained in DBPrivOpenCursor */
    UnlockCursor(db);
}


char *DBPrivDiagnose(const char *dbpath)
{
#define SWAB64(num) \
    ( \
        ((num & 0x00000000000000ffULL) << 56) | \
        ((num & 0x000000000000ff00ULL) << 40) | \
        ((num & 0x0000000000ff0000ULL) << 24) | \
        ((num & 0x00000000ff000000ULL) << 8) | \
        ((num & 0x000000ff00000000ULL) >> 8) | \
        ((num & 0x0000ff0000000000ULL) >> 24) | \
        ((num & 0x00ff000000000000ULL) >> 40) | \
        ((num & 0xff00000000000000ULL) >> 56) \
    )

    static const char *MAGIC="ToKyO CaBiNeT";

    FILE *fp = fopen(dbpath, "r");
    if(!fp)
    {
        return StringFormat("Error opening file '%s': %s", dbpath, strerror(errno));
    }

    if(fseek(fp, 0, SEEK_END) != 0)
    {
        fclose(fp);
        return StringFormat("Error seeking to end: %s\n", strerror(errno));
    }

    uint64_t size = ftell(fp);
    if(size < 256)
    {
        fclose(fp);
        return StringFormat("Seek-to-end size less than minimum required: %zd", size);
    }

    char hbuf[256];
    memset(hbuf, 0, (size_t)256);

    if(fseek(fp, 0, SEEK_SET) != 0)
    {
        fclose(fp);
        return StringFormat("Error seeking to offset 256: %s", strerror(errno));
    }

    if(fread(&hbuf, 256, 1, fp) != 1)
    {
        fclose(fp);
        return StringFormat("Error reading 256 bytes: %s\n", strerror(errno));
    }

    if(strncmp(hbuf, MAGIC, strlen(MAGIC)) != 0)
    {
        fclose(fp);
        return StringFormat("Magic string mismatch");
    }

    uint64_t declared_size = 0;
    memcpy(&declared_size, hbuf+56, sizeof(uint64_t));
    if(declared_size == size)
    {
        return NULL; // all is well
    }
    else
    {
        declared_size = SWAB64(declared_size);
        if(declared_size == size)
        {
            fclose(fp);
            return StringFormat("Endianness mismatch, declared size SWAB64 '%zd' equals seek-to-end size '%zd'", declared_size, size);
        }
        else
        {
            fclose(fp);
            return StringFormat("Size mismatch, declared size SWAB64 '%zd', seek-to-end-size '%zd'", declared_size, size);
        }
    }
}

#endif
