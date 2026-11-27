#pragma once

#include <array>
#include <cstddef>
#include <vector>

#include <Jolt/Jolt.h>
#include <Jolt/Core/JobSystem.h>
#include <Jolt/Core/TempAllocator.h>

#include "physics/physics_world.h"
#include "physics_asset_internal.h"
#include "system_allocator.h"

namespace ysm::physics {

class InlineJobs final : public JPH::JobSystem {
public:
    InlineJobs();
    int GetMaxConcurrency() const override { return 1; }
    JobHandle CreateJob(const char* name, JPH::ColorArg color, const JobFunction& run,
                        JPH::uint32 dependencies) override;
    void CreateJobs(const char* name, JPH::ColorArg color, const JobFunction& run,
                    JobHandle* handles, JPH::uint32 count, JPH::uint32 dependencies) override;
    Barrier* CreateBarrier() override { return &barrier_; }
    void DestroyBarrier(Barrier*) override {}
    void WaitForJobs(Barrier*) override {}

private:
    // A dependency reaching zero runs immediately on the submitting thread.
    class InlineBarrier final : public Barrier {
        void AddJob(const JobHandle&) override {}
        void AddJobs(const JobHandle*, JPH::uint) override {}
        void OnJobFinished(Job*) override {}
    } barrier_;

    void QueueJob(Job* job) override { job->Execute(); }
    void QueueJobs(Job** jobs, JPH::uint count) override;
    void FreeJob(Job* job) override;

    union Slot {
        Job job;
        Slot* next;
        Slot() : next(nullptr) {}
        ~Slot() {}
    };
    std::vector<Slot, SystemAllocator<Slot>> slots_;
    Slot* free_ = nullptr;
};

class FallbackTempAllocator final : public JPH::TempAllocator {
public:
    static constexpr size_t kStackBytes = 1 << 20;
    explicit FallbackTempAllocator(size_t stack_bytes = kStackBytes);
    void* Allocate(JPH::uint bytes) override;
    void Free(void* address, JPH::uint bytes) override;
    bool BeginStep(size_t upper_bound);
    void EndStep();
    size_t PeakBytes() const { return peak_; }

private:
    struct alignas(JPH_RVECTOR_ALIGNMENT) Block { std::byte bytes[JPH_RVECTOR_ALIGNMENT]; };
    std::vector<Block, SystemAllocator<Block>> stack_;
    std::byte* base_ = nullptr;
    size_t capacity_ = 0;
    size_t used_ = 0;
    size_t peak_ = 0;
};

struct WorkerScratch {
    static WorkerScratch& Current();
    explicit WorkerScratch(size_t stack_bytes = FallbackTempAllocator::kStackBytes)
        : temp(stack_bytes) {}
    void Prepare(size_t fk_count);

    InlineJobs jobs;
    FallbackTempAllocator temp;
    // Storage for the gather before the JVM releases its first array borrow.
    using BoneChannels = std::array<float, 14>;
    std::vector<BoneChannels, SystemAllocator<BoneChannels>> fk_bones;
    struct FkState {
        RigidPose animation;
        float scale = 1;
        uint32_t hidden_children = kNoBone;
        DeactivationReason invalid;
        DeactivationReason hidden;
    };
    std::vector<FkState, SystemAllocator<FkState>> fk_states;
};

}  // namespace ysm::physics
