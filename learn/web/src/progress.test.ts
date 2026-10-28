import { describe, expect, it } from 'vitest';
import {
  initialProgress,
  isComplete,
  passQuestion,
  readProgress,
  STORAGE_KEY,
  saveProgress,
} from './progress';

function storage(initial: string | null = null) {
  let value = initial;
  return {
    getItem: (_key: string) => value,
    setItem: (_key: string, next: string) => {
      value = next;
    },
  };
}

describe('local progress lifecycle', () => {
  it('starts incomplete and completes only after both distinct questions', () => {
    const initial = initialProgress();
    expect(isComplete(initial)).toBe(false);
    const first = passQuestion(initial, 'high');
    expect(first.passed).toEqual(['high']);
    expect(isComplete(first)).toBe(false);
    expect(passQuestion(first, 'high')).toBe(first);
    expect(isComplete(passQuestion(first, 'wick'))).toBe(true);
    expect(initial.passed).toEqual([]);
  });
  it('roundtrips completion then overwrites it with reset state', () => {
    const local = storage();
    saveProgress(passQuestion(passQuestion(initialProgress(), 'wick'), 'high'), local);
    expect(isComplete(readProgress(local))).toBe(true);
    saveProgress(initialProgress(), local);
    expect(readProgress(local)).toEqual(initialProgress());
  });
  it.each([
    null,
    '{',
    'null',
    '[]',
    'true',
    '42',
    '{}',
    '{"version":2,"passed":["high"]}',
    '{"version":1,"passed":"high"}',
    '{"version":1,"passed":["unknown"]}',
  ])('handles malformed/unsupported progress: %s', (raw) => {
    expect(readProgress(storage(raw))).toEqual(initialProgress());
  });
  it('does not count duplicate stored answers twice', () => {
    expect(readProgress(storage('{"version":1,"passed":["high","high"]}')).passed).toEqual([
      'high',
    ]);
  });
  it('keeps working when storage reads or writes throw', () => {
    const unavailable = {
      getItem: () => {
        throw new Error('unavailable');
      },
      setItem: () => {
        throw new Error('quota');
      },
    };
    expect(readProgress(unavailable)).toEqual(initialProgress());
    expect(() => saveProgress(initialProgress(), unavailable)).not.toThrow();
  });
  it('uses a namespaced, versioned storage key', () => {
    expect(STORAGE_KEY).toBe('exchange-learn:lesson-01:v1');
  });
});
