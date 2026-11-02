/* GabagoolOS internals. GPLv2+. */
/* The release key's public half: the 32 raw bytes of an Ed25519 public key
 * that signs every update's manifest. Its private half never comes near
 * this tree. Until it is filled in this build checks no update and
 * installs none (Settings > Software update says so).
 *
 * To fill it (the release owner, once):
 *   openssl genpkey -algorithm ed25519 -out gabagool-release.pem   (keep it off the repo)
 *   python3 sloppy-dev/ota/make-update.py pubkey gabagool-release.pem
 * and paste what that prints over the two lines below. */
#ifndef SLOPPY_OTA_KEY_H
#define SLOPPY_OTA_KEY_H
#define SL_OTA_KEY_SET 0
#define SL_OTA_KEY { 0 }   /* placeholder: 32 zero bytes, no key */
#endif
