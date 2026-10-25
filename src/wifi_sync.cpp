#include "wifi_sync.h"
#include "config.h"
#include "file_manager.h"
#include "sd_backup.h"
#include "git_sync.h"

#include <Arduino.h>
#include <WiFi.h>
#include <SDCardManager.h>
#include <Preferences.h>

// --- Internal state ---
static bool syncActive = false;
static SyncState syncState = SyncState::SCANNING;
static char statusText[64] = "";
extern UIState currentState;

extern bool screenDirty;

// --- Network list ---
static constexpr int MAX_NETWORKS = 20;
struct NetworkInfo {
  char ssid[33];
  int  rssi;
  bool encrypted;
  bool saved;  // Has stored password in NVS
};
static NetworkInfo networks[MAX_NETWORKS];
static int networkCount = 0;
static int selectedNet = 0;

// --- Password entry ---
static constexpr int MAX_PASSWORD_LEN = 63;
static char passwordBuf[MAX_PASSWORD_LEN + 1] = "";
static int  passwordLen = 0;

// --- NVS credential storage ---
static Preferences wifiPrefs;
static constexpr int MAX_SAVED_NETWORKS = 4;

static void loadSavedCredentials();
static bool getSavedPassword(const char* ssid, char* passBuf, int passBufSize);
static void saveCredential(const char* ssid, const char* pass);
static void forgetCredential(const char* ssid);

// --- Connecting state ---
static unsigned long connectStartMs = 0;
static char connectingSSID[33] = "";
static bool usedSavedPassword = false;
static bool autoConnectAttempted = false;  // True if we tried auto-connect with saved creds

static bool gitJobStarted = false;
static bool gitSyncSucceeded = false;
static bool returnToEditor = false;
static bool wifiPrefsOpen = false;
static char editorFileToReload[MAX_FILENAME_LEN] = "";

// --- DONE state ---
static unsigned long doneStartMs = 0;
static constexpr unsigned long DONE_DISPLAY_MS = 3000;  // 3s before returning to menu

// --- Forward declarations ---
static void beginScan();
static void beginConnect(const char* ssid, const char* pass);
static void enterSyncingState();
static void enterDoneState();

// =========================================================================
// SD card backup for WiFi credentials
// =========================================================================

static constexpr char WIFI_BACKUP_PATH[] = "/microslate/wifi.json";

static void writeWifiBackup() {
    static char buf[512];
    int count = wifiPrefs.getInt("wifi_count", 0);
    snprintf(buf, sizeof(buf), "{\"count\":%d", count);
    for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
        char sKey[16], pKey[16];
        snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", i);
        snprintf(pKey, sizeof(pKey), "wifi_pass_%d", i);
        String ssid = wifiPrefs.getString(sKey, "");
        String pass = wifiPrefs.getString(pKey, "");
        char tmp[16];
        snprintf(tmp, sizeof(tmp), ",\"s%d\":\"", i);  strncat(buf, tmp, sizeof(buf) - strlen(buf) - 1);
        jsonAppendStr(buf, sizeof(buf), ssid.c_str());  strncat(buf, "\"", sizeof(buf) - strlen(buf) - 1);
        snprintf(tmp, sizeof(tmp), ",\"p%d\":\"", i);  strncat(buf, tmp, sizeof(buf) - strlen(buf) - 1);
        jsonAppendStr(buf, sizeof(buf), pass.c_str());  strncat(buf, "\"", sizeof(buf) - strlen(buf) - 1);
    }
    strncat(buf, "}", sizeof(buf) - strlen(buf) - 1);
    if (!SdMan.exists("/microslate")) SdMan.mkdir("/microslate");
    sdWriteFile(WIFI_BACKUP_PATH, buf);
}

static void restoreWifiBackup() {
    static char buf[512];
    if (!sdReadFile(WIFI_BACKUP_PATH, buf, sizeof(buf))) return;
    int count = jsonGetInt(buf, "count");
    if (count <= 0) return;
    for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
        char sKey[16], pKey[16], sNvs[16], pNvs[16];
        snprintf(sKey, sizeof(sKey), "s%d", i);
        snprintf(pKey, sizeof(pKey), "p%d", i);
        snprintf(sNvs, sizeof(sNvs), "wifi_ssid_%d", i);
        snprintf(pNvs, sizeof(pNvs), "wifi_pass_%d", i);
        char ssid[64] = "", pass[128] = "";
        jsonGetStr(buf, sKey, ssid, sizeof(ssid));
        jsonGetStr(buf, pKey, pass, sizeof(pass));
        if (ssid[0]) {
            wifiPrefs.putString(sNvs, ssid);
            wifiPrefs.putString(pNvs, pass);
        }
    }
    wifiPrefs.putInt("wifi_count", count);
    DBG_PRINTF("[SYNC] Restored %d WiFi credential(s) from SD backup\n", count);
}

// =========================================================================
// NVS credential storage
// =========================================================================

static void loadSavedCredentials() {
  // Mark networks that have saved passwords
  int count = wifiPrefs.getInt("wifi_count", 0);
  for (int i = 0; i < networkCount; i++) {
    networks[i].saved = false;
    for (int j = 0; j < count && j < MAX_SAVED_NETWORKS; j++) {
      char key[16];
      snprintf(key, sizeof(key), "wifi_ssid_%d", j);
      String savedSSID = wifiPrefs.getString(key, "");
      if (savedSSID.length() > 0 && strcmp(savedSSID.c_str(), networks[i].ssid) == 0) {
        networks[i].saved = true;
        break;
      }
    }
  }
}

static bool getSavedPassword(const char* ssid, char* passBuf, int passBufSize) {
  int count = wifiPrefs.getInt("wifi_count", 0);
  for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
    char sKey[16], pKey[16];
    snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", i);
    snprintf(pKey, sizeof(pKey), "wifi_pass_%d", i);
    String savedSSID = wifiPrefs.getString(sKey, "");
    if (savedSSID.length() > 0 && strcmp(savedSSID.c_str(), ssid) == 0) {
      String savedPass = wifiPrefs.getString(pKey, "");
      strncpy(passBuf, savedPass.c_str(), passBufSize - 1);
      passBuf[passBufSize - 1] = '\0';
      return true;
    }
  }
  return false;
}

static void saveCredential(const char* ssid, const char* pass) {
  int count = wifiPrefs.getInt("wifi_count", 0);

  // Check if already saved — update in place
  for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
    char sKey[16], pKey[16];
    snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", i);
    snprintf(pKey, sizeof(pKey), "wifi_pass_%d", i);
    String savedSSID = wifiPrefs.getString(sKey, "");
    if (savedSSID.length() > 0 && strcmp(savedSSID.c_str(), ssid) == 0) {
      wifiPrefs.putString(pKey, pass);
      return;
    }
  }

  // Add new entry (wrap around if full)
  int slot = count < MAX_SAVED_NETWORKS ? count : (count % MAX_SAVED_NETWORKS);
  char sKey[16], pKey[16];
  snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", slot);
  snprintf(pKey, sizeof(pKey), "wifi_pass_%d", slot);
  wifiPrefs.putString(sKey, ssid);
  wifiPrefs.putString(pKey, pass);
  if (count < MAX_SAVED_NETWORKS) {
    wifiPrefs.putInt("wifi_count", count + 1);
  }
  writeWifiBackup();
}

static void forgetCredential(const char* ssid) {
  int count = wifiPrefs.getInt("wifi_count", 0);
  for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
    char sKey[16];
    snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", i);
    String savedSSID = wifiPrefs.getString(sKey, "");
    if (savedSSID.length() > 0 && strcmp(savedSSID.c_str(), ssid) == 0) {
      // Shift remaining entries down
      for (int j = i; j < count - 1 && j < MAX_SAVED_NETWORKS - 1; j++) {
        char srcS[16], srcP[16], dstS[16], dstP[16];
        snprintf(srcS, sizeof(srcS), "wifi_ssid_%d", j + 1);
        snprintf(srcP, sizeof(srcP), "wifi_pass_%d", j + 1);
        snprintf(dstS, sizeof(dstS), "wifi_ssid_%d", j);
        snprintf(dstP, sizeof(dstP), "wifi_pass_%d", j);
        wifiPrefs.putString(dstS, wifiPrefs.getString(srcS, ""));
        wifiPrefs.putString(dstP, wifiPrefs.getString(srcP, ""));
      }
      // Clear last slot
      int lastIdx = count - 1;
      char lastS[16], lastP[16];
      snprintf(lastS, sizeof(lastS), "wifi_ssid_%d", lastIdx);
      snprintf(lastP, sizeof(lastP), "wifi_pass_%d", lastIdx);
      wifiPrefs.remove(lastS);
      wifiPrefs.remove(lastP);
      wifiPrefs.putInt("wifi_count", count - 1);
      writeWifiBackup();
      return;
    }
  }
}

// =========================================================================
// WiFi scanning
// =========================================================================

static void beginScan() {
  syncState = SyncState::SCANNING;
  strcpy(statusText, "Scanning...");
  networkCount = 0;
  selectedNet = 0;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  WiFi.scanNetworks(true);  // async scan
  screenDirty = true;
  DBG_PRINTLN("[SYNC] WiFi scan started");
}

static void processScanResults() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;  // Still scanning

  if (n <= 0) {
    // Scan failed or no networks found
    networkCount = 0;
    syncState = SyncState::NETWORK_LIST;
    strcpy(statusText, n == 0 ? "No networks found" : "Scan failed");
    WiFi.scanDelete();
    screenDirty = true;
    return;
  }

  // Deduplicate by SSID, keeping strongest signal
  networkCount = 0;
  for (int i = 0; i < n && networkCount < MAX_NETWORKS; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;  // Skip hidden networks

    // Check for duplicate
    bool duplicate = false;
    for (int j = 0; j < networkCount; j++) {
      if (strcmp(networks[j].ssid, ssid.c_str()) == 0) {
        duplicate = true;
        if (WiFi.RSSI(i) > networks[j].rssi) {
          networks[j].rssi = WiFi.RSSI(i);
        }
        break;
      }
    }
    if (duplicate) continue;

    strncpy(networks[networkCount].ssid, ssid.c_str(), 32);
    networks[networkCount].ssid[32] = '\0';
    networks[networkCount].rssi = WiFi.RSSI(i);
    networks[networkCount].encrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    networks[networkCount].saved = false;
    networkCount++;
  }

  WiFi.scanDelete();

  // Mark saved networks
  loadSavedCredentials();

  // Sort: saved networks first, then by signal strength
  for (int i = 0; i < networkCount - 1; i++) {
    for (int j = i + 1; j < networkCount; j++) {
      bool swap = false;
      if (networks[j].saved && !networks[i].saved) {
        swap = true;
      } else if (networks[j].saved == networks[i].saved && networks[j].rssi > networks[i].rssi) {
        swap = true;
      }
      if (swap) {
        NetworkInfo tmp = networks[i];
        networks[i] = networks[j];
        networks[j] = tmp;
      }
    }
  }

  selectedNet = 0;
  syncState = SyncState::NETWORK_LIST;
  statusText[0] = '\0';
  screenDirty = true;
  DBG_PRINTF("[SYNC] Found %d networks\n", networkCount);

  // Intelligent auto-connect: if the strongest available network is saved, auto-connect
  if (networkCount > 0 && networks[0].saved) {
    char savedPass[MAX_PASSWORD_LEN + 1];
    if (getSavedPassword(networks[0].ssid, savedPass, sizeof(savedPass))) {
      usedSavedPassword = true;
      autoConnectAttempted = true;
      beginConnect(networks[0].ssid, savedPass);
      DBG_PRINTF("[SYNC] Auto-connecting to strongest saved network: %s\n", networks[0].ssid);
      return;
    }
  }
}

// =========================================================================
// Connection
// =========================================================================

static void beginConnect(const char* ssid, const char* pass) {
  strncpy(connectingSSID, ssid, 32);
  connectingSSID[32] = '\0';
  syncState = SyncState::CONNECTING;
  snprintf(statusText, sizeof(statusText), "Connecting to %s...", ssid);
  connectStartMs = millis();

  WiFi.disconnect(true);
  delay(50);
  WiFi.begin(ssid, pass);
  screenDirty = true;
  DBG_PRINTF("[SYNC] Connecting to %s\n", ssid);
}

static void enterSyncingState() {
  gitJobStarted = true;
  git_sync_set_callback([](git_sync_state_t, const char* message) {
    strncpy(statusText, message ? message : "", sizeof(statusText) - 1);
    statusText[sizeof(statusText) - 1] = '\0';
    screenDirty = true;
  });
  snprintf(statusText, sizeof(statusText), "%s",
           WiFi.localIP().toString().c_str());
  syncState = SyncState::SYNCING;
  screenDirty = true;
  if (git_sync_start(GIT_SYNC_BOTH) != ESP_OK) {
    const char* error = git_sync_get_last_error();
    strncpy(statusText, error && error[0] ? error : "Could not start Git sync",
            sizeof(statusText) - 1);
    statusText[sizeof(statusText) - 1] = '\0';
  }
  screenDirty = true;
  DBG_PRINTF("[GIT] Sync started on %s\n", statusText);
}

static void enterDoneState() {
  gitSyncSucceeded = git_sync_get_state() == GIT_SYNC_SUCCESS;
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  syncState = SyncState::DONE;
  doneStartMs = millis();

  screenDirty = true;
  DBG_PRINTF("[GIT] Done — %s\n", statusText);
}

static void pollConnection() {
  if (WiFi.status() == WL_CONNECTED) {
    // If we used a manually entered password, prompt to save first
    if (!usedSavedPassword) {
      syncState = SyncState::SAVE_PROMPT;
      snprintf(statusText, sizeof(statusText), "%s",
               WiFi.localIP().toString().c_str());
      screenDirty = true;
    } else {
      enterSyncingState();
    }
    return;
  }

  if (millis() - connectStartMs > 25000) {
    WiFi.disconnect(true);
    strcpy(statusText, "Connection failed");

    if (usedSavedPassword) {
      syncState = SyncState::FORGET_PROMPT;
    } else {
      syncState = SyncState::CONNECT_FAILED;
    }

    screenDirty = true;
    DBG_PRINTLN("[SYNC] Connection timed out");
  }
}

// =========================================================================
// Input handling — called from input_handler for all key events
// =========================================================================

void syncHandleKey(uint8_t keyCode, uint8_t modifiers) {
  switch (syncState) {
    case SyncState::SCANNING:
      // No input during scan
      if (keyCode == HID_KEY_ESCAPE) {
        wifiSyncStop();
      }
      break;

    case SyncState::NETWORK_LIST:
      if (keyCode == HID_KEY_DOWN && networkCount > 0) {
        selectedNet = (selectedNet + 1) % networkCount;
        screenDirty = true;
      } else if (keyCode == HID_KEY_UP && networkCount > 0) {
        selectedNet = (selectedNet - 1 + networkCount) % networkCount;
        screenDirty = true;
      } else if (keyCode == HID_KEY_ENTER && networkCount > 0) {
        // Try saved password first
        char savedPass[MAX_PASSWORD_LEN + 1];
        if (getSavedPassword(networks[selectedNet].ssid, savedPass, sizeof(savedPass))) {
          usedSavedPassword = true;
          autoConnectAttempted = false;
          beginConnect(networks[selectedNet].ssid, savedPass);
        } else if (!networks[selectedNet].encrypted) {
          // Open network — connect directly
          usedSavedPassword = false;
          autoConnectAttempted = false;
          beginConnect(networks[selectedNet].ssid, "");
        } else {
          // Need password
          usedSavedPassword = false;
          autoConnectAttempted = false;
          passwordBuf[0] = '\0';
          passwordLen = 0;
          syncState = SyncState::PASSWORD_ENTRY;
          screenDirty = true;
        }
      } else if (keyCode == HID_KEY_ESCAPE) {
        wifiSyncStop();
      }
      break;

    case SyncState::PASSWORD_ENTRY:
      if (keyCode == HID_KEY_ENTER) {
        if (passwordLen > 0) {
          beginConnect(networks[selectedNet].ssid, passwordBuf);
        }
      } else if (keyCode == HID_KEY_ESCAPE) {
        syncState = SyncState::NETWORK_LIST;
        screenDirty = true;
      } else if (keyCode == HID_KEY_BACKSPACE) {
        if (passwordLen > 0) {
          passwordLen--;
          passwordBuf[passwordLen] = '\0';
          screenDirty = true;
        }
      } else {
        // Printable character — Wi-Fi passwords always use US QWERTY input.
        extern char hidToAscii(uint8_t hid, uint8_t modifiers);
        char c = hidToAscii(keyCode, modifiers);
        if (c != 0 && c >= ' ' && c != '\n' && c != '\t' && passwordLen < MAX_PASSWORD_LEN) {
          passwordBuf[passwordLen++] = c;
          passwordBuf[passwordLen] = '\0';
          screenDirty = true;
        }
      }
      break;

    case SyncState::CONNECTING:
      // No input while connecting (timeout handles failure)
      if (keyCode == HID_KEY_ESCAPE) {
        WiFi.disconnect(true);
        if (autoConnectAttempted) {
          // Was auto-connecting — fall back to scan
          beginScan();
        } else {
          syncState = SyncState::NETWORK_LIST;
          screenDirty = true;
        }
      }
      break;

    case SyncState::SYNCING:
      if (keyCode == HID_KEY_ESCAPE &&
          git_sync_get_state() != GIT_SYNC_IN_PROGRESS) {
        wifiSyncStop();
      }
      break;

    case SyncState::DONE:
      // Any key press returns to menu immediately
      wifiSyncStop();
      break;

    case SyncState::CONNECT_FAILED:
      if (keyCode == HID_KEY_ENTER) {
        // Back to network list, re-scan
        beginScan();
      } else if (keyCode == HID_KEY_ESCAPE) {
        wifiSyncStop();
      }
      break;

    case SyncState::SAVE_PROMPT:
      // Up = Yes (save), Down = No (skip)
      if (keyCode == HID_KEY_UP || keyCode == HID_KEY_ENTER) {
        saveCredential(connectingSSID, passwordBuf);
        DBG_PRINTF("[SYNC] Saved credentials for %s\n", connectingSSID);
        enterSyncingState();
      } else if (keyCode == HID_KEY_DOWN || keyCode == HID_KEY_ESCAPE) {
        enterSyncingState();
      }
      break;

    case SyncState::FORGET_PROMPT:
      // Up = Yes (forget), Down = No (keep)
      if (keyCode == HID_KEY_UP || keyCode == HID_KEY_ENTER) {
        forgetCredential(connectingSSID);
        DBG_PRINTF("[SYNC] Forgot credentials for %s\n", connectingSSID);
        beginScan();
      } else if (keyCode == HID_KEY_DOWN || keyCode == HID_KEY_ESCAPE) {
        // Keep credentials — go to network list so user can retry manually
        // rather than triggering auto-connect again immediately
        syncState = SyncState::NETWORK_LIST;
        screenDirty = true;
      }
      break;
  }
}

// =========================================================================
// Public API
// =========================================================================

void wifiSyncStart() {
  if (syncActive) return;
  syncActive = true;
  gitSyncSucceeded = false;
  returnToEditor = currentState == UIState::TEXT_EDITOR;
  editorFileToReload[0] = '\0';
  if (returnToEditor) {
    const char* currentFile = editorGetCurrentFile();
    if (currentFile[0]) {
      strncpy(editorFileToReload, currentFile, sizeof(editorFileToReload) - 1);
      editorFileToReload[sizeof(editorFileToReload) - 1] = '\0';
      if (editorHasUnsavedChanges()) saveCurrentFile();
      if (editorHasUnsavedChanges()) {
        editorFileToReload[0] = '\0';
        syncState = SyncState::DONE;
        strlcpy(statusText, "Could not save note; Git sync cancelled",
                sizeof(statusText));
        doneStartMs = millis();
        screenDirty = true;
        return;
      }
    }
  }
  gitJobStarted = false;
  wifiPrefs.begin("wifi_creds", false);
  wifiPrefsOpen = true;
  if (!wifiPrefs.isKey("wifi_count")) restoreWifiBackup();
  if (!git_sync_is_configured()) {
    syncState = SyncState::DONE;
    strlcpy(statusText, "Create /git.cfg to configure Git sync", sizeof(statusText));
    doneStartMs = millis();
    screenDirty = true;
    return;
  }

  beginScan();

  DBG_PRINTLN("[SYNC] WiFi sync started");
}

void wifiSyncStop() {
  if (!syncActive) return;

  if (git_sync_get_state() != GIT_SYNC_IN_PROGRESS) {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
  }

  if (wifiPrefsOpen) {
    wifiPrefs.end();
    wifiPrefsOpen = false;
  }
  syncActive = false;
  networkCount = 0;
  passwordBuf[0] = '\0';
  passwordLen = 0;
  statusText[0] = '\0';

  if (returnToEditor && editorFileToReload[0]) {
    refreshFileList();
    loadFile(editorFileToReload);
  } else if (returnToEditor) {
    currentState = UIState::TEXT_EDITOR;
  } else {
    currentState = UIState::MAIN_MENU;
  }
  returnToEditor = false;
  editorFileToReload[0] = '\0';
  screenDirty = true;

  DBG_PRINTLN("[SYNC] WiFi sync stopped");
}

void wifiSyncLoop() {
  if (!syncActive) return;

  switch (syncState) {
    case SyncState::SCANNING:
      processScanResults();
      break;

    case SyncState::CONNECTING:
      pollConnection();
      break;

    case SyncState::SYNCING:
      if (gitJobStarted && (git_sync_get_state() == GIT_SYNC_SUCCESS ||
                            git_sync_get_state() == GIT_SYNC_ERROR)) {
        enterDoneState();
      }
      break;

    case SyncState::DONE:
      // Auto-return to menu after 3 seconds
      if (millis() - doneStartMs > DONE_DISPLAY_MS) {
        wifiSyncStop();
      }
      break;

    case SyncState::SAVE_PROMPT:
      // Git sync starts after the user responds to the credential prompt.
      break;

    default:
      break;
  }
}

bool isWifiSyncActive() {
  return syncActive;
}

SyncState getSyncState() {
  return syncState;
}

int getNetworkCount() {
  return networkCount;
}

const char* getNetworkSSID(int i) {
  if (i < 0 || i >= networkCount) return "";
  return networks[i].ssid;
}

int getNetworkRSSI(int i) {
  if (i < 0 || i >= networkCount) return -100;
  return networks[i].rssi;
}

bool isNetworkEncrypted(int i) {
  if (i < 0 || i >= networkCount) return false;
  return networks[i].encrypted;
}

bool isNetworkSaved(int i) {
  if (i < 0 || i >= networkCount) return false;
  return networks[i].saved;
}

int getSelectedNetwork() {
  return selectedNet;
}

const char* getPasswordBuffer() {
  return passwordBuf;
}

int getPasswordLen() {
  return passwordLen;
}

const char* getSyncStatusText() {
  return statusText;
}

bool didGitSyncSucceed() {
  return gitSyncSucceeded;
}
