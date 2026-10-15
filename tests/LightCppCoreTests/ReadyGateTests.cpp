// ReadyGateTests — the await behind R2-08.
//
// A completion retry after a clangd restart used to test "is the server
// running?" straight after start() and read the (always false) answer as
// failure, spending the retry budget without ever asking the server. The fix
// hands the request to a gate that runs it on the ready edge; these cases pin
// down that contract, including what must happen when the edge never arrives.

#include "LightCpp/Core/ReadyGate.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using LightCpp::Core::ReadyGate;

/// Records call order, so "queued then run in order" is checkable.
struct Recorder {
    std::vector<std::string> calls;

    ReadyGate::Callback operator()(std::string name)
    {
        return [this, name] { calls.push_back(name); };
    }
};

}  // namespace

TEST(ReadyGate, RunsImmediatelyWhenAlreadyOpen)
{
    ReadyGate gate;
    gate.open();
    Recorder recorder;

    gate.whenReady(recorder("work"));

    EXPECT_TRUE(gate.isOpen());
    EXPECT_EQ(recorder.calls, (std::vector<std::string>{"work"}));
    EXPECT_EQ(gate.pendingCount(), 0u);
}

TEST(ReadyGate, KeepsCallbacksUntilTheEdgeArrives)
{
    ReadyGate gate;
    Recorder recorder;

    gate.whenReady(recorder("first"));
    gate.whenReady(recorder("second"));
    gate.whenReady(recorder("third"));

    // Nothing runs before the handshake: this is the whole point of the gate.
    EXPECT_FALSE(gate.isOpen());
    EXPECT_TRUE(recorder.calls.empty());
    EXPECT_EQ(gate.pendingCount(), 3u);

    gate.open();

    EXPECT_EQ(recorder.calls,
              (std::vector<std::string>{"first", "second", "third"}));
    EXPECT_EQ(gate.pendingCount(), 0u);
}

TEST(ReadyGate, DropDiscardsTheQueueWithoutRunningIt)
{
    ReadyGate gate;
    Recorder recorder;

    gate.whenReady(recorder("never"));
    gate.drop();

    EXPECT_FALSE(gate.isOpen());
    EXPECT_EQ(gate.pendingCount(), 0u);

    // The handshake the callback was waiting for is not coming: opening later
    // (a fresh start()) must not revive it.
    gate.open();
    EXPECT_TRUE(recorder.calls.empty());
}

TEST(ReadyGate, OpeningTwiceRunsNothingTheSecondTime)
{
    ReadyGate gate;
    Recorder recorder;

    gate.whenReady(recorder("once"));
    gate.open();
    gate.open();
    gate.whenReady(recorder("after"));

    EXPECT_EQ(recorder.calls, (std::vector<std::string>{"once", "after"}));
}

TEST(ReadyGate, WorkQueuedFromInsideACallbackRunsImmediately)
{
    ReadyGate gate;
    Recorder recorder;

    gate.whenReady([&gate, &recorder] {
        recorder.calls.push_back("outer");
        // The gate is open by now, so this must not wait for another edge.
        gate.whenReady(recorder("inner"));
    });

    gate.open();

    EXPECT_EQ(recorder.calls, (std::vector<std::string>{"outer", "inner"}));
    EXPECT_EQ(gate.pendingCount(), 0u);
}

TEST(ReadyGate, ACallbackThatDropsTheGateAbandonsTheQueueBehindIt)
{
    ReadyGate gate;
    Recorder recorder;

    gate.whenReady([&gate, &recorder] {
        recorder.calls.push_back("first");
        gate.drop();  // e.g. the server died again while answering
    });
    gate.whenReady(recorder("second"));

    gate.open();

    EXPECT_EQ(recorder.calls, (std::vector<std::string>{"first"}));
    EXPECT_FALSE(gate.isOpen());
    EXPECT_EQ(gate.pendingCount(), 0u);
}

TEST(ReadyGate, AnEmptyCallbackIsIgnored)
{
    ReadyGate gate;

    gate.whenReady({});

    // An empty queue opens into nothing, and does not resurrect anything.
    EXPECT_EQ(gate.pendingCount(), 0u);
    gate.open();
    EXPECT_TRUE(gate.isOpen());
    EXPECT_EQ(gate.pendingCount(), 0u);
}

TEST(ReadyGate, OpenGateRunsAnEmptyCallbackWithoutComplaining)
{
    // The empty-callback guard must not swallow a real one after the edge.
    ReadyGate gate;
    gate.open();
    Recorder recorder;

    gate.whenReady({});
    gate.whenReady(recorder("real"));

    EXPECT_EQ(recorder.calls, (std::vector<std::string>{"real"}));
}
