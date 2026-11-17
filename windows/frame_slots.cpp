#include "frame_slots.h"

HvSlot *FrameSlots::acquire_write()
{
    std::lock_guard<std::mutex> lock(m_);
    for (int i = 0; i < 3; i++) {
        if (i != newest_ && i != reading_)
            return &slots_[i];
    }
    return nullptr; // unreachable with 3 slots; kept for safety
}

void FrameSlots::publish_write(HvSlot *slot)
{
    std::lock_guard<std::mutex> lock(m_);
    slot->valid = true;
    newest_ = (int)(slot - slots_);
}

HvSlot *FrameSlots::acquire_read()
{
    std::lock_guard<std::mutex> lock(m_);
    if (newest_ < 0 || newest_ == reading_)
        return nullptr;
    reading_ = newest_;
    return &slots_[reading_];
}

void FrameSlots::release_read(HvSlot *slot)
{
    std::lock_guard<std::mutex> lock(m_);
    if (reading_ == (int)(slot - slots_))
        reading_ = -1;
}
