export const DATABASE_PAGE_SIZE = 4096;
export const FIRST_DATA_PAGE_ID = 2;
export const MAX_DATA_PAGE_ID = 2_147_483_647;

export function validateDataPageId(pageId: number): void {
  if (!Number.isSafeInteger(pageId) || pageId < FIRST_DATA_PAGE_ID || pageId > MAX_DATA_PAGE_ID) {
    throw new RangeError(`Invalid page ID: ${pageId}`);
  }
}

export enum SchedulerStatus {
  Ready = 0,
  PageFault = 1,
  Flushing = 2,
  Complete = 3,
  Cancelled = 4,
  Error = 5,
}

export enum StorageResult {
  Success = 0,
  PageFull = 1,
  TupleTooLarge = 2,
  SlotNotFound = 3,
  CorruptedPage = 4,
  VersionMismatch = 5,
  SchemaMismatch = 6,
  InvalidArgument = 7,
  CycleDetected = 8,
  IoError = 9,
  BufferFull = 10,
  PageNotResident = 11,
  LoadInProgress = 12,
  Busy = 13,
  FlushRequired = 14,
}

export interface AsyncPageStore {
  readPages(pageIds: readonly number[]): Promise<Map<number, Uint8Array>>;
  writePages(pages: ReadonlyMap<number, Uint8Array>): Promise<void>;
}

// This is the TypeScript-shaped view of the Step 5 Embind adapter. It deliberately uses string
// IDs because JavaScript numbers cannot represent every uint64_t operation ID exactly.
export interface SchedulerBridge {
  startOperation(plan: string): string;
  lastStartResult(): StorageResult;
  stepOperation(operationId: string): SchedulerStatus;
  getPendingPageIds(operationId: string): readonly number[];
  providePage(operationId: string, pageId: number, bytes: Uint8Array): StorageResult;
  getDirtyPageIds(operationId: string): readonly number[];
  copyDirtyPage(operationId: string, pageId: number): Uint8Array;
  finishFlush(operationId: string, success: boolean): StorageResult;
  failOperation(operationId: string, message: string): StorageResult;
  cancelOperation(operationId: string): void;
  releaseOperation(operationId: string): StorageResult;
  getExecutionResults(operationId: string): string;
  getExecutionError(operationId: string): string;
}

export type OperationOutcome =
  | { readonly status: SchedulerStatus.Complete; readonly result: string }
  | { readonly status: SchedulerStatus.Cancelled }
  | { readonly status: SchedulerStatus.Error; readonly error: string };
