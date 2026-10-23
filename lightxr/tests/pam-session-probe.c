/* Test-only PAM interposition. Never opens a session or authenticates a user. */
#include <security/pam_appl.h>
#include <stdio.h>
#include <stdlib.h>
int pam_setcred(pam_handle_t *pamh, int flags) { (void)pamh; (void)flags; return PAM_SUCCESS; }
int pam_open_session(pam_handle_t *pamh, int flags) {
    (void)flags;
    const void *tty = NULL, *display = NULL;
    pam_get_item(pamh, PAM_TTY, &tty);
#ifdef PAM_XDISPLAY
    pam_get_item(pamh, PAM_XDISPLAY, &display);
#endif
    FILE *out = fopen(getenv("LIGHTXR_PAM_PROBE_REPORT"), "w");
    if (!out) return PAM_SESSION_ERR;
    const char *vt = pam_getenv(pamh,"XDG_VTNR");
    fprintf(out,"%s\n%s\n%s\n",tty?(const char*)tty:"(null)",display?(const char*)display:"(null)",vt?vt:"(null)");
    fclose(out);
    /* Stop the real worker before any actual PAM session, logind or desktop. */
    return PAM_SESSION_ERR;
}
