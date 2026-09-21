#pragma once

#include "common/types.hpp"
#include "storage/buffer_pool_manager.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace webdb {

using operation_id_t = uint64_t;

// Hard limits keep a hostile or malfunctioning host from growing WASM memory without bound.
inline constexpr size_t MAX_SCHEDULER_OPERATIONS = 64;
inline constexpr size_t MAX_OPERATION_PLAN_SIZE = 64 * 1024;
inline constexpr size_t MAX_PENDING_PAGE_REQUESTS = 64;
inline constexpr size_t MAX_RESIDENT_PAGES_PER_OPERATION = 256;
inline constexpr size_t MAX_DIRTY_PAGES_PER_OPERATION = 256;

enum class SchedulerStatus : uint8_t {
    READY,      // Can make progress when step_operation() is called.
    PAGE_FAULT, // Waiting for the host to supply one or more pages.
    FLUSHING,   // Waiting for the host to durably persist dirty pages.
    COMPLETE,   // Finished successfully; result remains available until release.
    CANCELLED,  // Stopped by the host; resources remain available for explicit release.
    ERROR,      // Stopped by an execution or protocol failure; diagnostics remain available.
};

struct PageRequest {
    page_id_t page_id{INVALID_PAGE_ID};
    bool is_write{false};
};

struct PageData {
    page_id_t page_id{INVALID_PAGE_ID};
    std::vector<uint8_t> bytes; // Owns a page copy rather than borrowing host-managed memory.
};

// Coordinates one or more resumable operations without performing asynchronous I/O itself.
// The JavaScript host performs I/O, then resumes an operation through this API.
class OperationScheduler {
   public:
    // Parses and validates the versioned Phase 2 test-operation format before creating an operation.
    StorageResult start_operation(std::string_view plan, operation_id_t& out_operation_id) noexcept;
    // Advances one state-machine action. Terminal operations retain their state and result.
    SchedulerStatus step_operation(operation_id_t operation_id) noexcept;
    // Step 2 native test hook. The Step 3 plan parser will create the same request internally.
    StorageResult request_page(operation_id_t operation_id, page_id_t page_id, bool is_write) noexcept;
    std::vector<PageRequest> get_pending_page_requests(operation_id_t operation_id) const noexcept;
    std::vector<page_id_t> get_pending_page_ids(operation_id_t operation_id) const noexcept;
    StorageResult provide_pages(operation_id_t operation_id, const std::vector<PageData>& pages) noexcept;
    StorageResult provide_page(operation_id_t operation_id,
                               page_id_t page_id,
                               const std::vector<uint8_t>& bytes) noexcept;
    // Returns a copy so callers cannot mutate scheduler-owned resident page memory.
    std::vector<uint8_t> copy_resident_page(operation_id_t operation_id, page_id_t page_id) const;
    std::vector<PageData> get_dirty_pages_for_flush(operation_id_t operation_id) noexcept;
    std::vector<page_id_t> get_dirty_page_ids(operation_id_t operation_id) noexcept;
    std::vector<uint8_t> copy_dirty_page(operation_id_t operation_id, page_id_t page_id) const;
    StorageResult finish_flush(operation_id_t operation_id, bool success) noexcept;
    StorageResult fail_operation(operation_id_t operation_id, std::string_view message) noexcept;
    // Cancellation is cooperative: late page or flush responses must be rejected by the host.
    void cancel_operation(operation_id_t operation_id) noexcept;
    // Only terminal operations may be released, preventing accidental loss of active work.
    StorageResult release_operation(operation_id_t operation_id) noexcept;
    std::string get_execution_results(operation_id_t operation_id) const;
    std::string get_execution_error(operation_id_t operation_id) const;

   private:
    struct Operation {
        struct Write {
            page_id_t page_id{INVALID_PAGE_ID};
            size_t byte_offset{0};
            uint8_t value{0};
        };

        SchedulerStatus status{SchedulerStatus::READY};
        std::string result; // Retained for the host until release_operation().
        std::string error;  // Retained for diagnostics until release_operation().
        std::vector<page_id_t> read_page_ids;
        std::vector<Write> writes;
        size_t next_read_index{0};
        bool writes_applied{false};
        std::unordered_map<page_id_t, PageHandle> pinned_pages;
        std::optional<PageRequest> pending_page_request;
        flush_batch_id_t active_flush_batch_id{0};
    };

    // The registry owns all operation memory. Erasing an entry is the scheduler's cleanup boundary.
    BufferPoolManager buffer_pool_{BufferPoolConfig{64, 64, 64}};
    std::unordered_map<operation_id_t, Operation> operations_;
    operation_id_t flush_owner_{0};
    // Zero is reserved as an invalid operation ID, so wraparound is treated as a creation failure.
    operation_id_t next_operation_id_{1};

    Operation* find_operation(operation_id_t operation_id) noexcept;
    const Operation* find_operation(operation_id_t operation_id) const noexcept;
    static StorageResult parse_test_operation(std::string_view plan,
                                              std::vector<page_id_t>& out_read_page_ids,
                                              std::vector<Operation::Write>& out_writes);
};

}  // namespace webdb
