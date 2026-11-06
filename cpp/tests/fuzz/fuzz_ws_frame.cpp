// libFuzzer harness — WebSocket frame decoder (ws_try_decode_frame), the bytes of every open
// WebSocket. A whole buffer is decoded frame by frame the way the event loop does it (offset walk
// over a string_view), so a length that walks past the buffer, a mask read out of bounds, or a
// "need more data" that never ends on a malformed header shows up. ASan+UBSan.
#include "look/websocket.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string buf(reinterpret_cast<const char*>(data), size);
    size_t off = 0;
    for (int n = 0; n < 4096 && off <= buf.size(); ++n) {
        look::WsFrame f = look::ws_try_decode_frame(std::string_view(buf).substr(off));
        if (f.protocol_error || !f.complete) break;
        if (f.consumed == 0 || f.consumed > buf.size() - off) __builtin_trap();   // must advance, inside buffer
        off += f.consumed;
    }
    return 0;
}
