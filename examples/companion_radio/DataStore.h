#pragma once

#include <helpers/IdentityStore.h>
#include <helpers/ContactInfo.h>
#include <helpers/ChannelDetails.h>
#include "NodePrefs.h"
#include "ScopeList.h"

class DataStoreHost {
public:
  virtual bool onContactLoaded(const ContactInfo& contact) =0;
  virtual bool getContactForSave(uint32_t idx, ContactInfo& contact) =0;
  virtual bool onChannelLoaded(uint8_t channel_idx, const ChannelDetails& ch) =0;
  virtual bool getChannelForSave(uint8_t channel_idx, ChannelDetails& ch) =0;
};

class DataStore {
  FILESYSTEM* _fs;
  FILESYSTEM* _fsExtra;
  mesh::RTCClock* _clock;
  IdentityStore identity_store;

  void loadPrefsInt(const char *filename, NodePrefs& prefs, double& node_lat, double& node_lon, size_t offset = 0);
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  void checkAdvBlobFile();
#endif

public:
  DataStore(FILESYSTEM& fs, mesh::RTCClock& clock);
  DataStore(FILESYSTEM& fs, FILESYSTEM& fsExtra, mesh::RTCClock& clock);
  void begin();
  bool formatFileSystem();
  FILESYSTEM* getPrimaryFS() const { return _fs; }
  FILESYSTEM* getSecondaryFS() const { return _fsExtra; }
  bool loadMainIdentity(mesh::LocalIdentity &identity);
  bool saveMainIdentity(const mesh::LocalIdentity &identity);
  void loadPrefs(NodePrefs& prefs, double& node_lat, double& node_lon);
  bool savePrefs(const NodePrefs& prefs, double node_lat, double node_lon);
  void loadContacts(DataStoreHost* host);
  void saveContacts(DataStoreHost* host, bool (*filter)(const ContactInfo& c) = NULL);
#if defined(ESP32) && defined(FIRMWARE_SOLO_BUILD)
  // Serialize an immutable snapshot, then advance one flash write per main
  // loop. All filesystem access stays on the same task as the UI and mesh.
  bool startContactSaveDeferred(DataStoreHost* host, bool (*filter)(const ContactInfo& c));
  void stepContactSave(bool force = false);
  bool isContactSaveBusy() const { return _contact_save_phase != 0; }
  uint32_t contactSaveStartedMillis() const { return _contact_save_started_ms; }
  bool takeContactSaveFailure() { bool failed = _contact_save_failed; _contact_save_failed = false; return failed; }
  void finishContactSave();
#endif
  // Returns true if a channels file (current or legacy) existed and was
  // loaded -- false only on a genuinely fresh device with neither file, so
  // the caller knows whether it's safe to seed default channels (see
  // MyMesh::begin()'s addChannel("Public", ...) -- seeding unconditionally
  // would resurrect a channel the user had deliberately deleted, since a
  // deleted slot is simply absent from the file, not written as empty).
  bool loadChannels(DataStoreHost* host);
  void saveChannels(DataStoreHost* host);
  // /scopes1: the shared named-scope list (see ScopeList.h). `prefs` is only
  // read, for a one-time migration of a pre-existing single
  // default_scope_name/key into list entry 1 -- the file is authoritative
  // once it exists.
  //
  // Returns true ONLY when that legacy migration just ran, i.e. this boot is
  // the first on a device that had a Scope configured the old way. The caller
  // uses that to seed the channels that already exist with the migrated entry
  // (see MyMesh::begin) -- channels carry their own scope now, so without the
  // seed an upgrader's channel traffic would silently drop to unscoped even
  // though their DMs kept the old scope. Returns false when /scopes1 was
  // already there, and on a genuinely fresh device with nothing to migrate
  // (list left empty, default_idx 0 == "*").
  bool loadScopeList(ScopeList& list, const NodePrefs& prefs);
  void saveScopeList(const ScopeList& list);
  void migrateToSecondaryFS();
  uint8_t getBlobByKey(const uint8_t key[], int key_len, uint8_t dest_buf[]);
  bool putBlobByKey(const uint8_t key[], int key_len, const uint8_t src_buf[], uint8_t len);
  bool deleteBlobByKey(const uint8_t key[], int key_len);
#if defined(DEFER_ADVERT_FLASH) && defined(ESP32)
  void stepPendingAdvertWrite();
  void finishPendingAdvertWrites();
  bool hasPendingAdvertWrites() const;
#endif
  File openRead(const char* filename);
  File openRead(FILESYSTEM* fs, const char* filename);
  File openWrite(const char* filename);
  // Replace final_path with a fully-written temp file (see openWrite()).
  // On filesystems whose rename cannot overwrite, this removes the old path
  // first; use A/B snapshots for records that must survive that window.
  bool commitFile(const char* tmp_path, const char* final_path);
  bool removeFile(const char* filename);
  bool removeFile(FILESYSTEM* fs, const char* filename);
  uint32_t getStorageUsedKb() const;
  uint32_t getStorageTotalKb() const;
  void saveRTCTime();
  void restoreRTCTime();

private:
#if defined(SOLO_SAFE_STORAGE) && defined(ESP32)
  int8_t _prefs_snapshot_slot = -1;
  uint32_t _prefs_generation = 0;
  int8_t _contacts_snapshot_slot = -1;
  uint32_t _contacts_generation = 0;
#endif
#if defined(DEFER_ADVERT_FLASH) && defined(ESP32)
  static constexpr uint8_t MAX_PENDING_ADVERTS = 64;
  struct PendingAdvert {
    uint8_t key[8];
    uint8_t data[255];
    uint8_t len;
    bool dirty;
  };
  PendingAdvert _pending_adverts[MAX_PENDING_ADVERTS] = {};
  uint8_t _next_advert_slot = 0;
  uint32_t _next_advert_write_ms = 0;
#endif
#if defined(ESP32) && defined(FIRMWARE_SOLO_BUILD)
  uint8_t* _contact_snapshot = nullptr;
  size_t _contact_snapshot_len = 0;
  size_t _contact_snapshot_pos = 0;
  File _contact_save_file;
  uint8_t _contact_save_phase = 0;
  bool _contact_save_failed = false;
  uint32_t _contact_save_io_us = 0;
  uint32_t _contact_save_data_us = 0;
  uint32_t _contact_save_started_ms = 0;
  uint32_t _contact_next_step_ms = 0;
#if defined(SOLO_SAFE_STORAGE)
  const char* _contact_save_target = nullptr;
  uint32_t _contact_save_generation = 0;
  uint32_t _contact_save_crc = 0xFFFFFFFFUL;
#endif
#endif
  FILESYSTEM* _getContactsChannelsFS() const { if (_fsExtra) return _fsExtra; return _fs;};
};
