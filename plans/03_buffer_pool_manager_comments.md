A thorough review of [03_buffer_pool_manager.md](plans/03_buffer_pool_manager.md) against the existing codebase ([types.hpp](src/include/common/types.hpp), [operation_scheduler.hpp](src/include/storage/operation_scheduler.hpp), [bindings.cpp](wasm/bindings.cpp), and [operation-coordinator.ts](web/src/operation-coordinator.ts)) reveals several critical implementation gaps, architectural ambiguities, and protocol mismatches that need to be addressed before coding.

---

### 1. Critical Missing APIs & Type Definitions

#### A. Missing `StorageResult::BUFFER_FULL`

- **The Issue:** The plan specifies that when no evictable frame is found, the pool returns `BUFFER_FULL` (lines 64, 172, 305). However, [types.hpp](src/include/common/types.hpp#L64-L75) defines `StorageResult` as:
  ```cpp
  enum class StorageResult : uint8_t {
      SUCCESS = 0, PAGE_FULL, TUPLE_TOO_LARGE, SLOT_NOT_FOUND,
      CORRUPTED_PAGE, VERSION_MISMATCH, SCHEMA_MISMATCH,
      INVALID_ARGUMENT, CYCLE_DETECTED, IO_ERROR
  };
  ```
- **What's Missing:** `BUFFER_FULL` is not in `StorageResult`. It must be added to [types.hpp](src/include/common/types.hpp), and exposed in [bindings.cpp](wasm/bindings.cpp) and [protocol.ts](web/src/protocol.ts) with an explicit numeric value.

#### B. Missing Load Failure / Cancellation API on `BufferPoolManager`

- **The Issue:** In the state machine and Step 4, a frame transitions `LOADING -> ABSENT` on load failure or cancellation.
- **What's Missing:** The proposed C++ API in [03_buffer_pool_manager.md](plans/03_buffer_pool_manager.md#L106-L123) has `provide_page(page_id, bytes)`, but **no method to signal a failed load**. If the host fails to fetch a page (e.g. IndexedDB error or missing page), how does the pool know to abort the load, free the frame back to `ABSENT`, and clear pending waiters?
- **Fix Required:** Add:
  ```cpp
  StorageResult abort_page_load(page_id_t page_id);
  ```

#### C. Ambiguous `pin_page` Semantics on Cache Miss

- **The Issue:** The signature is:
  ```cpp
  StorageResult pin_page(operation_id_t operation_id, page_id_t page_id, PageHandle& out_handle);
  ```
  What does `pin_page` return when the page is `ABSENT` or already `LOADING`?
  - Does it return `StorageResult::SUCCESS` with an invalid `PageHandle`?
  - Or does it return a new result code (e.g., `StorageResult::PAGE_FAULT` or `StorageResult::NOT_RESIDENT`)? (Notice `PAGE_FAULT` currently only exists in `SchedulerStatus`, not in `StorageResult`).
- **What's Missing:** The plan must explicitly define the return code, whether `out_handle` is modified, and whether `pin_page` automatically registers `operation_id` in the internal waiter list for that loading frame.

#### D. Generation Tracking in `finish_page_flush`

- **The Issue:** The plan specifies:

  > _"A successful flush clears the dirty state only if the frame still represents the same page and has not been modified since the snapshot."_

  Yet `finish_page_flush` is declared as:

  ```cpp
  StorageResult finish_page_flush(page_id_t page_id, bool success);
  ```

- **What's Missing:** If `finish_page_flush` only receives `page_id`, how does it distinguish the completed flush from a newer dirty generation?
- **Fix Required:** Either:
  1. `copy_page_for_flush` records `flushed_generation_ = current_generation_` on the frame, and `finish_page_flush(page_id, success)` checks if `current_generation_ == flushed_generation_`; OR
  2. Pass a `generation_id_t` token returned by `copy_page_for_flush` into `finish_page_flush(page_id, generation, success)`.

#### E. Missing Page Allocation / Zero-Initialization API

- **The Issue:** Phase 1 defines [IPageAccessor::allocate_page](src/include/storage/page_accessor.hpp#L29), used by [TableHeap::create](src/include/storage/table_heap.hpp#L55) to allocate brand-new zero-initialized pages.
- **What's Missing:** `BufferPoolManager` has no allocation API. If an operation needs a new data page that doesn't yet exist in storage, calling `pin_page` would trigger a host read for a non-existent page. The plan notes that free-list allocation is deferred to Phase 4, but does not clarify how a caller allocates a new zeroed page frame in memory during Phase 3.

---

### 2. State Machine & Concurrency Nuances

#### A. Contradiction in Clock Replacement Hand Sweep

- **The Issue:** Line 64 states:
  > _"The hand skips pinned, loading, flushing, and recently referenced frames. A full pass that finds no evictable frame returns `BUFFER_FULL` without changing any frame."_
- **The Problem:** In a classic second-chance Clock algorithm:
  - When encountering an unpinned frame with `ref_bit == true`, the hand **must clear `ref_bit = false`** and advance.
  - If a full pass literally "does not change any frame", then any state where all unpinned frames have `ref_bit == true` would return `BUFFER_FULL` immediately without giving any frame its second chance.
- **Clarification Needed:** The sweep should be bounded (at most $2 \times N$ steps). If all frames are pinned/loading/flushing, it can abort immediately after $N$ checks without modifying ref bits. But if unpinned frames exist with `ref_bit == true`, their bits must be cleared so a victim can be found on the second sweep.

#### B. Eviction vs. Dirty Pages Deadlock

- **The Issue:** Line 217 specifies that candidate selection skips `dirty` frames, and line 351 states that dirty pages cannot be evictable after an implicit flush in Phase 3.
- **The Consequence:** If the buffer pool has 64 frames, all 64 are modified (dirty) but unpinned, and a 65th page is requested:
  - Eviction skips dirty frames.
  - It returns `BUFFER_FULL`.
  - The system will deadlock unless there is a mechanism to trigger dirty page flushes when memory pressure occurs, or unless Phase 3 explicitly restricts tests to workloads where dirty pages are flushed prior to working-set rotation.

#### C. Direct Mutation via `PageHandle.bytes` during `FLUSHING`

- **The Issue:** `PageHandle` hands out a raw pointer: `uint8_t* bytes;`.
  Line 54 states: _"FLUSHING: a stable copy is being written by the host. The frame is pinned against overwrite..."_
  Line 163 states: _"While a page is FLUSHING, new pins may read only the stable pre-flush view... Otherwise they must wait or return a deterministic busy result."_
- **What's Missing:** If an operation held a pin _before_ flush snapshotting started, it retains a raw `uint8_t* bytes` pointer. C++ cannot prevent that pointer from writing to frame memory.
- **Fix Required:** The plan should state:
  1. Frames with active pins cannot enter `FLUSHING` (or pins must be read-only), OR
  2. `copy_page_for_flush` creates an isolated snapshot buffer (`std::vector<uint8_t>`) so host I/O is unaffected by in-place mutations, and any post-snapshot mutation must invoke `mark_page_dirty` (or be guarded by a dirty-generation increment on unpin) so `finish_page_flush` knows not to clear the dirty flag.

---

### 3. Scheduler & TypeScript Host Protocol Gaps

#### A. Host Coordinator Assertion (`pageIds.length !== 1`)

- **The Issue:** In [operation-coordinator.ts](web/src/operation-coordinator.ts#L40):
  ```typescript
  const pageIds = scheduler.getPendingPageIds(operationId);
  if (pageIds.length !== 1) {
    scheduler.failOperation(operationId, "Scheduler returned an invalid page-fault request.");
    ...
  ```
  And lines 50–53 only handle `pageIds[0]`.
- **What's Missing:** Phase 3 Step 4 and Step 7 introduce batch faults where `pageIds.length > 1`. If the C++ scheduler returns multiple pending IDs, the current TypeScript coordinator will immediately fail the operation!
- **Update Required:** [operation-coordinator.ts](web/src/operation-coordinator.ts) must be updated to:
  ```typescript
  const pageIds = scheduler.getPendingPageIds(operationId);
  if (pageIds.length === 0) { ... }
  const pages = await store.readPages(pageIds);
  for (const pageId of pageIds) {
    const page = pages.get(pageId);
    ...
    scheduler.providePage(operationId, pageId, page.slice());
  }
  ```

#### B. Multi-Operation Page Supply & Waiter Wakeup

- **The Issue:** In Phase 3, two operations requesting the same missing page join a single load.
  In WASM, [WasmOperationScheduler::providePage](wasm/bindings.cpp#L47-L55) requires an `operation_id`:
  ```cpp
  StorageResult provide_page(const std::string& operation_id, page_id_t page_id, ...);
  ```
  If Operation A and Operation B are both waiting for Page 2:
  - Host coordinator for Op A calls `providePage(opA, 2, bytes)`.
  - Page 2 becomes `RESIDENT`.
  - What happens when Host coordinator for Op B runs? Does it also call `providePage(opB, 2, bytes)`?
  - Line 144: _"a loading page cannot be supplied twice."_
- **What's Missing:** If supplying an already `RESIDENT` page returns an error, Op B will fail. The coordinator and bridge contract must specify whether `providePage` is idempotent for already resident pages, or whether the host can supply pages at the scheduler/pool level rather than per-operation.

#### C. Configurable Pool Size for WASM Tests

- **The Issue:** Step 7 requires testing small-frame pool eviction in browser/Node tests.
- **What's Missing:** In [bindings.cpp](wasm/bindings.cpp#L140), `OperationScheduler` has only a default constructor `constructor<>()` with hardcoded limits.
- **Fix Required:** Expose a constructor or method `OperationScheduler(uint32_t frameCount)` to Embind so browser tests can configure pools with 2–4 frames to deterministically verify eviction.

---

### 4. Missing Internal Data Structure Definitions

The plan currently lists public function signatures, but omits the internal descriptors. Specifying these structures in the plan will prevent design ambiguity during Step 1:

```cpp
// 1. Frame Descriptor
struct FrameDescriptor {
    frame_id_t frame_id{0};
    page_id_t page_id{INVALID_PAGE_ID};
    BufferFrameState state{BufferFrameState::ABSENT};
    uint32_t pin_count{0};
    bool ref_bit{false};
    uint64_t dirty_generation{0};
    uint64_t flushing_generation{0};
};

// 2. Pin Ownership Record (tracked inside BufferPoolManager)
// Maps (operation_id, pin_token) -> frame_id_t to enforce foreign-token rejection
// and single-operation pin release during cancellation.

// 3. Waiter Registry for Deduplication
// Maps page_id_t -> std::vector<operation_id_t> for frames in LOADING state.

// 4. Memory Layout
// std::unique_ptr<uint8_t[]> pool_storage_; (contiguous frame_count * DATABASE_PAGE_SIZE)
// to guarantee frame pointer stability across reallocations.
```

---

### Recommended Additions to [03_buffer_pool_manager.md](plans/03_buffer_pool_manager.md)

1. **Add `abort_page_load(page_id_t page_id)`** to C++ API and state transitions.
2. **Add `BUFFER_FULL`** to `StorageResult` enum in [types.hpp](src/include/common/types.hpp) and bindings.
3. **Clarify `pin_page` return contract** when a page is absent vs resident.
4. **Clarify Clock sweep loop**: Specify a 2-pass sweep ($2 \times N$ max steps) where `ref_bit` is reset on the first pass, and `BUFFER_FULL` is returned only when no evictable candidate is found after inspecting all frames.
5. **Update Step 7 tasks to explicitly include modifying [operation-coordinator.ts](web/src/operation-coordinator.ts)** to support multi-page batch faults and idempotent `providePage` calls.
6. **Expose `frame_count` configuration** in [bindings.cpp](wasm/bindings.cpp) for test harness control.

---

# Round 2 Review Comments

A secondary technical review of the updated [03_buffer_pool_manager.md](plans/03_buffer_pool_manager.md) confirms that the major structural items from Round 1 (result codes, `abort_page_load`, `new_page`, `FlushPage` with generation tracking, `FrameDescriptor`, bounded Clock sweep, and coordinator batching) were incorporated.

However, several subtle implementation details, control-flow contracts, and edge cases must be resolved before implementing Step 1:

### 1. Dirty-Pressure Eviction Control Flow in Synchronous C++

#### The Issue
Lines 81–82 and 264 state:
> *"When memory pressure finds only unpinned dirty candidates, the pool requests a flush rather than silently evicting or deadlocking. The host flushes the selected dirty batch, calls `finish_page_flush()` for the matching generations, and the replacement attempt is retried."*

#### Implementation Gap
C++ in WebDB is **synchronous and single-threaded** (no async/await, no blocking threads). `pin_page()` cannot pause internally and await host I/O. 

When `pin_page(op_id, page_id, out_handle)` runs, if all unpinned frames are dirty:
1. **What does `BufferPoolManager::pin_page` return synchronously?**
   - It cannot return `SUCCESS` because the page is not resident.
   - If it returns `BUFFER_FULL`, how does the scheduler know this is a *temporary* condition solvable by flushing dirty frames, rather than a terminal out-of-memory error?
   - Or should `pin_page` return a dedicated code (e.g. `StorageResult::FLUSH_REQUIRED`), or should `BufferPoolManager` expose `has_dirty_pressure()`?
2. **How does `OperationScheduler` step through this?**
   - In Phase 2, `OperationScheduler::step_operation` only transitions to `SchedulerStatus::FLUSHING` *after* all plan writes have executed.
   - For dirty-pressure eviction, an operation needs to pause in `SchedulerStatus::FLUSHING` *in the middle of reading/pinning* before its writes even begin.
   - Once the host completes `writePages()` and calls `finish_page_flush()`, the scheduler must resume and retry `pin_page()`.

**Recommendation:** Define the exact contract:
- When Clock finds only unpinned dirty candidates during victim selection, `BufferPoolManager::pin_page` returns a specific code (e.g. `StorageResult::BUSY` or `StorageResult::BUFFER_FULL` with an internal dirty flag).
- `OperationScheduler::step_operation` checks if dirty unpinned frames are eligible for flush, marks the operation as `SchedulerStatus::FLUSHING`, collects those dirty pages via `copy_page_for_flush()`, and yields to the host.
- When the host completes the flush, the operation transitions to `SchedulerStatus::READY` and re-attempts the pin.

---

### 2. `new_page()`: Page ID Assignment & Initial Frame State

#### The Issue
Line 137 defines:
```cpp
StorageResult new_page(operation_id_t operation_id,
                       page_id_t& out_page_id,
                       PageHandle& out_handle);
```
Line 165 states:
> *"assigns the next caller-provided page ID according to the current allocation policy"*
> *(and Open Decision 7 notes: "The final allocation source must be selected before Step 1 implementation.")*

#### Implementation Gap
1. **Input vs. Output parameter:**
   In [IPageAccessor::allocate_page](src/include/storage/page_accessor.hpp#L29), the caller provides `expected_page_id` because [TableHeap](src/include/storage/table_heap.hpp#L55) knows the next page ID from `MasterData.page_count`. 
   If `new_page` has `page_id_t& out_page_id` as an *output*, who decides what `page_id` to assign? If `BufferPoolManager` generates it, it needs an internal `next_page_id_` counter initialized at construction. If the caller decides it, the parameter should be `page_id_t expected_page_id` (input).
2. **Initial State (`RESIDENT` vs `DIRTY`):**
   When `new_page()` zero-initializes a new frame in memory:
   - Does it start in `BufferFrameState::RESIDENT` or `DIRTY`?
   - **Critical bug risk:** If a newly allocated page starts as `RESIDENT` (clean) with pin count 1, and the caller unpins it before modifying or marking it dirty, the Clock policy could evict it as clean! Because the page doesn't exist on disk yet, any future load will fail.
   - A newly created page must either start as `DIRTY` with `dirty_generation = 1`, or `new_page()` must explicitly document that it starts `DIRTY`.

---

### 3. Valid Page ID Range: `BufferPoolManager` vs. `OperationScheduler`

#### The Issue
Line 213 states:
> *"Page IDs and operation IDs use the same validation rules as Phase 2."*

In Phase 2 ([operation_scheduler.cpp](src/storage/operation_scheduler.cpp#L163)), page validation rejects anything less than `FIRST_DATA_PAGE_ID` (2):
```cpp
if (page_id < FIRST_DATA_PAGE_ID) return StorageResult::INVALID_ARGUMENT;
```

#### Implementation Gap
In Phase 4 ([PLAN.md](PLAN.md#L137-L148)), the Buffer Pool will need to manage **Master Page 0** and **Master Page 1**. If `BufferPoolManager` rejects `page_id < 2`, it will not be forward-compatible with Phase 4:
- **`BufferPoolManager`** should allow all valid non-negative page IDs: `page_id >= 0 && page_id <= MAX_DATA_PAGE_ID`.
- **`OperationScheduler`** (which parses user test-plans) is the layer that restricts user queries to data pages (`page_id >= FIRST_DATA_PAGE_ID`).

---

### 4. Waiter Wakeup API between `BufferPoolManager` and `OperationScheduler`

#### The Issue
Step 4 states:
> *"Copying host bytes into the reserved frame and waking all non-cancelled waiters."*
> *"Load abort returning frames to ABSENT and waking waiters with an error."*

#### Implementation Gap
`BufferPoolManager` tracks `operation_id_t` in a waiter registry, but `BufferPoolManager` does not have access to the `OperationScheduler::operations_` map. It cannot directly set `operation->status = READY` or `operation->status = ERROR`.

How does `OperationScheduler` find out which operations were unblocked?
- **Option A (Return woken IDs):** Update the signatures so the scheduler knows whom to wake:
  ```cpp
  StorageResult provide_page(page_id_t page_id,
                             const std::vector<uint8_t>& bytes,
                             std::vector<operation_id_t>& out_woken_operations);
  StorageResult abort_page_load(page_id_t page_id,
                                std::vector<operation_id_t>& out_failed_operations);
  ```
- **Option B (Query on step):** Or when `step_operation(op_id)` runs, if the operation is in `PAGE_FAULT`, it asks the buffer pool: `is_page_resident(page_id)` or re-attempts `pin_page()`.
- Option A is much cleaner because `abort_page_load()` can immediately transition all waiting operations to `SchedulerStatus::ERROR`.

---

### 5. Host Coordinator Concurrency & Idempotent Page Supply

#### The Issue
Line 197 states:
> *"The host supplies each page once through the buffer-pool/scheduler shared-load adapter... a second operation must not call the operation-owned Phase 2 `providePage()` API with the same page."*

#### Implementation Gap
In [operation-coordinator.ts](web/src/operation-coordinator.ts), each operation is run independently via `runOperation(scheduler, store, plan)`.
If Operation 1 and Operation 2 run concurrently in JavaScript and both fault on Page 2:
1. Both operations see `SchedulerStatus::PageFault` for Page 2.
2. Both initiate `await store.readPages([2])`.
3. Op 1 completes first and calls `providePage(op1, 2, bytes)`. Page 2 is now `RESIDENT`.
4. Op 2 completes and calls `providePage(op2, 2, bytes)`.
5. If line 188 ("a loading page cannot be supplied twice") causes Op 2's call to return `INVALID_ARGUMENT`, Op 2 will crash!

**Recommendation:**
In the WASM adapter/bridge, `provide_page` should be **idempotent**:
If Page 2 is already `RESIDENT` with matching bytes, supplying it again for another waiting operation should return `StorageResult::SUCCESS` and wake that operation, rather than returning an error.

---

### 6. Explicit Values for New `StorageResult` Enum Members

Line 49 notes that numeric values should be appended explicitly. To guarantee complete consistency across [types.hpp](src/include/common/types.hpp), [bindings.cpp](wasm/bindings.cpp), and [protocol.ts](web/src/protocol.ts), define the exact enum values in the plan:

```cpp
enum class StorageResult : uint8_t {
    SUCCESS = 0,
    PAGE_FULL = 1,
    TUPLE_TOO_LARGE = 2,
    SLOT_NOT_FOUND = 3,
    CORRUPTED_PAGE = 4,
    VERSION_MISMATCH = 5,
    SCHEMA_MISMATCH = 6,
    INVALID_ARGUMENT = 7,
    CYCLE_DETECTED = 8,
    IO_ERROR = 9,
    // Phase 3 additions:
    BUFFER_FULL = 10,
    PAGE_NOT_RESIDENT = 11,
    LOAD_IN_PROGRESS = 12,
    BUSY = 13,
};
```

---

### 7. Allocation Precedence: Free List (`ABSENT` frames) vs. Clock Eviction

The plan details the Clock policy for replacing `RESIDENT` pages, but should clarify how initial/empty frames are consumed:
- `BufferPoolManager` should maintain a free list (or simple vector of `frame_id_t` in `ABSENT` state).
- When a page is requested or allocated via `pin_page()` or `new_page()`:
  1. Check if the page is already `RESIDENT` or `LOADING`.
  2. If absent, take an `ABSENT` frame from the free list.
  3. Only if the free list is empty does the manager invoke the Clock replacement algorithm to evict a clean `RESIDENT` frame.
  4. If Clock finds only unpinned `DIRTY` frames, trigger the dirty-pressure flush.
  5. If no candidate exists after $2 \times \text{frame\_count}$ inspections, return `BUFFER_FULL`.

---

### Summary Checklist for Implementation Readiness

| Topic | Current Plan Status | Final Clarification Needed |
| :--- | :--- | :--- |
| **Dirty-pressure flush** | Concept described | Define synchronous return code and how `OperationScheduler` steps through it |
| **`new_page()`** | Signature has output ID; text says caller provides ID | Clarify if `page_id` is input or generated; ensure frame starts as `DIRTY` |
| **Page ID Range** | Says "same validation rules as Phase 2" | Allow `page_id >= 0` in buffer pool (reserving 0/1 for Master Pages) |
| **Waiter Wakeup** | "Wakes all waiters" | Return list of unblocked `operation_id_t` from `provide_page`/`abort_page_load` |
| **Duplicate Supply** | "Cannot be supplied twice" | Make supply idempotent at the adapter level so concurrent JS operations don't fail |
| **Enum Constants** | Explicit numbers recommended | Explicitly assign `10, 11, 12, 13` to prevent binding drifts |

---

# Round 3 Review Comments

The Phase 3 plan in [03_buffer_pool_manager.md](plans/03_buffer_pool_manager.md) is now in excellent shape. All architectural and foundational questions from Rounds 1 and 2 have been incorporated cleanly:
- `FLUSH_REQUIRED = 14` is explicitly defined in `StorageResult`.
- `new_page()` cleanly accepts `expected_page_id` from the caller and initializes the frame as `DIRTY` with `dirty_generation = 1`.
- `provide_page()` and `abort_page_load()` output `operation_id_t` vectors so the scheduler can wake or fail waiters without the buffer pool needing access to scheduler internals.
- Buffer pool page validation allows `page_id >= 0` (forward-compatible with Master Pages 0 and 1 in Phase 4).
- Free-frame list (`ABSENT` frames) is consumed prior to invoking Clock eviction.
- The dirty-pressure flush retry loop is concretely integrated into `OperationScheduler` via `FLUSH_REQUIRED` and state `FLUSHING`.

Before proceeding to Step 1 implementation, there are 4 final interface and test details to confirm so that C++, WASM bindings, and TypeScript coordinators align seamlessly:

### 1. In-Flight Read Deduplication at the Host Layer (`operation-coordinator.ts`)

#### The Situation
Line 201 states:
> *"In particular, a loading page cannot be supplied twice, page supply is pool-level rather than operation-level..."*
> *(and Acceptance Criterion 9: "The coordinator handles multi-page faults and shared waiter wakeups without duplicate page supply.")*

#### Implementation Detail
In [operation-coordinator.ts](web/src/operation-coordinator.ts), each operation is run independently via `runOperation(scheduler, store, plan)`.
If Operation 1 and Operation 2 run concurrently in the browser and both request Page 2:
1. Both operations step and yield `PAGE_FAULT` for Page 2.
2. If both operations independently call `await store.readPages([2])` and then attempt to call `scheduler.providePage(2, bytes)`:
   - The first `providePage(2, bytes)` succeeds and transitions Page 2 to `RESIDENT`.
   - The second `providePage(2, bytes)` would fail if the buffer pool strictly rejects duplicate supplies for non-`LOADING` pages.

#### Recommendation
Handle this at two levels:
1. **In `operation-coordinator.ts`:** Maintain a shared in-flight load map:
   ```typescript
   const inFlightReads = new Map<number, Promise<Uint8Array | undefined>>();
   ```
   If a page is already being fetched by another concurrent operation, the second operation awaits the same promise rather than issuing a duplicate IndexedDB read and duplicate `providePage()` call.
2. **In WASM Bridge:** Ensure that if `providePage(pageId, bytes)` is called for an already `RESIDENT` page with matching bytes, it returns `StorageResult.Success` idempotently as a defensive safeguard.

---

### 2. Concrete TypeScript `SchedulerBridge` Interface for Phase 3

Because Phase 3 removes operation-level page supply (Open Decision 5), the host interface in [protocol.ts](web/src/protocol.ts) and [wasm-scheduler-bridge.ts](web/src/wasm-scheduler-bridge.ts) changes from the Phase 2 signature:

```typescript
export interface FlushPageSnapshot {
  readonly pageId: number;
  readonly generation: bigint;
  readonly bytes: Uint8Array;
}

export interface SchedulerBridge {
  startOperation(plan: string): string;
  lastStartResult(): StorageResult;
  stepOperation(operationId: string): SchedulerStatus;
  
  // Page fault handling (Operation reports what it needs, pool accepts bytes)
  getPendingPageIds(operationId: string): readonly number[];
  providePage(pageId: number, bytes: Uint8Array): StorageResult;
  abortPageLoad(pageId: number): StorageResult;

  // Flush handling (Pool-level dirty pages with generation tokens)
  getDirtyPageIds(): readonly number[];
  copyDirtyPage(pageId: number): FlushPageSnapshot;
  finishPageFlush(pageId: number, flushingGeneration: bigint, success: boolean): StorageResult;

  // Diagnostics & Operation lifecycle
  cancelOperation(operationId: string): void;
  releaseOperation(operationId: string): StorageResult;
  getExecutionResults(operationId: string): string;
  getExecutionError(operationId: string): string;
}
```

---

### 3. Inspection Method Signatures for Native Unit Tests

Line 182 requires test inspection methods on `BufferPoolManager`. Specifying the exact signatures avoids ad-hoc naming during Step 1:

```cpp
// src/include/storage/buffer_pool_manager.hpp
size_t frame_count() const noexcept;
size_t resident_count() const noexcept;
size_t dirty_count() const noexcept;
size_t free_frame_count() const noexcept;

frame_id_t get_clock_hand() const noexcept;
std::optional<frame_id_t> find_frame_by_page_id(page_id_t page_id) const noexcept;
std::optional<FrameDescriptor> get_frame_descriptor(frame_id_t frame_id) const noexcept;
bool is_page_resident(page_id_t page_id) const noexcept;
bool is_page_loading(page_id_t page_id) const noexcept;
uint32_t get_pin_count(page_id_t page_id) const noexcept;
```

---

### 4. Migration of Existing Phase 2 Unit Tests (`test_operation_scheduler.cpp`)

In [test_operation_scheduler.cpp](tests/test_operation_scheduler.cpp), existing tests directly invoke:
- `scheduler.provide_pages(page_id, ...)`
- `scheduler.copy_resident_page(page_id, ...)`
- `scheduler.get_dirty_pages_for_flush(page_id)`
- `scheduler.finish_flush(page_id, ...)`

Since Phase 3 replaces these with pool-level methods (`provide_page(page_id, bytes, ...)`, `finish_page_flush(...)`), **Step 6 should explicitly note that [test_operation_scheduler.cpp](tests/test_operation_scheduler.cpp) will be migrated to the new pool-level APIs**.

---

### Verdict

The plan is **fully specified, technically sound, and ready for Step 1 implementation**.

---

# Round 4 Review Comments

The additions in [03_buffer_pool_manager.md](plans/03_buffer_pool_manager.md) make the plan exceptionally robust and production-ready:
- `FlushBatch` and `flush_batch_id_t` provide deterministic batch boundaries and reject cross-batch or duplicate completions.
- The C++ native inspection signatures (`frame_count`, `resident_count`, `dirty_count`, `loading_count`, `flushing_count`, `free_frame_count`, etc.) are fully specified.
- The `SchedulerBridge` TypeScript interface cleanly models pool-level page supply, aborts, and 64-bit generation snapshots.
- The Shared Host Coordinator architecture explicitly specifies in-flight read deduplication (`Map<number, Promise<Uint8Array | undefined>>`) at the host layer.
- Migration of `tests/test_operation_scheduler.cpp` to the new pool-level APIs is clearly scheduled in Step 6.

There are 2 fine-grained concurrency nuances to keep in mind during implementation:

### 1. Protect Freshly Loaded Frames from Immediate Eviction

#### The Scenario
1. Operation 1 faults on absent Page 2. A frame is allocated in `LOADING` state, and Op 1 is recorded as a waiter.
2. The host reads Page 2 and calls `provide_page(2, bytes, woken_ops)`. Op 1 is added to `woken_ops`, and the frame transitions to `RESIDENT`.
3. Before the scheduler steps Op 1, suppose another concurrent operation (Op 2) steps and requests missing Page 99 in a pool with 0 free frames.
4. If the freshly loaded Page 2 has `pin_count == 0`, Clock candidate selection could sweep past it, clear its second-chance bit, and evict Page 2 *before Op 1 ever gets stepped to claim its pin*!

#### Implementation Rule
To prevent this race:
- While an operation is in the waiter list for a loading frame, that frame must retain an effective pin count (or `pin_count` is incremented upon registering the waiter).
- When `provide_page()` transitions the frame from `LOADING -> RESIDENT`, the frame remains pinned (`pin_count = woken_operations.size()`).
- When Op 1 resumes and calls `pin_page()`, it receives its `pin_token` for the already-accounted pin rather than competing for a zero-pinned frame.

---

### 2. Single Active `FlushBatch` Invariant

#### The Scenario
Line 277 specifies that `getDirtyPageIds()` and `copyDirtyPage()` describe the active global flush batch, and line 281 requires all pins to be released before entering `FLUSHING`.

#### Implementation Rule
Clarify in the code that **at most one `FlushBatch` is in flight at any time**:
- When a flush batch is collected (`collect_dirty_pages_for_flush()`), the buffer pool enters an active flushing state with that `batch_id`.
- Any subsequent operation that attempts to trigger a flush before the current batch completes returns `BUSY`.
- The active batch is cleared once every page in the batch has received its `finish_page_flush(batch_id, ...)`.

---

### Ready for Execution

The plan is complete, rigorous, and ready to be executed starting with **Step 1: Frame table and lifecycle foundation**.

---

# Implementation Issues

This section documents code-level issues, bugs, and edge cases discovered during step-by-step implementation reviews of the buffer pool manager.

## Step 2 Implementation Findings (Pin Tokens & Operation Ownership)

During the review of Step 2 (`src/include/storage/buffer_pool_manager.hpp`, `src/storage/buffer_pool_manager.cpp`, and `tests/test_buffer_pool_manager.cpp`), the following critical bugs and API gaps were identified:

### 1. Critical: `release_operation_pins()` Drops Writes Without Marking Frames `DIRTY`

**Feedback: Valid. Fixed.** The original implementation bypassed `release_pin_token()`, so bulk operation cleanup could drop `READ_WRITE` mutations without marking the frame dirty. `release_operation_pins()` now delegates every token release through the shared dirty-aware path.
- **Location:** `src/storage/buffer_pool_manager.cpp` (in `release_operation_pins()`)
- **Bug:** When iterating over an operation's tokens, `release_operation_pins()` decrements descriptor `pin_count` and erases from `pins_`, but never checks `access_mode == AccessMode::READ_WRITE`. It fails to set `descriptor.state = BufferFrameState::DIRTY` and fails to increment `dirty_generation`.
- **Consequence:** If an operation pins a page with `READ_WRITE`, modifies page bytes, and the scheduler releases operation pins (e.g., on operation completion or abort), the frame remains clean (`RESIDENT`). When the `PageHandle` is subsequently destroyed, its token is already gone from `pins_`, so the write is **silently and permanently dropped**.
- **Fix:** Delegate all token releases directly through `release_pin_token(token, operation_id)`:
  ```cpp
  StorageResult BufferPoolManager::release_operation_pins(operation_id_t operation_id) noexcept {
      const auto operation_it = operation_pins_.find(operation_id);
      if (operation_it == operation_pins_.end()) {
          return StorageResult::SUCCESS;
      }

      const std::vector<pin_token_t> tokens = operation_it->second;
      for (const pin_token_t token : tokens) {
          release_pin_token(token, operation_id);
      }
      return StorageResult::SUCCESS;
  }
  ```

### 2. Critical: `release_pin_token()` Fails to Increment `dirty_generation` if Frame Is Already `DIRTY`

**Feedback: Valid. Fixed.** A write release must advance the mutation generation regardless of whether the page was clean or already dirty. The implementation now changes `RESIDENT` to `DIRTY` when needed and increments the generation for every `READ_WRITE` release.
- **Location:** `src/storage/buffer_pool_manager.cpp` (in `release_pin_token()`)
- **Bug:** The dirty transition logic is guarded by:
  ```cpp
  if (record.access_mode == AccessMode::READ_WRITE && descriptor.state == BufferFrameState::RESIDENT) {
      descriptor.state = BufferFrameState::DIRTY;
      ++descriptor.dirty_generation;
      if (descriptor.dirty_generation == 0) ++descriptor.dirty_generation;
  }
  ```
- **Consequence:** If a page is already in `DIRTY` state (from a prior write or concurrent operation), releasing a subsequent `READ_WRITE` pin will evaluate `descriptor.state == RESIDENT` as `false`. The mutation generation is **not incremented**. Consecutive writes will all share `dirty_generation == 1`. When optimistic flushing occurs, a background flush completing for generation 1 would reset `flushing_generation = 0` and transition the page to `RESIDENT`, overwriting and discarding subsequent in-memory mutations.
- **Fix:** Set state to `DIRTY` if currently `RESIDENT`, but **always** advance `dirty_generation` for `READ_WRITE` releases:
  ```cpp
  if (record.access_mode == AccessMode::READ_WRITE) {
      if (descriptor.state == BufferFrameState::RESIDENT) {
          descriptor.state = BufferFrameState::DIRTY;
      }
      ++descriptor.dirty_generation;
      if (descriptor.dirty_generation == 0) ++descriptor.dirty_generation;
  }
  ```

### 3. Critical: `mark_page_dirty()` Rejects Already-Dirty Pages and Clobbers Generation to 1

**Feedback: Valid. Fixed.** Repeated dirty marking is a legitimate operation, and resetting the generation to `1` would invalidate flush-generation ordering. The method now accepts `RESIDENT` and `DIRTY` pages and increments the generation without allowing zero after wraparound.
- **Location:** `src/storage/buffer_pool_manager.cpp` (in `mark_page_dirty(page_id_t page_id)`)
- **Bug:**
  1. It returns `StorageResult::INVALID_ARGUMENT` if `state != BufferFrameState::RESIDENT`. It is impossible to mark an already-dirty page dirty again after subsequent mutations.
  2. It hardcodes `descriptor.dirty_generation = 1;` instead of incrementing it monotonically (`++dirty_generation`). If generation was 5, marking it dirty resets it to 1.
- **Fix:** Allow both `RESIDENT` and `DIRTY` states, and increment `dirty_generation`:
  ```cpp
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
  ```

### 4. API Alignment: Token-Based `unpin_page()` and Foreign-Token Rejection

**Feedback: Valid. Fixed.** The page-based overload could release the wrong pin when one operation owned multiple pins on the same page. The public API now accepts the exact `pin_token_t` and operation ID, rejects foreign or unknown tokens, and optionally validates the dirty flag against read-only access.
- **Context:** The Phase 3 design (lines 160–164 & 525) specifies:
  - `StorageResult unpin_page(operation_id_t operation_id, pin_token_t pin_token, bool is_dirty);`
  - `StorageResult mark_page_dirty(operation_id_t operation_id, pin_token_t pin_token);`
  - Checklist requirement: *"Implement unpin_page() with underflow and foreign-token rejection"*.
- **Gap:** In the current implementation, `release_pin_token(pin_token, operation_id)` is private. The only public unpin method is `unpin_page(page_id_t page_id, operation_id_t operation_id)`.
- **Problems:**
  1. If an operation owns multiple pins on the same page (e.g. nested calls or multiple cursors), `unpin_page(page_id, op_id)` pops the first token in `operation_pins_`, which may not match the caller's specific handle/token.
  2. Foreign-token rejection cannot be tested with token identifiers directly.
- **Resolution:** Expose `unpin_page(pin_token_t pin_token, operation_id_t operation_id)` (or with `bool is_dirty = false`) and `mark_page_dirty(pin_token_t pin_token, operation_id_t operation_id)` as public methods.

### 5. RAII Usability: `PageHandle::reset()` Visibility and Operation Context

**Feedback: Valid. Fixed.** Public `reset()` is the expected RAII interface, and passing operation ID `0` weakened ownership verification. `PageHandle` now stores its owning operation ID, validates it during reset, and fully clears moved-from handle metadata.
- **Issue:** `PageHandle::reset()` is private. To release a handle early, callers had to assign a default-constructed temporary (`moved_handle = PageHandle{}`).
- **Resolution:**
  - Make `void reset() noexcept;` public, following standard C++ RAII idioms (`std::unique_ptr::reset()`).
  - Store `operation_id_` inside `PageHandle` so `reset()` calls `manager_->release_pin_token(pin_token_, operation_id_)` instead of passing 0, ensuring operation-ownership verification on RAII release.
  - In `PageHandle` move-assignment, ensure `other.page_id_ = INVALID_PAGE_ID`, `other.frame_id_ = 0`, and `other.access_mode_ = AccessMode::READ_ONLY` are reset alongside `manager_`, `bytes_`, and `pin_token_`.

### 6. Missing Unit Test Scenarios

**Feedback: Valid. Added.** Tests now cover write-pin cleanup through `release_operation_pins()`, monotonic generations across sequential writes and repeated dirty marking, exact-token ownership, foreign-token rejection, and double-release behavior.
The following test cases must be added to `tests/test_buffer_pool_manager.cpp` to validate the fixes:
1. **Unwinding `READ_WRITE` pins via `release_operation_pins`:** Verify that an operation holding a `READ_WRITE` handle has its page marked `DIRTY` with incremented `dirty_generation` when `release_operation_pins(op_id)` is called.
2. **Monotonic generation increment on subsequent writes:** Verify that two sequential `READ_WRITE` pins on the same page advance `dirty_generation` from $1 \to 2$.
3. **Explicit foreign-token rejection:** Verify that passing Operation B with Operation A's valid `pin_token` returns `StorageResult::INVALID_ARGUMENT`.

### Follow-up Review: Stale `PageHandle` After External Release

**Feedback: Valid. Fixed.** External `unpin_page()` and `release_operation_pins()` can remove a token without the original RAII object being destroyed. The manager now records the owning `PageHandle*` in each `PinRecord` and invalidates that handle whenever the token is released externally. Invalidated handles report `owns_pin() == false`, and both `data()` and `mutable_data()` return `nullptr`, preventing access through the handle after cancellation or unpinning. Move construction and move assignment update the registered handle pointer so invalidation targets the current owner.

As with any C++ raw pointer API, a caller must not retain a pointer previously returned by `data()` or `mutable_data()` after the handle is released. The handle invalidation prevents access through the handle object; it cannot revoke a raw pointer that the caller copied elsewhere.

### Follow-up Review: Cache Miss Must Reserve a Loading Frame

**Feedback: Valid. Fixed.** The original `pin_page()` miss branch returned `PAGE_NOT_RESIDENT` immediately without assigning a frame or recording the requesting operation. That contradicted the documented contract that a miss starts a load. Every retry could therefore return the same miss while no frame was owned by the buffer pool for the host to complete.

The corrected flow is:

1. Validate the page and operation IDs.
2. Reserve an `ABSENT` frame and transition it to `LOADING`.
3. Register the requesting operation in the page's load-waiter list.
4. Leave the caller's `PageHandle` unchanged and return `PAGE_NOT_RESIDENT` so the scheduler issues the host read.
5. If another operation requests the same page, register it in the existing waiter list and return `LOAD_IN_PROGRESS`; no duplicate frame or host load is created.
6. Roll back the `LOADING` frame and waiter registration if allocation fails while registering the first waiter.

Tests now verify that the first miss reserves a frame, preserves the output handle, a second operation joins the load, and the completed load can release its frame back to the free list.

### Follow-up Review: Allocation During `noexcept` Pin Cleanup

**Feedback: Valid. Fixed.** `release_operation_pins()` is declared `noexcept`, but copying the operation's token vector can allocate:

```cpp
const std::vector<pin_token_t> tokens = operation_it->second;
```

If that allocation throws `std::bad_alloc`, the `noexcept` function terminates the process instead of completing cancellation cleanup. The implementation now moves the vector:

```cpp
std::vector<pin_token_t> tokens = std::move(operation_it->second);
operation_pins_.erase(operation_it);
```

Moving transfers the vector's existing allocation instead of duplicating all token entries, so it is constant-time and non-allocating for the normal `std::vector` move path. The reverse operation index is erased before token release, allowing `release_pin_token()` to perform frame dirtying, handle invalidation, and token removal without rebuilding or modifying that operation's token vector. This preserves the `noexcept` cleanup guarantee while keeping write mutations and pin ownership state intact.

---

## Step 3 Implementation Validation (Clock Eviction) & Step 4 Follow-up Issues

During the revalidation of the latest implementation on branch `phase3_clock_eviction` (`src/include/storage/buffer_pool_manager.hpp`, `src/storage/buffer_pool_manager.cpp`, and `tests/test_buffer_pool_manager.cpp`), Clock replacement has been implemented and verified.

### 1. Step 3: Clock Replacement & Clean Eviction — Validated & Passing
- **Implementation Verification:**
  - `select_clean_victim(out_frame_id)` correctly iterates up to `2 * frame_count` inspections starting at `clock_hand_`.
  - Safely protects `pin_count > 0`, `DIRTY`, `FLUSHING`, and `LOADING` frames from eviction.
  - Implements second chance by clearing `ref_bit = false` and advancing past recently accessed pages.
  - Advance cursor: `clock_hand_` advances forward modulo `frame_count`, so subsequent sweeps resume immediately following the selected victim.
  - Frame reassignment in `assign_frame()` cleanly updates `page_to_frame_` by erasing the evicted page ID and mapping the new page ID in an exception-safe manner.
  - Comprehensive unit tests verify full working set fill, eviction of clean frames, second-chance retention of referenced pages, and deterministic `BUFFER_FULL` when all frames are pinned.
- **Status:** **Complete and verified clean.**

---

### Step 4 Follow-up Review (Async Page Loading & Cancellation Edge Cases)

The following critical issues in Step 4 async loading remain open and should be addressed before Step 5:

### 2. Critical: Permanent Frame Leak in `provide_page()` When All Waiters Have Cancelled
- **Location:** `src/storage/buffer_pool_manager.cpp` (in `provide_page()`)
- **Bug:**
  ```cpp
  const auto waiter_it = loading_waiters_.find(page_id);
  if (waiter_it == loading_waiters_.end()) {
      return StorageResult::INVALID_ARGUMENT;
  }
  ```
- **Scenario:**
  1. Operation 1 faults on Page 5. Page 5 enters `LOADING` with waiter `{1}`.
  2. Operation 1 is cancelled or timed out before the host read finishes. `release_operation_pins(1)` invokes `unregister_operation_load_waiters(1)`, which removes Operation 1.
  3. Because Operation 1 was the only waiter, `loading_waiters_.erase(waiter_it)` deletes the entry for Page 5 entirely.
  4. The host coordinator's asynchronous read finishes and delivers page bytes via `provide_page(5, bytes, woken_ops)`.
  5. `provide_page()` looks for `loading_waiters_.find(5)` and finds nothing, so it returns `StorageResult::INVALID_ARGUMENT`.
- **Consequence:** The frame remains stuck in `BufferFrameState::LOADING` **forever**. It is never transitioned to `RESIDENT`, never aborted, never freed back to `free_frames_`, and cannot be evicted. Every cancelled load permanently leaks a buffer pool frame until the pool runs out of frames.
- **Fix:** If `waiter_it == loading_waiters_.end()`, do not fail with `INVALID_ARGUMENT`. Instead, copy the bytes into the frame, set `ref_bit = true`, transition the frame to `BufferFrameState::RESIDENT`, and return `out_woken_operations` as empty (`out_woken_operations.clear()`). This preserves the cached page for future queries and prevents frame leakage:
  ```cpp
  std::vector<operation_id_t> woken_operations;
  if (waiter_it != loading_waiters_.end()) {
      woken_operations = std::move(waiter_it->second);
      loading_waiters_.erase(waiter_it);
  }
  std::copy(bytes.begin(), bytes.end(), get_frame_bytes(frame->descriptor.frame_id));
  out_woken_operations = std::move(woken_operations);
  frame->descriptor.ref_bit = true;
  frame->descriptor.state = BufferFrameState::RESIDENT;
  return StorageResult::SUCCESS;
  ```

### 3. Critical: Permanent Frame Leak in `abort_page_load()` When All Waiters Have Cancelled
- **Location:** `src/storage/buffer_pool_manager.cpp` (in `abort_page_load()`)
- **Bug:**
  ```cpp
  const auto waiter_it = loading_waiters_.find(page_id);
  if (waiter_it == loading_waiters_.end()) {
      return StorageResult::INVALID_ARGUMENT;
  }
  ```
- **Consequence:** If an operation cancels while waiting for a load, and the host subsequently reports a read failure (`abort_page_load()`), the missing waiter entry causes `abort_page_load()` to return `StorageResult::INVALID_ARGUMENT` without recycling the frame. The frame is permanently orphaned in `LOADING` state.
- **Fix:** Allow `abort_page_load()` to proceed when `waiter_it == loading_waiters_.end()`, returning an empty `out_failed_operations` vector while still cleaning up `page_to_frame_`, resetting the descriptor, and returning the frame to `free_frames_`:
  ```cpp
  std::vector<operation_id_t> failed_operations;
  if (waiter_it != loading_waiters_.end()) {
      failed_operations = std::move(waiter_it->second);
      loading_waiters_.erase(waiter_it);
  }
  out_failed_operations = std::move(failed_operations);
  page_to_frame_.erase(page_id);
  descriptor = FrameDescriptor{};
  descriptor.frame_id = *frame_id;
  free_frames_.push_back(*frame_id);
  return StorageResult::SUCCESS;
  ```

### 4. Bug: Retrying `pin_page()` on a Loading Page Fails With `INVALID_ARGUMENT`
- **Location:** `src/storage/buffer_pool_manager.cpp` (in `register_load_waiter()`)
- **Bug:**
  ```cpp
  auto& waiters = loading_waiters_[page_id];
  if (std::find(waiters.begin(), waiters.end(), operation_id) != waiters.end()) {
      return StorageResult::INVALID_ARGUMENT;
  }
  waiters.push_back(operation_id);
  return StorageResult::SUCCESS;
  ```
- **Consequence:** When an operation polls or re-executes `pin_page(page_id, operation_id)` while the page is still in `LOADING` state, `register_load_waiter` treats the already-registered operation as an error and returns `INVALID_ARGUMENT`. The retry fails rather than reporting `LOAD_IN_PROGRESS`.
- **Fix:** Make `register_load_waiter` idempotent. If `operation_id` is already in `waiters`, return `StorageResult::SUCCESS`:
  ```cpp
  auto& waiters = loading_waiters_[page_id];
  if (std::find(waiters.begin(), waiters.end(), operation_id) == waiters.end()) {
      waiters.push_back(operation_id);
  }
  return StorageResult::SUCCESS;
  ```

### 5. Specification Gap: `config_.max_pending_loads` Is Never Enforced
- **Location:** `src/storage/buffer_pool_manager.cpp` (in `begin_page_load()` and `pin_page()`)
- **Bug:** `BufferPoolConfig::max_pending_loads` is validated in the constructor, but never enforced when allocating a `LOADING` frame.
- **Consequence:** An operation burst with multiple page faults can transition every single pool frame to `LOADING`, completely bypassing the configured asynchronous I/O concurrency limit.
- **Fix:** In `begin_page_load()`, before calling `assign_frame(page_id, BufferFrameState::LOADING, out_frame_id)`, enforce:
  ```cpp
  if (loading_count() >= config_.max_pending_loads) {
      return StorageResult::BUSY;
  }
  ```
  This allows callers to suspend and retry rather than saturating host I/O and exhausting frames.
