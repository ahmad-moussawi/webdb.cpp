#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace webdb {

using page_id_t = int32_t;
using generation_id_t = uint64_t;

inline constexpr page_id_t INVALID_PAGE_ID = -1;
inline constexpr page_id_t MASTER_PAGE_A_ID = 0;
inline constexpr page_id_t MASTER_PAGE_B_ID = 1;
inline constexpr page_id_t FIRST_DATA_PAGE_ID = 2;
inline constexpr page_id_t MAX_DATA_PAGE_ID = std::numeric_limits<page_id_t>::max();

inline constexpr size_t DATABASE_PAGE_SIZE = 4096;
inline constexpr size_t PAGE_HEADER_SIZE = 36;
inline constexpr size_t SLOT_ENTRY_SIZE = 4;

// floor((DATABASE_PAGE_SIZE - PAGE_HEADER_SIZE) / SLOT_ENTRY_SIZE) = 1015.
inline constexpr uint16_t MAX_SLOT_COUNT = 1015;

// One tuple plus one slot entry must fit in an otherwise empty TablePage.
inline constexpr size_t MAX_TUPLE_SIZE =
    DATABASE_PAGE_SIZE - PAGE_HEADER_SIZE - SLOT_ENTRY_SIZE; // 4056

inline constexpr uint16_t MAX_COLUMNS = 256;
inline constexpr size_t MAX_TEXT_SIZE = MAX_TUPLE_SIZE;

// Iterator corruption guard: 1,048,576 * 4096 = 4 GiB maximum chain traversal.
inline constexpr size_t MAX_PAGES = 1'048'576;

enum class TypeId : uint8_t {
    INVALID = 0,
    INT = 1,
    DOUBLE = 2,
    TEXT = 3,
};

enum class SlotState : uint8_t {
    EMPTY = 0,     // Never valid inside [0, slot_count) on disk in Phase 1.
    LIVE = 1,      // Contains a readable tuple payload.
    DEAD = 2,      // Deleted/replaced tuple; payload region is reclaimable.
    FORWARDED = 3, // Reserved for Phase 3; invalid on disk in Phase 1.
};

struct RID {
    page_id_t page_id{INVALID_PAGE_ID};
    uint16_t slot_num{0};

    constexpr bool is_valid() const noexcept {
        return page_id != INVALID_PAGE_ID;
    }

    constexpr bool operator==(const RID& other) const noexcept {
        return page_id == other.page_id && slot_num == other.slot_num;
    }

    constexpr bool operator!=(const RID& other) const noexcept {
        return !(*this == other);
    }
};

enum class StorageResult : uint8_t {
    // The requested storage operation completed successfully. Callers may use
    // this value to distinguish a completed operation from a state-machine
    // result such as PAGE_NOT_RESIDENT or FLUSH_REQUIRED. Examples include a
    // validated page read, a successful page allocation, or a completed flush.
    SUCCESS = 0,

    // The target page has no space for the requested structural change. This
    // is a normal capacity result rather than corruption or I/O failure. Page
    // and table-page callers use it when inserting a tuple or slot would cross
    // the page's free-space boundary; the caller may allocate or select another
    // page and retry the insertion.
    PAGE_FULL = 1,

    // The requested tuple or serialized value cannot fit even in an otherwise
    // empty database page under the current storage-format limits. Unlike
    // PAGE_FULL, this describes an intrinsically oversized record, so choosing
    // another page cannot make the operation succeed. For example, a TEXT
    // value whose encoded tuple exceeds MAX_TUPLE_SIZE produces this result.
    TUPLE_TOO_LARGE = 2,

    // The caller referenced a slot that is not available in the target page.
    // This is returned by tuple/page accessors when a slot number is outside the
    // slot directory or refers to a deleted, empty, or otherwise non-readable
    // entry. Callers should treat it as a missing record, not as a page-I/O
    // failure.
    SLOT_NOT_FOUND = 3,

    // The bytes of a page or storage structure violate its integrity checks or
    // binary-layout invariants. Typical causes include a CRC mismatch, invalid
    // offsets, impossible slot boundaries, an invalid page identifier in a page
    // header, or malformed serialized metadata. Callers must not use the page
    // contents as valid data after receiving this result.
    CORRUPTED_PAGE = 4,

    // The bytes are structurally readable but were written by an incompatible
    // storage-format version. This is distinct from CORRUPTED_PAGE: the data may
    // be internally valid for an older or newer format, but this engine cannot
    // safely interpret it without migration or a compatible reader.
    VERSION_MISMATCH = 5,

    // The supplied data does not match the schema or type contract required for
    // the operation. Examples include a tuple with the wrong column count, a
    // value whose TypeId differs from the schema, or a typed NULL with an
    // incompatible column type.
    SCHEMA_MISMATCH = 6,

    // The caller supplied an invalid argument or requested an operation that is
    // invalid in the current API contract. Examples include a null output
    // pointer, an invalid page ID, a duplicate page in a response batch, a
    // malformed operation plan, or an attempt to complete a flush that is not
    // currently active. This result should be used for caller/protocol errors,
    // not for backend I/O failures.
    INVALID_ARGUMENT = 7,

    // Traversal encountered a cycle where the storage format requires a bounded
    // acyclic chain. TableHeap scanners use this result when page links revisit
    // an already visited page or otherwise exceed the permitted traversal
    // safety bound. Callers should stop scanning rather than continue following
    // links that may be corrupt or attacker-controlled.
    CYCLE_DETECTED = 8,

    // The underlying storage backend could not complete the requested I/O or
    // synchronization operation. Examples include a failed page read/write,
    // an IndexedDB transaction error, or a native sync failure. This result does
    // not assert that the in-memory page is corrupt; callers must preserve the
    // appropriate retry/error state and must not report durability success.
    IO_ERROR = 9,

    // The buffer pool cannot reserve a frame for the request, and no immediate
    // recovery path such as a dirty-page flush can be started. This is a bounded
    // resource condition, not a terminal database-corruption result. Callers
    // may retry after unpinning pages, completing an outstanding flush, or
    // reducing the operation's working set. Example: every frame is pinned or
    // all candidate frames are unavailable for eviction.
    BUFFER_FULL = 10,

    // A page lookup found that the requested page is not resident and a load has
    // been registered. The caller must yield to the host/scheduler and wait for
    // page bytes before retrying the pin or access. This is a normal asynchronous
    // control-flow result, not an error. For example, BufferPoolManager::pin_page
    // can return this when it reserves an ABSENT frame as LOADING for page 42.
    PAGE_NOT_RESIDENT = 11,

    // The requested page is already being loaded by another operation. The
    // caller joins the existing waiter set instead of creating a duplicate frame
    // or issuing a second logical load. The scheduler should yield and retry the
    // original pin after the shared load completes; the host supplies the page
    // once through the pool-level API.
    LOAD_IN_PROGRESS = 12,

    // The requested resource is temporarily occupied by an operation that must
    // finish first. In Phase 3 this primarily means that another flush batch is
    // active, or that the requested page is FLUSHING. The caller must retain its
    // original request and retry after the active batch clears; it must not create
    // a second flush batch or treat this as permanent out-of-memory.
    BUSY = 13,

    // The request cannot proceed until this caller's dirty-pressure flush batch
    // is persisted. Unlike BUSY, this result identifies the operation that
    // successfully created/owns the flush opportunity. The scheduler transitions
    // to FLUSHING, the host writes the returned batch, and the scheduler retries
    // the original pin request after successful completion. Example: all
    // unpinned replacement candidates are DIRTY, so they must be flushed before
    // a new page can use a frame.
    FLUSH_REQUIRED = 14,
};

struct UpdateResult {
    StorageResult status{StorageResult::INVALID_ARGUMENT};
    RID old_rid{};
    RID new_rid{};
    bool rid_changed{false};

    constexpr bool success() const noexcept {
        return status == StorageResult::SUCCESS;
    }
};

} // namespace webdb
