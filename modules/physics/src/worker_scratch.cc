#include "worker_scratch.h"

#include <algorithm>
#include <memory>
#include <span>

#include <Jolt/Physics/PhysicsSettings.h>
#include <mimalloc.h>

namespace ysm::physics {

InlineJobs::InlineJobs() : slots_(JPH::cMaxPhysicsJobs) {
    for (auto& slot : slots_) {
        slot.next = free_;
        free_ = &slot;
    }
}

JPH::JobHandle InlineJobs::CreateJob(const char* name, JPH::ColorArg color,
                                     const JobFunction& run, JPH::uint32 dependencies) {
    auto* slot = free_;
    free_ = slot->next;
    auto* job = std::construct_at(&slot->job, name, color, this, run, dependencies);
    JobHandle handle(job);
    if (dependencies == 0) {
        QueueJob(job);
    }
    return handle;
}

void InlineJobs::CreateJobs(const char* name, JPH::ColorArg color, const JobFunction& run,
                            JobHandle* handles, JPH::uint32 count, JPH::uint32 dependencies) {
    for (auto& handle : std::span(handles, count)) {
        handle = CreateJob(name, color, run, dependencies);
    }
}

void InlineJobs::QueueJobs(Job** jobs, JPH::uint count) {
    for (auto* job : std::span(jobs, count)) {
        QueueJob(job);
    }
}

void InlineJobs::FreeJob(Job* job) {
    auto* slot = reinterpret_cast<Slot*>(job);
    std::destroy_at(job);
    slot->next = free_;
    free_ = slot;
}

FallbackTempAllocator::FallbackTempAllocator(size_t stack_bytes)
    : stack_(stack_bytes / sizeof(Block)) {}

bool FallbackTempAllocator::BeginStep(size_t upper_bound) {
    JPH_ASSERT(used_ == 0 && base_ == nullptr);
    capacity_ = stack_.size() * sizeof(Block);
    peak_ = 0;
    if (upper_bound <= capacity_) [[likely]] {
        base_ = reinterpret_cast<std::byte*>(stack_.data());
        return true;
    }
    // Jolt dereferences temp pointers while holding locks. A whole-step loan
    // moves the only fallible spill allocation before that ownership boundary.
    capacity_ = upper_bound;
    base_ = static_cast<std::byte*>(mi_malloc_aligned(upper_bound, alignof(Block)));
    return base_ != nullptr;
}

void FallbackTempAllocator::EndStep() {
    JPH_ASSERT(used_ == 0);
    if (base_ != reinterpret_cast<std::byte*>(stack_.data())) [[unlikely]] {
        mi_free(base_);
    }
    base_ = nullptr;
}

void* FallbackTempAllocator::Allocate(JPH::uint bytes) {
    if (bytes == 0) {
        return nullptr;
    }
    auto* result = base_ + used_;
    used_ += (size_t(bytes) + sizeof(Block) - 1) & ~(sizeof(Block) - 1);
    JPH_ASSERT(used_ <= capacity_);
    peak_ = std::max(peak_, used_);
    return result;
}

void FallbackTempAllocator::Free(void* address, JPH::uint bytes) {
    used_ -= (size_t(bytes) + sizeof(Block) - 1) & ~(sizeof(Block) - 1);
    JPH_ASSERT(bytes == 0 || address == base_ + used_);
}

WorkerScratch& WorkerScratch::Current() {
    thread_local WorkerScratch worker;
    return worker;
}

void WorkerScratch::Prepare(size_t fk_count) {
    if (fk_bones.size() < fk_count) [[unlikely]] {
        fk_states.resize(fk_count);
        fk_bones.resize(fk_count);
    }
}

}  // namespace ysm::physics
