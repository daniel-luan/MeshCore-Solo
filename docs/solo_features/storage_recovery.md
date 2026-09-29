# Heltec V4 Solo storage recovery

The V4 Solo build mounts SPIFFS without automatic formatting. If mounting fails,
boot stops at **Storage unavailable**. A power cycle retries the mount without
changing files. Holding the PRG button for five seconds deliberately formats the
storage partition and reboots; this erases settings, contacts, channels, and
other saved data.

Settings and contacts use two checked snapshots each. Every save writes the
inactive slot, verifies its length and CRC, then makes it the newest generation.
On boot, the newest valid slot is loaded. If neither slot is valid, the firmware
falls back to the older Solo files (`/new_prefs` and `/contacts3`) so existing
devices migrate on their next save. The older files are left in place as a
fallback.

The snapshot files are specific to this build. Older firmware does not read
them; after a downgrade it may show settings or contacts from the last time the
legacy files were written. Back up device data before downgrading.

Settings saves batch bytes into 512-byte writes and verify the saved snapshot.
If verification fails, the on-device UI shows **Settings save failed**.
