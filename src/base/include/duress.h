/* The duress password (tools/DURESS.md): a second password that, typed where
 * the real one is asked (the boot passphrase prompt, the lock screen, sudo),
 * destroys the disk instead of opening it, as GrapheneOS's duress PIN does.
 *
 * base/duress.c: its verifier, PBKDF2-HMAC-SHA256 with a random salt, kept in
 * DURESS_FILE (0600, root's), and the wipe. Checked only after the real
 * credential has been refused, and progs/duress-shadow.c makes sure the typed
 * text is not the login password: the real one always wins, as GrapheneOS
 * gives the real unlock method precedence.
 *
 * duress_wipe overwrites the LUKS2 headers and key slots of the partitions
 * labelled locachy-crypt and locachy-swap-crypt (every key slot: passphrase
 * and TPM), clears the TPM 2.0 storage hierarchy where its lockout
 * authorization is empty, and powers off. Without the headers the data
 * cannot be decrypted by anyone, the passphrase included. On a system
 * without those LUKS2 partitions it does nothing (settings refuses to arm it
 * there: deleting files is neither quick nor final). */
#ifndef LOCACHY_DURESS_H
#define LOCACHY_DURESS_H

#ifndef DURESS_FILE
#define DURESS_FILE "/etc/locachy/duress"
#endif
#ifndef DURESS_SYS_BLOCK
#define DURESS_SYS_BLOCK "/sys/class/block"
#endif
#ifndef DURESS_DEV_DIR
#define DURESS_DEV_DIR "/dev"
#endif
/* The keys that sign this machine's boot images, destroyed with the disk. */
#ifndef DURESS_KEY_PCR
#define DURESS_KEY_PCR "/etc/systemd/tpm2-pcr-private-key.pem"
#endif
#ifndef DURESS_KEY_PK
#define DURESS_KEY_PK "/var/lib/sbctl/keys/PK/PK.key"
#endif
#ifndef DURESS_KEY_KEK
#define DURESS_KEY_KEK "/var/lib/sbctl/keys/KEK/KEK.key"
#endif
#ifndef DURESS_KEY_DB
#define DURESS_KEY_DB "/var/lib/sbctl/keys/db/db.key"
#endif

#define DURESS_MIN_LENGTH 8

int duress_configured(void);            /* 1: a valid verifier is there */
int duress_has_luks(void);              /* 1: a locachy-crypt or locachy-swap-crypt LUKS2 partition */
int duress_luks_paths(char paths[][160], int max); /* their device paths; their count */
int duress_set(const char *secret);     /* 0, or -1 (too short, cannot write) */
int duress_clear(void);                 /* 0, or -1 */
int duress_matches(const char *secret); /* 1: it is the duress password */
/* Destroys the LOCachy LUKS2 headers and key slots, the keys that sign boot
 * images, and powers off. Returns only when there was nothing to destroy (or
 * in tests built with DURESS_TEST_NO_POWEROFF). */
void duress_wipe(void);

/* progs/duress-shadow.c (glibc programs only, -lcrypt): whether pw is the
 * login password of user, by /etc/shadow (root only): 1 it is; 0 it is not
 * (or the account has no password to type); -1 it cannot be told (no entry,
 * or /etc/shadow unreadable). A wipe needs 0: an unknown login never wipes. */
int duress_login_check(const char *user, const char *pw);

#endif
