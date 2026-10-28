/** Lesson-only synthetic observations. No liquidity, matching, or execution claims. */
export interface Candle {
  readonly time: string;
  readonly open: number;
  readonly high: number;
  readonly low: number;
  readonly close: number;
  readonly volume: number;
}

export type CandleKind = 'bullish' | 'bearish' | 'doji';
export type Concept = 'ohlc' | 'body' | 'wicks';
export type PriceField = 'open' | 'high' | 'low' | 'close';

// Fixed daily sequence; each open continues from the previous close.
const rows: readonly (readonly [number, number, number, number, number])[] = [
  [100, 104, 98, 102, 1240],
  [102, 106, 101, 105, 1680],
  [105, 108, 103, 104, 1420],
  [104, 107, 102, 106, 1830],
  [106, 112, 105, 110, 2450],
  [110, 113, 107, 108, 1760],
  [108, 110, 104, 105, 1980],
  [105, 109, 103, 108, 2210],
  [108, 116, 106, 113, 2860],
  [113, 118, 111, 116, 2520],
  [116, 120, 113, 114, 2340],
  [114, 117, 110, 111, 2110],
  [111, 115, 109, 114, 1750],
  [114, 120, 112, 118, 2630],
  [118, 121, 110, 112, 3180],
  [112, 116, 108, 110, 2390],
  [110, 114, 108, 113, 1970],
  [113, 119, 112, 117, 2480],
  [117, 122, 115, 120, 2740],
  [120, 124, 117, 122, 2910],
  [122, 125, 118, 119, 2640],
  [119, 124, 115, 119, 2060],
  [119, 125, 117, 123, 2820],
  [123, 129, 121, 127, 3320],
];

export const candles: readonly Candle[] = rows.map(([open, high, low, close, volume], i) => ({
  time: `2026-06-${String(i + 1).padStart(2, '0')}`,
  open,
  high,
  low,
  close,
  volume,
}));

export function isValidCandle(c: Candle): boolean {
  return (
    [c.open, c.high, c.low, c.close, c.volume].every(Number.isFinite) &&
    Math.min(c.open, c.close, c.low) > 0 &&
    c.high >= Math.max(c.open, c.close) &&
    c.low <= Math.min(c.open, c.close) &&
    c.volume >= 0
  );
}

export function classify(c: Candle): CandleKind {
  return c.close === c.open ? 'doji' : c.close > c.open ? 'bullish' : 'bearish';
}

export const kindLabels: Record<CandleKind, string> = {
  bullish: '阳线 · 收盘 > 开盘',
  bearish: '阴线 · 收盘 < 开盘',
  doji: '十字线 · 收盘 = 开盘',
};

export const concepts: { id: Concept; label: string; english: string; hint: string }[] = [
  {
    id: 'ohlc',
    label: '四个价格',
    english: 'OHLC',
    hint: '一根 K 线，记录一个周期内的开盘、最高、最低和收盘价。',
  },
  {
    id: 'body',
    label: '实体',
    english: 'Body',
    hint: '实体连接开盘价与收盘价。它的高度，就是两者的价差。',
  },
  {
    id: 'wicks',
    label: '影线',
    english: 'Wicks',
    hint: '影线伸向最高价和最低价，记录实体之外到达过的价格。',
  },
];

export const priceLabels: {
  field: PriceField;
  chinese: string;
  english: string;
  letter: string;
}[] = [
  { field: 'open', chinese: '开盘', english: 'Open', letter: 'O' },
  { field: 'high', chinese: '最高', english: 'High', letter: 'H' },
  { field: 'low', chinese: '最低', english: 'Low', letter: 'L' },
  { field: 'close', chinese: '收盘', english: 'Close', letter: 'C' },
];

export const examples = [
  { index: 8, label: '阳线', symbol: '↗' },
  { index: 14, label: '阴线', symbol: '↘' },
  { index: 21, label: '十字线', symbol: '＋' },
] as const;

export function formatPrice(value: number): string {
  return value.toFixed(2);
}

export interface QuizQuestion {
  id: 'high' | 'wick';
  prompt: string;
  options: readonly { id: string; label: string }[];
  answer: string;
  explanation: string;
}

export function highQuestion(c: Candle): QuizQuestion {
  return {
    id: 'high',
    prompt: '选中这根 K 线的最高价是多少？',
    options: [c.low, c.high + 2, c.high].map((value) => ({
      id: String(value),
      label: formatPrice(value),
    })),
    answer: String(c.high),
    explanation: `最高价是 ${formatPrice(c.high)}，对应这根 K 线最上方的端点。`,
  };
}

export const wickQuestion: QuizQuestion = {
  id: 'wick',
  prompt: '上影线连接哪两个位置？',
  options: [
    { id: 'body', label: '开盘价与收盘价' },
    { id: 'upper', label: '最高价与实体顶部' },
    { id: 'lower', label: '最低价与实体底部' },
  ],
  answer: 'upper',
  explanation: '实体顶部是开盘价、收盘价中较高的一个；上影线从这里延伸到最高价。',
};

export function evaluateAnswer(question: QuizQuestion, answer: string): boolean {
  return question.options.some((option) => option.id === answer) && question.answer === answer;
}
