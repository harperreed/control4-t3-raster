/* ABOUTME: Dropbear compile-time options for the TT7 probe image (copied to localoptions.h).
 * ABOUTME: Key-only root login: password auth is compiled out, not just switched off by -s. */
#ifndef TT7_DROPBEAR_LOCALOPTIONS_H
#define TT7_DROPBEAR_LOCALOPTIONS_H

/* No password logins, ever. init also passes -s; this makes it impossible
 * rather than a runtime flag someone could drop. */
#define DROPBEAR_SVR_PASSWORD_AUTH 0
#define DROPBEAR_SVR_PAM_AUTH 0
#define DROPBEAR_SVR_PUBKEY_AUTH 1

/* No dbclient is built, so the client-side auth code never links in. */
#define DROPBEAR_CLI_PASSWORD_AUTH 0

/* BusyBox installs applets in all four of these. */
#define DEFAULT_PATH "/usr/bin:/bin:/usr/sbin:/sbin"

#endif
