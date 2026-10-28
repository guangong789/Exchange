export const STORAGE_KEY = 'exchange-learn:lesson-01:v1';
export type QuestionId = 'high' | 'wick';
export interface Progress {
  version: 1;
  passed: QuestionId[];
}
export const initialProgress = (): Progress => ({ version: 1, passed: [] });

type StorageAccess = Pick<Storage, 'getItem' | 'setItem'>;

export function readProgress(storage: StorageAccess): Progress {
  try {
    const raw: unknown = JSON.parse(storage.getItem(STORAGE_KEY) ?? 'null');
    if (
      !raw ||
      typeof raw !== 'object' ||
      !('version' in raw) ||
      raw.version !== 1 ||
      !('passed' in raw) ||
      !Array.isArray(raw.passed) ||
      !raw.passed.every((id) => id === 'high' || id === 'wick')
    )
      return initialProgress();
    return { version: 1, passed: [...new Set(raw.passed)] as QuestionId[] };
  } catch {
    return initialProgress();
  }
}

export function loadProgress(): Progress {
  try {
    return readProgress(window.localStorage);
  } catch {
    return initialProgress();
  }
}

export function saveProgress(progress: Progress, storage?: StorageAccess): void {
  try {
    (storage ?? window.localStorage).setItem(STORAGE_KEY, JSON.stringify(progress));
  } catch {
    /* Private browsing or quota failure must not interrupt the lesson. */
  }
}

export function passQuestion(progress: Progress, id: QuestionId): Progress {
  return progress.passed.includes(id)
    ? progress
    : { ...progress, passed: [...progress.passed, id] };
}

export function isComplete(progress: Progress): boolean {
  return progress.passed.includes('high') && progress.passed.includes('wick');
}
