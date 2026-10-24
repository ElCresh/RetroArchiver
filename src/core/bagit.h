/* bagit - BagIt 1.0 bags (RFC 8493) with SHA-256 manifests, following
 * the profile docs/bagit-profile.json (boxes closed by 261017 follow
 * docs/bagit-profile-2.json, those closed before mixed media existed
 * docs/bagit-profile-floppy.json). */
#ifndef BAGIT_H
#define BAGIT_H
#include <stddef.h>
#include "meta.h"
#include "strbuf.h"

#define BAG_PROFILE "urn:retroarchiver:bagit-profile:3"

/* the marker of a bag still being filled; 261017 called it .in-corso: a
 * bag with that one is open too, and closing removes whichever is there */
#define BAG_OPEN_MARK     ".in-progress"
#define BAG_OPEN_MARK_OLD ".in-corso"
/* the work folder of a closed bag being edited: a tag folder, ignored by
 * BagIt, so the bag stays valid while the new reads are made */
#define BAG_EDIT_DIR "editing"
/* Box-Status in bag-info.txt; 261017 wrote "completo" and "incompleto" */
#define BAG_COMPLETE   "complete"
#define BAG_INCOMPLETE "incomplete"
/* 1 if a Box-Status value, old or new, means complete */
int  bag_status_complete(const char *status);

/* folder name from the title; n = 81 for a name of at most 80 bytes */
void bag_dirname(const char *title, char *out, size_t n);
int  bag_create(const char *bagdir, strbuf *err);
int  bag_is_open(const char *bagdir);
/* digests already known to be right, by path relative to the bag
 * ("data/CD01/CD01.bin"): a bag reopened to be edited does not hash again
 * the files it has just verified */
typedef struct { int n, cap; char **rel; char (*hex)[65]; } bag_digests;
void bag_digests_add(bag_digests *d, const char *rel, const char *hex);
int  bag_digests_find(const bag_digests *d, const char *rel);   /* index, -1 if absent */
void bag_digests_free(bag_digests *d);
/* the lines of a manifest of the bag ("manifest-sha256.txt"); -1 if missing */
int  bag_manifest_load(const char *bagdir, const char *name, bag_digests *out);

/* *bad_disk: the disk with a missing or changed file, 0 if the problem is
 * something else; known (may be NULL): the files of data/ listed there are
 * not hashed again */
int  bag_close(const char *bagdir, const meta_box *box, const bag_digests *known,
               strbuf *err, int *bad_disk);

#endif
