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

#ifndef CFENGINE_LASTSEEN_H
#define CFENGINE_LASTSEEN_H

typedef struct
{
    time_t lastseen;
    QPoint Q;
} KeyHostSeen;

typedef enum
{
    LAST_SEEN_ROLE_CONNECT,
    LAST_SEEN_ROLE_ACCEPT
} LastSeenRole;

bool Address2Hostkey(const char *address, char *hostkey);
void LastSaw(char *ipaddress, unsigned char digest[EVP_MAX_MD_SIZE + 1], LastSeenRole role);
bool RemoveDigestFromLastSeen(const char *digest);
bool RemoveIPFromLastSeen(const char *ip);
void LastSaw(const char *ipaddress, unsigned char digest[EVP_MAX_MD_SIZE + 1], LastSeenRole role);
/*
 * Return false in order to stop iteration
 */
typedef bool (*LastSeenQualityCallback)(const char *hostkey, const char *address,
                                        bool incoming, const KeyHostSeen *quality,
                                        void *ctx);

bool ScanLastSeenQuality(LastSeenQualityCallback callback, void *ctx);
int LastSeenHostKeyCount(void);

#endif
