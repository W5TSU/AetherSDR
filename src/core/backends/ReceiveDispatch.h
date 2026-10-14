#pragma once

namespace AetherSDR {

// What a backend did with a control request: sent it to the radio, applied it
// locally, or refused it. Upstream's ReceiveCommand.h result type; the fork
// carries only this enum, not the request structs beside it.
enum class ReceiveDispatch { Dispatched, LocalOnly, Unsupported };

} // namespace AetherSDR
