#pragma once

#include "common/types.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace webdb {

using frame_id_t = uint32_t;
using pin_token_t = uint64_t;
using operation_id_t = uint64_t;
using flush_batch_id_t = uint64_t;

class BufferPoolManager;

enum class AccessMode : uint8_t {
    // The caller may inspect page bytes but must not mutate them through the
    // handle. Its mutable_data() accessor returns nullptr.
    READ_ONLY = 0,

    // The caller may mutate page bytes. Releasing this handle marks the page
    // DIRTY so a later flush cannot lose the write.
    READ_WRITE = 1,
};

class PageHandle {
public:
    PageHandle() noexcept = default;
    ~PageHandle() noexcept;

    PageHandle(const PageHandle&) = delete;
    PageHandle& operator=(const PageHandle&) = delete;

    PageHandle(PageHandle&& other) noexcept;
    PageHandle& operator=(PageHandle&& other) noexcept;

    // Returns a read-only view of the pinned page bytes. The pointer remains
    // valid only while this handle owns its pin.
    const uint8_t* data() const noexcept;

    // Returns mutable bytes only for a READ_WRITE handle; READ_ONLY handles
    // return nullptr. The caller must not retain this pointer after release.
    uint8_t* mutable_data() noexcept;

    // Releases this handle's pin immediately. The stored operation ID is used
    // for ownership validation, and repeated calls are harmless.
    void reset() noexcept;

    // Called by BufferPoolManager when an external unpin or operation-wide
    // release invalidates this handle before its destructor runs.
    void invalidate_from_manager() noexcept;

    page_id_t page_id() const noexcept { return page_id_; }
    frame_id_t frame_id() const noexcept { return frame_id_; }
    pin_token_t pin_token() const noexcept { return pin_token_; }
    AccessMode access_mode() const noexcept { return access_mode_; }
    bool owns_pin() const noexcept { return manager_ != nullptr; }

private:
    friend class BufferPoolManager;

    PageHandle(BufferPoolManager* manager,
               page_id_t page_id,
               frame_id_t frame_id,
               pin_token_t pin_token,
               operation_id_t operation_id,
               AccessMode access_mode,
               uint8_t* bytes) noexcept;

    BufferPoolManager* manager_{nullptr};
    page_id_t page_id_{INVALID_PAGE_ID};
    frame_id_t frame_id_{0};
    pin_token_t pin_token_{0};
    operation_id_t operation_id_{0};
    AccessMode access_mode_{AccessMode::READ_ONLY};
    uint8_t* bytes_{nullptr};
};

enum class BufferFrameState : uint8_t {
    ABSENT = 0,
    LOADING = 1,
    RESIDENT = 2,
    DIRTY = 3,
    FLUSHING = 4,
};

struct BufferPoolConfig {
    // Maximum number of page frames owned by this buffer pool. Each frame holds
    // exactly one DATABASE_PAGE_SIZE byte page. For example, frame_count = 4
    // creates a deliberately small pool that is useful for deterministic
    // eviction tests; zero is rejected by BufferPoolManager.
    uint32_t frame_count{0};

    // Maximum number of page loads that may be reserved as LOADING at once.
    // This bounds host requests and prevents an operation burst from reserving
    // unbounded frame/I/O state while pages are awaiting asynchronous delivery.
    uint32_t max_pending_loads{0};

    // Maximum number of pages that one flush batch may contain. A dirty-pressure
    // request must respect this limit so a single host write cannot grow without
    // bound. The value also controls the largest batch returned to JavaScript.
    uint32_t max_flush_batch_pages{0};
};

struct FrameDescriptor {
    // Stable index of this frame in the pool's contiguous storage. It is used by
    // the Clock hand and remains valid even when the frame's page mapping changes.
    frame_id_t frame_id{0};

    // Logical page currently assigned to the frame. INVALID_PAGE_ID means the
    // frame is ABSENT and has no page mapping. Master pages 0/1 and data pages
    // use the same field because the buffer pool owns physical residency, not
    // catalog semantics.
    page_id_t page_id{INVALID_PAGE_ID};

    // Current lifecycle state. The state determines whether the frame may be
    // pinned, modified, selected for eviction, or involved in host I/O.
    BufferFrameState state{BufferFrameState::ABSENT};

    // Number of active operation-owned pin tokens referencing this frame. A
    // nonzero count prevents eviction; LOADING and FLUSHING frames are also
    // protected by their in-flight I/O even when no user operation is reading
    // them directly.
    uint32_t pin_count{0};

    // Clock replacement reference bit. Successful access/pin sets it; the Clock
    // hand clears it to grant a recently used unpinned frame a second chance.
    bool ref_bit{false};

    // Monotonically increasing mutation generation for the page. It changes when
    // the frame becomes dirty or is modified, allowing an old flush completion to
    // avoid clearing a newer in-memory mutation.
    uint64_t dirty_generation{0};

    // Generation captured when the current flush snapshot was created. A flush
    // completion is accepted only when its batch and generation still match this
    // frame's active snapshot.
    uint64_t flushing_generation{0};

    // Unique identity of the current flush attempt. This distinguishes retries
    // that happen at the same dirty generation.
    flush_batch_id_t flushing_batch_id{0};
};

class BufferPoolManager {
public:
    friend class PageHandle;
    // Upper bound on frame_count. This protects native and WASM processes from
    // accidental or hostile configurations that would allocate excessive memory
    // before normal resource checks can run.
    static constexpr uint32_t MAX_FRAME_COUNT = 1'048'576;

    // Creates a fixed-size buffer pool and initializes every frame as ABSENT.
    // Invalid resource configurations are rejected before frame storage is
    // exposed to callers.
    explicit BufferPoolManager(BufferPoolConfig config);

    // Releases the pool-owned storage. Active I/O and operation pins must already
    // be resolved by the scheduler before destruction in the full implementation.
    ~BufferPoolManager();

    // Frame-table ownership is unique: moving/copying a manager would invalidate
    // frame addresses and operation pin tokens, so both operations are disabled.
    BufferPoolManager(const BufferPoolManager&) = delete;
    BufferPoolManager& operator=(const BufferPoolManager&) = delete;

    // Returns the configured number of fixed frames, including ABSENT frames.
    size_t frame_count() const noexcept;

    // Returns frames in RESIDENT, DIRTY, or FLUSHING state. For example, a pool
    // with two clean pages and one dirty page reports resident_count() == 3.
    size_t resident_count() const noexcept;

    // Returns frames currently marked DIRTY. FLUSHING frames are counted here
    // only after a failed flush returns them to DIRTY.
    size_t dirty_count() const noexcept;

    // Returns frames reserved for a host read whose bytes have not arrived yet.
    size_t loading_count() const noexcept;

    // Returns frames whose immutable snapshots are currently being written by
    // the host. These frames cannot be evicted or modified during the flush.
    size_t flushing_count() const noexcept;

    // Returns frames currently available in the ABSENT free-frame list. This is
    // the first allocation source checked before Clock eviction is attempted.
    size_t free_frame_count() const noexcept;

    // Returns the frame index at which the next Clock inspection begins. The
    // value is diagnostic in Step 1 and becomes replacement-policy state later.
    frame_id_t get_clock_hand() const noexcept;

    // Finds the unique frame mapped to page_id. ABSENT frames are not mapped;
    // std::nullopt means the page is not currently tracked by this pool.
    std::optional<frame_id_t> find_frame_by_page_id(page_id_t page_id) const noexcept;

    // Returns a copy of a frame's diagnostic metadata. Invalid frame IDs return
    // std::nullopt, allowing tests and diagnostics to probe bounds safely.
    std::optional<FrameDescriptor> get_frame_descriptor(frame_id_t frame_id) const noexcept;

    // Reports whether page_id has a valid resident image. This includes clean,
    // dirty, and flushing frames, but excludes ABSENT and LOADING frames.
    bool is_page_resident(page_id_t page_id) const noexcept;

    // Reports whether page_id has a reserved frame waiting for host-supplied
    // bytes. A true result means callers should wait/retry, not create a second
    // frame for the same page.
    bool is_page_loading(page_id_t page_id) const noexcept;

    // Returns the number of active pins for page_id, or zero if the page is not
    // currently mapped. A nonzero value makes the page ineligible for eviction.
    uint32_t get_pin_count(page_id_t page_id) const noexcept;

    // Pins a resident page for operation_id and returns a move-only RAII handle.
    // A missing page begins a load and returns PAGE_NOT_RESIDENT; an existing
    // LOADING page returns LOAD_IN_PROGRESS. No output handle is modified on
    // failure. READ_WRITE handles mark the page dirty when released.
    StorageResult pin_page(page_id_t page_id,
                           operation_id_t operation_id,
                           AccessMode access_mode,
                           PageHandle& out_handle);

    // Releases the exact token owned by operation_id. Foreign-operation,
    // unknown-token, and already-released tokens are rejected.
    StorageResult unpin_page(pin_token_t pin_token,
                             operation_id_t operation_id,
                             bool is_dirty = false);

    // Marks the exact token's frame dirty without releasing the pin.
    StorageResult mark_page_dirty(pin_token_t pin_token, operation_id_t operation_id);

    // Releases every pin owned by operation_id. This is idempotent for an
    // operation with no pins and is used when an operation completes or aborts.
    StorageResult release_operation_pins(operation_id_t operation_id) noexcept;

    // Reserves an ABSENT frame for a page load and transitions it to LOADING.
    // The returned frame ID is stable until the page is released. If the page is
    // already loading, LOAD_IN_PROGRESS is returned; duplicate residency is
    // rejected instead of creating a second mutable copy.
    StorageResult begin_page_load(page_id_t page_id, frame_id_t& out_frame_id);

    // Completes a synchronous/test page load by transitioning LOADING to
    // RESIDENT. The page bytes are supplied by the minimal test backend in a
    // later step; this Step 1 operation establishes lifecycle ownership only.
    StorageResult complete_page_load(page_id_t page_id);

    // Synchronously assigns an ABSENT frame as a resident page. This models the
    // Step 1 test backend, which has page bytes available immediately and does
    // not need to expose asynchronous host I/O yet.
    StorageResult load_page(page_id_t page_id, frame_id_t& out_frame_id);

    // Marks a clean resident page dirty after an in-memory mutation. Step 1 has
    // no pin token yet, so later steps will add operation ownership checks around
    // this lifecycle transition.
    StorageResult mark_page_dirty(page_id_t page_id);

    // Moves an unpinned dirty page into FLUSHING and returns the immutable
    // batch/generation snapshot that the host must echo on completion.
    StorageResult begin_page_flush(page_id_t page_id,
                                   flush_batch_id_t& out_batch_id,
                                   uint64_t& out_generation);

    // Completes a simulated flush only when its batch and generation still match
    // the active snapshot. Success returns the page to RESIDENT; failure
    // preserves the page in DIRTY and clears the snapshot metadata.
    StorageResult complete_page_flush(page_id_t page_id,
                                      flush_batch_id_t batch_id,
                                      uint64_t generation,
                                      bool success);

    // Releases an unpinned resident page mapping and returns its frame to the
    // ABSENT free list. Loading, dirty, or flushing pages cannot be discarded by
    // this Step 1 lifecycle method.
    StorageResult release_page(page_id_t page_id);

private:
    struct Frame {
        // Metadata for one fixed frame. The corresponding bytes live at
        // pool_storage_ + frame_id * DATABASE_PAGE_SIZE.
        FrameDescriptor descriptor{};
    };

    // Immutable configuration captured at construction. It controls allocation
    // and pending-I/O limits for the lifetime of this manager.
    BufferPoolConfig config_{};

    // Contiguous ownership of all frame bytes. Keeping this allocation stable
    // ensures PageHandle pointers remain valid while their frame is pinned.
    std::unique_ptr<uint8_t[]> pool_storage_;

    // One descriptor per physical frame, indexed by frame_id. Vector capacity is
    // established during construction and is not changed by page replacement.
    std::vector<Frame> frames_;

    // IDs of frames whose descriptors are ABSENT. New allocations consume this
    // list before invoking the Clock replacement policy.
    std::vector<frame_id_t> free_frames_;

    // Authoritative reverse lookup enforcing the one-frame-per-page invariant.
    // A page is inserted when assigned to a frame and removed when that frame is
    // released or evicted.
    std::unordered_map<page_id_t, frame_id_t> page_to_frame_;

    struct PinRecord {
        operation_id_t operation_id{0};
        page_id_t page_id{INVALID_PAGE_ID};
        frame_id_t frame_id{0};
        AccessMode access_mode{AccessMode::READ_ONLY};
        PageHandle* handle{nullptr};
    };

    // Each token identifies exactly one operation-owned pin. The reverse index
    // lets release_operation_pins() unwind all tokens without scanning frames.
    std::unordered_map<pin_token_t, PinRecord> pins_;
    std::unordered_map<operation_id_t, std::vector<pin_token_t>> operation_pins_;
    pin_token_t next_pin_token_{1};
    flush_batch_id_t next_flush_batch_id_{1};

    // Operations waiting for a page whose frame is LOADING. The first miss
    // reserves the frame and adds its operation here; later misses join this
    // list instead of creating duplicate load requests.
    std::unordered_map<page_id_t, std::vector<operation_id_t>> loading_waiters_;

    // Clock replacement cursor. Step 1 initializes it; later steps advance it
    // during bounded second-chance victim selection.
    frame_id_t clock_hand_{0};

    // Returns the mutable byte address for a valid frame. The offset is stable
    // for the lifetime of the pool, so Step 2 can populate PageHandle::bytes
    // without exposing pool_storage_ itself. Callers must validate frame_id
    // before invoking this helper; all internal callers obtain IDs from
    // frames_ or the checked free-frame/page maps.
    uint8_t* get_frame_bytes(frame_id_t frame_id) noexcept;

    // Const counterpart used by read-only inspection and future snapshot code.
    // It applies the same fixed-frame offset while preventing mutation through
    // the returned pointer.
    const uint8_t* get_frame_bytes(frame_id_t frame_id) const noexcept;

    StorageResult assign_frame(page_id_t page_id,
                               BufferFrameState state,
                               frame_id_t& out_frame_id);
    Frame* find_frame(page_id_t page_id) noexcept;
    const Frame* find_frame(page_id_t page_id) const noexcept;
    static bool is_valid_page_id(page_id_t page_id) noexcept;
    StorageResult release_pin_token(pin_token_t pin_token, operation_id_t operation_id) noexcept;
    StorageResult register_load_waiter(page_id_t page_id, operation_id_t operation_id);
    void unregister_load_waiter(page_id_t page_id, operation_id_t operation_id) noexcept;
    void discard_loading_frame(page_id_t page_id) noexcept;
};

} // namespace webdb