// LightCppCore - callbacks that wait for a one-shot "ready" edge.
//
// The defect class this exists for (R2-08): code that has to wait for an
// asynchronous handshake instead tests the ready flag *now*, reads "not ready
// yet" as "failed", and returns. The caller's retry budget is then spent on a
// request that was never sent, and the user is left with an empty completion
// popup and no explanation anywhere.
//
// A gate removes the decision from the caller: hand the work over and it either
// runs on the spot (already ready) or is kept until the edge arrives.

#pragma once

#include <cstddef>
#include <functional>
#include <vector>

namespace LightCpp::Core {

/// A readiness latch with a callback queue.
///
/// Two states, open and closed. `whenReady` runs the callback immediately when
/// the gate is open and queues it when it is closed; `open()` runs everything
/// queued so far, in the order it was queued. `drop()` closes the gate and
/// discards the queue without running it — the right answer when the handshake
/// those callbacks were waiting for is never going to complete (a failed start,
/// or a stop()).
///
/// Deliberately Qt-free (see the module rules in LightCppCore): the Qt side of
/// an await is just "spin the event loop", and the queue itself needs no event
/// loop at all.
class ReadyGate {
public:
    using Callback = std::function<void()>;

    /// Run `callback` now when the gate is open, otherwise keep it for the next
    /// `open()`. An empty callback is ignored: queueing one would only turn a
    /// missing call into a silent no-op later.
    void whenReady(Callback callback);

    /// Open the gate and run everything queued, in order. Opening an already
    /// open gate runs nothing (the edge is one-shot, not a level).
    ///
    /// A callback that queues more work from inside `open()` sees an open gate
    /// and runs it immediately; a callback that calls `drop()` abandons the
    /// callbacks queued behind it, because the thing they were waiting for is
    /// gone again.
    void open();

    /// Close the gate — a later start() has to hand-shake from scratch — and
    /// discard the queue without running any of it.
    void drop();

    [[nodiscard]] bool isOpen() const noexcept { return open_; }

    /// How many callbacks are waiting for the edge. For tests and diagnostics.
    [[nodiscard]] std::size_t pendingCount() const noexcept
    {
        return pending_.size();
    }

private:
    bool open_ = false;
    std::vector<Callback> pending_;
};

}  // namespace LightCpp::Core
