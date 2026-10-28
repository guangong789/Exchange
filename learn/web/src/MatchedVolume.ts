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

export interface VolumePlot extends CustomData<Time> {
  value: number;
  falling: boolean;
  selected: boolean;
}

/** Public series in a dedicated pane, sharing the candle series' time scale and geometry. */
export class MatchedVolume
  implements ICustomSeriesPaneView<Time, VolumePlot>, ICustomSeriesPaneRenderer
{
  private data: PaneRendererCustomData<Time, VolumePlot> | null = null;

  renderer(): ICustomSeriesPaneRenderer {
    return this;
  }
  update(data: PaneRendererCustomData<Time, VolumePlot>): void {
    this.data = data;
  }
  priceValueBuilder(row: VolumePlot): number[] {
    return [0, row.value, row.value];
  }
  isWhitespace(
    data: VolumePlot | CustomSeriesWhitespaceData<Time>,
  ): data is CustomSeriesWhitespaceData<Time> {
    return !('value' in data);
  }
  defaultOptions(): CustomSeriesOptions {
    return {
      ...customSeriesDefaultOptions,
      color: '#9abdaf',
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
    // biome-ignore lint/correctness/useHookAtTopLevel: FancyCanvas API, not a React hook.
    target.useBitmapCoordinateSpace(
      ({ context: ctx, horizontalPixelRatio: dx, verticalPixelRatio: dy, bitmapSize }) => {
        const zero = convert(0);
        if (zero === null) return;
        const baseline = Math.round(zero * dy);
        for (let i = from; i < to; i++) {
          const bar = data.bars[i];
          const row = bar.originalData;
          const value = convert(row.value);
          if (value === null) continue;
          const { left, width } = barGeometry(bar.x, data.barSpacing, dx);
          if (row.selected) {
            const selection = selectionGeometry(bar.x, data.barSpacing, dx);
            ctx.fillStyle = '#eef3f2';
            ctx.fillRect(selection.left, 0, selection.width, bitmapSize.height);
          }
          ctx.fillStyle = barColor(row.falling, row.selected, true);
          const top = Math.round(value * dy);
          ctx.fillRect(left, top, width, Math.max(0, baseline - top));
        }
      },
    );
  }
}
