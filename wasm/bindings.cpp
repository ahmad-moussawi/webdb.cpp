#include <charconv>

#include "engine.hpp"
#include "storage/operation_scheduler.hpp"

constexpr size_t kWasmPageSize = webdb::DATABASE_PAGE_SIZE;

#include <emscripten/bind.h>

/**
 * @file bindings.cpp
 * @brief Emscripten Embind interface for the WebDB SQL Engine.
 *
 * This file lives in the isolated 'wasm' directory to ensure the core
 * database engine (src/) remains completely decoupled from WebAssembly/Emscripten
 * toolchain headers.
 */

using namespace emscripten;
using namespace webdb;

namespace {

class WasmOperationScheduler {
   public:
    std::string start_operation(const std::string& plan) noexcept {
        operation_id_t operation_id = 0;
        last_start_result_ = scheduler_.start_operation(plan, operation_id);
        return last_start_result_ == StorageResult::SUCCESS ? std::to_string(operation_id) : std::string{};
    }

    StorageResult last_start_result() const noexcept { return last_start_result_; }

    uint32_t page_size() const noexcept { return static_cast<uint32_t>(kWasmPageSize); }

    SchedulerStatus step_operation(const std::string& operation_id) noexcept {
        operation_id_t parsed_id = 0;
        return parse_operation_id(operation_id, parsed_id) ? scheduler_.step_operation(parsed_id) : SchedulerStatus::ERROR;
    }

    std::vector<page_id_t> get_pending_page_ids(const std::string& operation_id) const noexcept {
        operation_id_t parsed_id = 0;
        return parse_operation_id(operation_id, parsed_id) ? scheduler_.get_pending_page_ids(parsed_id)
                                                          : std::vector<page_id_t>{};
    }

    StorageResult provide_page(const std::string& operation_id,
                               page_id_t page_id,
                               const std::vector<uint8_t>& bytes) noexcept {
        operation_id_t parsed_id = 0;
        if (!parse_operation_id(operation_id, parsed_id) || bytes.size() != kWasmPageSize) {
            return StorageResult::INVALID_ARGUMENT;
        }
        return scheduler_.provide_page(parsed_id, page_id, bytes);
    }

    std::vector<page_id_t> get_dirty_page_ids(const std::string& operation_id) noexcept {
        operation_id_t parsed_id = 0;
        return parse_operation_id(operation_id, parsed_id) ? scheduler_.get_dirty_page_ids(parsed_id)
                                                          : std::vector<page_id_t>{};
    }

    std::vector<uint8_t> copy_dirty_page(const std::string& operation_id, page_id_t page_id) const {
        operation_id_t parsed_id = 0;
        return parse_operation_id(operation_id, parsed_id) ? scheduler_.copy_dirty_page(parsed_id, page_id)
                                                          : std::vector<uint8_t>{};
    }

    StorageResult finish_flush(const std::string& operation_id, bool success) noexcept {
        operation_id_t parsed_id = 0;
        return parse_operation_id(operation_id, parsed_id) ? scheduler_.finish_flush(parsed_id, success)
                                                          : StorageResult::INVALID_ARGUMENT;
    }

    StorageResult fail_operation(const std::string& operation_id, const std::string& message) noexcept {
        operation_id_t parsed_id = 0;
        return parse_operation_id(operation_id, parsed_id) ? scheduler_.fail_operation(parsed_id, message)
                                                          : StorageResult::INVALID_ARGUMENT;
    }

    void cancel_operation(const std::string& operation_id) noexcept {
        operation_id_t parsed_id = 0;
        if (parse_operation_id(operation_id, parsed_id)) scheduler_.cancel_operation(parsed_id);
    }

    StorageResult release_operation(const std::string& operation_id) noexcept {
        operation_id_t parsed_id = 0;
        return parse_operation_id(operation_id, parsed_id) ? scheduler_.release_operation(parsed_id)
                                                          : StorageResult::INVALID_ARGUMENT;
    }

    std::string get_execution_results(const std::string& operation_id) const {
        operation_id_t parsed_id = 0;
        return parse_operation_id(operation_id, parsed_id) ? scheduler_.get_execution_results(parsed_id) : std::string{};
    }

    std::string get_execution_error(const std::string& operation_id) const {
        operation_id_t parsed_id = 0;
        return parse_operation_id(operation_id, parsed_id) ? scheduler_.get_execution_error(parsed_id) : std::string{};
    }

   private:
    static bool parse_operation_id(const std::string& input, operation_id_t& out_id) noexcept {
        if (input.empty()) return false;
        const char* begin = input.data();
        const char* end = begin + input.size();
        const auto [parsed_end, error] = std::from_chars(begin, end, out_id);
        return error == std::errc{} && parsed_end == end && out_id != 0;
    }

    OperationScheduler scheduler_;
    StorageResult last_start_result_{StorageResult::SUCCESS};
};

} // namespace

EMSCRIPTEN_BINDINGS(webdb_module) {
    enum_<StorageResult>("StorageResult")
        .value("SUCCESS", StorageResult::SUCCESS)
        .value("PAGE_FULL", StorageResult::PAGE_FULL)
        .value("TUPLE_TOO_LARGE", StorageResult::TUPLE_TOO_LARGE)
        .value("SLOT_NOT_FOUND", StorageResult::SLOT_NOT_FOUND)
        .value("CORRUPTED_PAGE", StorageResult::CORRUPTED_PAGE)
        .value("VERSION_MISMATCH", StorageResult::VERSION_MISMATCH)
        .value("SCHEMA_MISMATCH", StorageResult::SCHEMA_MISMATCH)
        .value("INVALID_ARGUMENT", StorageResult::INVALID_ARGUMENT)
        .value("CYCLE_DETECTED", StorageResult::CYCLE_DETECTED)
        .value("IO_ERROR", StorageResult::IO_ERROR)
        .value("BUFFER_FULL", StorageResult::BUFFER_FULL)
        .value("PAGE_NOT_RESIDENT", StorageResult::PAGE_NOT_RESIDENT)
        .value("LOAD_IN_PROGRESS", StorageResult::LOAD_IN_PROGRESS)
        .value("BUSY", StorageResult::BUSY)
        .value("FLUSH_REQUIRED", StorageResult::FLUSH_REQUIRED);

    enum_<SchedulerStatus>("SchedulerStatus")
        .value("READY", SchedulerStatus::READY)
        .value("PAGE_FAULT", SchedulerStatus::PAGE_FAULT)
        .value("FLUSHING", SchedulerStatus::FLUSHING)
        .value("COMPLETE", SchedulerStatus::COMPLETE)
        .value("CANCELLED", SchedulerStatus::CANCELLED)
        .value("ERROR", SchedulerStatus::ERROR);

    register_vector<uint8_t>("ByteVector");
    register_vector<page_id_t>("PageIdVector");

    class_<SqlEngine>("SqlEngine")
        .constructor<>()
        .function("executeQuery", &SqlEngine::execute_query);

    // Operation IDs cross the JavaScript boundary as decimal strings to avoid Number precision loss.
    class_<WasmOperationScheduler>("OperationScheduler")
        .constructor<>()
        .function("startOperation", &WasmOperationScheduler::start_operation)
        .function("lastStartResult", &WasmOperationScheduler::last_start_result)
        .function("pageSize", &WasmOperationScheduler::page_size)
        .function("stepOperation", &WasmOperationScheduler::step_operation)
        .function("getPendingPageIds", &WasmOperationScheduler::get_pending_page_ids)
        .function("providePage", &WasmOperationScheduler::provide_page)
        .function("getDirtyPageIds", &WasmOperationScheduler::get_dirty_page_ids)
        .function("copyDirtyPage", &WasmOperationScheduler::copy_dirty_page)
        .function("finishFlush", &WasmOperationScheduler::finish_flush)
        .function("failOperation", &WasmOperationScheduler::fail_operation)
        .function("cancelOperation", &WasmOperationScheduler::cancel_operation)
        .function("releaseOperation", &WasmOperationScheduler::release_operation)
        .function("getExecutionResults", &WasmOperationScheduler::get_execution_results)
        .function("getExecutionError", &WasmOperationScheduler::get_execution_error);
}
