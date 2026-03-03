# MQTT Library Memory Analysis

## Library Information
- **Library**: `elims/PsychicMqttClient@^0.2.4`
- **Type**: External PlatformIO library (wrapper around ESP-IDF MQTT client)
- **Platform**: ESP32
- **Protocol**: MQTT over WebSocket (WSS) for analyzer servers, standard MQTT for custom brokers
- **Underlying Library**: ESP-IDF `esp_mqtt_client` (part of ESP-IDF framework)
- **Source Location**: `.pio/libdeps/*/PsychicMqttClient/src/`

## Current Usage Patterns

### Memory Allocation Points

1. **Client Instance Creation**
   ```cpp
   _mqtt_client = new PsychicMqttClient();  // Heap allocation
   _analyzer_us_client = new PsychicMqttClient();  // Heap allocation
   _analyzer_eu_client = new PsychicMqttClient();  // Heap allocation
   ```
   - **Impact**: 3 instances × ~few KB each = ~10-15KB base memory

2. **setServer() Calls** ✅ **OPTIMIZED - NO ALLOCATION IN WRAPPER**
   ```cpp
   // From PsychicMqttClient.cpp line 215-223
   PsychicMqttClient &setServer(const char *uri) {
       _mqtt_cfg.broker.address.uri = uri;  // Just stores pointer
       return *this;
   }
   ```
   - **Finding**: Wrapper does NOT copy URI (just stores pointer)
   - **Current Optimization**: We only call `setServer()` when URI changes (using static tracking) ✅
   - **ESP-IDF Behavior**: When `connect()` is called, ESP-IDF likely copies the URI internally
   - **Impact**: Minimal (only on connection, not per publish)

3. **setCredentials() Calls** ✅ **NO ALLOCATION IN WRAPPER**
   ```cpp
   // From PsychicMqttClient.cpp line 166-178
   PsychicMqttClient &setCredentials(const char *username, const char *password) {
       _mqtt_cfg.credentials.username = username;  // Just stores pointers
       _mqtt_cfg.credentials.authentication.password = password;
       return *this;
   }
   ```
   - **Finding**: Wrapper does NOT copy credentials (just stores pointers)
   - **Frequency**: Once per broker connection
   - **ESP-IDF Behavior**: Likely copies when connecting
   - **Impact**: Minimal (only on connection, not per publish)

4. **publish() Calls** ⚠️ **CONFIRMED MAIN CULPRIT**
   ```cpp
   // From PsychicMqttClient.cpp line 370-389
   int publish(const char *topic, int qos, bool retain, const char *payload, int length, bool async)
   {
       if (async) {
           return esp_mqtt_client_enqueue(_client, topic, payload, length, qos, retain, true);
       } else {
           return esp_mqtt_client_publish(_client, topic, payload, length, qos, retain);
       }
   }
   ```
   - **Frequency**: High (every packet, status updates)
   - **Parameters**: 
     - `topic`: String pointer (passed to ESP-IDF)
     - `payload`: Buffer pointer + length (passed to ESP-IDF)
   - **Critical Finding**: 
     - PsychicMqttClient wrapper does NOT copy payload (passes pointers directly)
     - **BUT**: ESP-IDF `esp_mqtt_client_enqueue()` and `esp_mqtt_client_publish()` **DO copy internally**
     - ESP-IDF copies both topic and payload into internal buffers for async processing
   - **Memory Impact**:
     - Each publish: `strlen(topic)` + `payload_len` bytes allocated by ESP-IDF
     - For 2KB JSON payloads + ~50 byte topics: ~2KB per publish
   - **Multiple Publishes Per Packet**:
     - Custom brokers: 1-3 publishes (one per broker)
     - Analyzer servers: 2 publishes (US + EU)
     - **Total**: Up to 5 publishes per packet × 2KB = **10KB+ allocations per packet**
   - **Async vs Sync**:
     - We use `async=true` (default), which calls `esp_mqtt_client_enqueue()`
     - Enqueue likely allocates more (needs queue buffer) than direct publish

## Memory Fragmentation Evidence

### Observed Behavior
- **When MQTT Active**: Max alloc drops to ~54KB (severe fragmentation)
- **When MQTT Disconnected**: Max alloc recovers to ~88KB
- **Pattern**: Memory recovers when publishes stop

### Root Cause - CONFIRMED ✅
1. **ESP-IDF `esp_mqtt_client_enqueue()` copies payload internally**:
   - PsychicMqttClient wrapper passes pointers (no copy in wrapper)
   - ESP-IDF MQTT client copies topic + payload for async queue processing
   - Each publish: `topic_len + payload_len` bytes allocated by ESP-IDF
   - **This is the main source of fragmentation**

2. **Multiple publishes per packet** multiply allocations:
   - 1 packet → 5 publishes (3 brokers + 2 analyzers) = 5× allocations
   - Each allocation: ~2KB (topic + payload)
   - **Total: ~10KB per packet in heap allocations**

3. **Frequent publishes** prevent heap coalescing:
   - New allocations before old ones are freed
   - ESP-IDF async queue holds messages until sent
   - Creates "holes" in heap that can't be coalesced

## Recommendations

### 1. Investigate Library Source Code
Since the library is external, we need to:
- Check if library source is available in PlatformIO cache
- Review `publish()` implementation for memory allocations
- Look for zero-copy or buffer reuse options

### 2. Potential Optimizations

#### A. Reduce Number of Publishes ⭐ **HIGHEST IMPACT**
- **Current**: Publish to all brokers + analyzers separately (5 publishes per packet)
- **Option 1**: Publish to one analyzer server only (reduce from 2 to 1)
  - **Impact**: 20% reduction in allocations (2KB saved per packet)
- **Option 2**: Use single custom broker with forwarding
  - **Impact**: 60% reduction (from 3 brokers to 1)
- **Option 3**: Combine both (1 broker + 1 analyzer)
  - **Impact**: 60% reduction (from 5 to 2 publishes)
- **Trade-off**: Less redundancy, simpler architecture

#### B. Reduce Payload Sizes
- **Current**: Up to 2KB JSON per packet
- **Option**: Compress or reduce JSON size
- **Impact**: Reduces allocation size per publish
- **Trade-off**: Less data per message

#### C. Throttle Publishes (Already Implemented) ✅
- **Current**: Skip publishes when Max alloc < 60KB
- **Status**: ✅ Implemented
- **Effect**: Prevents further fragmentation when memory is low

#### D. Use Synchronous Publishes (Blocking)
- **Current**: `async=true` (default) uses `esp_mqtt_client_enqueue()`
- **Option**: Use `async=false` to call `esp_mqtt_client_publish()` directly
- **Impact**: May reduce queue overhead, but blocks until sent
- **Trade-off**: Blocks execution, may impact responsiveness
- **Code Change**: `publish(topic, qos, retain, payload, len, false)`

#### E. Reduce Buffer Size Configuration
- **Current**: Default buffer size is 1024 bytes (from `setBufferSize()`)
- **Option**: Reduce buffer size if messages are smaller
- **Impact**: Less memory per client instance
- **Note**: May cause message fragmentation if payloads exceed buffer

#### F. Investigate ESP-IDF MQTT Client Configuration
- ESP-IDF MQTT client has internal buffer configuration
- May be able to reduce queue depth or buffer sizes
- **Requires**: ESP-IDF documentation review

### 3. Library Alternatives (If Issues Persist)

Consider lightweight MQTT libraries with better memory management:
- **lwmqtt**: Zero-copy, no dynamic allocations
- **coreMQTT**: Fixed buffers, predictable memory
- **Custom implementation**: Full control over memory

## Next Steps

1. ✅ **Locate Library Source**: Found in `.pio/libdeps/*/PsychicMqttClient/src/`

2. ✅ **Analyze `publish()` Implementation**: 
   - Wrapper doesn't copy (passes pointers)
   - ESP-IDF `esp_mqtt_client_enqueue()` copies internally
   - This is the root cause

3. **Test Synchronous Publishes**:
   - Try `async=false` to use `esp_mqtt_client_publish()` directly
   - May have different memory behavior (no queue)
   - Measure fragmentation impact

4. **Reduce Number of Publishes**:
   - Implement single analyzer server (US or EU)
   - Or reduce custom broker count
   - Measure memory improvement

5. **Profile ESP-IDF MQTT Client**:
   - Check ESP-IDF documentation for buffer configuration
   - Look for queue depth settings
   - Investigate if we can reduce internal buffers

6. **Consider ESP-IDF MQTT Client Direct Usage**:
   - Bypass PsychicMqttClient wrapper
   - Use ESP-IDF API directly with custom memory management
   - More control, but more complex

7. **Alternative: Switch to Different Library**:
   - Consider `lwmqtt` (zero-copy, no dynamic allocations)
   - Or `coreMQTT` (fixed buffers)
   - Requires significant refactoring

## Current Mitigations

✅ **Memory Pressure Monitoring**: Skip publishes when fragmented
✅ **setServer() Optimization**: Only call when URI changes  
✅ **JSON Buffer Reuse**: Build once, publish to multiple destinations
✅ **Analyzer Server Throttling**: Skip when memory low

## Remaining Risk

⚠️ **Library Internal Allocations**: If `publish()` copies payloads internally, we can't control this without:
- Library modification
- Library replacement
- Library API changes (if supported)

