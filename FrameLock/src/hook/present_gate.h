// Reentrant-call-safe, process-wide serialization for the complete captured Present cycle.
// The caller holds this gate from before StepPre through the original Present and StepPost.
#pragma once
#include <mutex>

namespace flpresent {

class SequenceGate {
    std::mutex mutex_;
public:
    using Guard = std::unique_lock<std::mutex>;
    Guard Enter() { return Guard(mutex_); }
    Guard TryEnter() { return Guard(mutex_, std::try_to_lock); }
};

} // namespace flpresent
