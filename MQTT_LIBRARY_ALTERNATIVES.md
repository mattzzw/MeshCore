# MQTT Library Alternatives with WebSocket Support

## Current Library
- **PsychicMqttClient** (`elims/PsychicMqttClient@^0.2.4`)
- **WebSocket Support**: ✅ Yes (WSS://)
- **Memory Issue**: ESP-IDF `esp_mqtt_client_enqueue()` copies payloads internally
- **Platform**: ESP32 only

## Alternative Libraries with WebSocket Support

### 1. AsyncMqttClient ⭐ **RECOMMENDED**
- **Author**: Marvin Roger
- **GitHub**: https://github.com/marvinroger/AsyncMqttClient
- **PlatformIO**: `marvinroger/AsyncMqttClient`
- **WebSocket Support**: ✅ Yes (WSS://)
- **Memory Management**: 
  - Uses ESP-IDF MQTT client (same underlying library as PsychicMqttClient)
  - **Same memory issue**: ESP-IDF copies payloads internally
- **Pros**:
  - Well-established, widely used
  - Similar API to PsychicMqttClient (almost drop-in replacement)
  - Good documentation
- **Cons**:
  - Same underlying ESP-IDF library = same memory fragmentation issue
  - May not solve the memory problem

### 2. ESP-IDF MQTT Client (Direct Usage)
- **Library**: Built into ESP-IDF framework
- **WebSocket Support**: ✅ Yes (WSS://)
- **Memory Management**: 
  - Same as PsychicMqttClient (it's a wrapper)
  - Can configure buffer sizes via `esp_mqtt_client_config_t`
  - May allow more control over memory
- **Pros**:
  - No wrapper overhead
  - Direct control over configuration
  - Can set buffer sizes, queue depths
- **Cons**:
  - More complex API
  - Requires ESP-IDF knowledge
  - Still copies payloads internally
- **Configuration Options**:
  ```cpp
  esp_mqtt_client_config_t mqtt_cfg = {
      .buffer.size = 1024,  // Can reduce this
      .buffer.out_size = 1024,  // Can reduce this
      // ... other config
  };
  ```

### 3. lwmqtt (Lightweight MQTT)
- **GitHub**: https://github.com/256dpi/lwmqtt
- **WebSocket Support**: ❌ **NO** - TCP only
- **Memory Management**: 
  - Zero-copy design
  - No dynamic allocations
  - Fixed buffers
- **Pros**:
  - Excellent memory efficiency
  - Zero-copy, no fragmentation
  - Very lightweight
- **Cons**:
  - **No WebSocket support** (deal breaker for analyzer servers)
  - Would need separate WebSocket implementation
  - More complex integration

### 4. PubSubClient
- **PlatformIO**: `knolleary/PubSubClient`
- **WebSocket Support**: ❌ **NO** - TCP only
- **Memory Management**: 
  - Uses fixed buffer (configurable size)
  - Copies payloads into buffer
- **Pros**:
  - Simple API
  - Widely used
  - Predictable memory usage
- **Cons**:
  - **No WebSocket support** (deal breaker)
  - Synchronous (blocks)
  - Less efficient than async libraries

### 5. Custom WebSocket + MQTT Implementation
- **Approach**: Use ESP-IDF WebSocket client + custom MQTT protocol layer
- **WebSocket Support**: ✅ Yes (full control)
- **Memory Management**: 
  - Full control over allocations
  - Can implement zero-copy
  - Custom buffer management
- **Pros**:
  - Complete control over memory
  - Can optimize for our use case
  - No unnecessary copies
- **Cons**:
  - Significant development effort
  - Need to implement MQTT protocol
  - Testing and maintenance burden

## Recommendation

### Option A: Stay with PsychicMqttClient + Optimize Usage ⭐ **BEST SHORT-TERM**
- **Why**: All ESP32 MQTT libraries use ESP-IDF underneath = same memory issue
- **Actions**:
  1. Reduce number of publishes (single analyzer server)
  2. Test synchronous publishes (`async=false`)
  3. Configure ESP-IDF buffer sizes via PsychicMqttClient
  4. Keep memory pressure monitoring

### Option B: Switch to AsyncMqttClient
- **Why**: More mature, better documented, similar API
- **Trade-off**: Same memory issue (uses ESP-IDF)
- **Effort**: Medium (API is similar, mostly drop-in)

### Option C: Use ESP-IDF MQTT Client Directly
- **Why**: More control over configuration
- **Actions**:
  - Bypass wrapper library
  - Configure buffer sizes directly
  - May reduce some overhead
- **Effort**: High (need to rewrite MQTT bridge code)
- **Benefit**: Can tune ESP-IDF buffer sizes

### Option D: Custom Implementation (Long-term)
- **Why**: Complete control over memory
- **Effort**: Very High (weeks of development)
- **Benefit**: Optimal memory usage, zero-copy possible

## Key Finding

⚠️ **All ESP32 MQTT libraries that support WebSockets use ESP-IDF `esp_mqtt_client` underneath**, which copies payloads internally. This is a limitation of the ESP-IDF framework, not the wrapper libraries.

**Options to reduce memory impact:**
1. ✅ Reduce number of publishes (already identified)
2. ✅ Memory pressure monitoring (already implemented)
3. ⚠️ Configure ESP-IDF buffer sizes (may help)
4. ⚠️ Use synchronous publishes (may reduce queue overhead)
5. ⚠️ Custom implementation (significant effort)

## Next Steps

1. **Test ESP-IDF Buffer Configuration**:
   - Try reducing `setBufferSize()` in PsychicMqttClient
   - May reduce per-client memory but won't fix publish allocations

2. **Test Synchronous Publishes**:
   - Try `async=false` to bypass queue
   - May reduce memory but blocks execution

3. **Reduce Publishes**:
   - Implement single analyzer server
   - Measure memory improvement

4. **Consider ESP-IDF Direct Usage**:
   - If other optimizations don't help enough
   - More control but more complexity

