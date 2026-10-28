import {
  ColorType,
  CrosshairMode,
  createChart,
  createSeriesMarkers,
  type ISeriesApi,
  type ISeriesMarkersPluginApi,
  LineStyle,
  type Time,
  type WhitespaceData,
} from 'lightweight-charts';
import { useEffect, useRef } from 'react';
import { type Concept, candles, classify, type PriceField } from './lesson';
import { MatchedVolume, type VolumePlot } from './MatchedVolume';
import { type CandlePlot, SlenderCandles } from './SlenderCandles';

interface Props {
  selected: number;
  concept: Concept;
  field: PriceField | null;
  onSelect: (index: number) => void;
}

export function CandleChart({ selected, concept, field, onSelect }: Props) {
  const container = useRef<HTMLDivElement>(null);
  const seriesRef = useRef<ISeriesApi<'Custom', Time, CandlePlot | WhitespaceData<Time>> | null>(
    null,
  );
  const volumeRef = useRef<ISeriesApi<'Custom', Time, VolumePlot | WhitespaceData<Time>> | null>(
    null,
  );
  const markersRef = useRef<ISeriesMarkersPluginApi<Time> | null>(null);

  useEffect(() => {
    if (!container.current) return;
    const host = container.current;
    const chart = createChart(host, {
      autoSize: true,
      layout: {
        background: { type: ColorType.Solid, color: '#ffffff' },
        textColor: '#67727d',
        fontFamily: 'Learn Sans, system-ui, sans-serif',
        fontSize: 12,
        // Full NOTICE and TradingView link remain in the page footer, so the
        // optional in-chart logo can be hidden to keep early volume bars clear.
        attributionLogo: false,
        panes: { enableResize: false, separatorColor: '#e7ede9', separatorHoverColor: '#e7ede9' },
      },
      grid: {
        vertLines: { visible: false },
        horzLines: { color: '#edf0f2', style: LineStyle.Solid },
      },
      rightPriceScale: {
        borderVisible: false,
        entireTextOnly: true,
        scaleMargins: { top: 0.12, bottom: 0.12 },
      },
      timeScale: { borderVisible: false, rightOffset: 1, fixLeftEdge: true, fixRightEdge: true },
      crosshair: {
        mode: CrosshairMode.Normal,
        vertLine: { color: '#b9c4c5', labelBackgroundColor: '#344743' },
        horzLine: { color: '#b9c4c5', labelBackgroundColor: '#344743' },
      },
      handleScroll: false,
      handleScale: false,
      localization: { locale: 'zh-CN' },
    });
    const series = chart.addCustomSeries(new SlenderCandles(), {
      priceFormat: { type: 'price', precision: 2, minMove: 0.01 },
    });
    const volume = chart.addCustomSeries(
      new MatchedVolume(),
      {
        priceFormat: { type: 'volume' },
        // An overlay scale leaves the pane's default axis widget intact while
        // keeping volume units separate from the price axis.
        priceScaleId: 'volume',
      },
      1,
    );
    volume.priceScale().applyOptions({ scaleMargins: { top: 0.18, bottom: 0.05 } });
    chart.panes()[0].setStretchFactor(4);
    chart.panes()[1].setStretchFactor(1);
    volumeRef.current = volume;
    const markers = createSeriesMarkers(series, []);
    seriesRef.current = series;
    markersRef.current = markers;
    chart.subscribeClick((event) => {
      if (!event.time) return;
      // Library event times preserve the string inputs as BusinessDay objects.
      const time =
        typeof event.time === 'object'
          ? `${event.time.year}-${String(event.time.month).padStart(2, '0')}-${String(event.time.day).padStart(2, '0')}`
          : String(event.time);
      const index = candles.findIndex((c) => c.time === time);
      if (index >= 0) onSelect(index);
    });
    let active = true;
    void document.fonts.ready.then(() => {
      if (active)
        chart.applyOptions({ layout: { fontFamily: 'Learn Sans, system-ui, sans-serif' } });
    });
    const observer = new ResizeObserver(() => chart.timeScale().fitContent());
    observer.observe(host);
    return () => {
      active = false;
      observer.disconnect();
      markers.detach();
      chart.remove();
      seriesRef.current = null;
      volumeRef.current = null;
      markersRef.current = null;
    };
  }, [onSelect]);

  useEffect(() => {
    const series = seriesRef.current;
    if (!series) return;
    series.setData(
      candles.map((c, i) => ({
        time: c.time,
        open: c.open,
        high: c.high,
        low: c.low,
        close: c.close,
        selected: i === selected,
      })),
    );
    volumeRef.current?.setData(
      candles.map((c, i) => ({
        time: c.time,
        value: c.volume,
        falling: classify(c) === 'bearish',
        selected: i === selected,
      })),
    );
    const candle = candles[selected];
    markersRef.current?.setMarkers([
      { time: candle.time, position: 'belowBar', shape: 'circle', color: '#344743', size: 0.4 },
    ]);
    // A single quiet guide on explicit price selection; the four values live outside the plot.
    const lines =
      concept === 'ohlc' && field !== null
        ? [
            series.createPriceLine({
              price: candle[field],
              color: '#8daba5',
              lineWidth: 1,
              lineStyle: LineStyle.Dashed,
              axisLabelVisible: false,
              lineVisible: true,
            }),
          ]
        : [];
    return () => {
      for (const line of lines) series.removePriceLine(line);
    };
  }, [selected, concept, field]);

  return (
    <div className="chart-canvas" ref={container} aria-hidden="true" data-testid="market-chart" />
  );
}
