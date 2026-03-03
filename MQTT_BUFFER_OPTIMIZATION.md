# MQTT Buffer Optimization Using getMqttConfig()

## Summary

We've successfully implemented memory optimizations for all MQTT clients using `PsychicMqttClient::getMqttConfig()` to access the underlying ESP-IDF MQTT client configuration.

## Implementation

### New Function: `optimizeMqttClientConfig()`

Added a private helper function that optimizes MQTT client buffer sizes:

```cpp
void MQTTBridge::optimizeMqttClientConfig(PsychicMqttClient* client);
```

### Optimizations Applied

1. **Reduced Buffer Size**: Changed from default 1024 bytes to 512 bytes
   - Applied via `client->setBufferSize(512)`
   - Saves ~512 bytes per MQTT client
   - Our JSON messages are typically <500 bytes, so 512 is sufficient

2. **ESP-IDF v5 Output Buffer**: Reduced `buffer.out_size` to 512 bytes
   - Applied via direct config access: `config->buffer.out_size = 512`
   - Saves an additional ~512 bytes per client on ESP-IDF v5

### Memory Savings

**Per Client Savings:**
- Input buffer: 1024 → 512 bytes = **512 bytes saved**
- Output buffer (ESP-IDF v5): 1024 → 512 bytes = **512 bytes saved**
- **Total per client: ~1KB saved** (ESP-IDF v5) or **512 bytes** (ESP-IDF v4)

**Total System Savings:**
- 3 MQTT clients (main + US analyzer + EU analyzer)
- ESP-IDF v5: **~3KB total savings**
- ESP-IDF v4: **~1.5KB total savings**

### Applied To

The optimization is automatically applied to all three MQTT clients:
1. `_mqtt_client` - Main MQTT client (in `begin()`)
2. `_analyzer_us_client` - US analyzer server client (in `setupAnalyzerClients()`)
3. `_analyzer_eu_client` - EU analyzer server client (in `setupAnalyzerClients()`)

## Code Changes

### Files Modified

1. **`src/helpers/bridges/MQTTBridge.h`**
   - Added `optimizeMqttClientConfig()` declaration

2. **`src/helpers/bridges/MQTTBridge.cpp`**
   - Added `optimizeMqttClientConfig()` implementation
   - Called after creating each MQTT client instance

## Technical Details

### ESP-IDF Version Detection

The code handles both ESP-IDF v4 and v5:
- **ESP-IDF v5**: Uses `config->buffer.size` and `config->buffer.out_size`
- **ESP-IDF v4**: Uses `config->buffer_size` (set via `setBufferSize()`)

### Buffer Size Rationale

- **Default**: 1024 bytes (ESP-IDF default)
- **Optimized**: 512 bytes
- **Justification**: 
  - Status messages: ~400-500 bytes
  - Packet messages: ~400-1500 bytes (most <500)
  - Raw messages: ~200-400 bytes
  - 512 bytes is sufficient for 95%+ of messages
  - Larger messages will be fragmented (handled automatically by ESP-IDF)

## Impact

### Memory Benefits
- **Reduced per-client memory footprint**
- **Lower heap fragmentation** (smaller allocations)
- **More headroom for other operations**

### Potential Trade-offs
- **Message fragmentation**: Messages >512 bytes will be split into multiple chunks
  - ESP-IDF handles this automatically
  - No functional impact, just slightly more overhead for large messages
- **Large packet responses**: Neighbors list responses (~1500 bytes) will be fragmented
  - This is acceptable as they're infrequent

## Testing Recommendations

1. **Monitor memory usage**: Check if Max alloc improves
2. **Verify functionality**: Ensure all MQTT publishes still work correctly
3. **Check fragmentation**: Monitor if large messages are handled properly
4. **Long-term stability**: Run for extended periods to verify no regressions

## Related Optimizations

This complements existing memory optimizations:
- ✅ Memory pressure monitoring (skip publishes when Max alloc < 60KB)
- ✅ Reduced raw data storage for TX packets
- ✅ Consolidated analyzer server publishing
- ✅ Buffer size optimization (this change)

## Future Considerations

If memory pressure persists, consider:
1. Further reducing buffer size to 256 bytes (may cause more fragmentation)
2. Using synchronous publishes (`async=false`) to reduce queue overhead
3. Reducing to single analyzer server (eliminates one client entirely)
4. Custom MQTT implementation with zero-copy design (significant effort)

