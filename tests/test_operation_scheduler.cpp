#include "test_support.hpp"

namespace webdb::test {

void test_operation_scheduler() {
    std::cout << "[RUNNING] operation scheduler lifecycle tests..." << std::endl;

    OperationScheduler scheduler;
    constexpr std::string_view empty_plan = R"({"version":1,"reads":[]})";
    operation_id_t first_id = 0;
    TEST_ASSERT(scheduler.start_operation(empty_plan, first_id) == StorageResult::SUCCESS,
                "A valid version-one operation creates an operation");
    TEST_ASSERT(first_id != 0, "Operation IDs never use zero");
    TEST_ASSERT(scheduler.step_operation(first_id) == SchedulerStatus::COMPLETE,
                "Step 1 scheduler completes an operation without page work");
    TEST_ASSERT(scheduler.get_execution_results(first_id) == "{}",
                "Completed operations retain their result until release");
    TEST_ASSERT(scheduler.release_operation(first_id) == StorageResult::SUCCESS,
                "Completed operations can be released");
    TEST_ASSERT(scheduler.step_operation(first_id) == SchedulerStatus::ERROR,
                "Released operations cannot be stepped");

    operation_id_t cancelled_id = 0;
    TEST_ASSERT(scheduler.start_operation(empty_plan, cancelled_id) == StorageResult::SUCCESS,
                "A second operation creates successfully");
    scheduler.cancel_operation(cancelled_id);
    TEST_ASSERT(scheduler.step_operation(cancelled_id) == SchedulerStatus::CANCELLED,
                "Cancellation prevents further execution");
    TEST_ASSERT(scheduler.release_operation(cancelled_id) == StorageResult::SUCCESS,
                "Cancelled operations can be released");

    operation_id_t active_id = 0;
    TEST_ASSERT(scheduler.start_operation(empty_plan, active_id) == StorageResult::SUCCESS,
                "An active operation creates successfully");
    TEST_ASSERT(scheduler.release_operation(active_id) == StorageResult::INVALID_ARGUMENT,
                "Active operations cannot be released without cancellation");
    TEST_ASSERT(scheduler.get_pending_page_requests(active_id).empty(),
                "Step 1 operations have no page requests before the page-cache step");
    TEST_ASSERT(scheduler.get_dirty_pages_for_flush(active_id).empty(),
                "Step 1 operations have no dirty-page snapshots before the page-cache step");
    TEST_ASSERT(scheduler.provide_pages(active_id, {}) == StorageResult::INVALID_ARGUMENT,
                "Pages cannot be supplied outside a page fault");
    TEST_ASSERT(scheduler.finish_flush(active_id, true) == StorageResult::INVALID_ARGUMENT,
                "Flush completion is rejected outside flushing state");
    scheduler.cancel_operation(active_id);
    TEST_ASSERT(scheduler.release_operation(active_id) == StorageResult::SUCCESS,
                "Cancelled active operations release their memory");

    operation_id_t page_id = 0;
    TEST_ASSERT(scheduler.start_operation(empty_plan, page_id) == StorageResult::SUCCESS,
                "A page-cache test operation creates successfully");
    TEST_ASSERT(scheduler.request_page(page_id, 8, false) == StorageResult::SUCCESS,
                "Ready operations can request an absent data page");
    TEST_ASSERT(scheduler.step_operation(page_id) == SchedulerStatus::PAGE_FAULT,
                "A requested page pauses the operation at a page fault");
    const auto requests = scheduler.get_pending_page_requests(page_id);
    TEST_ASSERT(requests.size() == 1 && requests.front().page_id == 8 && !requests.front().is_write,
                "The scheduler exposes the exact pending page request");

    PageData wrong_size{8, std::vector<uint8_t>(DATABASE_PAGE_SIZE - 1, 0)};
    TEST_ASSERT(scheduler.provide_pages(page_id, {wrong_size}) == StorageResult::INVALID_ARGUMENT,
                "Wrong-sized pages cannot resume a page fault");
    PageData wrong_id{9, std::vector<uint8_t>(DATABASE_PAGE_SIZE, 0)};
    TEST_ASSERT(scheduler.provide_pages(page_id, {wrong_id}) == StorageResult::INVALID_ARGUMENT,
                "Unexpected page IDs cannot resume a page fault");
    TEST_ASSERT(scheduler.get_pending_page_requests(page_id).size() == 1,
                "Invalid supplied pages preserve the outstanding request");

    PageData supplied_page{8, std::vector<uint8_t>(DATABASE_PAGE_SIZE, 0x3C)};
    TEST_ASSERT(scheduler.provide_pages(page_id, {supplied_page}) == StorageResult::SUCCESS,
                "The requested 4 KiB page resumes the operation");
    supplied_page.bytes[0] = 0x00;
    const auto resident_copy = scheduler.copy_resident_page(page_id, 8);
    TEST_ASSERT(resident_copy.size() == DATABASE_PAGE_SIZE && resident_copy[0] == 0x3C,
                "The scheduler owns a copy of supplied page bytes");
    TEST_ASSERT(scheduler.request_page(page_id, 8, false) == StorageResult::SUCCESS,
                "Requesting an already resident page does not create another fault");
    TEST_ASSERT(scheduler.step_operation(page_id) == SchedulerStatus::COMPLETE,
                "A resumed operation can make forward progress");
    TEST_ASSERT(scheduler.release_operation(page_id) == StorageResult::SUCCESS,
                "Completed page-cache operations release successfully");

    operation_id_t fault_cancel_id = 0;
    TEST_ASSERT(scheduler.start_operation(empty_plan, fault_cancel_id) == StorageResult::SUCCESS,
                "A fault-cancellation test operation creates successfully");
    TEST_ASSERT(scheduler.request_page(fault_cancel_id, FIRST_DATA_PAGE_ID, false) == StorageResult::SUCCESS,
                "The operation enters a page fault before cancellation");
    scheduler.cancel_operation(fault_cancel_id);
    TEST_ASSERT(scheduler.provide_pages(fault_cancel_id, {supplied_page}) == StorageResult::INVALID_ARGUMENT,
                "Cancelled operations reject late page responses");
    TEST_ASSERT(scheduler.release_operation(fault_cancel_id) == StorageResult::SUCCESS,
                "Cancelled page-fault operations release successfully");

    operation_id_t invalid_plan_id = 0;
    for (const std::string_view invalid_plan : {
             std::string_view{"{"},
             std::string_view{R"({"version":2,"reads":[]})"},
             std::string_view{R"({"version":1,"reads":[2,2]})"},
             std::string_view{R"({"version":1,"reads":[2],"unknown":0})"},
             std::string_view{R"({"version":1,"reads":[2],"writes":[{"page_id":3,"byte_offset":0,"value":1}]})"},
             std::string_view{R"({"version":1,"reads":[2],"writes":[{"page_id":2,"byte_offset":4096,"value":1}]})"},
             std::string_view{R"({"version":1,"reads":[2],"writes":[{"page_id":2,"byte_offset":0,"value":256}]})"},
         }) {
        TEST_ASSERT(scheduler.start_operation(invalid_plan, invalid_plan_id) == StorageResult::INVALID_ARGUMENT,
                    "Malformed or out-of-contract test operations are rejected before registration");
    }
    const std::string oversized_plan(MAX_OPERATION_PLAN_SIZE + 1, 'x');
    TEST_ASSERT(scheduler.start_operation(oversized_plan, invalid_plan_id) == StorageResult::INVALID_ARGUMENT,
                "Plans above the configured limit are rejected");

    std::vector<operation_id_t> operation_ids;
    operation_ids.reserve(MAX_SCHEDULER_OPERATIONS);
    for (size_t index = 0; index < MAX_SCHEDULER_OPERATIONS; ++index) {
        operation_id_t operation_id = 0;
        TEST_ASSERT(scheduler.start_operation(empty_plan, operation_id) == StorageResult::SUCCESS,
                    "Operation creation succeeds up to the configured limit");
        operation_ids.push_back(operation_id);
    }
    operation_id_t overflow_id = 0;
    TEST_ASSERT(scheduler.start_operation("{}", overflow_id) == StorageResult::INVALID_ARGUMENT,
                "Operation creation rejects requests above the configured limit");
    for (const operation_id_t operation_id : operation_ids) {
        scheduler.cancel_operation(operation_id);
        TEST_ASSERT(scheduler.release_operation(operation_id) == StorageResult::SUCCESS,
                    "Cancelled limit-test operations release successfully");
    }

    operation_id_t write_operation_id = 0;
    constexpr std::string_view write_plan =
        R"({"version":1,"reads":[4,5],"writes":[{"page_id":4,"byte_offset":64,"value":42},{"page_id":4,"byte_offset":65,"value":43}]})";
    TEST_ASSERT(scheduler.start_operation(write_plan, write_operation_id) == StorageResult::SUCCESS,
                "A valid read/write test operation creates successfully");
    TEST_ASSERT(scheduler.step_operation(write_operation_id) == SchedulerStatus::PAGE_FAULT,
                "The first missing read page causes a page fault");
    TEST_ASSERT(scheduler.get_pending_page_requests(write_operation_id).front().page_id == 4 &&
                    scheduler.get_pending_page_ids(write_operation_id) == std::vector<page_id_t>{4},
                "Read pages are requested in plan order");
    TEST_ASSERT(scheduler.provide_page(write_operation_id, 4, std::vector<uint8_t>(DATABASE_PAGE_SIZE, 0)) ==
                    StorageResult::SUCCESS,
                "The scalar page wire API resumes the first page fault");
    TEST_ASSERT(scheduler.step_operation(write_operation_id) == SchedulerStatus::PAGE_FAULT,
                "The next missing read page causes a separate page fault");
    TEST_ASSERT(scheduler.get_pending_page_requests(write_operation_id).front().page_id == 5,
                "The second requested page follows the read order");
    TEST_ASSERT(scheduler.provide_pages(write_operation_id, {PageData{5, std::vector<uint8_t>(DATABASE_PAGE_SIZE, 0)}}) ==
                    StorageResult::SUCCESS,
                "Supplying the second requested page resumes the operation");
    TEST_ASSERT(scheduler.step_operation(write_operation_id) == SchedulerStatus::FLUSHING,
                "Applying writes transitions the operation to flushing");
    const auto dirty_pages = scheduler.get_dirty_pages_for_flush(write_operation_id);
    TEST_ASSERT(dirty_pages.size() == 1, "One dirty page is collected for the write operation");
    TEST_ASSERT(dirty_pages.front().page_id == 4, "The dirty snapshot identifies the written page");
    TEST_ASSERT(dirty_pages.front().bytes[64] == 42 && dirty_pages.front().bytes[65] == 43,
                "Multiple writes to one page produce one dirty snapshot with both changes");
    TEST_ASSERT(scheduler.get_dirty_page_ids(write_operation_id) == std::vector<page_id_t>{4} &&
                    scheduler.copy_dirty_page(write_operation_id, 4) == dirty_pages.front().bytes,
                "The scalar dirty-page wire API returns an independent page copy");
    auto modified_snapshot = dirty_pages;
    modified_snapshot.front().bytes[64] = 0;
    TEST_ASSERT(scheduler.get_dirty_pages_for_flush(write_operation_id).front().bytes[64] == 42,
                "Dirty-page snapshots are copied and remain stable during flushing");
    TEST_ASSERT(scheduler.finish_flush(write_operation_id, true) == StorageResult::SUCCESS,
                "Successful host flush returns the operation to ready");
    TEST_ASSERT(scheduler.step_operation(write_operation_id) == SchedulerStatus::COMPLETE,
                "The operation completes after its dirty pages are flushed");
    TEST_ASSERT(scheduler.release_operation(write_operation_id) == StorageResult::SUCCESS,
                "Completed read/write operations release successfully");

    operation_id_t reordered_operation_id = 0;
    constexpr std::string_view reordered_plan =
        R"({"writes":[{"page_id":2,"byte_offset":0,"value":1}],"reads":[2],"version":1})";
    TEST_ASSERT(scheduler.start_operation(reordered_plan, reordered_operation_id) == StorageResult::SUCCESS,
                "Valid operations accept writes before reads because JSON object order is insignificant");
    scheduler.cancel_operation(reordered_operation_id);
    TEST_ASSERT(scheduler.release_operation(reordered_operation_id) == StorageResult::SUCCESS,
                "Reordered operations release successfully");

    operation_id_t failed_flush_id = 0;
    TEST_ASSERT(scheduler.start_operation(R"({"version":1,"reads":[6],"writes":[{"page_id":6,"byte_offset":0,"value":1}]})",
                                          failed_flush_id) == StorageResult::SUCCESS,
                "A failed-flush test operation creates successfully");
    TEST_ASSERT(scheduler.step_operation(failed_flush_id) == SchedulerStatus::PAGE_FAULT,
                "The failed-flush operation requests its page");
    TEST_ASSERT(scheduler.provide_pages(failed_flush_id, {PageData{6, std::vector<uint8_t>(DATABASE_PAGE_SIZE, 0)}}) ==
                    StorageResult::SUCCESS,
                "The failed-flush operation accepts its page");
    TEST_ASSERT(scheduler.step_operation(failed_flush_id) == SchedulerStatus::FLUSHING,
                "The failed-flush operation reaches flushing");
    TEST_ASSERT(scheduler.finish_flush(failed_flush_id, false) == StorageResult::SUCCESS &&
                    scheduler.step_operation(failed_flush_id) == SchedulerStatus::ERROR,
                "A failed flush transitions the operation to error");
    TEST_ASSERT(!scheduler.get_execution_error(failed_flush_id).empty(),
                "A failed flush retains diagnostics until release");
    TEST_ASSERT(scheduler.release_operation(failed_flush_id) == StorageResult::SUCCESS,
                "Failed-flush operations release successfully");

    OperationScheduler shared_scheduler;
    operation_id_t shared_first = 0;
    operation_id_t shared_second = 0;
    TEST_ASSERT(shared_scheduler.start_operation(empty_plan, shared_first) == StorageResult::SUCCESS &&
                    shared_scheduler.start_operation(empty_plan, shared_second) == StorageResult::SUCCESS &&
                    shared_scheduler.request_page(shared_first, 20, false) == StorageResult::SUCCESS &&
                    shared_scheduler.request_page(shared_second, 20, false) == StorageResult::SUCCESS &&
                    shared_scheduler.step_operation(shared_first) == SchedulerStatus::PAGE_FAULT &&
                    shared_scheduler.step_operation(shared_second) == SchedulerStatus::PAGE_FAULT,
                "Two scheduler operations can join one pool-level page load");
    TEST_ASSERT(shared_scheduler.provide_page(shared_first, 20,
                                               std::vector<uint8_t>(DATABASE_PAGE_SIZE, 0x5E)) ==
                    StorageResult::SUCCESS &&
                    shared_scheduler.provide_page(shared_second, 20,
                                                  std::vector<uint8_t>(DATABASE_PAGE_SIZE, 0x5E)) ==
                        StorageResult::SUCCESS &&
                    shared_scheduler.step_operation(shared_first) == SchedulerStatus::COMPLETE &&
                    shared_scheduler.step_operation(shared_second) == SchedulerStatus::COMPLETE,
                "One pool supply wakes and resumes every waiting scheduler operation and duplicate supply is idempotent");
    TEST_ASSERT(shared_scheduler.release_operation(shared_first) == StorageResult::SUCCESS &&
                    shared_scheduler.release_operation(shared_second) == StorageResult::SUCCESS,
                "Shared scheduler operations release their pool pins");

    OperationScheduler cancelled_scheduler;
    operation_id_t cancelled_waiter = 0;
    operation_id_t surviving_waiter = 0;
    TEST_ASSERT(cancelled_scheduler.start_operation(empty_plan, cancelled_waiter) == StorageResult::SUCCESS &&
                    cancelled_scheduler.start_operation(empty_plan, surviving_waiter) == StorageResult::SUCCESS &&
                    cancelled_scheduler.request_page(cancelled_waiter, 21, false) == StorageResult::SUCCESS &&
                    cancelled_scheduler.request_page(surviving_waiter, 21, false) == StorageResult::SUCCESS &&
                    cancelled_scheduler.step_operation(cancelled_waiter) == SchedulerStatus::PAGE_FAULT &&
                    cancelled_scheduler.step_operation(surviving_waiter) == SchedulerStatus::PAGE_FAULT,
                "A second shared load can have a cancellable waiter");
    cancelled_scheduler.cancel_operation(cancelled_waiter);
    TEST_ASSERT(cancelled_scheduler.provide_page(surviving_waiter, 21,
                                                  std::vector<uint8_t>(DATABASE_PAGE_SIZE, 0x6F)) ==
                    StorageResult::SUCCESS &&
                    cancelled_scheduler.step_operation(surviving_waiter) == SchedulerStatus::COMPLETE &&
                    cancelled_scheduler.step_operation(cancelled_waiter) == SchedulerStatus::CANCELLED,
                "Cancelled waiters are excluded while surviving waiters resume");
    TEST_ASSERT(cancelled_scheduler.release_operation(cancelled_waiter) == StorageResult::SUCCESS &&
                    cancelled_scheduler.release_operation(surviving_waiter) == StorageResult::SUCCESS,
                "Cancelled shared-load operations release cleanly");

    OperationScheduler flush_fail_scheduler;
    operation_id_t flush_fail_id = 0;
    TEST_ASSERT(flush_fail_scheduler.start_operation(R"({"version":1,"reads":[30],"writes":[{"page_id":30,"byte_offset":0,"value":7}]})",
                                                    flush_fail_id) == StorageResult::SUCCESS &&
                    flush_fail_scheduler.step_operation(flush_fail_id) == SchedulerStatus::PAGE_FAULT &&
                    flush_fail_scheduler.provide_page(flush_fail_id, 30, std::vector<uint8_t>(DATABASE_PAGE_SIZE, 0)) == StorageResult::SUCCESS &&
                    flush_fail_scheduler.step_operation(flush_fail_id) == SchedulerStatus::FLUSHING,
                "An operation enters flushing");
    TEST_ASSERT(flush_fail_scheduler.get_dirty_page_ids(flush_fail_id).size() == 1,
                "Operation claims flush ownership");
    TEST_ASSERT(flush_fail_scheduler.fail_operation(flush_fail_id, "Flush write failed") == StorageResult::SUCCESS,
                "Failing an operation during flush clears the active batch and ownership");
    TEST_ASSERT(flush_fail_scheduler.release_operation(flush_fail_id) == StorageResult::SUCCESS,
                "Failed flush operation releases cleanly");

    OperationScheduler multi_flush_scheduler;
    operation_id_t multi_op1 = 0;
    operation_id_t multi_op2 = 0;
    TEST_ASSERT(multi_flush_scheduler.start_operation(R"({"version":1,"reads":[40],"writes":[{"page_id":40,"byte_offset":0,"value":1}]})",
                                                      multi_op1) == StorageResult::SUCCESS &&
                    multi_flush_scheduler.start_operation(R"({"version":1,"reads":[41],"writes":[{"page_id":41,"byte_offset":0,"value":2}]})",
                                                          multi_op2) == StorageResult::SUCCESS &&
                    multi_flush_scheduler.step_operation(multi_op1) == SchedulerStatus::PAGE_FAULT &&
                    multi_flush_scheduler.step_operation(multi_op2) == SchedulerStatus::PAGE_FAULT &&
                    multi_flush_scheduler.provide_page(multi_op1, 40, std::vector<uint8_t>(DATABASE_PAGE_SIZE, 0)) == StorageResult::SUCCESS &&
                    multi_flush_scheduler.provide_page(multi_op2, 41, std::vector<uint8_t>(DATABASE_PAGE_SIZE, 0)) == StorageResult::SUCCESS &&
                    multi_flush_scheduler.step_operation(multi_op1) == SchedulerStatus::FLUSHING &&
                    multi_flush_scheduler.step_operation(multi_op2) == SchedulerStatus::FLUSHING,
                "Both operations have writes applied and enter flushing");
    const auto batch_pages = multi_flush_scheduler.get_dirty_pages_for_flush(multi_op1);
    TEST_ASSERT(batch_pages.size() == 2, "Operation 1 flush batch captures both dirty pages");
    TEST_ASSERT(multi_flush_scheduler.finish_flush(multi_op1, true) == StorageResult::SUCCESS,
                "Operation 1 finishes the shared flush batch");
    TEST_ASSERT(multi_flush_scheduler.step_operation(multi_op1) == SchedulerStatus::COMPLETE &&
                    multi_flush_scheduler.step_operation(multi_op2) == SchedulerStatus::COMPLETE,
                "Both operations reach COMPLETE after their writes are flushed");
    TEST_ASSERT(multi_flush_scheduler.release_operation(multi_op1) == StorageResult::SUCCESS &&
                    multi_flush_scheduler.release_operation(multi_op2) == StorageResult::SUCCESS,
                "Both operations release cleanly");

    operation_id_t host_failure_id = 0;
    TEST_ASSERT(scheduler.start_operation(empty_plan, host_failure_id) == StorageResult::SUCCESS &&
                    scheduler.fail_operation(host_failure_id, "Host read failed") == StorageResult::SUCCESS &&
                    scheduler.step_operation(host_failure_id) == SchedulerStatus::ERROR,
                "The host can transition an active operation to an error with diagnostics");
    TEST_ASSERT(scheduler.get_execution_error(host_failure_id) == "Host read failed",
                "Host failure diagnostics remain available until release");
    TEST_ASSERT(scheduler.release_operation(host_failure_id) == StorageResult::SUCCESS,
                "Host-failed operations release successfully");


    std::cout << "[PASSED] operation scheduler lifecycle tests" << std::endl;
}

} // namespace webdb::test
