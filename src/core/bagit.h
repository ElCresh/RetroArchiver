/* bagit - BagIt 1.0 bags (RFC 8493) with SHA-256 manifests, following
 * the profile docs/bagit-profile.json (boxes closed before mixed media
 * existed follow docs/bagit-profile-floppy.json). */
#ifndef BAGIT_H
#define BAGIT_H
#include <stddef.h>
#include "meta.h"
#include "strbuf.h"

#define BAG_PROFILE "urn:retroarchiver:bagit-profile:2"

/* folder name from the title; n = 81 for a name of at most 80 bytes */
void bag_dirname(const char *title, char *out, size_t n);
int  bag_create(const char *bagdir, strbuf *err);
int  bag_is_open(const char *bagdir);
/* *bad_disk: the disk with a missing or changed file, 0 if the problem is something else */
int  bag_close(const char *bagdir, const meta_box *box, strbuf *err, int *bad_disk);

#endif
