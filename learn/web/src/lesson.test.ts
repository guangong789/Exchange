import { describe, expect, it } from 'vitest';
import {
  candles,
  classify,
  evaluateAnswer,
  highQuestion,
  isValidCandle,
  wickQuestion,
} from './lesson';

describe('local synthetic candle sequence', () => {
  it('contains at least 20 chronological, connected daily observations', () => {
    expect(candles.length).toBeGreaterThanOrEqual(20);
    expect(new Set(candles.map((c) => c.time)).size).toBe(candles.length);
    for (let i = 1; i < candles.length; i++) {
      expect(candles[i].open).toBe(candles[i - 1].close);
      expect(Date.parse(candles[i].time) - Date.parse(candles[i - 1].time)).toBe(86_400_000);
    }
  });
  it.each(candles)('$time has valid OHLC and volume', (candle) => {
    expect(isValidCandle(candle)).toBe(true);
  });
  it.each([
    { ...candles[0], high: 99 },
    { ...candles[0], low: 103 },
    { ...candles[0], close: Number.NaN },
    { ...candles[0], volume: -1 },
    { ...candles[0], volume: Infinity },
    { ...candles[0], low: 0 },
  ])('rejects invalid relationships and non-finite values', (candle) => {
    expect(isValidCandle(candle)).toBe(false);
  });
  it('retains exact OHLCV for the selected bullish example', () => {
    expect(candles[8]).toEqual({
      time: '2026-06-09',
      open: 108,
      high: 116,
      low: 106,
      close: 113,
      volume: 2860,
    });
  });
  it('retains exact OHLCV for bearish and doji examples', () => {
    expect(candles[14]).toMatchObject({ open: 118, high: 121, low: 110, close: 112, volume: 3180 });
    expect(candles[21]).toMatchObject({ open: 119, high: 124, low: 115, close: 119, volume: 2060 });
  });
  it.each([
    [8, 'bullish'],
    [14, 'bearish'],
    [21, 'doji'],
  ] as const)('classifies example %i as %s', (index, kind) => {
    expect(classify(candles[index])).toBe(kind);
  });
});

describe('deterministic lesson questions', () => {
  it.each([8, 14, 21])('binds the OHLC question to displayed sample %i', (index) => {
    const question = highQuestion(candles[index]);
    expect(evaluateAnswer(question, String(candles[index].high))).toBe(true);
    expect(evaluateAnswer(question, String(candles[index].low))).toBe(false);
    expect(evaluateAnswer(question, 'not-an-option')).toBe(false);
    expect(question.explanation).toContain(candles[index].high.toFixed(2));
  });
  it('supports a wrong answer followed by a correct retry without mutating the question', () => {
    const before = structuredClone(wickQuestion);
    expect(evaluateAnswer(wickQuestion, 'body')).toBe(false);
    expect(evaluateAnswer(wickQuestion, 'upper')).toBe(true);
    expect(wickQuestion).toEqual(before);
  });
});
