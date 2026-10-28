import {
  type CustomData,
  type CustomSeriesOptions,
  type CustomSeriesWhitespaceData,
  customSeriesDefaultOptions,
  type ICustomSeriesPaneRenderer,
  type ICustomSeriesPaneView,
  type PaneRendererCustomData,
  type PriceToCoordinateConverter,
  type Time,
} from 'lightweight-charts';
import { barColor, barGeometry, selectionGeometry } from './chartGeometry';

export interface CandlePlot extends CustomData<Time> {
  open: number;
  high: number;
  low: number;
  close: number;
  selected: boolean;
}

/** Public custom-series renderer: only candle geometry; the library owns scales and input. */
export class SlenderCandles
  implements ICustomSeriesPaneView<Time, CandlePlot>, ICustomSeriesPaneRenderer
{
  private data: PaneRendererCustomData<Time, CandlePlot> | null = null;

  renderer(): ICustomSeriesPaneRenderer {
    return this;
  }

  update(data: PaneRendererCustomData<Time, CandlePlot>): void {
    this.data = data;
  }

  priceValueBuilder(candle: CandlePlot): number[] {
    return [candle.low, candle.high, candle.close];
  }

  isWhitespace(
    data: CandlePlot | CustomSeriesWhitespaceData<Time>,
  ): data is CustomSeriesWhitespaceData<Time> {
    return !('close' in data);
  }

  defaultOptions(): CustomSeriesOptions {
    return {
      ...customSeriesDefaultOptions,
      color: '#387b70',
      priceLineVisible: false,
      lastValueVisible: false,
    };
  }

  draw(
    target: Parameters<ICustomSeriesPaneRenderer['draw']>[0],
    convert: PriceToCoordinateConverter,
  ): void {
    const data = this.data;
    if (!data?.visibleRange) return;
    const { from, to } = data.visibleRange;
    // biome-ignore lint/correctness/useHookAtTopLevel: FancyCanvas drawing API, not a React hook.
    target.useBitmapCoordinateSpace(
      ({ context: ctx, horizontalPixelRatio: dx, verticalPixelRatio: dy, bitmapSize }) => {
        const wickWidth = Math.max(1, Math.round(dx));
        for (let i = from; i < to; i++) {
          const bar = data.bars[i];
          const candle = bar.originalData;
          const prices = [candle.open, candle.high, candle.low, candle.close].map(convert);
          if (prices.some((p) => p === null)) continue;
          const [open, high, low, close] = prices.map((p) => Math.round((p as number) * dy));
          const { center: x, left, width } = barGeometry(bar.x, data.barSpacing, dx);
          if (candle.selected) {
            const selection = selectionGeometry(bar.x, data.barSpacing, dx);
            ctx.fillStyle = '#eef3f2';
            ctx.fillRect(selection.left, 0, selection.width, bitmapSize.height);
          }
          const falling = candle.close < candle.open;
          ctx.fillStyle = barColor(falling, candle.selected);
          ctx.fillRect(x - Math.floor(wickWidth / 2), high, wickWidth, Math.max(1, low - high));
          ctx.fillRect(
            left,
            Math.min(open, close),
            width,
            Math.max(Math.round(dy), Math.abs(open - close)),
          );
        }
      },
    );
  }
}
