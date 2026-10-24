/* messages - the core's texts that can reach a window: errors, and the
 * reasons a CD was not read. The core writes them in English (the console
 * and the log stay as they are); the window recognizes them and writes them
 * again in its language with the key of messages_list.h (lang_core_text).
 *
 * A format stays a literal for the compiler, which checks it against its
 * arguments; every M_ has its line in messages_list.h (make test checks it).
 * Changing a text here changes its English in the window too; a translation
 * must keep the same conversions in the same order. */
#ifndef MESSAGES_H
#define MESSAGES_H

/* the media's names, inside other messages */
#define M_MEDIUM_FLOPPY "floppy %d"
#define M_MEDIUM_CD     "CD %d"
#define M_MEDIUM_DVD    "DVD %d"

/* session.c */
#define M_SES_READ_BACK "cannot read back %s, just written: check the destination disk."
#define M_SES_MOVE_ASIDE "cannot move data/%s to %s: is it open in another program? close it and start again."
#define M_SES_REMOVE "cannot remove data/%s: is it open in another program? close it and start again."
#define M_SES_CLOSED "The box \"%s\" is already closed in %s: choose another title, or tell it apart with the edition."
#define M_SES_MKDIR_PERM "Cannot create the folder %s: check the permissions."
#define M_SES_HOLDS "The folder %s holds the box %s, not %s: choose a different title, or resume that box with its exact title and edition."
#define M_SES_NAMES "The naming scheme \"%s\" gives the same name to different media: use {n}, {nn} or {nnn} and a shorter title."
#define M_SES_NOT_IN_MANIFEST "%s is not in the manifest of the box: move it out of data and try again."
#define M_SES_REREAD_TOO "%s does not match its fingerprint any more: read %s again too."
#define M_SES_DAMAGED "%s does not match its fingerprint any more: the box is damaged."
#define M_SES_NO_BOX "There is no box \"%s\" in %s: check the title and the edition."
#define M_SES_NOT_EDITABLE "The box in %s cannot be edited: %s."
#define M_SES_EDIT_HOLDS "The folder %s holds the box \"%s\", not \"%s\": use its exact title and edition."
#define M_SES_NOMEM "Out of memory."
#define M_SES_STAYS_ERR "%s. %s stays as it was."
#define M_SES_MKDIR_PATH "Cannot create the folder %s: check the path."
#define M_SES_TOO_MANY "Too many media: at most %d per box."
#define M_SES_CLOSE_FAILED "%s. Start again with the same title: %s will be asked for again."

/* meta.c */
#define M_META_WRITE "cannot write %s: is the destination disk full or write-protected?"
#define M_META_NOT_FOUND "cannot find %s: if the box is damaged, move its folder away and start over"
#define M_META_UNREADABLE "%s cannot be read (%s): if the box is damaged, move its folder away and start over"
#define M_META_NEWER "%s comes from a newer version of RetroArchiver: update the program to open this box"
#define M_META_NOT_BOX "%s is not a RetroArchiver box: if the box is damaged, move its folder away and start over"

/* bagit.c */
#define M_BAG_MKDIR "cannot create the folder %s"
#define M_BAG_WRITE "cannot write %s"
#define M_BAG_READ "cannot read %s"
#define M_BAG_NO_DATA "the title, the platform or the composition is missing: fill them in before closing the box"
#define M_BAG_MISSING "data/%s is missing: read %s again"
#define M_BAG_CHANGED "data/%s changed after the read (different fingerprint): read %s again"
#define M_BAG_REMOVE_MARK "cannot remove %s from %s: check the permissions"

/* archive.c */
#define M_ARC_NO_MANIFEST "manifest-sha256.txt is missing in %s: the package was not closed properly"

/* boxtxn.c */
#define M_TXN_MISSING "%s is missing in %s: nothing was changed"
#define M_TXN_EXISTS "%s already exists in %s: nothing was changed"
#define M_TXN_FOLDER_EXISTS "the folder %s already exists: choose another title or edition"
#define M_TXN_MKDIR "cannot create %s"
#define M_TXN_COPY "cannot copy %s"
#define M_TXN_MOVE "cannot move %s to %s: is it open in another program?"
#define M_TXN_DISK_FULL "cannot write the manifests: is the disk full?"
#define M_TXN_NO_MATCH "the new package does not match its manifest"
#define M_TXN_MANIFESTS "the manifests of %s cannot be read"
#define M_TXN_LINE "%s does not match the new manifest"
#define M_TXN_EXTRA "data/ in %s has files the new manifest does not list"
#define M_TXN_RENAME "cannot rename %s: close the folder if it is open in Explorer and try again"

/* boxedit.c */
#define M_EDIT_NOT_BOX "%s is not a box"
#define M_EDIT_INTERRUPTED "an edit of this box was interrupted: open the archive again to finish or undo it"
#define M_EDIT_OPEN "the box is still open: resume it instead"
#define M_EDIT_UNREADABLE "the box cannot be read: %s"
#define M_EDIT_OLD "the box was made by RetroArchiver 261017: convert it first"
#define M_EDIT_NAMES_LONG "the file names of the %s use the title, and with \"%s\" they would be too long: choose a shorter title"
#define M_EDIT_DAMAGED "%s does not match the manifest: read that medium again with Edit, without changing the data of the box"
#define M_EDIT_NOMEM "out of memory"
#define M_EDIT_EMPTY "the title and the platform cannot be empty"
#define M_EDIT_HISTORY_FULL "the history of the box is full: no more changes can be recorded"

/* floppy.c: why a read did not start */
#define M_FLP_NOTOOL "Cannot find %s: put it in the program folder or in the PATH, or give the full path of samdisk.exe."

/* optical.c: why a read did not start, or why DiscImageCreator stopped */
#define M_OPT_NODLL "%s does not start: the Visual C++ 2015-2019 x86 runtimes are missing (VCRUNTIME140.dll). On Windows XP install version 14.27 of the Visual C++ Redistributable."
#define M_OPT_NOTOOL "Cannot find %s: put it in the same folder as the program or in the PATH, or give the full path of DiscImageCreator.exe."
#define M_OPT_FAT32 "the folder is on a %s drive, which cannot hold files over 4 GB, and the image of this disc will be %lld MB: choose a folder on an NTFS drive"
#define M_OPT_ANSI "the name of folder %s has characters that DiscImageCreator (an ANSI program) cannot read, and it has no 8.3 short name: choose an archive folder whose name has no foreign letters"
#define M_OPT_SPACE "%s has %lld MB free and the read needs about %lld: free some space or choose another archive folder"
#define M_OPT_CANNOT_READ "Cannot read the %s: %s."
#define M_OPT_MKDIR "Cannot create folder %s: check the destination drive."
#define M_OPT_LAST_LINE " Last line: \"%.280s\"."
#define M_OPT_STOPPED "DiscImageCreator stopped with error %d.%s Check that the disc is in drive %s: and clean, and the space in folder %s (%lld MB free), then try again."
#define M_OPT_MODEL "model %s"
#define M_OPT_MODEL_UNKNOWN "this drive's model (see the _drive.txt log of the read)"
#define M_OPT_OFFSET "The drive's read offset is not set, and DiscImageCreator does not know this drive: look it up for %s in the AccurateRip or redump list and set [Optical] DriveOffset in RetroArchiver.ini (dskrun: --drive-offset), then read it again."
#define M_OPT_RESETS_SF "The drive was reset %d times while DiscImageCreator looked for copy protections (/sf): stopped it, it would go on like this for an hour. Read the CD again without the protection scan (Copy protection: Off)."
#define M_OPT_WRONG_DISC "The drive does not hold a %s: DiscImageCreator says the disc is of another type. Check the disc, or the contents of the box."
#define M_OPT_DENIED "DiscImageCreator cannot open drive %s: (access denied): run the program as an administrator."
#define M_OPT_C2_SLOWER "DiscImageCreator stopped with %d sectors still unreadable after %s rereads (C2 errors) at %sx: the disc is scratched or dirty there. Retry reads it again at %dx; washing it first helps (water and a drop of soap, dried from the centre outwards)."
#define M_OPT_C2_LOWEST "DiscImageCreator stopped with %d sectors still unreadable after %s rereads (C2 errors) at %sx, the lowest speed RetroArchiver uses: the disc is scratched or dirty there. Wash it with water and a drop of soap, dry it from the centre outwards and try again, or read it with another drive."
#define M_OPT_FIRMWARE "DiscImageCreator does not read with this drive until its firmware is the latest one: update the drive's firmware (DiscImageCreator's log says where to find it), restart the PC and read the %s again."
#define M_OPT_INCOMPLETE "DiscImageCreator finished but %.200s is missing: the read is incomplete."
#define M_OPT_WITH_RESETS "%s The drive was reset during the read (UNIT_ATTENTION, timeouts): the copy protection scan would not help, it makes the drive reset even more."

#endif
