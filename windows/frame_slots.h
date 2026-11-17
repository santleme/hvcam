// frame_slots — three-slot "always newest" frame exchange.
//
// Producer (the MF capture callback thread) writes into the one slot
// that is neither `newest` nor held by the consumer, then publishes
// it. Consumer (the socket send loop) claims `newest`, sends it,
// releases it. With three slots there is always exactly one free
// slot for the producer, so capture never blocks on a slow client —
// it just overwrites forward and the client sees the newest.
//
// The index exchange is a ~30ns critical section under a mutex. A
// packed-CAS lock-free variant is possible, but this version is
// provably correct on first write; revisit only if profiling says so.
#pragma once

#include <stdint.h>
#include <mutex>
#include <vector>

struct HvSlot {
    std::vector<uint8_t> bytes;
    uint64_t seq = 0;
    uint64_t ts_us = 0;
    uint16_t w = 0, h = 0;
    uint32_t format = 0;
    bool valid = false;
};

class FrameSlots {
public:
    // Producer side (capture thread): returns the slot to fill.
    // Always non-null — with 3 slots one is always free.
    HvSlot *acquire_write();
    void publish_write(HvSlot *slot); // marks it newest

    // Consumer side (send loop): claims the newest slot or null.
    // Call release_read(slot) when done sending.
    HvSlot *acquire_read();
    void release_read(HvSlot *slot);

private:
    std::mutex m_;
    HvSlot slots_[3];
    int newest_ = -1;   // index into slots_, or -1 before first frame
    int reading_ = -1;  // slot currently owned by the consumer, or -1
};
