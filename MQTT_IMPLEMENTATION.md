# MQTT Bridge Implementation for MeshCore

This document describes the MQTT bridge implementation that allows MeshCore repeaters to uplink packet data to multiple MQTT brokers.

## Quick Start Guide

### Essential Commands to Get MQTT Repeater Running

**1. Connect to device console via repeater login or serial console (115200 baud)**
```bash
# Connect to device via serial
```

**2. Configure WiFi Credentials**
```bash
set wifi.ssid YourWiFiNetwork
set wifi.pwd YourWiFiPassword
```

If you wish to upload to the MeshCore Analyzer, also `set mqtt.iata XXX` to a valid IATA airport code

**3. Reboot to Connect to WiFi**
```bash
reboot
```

**4. Toggle bridge.source to rx**
```bash
set bridge.source rx
```

**5. Verify Configuration**
```bash
get wifi.ssid
get bridge.enabled
get bridge.source
get mqtt.origin
get mqtt.iata
```

**6. Restart Bridge (if needed)**
```bash
# Option A: Toggle bridge off then on
set bridge.enabled off
set bridge.enabled on

# Option B: Full device reboot
reboot
```

**That's it!** The device will now:
- Connect to WiFi automatically
- Start uplinking mesh packets to Let's Mesh Analyzer
- Publish to both custom MQTT broker and Let's Mesh Analyzer servers
- Use device name as MQTT origin (set automatically)

---

## Overview

The MQTT bridge implementation provides:
- Multiple MQTT broker support (up to 3 brokers)
- Automatic reconnection with exponential backoff
- JSON message formatting for status, packets, and raw data
- Configurable topics and QoS levels
- Packet queuing during connection issues

## Files Added

### Core Implementation
- `src/helpers/bridges/MQTTBridge.h` - MQTT bridge class definition
- `src/helpers/bridges/MQTTBridge.cpp` - MQTT bridge implementation
- `src/helpers/MQTTMessageBuilder.h` - JSON message formatting utilities
- `src/helpers/MQTTMessageBuilder.cpp` - JSON message formatting implementation

### Integration
- Updated `examples/simple_repeater/MyMesh.h` - Added MQTT bridge support
- Updated `examples/simple_repeater/MyMesh.cpp` - Added MQTT bridge integration and raw radio data capture
- Updated `src/helpers/CommonCLI.h` - Added MQTT, WiFi, and timezone configuration fields
- Updated `src/helpers/CommonCLI.cpp` - Added MQTT, WiFi, and timezone CLI commands
- Updated `variants/heltec_v3/platformio.ini` - Added MQTT build configuration
- Updated `variants/station_g2/platformio.ini` - Added MQTT build configuration for Station G2

## Build Configuration

To build the MQTT bridge firmware:

### Heltec V3
```bash
pio run -e Heltec_v3_repeater_observer_mqtt
```

### Station G2
```bash
pio run -e Station_G2_repeater_observer_mqtt
```

### Custom MQTT Server Configuration

You can configure a custom MQTT server using build flags in `platformio.ini`:

```ini
[env:Heltec_v3_repeater_observer_mqtt]
build_flags =
  ${Heltec_lora32_v3.build_flags}
  -D WITH_MQTT_BRIDGE=1
  -D MQTT_SERVER='"your-mqtt-broker.com"'
  -D MQTT_PORT=1883
  -D MQTT_USERNAME='"your-username"'
  -D MQTT_PASSWORD='"your-password"'
```

**Build Flags:**
- `MQTT_SERVER` - MQTT broker hostname
- `MQTT_PORT` - MQTT broker port (default: 1883)
- `MQTT_USERNAME` - MQTT username
- `MQTT_PASSWORD` - MQTT password
- `MQTT_WIFI_TX_POWER` - WiFi TX power level (default: `WIFI_POWER_11dBm`)
  - Available values: `WIFI_POWER_19_5dBm`, `WIFI_POWER_19dBm`, `WIFI_POWER_18_5dBm`, `WIFI_POWER_17_5dBm`, `WIFI_POWER_15dBm`, `WIFI_POWER_13dBm`, `WIFI_POWER_11dBm`, `WIFI_POWER_8_5dBm`, `WIFI_POWER_7dBm`, `WIFI_POWER_5dBm`, `WIFI_POWER_2dBm`, `WIFI_POWER_MINUS_1dBm`
  - Example: `-D MQTT_WIFI_TX_POWER=WIFI_POWER_19_5dBm` for maximum power
  - **Note**: These power levels are appropriate for ESP32 and ESP32-S3. ESP32-C3 and ESP32-C6 may have different maximum power capabilities. If an invalid constant is used for your chip, the compiler will report an error. Check your specific ESP32 variant's datasheet for maximum supported TX power.

## Default Configuration

The MQTT bridge comes with the following defaults:
- **Origin**: "MeshCore-Repeater"
- **IATA**: "SEA"
- **Status Messages**: Enabled
- **Packet Messages**: Enabled
- **Raw Messages**: Disabled
- **TX Messages**: Disabled (RX only by default)
- **Status Interval**: 5 minutes (300000 ms)
- **Default Broker**: meshtastic.pugetmesh.org:1883 (username: meshdev, password: large4cats)
- **WiFi SSID**: "ssid_here" (must be configured)
- **WiFi Password**: "password_here" (must be configured)
- **WiFi Power Save**: "min" (minimum power saving, balanced performance and power)
- **Timezone**: "America/Los_Angeles" (Pacific Time with DST support)
- **Timezone Offset**: -8 hours (fallback)
- **Let's Mesh Analyzer US**: Enabled (mqtt-us-v1.letsmesh.net:443)
- **Let's Mesh Analyzer EU**: Enabled (mqtt-eu-v1.letsmesh.net:443)

## CLI Commands

### MQTT Commands

#### Get Commands
- `get mqtt.origin` - Get device origin name
- `get mqtt.iata` - Get IATA code
- `get mqtt.status` - Get status message setting (on/off)
- `get mqtt.packets` - Get packet message setting (on/off)
- `get mqtt.raw` - Get raw message setting (on/off)
- `get mqtt.tx` - Get TX message setting (on/off)
- `get mqtt.interval` - Get status publish interval (ms)
- `get mqtt.server` - Get MQTT server hostname
- `get mqtt.port` - Get MQTT server port
- `get mqtt.username` - Get MQTT username
- `get mqtt.password` - Get MQTT password
- `get mqtt.analyzer.us` - Get US Let's Mesh Analyzer server setting (on/off)
- `get mqtt.analyzer.eu` - Get EU Let's Mesh Analyzer server setting (on/off)
- `get mqtt.owner` - Get owner public key (64 hex characters)
  - **Note**: Available via serial console only (not via LoRa repeater console)
- `get mqtt.email` - Get owner email address
  - **Note**: Available via serial console only (not via LoRa repeater console)

#### Set Commands
- `set mqtt.origin <name>` - Set device origin name
- `set mqtt.iata <code>` - Set IATA code
- `set mqtt.status on|off` - Enable/disable status messages
- `set mqtt.packets on|off` - Enable/disable packet messages
- `set mqtt.raw on|off` - Enable/disable raw messages
- `set mqtt.tx on|off` - Enable/disable TX packet messages
- `set mqtt.interval <ms>` - Set status publish interval (1000-3600000 ms)
- `set mqtt.server <hostname>` - Set MQTT server hostname
- `set mqtt.port <port>` - Set MQTT server port (1-65535)
- `set mqtt.username <username>` - Set MQTT username
- `set mqtt.password <password>` - Set MQTT password
- `set mqtt.analyzer.us on|off` - Enable/disable US Let's Mesh Analyzer server
- `set mqtt.analyzer.eu on|off` - Enable/disable EU Let's Mesh Analyzer server
- `set mqtt.owner <64-hex-char-public-key>` - Set owner public key (64 hex characters, 32 bytes)
- `set mqtt.email <email>` - Set owner email address for matching nodes with owners

### WiFi Commands

#### Get Commands
- `get wifi.ssid` - Get WiFi SSID
- `get wifi.pwd` - Get WiFi password
- `get wifi.powersave` - Get WiFi power save mode (none/min/max)

#### Set Commands
- `set wifi.ssid <ssid>` - Set WiFi SSID
- `set wifi.pwd <password>` - Set WiFi password
- `set wifi.powersave none|min|max` - Set WiFi power save mode
  - `none` - No power saving (best performance, highest power consumption)
  - `min` - Minimum power saving (default, balanced performance and power)
  - `max` - Maximum power saving (lowest power consumption, may affect performance)

### Timezone Commands

#### Get Commands
- `get timezone` - Get timezone string (e.g., "America/Los_Angeles")
- `get timezone.offset` - Get timezone offset in hours (-12 to +14)

#### Set Commands
- `set timezone <string>` - Set timezone string (IANA format or abbreviation)
- `set timezone.offset <offset>` - Set timezone offset in hours (-12 to +14)

#### Supported Timezone Formats
- **IANA strings**: `America/Los_Angeles`, `Europe/London`, `Asia/Tokyo`, etc.
- **Common abbreviations**: `PDT`, `PST`, `MDT`, `MST`, `CDT`, `CST`, `EDT`, `EST`, `BST`, `GMT`, `CEST`, `CET`
- **UTC offsets**: `UTC-8`, `UTC+5`, `+5`, `-8`, etc.

### Bridge Commands

#### Get Commands
- `get bridge.source` - Get packet source (rx/tx)
- `get bridge.enabled` - Get bridge enabled status (on/off)

#### Set Commands
- `set bridge.source rx|tx` - Set packet source (rx for received, tx for transmitted)
- `set bridge.enabled on|off` - Enable/disable bridge

## Command Architecture

The CLI commands are organized into two levels:

### Bridge Commands (`bridge.*`)
**Low-level bridge control** - These settings apply to all bridge types (MQTT, RS232, ESP-NOW, etc.):
- `bridge.enabled` - Master switch for the entire bridge system
- `bridge.source` - Controls which packet events to capture (RX vs TX)

### Bridge-Specific Commands (`mqtt.*`, `wifi.*`, `timezone.*`)
**Implementation-specific settings** - These only apply to the MQTT bridge:
- `mqtt.*` - MQTT broker configuration, message types, and formatting
- `wifi.*` - WiFi connection settings for MQTT connectivity
- `timezone.*` - Timezone configuration for accurate timestamps

This design allows MeshCore to support multiple bridge types simultaneously while keeping configuration clean and logical.

## MQTT Topics

The bridge publishes to three main topics with the following structure:

### Status Topic: `meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/status`
Device connection status and metadata (retained messages).

### Packets Topic: `meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/packets`
Full packet data with RF characteristics and metadata.

### Raw Topic: `meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/raw`
Minimal raw packet data for map integration.

**Note**: `{DEVICE_PUBLIC_KEY}` is the device's public key in hexadecimal format (64 characters).

## JSON Message Formats

### Status Message
```json
{
  "status": "online|offline",
  "timestamp": "2024-01-01T12:00:00.000000",
  "origin": "Device Name",
  "origin_id": "DEVICE_PUBLIC_KEY",
  "model": "device_model",
  "firmware_version": "firmware_version",
  "radio": "radio_info",
  "client_version": "meshcore-custom-repeater/{build_date}"
}
```

### Packet Message
```json
{
  "origin": "MeshCore-HOWL",
  "origin_id": "A1B2C3D4E5F67890...",
  "timestamp": "2024-01-01T12:00:00.000000",
  "type": "PACKET",
  "direction": "rx|tx",
  "time": "12:00:00",
  "date": "01/01/2024",
  "len": "45",
  "packet_type": "4",
  "route": "F|D|T|U",
  "payload_len": "32",
  "raw": "F5930103807E5F1E...",
  "SNR": "12.5",
  "RSSI": "-65",
  "hash": "A1B2C3D4E5F67890",
  "path": "node1,node2,node3"
}
```

### Raw Message
```json
{
  "origin": "MeshCore-HOWL",
  "origin_id": "A1B2C3D4E5F67890...",
  "timestamp": "2024-01-01T12:00:00.000000",
  "type": "RAW",
  "data": "F5930103807E5F1E..."
}
```

## Key Features

### Raw Radio Data Capture
- Captures actual raw radio transmission data (including radio headers)
- Uses proper MeshCore packet hashing (SHA256-based)
- Provides accurate SNR/RSSI values from actual radio reception
- Supports both RX and TX packet uplinking (configurable)

### Timezone Support
- Full timezone support with automatic DST handling
- Supports IANA timezone strings, common abbreviations, and UTC offsets
- Separates local time (for timestamps) and UTC time (for time/date fields)
- Uses JChristensen/Timezone library for accurate timezone conversions

### WiFi Configuration
- Runtime WiFi credential management via CLI
- Persistent storage across reboots
- Automatic reconnection with exponential backoff

### NTP Time Synchronization
- Automatic time synchronization with NTP servers
- Periodic time updates (every hour)
- Proper UTC system time handling

### Let's Mesh Analyzer Integration
- **JWT Authentication**: Ed25519-signed tokens for secure MQTT authentication
- **WebSocket MQTT**: Support for MQTT over WebSocket connections (TLS/SSL)
- **Dual Server Support**: Both US and EU servers enabled by default
- **Automatic Token Generation**: Creates authentication tokens using device's Ed25519 keys
- **Username Format**: `v1_{UPPERCASE_PUBLIC_KEY}` (e.g., `v1_7E7662676F7F0850A8A355BAAFBFC1EB7B4174C340442D7D7161C9474A2C9400`)
- **Server Configuration**:
  - US Server: `mqtt-us-v1.letsmesh.net:443` (WebSocket with TLS)
  - EU Server: `mqtt-eu-v1.letsmesh.net:443` (WebSocket with TLS)

## First-Time Setup

### Prerequisites
- MeshCore device with MQTT bridge firmware flashed
- WiFi network credentials
- MQTT broker (optional - default broker is provided)
- LoRa-capable device for configuration (repeater console)
- MeshCore network access

### Step 1: Initial Boot and Network Connection
1. **Flash the firmware** to your device using PlatformIO or the build script
2. **Deploy the device** in your mesh network location
3. **Ensure WiFi connectivity** - the device will automatically connect to WiFi if credentials are pre-configured
4. **Verify mesh network access** - device should be discoverable by other mesh nodes

### Step 2: Connect via LoRa Repeater Console
Use a MeshCore companion device to configure the Repeater's MQTT bridge.

1. **Connect to the mesh** using your companion
2. **Locate the MQTT bridge device** in your contacts
3. **Log into your Repeater** using the default password (password) or whatever you configured via serial console
3. **Tap on the repeater console** on your repeater's settings
4. **Send configuration commands** via LoRa to the MQTT bridge device

### Step 3: Configure WiFi Connection
The device needs internet connectivity to publish to MQTT brokers.

**Via LoRa Repeater Console:**
```
# Set your WiFi credentials
set wifi.ssid "YourWiFiNetwork"
set wifi.pwd "YourWiFiPassword"

# Optionally configure WiFi power saving (default: min)
# Use "none" for best performance, "min" for balanced (default), "max" for lowest power
set wifi.powersave min

# Verify WiFi settings
get wifi.ssid
get wifi.pwd
get wifi.powersave
```

### Step 4: Configure Device Identity
Set up your device's identity for MQTT topics and status messages.

**Via LoRa Repeater Console:**
```
# Set IATA code for topic structure (e.g., airport code)
set mqtt.iata "SEA"

# Verify settings (origin is set automatically to device name)
get mqtt.origin
get mqtt.iata
```

**Via Serial Console (Optional - Owner Configuration):**
```
# Set owner public key (64 hex characters, 32 bytes)
# This is used for matching nodes with owners in MQTT messages
set mqtt.owner A1B2C3D4E5F6789012345678901234567890123456789012345678901234567890

# Set owner email address
set mqtt.email owner@example.com

# Verify owner settings
get mqtt.owner
get mqtt.email
```

### Step 5: Configure Timezone
Set your local timezone for accurate timestamps.

**Via LoRa Repeater Console:**
```
# Set timezone (choose one method)
set timezone "America/Los_Angeles"    # IANA format
set timezone "PDT"                    # Abbreviation
set timezone "UTC-8"                  # UTC offset

# Verify timezone
get timezone
```

### Step 6: Configure MQTT Settings
Customize which messages to publish and how often.

**Via LoRa Repeater Console:**
```
# Configure MQTT server (optional - uses defaults if not set)
set mqtt.server "your-mqtt-broker.com"
set mqtt.port 1883
set mqtt.username "your-username"
set mqtt.password "your-password"

# Enable/disable message types
set mqtt.status on                    # Device status messages
set mqtt.packets on                   # Packet data messages
set mqtt.raw off                      # Raw packet data (optional)
set mqtt.tx off                       # Transmitted packets (optional)

# Set status publish interval (default: 5 minutes)
set mqtt.interval 300000

# Verify settings
get mqtt.server
get mqtt.port
get mqtt.username
get mqtt.status
get mqtt.packets
get mqtt.interval
```

### Step 7: Verify MQTT Broker Connection
Check that the device can connect to MQTT brokers.

**Via LoRa Repeater Console:**
```
# Check bridge status
get bridge.enabled

# If disabled, enable it
set bridge.enabled on

# Check MQTT analyzer servers (optional)
get mqtt.analyzer.us
get mqtt.analyzer.eu
```

### Step 8: Monitor MQTT Messages
Once configured, the device will automatically publish messages to MQTT brokers.

**Default MQTT Broker**: `meshtastic.pugetmesh.org:1883`
- Username: `meshdev`
- Password: `large4cats`

**Topic Structure**:
- Status: `meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/status`
- Packets: `meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/packets`
- Raw: `meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/raw`

**Example Topics**:
- `meshcore/SEA/7E7662676F7F0850A8A355BAAFBFC1EB7B4174C340442D7D7161C9474A2C9400/status`
- `meshcore/SEA/7E7662676F7F0850A8A355BAAFBFC1EB7B4174C340442D7D7161C9474A2C9400/packets`

### Step 9: Troubleshooting

#### Device Won't Connect to WiFi
**Via LoRa Repeater Console:**
```
# Check WiFi settings
get wifi.ssid
get wifi.pwd
get wifi.powersave

# Reset WiFi settings
set wifi.ssid ""
set wifi.pwd ""

# Reconfigure with correct credentials
set wifi.ssid "YourWiFiNetwork"
set wifi.pwd "YourWiFiPassword"

# If connection issues persist, try disabling power saving for better reliability
set wifi.powersave none
```

#### No MQTT Messages Appearing
**Via LoRa Repeater Console:**
```
# Check bridge status
get bridge.enabled

# Check message types
get mqtt.status
get mqtt.packets

# Check device identity (origin is set automatically)
get mqtt.origin
get mqtt.iata

# Enable bridge if needed
set bridge.enabled on
```

#### Timezone Issues
**Via LoRa Repeater Console:**
```
# Check current timezone
get timezone

# Try different timezone formats
set timezone "America/New_York"       # IANA format
set timezone "EST"                    # Abbreviation
set timezone "UTC-5"                  # UTC offset
```

#### LoRa Configuration Issues
- **Device not responding**: Ensure both devices are on the same mesh network
- **Commands not working**: Check that the target device is reachable via LoRa
- **No response to get commands**: Verify the device is powered and in range

### Step 10: Advanced Configuration (Optional)

#### Custom MQTT Broker
If you want to use your own MQTT broker instead of the default:

```
# Note: Custom broker configuration requires code modification
# The default broker is: meshtastic.pugetmesh.org:1883
# Username: meshdev, Password: large4cats
```

#### Let's Mesh Analyzer Servers
The device automatically connects to Let's Mesh Analyzer servers for additional monitoring:

- **US Server**: `mqtt-us-v1.letsmesh.net:443` (WebSocket with TLS)
- **EU Server**: `mqtt-eu-v1.letsmesh.net:443` (WebSocket with TLS)

These are enabled by default and use JWT authentication with your device's Ed25519 keys.

## Testing

1. Flash the MQTT bridge firmware to your device
2. Follow the first-time setup instructions above
3. Monitor MQTT broker for incoming messages
4. Verify message formats match the JSON schemas in this document

## Dependencies

- **PubSubClient**: MQTT client library
- **ArduinoJson**: JSON message formatting (v6.17.3)
- **NTPClient**: Network time protocol client
- **Timezone**: Timezone conversion library (JChristensen/Timezone)
- **WiFi**: ESP32 WiFi functionality
- **Ed25519**: Cryptographic library for JWT token signing
- **JWTHelper**: Custom JWT token generation for Let's Mesh Analyzer authentication

## Future Enhancements

- Full WebSocket MQTT implementation (currently JWT tokens are generated but WebSocket publishing is pending)
- Multiple broker configuration via CLI
- Advanced packet filtering
- Custom topic templates
- TLS/SSL support for secure connections
- Real-time WebSocket MQTT publishing to Let's Mesh Analyzer servers
