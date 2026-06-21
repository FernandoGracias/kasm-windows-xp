# XP Bridge Wire Protocol

## Transport
- TCP, port 9500
- Guest initiates connection to host at 10.0.2.2:9500
- Persistent connection, guest reconnects on drop (5s backoff)

## Message Format
```
[4 bytes: payload length (big-endian, includes type byte but not these 4 bytes)]
[1 byte: message type]
[payload bytes]
```

## Message Types

| Type | Name           | Direction    | Payload                                        |
|------|----------------|--------------|------------------------------------------------|
| 0x01 | CLIPBOARD_TEXT | Bidirectional | UTF-8 text                                    |
| 0x02 | FILE_START     | Bidirectional | 2-byte filename length + filename (UTF-8) + 8-byte file size (big-endian) |
| 0x03 | FILE_CHUNK     | Bidirectional | Raw bytes (max 32KB per chunk)                |
| 0x04 | FILE_END       | Bidirectional | Empty                                          |
| 0x05 | PING           | Bidirectional | Empty                                          |
| 0x06 | PONG           | Bidirectional | Empty                                          |
| 0x07 | SET_RESOLUTION | Host→Guest   | 4-byte width + 4-byte height (big-endian)     |

## Flow

### Clipboard
Sender detects clipboard change → sends CLIPBOARD_TEXT with new content.
Receiver applies to local clipboard, suppresses echo (tracks last-set value).

### File Transfer
Sender sends FILE_START → multiple FILE_CHUNK (≤32KB each) → FILE_END.
Receiver creates file, writes chunks, closes on FILE_END.

### Resolution
Host monitors KasmVNC display size (via xdpyinfo). When it changes, finds the
best VESA mode that fits and sends SET_RESOLUTION. Guest calls ChangeDisplaySettings.

### Heartbeat
Either side sends PING every 10s of inactivity. Other side replies PONG.
If no PONG within 15s, connection is considered dead → reconnect.
