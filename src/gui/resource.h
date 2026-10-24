/* Ids of the dialogs in retroarchiver.rc and of their controls: shared by
 * the resource file and by settings_dlg.c and about.c. The texts are not
 * in the resource file: they come from tr() when a dialog opens. */
#ifndef RA_RESOURCE_H
#define RA_RESOURCE_H

#define IDI_APP          1               /* the icon */

/* Settings: the dialog with the tabs, one child dialog per tab */
#define IDD_SETTINGS     100
#define IDD_PG_GENERAL   101
#define IDD_PG_FLOPPY    102
#define IDD_PG_OPTICAL   103
#define IDD_PG_AUTO      104
#define IDD_ABOUT        110

#define IDC_TABS         1000

/* General */
#define IDC_UILANG_L     1100
#define IDC_UILANG       1101
#define IDC_UILANG_H     1102
#define IDC_FOLDER_L     1103
#define IDC_FOLDER       1104
#define IDC_FOLDER_B     1105
#define IDC_MAXREADS_L   1106
#define IDC_MAXREADS     1107
#define IDC_MAXREADS_H   1108
#define IDC_SOUNDS       1109
#define IDC_SOUNDS_H     1110

/* Floppy */
#define IDC_SAMDISK_L    1200
#define IDC_SAMDISK      1201
#define IDC_SAMDISK_B    1202
#define IDC_SAMDISK_H    1203
#define IDC_READFROM_L   1204
#define IDC_READFROM     1205
#define IDC_READFROM_B   1206
#define IDC_READFROM_H   1207
#define IDC_FNAME_L      1208
#define IDC_FNAME        1209
#define IDC_FNAME_H      1210

/* CD and DVD */
#define IDC_DIC_L        1300
#define IDC_DIC          1301
#define IDC_DIC_B        1302
#define IDC_DRIVE_L      1303
#define IDC_DRIVE        1304
#define IDC_CDNAME_L     1305
#define IDC_CDNAME       1306
#define IDC_DVDNAME_L    1307
#define IDC_DVDNAME      1308
#define IDC_DAT_L        1309
#define IDC_DAT          1310
#define IDC_DAT_B        1311
#define IDC_DAT_H        1312
#define IDC_OFFSET_L     1313
#define IDC_OFFSET       1314
#define IDC_OFFSET_H     1315
#define IDC_OFFSET_LINK  1316
#define IDC_NOLEADIN_L   1317
#define IDC_NOLEADIN     1318
#define IDC_FORGET       1319

/* Automatic */
#define IDC_AUTO_INTRO   1400
#define IDC_COUNT_L      1401
#define IDC_COUNT        1402
#define IDC_COUNT_U      1403
#define IDC_COUNT_H      1404
#define IDC_POLL_L       1405
#define IDC_POLL         1406
#define IDC_POLL_U       1407
#define IDC_POLL_H       1408
#define IDC_AUTO_NOTE    1409

/* About */
#define IDC_AB_ICON      1500
#define IDC_AB_NAME      1501
#define IDC_AB_TAGLINE   1502
#define IDC_AB_LINK      1503
#define IDC_AB_LICENSE   1504
#define IDC_AB_LICVIEW   1505
#define IDC_AB_TPTITLE   1506
#define IDC_AB_LIST      1507
#define IDC_AB_STATUS    1508
#define IDC_AB_OPENLIC   1509
#define IDC_AB_NOTICES   1510
#define IDC_AB_COPY      1511

#endif
