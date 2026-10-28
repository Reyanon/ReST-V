// ADR-209: update check and install from the GitHub releases of Reyanon/ReST-V
#ifndef RSTV_UPDATE_H
#define RSTV_UPDATE_H

// start the check on its own thread (network, GitHub API); nothing when the measuring tools started the app
void rstv_update_check_start(void);
// 1 once a release newer than this build (and not skipped) is known
int rstv_update_ready(void);
// ADR-213 (owner): "Check for updates" in the game list options. A skipped version is offered again
void rstv_update_check_now(void);
enum { RSTV_UPD_NONE, RSTV_UPD_CHECKING, RSTV_UPD_FOUND, RSTV_UPD_LATEST, RSTV_UPD_FAILED };
int rstv_update_state(void);
const char *rstv_update_version(void); // its tag, e.g. "v0.1.460"
const char *rstv_update_notes(void);   // the notes of every newer release, newest first (plain text, '\n' lines)
void rstv_update_skip(void);           // do not offer this version again
// download, unpack and hand over to app0:updater.bin (does not return then). progress(stage, percent) is called
// while it runs (percent -1: unknown). Returns a message on failure
const char *rstv_update_install(void (*progress)(const char *stage, int percent));

#endif
