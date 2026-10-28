import type { ICustomSeriesPaneRenderer, PaneRendererCustomData, Time } from 'lightweight-charts';
import { describe, expect, it } from 'vitest';
import { barGeometry } from './chartGeometry';
import { candles } from './lesson';
import { MatchedVolume, type VolumePlot } from './MatchedVolume';
import { type CandlePlot, SlenderCandles } from './SlenderCandles';

type Rect = { x: number; y: number; width: number; height: number; color: string };
function target(ratio: number, height: number) {
  const rects: Rect[] = [];
  const context = {
    fillStyle: '',
    fillRect(x: number, y: number, width: number, height: number) {
      rects.push({ x, y, width, height, color: this.fillStyle });
    },
  };
  const canvas = {
    useBitmapCoordinateSpace(callback: (scope: unknown) => void) {
      callback({
        context,
        horizontalPixelRatio: ratio,
        verticalPixelRatio: ratio,
        bitmapSize: { width: 500 * ratio, height: height * ratio },
      });
    },
  } as unknown as Parameters<ICustomSeriesPaneRenderer['draw']>[0];
  return { canvas, rects };
}

describe('candle and volume correspondence', () => {
  for (const ratio of [1, 1.25, 2, 3]) {
    for (const spacing of [8, 18, 30, 36.5, 48]) {
      it(`shares bitmap edges, width and selection at DPR ${ratio}, spacing ${spacing}`, () => {
        const price = new SlenderCandles();
        const volume = new MatchedVolume();
        const priceTarget = target(ratio, 250);
        const volumeTarget = target(ratio, 80);
        const candle: CandlePlot = { ...candles[8], selected: true };
        const row: VolumePlot = { time: candle.time, value: 2860, selected: true, falling: false };
        const common = {
          barSpacing: spacing,
          visibleRange: { from: 0, to: 1 },
          conflationFactor: 1,
        };
        const priceData: PaneRendererCustomData<Time, CandlePlot> = {
          ...common,
          bars: [{ x: 99.5, time: 8, originalData: candle, barColor: '' }],
        };
        const volumeData: PaneRendererCustomData<Time, VolumePlot> = {
          ...common,
          bars: [{ x: 99.5, time: 8, originalData: row, barColor: '' }],
        };
        price.update(priceData);
        volume.update(volumeData);
        price.draw(
          priceTarget.canvas,
          (p) => (150 - p) as ReturnType<Parameters<SlenderCandles['draw']>[1]>,
        );
        volume.draw(
          volumeTarget.canvas,
          (p) => (75 - p / 50) as ReturnType<Parameters<MatchedVolume['draw']>[1]>,
        );
        const body = priceTarget.rects[2];
        const bar = volumeTarget.rects[1];
        expect({ x: bar.x, width: bar.width, color: bar.color }).toEqual({
          x: body.x,
          width: body.width,
          color: body.color,
        });
        const candleHighlight = priceTarget.rects[0];
        const volumeHighlight = volumeTarget.rects[0];
        expect(volumeHighlight.x).toBe(candleHighlight.x);
        expect(volumeHighlight.width).toBe(candleHighlight.width);
        expect(bar.height).toBe(Math.round(75 * ratio) - Math.round((75 - 2860 / 50) * ratio));
        const geometry = barGeometry(99.5, spacing, ratio);
        const next = barGeometry(99.5 + spacing, spacing, ratio);
        const visibleGap = next.left - (geometry.left + geometry.width);
        expect(visibleGap).toBeGreaterThanOrEqual(1);
        expect(visibleGap / ratio).toBeLessThanOrEqual(2.5);
      });
    }
  }

  it('retains zero as the baseline and does not invent volume for a zero observation', () => {
    const series = new MatchedVolume();
    const zero: VolumePlot = { time: candles[0].time, value: 0, falling: false, selected: false };
    expect(series.priceValueBuilder(zero)).toEqual([0, 0, 0]);
    const capture = target(1, 80);
    series.update({
      bars: [{ x: 30, time: 0, originalData: zero, barColor: '' }],
      visibleRange: { from: 0, to: 1 },
      barSpacing: 20,
      conflationFactor: 1,
    });
    series.draw(capture.canvas, () => 75 as ReturnType<Parameters<MatchedVolume['draw']>[1]>);
    expect(capture.rects[0].height).toBe(0);
  });
});
