import type { ICustomSeriesPaneRenderer, PaneRendererCustomData, Time } from 'lightweight-charts';
import { describe, expect, it, vi } from 'vitest';
import { candles } from './lesson';
import { type CandlePlot, SlenderCandles } from './SlenderCandles';

function drawSample(index: number, ratio = 1, selected = false) {
  const candle = { ...candles[index], selected };
  const series = new SlenderCandles();
  const fillRect = vi.fn();
  const context = { fillStyle: '', fillRect };
  type Target = Parameters<ICustomSeriesPaneRenderer['draw']>[0];
  const target = {
    useBitmapCoordinateSpace(callback: (scope: unknown) => void) {
      callback({
        context,
        horizontalPixelRatio: ratio,
        verticalPixelRatio: ratio,
        bitmapSize: { width: 300 * ratio, height: 250 * ratio },
      });
    },
  } as unknown as Target;
  const data: PaneRendererCustomData<Time, CandlePlot> = {
    bars: [{ x: 100, time: 0, originalData: candle, barColor: '' }],
    barSpacing: 30,
    visibleRange: { from: 0, to: 1 },
    conflationFactor: 1,
  };
  series.update(data);
  series.draw(
    target,
    (price) => (150 - price) as ReturnType<Parameters<SlenderCandles['draw']>[1]>,
  );
  return { series, candle, fillRect };
}

describe('slender candles preserve OHLC geometry through the public renderer', () => {
  it.each([8, 14, 21])('keeps low/high/close available for scaling, sample %i', (index) => {
    const { series, candle } = drawSample(index);
    expect(series.priceValueBuilder(candle)).toEqual([candle.low, candle.high, candle.close]);
    expect(series.isWhitespace({ time: candle.time })).toBe(true);
    expect(series.isWhitespace(candle)).toBe(false);
  });

  it.each([1, 2])('renders true high/low wick and open/close body at DPR %i', (ratio) => {
    const { fillRect } = drawSample(8, ratio);
    const [wick, body] = fillRect.mock.calls;
    // Sample: O=108,H=116,L=106,C=113; screen y=150-price.
    expect(wick.slice(1)).toEqual([34 * ratio, ratio, 10 * ratio]);
    expect(body.slice(1)).toEqual([37 * ratio, ratio === 1 ? 29 : 57, 5 * ratio]);
  });

  it('preserves a visible doji and a separate selection background', () => {
    const { fillRect } = drawSample(21, 2, true);
    const [highlight, wick, body] = fillRect.mock.calls;
    expect(highlight[3]).toBe(500);
    expect(wick[1]).toBe(52);
    expect(wick[3]).toBe(18);
    expect(body[1]).toBe(62);
    expect(body[3]).toBe(2);
  });
});
