#include "MQTTBridge.h"
#include "../MQTTMessageBuilder.h"
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <Timezone.h>

#ifdef ESP_PLATFORM
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#endif

// Helper function to strip quotes from strings (both single and double quotes)
static void stripQuotes(char* str, size_t max_len) {
  if (!str || max_len == 0) return;
  
  size_t len = strlen(str);
  if (len == 0) return;
  
  // Remove leading quote (single or double)
  if (str[0] == '"' || str[0] == '\'') {
    memmove(str, str + 1, len);
    len--;
  }
  
  // Remove trailing quote (single or double)
  if (len > 0 && (str[len-1] == '"' || str[len-1] == '\'')) {
    str[len-1] = '\0';
  }
}

// Helper function to check if WiFi credentials are valid
static bool isWiFiConfigValid(const NodePrefs* prefs) {
  // Check if WiFi SSID is configured (not empty)
  if (strlen(prefs->wifi_ssid) == 0) {
    return false;
  }
  
  // WiFi password can be empty for open networks, so we don't check it
  
  return true;
}

#ifdef WITH_MQTT_BRIDGE

// PSRAM-aware allocation: prefer PSRAM on ESP32 when BOARD_HAS_PSRAM, fallback to internal heap or malloc.
// Use psram_free() for any pointer returned by psram_malloc().
static void* psram_malloc(size_t size) {
  if (size == 0) return nullptr;
#if defined(ESP_PLATFORM) && defined(BOARD_HAS_PSRAM)
  void* p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
  if (p != nullptr) return p;
  p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL);
  return p;
#else
  return malloc(size);
#endif
}

static void psram_free(void* ptr) {
  if (ptr == nullptr) return;
#if defined(ESP_PLATFORM)
  heap_caps_free(ptr);
#else
  free(ptr);
#endif
}

// Time (millis()) when WiFi was last seen connected; 0 when disconnected. Used for get wifi.status uptime.
static unsigned long s_wifi_connected_at = 0;

#ifdef MQTT_MEMORY_DEBUG
// #region agent log
static void agentLogHeap(const char* location, const char* message, const char* hypothesisId,
                         size_t free_h, size_t max_alloc, unsigned long internal_free, unsigned long spiram_free) {
  char buf[320];
  snprintf(buf, sizeof(buf),
          "{\"sessionId\":\"debug-session\",\"location\":\"%s\",\"message\":\"%s\",\"hypothesisId\":\"%s\","
          "\"data\":{\"free\":%u,\"max_alloc\":%u,\"internal_free\":%lu,\"spiram_free\":%lu},\"timestamp\":%lu}",
          location, message, hypothesisId, (unsigned)free_h, (unsigned)max_alloc, internal_free, spiram_free,
          (unsigned long)millis());
  Serial.println(buf);
}
// #endregion
#endif

// Singleton for formatMqttStatusReply (set in begin(), cleared in end())
static MQTTBridge* s_mqtt_bridge_instance = nullptr;

// Only force-disconnect main broker after this many consecutive publish failures (reduces heap fragmentation from disconnect/reconnect storms)
static const int MAIN_CLIENT_DISCONNECT_FAILURE_THRESHOLD = 3;
static int s_consecutive_main_publish_failures = 0;

unsigned long MQTTBridge::getWifiConnectedAtMillis() {
  return s_wifi_connected_at;
}

void MQTTBridge::formatMqttStatusReply(char* buf, size_t bufsize, const NodePrefs* prefs) {
  if (buf == nullptr || bufsize == 0) return;
  const char* msgs = (prefs->mqtt_status_enabled) ? "on" : "off";
  if (s_mqtt_bridge_instance == nullptr || !s_mqtt_bridge_instance->_initialized) {
    snprintf(buf, bufsize, "> msgs: %s (bridge not running)", msgs);
    return;
  }
  MQTTBridge* b = s_mqtt_bridge_instance;
  const char* broker = "n/a";
  if (b->_config_valid) {
    broker = b->_cached_has_brokers ? "connected" : "disconnected";
  }
  const char* us = "off";
  if (prefs->mqtt_analyzer_us_enabled) {
    us = (b->_analyzer_us_client && b->_analyzer_us_client->connected()) ? "connected" : "disconnected";
  }
  const char* eu = "off";
  if (prefs->mqtt_analyzer_eu_enabled) {
    eu = (b->_analyzer_eu_client && b->_analyzer_eu_client->connected()) ? "connected" : "disconnected";
  }
  int q = 0;
#ifdef ESP_PLATFORM
  if (b->_packet_queue_handle != nullptr) {
    q = (int)uxQueueMessagesWaiting(b->_packet_queue_handle);
  }
#else
  q = b->_queue_count;
#endif
  snprintf(buf, bufsize, "> msgs: %s, broker: %s, us: %s, eu: %s, queue: %d",
           msgs, broker, us, eu, q);
}

MQTTBridge::MQTTBridge(NodePrefs *prefs, mesh::PacketManager *mgr, mesh::RTCClock *rtc, mesh::LocalIdentity *identity)
    : BridgeBase(prefs, mgr, rtc), _mqtt_client(nullptr),
      _active_brokers(0), _queue_count(0),
      _last_status_publish(0), _last_status_retry(0), _status_interval(300000), // 5 minutes default
              _ntp_client(_ntp_udp, "pool.ntp.org", 0, 60000), _last_ntp_sync(0), _ntp_synced(false), _ntp_sync_pending(false),
              _timezone(nullptr), _last_raw_len(0), _last_snr(0), _last_rssi(0), _last_raw_timestamp(0),
              _analyzer_us_enabled(false), _analyzer_eu_enabled(false), _identity(identity),
              _analyzer_us_client(nullptr), _analyzer_eu_client(nullptr), _config_valid(false),
              _cached_has_brokers(false), _cached_has_analyzer_servers(false),
              _last_memory_check(0), _skipped_publishes(0), _last_fragmentation_recovery(0),
              _fragmentation_pressure_since(0), _last_critical_check_run(0),
              _last_no_broker_log(0), _last_config_warning(0), _dispatcher(nullptr), _radio(nullptr), _board(nullptr), _ms(nullptr),
              _last_wifi_check(0), _last_wifi_status(WL_DISCONNECTED), _wifi_status_initialized(false),
              _wifi_disconnected_time(0), _last_wifi_reconnect_attempt(0), _wifi_reconnect_backoff_attempt(0),
              _main_broker_reconnect_backoff_attempt(0), _analyzer_us_reconnect_backoff_attempt(0), _analyzer_eu_reconnect_backoff_attempt(0)
#ifdef ESP_PLATFORM
              , _packet_queue_handle(nullptr), _mqtt_task_handle(nullptr), _raw_data_mutex(nullptr), _mqtt_task_stack(nullptr), _packet_queue_storage(nullptr)
#else
              , _queue_head(0), _queue_tail(0)
#endif
{
  
  // Initialize default values
  strncpy(_origin, "MeshCore-Repeater", sizeof(_origin) - 1);
  strncpy(_iata, "XXX", sizeof(_iata) - 1);
  strncpy(_device_id, "DEVICE_ID_PLACEHOLDER", sizeof(_device_id) - 1);
  strncpy(_firmware_version, "unknown", sizeof(_firmware_version) - 1);
  strncpy(_board_model, "unknown", sizeof(_board_model) - 1);
  strncpy(_build_date, "unknown", sizeof(_build_date) - 1);
  _status_enabled = true;
  _packets_enabled = true;
  _raw_enabled = false;
  _tx_enabled = false;  // Disable TX packets by default
  
  // Initialize MQTT server settings with defaults (empty/null values)
  _prefs->mqtt_server[0] = '\0';  // Empty string
  _prefs->mqtt_port = 0;          // Invalid port
  _prefs->mqtt_username[0] = '\0'; // Empty string
  _prefs->mqtt_password[0] = '\0'; // Empty string
  
  // Override with build flags if defined
#ifdef MQTT_SERVER
  strncpy(_prefs->mqtt_server, MQTT_SERVER, sizeof(_prefs->mqtt_server) - 1);
#endif
#ifdef MQTT_PORT
  _prefs->mqtt_port = MQTT_PORT;
#endif
#ifdef MQTT_USERNAME
  strncpy(_prefs->mqtt_username, MQTT_USERNAME, sizeof(_prefs->mqtt_username) - 1);
#endif
#ifdef MQTT_PASSWORD
  strncpy(_prefs->mqtt_password, MQTT_PASSWORD, sizeof(_prefs->mqtt_password) - 1);
#endif
  
  // Initialize packet queue (FreeRTOS queue will be created in begin())
  #ifdef ESP_PLATFORM
  // Queue and mutex will be created in begin()
  #else
  // Initialize circular buffer for non-ESP32 platforms
  memset(_packet_queue, 0, sizeof(_packet_queue));
  for (int i = 0; i < MAX_QUEUE_SIZE; i++) {
    _packet_queue[i].has_raw_data = false;
  }
  #endif
  
  // Initialize throttle log timers
  _last_no_broker_log = 0;
  _last_analyzer_us_log = 0;
  _last_analyzer_eu_log = 0;
  
  // JWT token buffers: allocate in PSRAM when available (plan §2)
  _auth_token_us = (char*)psram_malloc(AUTH_TOKEN_SIZE);
  _auth_token_eu = (char*)psram_malloc(AUTH_TOKEN_SIZE);
  if (_auth_token_us) _auth_token_us[0] = '\0';
  if (_auth_token_eu) _auth_token_eu[0] = '\0';
  
  // Raw radio buffer in PSRAM when available (plan §6)
  _last_raw_data = (uint8_t*)psram_malloc(LAST_RAW_DATA_SIZE);
  
  // Set default broker configuration
  setBrokerDefaults();
}

void MQTTBridge::begin() {
  MQTT_DEBUG_PRINTLN("Initializing MQTT Bridge...");

  // PSRAM diagnostic - helps debug memory fragmentation on boards with external RAM
  #ifdef BOARD_HAS_PSRAM
  {
    bool psram_available = psramFound();
    size_t psram_size = 0;
    size_t psram_free = 0;
    if (psram_available) {
      psram_size = ESP.getPsramSize();
      psram_free = ESP.getFreePsram();
    }
    MQTT_DEBUG_PRINTLN("PSRAM: found=%s, size=%u, free=%u",
      psram_available ? "YES" : "NO", psram_size, psram_free);
    if (!psram_available) {
      MQTT_DEBUG_PRINTLN("PSRAM: board has PSRAM flag but psramFound()=false. "
        "Trying explicit psramInit()...");
      bool init_result = psramInit();
      MQTT_DEBUG_PRINTLN("PSRAM: psramInit() returned %s", init_result ? "true" : "false");
      if (init_result) {
        psram_size = ESP.getPsramSize();
        psram_free = ESP.getFreePsram();
        MQTT_DEBUG_PRINTLN("PSRAM: after init - size=%u, free=%u", psram_size, psram_free);
      }
    }
    // Log internal heap for comparison
    MQTT_DEBUG_PRINTLN("PSRAM: internal_free=%u, internal_max_alloc=%u",
      heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  }
  #else
  MQTT_DEBUG_PRINTLN("PSRAM: not configured for this board (no BOARD_HAS_PSRAM)");
  #endif

  // Check if WiFi credentials are configured first
  if (!isWiFiConfigValid(_prefs)) {
    MQTT_DEBUG_PRINTLN("MQTT Bridge initialization skipped - WiFi credentials not configured");
    return;
  }
  
  // Validate custom MQTT broker configuration (optional)
  _config_valid = isMQTTConfigValid();
  if (!_config_valid) {
    MQTT_DEBUG_PRINTLN("No valid custom MQTT server configured - analyzer servers will still work");
  } else {
    MQTT_DEBUG_PRINTLN("Custom MQTT server configuration is valid");
  }
  
  // Update origin and IATA from preferences
  strncpy(_origin, _prefs->mqtt_origin, sizeof(_origin) - 1);
  _origin[sizeof(_origin) - 1] = '\0';
  strncpy(_iata, _prefs->mqtt_iata, sizeof(_iata) - 1);
  _iata[sizeof(_iata) - 1] = '\0';
  
  // Strip quotes from MQTT server configuration if present
  stripQuotes(_prefs->mqtt_server, sizeof(_prefs->mqtt_server));
  stripQuotes(_prefs->mqtt_username, sizeof(_prefs->mqtt_username));
  stripQuotes(_prefs->mqtt_password, sizeof(_prefs->mqtt_password));
  
  // Strip quotes from origin and IATA if present
  stripQuotes(_origin, sizeof(_origin));
  stripQuotes(_iata, sizeof(_iata));
  
  // Convert IATA code to uppercase (IATA codes are conventionally uppercase)
  for (int i = 0; _iata[i]; i++) {
    _iata[i] = toupper(_iata[i]);
  }
  
  // Update enabled flags from preferences
  _status_enabled = _prefs->mqtt_status_enabled;
  _packets_enabled = _prefs->mqtt_packets_enabled;
  _raw_enabled = _prefs->mqtt_raw_enabled;
  _tx_enabled = _prefs->mqtt_tx_enabled;
  // Set status interval to 5 minutes (300000 ms), or use preference if set and valid
  if (_prefs->mqtt_status_interval >= 1000 && _prefs->mqtt_status_interval <= 3600000) {
    _status_interval = _prefs->mqtt_status_interval;
  } else {
    // Invalid or uninitialized value - fix it in preferences and use default
    _prefs->mqtt_status_interval = 300000; // Fix the preference value
    _status_interval = 300000; // 5 minutes default
  }
  
  // Check for configuration mismatch: bridge.source=tx but mqtt.tx=off
  checkConfigurationMismatch();
  
  MQTT_DEBUG_PRINTLN("Config: Origin=%s, IATA=%s, Device=%s", _origin, _iata, _device_id);
  
  #ifdef ESP_PLATFORM
  // Create FreeRTOS queue; use PSRAM storage when available (plan §5)
  #ifdef BOARD_HAS_PSRAM
  _packet_queue_storage = (uint8_t*)psram_malloc(MAX_QUEUE_SIZE * sizeof(QueuedPacket));
  if (_packet_queue_storage != nullptr) {
    _packet_queue_handle = xQueueCreateStatic(MAX_QUEUE_SIZE, sizeof(QueuedPacket), _packet_queue_storage, &_packet_queue_struct);
  } else {
    _packet_queue_handle = nullptr;
  }
  #else
  _packet_queue_storage = nullptr;
  _packet_queue_handle = nullptr;
  #endif
  if (_packet_queue_handle == nullptr) {
    _packet_queue_handle = xQueueCreate(MAX_QUEUE_SIZE, sizeof(QueuedPacket));
  }
  if (_packet_queue_handle == nullptr) {
    MQTT_DEBUG_PRINTLN("Failed to create packet queue!");
    psram_free(_packet_queue_storage);
    _packet_queue_storage = nullptr;
    return;
  }
  
  // Create mutex for raw radio data protection
  _raw_data_mutex = xSemaphoreCreateMutex();
  if (_raw_data_mutex == nullptr) {
    MQTT_DEBUG_PRINTLN("Failed to create raw data mutex!");
    vQueueDelete(_packet_queue_handle);
    _packet_queue_handle = nullptr;
    return;
  }
  
  // Create main MQTT client only when a custom broker is configured (saves RAM when using analyzer-only)
  if (_config_valid) {
    _mqtt_client = new PsychicMqttClient();
    optimizeMqttClientConfig(_mqtt_client, false);
    _mqtt_client->onConnect([this](bool sessionPresent) {
      MQTT_DEBUG_PRINTLN("MQTT broker connected");
      _main_broker_reconnect_backoff_attempt = 0;
      for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
        if (_brokers[i].enabled && !_brokers[i].connected) {
          _brokers[i].connected = true;
          _active_brokers++;
          _cached_has_brokers = isAnyBrokerConnected();
          break;
        }
      }
    });
    _mqtt_client->onDisconnect([this](bool sessionPresent) {
      MQTT_DEBUG_PRINTLN("MQTT broker disconnected");
      for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
        if (_brokers[i].connected) {
          _brokers[i].connected = false;
          _active_brokers--;
          _cached_has_brokers = isAnyBrokerConnected();
          break;
        }
      }
    });
  }
  
  // Set default broker from preferences or build flags
  setBroker(0, _prefs->mqtt_server, _prefs->mqtt_port, _prefs->mqtt_username, _prefs->mqtt_password, true);
  
  // Setup Let's Mesh Analyzer servers configuration
  _analyzer_us_enabled = _prefs->mqtt_analyzer_us_enabled;
  _analyzer_eu_enabled = _prefs->mqtt_analyzer_eu_enabled;
  MQTT_DEBUG_PRINTLN("Analyzer servers - US: %s, EU: %s", 
                     _analyzer_us_enabled ? "enabled" : "disabled",
                     _analyzer_eu_enabled ? "enabled" : "disabled");
  
  // Create FreeRTOS task for MQTT/WiFi processing on Core 0
  #ifndef MQTT_TASK_CORE
  #define MQTT_TASK_CORE 0
  #endif
  #ifndef MQTT_TASK_STACK_SIZE
  #define MQTT_TASK_STACK_SIZE 8192  // Reverted: 6144 was too small, caused boot loop after NTP sync
  #endif
  #ifndef MQTT_TASK_PRIORITY
  #define MQTT_TASK_PRIORITY 1
  #endif
  
  // Task stack: use dynamic allocation (internal RAM). PSRAM stack was disabled because it
  // causes resets on some boards (e.g. Heltec V4) when the task runs from PSRAM stack.
  _mqtt_task_stack = nullptr;
  _mqtt_task_handle = nullptr;
  BaseType_t create_result = xTaskCreatePinnedToCore(
    mqttTask,
    "MQTTBridge",
    MQTT_TASK_STACK_SIZE,
    this,
    MQTT_TASK_PRIORITY,
    &_mqtt_task_handle,
    MQTT_TASK_CORE
  );
  if (create_result != pdPASS) _mqtt_task_handle = nullptr;
  if (_mqtt_task_handle == nullptr) {
    MQTT_DEBUG_PRINTLN("Failed to create MQTT task!");
    psram_free(_mqtt_task_stack);
    _mqtt_task_stack = nullptr;
    vQueueDelete(_packet_queue_handle);
    _packet_queue_handle = nullptr;
    psram_free(_packet_queue_storage);
    _packet_queue_storage = nullptr;
    vSemaphoreDelete(_raw_data_mutex);
    _raw_data_mutex = nullptr;
    if (_mqtt_client) {
      delete _mqtt_client;
      _mqtt_client = nullptr;
    }
    return;
  }
  
  MQTT_DEBUG_PRINTLN("MQTT task created on Core %d", MQTT_TASK_CORE);
  #else
  // Non-ESP32: Initialize WiFi directly (no task)
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setAutoConnect(true);
  WiFi.begin(_prefs->wifi_ssid, _prefs->wifi_password);
  
  // Create main MQTT client only when a custom broker is configured (saves RAM when using analyzer-only)
  if (_config_valid) {
    _mqtt_client = new PsychicMqttClient();
    optimizeMqttClientConfig(_mqtt_client, false);
    _mqtt_client->onConnect([this](bool sessionPresent) {
      MQTT_DEBUG_PRINTLN("MQTT broker connected");
      _main_broker_reconnect_backoff_attempt = 0;
      for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
        if (_brokers[i].enabled && !_brokers[i].connected) {
          _brokers[i].connected = true;
          _active_brokers++;
          _cached_has_brokers = isAnyBrokerConnected();
          break;
        }
      }
    });
    _mqtt_client->onDisconnect([this](bool sessionPresent) {
      MQTT_DEBUG_PRINTLN("MQTT broker disconnected");
      for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
        if (_brokers[i].connected) {
          _brokers[i].connected = false;
          _active_brokers--;
          _cached_has_brokers = isAnyBrokerConnected();
          break;
        }
      }
    });
  }
  
  setBroker(0, _prefs->mqtt_server, _prefs->mqtt_port, _prefs->mqtt_username, _prefs->mqtt_password, true);
  _analyzer_us_enabled = _prefs->mqtt_analyzer_us_enabled;
  _analyzer_eu_enabled = _prefs->mqtt_analyzer_eu_enabled;
  setupAnalyzerClients();
  connectToBrokers();
  #endif
  
  _initialized = true;
  s_mqtt_bridge_instance = this;
  MQTT_DEBUG_PRINTLN("MQTT Bridge initialized");
}

void MQTTBridge::end() {
  MQTT_DEBUG_PRINTLN("Stopping MQTT Bridge...");
  s_mqtt_bridge_instance = nullptr;
  
  #ifdef ESP_PLATFORM
  // Delete FreeRTOS task first (it will clean up WiFi/MQTT connections)
  if (_mqtt_task_handle != nullptr) {
    vTaskDelete(_mqtt_task_handle);
    _mqtt_task_handle = nullptr;
    // Give task time to clean up
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  // Free PSRAM task stack (plan §3)
  psram_free(_mqtt_task_stack);
  _mqtt_task_stack = nullptr;
  
  // Clean up queued packets from FreeRTOS queue
  // NOTE: Do NOT free queued.packet - the Dispatcher owns those packets.
  // We just discard our references to them.
  if (_packet_queue_handle != nullptr) {
    QueuedPacket queued;
    while (xQueueReceive(_packet_queue_handle, &queued, 0) == pdTRUE) {
      queued.packet = nullptr;
      _queue_count--;
    }
    vQueueDelete(_packet_queue_handle);
    _packet_queue_handle = nullptr;
  }
  psram_free(_packet_queue_storage);
  _packet_queue_storage = nullptr;
  
  // Delete mutex
  if (_raw_data_mutex != nullptr) {
    vSemaphoreDelete(_raw_data_mutex);
    _raw_data_mutex = nullptr;
  }
  #else
  // Disconnect from all brokers (main client only exists when _config_valid)
  if (_mqtt_client) {
    for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
      if (_brokers[i].enabled && _brokers[i].connected) {
        _mqtt_client->disconnect();
        _brokers[i].connected = false;
      }
    }
  }
  
  // Disconnect analyzer clients
  if (_analyzer_us_client) {
    _analyzer_us_client->disconnect();
    delete _analyzer_us_client;
    _analyzer_us_client = nullptr;
  }
  if (_analyzer_eu_client) {
    _analyzer_eu_client->disconnect();
    delete _analyzer_eu_client;
    _analyzer_eu_client = nullptr;
  }
  
  // Clean up queued packet references
  // NOTE: Do NOT free the packets - the Dispatcher owns those packets.
  // We just discard our references to them.
  for (int i = 0; i < _queue_count; i++) {
    int index = (_queue_head + i) % MAX_QUEUE_SIZE;
    _packet_queue[index].packet = nullptr;
    memset(&_packet_queue[index], 0, sizeof(QueuedPacket));
  }
  
  _queue_count = 0;
  _queue_head = 0;
  _queue_tail = 0;
  memset(_packet_queue, 0, sizeof(_packet_queue));
  #endif
  
  // Clean up timezone object to prevent memory leak
  if (_timezone) {
    delete _timezone;
    _timezone = nullptr;
  }
  
  // Clean up resources
  if (_mqtt_client) {
    delete _mqtt_client;
    _mqtt_client = nullptr;
  }
  
  // Free PSRAM-backed JWT token buffers (plan §2)
  psram_free(_auth_token_us);
  _auth_token_us = nullptr;
  psram_free(_auth_token_eu);
  _auth_token_eu = nullptr;
  psram_free(_last_raw_data);
  _last_raw_data = nullptr;
  
  _initialized = false;
  MQTT_DEBUG_PRINTLN("MQTT Bridge stopped");
}

#ifdef ESP_PLATFORM
void MQTTBridge::mqttTask(void* parameter) {
  MQTTBridge* bridge = static_cast<MQTTBridge*>(parameter);
  if (bridge) {
    bridge->mqttTaskLoop();
  }
  // Task should never return, but if it does, delete itself
  vTaskDelete(nullptr);
}

void MQTTBridge::initializeWiFiInTask() {
  MQTT_DEBUG_PRINTLN("Initializing WiFi in MQTT task...");
  
  // Initialize WiFi
  WiFi.mode(WIFI_STA);
  
  // Enable automatic reconnection - ESP32 will handle reconnection automatically
  WiFi.setAutoReconnect(true);
  WiFi.setAutoConnect(true);
  
  // Set up WiFi event handlers for better diagnostics and immediate disconnection detection
  WiFi.onEvent([this](WiFiEvent_t event, WiFiEventInfo_t info) {
    switch(event) {
      case ARDUINO_EVENT_WIFI_STA_GOT_IP:
        MQTT_DEBUG_PRINTLN("WiFi connected: %s", IPAddress(info.got_ip.ip_info.ip.addr).toString().c_str());
        // Set flag to trigger NTP sync from loop() instead of doing it here
        if (!_ntp_synced && !_ntp_sync_pending) {
          _ntp_sync_pending = true;
        }
        break;
      default:
        break;
    }
  });
  
  WiFi.begin(_prefs->wifi_ssid, _prefs->wifi_password);
  
  // WiFi connection is asynchronous - don't block here
  // Auto-reconnect will handle connection in the background
  
  // Setup PsychicMqttClient WebSocket clients for analyzer servers
  setupAnalyzerClients();
  
  MQTT_DEBUG_PRINTLN("WiFi initialization started in task");
}

void MQTTBridge::mqttTaskLoop() {
  // Initialize WiFi first
  initializeWiFiInTask();
  
  // Wait a bit for WiFi to start connecting
  vTaskDelay(pdMS_TO_TICKS(1000));
  
  // Main task loop
  #ifdef MQTT_MEMORY_DEBUG
  static unsigned long last_agent_log = 0;
  #endif
  while (true) {
    // Run the main MQTT bridge loop logic
    // This replaces the original loop() method but runs in the task
    
    #ifdef MQTT_MEMORY_DEBUG
    // #region agent log
    unsigned long now_loop = millis();
    if (now_loop - last_agent_log >= 60000) {
      last_agent_log = now_loop;
      size_t free_h = ESP.getFreeHeap();
      size_t max_alloc = ESP.getMaxAllocHeap();
      unsigned long internal_f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
      unsigned long spiram_f = 0;
      #ifdef BOARD_HAS_PSRAM
      spiram_f = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
      #endif
      agentLogHeap("MQTTBridge.cpp:505", "mqtt_loop_60s", "H5", free_h, max_alloc, internal_f, spiram_f);
    }
    // #endregion
    #endif
    
    unsigned long now = millis();
    handleWiFiConnection(now);

    // Check for pending NTP sync (triggered from WiFi event handler)
    if (_ntp_sync_pending && WiFi.status() == WL_CONNECTED) {
      _ntp_sync_pending = false;
      syncTimeWithNTP();
    }
    
    // Check if analyzer server settings have changed in preferences
    static unsigned long last_analyzer_check = 0;
    if (now - last_analyzer_check > 5000) {
      last_analyzer_check = now;
      if (_analyzer_us_enabled != _prefs->mqtt_analyzer_us_enabled || 
          _analyzer_eu_enabled != _prefs->mqtt_analyzer_eu_enabled) {
        MQTT_DEBUG_PRINTLN("Analyzer settings changed - updating...");
        setupAnalyzerServers();
      }
    }
    
    // Maintain broker connections
    connectToBrokers();
    
    // Maintain analyzer server connections
    maintainAnalyzerConnections();
    
    // Process packet queue
    processPacketQueue();
    
    // Periodic configuration check (throttled to avoid spam)
    checkConfigurationMismatch();
    
    // Periodic NTP sync (every hour) - only when connected
    if (WiFi.status() == WL_CONNECTED && now - _last_ntp_sync > 3600000) {
      syncTimeWithNTP();
    }
    
    // Publish status updates (handle millis() overflow correctly)
    if (_status_enabled) {
      // Use cached destination status (updated in connection callbacks) - early exit if no destinations
      // Only refresh cache if status publish is enabled to avoid unnecessary checks
      bool has_custom_brokers = _cached_has_brokers && _config_valid;
      bool has_destinations = has_custom_brokers || _cached_has_analyzer_servers;
      
      // Early exit if no destinations - skip all the expensive logic below
      if (!has_destinations) {
        if (_last_status_retry != 0) {
          _last_status_retry = 0;
        }
      } else {
        bool should_publish = false;
        
        // First, check if we need to respect retry interval (prevents spam when publish keeps failing)
        if (_last_status_retry != 0) {
          unsigned long retry_elapsed = (now >= _last_status_retry) ?
                                       (now - _last_status_retry) :
                                       (ULONG_MAX - _last_status_retry + now + 1);
          if (retry_elapsed < STATUS_RETRY_INTERVAL) {
            // Too soon to retry - wait longer
            should_publish = false;
          } else {
            // Retry interval has passed - allow retry
            should_publish = true;
          }
        } else {
          // No pending retry - check if normal interval has passed
          // Handle case where _last_status_publish is 0 (first publish attempt)
          if (_last_status_publish == 0) {
            // First publish attempt - allow it immediately
            should_publish = true;
          } else {
            // Calculate elapsed time since last successful publish
            unsigned long elapsed = (now >= _last_status_publish) ? 
                                   (now - _last_status_publish) : 
                                   (ULONG_MAX - _last_status_publish + now + 1);
            should_publish = (elapsed >= _status_interval);
          }
        }
        
        if (should_publish) {
          // Only log elapsed time if we have a previous successful publish
          if (_last_status_publish != 0) {
            unsigned long elapsed = (now >= _last_status_publish) ? 
                                   (now - _last_status_publish) : 
                                   (ULONG_MAX - _last_status_publish + now + 1);
            MQTT_DEBUG_PRINTLN("Status publish timer expired (elapsed: %lu ms, interval: %lu ms)", elapsed, _status_interval);
          } else {
            MQTT_DEBUG_PRINTLN("Status publish attempt (first publish or retry)");
          }
          
          _last_status_retry = now;
          if (publishStatus()) {
            _last_status_publish = now;
            _last_status_retry = 0;
            MQTT_DEBUG_PRINTLN("Status published successfully, next publish in %lu ms", _status_interval);
            // If we're in the hole but just proved connectivity, recover sooner than the dedicated pressure timer
            size_t max_alloc = ESP.getMaxAllocHeap();
            if (max_alloc < 58000 && (now - _last_fragmentation_recovery) > 300000) {
              _last_fragmentation_recovery = now;
              _fragmentation_pressure_since = 0;  // Reset pressure timer so dedicated check doesn't fire again soon
              MQTT_DEBUG_PRINTLN("Fragmentation recovery after status (max_alloc=%d)", (int)max_alloc);
              recreateMqttClientsForFragmentationRecovery();
            }
          } else {
            MQTT_DEBUG_PRINTLN("Status publish failed, will retry in %lu ms", STATUS_RETRY_INTERVAL);
            // _last_status_retry already set above - will prevent immediate retry
          }
        }
      }
    }
    
    runCriticalMemoryCheckAndRecovery();

    // Update cached analyzer server status periodically (every 5 seconds)
    // This ensures cache stays accurate even if callbacks miss updates
    static unsigned long last_analyzer_status_update = 0;
    if (now - last_analyzer_status_update > 5000) {
      _cached_has_analyzer_servers = (_analyzer_us_enabled && _analyzer_us_client && _analyzer_us_client->connected()) ||
                                     (_analyzer_eu_enabled && _analyzer_eu_client && _analyzer_eu_client->connected());
      last_analyzer_status_update = now;
    }
    
    // Adaptive task delay based on work done
    // Check if we have work to do (queue has packets or status needs publishing)
    bool has_work = (_queue_count > 0);
    if (!has_work && _status_enabled) {
      // Check if status publish is needed soon
      if (_last_status_publish == 0 || 
          (now - _last_status_publish >= (_status_interval - 10000))) {  // Within 10s of next publish
        has_work = true;
      }
    }
    
    // Adaptive delay: shorter when work pending, longer when idle
    if (has_work) {
      vTaskDelay(pdMS_TO_TICKS(5));   // 5ms delay when work pending - process faster
    } else {
      vTaskDelay(pdMS_TO_TICKS(50));  // 50ms delay when idle - save CPU
    }
  }
}
#endif

bool MQTTBridge::isConfigValid() const {
  return _config_valid;
}

bool MQTTBridge::isConfigValid(const NodePrefs* prefs) {
  // Check if MQTT server is configured (not default placeholder)
  if (strlen(prefs->mqtt_server) == 0 || 
      strcmp(prefs->mqtt_server, "your-mqtt-broker.com") == 0) {
    return false;
  }
  
  // Check if MQTT port is valid
  if (prefs->mqtt_port == 0 || prefs->mqtt_port > 65535) {
    return false;
  }
  
  // Username and password are optional - anonymous mode is supported
  // Only reject if they contain the default placeholder values
  if (strcmp(prefs->mqtt_username, "your-username") == 0) {
    return false;
  }
  
  if (strcmp(prefs->mqtt_password, "your-password") == 0) {
    return false;
  }
  
  return true;
}

void MQTTBridge::checkConfigurationMismatch() {
  // Check if bridge.source is set to tx (logTx) but mqtt.tx is disabled
  // This would prevent packet publishing since sendPacket() requires both packets_enabled and tx_enabled
  if (_prefs->bridge_pkt_src == 0 && _packets_enabled && !_tx_enabled) {
    unsigned long now = millis();
    // Always log on first detection, then throttle to every 5 minutes to avoid spam
    if (_last_config_warning == 0 || (now - _last_config_warning > CONFIG_WARNING_INTERVAL)) {
      MQTT_DEBUG_PRINTLN("MQTT: Configuration mismatch detected! bridge.source=tx (logTx) but mqtt.tx=off. Packets will not be published. Run 'set bridge.source rx' or 'set mqtt.tx on' to fix.");
      _last_config_warning = now;
    }
  } else {
    // Configuration is correct, reset warning timer so we log immediately if it becomes wrong again
    _last_config_warning = 0;
  }
}

bool MQTTBridge::handleWiFiConnection(unsigned long now) {
  wl_status_t current_wifi_status = WiFi.status();
  bool transitioned_to_connected = false;

  if (current_wifi_status == WL_CONNECTED && s_wifi_connected_at == 0) {
    s_wifi_connected_at = now;
  }
  if (!_wifi_status_initialized) {
    _last_wifi_status = current_wifi_status;
    _wifi_status_initialized = true;
  }
  if (now - _last_wifi_check <= 10000) {
    return false;
  }
  _last_wifi_check = now;

  if (current_wifi_status == WL_CONNECTED) {
    if (_last_wifi_status != WL_CONNECTED) {
      transitioned_to_connected = true;
      _wifi_disconnected_time = 0;
      s_wifi_connected_at = now;
      _wifi_reconnect_backoff_attempt = 0;
      #ifdef ESP_PLATFORM
      wifi_ps_type_t ps_mode;
      uint8_t ps_pref = _prefs->wifi_power_save;
      if (ps_pref == 1) {
        ps_mode = WIFI_PS_NONE;
      } else if (ps_pref == 2) {
        ps_mode = WIFI_PS_MAX_MODEM;
      } else {
        ps_mode = WIFI_PS_MIN_MODEM;
      }
      esp_wifi_set_ps(ps_mode);
      #ifdef MQTT_WIFI_TX_POWER
      WiFi.setTxPower(MQTT_WIFI_TX_POWER);
      #else
      WiFi.setTxPower(WIFI_POWER_11dBm);
      #endif
      #endif
    }
    if (s_wifi_connected_at == 0) {
      s_wifi_connected_at = now;
    }
    _last_wifi_status = WL_CONNECTED;
  } else {
    if (_last_wifi_status == WL_CONNECTED) {
      _wifi_disconnected_time = now;
      s_wifi_connected_at = 0;
      if (_analyzer_us_client) {
        _analyzer_us_client->disconnect();
      }
      if (_analyzer_eu_client) {
        _analyzer_eu_client->disconnect();
      }
    } else if (_wifi_disconnected_time > 0) {
      unsigned long disconnected_duration = now - _wifi_disconnected_time;
      static const unsigned long WIFI_BACKOFF_MS[] = { 15000, 30000, 60000, 120000, 300000 };
      unsigned int idx = (_wifi_reconnect_backoff_attempt < 5) ? _wifi_reconnect_backoff_attempt : 4;
      unsigned long delay_ms = WIFI_BACKOFF_MS[idx];
      unsigned long elapsed_since_attempt = (now >= _last_wifi_reconnect_attempt)
          ? (now - _last_wifi_reconnect_attempt)
          : (ULONG_MAX - _last_wifi_reconnect_attempt + now + 1);
      if (disconnected_duration >= delay_ms && elapsed_since_attempt >= delay_ms) {
        _last_wifi_reconnect_attempt = now;
        if (_wifi_reconnect_backoff_attempt < 5) {
          _wifi_reconnect_backoff_attempt++;
        }
        WiFi.disconnect();
        WiFi.begin(_prefs->wifi_ssid, _prefs->wifi_password);
      }
    }
    _last_wifi_status = current_wifi_status;
  }
  return transitioned_to_connected;
}

bool MQTTBridge::isReady() const {
  return _initialized && isWiFiConfigValid(_prefs);
}

void MQTTBridge::loop() {
  if (!_initialized) return;
  
  #ifdef ESP_PLATFORM
  // On ESP32, loop() is a no-op - all processing happens in the FreeRTOS task
  // This method is kept for API compatibility but does nothing
  return;
  #else
  // Non-ESP32: loop() drives WiFi and MQTT (same logic as mqttTaskLoop via handleWiFiConnection)
  unsigned long now = millis();
  if (handleWiFiConnection(now) && !_ntp_synced) {
    syncTimeWithNTP();
  }
  if (_ntp_sync_pending && WiFi.status() == WL_CONNECTED) {
    _ntp_sync_pending = false;
    syncTimeWithNTP();
  }
  // Check if analyzer server settings have changed in preferences
  static unsigned long last_analyzer_check = 0;
  if (millis() - last_analyzer_check > 5000) {
    last_analyzer_check = millis();
    if (_analyzer_us_enabled != _prefs->mqtt_analyzer_us_enabled || 
        _analyzer_eu_enabled != _prefs->mqtt_analyzer_eu_enabled) {
      MQTT_DEBUG_PRINTLN("Analyzer settings changed - updating...");
      setupAnalyzerServers();
    }
  }
  
  // Maintain broker connections
  connectToBrokers();
  
  // Maintain analyzer server connections
  maintainAnalyzerConnections();
  
  // Process packet queue
  processPacketQueue();
  
  // Periodic configuration check (throttled to avoid spam)
  checkConfigurationMismatch();
  
  // Periodic NTP sync (every hour) - only when connected
  if (WiFi.status() == WL_CONNECTED && millis() - _last_ntp_sync > 3600000) {
    syncTimeWithNTP();
  }
  
  // Publish status updates (handle millis() overflow correctly)
  if (_status_enabled) {
    // Use cached destination status (updated in connection callbacks) - early exit if no destinations
    bool has_custom_brokers = _cached_has_brokers && _config_valid;
    bool has_destinations = has_custom_brokers || _cached_has_analyzer_servers;
    
    // Only attempt to publish if we have destinations available
    if (has_destinations) {
      unsigned long now = millis();
      bool should_publish = false;
      
      // First, check if we need to respect retry interval (prevents spam when publish keeps failing)
      if (_last_status_retry != 0) {
        unsigned long retry_elapsed = (now >= _last_status_retry) ?
                                     (now - _last_status_retry) :
                                     (ULONG_MAX - _last_status_retry + now + 1);
        if (retry_elapsed < STATUS_RETRY_INTERVAL) {
          // Too soon to retry - wait longer
          should_publish = false;
        } else {
          // Retry interval has passed - allow retry
          should_publish = true;
        }
      } else {
        // No pending retry - check if normal interval has passed
        // Handle case where _last_status_publish is 0 (first publish attempt)
        if (_last_status_publish == 0) {
          // First publish attempt - allow it immediately
          should_publish = true;
        } else {
          // Calculate elapsed time since last successful publish
          unsigned long elapsed = (now >= _last_status_publish) ? 
                               (now - _last_status_publish) : 
                               (ULONG_MAX - _last_status_publish + now + 1);
          should_publish = (elapsed >= _status_interval);
        }
      }
      
      if (should_publish) {
        // Only log elapsed time if we have a previous successful publish
        if (_last_status_publish != 0) {
          unsigned long elapsed = (now >= _last_status_publish) ? 
                                 (now - _last_status_publish) : 
                                 (ULONG_MAX - _last_status_publish + now + 1);
          MQTT_DEBUG_PRINTLN("Status publish timer expired (elapsed: %lu ms, interval: %lu ms)", elapsed, _status_interval);
        } else {
          MQTT_DEBUG_PRINTLN("Status publish attempt (first publish or retry)");
        }
        
        _last_status_retry = now;
        if (publishStatus()) {
          _last_status_publish = now;
          _last_status_retry = 0;
          MQTT_DEBUG_PRINTLN("Status published successfully, next publish in %lu ms", _status_interval);
        } else {
          MQTT_DEBUG_PRINTLN("Status publish failed, will retry in %lu ms", STATUS_RETRY_INTERVAL);
          // _last_status_retry already set above - will prevent immediate retry
        }
      }
    } else {
      if (_last_status_retry != 0) {
        _last_status_retry = 0;
      }
    }
    
    // Check if status hasn't been published successfully for too long
    // If status publishes have been failing for > 10 minutes, force full MQTT reinitialization
    if (_status_enabled && _last_status_publish != 0) {
      unsigned long time_since_last_success = (now >= _last_status_publish) ?
                                              (now - _last_status_publish) :
                                              (ULONG_MAX - _last_status_publish + now + 1);
      const unsigned long MAX_FAILURE_TIME_MS = 600000;  // 10 minutes
      
      if (time_since_last_success > MAX_FAILURE_TIME_MS) {
        static unsigned long last_reinit_log = 0;
        if (now - last_reinit_log > 300000) {  // Log every 5 minutes max
          MQTT_DEBUG_PRINTLN("CRITICAL: Status publish has been failing for %lu ms (>%lu ms), forcing MQTT session reinitialization",
                             time_since_last_success, MAX_FAILURE_TIME_MS);
          last_reinit_log = now;
        }
        
        recreateMqttClientsForFragmentationRecovery();
        _last_status_publish = 0;
        _last_status_retry = 0;
        MQTT_DEBUG_PRINTLN("MQTT session reinitialized (clients recreated) - reconnection on next loop");
      }
    }
  }
  #endif
  
  #ifdef ESP_PLATFORM
  runCriticalMemoryCheckAndRecovery();
  #endif
}

void MQTTBridge::onPacketReceived(mesh::Packet *packet) {
  if (!_initialized || !_packets_enabled) return;
  
  // Check if we have any valid brokers to send to
  bool has_valid_brokers = _config_valid || 
                          (_analyzer_us_enabled && _analyzer_us_client) ||
                          (_analyzer_eu_enabled && _analyzer_eu_client);
  
  if (!has_valid_brokers) return;
  
  // Queue packet for transmission
  queuePacket(packet, false);
}

void MQTTBridge::sendPacket(mesh::Packet *packet) {
  if (!_initialized || !_packets_enabled || !_tx_enabled) return;
  
  // Queue packet for transmission (only if TX enabled)
  queuePacket(packet, true);
}

bool MQTTBridge::isMQTTConfigValid() {
  // Check if MQTT server is configured (not default placeholder)
  if (strlen(_prefs->mqtt_server) == 0 || 
      strcmp(_prefs->mqtt_server, "your-mqtt-broker.com") == 0) {
    return false;
  }
  
  // Check if MQTT port is valid
  if (_prefs->mqtt_port == 0 || _prefs->mqtt_port > 65535) {
    return false;
  }
  
  // Username and password are optional - anonymous mode is supported
  // Only reject if they contain the default placeholder values
  if (strcmp(_prefs->mqtt_username, "your-username") == 0) {
    return false;
  }
  
  if (strcmp(_prefs->mqtt_password, "your-password") == 0) {
    return false;
  }
  
  return true;
}

bool MQTTBridge::isIATAValid() const {
  // Check if IATA code is configured (not empty, not default "XXX")
  if (strlen(_iata) == 0 || strcmp(_iata, "XXX") == 0) {
    return false;
  }
  return true;
}

void MQTTBridge::ensureMainMqttClient() {
  if (!_config_valid || _mqtt_client != nullptr) return;
  _mqtt_client = new PsychicMqttClient();
  optimizeMqttClientConfig(_mqtt_client, false);
  _mqtt_client->onConnect([this](bool sessionPresent) {
    MQTT_DEBUG_PRINTLN("MQTT broker connected");
    _main_broker_reconnect_backoff_attempt = 0;
    for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
      if (_brokers[i].enabled && !_brokers[i].connected) {
        _brokers[i].connected = true;
        _active_brokers++;
        _cached_has_brokers = isAnyBrokerConnected();
        break;
      }
    }
  });
  _mqtt_client->onDisconnect([this](bool sessionPresent) {
    MQTT_DEBUG_PRINTLN("MQTT broker disconnected");
    for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
      if (_brokers[i].connected) {
        _brokers[i].connected = false;
        _active_brokers--;
        _cached_has_brokers = isAnyBrokerConnected();
        break;
      }
    }
  });
  MQTT_DEBUG_PRINTLN("Main MQTT client recreated (fresh buffers)");
}

#ifdef ESP_PLATFORM
void MQTTBridge::runCriticalMemoryCheckAndRecovery() {
  const unsigned long CRITICAL_CHECK_INTERVAL_MS = 60000;   // Sample heap at most every 60s
  const unsigned long PRESSURE_WINDOW_CRITICAL_MS = 180000;  // Recover if critical pressure for 3 min
  const unsigned long PRESSURE_WINDOW_MODERATE_MS = 300000; // Recover if moderate pressure for 5 min
  const unsigned long RECOVERY_THROTTLE_MS = 300000;         // 5 min between recovery runs
  const unsigned long CRITICAL_LOG_INTERVAL_MS = 900000;     // Log CRITICAL/WARNING/client count at most every 15 min
  const size_t PRESSURE_THRESHOLD_CRITICAL = 58000;           // max_alloc below this = critical (3 min window)
  const size_t PRESSURE_THRESHOLD_MODERATE = 70000;          // max_alloc below this = under pressure (clear when >= this)

  unsigned long now = millis();
  if (now - _last_critical_check_run < CRITICAL_CHECK_INTERVAL_MS) {
    return;
  }
  _last_critical_check_run = now;

  size_t free_h = ESP.getFreeHeap();
  size_t max_alloc = ESP.getMaxAllocHeap();
  #ifdef MQTT_MEMORY_DEBUG
  unsigned long internal_f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  unsigned long spiram_f = 0;
  #ifdef BOARD_HAS_PSRAM
  spiram_f = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  #endif
  agentLogHeap("MQTTBridge.cpp:runCriticalMemoryCheckAndRecovery", "critical_memory_check", "H1_H4", free_h, max_alloc, internal_f, spiram_f);
  #endif

  // Pressure timer: track how long max_alloc has been below moderate threshold
  if (max_alloc >= PRESSURE_THRESHOLD_MODERATE) {
    _fragmentation_pressure_since = 0;
  } else {
    if (_fragmentation_pressure_since == 0) {
      _fragmentation_pressure_since = now;
    }
  }

  // Rate-limited diagnostic logging (every 15 min)
  static unsigned long last_critical_log = 0;
  if (now - last_critical_log >= CRITICAL_LOG_INTERVAL_MS) {
    last_critical_log = now;
    if (max_alloc < 40000) {
      MQTT_DEBUG_PRINTLN("CRITICAL: Low memory! Free: %d, Max: %d", (int)free_h, (int)max_alloc);
    } else if (max_alloc < 60000) {
      MQTT_DEBUG_PRINTLN("WARNING: Memory pressure. Free: %d, Max: %d", (int)free_h, (int)max_alloc);
    }
    int n_main = (_mqtt_client != nullptr) ? 1 : 0;
    int n_us = (_analyzer_us_client != nullptr) ? 1 : 0;
    int n_eu = (_analyzer_eu_client != nullptr) ? 1 : 0;
    MQTT_DEBUG_PRINTLN("MQTT clients active: %d (main=%d us=%d eu=%d)", n_main + n_us + n_eu, n_main, n_us, n_eu);
  }

  // Dedicated recovery: critical (<58k) recovers after 3 min; moderate (58k–70k) after 5 min
  unsigned long required_window_ms = (max_alloc < PRESSURE_THRESHOLD_CRITICAL)
      ? PRESSURE_WINDOW_CRITICAL_MS
      : PRESSURE_WINDOW_MODERATE_MS;
  if (_fragmentation_pressure_since != 0 &&
      (now - _fragmentation_pressure_since) >= required_window_ms &&
      (now - _last_fragmentation_recovery) >= RECOVERY_THROTTLE_MS) {
    _last_fragmentation_recovery = now;
    _fragmentation_pressure_since = 0;
    MQTT_DEBUG_PRINTLN("Fragmentation recovery: recreating MQTT clients (max_alloc=%d, pressure %lu min)", (int)max_alloc, (unsigned long)(required_window_ms / 60000));
    recreateMqttClientsForFragmentationRecovery();
  }
}
#endif

void MQTTBridge::recreateMqttClientsForFragmentationRecovery() {
  // Disconnect, delete, and recreate all MQTT clients so they allocate fresh buffers.
  // This can recover max_alloc when the internal heap was fragmented (e.g. after poor
  // WiFi, failed publishes, and reconnect).
  if (_mqtt_client) {
    if (_mqtt_client->connected()) _mqtt_client->disconnect();
    #ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(100));
    #else
    delay(100);
    #endif
    delete _mqtt_client;
    _mqtt_client = nullptr;
  }
  if (_analyzer_us_client) {
    if (_analyzer_us_client->connected()) _analyzer_us_client->disconnect();
    delete _analyzer_us_client;
    _analyzer_us_client = nullptr;
  }
  if (_analyzer_eu_client) {
    if (_analyzer_eu_client->connected()) _analyzer_eu_client->disconnect();
    delete _analyzer_eu_client;
    _analyzer_eu_client = nullptr;
  }
  for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
    if (_brokers[i].enabled) {
      _brokers[i].connected = false;
      _brokers[i].initial_connect_done = false;
      _brokers[i].last_attempt = 0;
    }
  }
  _active_brokers = 0;
  _cached_has_brokers = false;
  _cached_has_analyzer_servers = false;
  setupAnalyzerServers();
  // Recreate analyzer client objects (we just set them to nullptr above; setupAnalyzerServers
  // only calls setupAnalyzerClients when enabled flags change, so we must call it explicitly).
  setupAnalyzerClients();
}

void MQTTBridge::connectToBrokers() {
  // Recreate main client if it was deleted during reinit (allows fresh heap allocations)
  ensureMainMqttClient();
  // Check if MQTT configuration is valid before attempting connection
  if (!_config_valid) {
    return;
  }
  
  // Check WiFi status first - don't attempt MQTT connection if WiFi is disconnected
  if (WiFi.status() != WL_CONNECTED) {
    // WiFi is not connected - skip MQTT connection attempts
    // WiFi auto-reconnect will handle WiFi, then we can connect MQTT
    static unsigned long last_wifi_warning = 0;
    unsigned long now = millis();
    if (now - last_wifi_warning > 300000) { // Log every 5 minutes max
      MQTT_DEBUG_PRINTLN("Skipping MQTT broker connection - WiFi not connected");
      last_wifi_warning = now;
    }
    return;
  }
  
  // Main broker reconnect uses exponential backoff: 15s, 30s, 60s, 120s, 300s (reset on connect)
  static const unsigned long MAIN_BROKER_BACKOFF_MS[] = { 15000, 30000, 60000, 120000, 300000 };

  // For now, connect to the first enabled broker
  // TODO: Implement multi-broker support with PsychicMqttClient
  for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
    if (!_brokers[i].enabled) continue;

    // Only call connect() once for initial connection.
    // After that, ESP-IDF's auto-reconnect handles reconnection automatically.
    // If we forced disconnect (e.g. on publish failure), we must call connect() again
    // since disconnect() stops the client; use exponential backoff to avoid reconnect storms.
    if (!_brokers[i].initial_connect_done) {
      MQTT_DEBUG_PRINTLN("Initial connection to broker %d: %s:%d", i, _brokers[i].host, _brokers[i].port);

      // Set broker URI and connect using PsychicMqttClient API
      char broker_uri[128];
      snprintf(broker_uri, sizeof(broker_uri), "mqtt://%s:%d", _brokers[i].host, _brokers[i].port);
      _mqtt_client->setServer(broker_uri);

      // Set credentials if provided
      if (strlen(_brokers[i].username) > 0) {
        _mqtt_client->setCredentials(_brokers[i].username, _brokers[i].password);
      }

      // Connect to the broker (PsychicMqttClient uses async connection)
      // ESP-IDF MQTT client has auto-reconnect enabled by default (10s retry)
      _mqtt_client->connect();

      _brokers[i].initial_connect_done = true;
      _brokers[i].last_attempt = millis();
      MQTT_DEBUG_PRINTLN("Initiated connection to broker %d (auto-reconnect will handle future reconnections)", i);
    } else if (_mqtt_client && !_mqtt_client->connected()) {
      unsigned long now = millis();
      unsigned long reconnect_elapsed = (_brokers[i].last_attempt <= now)
          ? (now - _brokers[i].last_attempt)
          : (ULONG_MAX - _brokers[i].last_attempt + now + 1);
      unsigned int idx = (_main_broker_reconnect_backoff_attempt < 5) ? _main_broker_reconnect_backoff_attempt : 4;
      unsigned long delay_ms = MAIN_BROKER_BACKOFF_MS[idx];
      if (reconnect_elapsed >= delay_ms) {
        MQTT_DEBUG_PRINTLN("Reconnecting to broker %d: %s:%d (backoff)", i, _brokers[i].host, _brokers[i].port);
        char broker_uri[128];
        snprintf(broker_uri, sizeof(broker_uri), "mqtt://%s:%d", _brokers[i].host, _brokers[i].port);
        _mqtt_client->setServer(broker_uri);
        if (strlen(_brokers[i].username) > 0) {
          _mqtt_client->setCredentials(_brokers[i].username, _brokers[i].password);
        }
        _mqtt_client->connect();
        _brokers[i].last_attempt = now;
        if (_main_broker_reconnect_backoff_attempt < 5) {
          _main_broker_reconnect_backoff_attempt++;
        }
      }
    }

    // Note: Connection state (_brokers[i].connected) is updated by onConnect/onDisconnect callbacks
    // We just update the cached status here for consistency
    _cached_has_brokers = isAnyBrokerConnected();
  }
  
  // Update cached broker status after connection attempts
  _cached_has_brokers = isAnyBrokerConnected();
}

void MQTTBridge::processPacketQueue() {
  #ifdef ESP_PLATFORM
  // Use FreeRTOS queue
  if (_packet_queue_handle == nullptr) {
    return;
  }
  
  // Update queue count from actual queue state
  _queue_count = uxQueueMessagesWaiting(_packet_queue_handle);
  
  if (_queue_count == 0) {
    return;
  }
  
  // Use cached broker connection status to avoid redundant checks
  bool has_connected_brokers = _cached_has_brokers || _cached_has_analyzer_servers;
  
  if (!has_connected_brokers) {
    if (_queue_count > 0) {
      unsigned long now = millis();
      if (now - _last_no_broker_log > NO_BROKER_LOG_INTERVAL) {
        MQTT_DEBUG_PRINTLN("Queue has %d packets but no brokers connected", _queue_count);
        _last_no_broker_log = now;
      }
    }
    return;
  }
  
  _last_no_broker_log = 0;
  
  // Process up to 1 packet per call to maintain responsiveness
  int processed = 0;
  int max_per_loop = 1;
  unsigned long loop_start_time = millis();
  const unsigned long MAX_PROCESSING_TIME_MS = 30;
  
  while (processed < max_per_loop) {
    unsigned long elapsed = millis() - loop_start_time;
    if (elapsed > MAX_PROCESSING_TIME_MS) {
      break;
    }
    
    QueuedPacket queued;
    // Try to receive from queue (non-blocking)
    if (xQueueReceive(_packet_queue_handle, &queued, 0) != pdTRUE) {
      break;  // No more packets
    }
    
    // Publish packet (use stored raw data if available)
    publishPacket(queued.packet, queued.is_tx, 
                  queued.has_raw_data ? queued.raw_data : nullptr,
                  queued.has_raw_data ? queued.raw_len : 0,
                  queued.has_raw_data ? queued.snr : 0.0f,
                  queued.has_raw_data ? queued.rssi : 0.0f);
    
    // Publish raw if enabled
    if (_raw_enabled) {
      publishRaw(queued.packet);
    }
    
    // NOTE: Do NOT free the packet here - the Dispatcher owns and frees it after logRx() returns.
    // The MQTT bridge only stores a pointer to read from; it does not own the packet.
    queued.packet = nullptr;
    
    _queue_count--;
    processed++;
    
    // No need for vTaskDelay here - task already yields at end of main loop
  }
  #else
  // Non-ESP32: Use circular buffer
  if (_queue_count == 0) {
    return;
  }
  
  // Use cached broker connection status to avoid redundant checks
  bool has_connected_brokers = _cached_has_brokers || _cached_has_analyzer_servers;
  
  if (!has_connected_brokers) {
    if (_queue_count > 0) {
      unsigned long now = millis();
      if (now - _last_no_broker_log > NO_BROKER_LOG_INTERVAL) {
        MQTT_DEBUG_PRINTLN("Queue has %d packets but no brokers connected", _queue_count);
        _last_no_broker_log = now;
      }
    }
    return;
  }
  
  _last_no_broker_log = 0;
  
  int processed = 0;
  int max_per_loop = 1;
  unsigned long loop_start_time = millis();
  const unsigned long MAX_PROCESSING_TIME_MS = 30;
  
  while (_queue_count > 0 && processed < max_per_loop) {
    unsigned long elapsed = millis() - loop_start_time;
    if (elapsed > MAX_PROCESSING_TIME_MS) {
      break;
    }
    
    QueuedPacket& queued = _packet_queue[_queue_head];
    
    publishPacket(queued.packet, queued.is_tx, 
                  queued.has_raw_data ? queued.raw_data : nullptr,
                  queued.has_raw_data ? queued.raw_len : 0,
                  queued.has_raw_data ? queued.snr : 0.0f,
                  queued.has_raw_data ? queued.rssi : 0.0f);
    
    if (_raw_enabled) {
      publishRaw(queued.packet);
    }
    
    // NOTE: Do NOT free the packet here - the Dispatcher owns and frees it after logRx() returns.
    queued.packet = nullptr;
    
    dequeuePacket();
    processed++;
  }
  #endif
}

bool MQTTBridge::publishStatus() {
  // Check if IATA is configured before attempting to publish
  if (!isIATAValid()) {
    static unsigned long last_iata_warning = 0;
    unsigned long now = millis();
    // Only log this warning every 5 minutes to avoid spam
    if (now - last_iata_warning > 300000) {
      MQTT_DEBUG_PRINTLN("MQTT: Cannot publish status - IATA code not configured (current: '%s'). Please set mqtt.iata via CLI.", _iata);
      last_iata_warning = now;
    }
    return false;
  }
  
  // Allow status publish even when max_alloc is low; buffer is PSRAM, so attempt may succeed.
  // Recovery can be triggered after a successful publish (see task loop).

  // Use cached destination status to avoid redundant checks
  // Note: Connection state is verified in connectToBrokers() which runs before publishStatus()
  bool has_custom_brokers = _cached_has_brokers && _config_valid;
  bool has_destinations = has_custom_brokers || _cached_has_analyzer_servers;
  
  if (!has_destinations) {
    return false;  // No destinations available
  }
  
  // JSON buffer in PSRAM when available (plan §4)
  static const size_t STATUS_JSON_BUFFER_SIZE = 768;
  char* json_buffer = (char*)psram_malloc(STATUS_JSON_BUFFER_SIZE);
  if (json_buffer == nullptr) {
    return false;
  }
  char origin_id[65];
  char timestamp[32];
  char radio_info[64];
  
  // Get current timestamp in ISO 8601 format
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S.000000", &timeinfo);
  } else {
    strcpy(timestamp, "2024-01-01T12:00:00.000000");
  }
  
  // Build radio info string (freq,bw,sf,cr)
  snprintf(radio_info, sizeof(radio_info), "%.6f,%.1f,%d,%d", 
           _prefs->freq, _prefs->bw, _prefs->sf, _prefs->cr);
  
  // Use actual device ID
  strncpy(origin_id, _device_id, sizeof(origin_id) - 1);
  origin_id[sizeof(origin_id) - 1] = '\0';
  
  // Build client version string
  char client_version[64];
  getClientVersion(client_version, sizeof(client_version));
  
  // Collect stats on-demand if sources are available
  int battery_mv = -1;
  int uptime_secs = -1;
  int errors = -1;
  int noise_floor = -999;
  int tx_air_secs = -1;
  int rx_air_secs = -1;
  int recv_errors = -1;
  
  if (_board) {
    battery_mv = _board->getBattMilliVolts();
  }
  if (_ms) {
    uptime_secs = _ms->getMillis() / 1000;
  }
  if (_dispatcher) {
    errors = _dispatcher->getErrFlags();
    tx_air_secs = _dispatcher->getTotalAirTime() / 1000;
    rx_air_secs = _dispatcher->getReceiveAirTime() / 1000;
  }
  if (_radio) {
    noise_floor = (int16_t)_radio->getNoiseFloor();
    recv_errors = (int)_radio->getPacketsRecvErrors();
  }
  
  // Build status message with stats
  int len = MQTTMessageBuilder::buildStatusMessage(
    _origin,
    origin_id,
    _board_model,  // model - now dynamic!
    _firmware_version,  // firmware version
    radio_info,
    client_version,  // client version
    "online",
    timestamp,
    json_buffer,
    STATUS_JSON_BUFFER_SIZE,
    battery_mv,
    uptime_secs,
    errors,
    _queue_count,  // Use current queue length
    noise_floor,
    tx_air_secs,
    rx_air_secs,
    recv_errors
  );
  
          if (len > 0) {
            bool published = false;
            
            // Build topic string once and reuse (optimization: avoid redundant snprintf calls)
            char topic[128];
            snprintf(topic, sizeof(topic), "meshcore/%s/%s/status", _iata, _device_id);
            size_t json_len = strlen(json_buffer); // Cache length to avoid multiple strlen() calls
            
            // Publish to all connected custom brokers
            // Use same logic as packet publishes for consistency
            if (_config_valid && _mqtt_client) {
              // Share the same broker URI tracking as packet publishes to avoid sync issues
              // Track last broker URI to avoid calling setServer() unnecessarily (memory optimization)
              // setServer() may allocate memory, so we only call it when the broker changes
              static char last_broker_uri_shared[128] = "";
              
              for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
                // Verify broker is actually connected (state might be stale)
                if (_brokers[i].enabled && _brokers[i].connected) {
                  // Check connection state right before publish (like packet publishes do)
                  if (!_mqtt_client->connected()) {
                    // Connection lost - mark as disconnected but don't disconnect here
                    // (packet publishes handle this more gracefully)
                    _brokers[i].connected = false;
                    _active_brokers--;
                    _brokers[i].last_attempt = millis();  // Throttle reconnection
                    _cached_has_brokers = isAnyBrokerConnected();
                    continue;
                  }
                  
                  // Build broker URI
                  char broker_uri[128];
                  snprintf(broker_uri, sizeof(broker_uri), "mqtt://%s:%d", _brokers[i].host, _brokers[i].port);
                  
                  // Only call setServer() if broker URI changed (reduces memory allocations)
                  if (strcmp(broker_uri, last_broker_uri_shared) != 0) {
                    _mqtt_client->setServer(broker_uri);
                    strncpy(last_broker_uri_shared, broker_uri, sizeof(last_broker_uri_shared) - 1);
                    last_broker_uri_shared[sizeof(last_broker_uri_shared) - 1] = '\0';
                  }
                  
                  // Publish with timeout check - don't block if connection is slow
                  int publish_result = _mqtt_client->publish(topic, 1, true, json_buffer, json_len);
                  if (publish_result > 0) {
                    published = true;
                    s_consecutive_main_publish_failures = 0;
                  } else {
                    s_consecutive_main_publish_failures++;
                    bool should_disconnect = (s_consecutive_main_publish_failures >= MAIN_CLIENT_DISCONNECT_FAILURE_THRESHOLD);
                    static unsigned long last_status_publish_fail_log = 0;
                    unsigned long now = millis();
                    if (now - last_status_publish_fail_log > 60000) {
                      MQTT_DEBUG_PRINTLN("Status publish failed (result=%d), failures=%d%s", publish_result, s_consecutive_main_publish_failures,
                                        should_disconnect ? ", forcing reconnect" : "");
                      last_status_publish_fail_log = now;
                    }
                    if (should_disconnect && _mqtt_client->connected()) {
                      _mqtt_client->disconnect();
                      s_consecutive_main_publish_failures = 0;
                    }
                    if (should_disconnect) {
                      _brokers[i].connected = false;
                      _active_brokers--;
                      _brokers[i].last_attempt = millis();
                      _cached_has_brokers = isAnyBrokerConnected();
                    }
                  }
                }
              }
            } else if (_config_valid) {
              // Connection state is out of sync - mark all brokers as disconnected
              // (Same logic as packet publishes)
              for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
                if (_brokers[i].enabled && _brokers[i].connected) {
                  _brokers[i].connected = false;
                  _active_brokers--;
                }
              }
              _cached_has_brokers = false;
            }
            
            // Always publish to Let's Mesh Analyzer servers if enabled and connected
            // Use shared helper function to publish same JSON to both servers (avoids duplication)
            // Use same memory threshold as main check (60000) for consistency
            if (_cached_has_analyzer_servers) {
              #ifdef ESP32
              size_t max_alloc = ESP.getMaxAllocHeap();
              if (max_alloc >= 60000) {  // Same threshold as main memory check
              #endif
                // publishToAnalyzerServers returns true if at least one publish succeeded
                if (publishToAnalyzerServers(topic, json_buffer, true)) {  // retained=true for status
                  published = true;
                }
              #ifdef ESP32
              }
              #endif
            }
            
            // Return true if we successfully published to at least one destination
            if (published) {
              MQTT_DEBUG_PRINTLN("Status published");
              psram_free(json_buffer);
              return true;
            }
          }
          
          psram_free(json_buffer);
          return false;  // Failed to build or publish message
}

void MQTTBridge::publishPacket(mesh::Packet* packet, bool is_tx, 
                                const uint8_t* raw_data, int raw_len, 
                                float snr, float rssi) {
  if (!packet) return;
  
  // Check if IATA is configured before attempting to publish
  if (!isIATAValid()) {
    static unsigned long last_iata_warning = 0;
    unsigned long now = millis();
    // Only log this warning every 5 minutes to avoid spam
    if (now - last_iata_warning > 300000) {
      MQTT_DEBUG_PRINTLN("MQTT: Cannot publish packet - IATA code not configured (current: '%s'). Please set mqtt.iata via CLI.", _iata);
      last_iata_warning = now;
    }
    return;
  }
  
  // Memory pressure check: Skip publishes when heap is severely fragmented
  // This prevents further fragmentation and allows memory to recover
  // Threshold: Max alloc < 60KB indicates severe fragmentation
  #ifdef ESP32
  unsigned long now = millis();
  if (now - _last_memory_check > 5000) {  // Check every 5 seconds
    size_t max_alloc = ESP.getMaxAllocHeap();
    if (max_alloc < 60000) {  // Less than 60KB max alloc = severe fragmentation
      _skipped_publishes++;
      static unsigned long last_skip_log = 0;
      if (now - last_skip_log > 60000) {  // Log every minute
        MQTT_DEBUG_PRINTLN("MQTT: Skipping publish due to memory pressure (Max alloc: %d, skipped: %d)", max_alloc, _skipped_publishes);
        last_skip_log = now;
      }
      return;  // Skip this publish to allow memory to recover
    }
    _last_memory_check = now;
  }
  #endif
  
  // JSON buffer: prefer PSRAM to reduce stack (plan §4); fallback to stack if allocation fails
  static const size_t PUBLISH_JSON_BUFFER_SIZE = 2048;
  char* json_buffer_psram = (char*)psram_malloc(PUBLISH_JSON_BUFFER_SIZE);
  char json_buffer_stack[1024];
  char json_buffer_large_stack[2048];
  int packet_size = packet->getRawLength();
  char* active_buffer;
  size_t active_buffer_size;
  if (json_buffer_psram != nullptr) {
    active_buffer = json_buffer_psram;
    active_buffer_size = PUBLISH_JSON_BUFFER_SIZE;
  } else {
    active_buffer = (packet_size > 200) ? json_buffer_large_stack : json_buffer_stack;
    active_buffer_size = (packet_size > 200) ? 2048 : 1024;
  }
  char origin_id[65];
  
  // Use actual device ID
  strncpy(origin_id, _device_id, sizeof(origin_id) - 1);
  origin_id[sizeof(origin_id) - 1] = '\0';
  
  // Build packet message using raw radio data if provided
  int len;
  if (raw_data && raw_len > 0) {
    // Use provided raw radio data
    len = MQTTMessageBuilder::buildPacketJSONFromRaw(
      raw_data, raw_len, packet, is_tx, _origin, origin_id, 
      snr, rssi, _timezone, active_buffer, active_buffer_size
    );
  } else if (_last_raw_data && _last_raw_len > 0 && (millis() - _last_raw_timestamp) < 1000) {
    // Fallback to global raw radio data (within 1 second of packet)
    len = MQTTMessageBuilder::buildPacketJSONFromRaw(
      _last_raw_data, _last_raw_len, packet, is_tx, _origin, origin_id, 
      _last_snr, _last_rssi, _timezone, active_buffer, active_buffer_size
    );
  } else {
    // Fallback to reconstructed packet data
    len = MQTTMessageBuilder::buildPacketJSON(
      packet, is_tx, _origin, origin_id, _timezone, active_buffer, active_buffer_size
    );
  }
  
  if (len > 0) {
    // Build topic string once and reuse (optimization: avoid redundant snprintf calls)
    char topic[128];
    snprintf(topic, sizeof(topic), "meshcore/%s/%s/packets", _iata, _device_id);
    size_t json_len = strlen(active_buffer); // Cache length to avoid multiple strlen() calls
    
    // Publish to custom brokers (only if config is valid)
    // Double-check client is actually connected before attempting publish
    if (_config_valid && _mqtt_client && _mqtt_client->connected()) {
      // Track last broker URI to avoid calling setServer() unnecessarily (memory optimization)
      // setServer() may allocate memory, so we only call it when the broker changes
      static char last_broker_uri[128] = "";
      
      for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
        // Verify broker is actually connected (state might be stale)
        if (_brokers[i].enabled && _brokers[i].connected && _mqtt_client->connected()) {
          // Build broker URI
          char broker_uri[128];
          snprintf(broker_uri, sizeof(broker_uri), "mqtt://%s:%d", _brokers[i].host, _brokers[i].port);
          
          // Only call setServer() if broker URI changed (reduces memory allocations)
          if (strcmp(broker_uri, last_broker_uri) != 0) {
            _mqtt_client->setServer(broker_uri);
            strncpy(last_broker_uri, broker_uri, sizeof(last_broker_uri) - 1);
            last_broker_uri[sizeof(last_broker_uri) - 1] = '\0';
          }
          
          // Publish with timeout check - don't block if connection is slow
          // This prevents blocking the main loop when MQTT broker is slow or unresponsive
          int publish_result = _mqtt_client->publish(topic, 1, false, active_buffer, json_len); // qos=1, retained=false
          if (publish_result > 0) {
            s_consecutive_main_publish_failures = 0;
          } else {
            s_consecutive_main_publish_failures++;
            // Only disconnect after several consecutive failures to avoid heap fragmentation from disconnect/reconnect storms
            bool should_disconnect = (s_consecutive_main_publish_failures >= MAIN_CLIENT_DISCONNECT_FAILURE_THRESHOLD);
            static unsigned long last_publish_fail_log = 0;
            unsigned long now = millis();
            if (now - last_publish_fail_log > 60000) {
              MQTT_DEBUG_PRINTLN("Publish failed (result=%d), failures=%d%s", publish_result, s_consecutive_main_publish_failures,
                                should_disconnect ? ", forcing reconnect" : "");
              last_publish_fail_log = now;
            }
            if (should_disconnect && _mqtt_client->connected()) {
              _mqtt_client->disconnect();
              s_consecutive_main_publish_failures = 0;
            }
            if (should_disconnect) {
              _brokers[i].connected = false;
              _active_brokers--;
              _brokers[i].last_attempt = millis();
              _cached_has_brokers = isAnyBrokerConnected();
            }
          }
        }
      }
    } else if (_config_valid) {
      // Connection state is out of sync - mark all brokers as disconnected
      for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
        if (_brokers[i].enabled && _brokers[i].connected) {
          _brokers[i].connected = false;
          _active_brokers--;
        }
      }
    }
    
    // Always publish to Let's Mesh Analyzer servers (independent of custom broker config)
    // Skip analyzer servers if memory is severely fragmented (they're less critical than custom brokers)
    #ifdef ESP32
    size_t max_alloc = ESP.getMaxAllocHeap();
    if (max_alloc >= 60000) {  // Only publish to analyzer servers if memory is OK
      publishToAnalyzerServers(topic, active_buffer, false);
    }
    #else
    publishToAnalyzerServers(topic, active_buffer, false);
    #endif
  } else {
    // Debug: log when packet message building fails
    uint8_t packet_type = packet->getPayloadType();
    if (packet_type == 4 || packet_type == 9) {  // ADVERT or TRACE
      MQTT_DEBUG_PRINTLN("Failed to build packet JSON for type=%d (len=%d), packet not published", packet_type, len);
    }
  }
  psram_free(json_buffer_psram);
}

void MQTTBridge::publishRaw(mesh::Packet* packet) {
  if (!packet) return;
  
  // Check if IATA is configured before attempting to publish
  if (!isIATAValid()) {
    static unsigned long last_iata_warning = 0;
    unsigned long now = millis();
    // Only log this warning every 5 minutes to avoid spam
    if (now - last_iata_warning > 300000) {
      MQTT_DEBUG_PRINTLN("MQTT: Cannot publish raw packet - IATA code not configured (current: '%s'). Please set mqtt.iata via CLI.", _iata);
      last_iata_warning = now;
    }
    return;
  }
  
  // JSON buffer: prefer PSRAM (plan §4); fallback to stack if allocation fails
  char* json_buffer_psram = (char*)psram_malloc(2048);
  char json_buffer_stack[1024];
  char json_buffer_large_stack[2048];
  int packet_size = packet->getRawLength();
  char* active_buffer;
  size_t active_buffer_size;
  if (json_buffer_psram != nullptr) {
    active_buffer = json_buffer_psram;
    active_buffer_size = 2048;
  } else {
    active_buffer = (packet_size > 200) ? json_buffer_large_stack : json_buffer_stack;
    active_buffer_size = (packet_size > 200) ? 2048 : 1024;
  }
  char origin_id[65];
  
  // Use actual device ID
  strncpy(origin_id, _device_id, sizeof(origin_id) - 1);
  origin_id[sizeof(origin_id) - 1] = '\0';
  
  // Build raw message
  int len = MQTTMessageBuilder::buildRawJSON(
    packet, _origin, origin_id, _timezone, active_buffer, active_buffer_size
  );
  
  if (len > 0) {
    // Build topic string once and reuse (optimization: avoid redundant snprintf calls)
    char topic[128];
    snprintf(topic, sizeof(topic), "meshcore/%s/%s/raw", _iata, _device_id);
    size_t json_len = strlen(active_buffer); // Cache length to avoid multiple strlen() calls
    
    // Publish to custom brokers (only if config is valid)
    // Double-check client is actually connected before attempting publish
    if (_config_valid && _mqtt_client && _mqtt_client->connected()) {
      // Track last broker URI to avoid calling setServer() unnecessarily (memory optimization)
      // setServer() may allocate memory, so we only call it when the broker changes
      static char last_broker_uri_raw[128] = "";
      
      for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
        // Verify broker is actually connected (state might be stale)
        if (_brokers[i].enabled && _brokers[i].connected && _mqtt_client->connected()) {
          // Build broker URI
          char broker_uri[128];
          snprintf(broker_uri, sizeof(broker_uri), "mqtt://%s:%d", _brokers[i].host, _brokers[i].port);
          
          // Only call setServer() if broker URI changed (reduces memory allocations)
          if (strcmp(broker_uri, last_broker_uri_raw) != 0) {
            _mqtt_client->setServer(broker_uri);
            strncpy(last_broker_uri_raw, broker_uri, sizeof(last_broker_uri_raw) - 1);
            last_broker_uri_raw[sizeof(last_broker_uri_raw) - 1] = '\0';
          }
          
          // Publish with timeout check - don't block if connection is slow
          int publish_result = _mqtt_client->publish(topic, 1, false, active_buffer, json_len); // qos=1, retained=false
          if (publish_result > 0) {
            s_consecutive_main_publish_failures = 0;
          } else {
            s_consecutive_main_publish_failures++;
            bool should_disconnect = (s_consecutive_main_publish_failures >= MAIN_CLIENT_DISCONNECT_FAILURE_THRESHOLD);
            static unsigned long last_raw_publish_fail_log = 0;
            unsigned long now = millis();
            if (now - last_raw_publish_fail_log > 60000) {
              MQTT_DEBUG_PRINTLN("Raw publish failed (result=%d), failures=%d%s", publish_result, s_consecutive_main_publish_failures,
                                should_disconnect ? ", forcing reconnect" : "");
              last_raw_publish_fail_log = now;
            }
            if (should_disconnect && _mqtt_client->connected()) {
              _mqtt_client->disconnect();
              s_consecutive_main_publish_failures = 0;
            }
            if (should_disconnect) {
              _brokers[i].connected = false;
              _active_brokers--;
              _brokers[i].last_attempt = millis();
              _cached_has_brokers = isAnyBrokerConnected();
            }
          }
        }
      }
    }
    
    // Always publish to Let's Mesh Analyzer servers (independent of custom broker config)
    // Skip analyzer servers if memory is severely fragmented (they're less critical than custom brokers)
    #ifdef ESP32
    size_t max_alloc = ESP.getMaxAllocHeap();
    if (max_alloc >= 60000) {  // Only publish to analyzer servers if memory is OK
      publishToAnalyzerServers(topic, active_buffer, false);
    }
    #else
    publishToAnalyzerServers(topic, active_buffer, false);
    #endif
  }
  psram_free(json_buffer_psram);
}

void MQTTBridge::queuePacket(mesh::Packet* packet, bool is_tx) {
  #ifdef ESP_PLATFORM
  // Use FreeRTOS queue for thread-safe operation
  if (_packet_queue_handle == nullptr) {
    return;  // Queue not initialized
  }
  
  QueuedPacket queued;
  memset(&queued, 0, sizeof(QueuedPacket));
  
  queued.packet = packet;
  queued.timestamp = millis();
  queued.is_tx = is_tx;
  queued.has_raw_data = false;
  
  // Capture raw radio data with mutex protection
  // Use non-blocking mutex to prevent Core 1 from blocking - if mutex is busy, skip raw data
  if (!is_tx) {
    if (xSemaphoreTake(_raw_data_mutex, 0) == pdTRUE) {
      unsigned long current_time = millis();
      if (_last_raw_len > 0 && (current_time - _last_raw_timestamp) < 1000) {
        if (_last_raw_data && _last_raw_len <= sizeof(queued.raw_data)) {
          memcpy(queued.raw_data, _last_raw_data, _last_raw_len);
          queued.raw_len = _last_raw_len;
          queued.snr = _last_snr;
          queued.rssi = _last_rssi;
          queued.has_raw_data = true;
        }
      }
      xSemaphoreGive(_raw_data_mutex);
    }
    // If mutex unavailable, packet is queued without raw data (acceptable trade-off for responsiveness)
  }
  
  // Try to send to queue (non-blocking)
  if (xQueueSend(_packet_queue_handle, &queued, 0) != pdTRUE) {
    // Queue full - try to remove oldest packet
    QueuedPacket oldest;
    if (xQueueReceive(_packet_queue_handle, &oldest, 0) == pdTRUE) {
      // NOTE: Do NOT free oldest.packet - the Dispatcher owns and frees it.
      // We just drop our reference to it.
      MQTT_DEBUG_PRINTLN("Queue full, dropping oldest packet reference");
      // Now try to send again
      if (xQueueSend(_packet_queue_handle, &queued, 0) != pdTRUE) {
        MQTT_DEBUG_PRINTLN("Failed to queue packet after dropping oldest");
        return;
      }
    } else {
      MQTT_DEBUG_PRINTLN("Queue full and cannot remove oldest packet");
      return;
    }
  }
  
  // Update queue count (approximate, since we can't atomically update it)
  UBaseType_t queue_messages = uxQueueMessagesWaiting(_packet_queue_handle);
  _queue_count = queue_messages;
  #else
  // Non-ESP32: Use circular buffer
  if (_queue_count >= MAX_QUEUE_SIZE) {
    QueuedPacket& oldest = _packet_queue[_queue_head];
    // NOTE: Do NOT free oldest.packet - the Dispatcher owns and frees it.
    // We just drop our reference to it.
    MQTT_DEBUG_PRINTLN("Queue full, dropping oldest packet reference (queue size: %d)", _queue_count);
    oldest.packet = nullptr;
    dequeuePacket();
  }
  
  QueuedPacket& queued = _packet_queue[_queue_tail];
  memset(&queued, 0, sizeof(QueuedPacket));
  
  queued.packet = packet;
  queued.timestamp = millis();
  queued.is_tx = is_tx;
  queued.has_raw_data = false;
  
  if (!is_tx && _last_raw_data && _last_raw_len > 0 && (millis() - _last_raw_timestamp) < 1000) {
    if (_last_raw_len <= sizeof(queued.raw_data)) {
      memcpy(queued.raw_data, _last_raw_data, _last_raw_len);
      queued.raw_len = _last_raw_len;
      queued.snr = _last_snr;
      queued.rssi = _last_rssi;
      queued.has_raw_data = true;
    }
  }
  
  _queue_tail = (_queue_tail + 1) % MAX_QUEUE_SIZE;
  _queue_count++;
  #endif
}

void MQTTBridge::dequeuePacket() {
  #ifdef ESP_PLATFORM
  // On ESP32, dequeuePacket() is not used - we use FreeRTOS queue operations directly
  // This method should never be called on ESP32
  return;
  #else
  // Non-ESP32: Use circular buffer
  if (_queue_count == 0) return;
  
  // Clear the dequeued packet structure to free memory and prevent stale data
  QueuedPacket& dequeued = _packet_queue[_queue_head];
  memset(&dequeued, 0, sizeof(QueuedPacket));
  dequeued.has_raw_data = false; // Explicitly set after memset
  
  _queue_head = (_queue_head + 1) % MAX_QUEUE_SIZE;
  _queue_count--;
  #endif
}

bool MQTTBridge::isAnyBrokerConnected() {
  for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
    if (_brokers[i].enabled && _brokers[i].connected) {
      return true;
    }
  }
  return false;
}

void MQTTBridge::setBrokerDefaults() {
  for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
    memset(&_brokers[i], 0, sizeof(MQTTBroker));
    _brokers[i].port = 1883;
    _brokers[i].qos = 0;
    _brokers[i].enabled = false;
    _brokers[i].connected = false;
    _brokers[i].initial_connect_done = false;
    _brokers[i].reconnect_interval = 5000; // 5 seconds
  }
}

void MQTTBridge::setBroker(int broker_index, const char* host, uint16_t port, 
                          const char* username, const char* password, bool enabled) {
  if (broker_index < 0 || broker_index >= MAX_MQTT_BROKERS_COUNT) return;
  
  MQTTBroker& broker = _brokers[broker_index];
  strncpy(broker.host, host, sizeof(broker.host) - 1);
  broker.port = port;
  strncpy(broker.username, username, sizeof(broker.username) - 1);
  strncpy(broker.password, password, sizeof(broker.password) - 1);
  broker.enabled = enabled;
  broker.connected = false;
  broker.reconnect_interval = 5000;
}

void MQTTBridge::setOrigin(const char* origin) {
  strncpy(_origin, origin, sizeof(_origin) - 1);
  _origin[sizeof(_origin) - 1] = '\0';
}

void MQTTBridge::setIATA(const char* iata) {
  strncpy(_iata, iata, sizeof(_iata) - 1);
  _iata[sizeof(_iata) - 1] = '\0';
  // Convert IATA code to uppercase (IATA codes are conventionally uppercase)
  for (int i = 0; _iata[i]; i++) {
    _iata[i] = toupper(_iata[i]);
  }
}

void MQTTBridge::setDeviceID(const char* device_id) {
  strncpy(_device_id, device_id, sizeof(_device_id) - 1);
  _device_id[sizeof(_device_id) - 1] = '\0';
  MQTT_DEBUG_PRINTLN("Device ID set to: %s", _device_id);
}

void MQTTBridge::setFirmwareVersion(const char* firmware_version) {
  strncpy(_firmware_version, firmware_version, sizeof(_firmware_version) - 1);
  _firmware_version[sizeof(_firmware_version) - 1] = '\0';
}

void MQTTBridge::setBoardModel(const char* board_model) {
  strncpy(_board_model, board_model, sizeof(_board_model) - 1);
  _board_model[sizeof(_board_model) - 1] = '\0';
}

void MQTTBridge::setBuildDate(const char* build_date) {
  strncpy(_build_date, build_date, sizeof(_build_date) - 1);
  _build_date[sizeof(_build_date) - 1] = '\0';
}

void MQTTBridge::storeRawRadioData(const uint8_t* raw_data, int len, float snr, float rssi) {
  if (len > 0 && len <= LAST_RAW_DATA_SIZE && _last_raw_data) {
    #ifdef ESP_PLATFORM
    // Protect with mutex for thread-safe access
    if (_raw_data_mutex != nullptr && xSemaphoreTake(_raw_data_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      memcpy(_last_raw_data, raw_data, len);
      _last_raw_len = len;
      _last_snr = snr;
      _last_rssi = rssi;
      _last_raw_timestamp = millis();
      xSemaphoreGive(_raw_data_mutex);
      MQTT_DEBUG_PRINTLN("Stored raw radio data: %d bytes, SNR=%.1f, RSSI=%.1f", len, snr, rssi);
    }
    #else
    memcpy(_last_raw_data, raw_data, len);
    _last_raw_len = len;
    _last_snr = snr;
    _last_rssi = rssi;
    _last_raw_timestamp = millis();
    MQTT_DEBUG_PRINTLN("Stored raw radio data: %d bytes, SNR=%.1f, RSSI=%.1f", len, snr, rssi);
    #endif
  }
}

void MQTTBridge::setupAnalyzerServers() {
  // Update analyzer server settings from preferences
  bool previous_us_enabled = _analyzer_us_enabled;
  bool previous_eu_enabled = _analyzer_eu_enabled;
  
  _analyzer_us_enabled = _prefs->mqtt_analyzer_us_enabled;
  _analyzer_eu_enabled = _prefs->mqtt_analyzer_eu_enabled;
  
  MQTT_DEBUG_PRINTLN("Analyzer servers - US: %s, EU: %s", 
                     _analyzer_us_enabled ? "enabled" : "disabled",
                     _analyzer_eu_enabled ? "enabled" : "disabled");
  
  // Create authentication token if any analyzer servers are enabled
  // Only create tokens if WiFi is connected and NTP is synced (to ensure correct timestamps)
  if (_analyzer_us_enabled || _analyzer_eu_enabled) {
    if (WiFi.status() == WL_CONNECTED && _ntp_synced) {
      if (createAuthToken()) {
        MQTT_DEBUG_PRINTLN("Created authentication token for analyzer servers");
        // Update client credentials with new tokens if clients exist
        if (_analyzer_us_enabled && _analyzer_us_client && _auth_token_us && strlen(_auth_token_us) > 0) {
          _analyzer_us_client->setCredentials(_analyzer_username, _auth_token_us);
        }
        if (_analyzer_eu_enabled && _analyzer_eu_client && _auth_token_eu && strlen(_auth_token_eu) > 0) {
          _analyzer_eu_client->setCredentials(_analyzer_username, _auth_token_eu);
        }
      } else {
        MQTT_DEBUG_PRINTLN("Failed to create authentication token");
      }
    } else {
      MQTT_DEBUG_PRINTLN("Deferring JWT token creation - WiFi: %s, NTP: %s", 
                        (WiFi.status() == WL_CONNECTED) ? "connected" : "disconnected",
                        _ntp_synced ? "synced" : "not synced");
    }
  }
  
  // If settings changed and bridge is already initialized, recreate clients
  // This handles the case where settings change after initialization
  if (_initialized && (previous_us_enabled != _analyzer_us_enabled || previous_eu_enabled != _analyzer_eu_enabled)) {
    MQTT_DEBUG_PRINTLN("Analyzer server settings changed - recreating clients");
    setupAnalyzerClients();
  }
}

bool MQTTBridge::createAuthToken() {
  if (!_identity) {
    MQTT_DEBUG_PRINTLN("No identity for auth token");
    return false;
  }
  
  // Create username in the format: v1_{UPPERCASE_PUBLIC_KEY}
  char public_key_hex[65];
  mesh::Utils::toHex(public_key_hex, _identity->pub_key, PUB_KEY_SIZE);
  snprintf(_analyzer_username, sizeof(_analyzer_username), "v1_%s", public_key_hex);
  
  bool us_token_created = false;
  bool eu_token_created = false;
  
  unsigned long current_time = time(nullptr);
  unsigned long expires_in = 86400; // 24 hours
  bool time_synced = (current_time >= 1000000000);
  
  // Prepare owner public key (if set) - convert to uppercase hex
  const char* owner_key = nullptr;
  char owner_key_uppercase[65];
  if (_prefs->mqtt_owner_public_key[0] != '\0') {
    strncpy(owner_key_uppercase, _prefs->mqtt_owner_public_key, sizeof(owner_key_uppercase) - 1);
    owner_key_uppercase[sizeof(owner_key_uppercase) - 1] = '\0';
    for (int i = 0; owner_key_uppercase[i]; i++) {
      owner_key_uppercase[i] = toupper(owner_key_uppercase[i]);
    }
    owner_key = owner_key_uppercase;
  }
  
  char client_version[64];
  getClientVersion(client_version, sizeof(client_version));
  
  const char* email = (_prefs->mqtt_email[0] != '\0') ? _prefs->mqtt_email : nullptr;
  
  // Create JWT token for US server (only if buffer was allocated)
  if (_analyzer_us_enabled && _auth_token_us) {
    if (JWTHelper::createAuthToken(
        *_identity, "mqtt-us-v1.letsmesh.net", 
        0, expires_in, _auth_token_us, AUTH_TOKEN_SIZE,
        owner_key, client_version, email)) {
      us_token_created = true;
      _token_us_expires_at = time_synced ? (current_time + expires_in) : 0;
    } else {
      MQTT_DEBUG_PRINTLN("Failed to create US token");
      _token_us_expires_at = 0;
    }
  }
  
  // Create JWT token for EU server (only if buffer was allocated)
  if (_analyzer_eu_enabled && _auth_token_eu) {
    if (JWTHelper::createAuthToken(
        *_identity, "mqtt-eu-v1.letsmesh.net", 
        0, expires_in, _auth_token_eu, AUTH_TOKEN_SIZE,
        owner_key, client_version, email)) {
      eu_token_created = true;
      _token_eu_expires_at = time_synced ? (current_time + expires_in) : 0;
    } else {
      MQTT_DEBUG_PRINTLN("Failed to create EU token");
      _token_eu_expires_at = 0;
    }
  }
  
  if (us_token_created || eu_token_created) {
    MQTT_DEBUG_PRINTLN("Auth tokens created (US:%s EU:%s)", 
                       us_token_created ? "yes" : "no", eu_token_created ? "yes" : "no");
  }
  
  return us_token_created || eu_token_created;
}

bool MQTTBridge::publishToAnalyzerServers(const char* topic, const char* payload, bool retained) {
  if (!_analyzer_us_enabled && !_analyzer_eu_enabled) return false;
  
  bool published = false;
  
  // Publish to US server if enabled
  if (_analyzer_us_enabled && _analyzer_us_client) {
    if (publishToAnalyzerClient(_analyzer_us_client, topic, payload, retained)) {
      published = true;
    }
  }
  
  // Publish to EU server if enabled
  if (_analyzer_eu_enabled && _analyzer_eu_client) {
    if (publishToAnalyzerClient(_analyzer_eu_client, topic, payload, retained)) {
      published = true;
    }
  }
  
  return published;  // Return true if at least one publish succeeded
}

// Google Trust Services - GTS Root R4
const char* GTS_ROOT_R4 = 
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDejCCAmKgAwIBAgIQf+UwvzMTQ77dghYQST2KGzANBgkqhkiG9w0BAQsFADBX\n"
    "MQswCQYDVQQGEwJCRTEZMBcGA1UEChMQR2xvYmFsU2lnbiBudi1zYTEQMA4GA1UE\n"
    "CxMHUm9vdCBDQTEbMBkGA1UEAxMSR2xvYmFsU2lnbiBSb290IENBMB4XDTIzMTEx\n"
    "NTAzNDMyMVoXDTI4MDEyODAwMDA0MlowRzELMAkGA1UEBhMCVVMxIjAgBgNVBAoT\n"
    "GUdvb2dsZSBUcnVzdCBTZXJ2aWNlcyBMTEMxFDASBgNVBAMTC0dUUyBSb290IFI0\n"
    "MHYwEAYHKoZIzj0CAQYFK4EEACIDYgAE83Rzp2iLYK5DuDXFgTB7S0md+8Fhzube\n"
    "Rr1r1WEYNa5A3XP3iZEwWus87oV8okB2O6nGuEfYKueSkWpz6bFyOZ8pn6KY019e\n"
    "WIZlD6GEZQbR3IvJx3PIjGov5cSr0R2Ko4H/MIH8MA4GA1UdDwEB/wQEAwIBhjAd\n"
    "BgNVHSUEFjAUBggrBgEFBQcDAQYIKwYBBQUHAwIwDwYDVR0TAQH/BAUwAwEB/zAd\n"
    "BgNVHQ4EFgQUgEzW63T/STaj1dj8tT7FavCUHYwwHwYDVR0jBBgwFoAUYHtmGkUN\n"
    "l8qJUC99BM00qP/8/UswNgYIKwYBBQUHAQEEKjAoMCYGCCsGAQUFBzAChhpodHRw\n"
    "Oi8vaS5wa2kuZ29vZy9nc3IxLmNydDAtBgNVHR8EJjAkMCKgIKAehhxodHRwOi8v\n"
    "Yy5wa2kuZ29vZy9yL2dzcjEuY3JsMBMGA1UdIAQMMAowCAYGZ4EMAQIBMA0GCSqG\n"
    "SIb3DQEBCwUAA4IBAQAYQrsPBtYDh5bjP2OBDwmkoWhIDDkic574y04tfzHpn+cJ\n"
    "odI2D4SseesQ6bDrarZ7C30ddLibZatoKiws3UL9xnELz4ct92vID24FfVbiI1hY\n"
    "+SW6FoVHkNeWIP0GCbaM4C6uVdF5dTUsMVs/ZbzNnIdCp5Gxmx5ejvEau8otR/Cs\n"
    "kGN+hr/W5GvT1tMBjgWKZ1i4//emhA1JG1BbPzoLJQvyEotc03lXjTaCzv8mEbep\n"
    "8RqZ7a2CPsgRbuvTPBwcOMBBmuFeU88+FSBX6+7iP0il8b4Z0QFqIwwMHfs/L6K1\n"
    "vepuoxtGzi4CZ68zJpiq1UvSqTbFJjtbD4seiMHl\n"
    "-----END CERTIFICATE-----\n";

void MQTTBridge::setupAnalyzerClients() {
  MQTT_DEBUG_PRINTLN("Setting up PsychicMqttClient WebSocket clients...");
  MQTT_DEBUG_PRINTLN("Analyzer servers - US: %s, EU: %s", 
                     _analyzer_us_enabled ? "enabled" : "disabled",
                     _analyzer_eu_enabled ? "enabled" : "disabled");

  // Clean up existing clients if they're no longer enabled
  // This handles the case where settings change after initialization
  if (!_analyzer_us_enabled && _analyzer_us_client) {
    MQTT_DEBUG_PRINTLN("US analyzer disabled - cleaning up client");
    _analyzer_us_client->disconnect();
    delete _analyzer_us_client;
    _analyzer_us_client = nullptr;
  }
  
  if (!_analyzer_eu_enabled && _analyzer_eu_client) {
    MQTT_DEBUG_PRINTLN("EU analyzer disabled - cleaning up client");
    _analyzer_eu_client->disconnect();
    delete _analyzer_eu_client;
    _analyzer_eu_client = nullptr;
  }

  if (!_analyzer_us_enabled && !_analyzer_eu_enabled) {
    MQTT_DEBUG_PRINTLN("No analyzer servers enabled, skipping PsychicMqttClient setup");
    return;
  }

  // Setup US server client (only if enabled and doesn't already exist)
  if (_analyzer_us_enabled && !_analyzer_us_client) {
    _analyzer_us_client = new PsychicMqttClient();
    #ifdef MQTT_MEMORY_DEBUG
    // #region agent log
    agentLogHeap("MQTTBridge.cpp:2142", "after_new_analyzer_us_client", "H4",
                 ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 #ifdef BOARD_HAS_PSRAM
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
                 #else
                 0ul
                 #endif
                 );
    // #endregion
    #endif
    // Optimize MQTT client configuration for memory efficiency
    // Analyzer clients use 768-byte JWT tokens, need larger buffer for CONNECT message
    optimizeMqttClientConfig(_analyzer_us_client, true);

    // Set up event callbacks for US server
    _analyzer_us_client->onConnect([this](bool sessionPresent) {
      MQTT_DEBUG_PRINTLN("Connected to US analyzer");
      // Update cached analyzer server status
      _cached_has_analyzer_servers = (_analyzer_us_enabled && _analyzer_us_client && _analyzer_us_client->connected()) ||
                                     (_analyzer_eu_enabled && _analyzer_eu_client && _analyzer_eu_client->connected());
      publishStatusToAnalyzerClient(_analyzer_us_client, "mqtt-us-v1.letsmesh.net");
    });

    _analyzer_us_client->onDisconnect([this](bool sessionPresent) {
      MQTT_DEBUG_PRINTLN("Disconnected from US analyzer");
      // Update cached analyzer server status
      _cached_has_analyzer_servers = (_analyzer_us_enabled && _analyzer_us_client && _analyzer_us_client->connected()) ||
                                     (_analyzer_eu_enabled && _analyzer_eu_client && _analyzer_eu_client->connected());
    });

    _analyzer_us_client->onError([this](esp_mqtt_error_codes error) {
      MQTT_DEBUG_PRINTLN("US analyzer error: type=%d, code=%d", error.error_type, error.connect_return_code);
    });

    _analyzer_us_client->setServer("wss://mqtt-us-v1.letsmesh.net:443/mqtt");
    if (_auth_token_us) _analyzer_us_client->setCredentials(_analyzer_username, _auth_token_us);
    _analyzer_us_client->setCACert(GTS_ROOT_R4);

    if (WiFi.status() == WL_CONNECTED && _ntp_synced) {
      _analyzer_us_client->connect();
    }
  }

  // Setup EU server client (only if enabled and doesn't already exist)
  if (_analyzer_eu_enabled && !_analyzer_eu_client) {
    _analyzer_eu_client = new PsychicMqttClient();
    #ifdef MQTT_MEMORY_DEBUG
    // #region agent log
    agentLogHeap("MQTTBridge.cpp:2182", "after_new_analyzer_eu_client", "H4",
                 ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 #ifdef BOARD_HAS_PSRAM
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
                 #else
                 0ul
                 #endif
                 );
    // #endregion
    #endif
    // Optimize MQTT client configuration for memory efficiency
    // Analyzer clients use 768-byte JWT tokens, need larger buffer for CONNECT message
    optimizeMqttClientConfig(_analyzer_eu_client, true);

    // Set up event callbacks for EU server
    _analyzer_eu_client->onConnect([this](bool sessionPresent) {
      MQTT_DEBUG_PRINTLN("Connected to EU analyzer");
      // Update cached analyzer server status
      _cached_has_analyzer_servers = (_analyzer_us_enabled && _analyzer_us_client && _analyzer_us_client->connected()) ||
                                     (_analyzer_eu_enabled && _analyzer_eu_client && _analyzer_eu_client->connected());
      publishStatusToAnalyzerClient(_analyzer_eu_client, "mqtt-eu-v1.letsmesh.net");
    });

    _analyzer_eu_client->onDisconnect([this](bool sessionPresent) {
      MQTT_DEBUG_PRINTLN("Disconnected from EU analyzer");
      // Update cached analyzer server status
      _cached_has_analyzer_servers = (_analyzer_us_enabled && _analyzer_us_client && _analyzer_us_client->connected()) ||
                                     (_analyzer_eu_enabled && _analyzer_eu_client && _analyzer_eu_client->connected());
    });

    _analyzer_eu_client->onError([this](esp_mqtt_error_codes error) {
      MQTT_DEBUG_PRINTLN("EU analyzer error: type=%d, code=%d", error.error_type, error.connect_return_code);
    });

    _analyzer_eu_client->setServer("wss://mqtt-eu-v1.letsmesh.net:443/mqtt");
    if (_auth_token_eu) _analyzer_eu_client->setCredentials(_analyzer_username, _auth_token_eu);
    _analyzer_eu_client->setCACert(GTS_ROOT_R4);

    if (WiFi.status() == WL_CONNECTED && _ntp_synced) {
      _analyzer_eu_client->connect();
    }
  }
}

bool MQTTBridge::publishToAnalyzerClient(PsychicMqttClient* client, const char* topic, const char* payload, bool retained) {
  if (!client) {
    return false; // Don't log null client - this is expected if analyzer is disabled
  }
  
  if (!client->connected()) {
    // Throttle log spam - only log periodically for each analyzer server
    unsigned long now = millis();
    bool should_log = false;
    
    if (client == _analyzer_us_client && (now - _last_analyzer_us_log > ANALYZER_LOG_INTERVAL)) {
      should_log = true;
      _last_analyzer_us_log = now;
    } else if (client == _analyzer_eu_client && (now - _last_analyzer_eu_log > ANALYZER_LOG_INTERVAL)) {
      should_log = true;
      _last_analyzer_eu_log = now;
    }
    
    if (should_log) {
      MQTT_DEBUG_PRINTLN("PsychicMqttClient not connected - skipping publish to topic: %s", topic);
    }
    return false;
  }
  
  // Reset log timer when connected
  if (client == _analyzer_us_client) {
    _last_analyzer_us_log = 0;
  } else if (client == _analyzer_eu_client) {
    _last_analyzer_eu_log = 0;
  }
  
  int result = client->publish(topic, 1, retained, payload, strlen(payload));
  if (result <= 0) {
    static unsigned long last_analyzer_publish_fail_log = 0;
    unsigned long now = millis();
    if (now - last_analyzer_publish_fail_log > 60000) { // Log every minute max
      MQTT_DEBUG_PRINTLN("Analyzer publish failed (result=%d)", result);
      last_analyzer_publish_fail_log = now;
    }
    return false;
  }
  
  return true;  // Publish succeeded
}

void MQTTBridge::publishStatusToAnalyzerClient(PsychicMqttClient* client, const char* server_name) {
  if (!client || !client->connected()) {
    return;
  }
  
  // Check if IATA is configured before attempting to publish
  if (!isIATAValid()) {
    static unsigned long last_iata_warning = 0;
    unsigned long now = millis();
    // Only log this warning every 5 minutes to avoid spam
    if (now - last_iata_warning > 300000) {
      MQTT_DEBUG_PRINTLN("MQTT: Cannot publish status to analyzer - IATA code not configured (current: '%s'). Please set mqtt.iata via CLI.", _iata);
      last_iata_warning = now;
    }
    return;
  }
  
  // Create status message
  char status_topic[128];
  snprintf(status_topic, sizeof(status_topic), "meshcore/%s/%s/status", _iata, _device_id);
  
  // JSON buffer in PSRAM when available (plan §4)
  static const size_t ANALYZER_STATUS_JSON_SIZE = 768;
  char* json_buffer = (char*)psram_malloc(ANALYZER_STATUS_JSON_SIZE);
  if (json_buffer == nullptr) {
    return;
  }
  char origin_id[65];
  char timestamp[32];
  char radio_info[64];
  
  // Get current timestamp in ISO 8601 format
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S.000000", &timeinfo);
  } else {
    strcpy(timestamp, "2024-01-01T12:00:00.000000");
  }
  
  // Build radio info string (freq,bw,sf,cr)
  snprintf(radio_info, sizeof(radio_info), "%.6f,%.1f,%d,%d", 
           _prefs->freq, _prefs->bw, _prefs->sf, _prefs->cr);
  
  // Use actual device ID
  strncpy(origin_id, _device_id, sizeof(origin_id) - 1);
  origin_id[sizeof(origin_id) - 1] = '\0';
  
  // Build client version string
  char client_version[64];
  getClientVersion(client_version, sizeof(client_version));
  
  // Collect stats on-demand if sources are available
  int battery_mv = -1;
  int uptime_secs = -1;
  int errors = -1;
  int noise_floor = -999;
  int tx_air_secs = -1;
  int rx_air_secs = -1;
  int recv_errors = -1;
  
  if (_board) {
    battery_mv = _board->getBattMilliVolts();
  }
  if (_ms) {
    uptime_secs = _ms->getMillis() / 1000;
  }
  if (_dispatcher) {
    errors = _dispatcher->getErrFlags();
    tx_air_secs = _dispatcher->getTotalAirTime() / 1000;
    rx_air_secs = _dispatcher->getReceiveAirTime() / 1000;
  }
  if (_radio) {
    noise_floor = (int16_t)_radio->getNoiseFloor();
    recv_errors = (int)_radio->getPacketsRecvErrors();
  }
  
  // Build status message using MQTTMessageBuilder with stats
  int len = MQTTMessageBuilder::buildStatusMessage(
    _origin,
    origin_id,
    _board_model,  // model
    _firmware_version,  // firmware version
    radio_info,
    client_version,  // client version
    "online",
    timestamp,
    json_buffer,
    ANALYZER_STATUS_JSON_SIZE,
    battery_mv,
    uptime_secs,
    errors,
    _queue_count,  // Use current queue length
    noise_floor,
    tx_air_secs,
    rx_air_secs,
    recv_errors
  );
  
  if (len > 0) {
    int result = client->publish(status_topic, 1, true, json_buffer, strlen(json_buffer));
    if (result <= 0) {
      MQTT_DEBUG_PRINTLN("Status publish to %s failed", server_name);
    }
  }
  psram_free(json_buffer);
}

void MQTTBridge::maintainAnalyzerConnections() {
  if (!_identity) {
    return;
  }
  
  // Check WiFi status first - don't attempt MQTT reconnection if WiFi is disconnected
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  
  // JWT tokens require valid timestamps. Allow if NTP sync flag is set, or if system clock
  // is clearly set (e.g. not firmware default 2024) so we don't block reconnection after
  // a successful NTP sync at boot when _ntp_synced might be wrong.
  unsigned long clock_sec = time(nullptr);
  bool clock_looks_set = (clock_sec >= 1735689600);  // 2025-01-01 00:00:00 UTC
  if (!_ntp_synced && !clock_looks_set) {
    return;
  }
  
  // Create JWT tokens if they don't exist yet and conditions are met
  if ((_analyzer_us_enabled || _analyzer_eu_enabled) && 
      ((!_auth_token_us || strlen(_auth_token_us) == 0) && (!_auth_token_eu || strlen(_auth_token_eu) == 0))) {
    if (createAuthToken()) {
      if (_analyzer_us_enabled && _analyzer_us_client && _auth_token_us && strlen(_auth_token_us) > 0) {
        _analyzer_us_client->setCredentials(_analyzer_username, _auth_token_us);
        if (!_analyzer_us_client->connected()) {
          _analyzer_us_client->connect();
        }
      }
      if (_analyzer_eu_enabled && _analyzer_eu_client && _auth_token_eu && strlen(_auth_token_eu) > 0) {
        _analyzer_eu_client->setCredentials(_analyzer_username, _auth_token_eu);
        if (!_analyzer_eu_client->connected()) {
          _analyzer_eu_client->connect();
        }
      }
    }
  }
  
  unsigned long current_time = time(nullptr);
  // If time is not synced (time() returns 0 or very small value), skip expiration checks
  // Tokens will still work but we can't track expiration properly
  // If expiration time was set before time sync, it will be a small value, so we'll renew
  bool time_synced = (current_time >= 1000000000); // After year 2001
  
  const unsigned long RENEWAL_BUFFER = 60; // Renew tokens 60 seconds before expiration (minimal buffer to avoid downtime)
  const unsigned long DISCONNECT_THRESHOLD = 60; // Only disconnect if token expires within 60 seconds
  const unsigned long RENEWAL_THROTTLE_MS = 60000; // Don't attempt renewal more than once per minute
  static const unsigned long ANALYZER_BACKOFF_MS[] = { 60000, 120000, 240000, 300000 }; // 1min, 2min, 4min, 5min cap
  
  unsigned long now_millis = millis();
  
  // Check and renew US server token if needed
  if (_analyzer_us_enabled && _analyzer_us_client) {
    if (_analyzer_us_client->connected()) {
      _analyzer_us_reconnect_backoff_attempt = 0;
    }
    // Check if token is expired or will expire soon
    // Only check expiration if time is synced - if time isn't synced, we can't validate expiration
    // If time wasn't synced when token was created, expiration time will be invalid (< 1000000000), so renew when time syncs
    bool token_needs_renewal = false;
    if (!time_synced) {
      // Time not synced yet - only renew if token is missing (expires_at == 0)
      // Don't renew if token exists but expiration is invalid - wait for time sync
      token_needs_renewal = (_token_us_expires_at == 0);
    } else {
      // Time is synced - check if token needs renewal
      token_needs_renewal = (_token_us_expires_at == 0) || 
                           !(_token_us_expires_at >= 1000000000) || // Expiration time invalid (created before time sync)
                           (current_time >= _token_us_expires_at) ||
                           (current_time >= (_token_us_expires_at - RENEWAL_BUFFER));
    }
    
    // Throttle renewal attempts - don't try more than once per minute to avoid blocking
    bool can_attempt_renewal = (now_millis - _last_token_renewal_attempt_us) >= RENEWAL_THROTTLE_MS;
    
    // Check if client is disconnected and needs reconnection with new token
    bool needs_reconnect = !_analyzer_us_client->connected();
    
    if (token_needs_renewal && can_attempt_renewal) {
      _last_token_renewal_attempt_us = now_millis;
      
      // Prepare owner public key (if set) - convert to uppercase hex
      const char* owner_key = nullptr;
      char owner_key_uppercase[65];
      if (_prefs->mqtt_owner_public_key[0] != '\0') {
        // Copy and convert to uppercase
        strncpy(owner_key_uppercase, _prefs->mqtt_owner_public_key, sizeof(owner_key_uppercase) - 1);
        owner_key_uppercase[sizeof(owner_key_uppercase) - 1] = '\0';
        for (int i = 0; owner_key_uppercase[i]; i++) {
          owner_key_uppercase[i] = toupper(owner_key_uppercase[i]);
        }
        owner_key = owner_key_uppercase;
      }
      
      // Build client version string (same format as used in status messages)
      char client_version[64];
      getClientVersion(client_version, sizeof(client_version));
      
      // Get email from preferences (if set)
      const char* email = nullptr;
      if (_prefs->mqtt_email[0] != '\0') {
        email = _prefs->mqtt_email;
      }
      
      // Store old expiration time before renewing (to check if we need to disconnect)
      unsigned long old_token_expires_at = _token_us_expires_at;
      
      // Renew the token (only if buffer was allocated)
      if (_auth_token_us && JWTHelper::createAuthToken(
          *_identity, "mqtt-us-v1.letsmesh.net", 
          0, 86400, _auth_token_us, AUTH_TOKEN_SIZE,
          owner_key, client_version, email)) {
        unsigned long expires_in = 86400; // 24 hours
        _token_us_expires_at = time_synced ? (current_time + expires_in) : 0;
        MQTT_DEBUG_PRINTLN("US token renewed");
        
        _analyzer_us_client->setCredentials(_analyzer_username, _auth_token_us);
        
        bool old_token_expired_or_imminent = !time_synced || 
                                            (old_token_expires_at == 0) ||
                                            (current_time >= old_token_expires_at) ||
                                            (time_synced && old_token_expires_at >= 1000000000 && 
                                             current_time >= (old_token_expires_at - DISCONNECT_THRESHOLD));
        
        if (old_token_expired_or_imminent && _analyzer_us_client->connected()) {
          _analyzer_us_client->disconnect();
          _last_reconnect_attempt_us = now_millis;
          _analyzer_us_client->connect();
        } else if (!_analyzer_us_client->connected()) {
          _last_reconnect_attempt_us = now_millis;
          _analyzer_us_client->connect();
        }
      } else {
        MQTT_DEBUG_PRINTLN("Failed to renew US token");
        _token_us_expires_at = 0;
      }
    } else if (needs_reconnect) {
      unsigned long reconnect_elapsed = (now_millis >= _last_reconnect_attempt_us) ?
                                      (now_millis - _last_reconnect_attempt_us) :
                                      (ULONG_MAX - _last_reconnect_attempt_us + now_millis + 1);
      unsigned int idx = (_analyzer_us_reconnect_backoff_attempt < 4) ? _analyzer_us_reconnect_backoff_attempt : 3;
      unsigned long delay_ms = ANALYZER_BACKOFF_MS[idx];
      if (reconnect_elapsed >= delay_ms) {
        _last_reconnect_attempt_us = now_millis;
        if (_analyzer_us_reconnect_backoff_attempt < 4) {
          _analyzer_us_reconnect_backoff_attempt++;
        }
        _analyzer_us_client->connect();
      }
    }
  }
  
  // Check and renew EU server token if needed
  if (_analyzer_eu_enabled && _analyzer_eu_client) {
    if (_analyzer_eu_client->connected()) {
      _analyzer_eu_reconnect_backoff_attempt = 0;
    }
    // Check if token is expired or will expire soon
    // Only check expiration if time is synced - if time isn't synced, we can't validate expiration
    // If time wasn't synced when token was created, expiration time will be invalid (< 1000000000), so renew when time syncs
    bool token_needs_renewal = false;
    if (!time_synced) {
      // Time not synced yet - only renew if token is missing (expires_at == 0)
      // Don't renew if token exists but expiration is invalid - wait for time sync
      token_needs_renewal = (_token_eu_expires_at == 0);
    } else {
      // Time is synced - check if token needs renewal
      token_needs_renewal = (_token_eu_expires_at == 0) || 
                           !(_token_eu_expires_at >= 1000000000) || // Expiration time invalid (created before time sync)
                           (current_time >= _token_eu_expires_at) ||
                           (current_time >= (_token_eu_expires_at - RENEWAL_BUFFER));
    }
    
    // Throttle renewal attempts - don't try more than once per minute to avoid blocking
    bool can_attempt_renewal = (now_millis - _last_token_renewal_attempt_eu) >= RENEWAL_THROTTLE_MS;
    
    // Check if client is disconnected and needs reconnection with new token
    bool needs_reconnect = !_analyzer_eu_client->connected();
    
    if (token_needs_renewal && can_attempt_renewal) {
      _last_token_renewal_attempt_eu = now_millis;
      
      // Prepare owner public key (if set) - convert to uppercase hex
      const char* owner_key = nullptr;
      char owner_key_uppercase[65];
      if (_prefs->mqtt_owner_public_key[0] != '\0') {
        // Copy and convert to uppercase
        strncpy(owner_key_uppercase, _prefs->mqtt_owner_public_key, sizeof(owner_key_uppercase) - 1);
        owner_key_uppercase[sizeof(owner_key_uppercase) - 1] = '\0';
        for (int i = 0; owner_key_uppercase[i]; i++) {
          owner_key_uppercase[i] = toupper(owner_key_uppercase[i]);
        }
        owner_key = owner_key_uppercase;
      }
      
      // Build client version string
      char client_version[64];
      getClientVersion(client_version, sizeof(client_version));
      
      // Get email from preferences (if set)
      const char* email = nullptr;
      if (_prefs->mqtt_email[0] != '\0') {
        email = _prefs->mqtt_email;
      }
      
      // Store old expiration time before renewing (to check if we need to disconnect)
      unsigned long old_token_expires_at = _token_eu_expires_at;
      
      // Renew the token (only if buffer was allocated)
      if (_auth_token_eu && JWTHelper::createAuthToken(
          *_identity, "mqtt-eu-v1.letsmesh.net", 
          0, 86400, _auth_token_eu, AUTH_TOKEN_SIZE,
          owner_key, client_version, email)) {
        unsigned long expires_in = 86400; // 24 hours
        _token_eu_expires_at = time_synced ? (current_time + expires_in) : 0;
        MQTT_DEBUG_PRINTLN("EU token renewed");
        
        _analyzer_eu_client->setCredentials(_analyzer_username, _auth_token_eu);
        
        bool old_token_expired_or_imminent = !time_synced || 
                                            (old_token_expires_at == 0) ||
                                            (current_time >= old_token_expires_at) ||
                                            (time_synced && old_token_expires_at >= 1000000000 && 
                                             current_time >= (old_token_expires_at - DISCONNECT_THRESHOLD));
        
        if (old_token_expired_or_imminent && _analyzer_eu_client->connected()) {
          _analyzer_eu_client->disconnect();
          _last_reconnect_attempt_eu = now_millis;
          _analyzer_eu_client->connect();
        } else if (!_analyzer_eu_client->connected()) {
          _last_reconnect_attempt_eu = now_millis;
          _analyzer_eu_client->connect();
        }
      } else {
        MQTT_DEBUG_PRINTLN("Failed to renew EU token");
        _token_eu_expires_at = 0;
      }
    } else if (needs_reconnect) {
      unsigned long reconnect_elapsed = (now_millis >= _last_reconnect_attempt_eu) ?
                                      (now_millis - _last_reconnect_attempt_eu) :
                                      (ULONG_MAX - _last_reconnect_attempt_eu + now_millis + 1);
      unsigned int idx = (_analyzer_eu_reconnect_backoff_attempt < 4) ? _analyzer_eu_reconnect_backoff_attempt : 3;
      unsigned long delay_ms = ANALYZER_BACKOFF_MS[idx];
      if (reconnect_elapsed >= delay_ms) {
        _last_reconnect_attempt_eu = now_millis;
        if (_analyzer_eu_reconnect_backoff_attempt < 4) {
          _analyzer_eu_reconnect_backoff_attempt++;
        }
        _analyzer_eu_client->connect();
      }
    }
  }

  // Note: PsychicMqttClient handles automatic reconnection internally,
  // but we need to ensure tokens are renewed before reconnection attempts
}

void MQTTBridge::setMessageTypes(bool status, bool packets, bool raw) {
  _status_enabled = status;
  _packets_enabled = packets;
  _raw_enabled = raw;
}

int MQTTBridge::getConnectedBrokers() const {
  int count = 0;
  for (int i = 0; i < MAX_MQTT_BROKERS_COUNT; i++) {
    if (_brokers[i].enabled && _brokers[i].connected) {
      count++;
    }
  }
  return count;
}

int MQTTBridge::getQueueSize() const {
  #ifdef ESP_PLATFORM
  // Get actual queue size from FreeRTOS queue
  if (_packet_queue_handle != nullptr) {
    return uxQueueMessagesWaiting(_packet_queue_handle);
  }
  return 0;
  #else
  return _queue_count;
  #endif
}

void MQTTBridge::setStatsSources(mesh::Dispatcher* dispatcher, mesh::Radio* radio, 
                                  mesh::MainBoard* board, mesh::MillisecondClock* ms) {
  _dispatcher = dispatcher;
  _radio = radio;
  _board = board;
  _ms = ms;
}

void MQTTBridge::syncTimeWithNTP() {
  if (!WiFi.isConnected()) {
    MQTT_DEBUG_PRINTLN("Cannot sync time - WiFi not connected");
    return;
  }
  
  // Prevent multiple simultaneous NTP syncs
  // Check if we're already synced and sync was recent (within last 5 seconds)
  unsigned long now = millis();
  if (_ntp_synced && (now - _last_ntp_sync) < 5000) {
    // Already synced recently, skip
    return;
  }
  
  // Set flag to prevent concurrent syncs
  static bool sync_in_progress = false;
  if (sync_in_progress) {
    return;  // Another sync is already in progress
  }
  sync_in_progress = true;
  
  MQTT_DEBUG_PRINTLN("Syncing time with NTP...");
  
  // Test DNS resolution before attempting NTP sync
  #ifdef ESP_PLATFORM
  IPAddress resolved_ip;
  if (!WiFi.hostByName("pool.ntp.org", resolved_ip)) {
    MQTT_DEBUG_PRINTLN("WARNING: DNS resolution failed for pool.ntp.org - NTP sync may fail");
  }
  #endif
  
  bool ntp_ok = false;
  unsigned long epochTime = 0;
  const unsigned long kMinValidEpoch = 1704067200;  // 2024-01-01 00:00:00 UTC
  
  // Begin NTP client and try forceUpdate with retries (helps on some boards e.g. Heltec V3)
  _ntp_client.begin();
  const int kMaxNtpRetries = 3;
  for (int attempt = 1; attempt <= kMaxNtpRetries && !ntp_ok; attempt++) {
    if (attempt > 1) {
      MQTT_DEBUG_PRINTLN("NTP retry %d/%d...", attempt, kMaxNtpRetries);
      delay(1000);
    }
    if (_ntp_client.forceUpdate()) {
      epochTime = _ntp_client.getEpochTime();
      if (epochTime >= kMinValidEpoch) {
        ntp_ok = true;
      }
    }
  }
  _ntp_client.end();
  
  // Fallback: use ESP32 built-in SNTP (configTime) when NTPClient fails
  #ifdef ESP_PLATFORM
  if (!ntp_ok) {
    MQTT_DEBUG_PRINTLN("NTP client failed, trying SNTP fallback...");
    configTime(0, 0, "pool.ntp.org");
    for (int i = 0; i < 20; i++) {
      delay(500);
      epochTime = (unsigned long)time(nullptr);
      if (epochTime >= kMinValidEpoch) {
        ntp_ok = true;
        MQTT_DEBUG_PRINTLN("SNTP fallback succeeded: %lu", epochTime);
        break;
      }
    }
  }
  #endif
  
  if (ntp_ok) {
    // Set system timezone to UTC (idempotent; SNTP fallback already uses pool.ntp.org)
    configTime(0, 0, "pool.ntp.org");
    
    // Update the device's RTC clock with UTC time (if available)
    if (_rtc) {
      _rtc->setCurrentTime(epochTime);
    }
    
    bool was_ntp_synced = _ntp_synced;
    _ntp_synced = true;
    _last_ntp_sync = millis();
    sync_in_progress = false;
    
    MQTT_DEBUG_PRINTLN("Time synced: %lu", epochTime);
    
    if (!was_ntp_synced) {
      unsigned long current_time = time(nullptr);
      unsigned long expires_in = 86400; // 24 hours
      
      if (_analyzer_us_enabled && _token_us_expires_at == 0 && _auth_token_us && strlen(_auth_token_us) > 0) {
        _token_us_expires_at = current_time + expires_in;
        MQTT_DEBUG_PRINTLN("US token expiration set after NTP sync: %lu", _token_us_expires_at);
      }
      
      if (_analyzer_eu_enabled && _token_eu_expires_at == 0 && _auth_token_eu && strlen(_auth_token_eu) > 0) {
        _token_eu_expires_at = current_time + expires_in;
      }
      
      if ((_analyzer_us_enabled || _analyzer_eu_enabled) && 
          ((!_auth_token_us || strlen(_auth_token_us) == 0) && (!_auth_token_eu || strlen(_auth_token_eu) == 0))) {
        if (createAuthToken()) {
          if (_analyzer_us_enabled && _analyzer_us_client && _auth_token_us && strlen(_auth_token_us) > 0) {
            _analyzer_us_client->setCredentials(_analyzer_username, _auth_token_us);
            if (!_analyzer_us_client->connected()) {
              _analyzer_us_client->connect();
            }
          }
          if (_analyzer_eu_enabled && _analyzer_eu_client && _auth_token_eu && strlen(_auth_token_eu) > 0) {
            _analyzer_eu_client->setCredentials(_analyzer_username, _auth_token_eu);
            if (!_analyzer_eu_client->connected()) {
              _analyzer_eu_client->connect();
            }
          }
        } else {
          MQTT_DEBUG_PRINTLN("Failed to create tokens after NTP sync");
        }
      }
    }
    
    // Set timezone from string (with DST support) - only if changed
    static char last_timezone[64] = "";
    if (strcmp(_prefs->timezone_string, last_timezone) != 0) {
      if (_timezone) {
        delete _timezone;
        _timezone = nullptr;
      }
      Timezone* tz = createTimezoneFromString(_prefs->timezone_string);
      if (tz) {
        _timezone = tz;
      } else {
        TimeChangeRule utc = {"UTC", Last, Sun, Mar, 0, 0};
        _timezone = new Timezone(utc, utc);
      }
      strncpy(last_timezone, _prefs->timezone_string, sizeof(last_timezone) - 1);
      last_timezone[sizeof(last_timezone) - 1] = '\0';
    }
    
    (void)gmtime((time_t*)&epochTime);
    (void)localtime((time_t*)&epochTime);
  } else {
    MQTT_DEBUG_PRINTLN("NTP sync failed");
    sync_in_progress = false;
  }
}

Timezone* MQTTBridge::createTimezoneFromString(const char* tz_string) {
  // Create Timezone objects for common IANA timezone strings
  
  // North America
  if (strcmp(tz_string, "America/Los_Angeles") == 0 || strcmp(tz_string, "America/Vancouver") == 0) {
    TimeChangeRule pst = {"PST", First, Sun, Nov, 2, -480};  // UTC-8
    TimeChangeRule pdt = {"PDT", Second, Sun, Mar, 2, -420}; // UTC-7
    return new Timezone(pdt, pst);
  } else if (strcmp(tz_string, "America/Denver") == 0) {
    TimeChangeRule mst = {"MST", First, Sun, Nov, 2, -420};  // UTC-7
    TimeChangeRule mdt = {"MDT", Second, Sun, Mar, 2, -360};  // UTC-6
    return new Timezone(mdt, mst);
  } else if (strcmp(tz_string, "America/Chicago") == 0) {
    TimeChangeRule cst = {"CST", First, Sun, Nov, 2, -360};  // UTC-6
    TimeChangeRule cdt = {"CDT", Second, Sun, Mar, 2, -300}; // UTC-5
    return new Timezone(cdt, cst);
  } else if (strcmp(tz_string, "America/New_York") == 0 || strcmp(tz_string, "America/Toronto") == 0) {
    TimeChangeRule est = {"EST", First, Sun, Nov, 2, -300};   // UTC-5
    TimeChangeRule edt = {"EDT", Second, Sun, Mar, 2, -240}; // UTC-4
    return new Timezone(edt, est);
  } else if (strcmp(tz_string, "America/Anchorage") == 0) {
    TimeChangeRule akst = {"AKST", First, Sun, Nov, 2, -540}; // UTC-9
    TimeChangeRule akdt = {"AKDT", Second, Sun, Mar, 2, -480}; // UTC-8
    return new Timezone(akdt, akst);
  } else if (strcmp(tz_string, "Pacific/Honolulu") == 0) {
    TimeChangeRule hst = {"HST", Last, Sun, Oct, 2, -600}; // UTC-10 (no DST)
    return new Timezone(hst, hst);
  
  // Europe
  } else if (strcmp(tz_string, "Europe/London") == 0) {
    TimeChangeRule gmt = {"GMT", Last, Sun, Oct, 2, 0};     // UTC+0
    TimeChangeRule bst = {"BST", Last, Sun, Mar, 1, 60};    // UTC+1
    return new Timezone(bst, gmt);
  } else if (strcmp(tz_string, "Europe/Paris") == 0 || strcmp(tz_string, "Europe/Berlin") == 0) {
    TimeChangeRule cet = {"CET", Last, Sun, Oct, 3, 60};    // UTC+1
    TimeChangeRule cest = {"CEST", Last, Sun, Mar, 2, 120}; // UTC+2
    return new Timezone(cest, cet);
  } else if (strcmp(tz_string, "Europe/Moscow") == 0) {
    TimeChangeRule msk = {"MSK", Last, Sun, Oct, 3, 180};   // UTC+3 (no DST since 2014)
    return new Timezone(msk, msk);
  
  // Asia
  } else if (strcmp(tz_string, "Asia/Tokyo") == 0) {
    TimeChangeRule jst = {"JST", Last, Sun, Oct, 2, 540};   // UTC+9 (no DST)
    return new Timezone(jst, jst);
  } else if (strcmp(tz_string, "Asia/Shanghai") == 0 || strcmp(tz_string, "Asia/Hong_Kong") == 0) {
    TimeChangeRule cst = {"CST", Last, Sun, Oct, 2, 480};   // UTC+8 (no DST)
    return new Timezone(cst, cst);
  } else if (strcmp(tz_string, "Asia/Kolkata") == 0) {
    TimeChangeRule ist = {"IST", Last, Sun, Oct, 2, 330};   // UTC+5:30 (no DST)
    return new Timezone(ist, ist);
  } else if (strcmp(tz_string, "Asia/Dubai") == 0) {
    TimeChangeRule gst = {"GST", Last, Sun, Oct, 2, 240};   // UTC+4 (no DST)
    return new Timezone(gst, gst);
  
  // Australia
  } else if (strcmp(tz_string, "Australia/Sydney") == 0 || strcmp(tz_string, "Australia/Melbourne") == 0) {
    TimeChangeRule aest = {"AEST", First, Sun, Apr, 3, 600};  // UTC+10
    TimeChangeRule aedt = {"AEDT", First, Sun, Oct, 2, 660};   // UTC+11
    return new Timezone(aedt, aest);
  } else if (strcmp(tz_string, "Australia/Perth") == 0) {
    TimeChangeRule awst = {"AWST", Last, Sun, Oct, 2, 480};   // UTC+8 (no DST)
    return new Timezone(awst, awst);
  
  // Timezone abbreviations (with DST handling)
  } else if (strcmp(tz_string, "PDT") == 0 || strcmp(tz_string, "PST") == 0) {
    // Pacific Time (PST/PDT)
    TimeChangeRule pst = {"PST", First, Sun, Nov, 2, -480};  // UTC-8
    TimeChangeRule pdt = {"PDT", Second, Sun, Mar, 2, -420}; // UTC-7
    return new Timezone(pdt, pst);
  } else if (strcmp(tz_string, "MDT") == 0 || strcmp(tz_string, "MST") == 0) {
    // Mountain Time (MST/MDT)
    TimeChangeRule mst = {"MST", First, Sun, Nov, 2, -420};  // UTC-7
    TimeChangeRule mdt = {"MDT", Second, Sun, Mar, 2, -360};  // UTC-6
    return new Timezone(mdt, mst);
  } else if (strcmp(tz_string, "CDT") == 0 || strcmp(tz_string, "CST") == 0) {
    // Central Time (CST/CDT)
    TimeChangeRule cst = {"CST", First, Sun, Nov, 2, -360};  // UTC-6
    TimeChangeRule cdt = {"CDT", Second, Sun, Mar, 2, -300}; // UTC-5
    return new Timezone(cdt, cst);
  } else if (strcmp(tz_string, "EDT") == 0 || strcmp(tz_string, "EST") == 0) {
    // Eastern Time (EST/EDT)
    TimeChangeRule est = {"EST", First, Sun, Nov, 2, -300};   // UTC-5
    TimeChangeRule edt = {"EDT", Second, Sun, Mar, 2, -240}; // UTC-4
    return new Timezone(edt, est);
  } else if (strcmp(tz_string, "BST") == 0 || strcmp(tz_string, "GMT") == 0) {
    // British Time (GMT/BST)
    TimeChangeRule gmt = {"GMT", Last, Sun, Oct, 2, 0};     // UTC+0
    TimeChangeRule bst = {"BST", Last, Sun, Mar, 1, 60};    // UTC+1
    return new Timezone(bst, gmt);
  } else if (strcmp(tz_string, "CEST") == 0 || strcmp(tz_string, "CET") == 0) {
    // Central European Time (CET/CEST)
    TimeChangeRule cet = {"CET", Last, Sun, Oct, 3, 60};    // UTC+1
    TimeChangeRule cest = {"CEST", Last, Sun, Mar, 2, 120}; // UTC+2
    return new Timezone(cest, cet);
  
  // UTC and simple offsets
  } else if (strcmp(tz_string, "UTC") == 0) {
    TimeChangeRule utc = {"UTC", Last, Sun, Mar, 0, 0};
    return new Timezone(utc, utc);
  } else if (strncmp(tz_string, "UTC", 3) == 0) {
    // Handle UTC+/-X format (UTC-8, UTC+5, etc.)
    int offset = atoi(tz_string + 3);
    TimeChangeRule utc_offset = {"UTC", Last, Sun, Mar, 0, offset * 60};
    return new Timezone(utc_offset, utc_offset);
  } else if (strncmp(tz_string, "GMT", 3) == 0) {
    // Handle GMT+/-X format (GMT-8, GMT+5, etc.)
    int offset = atoi(tz_string + 3);
    TimeChangeRule gmt_offset = {"GMT", Last, Sun, Mar, 0, offset * 60};
    return new Timezone(gmt_offset, gmt_offset);
  } else if (strncmp(tz_string, "+", 1) == 0 || strncmp(tz_string, "-", 1) == 0) {
    // Handle simple +/-X format (+5, -8, etc.)
    int offset = atoi(tz_string);
    TimeChangeRule offset_tz = {"TZ", Last, Sun, Mar, 0, offset * 60};
    return new Timezone(offset_tz, offset_tz);
  } else {
    // Unknown timezone, return null
    MQTT_DEBUG_PRINTLN("Unknown timezone: %s", tz_string);
    return nullptr;
  }
}

void MQTTBridge::getClientVersion(char* buffer, size_t buffer_size) const {
  if (!buffer || buffer_size == 0) {
    return;
  }
  // Generate client version string in format "meshcore/{firmware_version}"
  snprintf(buffer, buffer_size, "meshcore/%s", _firmware_version);
}

void MQTTBridge::optimizeMqttClientConfig(PsychicMqttClient* client, bool is_analyzer_client) {
  if (!client) return;
  
  // Keepalive 45s: Cloudflare closes WebSocket connections after 100s idle (non-configurable).
  // Sending PINGREQ every 45s keeps the connection alive through the proxy.
  client->setKeepAlive(45);
  
  // Use a single buffer size for all clients to reduce heap fragmentation: mixed sizes
  // (e.g. 640 vs 896) create different-sized holes that are harder to reuse on reconnect.
  // 896 is the minimum safe size for analyzer clients (CONNECT + 768-byte JWT); main
  // client uses the same size so all MQTT buffer allocations are identical.
  static const int MQTT_CLIENT_BUFFER_SIZE = 896;
  
  client->setBufferSize(MQTT_CLIENT_BUFFER_SIZE);
  
  // Access ESP-IDF config to optimize additional settings
  esp_mqtt_client_config_t* config = client->getMqttConfig();
  if (config) {
    #if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
      if (config->buffer.out_size == 0 || config->buffer.out_size > MQTT_CLIENT_BUFFER_SIZE) {
        config->buffer.out_size = MQTT_CLIENT_BUFFER_SIZE;
      }
    #endif
  }
}

void MQTTBridge::logMemoryStatus() {
  MQTT_DEBUG_PRINTLN("Memory: Free=%d, Max=%d, Queue=%d/%d", 
                     ESP.getFreeHeap(), ESP.getMaxAllocHeap(), _queue_count, MAX_QUEUE_SIZE);
}

#endif

