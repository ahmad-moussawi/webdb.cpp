#include "test_support.hpp"

#include "storage/buffer_pool_manager.hpp"

namespace webdb::test {

void test_buffer_pool_manager() {
    std::cout << "[RUNNING] buffer pool frame table tests..." << std::endl;

    BufferPoolManager pool(BufferPoolConfig{4, 2, 2});
    TEST_ASSERT(pool.frame_count() == 4, "Configured frame count is retained");
    TEST_ASSERT(pool.free_frame_count() == 4, "All frames begin on the free list");
    TEST_ASSERT(pool.resident_count() == 0 && pool.dirty_count() == 0,
                "A new pool has no resident or dirty pages");
    TEST_ASSERT(pool.loading_count() == 0 && pool.flushing_count() == 0,
                "A new pool has no in-flight page operations");
    TEST_ASSERT(pool.get_clock_hand() == 0, "Clock hand starts at frame zero");
    TEST_ASSERT(!pool.find_frame_by_page_id(FIRST_DATA_PAGE_ID).has_value(),
                "Absent pages have no frame mapping");
    TEST_ASSERT(!pool.get_frame_descriptor(4).has_value(),
                "Invalid frame IDs return no descriptor");

    for (frame_id_t frame_id = 0; frame_id < 4; ++frame_id) {
        const auto descriptor = pool.get_frame_descriptor(frame_id);
        TEST_ASSERT(descriptor.has_value() && descriptor->frame_id == frame_id &&
                        descriptor->page_id == INVALID_PAGE_ID &&
                        descriptor->state == BufferFrameState::ABSENT && descriptor->pin_count == 0 &&
                        !descriptor->ref_bit && descriptor->dirty_generation == 0 &&
                        descriptor->flushing_generation == 0 && descriptor->flushing_batch_id == 0,
                    "Every frame starts with a clean ABSENT descriptor");
    }

    bool rejected_zero_frames = false;
    try {
        BufferPoolManager invalid_pool(BufferPoolConfig{0, 1, 1});
    } catch (const std::invalid_argument&) {
        rejected_zero_frames = true;
    }
    TEST_ASSERT(rejected_zero_frames, "Zero-sized buffer pools are rejected");

    bool rejected_oversized_pool = false;
    try {
        BufferPoolManager oversized_pool(
            BufferPoolConfig{BufferPoolManager::MAX_FRAME_COUNT + 1, 1, 1});
    } catch (const std::invalid_argument&) {
        rejected_oversized_pool = true;
    }
    TEST_ASSERT(rejected_oversized_pool, "Frame counts above the configured maximum are rejected");

    bool rejected_zero_load_limit = false;
    try {
        BufferPoolManager invalid_load_limit(BufferPoolConfig{4, 0, 2});
    } catch (const std::invalid_argument&) {
        rejected_zero_load_limit = true;
    }
    TEST_ASSERT(rejected_zero_load_limit, "Zero pending-load limits are rejected");

    bool rejected_oversized_flush_limit = false;
    try {
        BufferPoolManager invalid_flush_limit(BufferPoolConfig{4, 2, 5});
    } catch (const std::invalid_argument&) {
        rejected_oversized_flush_limit = true;
    }
    TEST_ASSERT(rejected_oversized_flush_limit,
                "Flush batches larger than the frame budget are rejected");

    frame_id_t resident_frame = 0;
    TEST_ASSERT(pool.load_page(FIRST_DATA_PAGE_ID, resident_frame) == StorageResult::SUCCESS,
                "The synchronous test backend assigns an ABSENT frame as RESIDENT");
    TEST_ASSERT(pool.is_page_resident(FIRST_DATA_PAGE_ID) && pool.resident_count() == 1 &&
                    pool.free_frame_count() == 3,
                "A loaded page is mapped and removed from the free list");
    frame_id_t duplicate_frame = 0;
    const size_t free_frames_before_duplicate = pool.free_frame_count();
    const auto descriptor_before_duplicate = pool.get_frame_descriptor(resident_frame);
    TEST_ASSERT(pool.load_page(FIRST_DATA_PAGE_ID, duplicate_frame) == StorageResult::INVALID_ARGUMENT,
                "A page cannot be assigned to a second frame");
    const auto descriptor_after_duplicate = pool.get_frame_descriptor(resident_frame);
    TEST_ASSERT(pool.find_frame_by_page_id(FIRST_DATA_PAGE_ID).value() == resident_frame &&
                    pool.free_frame_count() == free_frames_before_duplicate &&
                    descriptor_before_duplicate.has_value() && descriptor_after_duplicate.has_value() &&
                    descriptor_after_duplicate->page_id == descriptor_before_duplicate->page_id &&
                    descriptor_after_duplicate->state == descriptor_before_duplicate->state &&
                    descriptor_after_duplicate->pin_count == descriptor_before_duplicate->pin_count,
                "Failed duplicate assignment preserves mapping, descriptor, and free-list state");

    PageHandle missing_handle;
    const size_t free_frames_before_miss = pool.free_frame_count();
    TEST_ASSERT(pool.pin_page(FIRST_DATA_PAGE_ID + 10, 300, AccessMode::READ_ONLY, missing_handle) ==
                    StorageResult::PAGE_NOT_RESIDENT && !missing_handle.owns_pin() &&
                    pool.is_page_loading(FIRST_DATA_PAGE_ID + 10) &&
                    pool.free_frame_count() == free_frames_before_miss - 1,
                "The first cache miss reserves a LOADING frame without modifying the output handle");
    TEST_ASSERT(pool.pin_page(FIRST_DATA_PAGE_ID + 10, 301, AccessMode::READ_ONLY, missing_handle) ==
                    StorageResult::LOAD_IN_PROGRESS,
                "A second operation joins the existing page load");
    TEST_ASSERT(pool.get_pending_page_ids() == std::vector<page_id_t>{FIRST_DATA_PAGE_ID + 10},
                "The pool exposes its pending loading page IDs");
    std::vector<operation_id_t> woken_operations;
    const std::vector<operation_id_t> expected_woken_operations{300, 301};
    const std::vector<uint8_t> supplied_bytes(DATABASE_PAGE_SIZE, 0x7B);
    TEST_ASSERT(pool.provide_page(FIRST_DATA_PAGE_ID + 10,
                                  std::vector<uint8_t>(DATABASE_PAGE_SIZE - 1),
                                  woken_operations) == StorageResult::INVALID_ARGUMENT &&
                    pool.is_page_loading(FIRST_DATA_PAGE_ID + 10),
                "Malformed page bytes do not complete a shared load");
    TEST_ASSERT(pool.provide_page(FIRST_DATA_PAGE_ID + 10, supplied_bytes, woken_operations) ==
                    StorageResult::SUCCESS &&
                    woken_operations == expected_woken_operations &&
                    pool.is_page_resident(FIRST_DATA_PAGE_ID + 10) &&
                    pool.get_frame_descriptor(pool.find_frame_by_page_id(FIRST_DATA_PAGE_ID + 10).value())
                            ->ref_bit,
                "One supplied page wakes every waiter and becomes resident");
    PageHandle supplied_handle;
    TEST_ASSERT(pool.pin_page(FIRST_DATA_PAGE_ID + 10, 302, AccessMode::READ_ONLY, supplied_handle) ==
                    StorageResult::SUCCESS && supplied_handle.data()[0] == 0x7B,
                "Supplied bytes are copied into the pool-owned frame");
    supplied_handle.reset();
    TEST_ASSERT(pool.release_operation_pins(300) == StorageResult::SUCCESS &&
                    pool.release_operation_pins(301) == StorageResult::SUCCESS &&
                    pool.release_page(FIRST_DATA_PAGE_ID + 10) == StorageResult::SUCCESS,
                "A completed shared load can return its frame to the free list");

    PageHandle aborted_handle;
    TEST_ASSERT(pool.pin_page(FIRST_DATA_PAGE_ID + 11, 303, AccessMode::READ_ONLY, aborted_handle) ==
                    StorageResult::PAGE_NOT_RESIDENT,
                "A new page miss creates another shared load reservation");
    TEST_ASSERT(pool.release_operation_pins(303) == StorageResult::SUCCESS,
                "Cancelling a load waiter removes only that operation from the waiter registry");
    TEST_ASSERT(pool.pin_page(FIRST_DATA_PAGE_ID + 11, 304, AccessMode::READ_ONLY, aborted_handle) ==
                    StorageResult::LOAD_IN_PROGRESS,
                "A remaining load reservation accepts a new waiter");
    std::vector<operation_id_t> failed_operations;
    TEST_ASSERT(pool.abort_page_load(FIRST_DATA_PAGE_ID + 11, failed_operations) == StorageResult::SUCCESS &&
                    failed_operations == std::vector<operation_id_t>(1, 304) &&
                    !pool.find_frame_by_page_id(FIRST_DATA_PAGE_ID + 11).has_value() &&
                    pool.free_frame_count() == free_frames_before_miss,
                "Aborting a load wakes remaining waiters and recycles the frame");
    TEST_ASSERT(pool.pin_page(FIRST_DATA_PAGE_ID + 11, 305, AccessMode::READ_ONLY, aborted_handle) ==
                    StorageResult::PAGE_NOT_RESIDENT,
                "An aborted load can be retried");
    TEST_ASSERT(pool.abort_page_load(FIRST_DATA_PAGE_ID + 11, failed_operations) == StorageResult::SUCCESS,
                "A retried load can be aborted cleanly");

    TEST_ASSERT(pool.pin_page(FIRST_DATA_PAGE_ID + 12, 306, AccessMode::READ_ONLY, aborted_handle) ==
                    StorageResult::PAGE_NOT_RESIDENT &&
                    pool.pin_page(FIRST_DATA_PAGE_ID + 12, 306, AccessMode::READ_ONLY, aborted_handle) ==
                        StorageResult::LOAD_IN_PROGRESS,
                "Retrying the same loading page is idempotent for its waiter");
    TEST_ASSERT(pool.abort_page_load(FIRST_DATA_PAGE_ID + 12, failed_operations) == StorageResult::SUCCESS,
                "An idempotent load retry can be cleaned up");

    BufferPoolManager late_supply_pool(BufferPoolConfig{1, 1, 1});
    PageHandle late_supply_handle;
    TEST_ASSERT(late_supply_pool.pin_page(FIRST_DATA_PAGE_ID, 501, AccessMode::READ_ONLY, late_supply_handle) ==
                    StorageResult::PAGE_NOT_RESIDENT &&
                    late_supply_pool.release_operation_pins(501) == StorageResult::SUCCESS,
                "Cancelling the only load waiter leaves the host load reservation active");
    TEST_ASSERT(late_supply_pool.get_pending_page_ids() == std::vector<page_id_t>{FIRST_DATA_PAGE_ID},
                "Cancelled loads remain visible until the host completes or aborts them");
    std::vector<operation_id_t> late_woken_operations{999};
    TEST_ASSERT(late_supply_pool.provide_page(FIRST_DATA_PAGE_ID,
                                              std::vector<uint8_t>(DATABASE_PAGE_SIZE, 0x4D),
                                              late_woken_operations) == StorageResult::SUCCESS &&
                    late_woken_operations.empty() && late_supply_pool.is_page_resident(FIRST_DATA_PAGE_ID) &&
                    late_supply_pool.release_page(FIRST_DATA_PAGE_ID) == StorageResult::SUCCESS,
                "A late successful supply resolves a cancelled load without leaking its frame");

    BufferPoolManager late_abort_pool(BufferPoolConfig{1, 1, 1});
    PageHandle late_abort_handle;
    TEST_ASSERT(late_abort_pool.pin_page(FIRST_DATA_PAGE_ID, 502, AccessMode::READ_ONLY, late_abort_handle) ==
                    StorageResult::PAGE_NOT_RESIDENT &&
                    late_abort_pool.release_operation_pins(502) == StorageResult::SUCCESS,
                "The only waiter can cancel before a host load failure");
    std::vector<operation_id_t> late_failed_operations{999};
    TEST_ASSERT(late_abort_pool.abort_page_load(FIRST_DATA_PAGE_ID, late_failed_operations) ==
                    StorageResult::SUCCESS &&
                    late_failed_operations.empty() &&
                    !late_abort_pool.find_frame_by_page_id(FIRST_DATA_PAGE_ID).has_value() &&
                    late_abort_pool.free_frame_count() == 1,
                "A late load failure recycles a cancelled load frame");

    BufferPoolManager pending_limit_pool(BufferPoolConfig{2, 1, 1});
    PageHandle pending_limit_handle;
    TEST_ASSERT(pending_limit_pool.pin_page(FIRST_DATA_PAGE_ID, 503, AccessMode::READ_ONLY,
                                             pending_limit_handle) == StorageResult::PAGE_NOT_RESIDENT &&
                    pending_limit_pool.pin_page(FIRST_DATA_PAGE_ID + 1, 504, AccessMode::READ_ONLY,
                                                pending_limit_handle) == StorageResult::BUSY &&
                    pending_limit_pool.loading_count() == 1,
                "The pending-load limit prevents a second concurrent page load");
    TEST_ASSERT(pending_limit_pool.abort_page_load(FIRST_DATA_PAGE_ID, late_failed_operations) ==
                    StorageResult::SUCCESS &&
                    pending_limit_pool.pin_page(FIRST_DATA_PAGE_ID + 1, 504, AccessMode::READ_ONLY,
                                                pending_limit_handle) == StorageResult::PAGE_NOT_RESIDENT,
                "A pending-load slot becomes available after the active load is aborted");
    TEST_ASSERT(pending_limit_pool.abort_page_load(FIRST_DATA_PAGE_ID + 1, late_failed_operations) ==
                    StorageResult::SUCCESS,
                "The replacement pending load can be cleaned up");

    {
        PageHandle read_handle;
        TEST_ASSERT(pool.pin_page(FIRST_DATA_PAGE_ID, 100, AccessMode::READ_ONLY, read_handle) ==
                        StorageResult::SUCCESS,
                    "A resident page can be pinned read-only");
        TEST_ASSERT(read_handle.owns_pin() && read_handle.data() != nullptr &&
                        read_handle.mutable_data() == nullptr && pool.get_pin_count(FIRST_DATA_PAGE_ID) == 1,
                    "Read-only handles expose const bytes and hold one operation-owned pin");
        PageHandle moved_handle = std::move(read_handle);
        TEST_ASSERT(!read_handle.owns_pin() && moved_handle.owns_pin(),
                    "PageHandle move transfers pin ownership");
        TEST_ASSERT(pool.unpin_page(moved_handle.pin_token(), 101) == StorageResult::INVALID_ARGUMENT &&
                        pool.get_pin_count(FIRST_DATA_PAGE_ID) == 1,
                    "Foreign operations cannot unpin another operation's page");
        TEST_ASSERT(pool.unpin_page(moved_handle.pin_token(), 100) == StorageResult::SUCCESS &&
                        pool.get_pin_count(FIRST_DATA_PAGE_ID) == 0,
                    "The owning operation can release the exact pin token");
        TEST_ASSERT(!moved_handle.owns_pin() && moved_handle.data() == nullptr &&
                        moved_handle.mutable_data() == nullptr,
                    "External unpin invalidates the handle before it can access reused bytes");
        moved_handle = PageHandle{};
        TEST_ASSERT(pool.get_pin_count(FIRST_DATA_PAGE_ID) == 0,
                    "Destroying or resetting a handle releases its pin");
        TEST_ASSERT(pool.unpin_page(moved_handle.pin_token(), 100) == StorageResult::INVALID_ARGUMENT,
                    "Releasing the same pin twice is rejected");
    }

    {
        PageHandle write_handle;
        TEST_ASSERT(pool.pin_page(FIRST_DATA_PAGE_ID, 102, AccessMode::READ_WRITE, write_handle) ==
                        StorageResult::SUCCESS,
                    "A resident page can be pinned read-write");
        TEST_ASSERT(write_handle.mutable_data() != nullptr && write_handle.data() == write_handle.mutable_data(),
                    "Read-write handles expose mutable page bytes");
        write_handle.mutable_data()[0] = 0xA5;
    }
    auto raii_dirty_descriptor = pool.get_frame_descriptor(resident_frame);
    TEST_ASSERT(raii_dirty_descriptor.has_value() && raii_dirty_descriptor->state == BufferFrameState::DIRTY &&
                    raii_dirty_descriptor->dirty_generation == 1 && pool.get_pin_count(FIRST_DATA_PAGE_ID) == 0,
                "Releasing a read-write handle marks the page dirty and drops its pin");
    flush_batch_id_t flush_batch = 0;
    uint64_t flush_generation = 0;
    TEST_ASSERT(pool.begin_page_flush(FIRST_DATA_PAGE_ID, flush_batch, flush_generation) ==
                    StorageResult::SUCCESS &&
                    pool.complete_page_flush(FIRST_DATA_PAGE_ID, flush_batch, flush_generation, true) ==
                        StorageResult::SUCCESS,
                "The RAII dirtied page can be flushed before further tests");

    {
        PageHandle write_handle;
        TEST_ASSERT(pool.pin_page(FIRST_DATA_PAGE_ID, 103, AccessMode::READ_WRITE, write_handle) ==
                        StorageResult::SUCCESS,
                    "A write pin can be created for operation cleanup testing");
        write_handle.mutable_data()[1] = 0x5A;
        TEST_ASSERT(pool.release_operation_pins(103) == StorageResult::SUCCESS,
                    "Operation cleanup releases write pins through the normal token path");
        TEST_ASSERT(!write_handle.owns_pin() && write_handle.data() == nullptr &&
                        write_handle.mutable_data() == nullptr,
                    "Operation-wide release invalidates outstanding handles");
        const auto cleanup_descriptor = pool.get_frame_descriptor(resident_frame);
        TEST_ASSERT(cleanup_descriptor.has_value() && cleanup_descriptor->state == BufferFrameState::DIRTY &&
                        cleanup_descriptor->dirty_generation == 2,
                    "Bulk operation cleanup preserves write dirtiness");
    }
    TEST_ASSERT(pool.begin_page_flush(FIRST_DATA_PAGE_ID, flush_batch, flush_generation) ==
                    StorageResult::SUCCESS &&
                    pool.complete_page_flush(FIRST_DATA_PAGE_ID, flush_batch, flush_generation, true) ==
                        StorageResult::SUCCESS,
                "The cleanup-tested page can be flushed");

    frame_id_t sequential_frame = 0;
    TEST_ASSERT(pool.load_page(FIRST_DATA_PAGE_ID + 2, sequential_frame) == StorageResult::SUCCESS,
                "A fresh page is available for generation sequencing");
    {
        PageHandle first_write;
        TEST_ASSERT(pool.pin_page(FIRST_DATA_PAGE_ID + 2, 104, AccessMode::READ_WRITE, first_write) ==
                        StorageResult::SUCCESS,
                    "The first sequential write pin succeeds");
        first_write.mutable_data()[2] = 1;
    }
    auto first_generation = pool.get_frame_descriptor(sequential_frame);
    TEST_ASSERT(first_generation.has_value() && first_generation->dirty_generation == 1,
                "The first write establishes generation one");
    {
        PageHandle second_write;
        TEST_ASSERT(pool.pin_page(FIRST_DATA_PAGE_ID + 2, 105, AccessMode::READ_WRITE, second_write) ==
                        StorageResult::SUCCESS,
                    "The second sequential write pin succeeds on a dirty page");
        second_write.mutable_data()[3] = 2;
    }
    auto second_generation = pool.get_frame_descriptor(sequential_frame);
    TEST_ASSERT(second_generation.has_value() && second_generation->state == BufferFrameState::DIRTY &&
                    second_generation->dirty_generation == 2,
                "Sequential writes advance the dirty generation monotonically");
    TEST_ASSERT(pool.mark_page_dirty(FIRST_DATA_PAGE_ID + 2) == StorageResult::SUCCESS &&
                    pool.get_frame_descriptor(sequential_frame)->dirty_generation == 3,
                "Explicit dirty marking also advances an existing generation");
    TEST_ASSERT(pool.begin_page_flush(FIRST_DATA_PAGE_ID + 2, flush_batch, flush_generation) ==
                    StorageResult::SUCCESS &&
                    pool.complete_page_flush(FIRST_DATA_PAGE_ID + 2, flush_batch, flush_generation, true) ==
                        StorageResult::SUCCESS,
                "The sequential-write page can be flushed");

    {
        PageHandle first_pin;
        PageHandle second_pin;
        TEST_ASSERT(pool.pin_page(FIRST_DATA_PAGE_ID, 200, AccessMode::READ_ONLY, first_pin) ==
                        StorageResult::SUCCESS &&
                        pool.pin_page(FIRST_DATA_PAGE_ID, 200, AccessMode::READ_ONLY, second_pin) ==
                            StorageResult::SUCCESS &&
                        pool.get_pin_count(FIRST_DATA_PAGE_ID) == 2,
                    "One operation can own multiple pins on a page");
        TEST_ASSERT(pool.release_operation_pins(200) == StorageResult::SUCCESS &&
                        pool.get_pin_count(FIRST_DATA_PAGE_ID) == 0,
                    "Operation cleanup releases every owned pin");
    }

    frame_id_t loading_frame = 0;
    TEST_ASSERT(pool.begin_page_load(FIRST_DATA_PAGE_ID + 1, loading_frame) == StorageResult::SUCCESS,
                "An ABSENT frame can enter LOADING");
    TEST_ASSERT(pool.is_page_loading(FIRST_DATA_PAGE_ID + 1) && pool.loading_count() == 1,
                "LOADING pages are visible to inspection methods");
    frame_id_t joined_frame = 0;
    TEST_ASSERT(pool.begin_page_load(FIRST_DATA_PAGE_ID + 1, joined_frame) == StorageResult::LOAD_IN_PROGRESS,
                "A duplicate load joins the existing page load");
    std::vector<operation_id_t> synchronous_woken_operations;
    TEST_ASSERT(pool.complete_page_load(FIRST_DATA_PAGE_ID + 1,
                                        std::vector<uint8_t>(DATABASE_PAGE_SIZE, 0x2A),
                                        synchronous_woken_operations) == StorageResult::SUCCESS &&
                    synchronous_woken_operations.empty(),
                "A page supply transitions LOADING to RESIDENT without discarding waiters");
    TEST_ASSERT(pool.is_page_resident(FIRST_DATA_PAGE_ID + 1) && pool.loading_count() == 0,
                "Completed loads leave no LOADING frame behind");

    const auto previous_dirty_descriptor = pool.get_frame_descriptor(resident_frame);
    TEST_ASSERT(pool.mark_page_dirty(FIRST_DATA_PAGE_ID) == StorageResult::SUCCESS,
                "A resident page can transition to DIRTY");
    auto dirty_descriptor = pool.get_frame_descriptor(resident_frame);
    TEST_ASSERT(dirty_descriptor.has_value() && dirty_descriptor->state == BufferFrameState::DIRTY &&
                    previous_dirty_descriptor.has_value() &&
                    dirty_descriptor->dirty_generation == previous_dirty_descriptor->dirty_generation + 1 &&
                    pool.dirty_count() == 1,
                "Dirty transitions advance the mutation generation");
    TEST_ASSERT(pool.begin_page_flush(FIRST_DATA_PAGE_ID, flush_batch, flush_generation) ==
                    StorageResult::SUCCESS,
                "An unpinned DIRTY page can enter FLUSHING");
    TEST_ASSERT(pool.flushing_count() == 1 && pool.dirty_count() == 0,
                "FLUSHING pages leave the DIRTY count while the snapshot is active");
    TEST_ASSERT(pool.complete_page_flush(FIRST_DATA_PAGE_ID, flush_batch, flush_generation, true) ==
                    StorageResult::SUCCESS,
                "A successful flush returns a page to RESIDENT");
    TEST_ASSERT(pool.is_page_resident(FIRST_DATA_PAGE_ID) && pool.flushing_count() == 0,
                "Successful flush completion clears FLUSHING state");

    TEST_ASSERT(pool.mark_page_dirty(FIRST_DATA_PAGE_ID + 1) == StorageResult::SUCCESS &&
                    pool.begin_page_flush(FIRST_DATA_PAGE_ID + 1, flush_batch, flush_generation) ==
                        StorageResult::SUCCESS &&
                    pool.complete_page_flush(FIRST_DATA_PAGE_ID + 1, flush_batch, flush_generation, false) ==
                        StorageResult::SUCCESS,
                "A failed flush returns the page to DIRTY");
    auto failed_descriptor = pool.get_frame_descriptor(loading_frame);
    TEST_ASSERT(failed_descriptor.has_value() && failed_descriptor->state == BufferFrameState::DIRTY &&
                    failed_descriptor->flushing_generation == 0 && failed_descriptor->flushing_batch_id == 0 &&
                    pool.dirty_count() == 1,
                "Failed flushes preserve dirty state and clear snapshot metadata");

    const flush_batch_id_t failed_batch = flush_batch;
    const uint64_t failed_generation = flush_generation;
    TEST_ASSERT(pool.begin_page_flush(FIRST_DATA_PAGE_ID + 1, flush_batch, flush_generation) ==
                    StorageResult::SUCCESS &&
                    flush_batch != failed_batch,
                "A retry receives a distinct batch identity at the same generation");
    TEST_ASSERT(pool.complete_page_flush(FIRST_DATA_PAGE_ID + 1, failed_batch, failed_generation, true) ==
                    StorageResult::INVALID_ARGUMENT &&
                    pool.flushing_count() == 1,
                "A delayed completion cannot finish a newer flush attempt");
    TEST_ASSERT(pool.complete_page_flush(FIRST_DATA_PAGE_ID + 1, flush_batch, flush_generation, false) ==
                    StorageResult::SUCCESS,
                "The active retry can still complete and preserve dirty state");

    TEST_ASSERT(pool.release_page(FIRST_DATA_PAGE_ID) == StorageResult::SUCCESS,
                "A clean unpinned page can be released");
    TEST_ASSERT(!pool.find_frame_by_page_id(FIRST_DATA_PAGE_ID).has_value() &&
                    pool.free_frame_count() == 2,
                "Released pages return their frame to the free list");
    TEST_ASSERT(pool.release_page(FIRST_DATA_PAGE_ID + 1) == StorageResult::INVALID_ARGUMENT,
                "Dirty pages cannot be released without a flush");

    BufferPoolManager clock_pool(BufferPoolConfig{2, 2, 2});
    frame_id_t first_clock_frame = 0;
    frame_id_t second_clock_frame = 0;
    TEST_ASSERT(clock_pool.load_page(FIRST_DATA_PAGE_ID, first_clock_frame) == StorageResult::SUCCESS &&
                    clock_pool.load_page(FIRST_DATA_PAGE_ID + 1, second_clock_frame) == StorageResult::SUCCESS,
                "A full pool can load its initial clean working set");
    TEST_ASSERT(clock_pool.load_page(FIRST_DATA_PAGE_ID + 2, first_clock_frame) == StorageResult::SUCCESS &&
                    !clock_pool.find_frame_by_page_id(FIRST_DATA_PAGE_ID + 1).has_value() &&
                    clock_pool.find_frame_by_page_id(FIRST_DATA_PAGE_ID + 2).value() == first_clock_frame,
                "Clock eviction reuses an unpinned clean frame without stale page mappings");

    BufferPoolManager second_chance_pool(BufferPoolConfig{2, 2, 2});
    frame_id_t second_chance_first = 0;
    frame_id_t second_chance_second = 0;
    TEST_ASSERT(second_chance_pool.load_page(FIRST_DATA_PAGE_ID, second_chance_first) == StorageResult::SUCCESS &&
                    second_chance_pool.load_page(FIRST_DATA_PAGE_ID + 1, second_chance_second) ==
                        StorageResult::SUCCESS,
                "A second pool can establish a full working set");
    {
        PageHandle first_hot_pin;
        PageHandle second_hot_pin;
        TEST_ASSERT(second_chance_pool.pin_page(FIRST_DATA_PAGE_ID, 401, AccessMode::READ_ONLY, first_hot_pin) ==
                        StorageResult::SUCCESS &&
                        second_chance_pool.pin_page(FIRST_DATA_PAGE_ID + 1, 402, AccessMode::READ_ONLY,
                                                    second_hot_pin) == StorageResult::SUCCESS,
                    "Accessing both pages sets their Clock reference bits");
    }
    TEST_ASSERT(second_chance_pool.load_page(FIRST_DATA_PAGE_ID + 2, first_clock_frame) == StorageResult::SUCCESS &&
                    second_chance_pool.find_frame_by_page_id(FIRST_DATA_PAGE_ID).has_value() &&
                    !second_chance_pool.find_frame_by_page_id(FIRST_DATA_PAGE_ID + 1).has_value() &&
                    !second_chance_pool.get_frame_descriptor(second_chance_first)->ref_bit,
                "Clock grants a second chance before evicting a recently referenced page");

    BufferPoolManager protected_pool(BufferPoolConfig{2, 2, 2});
    frame_id_t protected_first = 0;
    frame_id_t protected_second = 0;
    TEST_ASSERT(protected_pool.load_page(FIRST_DATA_PAGE_ID, protected_first) == StorageResult::SUCCESS &&
                    protected_pool.load_page(FIRST_DATA_PAGE_ID + 1, protected_second) == StorageResult::SUCCESS,
                "A protected pool can fill both frames");
    PageHandle protected_first_pin;
    PageHandle protected_second_pin;
    TEST_ASSERT(protected_pool.pin_page(FIRST_DATA_PAGE_ID, 403, AccessMode::READ_ONLY, protected_first_pin) ==
                    StorageResult::SUCCESS &&
                    protected_pool.pin_page(FIRST_DATA_PAGE_ID + 1, 404, AccessMode::READ_ONLY,
                                            protected_second_pin) == StorageResult::SUCCESS &&
                    protected_pool.load_page(FIRST_DATA_PAGE_ID + 2, protected_first) == StorageResult::BUFFER_FULL,
                "Clock never evicts pinned frames and reports a full pool deterministically");
    const frame_id_t protected_hand_before = protected_pool.get_clock_hand();
    const auto protected_first_before = protected_pool.get_frame_descriptor(protected_first);
    const auto protected_second_before = protected_pool.get_frame_descriptor(protected_second);
    TEST_ASSERT(protected_pool.load_page(FIRST_DATA_PAGE_ID + 2, protected_first) == StorageResult::BUFFER_FULL &&
                    protected_pool.get_clock_hand() == protected_hand_before &&
                    protected_pool.get_frame_descriptor(protected_first)->ref_bit ==
                        protected_first_before->ref_bit &&
                    protected_pool.get_frame_descriptor(protected_second)->ref_bit ==
                        protected_second_before->ref_bit,
                "Clock preserves replacement state when no victim can be selected");

    std::cout << "[PASSED] buffer pool frame table tests" << std::endl;
}

} // namespace webdb::test