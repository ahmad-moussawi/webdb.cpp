#include "storage/buffer_pool_manager.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace webdb {

namespace {

bool is_resident_state(BufferFrameState state) noexcept {
    return state == BufferFrameState::RESIDENT || state == BufferFrameState::DIRTY ||
           state == BufferFrameState::FLUSHING;
}

} // namespace

PageHandle::PageHandle(BufferPoolManager* manager,
                       page_id_t page_id,
                       frame_id_t frame_id,
                       pin_token_t pin_token,
                       operation_id_t operation_id,
                       AccessMode access_mode,
                       uint8_t* bytes) noexcept
    : manager_(manager),
      page_id_(page_id),
      frame_id_(frame_id),
      pin_token_(pin_token),
    operation_id_(operation_id),
      access_mode_(access_mode),
      bytes_(bytes) {}

PageHandle::~PageHandle() noexcept { reset(); }

PageHandle::PageHandle(PageHandle&& other) noexcept
    : manager_(other.manager_),
      page_id_(other.page_id_),
      frame_id_(other.frame_id_),
      pin_token_(other.pin_token_),
    operation_id_(other.operation_id_),
      access_mode_(other.access_mode_),
      bytes_(other.bytes_) {
    other.manager_ = nullptr;
    other.page_id_ = INVALID_PAGE_ID;
    other.frame_id_ = 0;
    other.bytes_ = nullptr;
    other.pin_token_ = 0;
    other.operation_id_ = 0;
    other.access_mode_ = AccessMode::READ_ONLY;
    if (manager_ != nullptr) {
        const auto pin_it = manager_->pins_.find(pin_token_);
        if (pin_it != manager_->pins_.end()) pin_it->second.handle = this;
    }
}

PageHandle& PageHandle::operator=(PageHandle&& other) noexcept {
    if (this == &other) return *this;
    reset();
    manager_ = other.manager_;
    page_id_ = other.page_id_;
    frame_id_ = other.frame_id_;
    pin_token_ = other.pin_token_;
    operation_id_ = other.operation_id_;
    access_mode_ = other.access_mode_;
    bytes_ = other.bytes_;
    other.manager_ = nullptr;
    other.page_id_ = INVALID_PAGE_ID;
    other.frame_id_ = 0;
    other.bytes_ = nullptr;
    other.pin_token_ = 0;
    other.operation_id_ = 0;
    other.access_mode_ = AccessMode::READ_ONLY;
    if (manager_ != nullptr) {
        const auto pin_it = manager_->pins_.find(pin_token_);
        if (pin_it != manager_->pins_.end()) pin_it->second.handle = this;
    }
    return *this;
}

const uint8_t* PageHandle::data() const noexcept { return bytes_; }

uint8_t* PageHandle::mutable_data() noexcept {
    return access_mode_ == AccessMode::READ_WRITE ? bytes_ : nullptr;
}

void PageHandle::reset() noexcept {
    if (manager_ != nullptr) manager_->release_pin_token(pin_token_, operation_id_);
    manager_ = nullptr;
    page_id_ = INVALID_PAGE_ID;
    frame_id_ = 0;
    bytes_ = nullptr;
    pin_token_ = 0;
    operation_id_ = 0;
    access_mode_ = AccessMode::READ_ONLY;
}

void PageHandle::invalidate_from_manager() noexcept {
    manager_ = nullptr;
    page_id_ = INVALID_PAGE_ID;
    frame_id_ = 0;
    bytes_ = nullptr;
    pin_token_ = 0;
    operation_id_ = 0;
    access_mode_ = AccessMode::READ_ONLY;
}

BufferPoolManager::BufferPoolManager(BufferPoolConfig config) : config_(config) {
    if (config_.frame_count == 0 || config_.frame_count > MAX_FRAME_COUNT ||
        config_.max_pending_loads == 0 || config_.max_pending_loads > config_.frame_count ||
        config_.max_flush_batch_pages == 0 || config_.max_flush_batch_pages > config_.frame_count) {
        throw std::invalid_argument("Buffer pool configuration is outside the supported range");
    }

    const auto frame_count = static_cast<size_t>(config_.frame_count);
    if (frame_count > std::numeric_limits<size_t>::max() / DATABASE_PAGE_SIZE) {
        throw std::invalid_argument("Buffer pool storage size overflows size_t");
    }

    pool_storage_ = std::make_unique<uint8_t[]>(frame_count * DATABASE_PAGE_SIZE);
    frames_.resize(frame_count);
    free_frames_.reserve(frame_count);
    page_to_frame_.reserve(frame_count);

    for (frame_id_t frame_id = 0; frame_id < config_.frame_count; ++frame_id) {
        frames_[frame_id].descriptor.frame_id = frame_id;
        free_frames_.push_back(frame_id);
    }
}

BufferPoolManager::~BufferPoolManager() = default;

size_t BufferPoolManager::frame_count() const noexcept { return frames_.size(); }

size_t BufferPoolManager::resident_count() const noexcept {
    size_t count = 0;
    for (const Frame& frame : frames_) {
        if (is_resident_state(frame.descriptor.state)) ++count;
    }
    return count;
}

size_t BufferPoolManager::dirty_count() const noexcept {
    size_t count = 0;
    for (const Frame& frame : frames_) {
        if (frame.descriptor.state == BufferFrameState::DIRTY) ++count;
    }
    return count;
}

size_t BufferPoolManager::loading_count() const noexcept {
    size_t count = 0;
    for (const Frame& frame : frames_) {
        if (frame.descriptor.state == BufferFrameState::LOADING) ++count;
    }
    return count;
}

size_t BufferPoolManager::flushing_count() const noexcept {
    size_t count = 0;
    for (const Frame& frame : frames_) {
        if (frame.descriptor.state == BufferFrameState::FLUSHING) ++count;
    }
    return count;
}

size_t BufferPoolManager::free_frame_count() const noexcept { return free_frames_.size(); }

frame_id_t BufferPoolManager::get_clock_hand() const noexcept { return clock_hand_; }

std::optional<frame_id_t> BufferPoolManager::find_frame_by_page_id(page_id_t page_id) const noexcept {
    const auto it = page_to_frame_.find(page_id);
    if (it == page_to_frame_.end()) return std::nullopt;
    return it->second;
}

std::optional<FrameDescriptor> BufferPoolManager::get_frame_descriptor(frame_id_t frame_id) const noexcept {
    if (static_cast<size_t>(frame_id) >= frames_.size()) return std::nullopt;
    return frames_[frame_id].descriptor;
}

bool BufferPoolManager::is_page_resident(page_id_t page_id) const noexcept {
    const auto frame_id = find_frame_by_page_id(page_id);
    return frame_id.has_value() && is_resident_state(frames_[*frame_id].descriptor.state);
}

bool BufferPoolManager::is_page_loading(page_id_t page_id) const noexcept {
    const auto frame_id = find_frame_by_page_id(page_id);
    return frame_id.has_value() && frames_[*frame_id].descriptor.state == BufferFrameState::LOADING;
}

uint32_t BufferPoolManager::get_pin_count(page_id_t page_id) const noexcept {
    const auto frame_id = find_frame_by_page_id(page_id);
    return frame_id.has_value() ? frames_[*frame_id].descriptor.pin_count : 0;
}

StorageResult BufferPoolManager::pin_page(page_id_t page_id,
                                           operation_id_t operation_id,
                                           AccessMode access_mode,
                                           PageHandle& out_handle) {
    if (!is_valid_page_id(page_id) || operation_id == 0) {
        return StorageResult::INVALID_ARGUMENT;
    }

    const auto frame_id = find_frame_by_page_id(page_id);
    if (!frame_id.has_value()) {
        frame_id_t loading_frame = 0;
        const StorageResult load_result = begin_page_load(page_id, loading_frame);
        if (load_result != StorageResult::SUCCESS) return load_result;
        try {
            const StorageResult waiter_result = register_load_waiter(page_id, operation_id);
            if (waiter_result != StorageResult::SUCCESS) {
                discard_loading_frame(page_id);
                return waiter_result;
            }
        } catch (...) {
            discard_loading_frame(page_id);
            throw;
        }
        return StorageResult::PAGE_NOT_RESIDENT;
    }

    FrameDescriptor& descriptor = frames_[*frame_id].descriptor;
    if (descriptor.state == BufferFrameState::LOADING) {
        const StorageResult waiter_result = register_load_waiter(page_id, operation_id);
        if (waiter_result != StorageResult::SUCCESS) return waiter_result;
        return StorageResult::LOAD_IN_PROGRESS;
    }
    if (!is_resident_state(descriptor.state) || descriptor.state == BufferFrameState::FLUSHING) {
        return StorageResult::BUSY;
    }

    if (next_pin_token_ == 0) {
        return StorageResult::BUFFER_FULL;
    }

    const pin_token_t token = next_pin_token_++;
    const auto insertion = pins_.emplace(token, PinRecord{
        operation_id,
        page_id,
        *frame_id,
        access_mode
    });

    if (!insertion.second) {
        return StorageResult::INVALID_ARGUMENT;
    }

    try {
        operation_pins_[operation_id].push_back(token);
    } catch (...) {
        pins_.erase(token);
        throw;
    }

    ++descriptor.pin_count;
    descriptor.ref_bit = true;
    out_handle = PageHandle(this, page_id, *frame_id, token, operation_id, access_mode,
                            get_frame_bytes(*frame_id));
    pins_.find(token)->second.handle = &out_handle;

    return StorageResult::SUCCESS;
}

StorageResult BufferPoolManager::unpin_page(pin_token_t pin_token,
                                            operation_id_t operation_id,
                                            bool is_dirty) {
    const auto pin_it = pins_.find(pin_token);
    if (pin_it == pins_.end() || pin_it->second.operation_id != operation_id) {
        return StorageResult::INVALID_ARGUMENT;
    }
    if (is_dirty && pin_it->second.access_mode == AccessMode::READ_ONLY) {
        return StorageResult::INVALID_ARGUMENT;
    }
    return release_pin_token(pin_token, operation_id);
}

StorageResult BufferPoolManager::mark_page_dirty(pin_token_t pin_token,
                                                  operation_id_t operation_id) {
    const auto pin_it = pins_.find(pin_token);
    if (pin_it == pins_.end() || pin_it->second.operation_id != operation_id) {
        return StorageResult::INVALID_ARGUMENT;
    }
    FrameDescriptor& descriptor = frames_[pin_it->second.frame_id].descriptor;
    if (descriptor.state != BufferFrameState::RESIDENT && descriptor.state != BufferFrameState::DIRTY) {
        return StorageResult::INVALID_ARGUMENT;
    }
    descriptor.state = BufferFrameState::DIRTY;
    ++descriptor.dirty_generation;
    if (descriptor.dirty_generation == 0) ++descriptor.dirty_generation;
    return StorageResult::SUCCESS;
}

StorageResult BufferPoolManager::release_operation_pins(operation_id_t operation_id) noexcept {
    const auto operation_it = operation_pins_.find(operation_id);

    if (operation_it == operation_pins_.end()) {
        return StorageResult::SUCCESS;
    }

    const std::vector<pin_token_t> tokens = operation_it->second;

    for (const pin_token_t token : tokens) release_pin_token(token, operation_id);
    return StorageResult::SUCCESS;
}

uint8_t* BufferPoolManager::get_frame_bytes(frame_id_t frame_id) noexcept {
    // Frame IDs are validated by the owning operation before this private helper
    // is called. Keeping the offset calculation here prevents future page-handle
    // code from duplicating the storage-layout arithmetic.
    return pool_storage_.get() + static_cast<size_t>(frame_id) * DATABASE_PAGE_SIZE;
}

const uint8_t* BufferPoolManager::get_frame_bytes(frame_id_t frame_id) const noexcept {
    return pool_storage_.get() + static_cast<size_t>(frame_id) * DATABASE_PAGE_SIZE;
}

bool BufferPoolManager::is_valid_page_id(page_id_t page_id) noexcept {
    return page_id >= 0 && page_id <= MAX_DATA_PAGE_ID;
}

StorageResult BufferPoolManager::register_load_waiter(page_id_t page_id,
                                                       operation_id_t operation_id) {
    auto& waiters = loading_waiters_[page_id];
    if (std::find(waiters.begin(), waiters.end(), operation_id) != waiters.end()) {
        return StorageResult::INVALID_ARGUMENT;
    }
    waiters.push_back(operation_id);
    return StorageResult::SUCCESS;
}

void BufferPoolManager::unregister_load_waiter(page_id_t page_id,
                                               operation_id_t operation_id) noexcept {
    const auto waiter_it = loading_waiters_.find(page_id);
    if (waiter_it == loading_waiters_.end()) return;
    auto& waiters = waiter_it->second;
    waiters.erase(std::remove(waiters.begin(), waiters.end(), operation_id), waiters.end());
    if (waiters.empty()) loading_waiters_.erase(waiter_it);
}

void BufferPoolManager::discard_loading_frame(page_id_t page_id) noexcept {
    const auto frame_id = find_frame_by_page_id(page_id);
    if (!frame_id.has_value()) return;
    FrameDescriptor& descriptor = frames_[*frame_id].descriptor;
    if (descriptor.state != BufferFrameState::LOADING || descriptor.pin_count != 0) return;
    loading_waiters_.erase(page_id);
    page_to_frame_.erase(page_id);
    descriptor = FrameDescriptor{};
    descriptor.frame_id = *frame_id;
    free_frames_.push_back(*frame_id);
}

StorageResult BufferPoolManager::release_pin_token(pin_token_t pin_token,
                                                   operation_id_t operation_id) noexcept {
    const auto pin_it = pins_.find(pin_token);

    if (pin_it == pins_.end()) {
        return StorageResult::INVALID_ARGUMENT;
    }

    if (operation_id != 0 && pin_it->second.operation_id != operation_id) {
        return StorageResult::INVALID_ARGUMENT;
    }

    const PinRecord record = pin_it->second;
    FrameDescriptor& descriptor = frames_[record.frame_id].descriptor;

    if (descriptor.pin_count == 0) {
        return StorageResult::INVALID_ARGUMENT;
    }

    --descriptor.pin_count;

    if (record.handle != nullptr) record.handle->invalidate_from_manager();

    if (record.access_mode == AccessMode::READ_WRITE) {
        if (descriptor.state == BufferFrameState::RESIDENT) descriptor.state = BufferFrameState::DIRTY;
        ++descriptor.dirty_generation;
        if (descriptor.dirty_generation == 0) ++descriptor.dirty_generation;
    }

    pins_.erase(pin_it);
    const auto operation_it = operation_pins_.find(record.operation_id);

    if (operation_it != operation_pins_.end()) {
        auto& tokens = operation_it->second;
        tokens.erase(std::remove(tokens.begin(), tokens.end(), pin_token), tokens.end());

        if (tokens.empty()) {
            operation_pins_.erase(operation_it);
        }
    }

    return StorageResult::SUCCESS;
}

BufferPoolManager::Frame* BufferPoolManager::find_frame(page_id_t page_id) noexcept {
    const auto frame_id = find_frame_by_page_id(page_id);
    return frame_id.has_value() ? &frames_[*frame_id] : nullptr;
}

const BufferPoolManager::Frame* BufferPoolManager::find_frame(page_id_t page_id) const noexcept {
    const auto frame_id = find_frame_by_page_id(page_id);
    return frame_id.has_value() ? &frames_[*frame_id] : nullptr;
}

// Assigns one currently ABSENT frame to a new logical page mapping. This is the
// common reservation path for both begin_page_load() and the synchronous
// load_page() test helper: the former requests LOADING and the latter requests
// RESIDENT. For example, when page 42 is absent and frame 3 is the last free
// frame, this method removes frame 3 from free_frames_, records 42 -> 3 in
// page_to_frame_, initializes the descriptor, and returns frame 3 to the caller.
//
// The operation is deliberately limited to LOADING and RESIDENT. DIRTY and
// FLUSHING are reached only through their lifecycle transitions so mutation and
// flush-generation invariants cannot be bypassed by a caller assigning a frame
// directly. Duplicate page IDs are rejected before the free list is changed,
// and a full free list returns BUFFER_FULL without modifying the frame table.
StorageResult BufferPoolManager::assign_frame(page_id_t page_id,
                                               BufferFrameState state,
                                               frame_id_t& out_frame_id) {
    if (!is_valid_page_id(page_id) || (state != BufferFrameState::LOADING &&
                                       state != BufferFrameState::RESIDENT)) {
        return StorageResult::INVALID_ARGUMENT;
    }
    if (page_to_frame_.find(page_id) != page_to_frame_.end()) {
        return StorageResult::INVALID_ARGUMENT;
    }
    if (free_frames_.empty()) return StorageResult::BUFFER_FULL;

    const frame_id_t frame_id = free_frames_.back();
    // Insert first: unordered_map::emplace may allocate and throw. Keeping the
    // free list and descriptor untouched until insertion succeeds preserves the
    // frame-table invariants if allocation fails.
    const auto insertion = page_to_frame_.emplace(page_id, frame_id);
    if (!insertion.second) return StorageResult::INVALID_ARGUMENT;

    FrameDescriptor& descriptor = frames_[frame_id].descriptor;
    descriptor.page_id = page_id;
    descriptor.state = state;
    descriptor.pin_count = 0;
    descriptor.ref_bit = false;
    descriptor.dirty_generation = 0;
    descriptor.flushing_generation = 0;
    free_frames_.pop_back();
    out_frame_id = frame_id;
    return StorageResult::SUCCESS;
}

StorageResult BufferPoolManager::begin_page_load(page_id_t page_id, frame_id_t& out_frame_id) {
    if (!is_valid_page_id(page_id)) return StorageResult::INVALID_ARGUMENT;
    const Frame* existing = find_frame(page_id);
    if (existing != nullptr) {
        if (existing->descriptor.state == BufferFrameState::LOADING) {
            return StorageResult::LOAD_IN_PROGRESS;
        }
        return existing->descriptor.state == BufferFrameState::FLUSHING ? StorageResult::BUSY
                                                                          : StorageResult::INVALID_ARGUMENT;
    }
    return assign_frame(page_id, BufferFrameState::LOADING, out_frame_id);
}

StorageResult BufferPoolManager::complete_page_load(page_id_t page_id) {
    Frame* frame = find_frame(page_id);
    if (frame == nullptr) return StorageResult::INVALID_ARGUMENT;
    if (frame->descriptor.state != BufferFrameState::LOADING) return StorageResult::INVALID_ARGUMENT;
    frame->descriptor.state = BufferFrameState::RESIDENT;
    return StorageResult::SUCCESS;
}

StorageResult BufferPoolManager::load_page(page_id_t page_id, frame_id_t& out_frame_id) {
    return assign_frame(page_id, BufferFrameState::RESIDENT, out_frame_id);
}

StorageResult BufferPoolManager::mark_page_dirty(page_id_t page_id) {
    Frame* frame = find_frame(page_id);
    if (frame == nullptr || (frame->descriptor.state != BufferFrameState::RESIDENT &&
                             frame->descriptor.state != BufferFrameState::DIRTY)) {
        return StorageResult::INVALID_ARGUMENT;
    }
    frame->descriptor.state = BufferFrameState::DIRTY;
    ++frame->descriptor.dirty_generation;
    if (frame->descriptor.dirty_generation == 0) ++frame->descriptor.dirty_generation;
    return StorageResult::SUCCESS;
}

StorageResult BufferPoolManager::begin_page_flush(page_id_t page_id) {
    Frame* frame = find_frame(page_id);
    if (frame == nullptr || frame->descriptor.state != BufferFrameState::DIRTY ||
        frame->descriptor.pin_count != 0) {
        return StorageResult::INVALID_ARGUMENT;
    }
    frame->descriptor.state = BufferFrameState::FLUSHING;
    frame->descriptor.flushing_generation = frame->descriptor.dirty_generation;
    return StorageResult::SUCCESS;
}

StorageResult BufferPoolManager::complete_page_flush(page_id_t page_id, bool success) {
    Frame* frame = find_frame(page_id);
    if (frame == nullptr || frame->descriptor.state != BufferFrameState::FLUSHING) {
        return StorageResult::INVALID_ARGUMENT;
    }
    frame->descriptor.state = success ? BufferFrameState::RESIDENT : BufferFrameState::DIRTY;
    if (success) frame->descriptor.flushing_generation = 0;
    return StorageResult::SUCCESS;
}

StorageResult BufferPoolManager::release_page(page_id_t page_id) {
    const auto frame_id = find_frame_by_page_id(page_id);
    if (!frame_id.has_value()) return StorageResult::INVALID_ARGUMENT;
    FrameDescriptor& descriptor = frames_[*frame_id].descriptor;
    if (descriptor.pin_count != 0 || descriptor.state == BufferFrameState::LOADING ||
        descriptor.state == BufferFrameState::DIRTY || descriptor.state == BufferFrameState::FLUSHING) {
        return StorageResult::INVALID_ARGUMENT;
    }
    page_to_frame_.erase(page_id);
    descriptor = FrameDescriptor{};
    descriptor.frame_id = *frame_id;
    free_frames_.push_back(*frame_id);
    return StorageResult::SUCCESS;
}

} // namespace webdb