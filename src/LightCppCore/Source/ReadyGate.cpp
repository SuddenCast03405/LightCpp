#include "LightCpp/Core/ReadyGate.hpp"

#include <utility>

namespace LightCpp::Core {

void ReadyGate::whenReady(Callback callback)
{
    if (!callback) {
        return;
    }
    if (open_) {
        callback();
        return;
    }
    pending_.push_back(std::move(callback));
}

void ReadyGate::open()
{
    if (open_) {
        return;
    }
    open_ = true;
    // Swap the queue out before running anything: a callback may drop() the
    // gate (or queue more work, which runs immediately because the gate is now
    // open), and walking the member while it is being mutated underneath is how
    // that turns into a use-after-free.
    std::vector<Callback> pending;
    pending.swap(pending_);
    for (Callback& callback : pending) {
        // Checked per callback, not once: a callback that drops the gate
        // abandons the ones queued behind it.
        if (!open_) {
            break;
        }
        if (callback) {
            callback();
        }
    }
}

void ReadyGate::drop()
{
    open_ = false;
    pending_.clear();
}

}  // namespace LightCpp::Core
