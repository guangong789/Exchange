import { useId } from 'react';
import { type Candle, type Concept, classify, formatPrice, type PriceField } from './lesson';

export function CandleDiagram({
  candle,
  concept,
  field,
}: {
  candle: Candle;
  concept: Concept;
  field: PriceField | null;
}) {
  const id = useId();
  const span = Math.max(candle.high - candle.low, 1);
  const y = (price: number) => 40 + ((candle.high - price) / span) * 164;
  const top = y(Math.max(candle.open, candle.close));
  const bottom = y(Math.min(candle.open, candle.close));
  const doji = classify(candle) === 'doji';
  const labels: { field: PriceField; y: number; text: string; left: boolean; labelY?: number }[] = [
    { field: 'high', y: y(candle.high), text: 'H 最高', left: false },
    { field: 'low', y: y(candle.low), text: 'L 最低', left: false },
    { field: 'open', y: y(candle.open), text: 'O 开盘', left: true },
    { field: 'close', y: y(candle.close), text: 'C 收盘', left: false },
  ];
  const rightLabels = labels.filter((label) => !label.left).sort((a, b) => a.y - b.y);
  let previous = 0;
  for (const label of rightLabels) {
    label.labelY = Math.max(label.y, previous + 38);
    previous = label.labelY;
  }
  let next = 246;
  for (const label of [...rightLabels].reverse()) {
    label.labelY = Math.min(label.labelY ?? label.y, next - 38);
    next = label.labelY;
  }
  const tone = classify(candle) === 'bearish' ? '#aa615b' : '#367d70';
  const bodyActive = concept !== 'wicks';
  const wickActive = concept !== 'body';

  return (
    <svg
      className="candle-diagram"
      viewBox="0 0 300 246"
      role="img"
      aria-labelledby={`${id}-title ${id}-description`}
    >
      <title id={`${id}-title`}>
        选中 K 线的{concept === 'body' ? '实体' : concept === 'wicks' ? '影线' : '四个价格'}
      </title>
      <desc id={`${id}-description`}>
        开盘 {candle.open}，最高 {candle.high}，最低 {candle.low}，收盘 {candle.close}。 实体高度{' '}
        {Math.abs(candle.close - candle.open)}，上影线{' '}
        {candle.high - Math.max(candle.open, candle.close)}，下影线{' '}
        {Math.min(candle.open, candle.close) - candle.low}。
      </desc>
      <line x1="139" x2="139" y1="24" y2="221" stroke="#e6e9e8" strokeDasharray="3 5" />
      {concept === 'body' && (
        <rect
          x="117"
          y={top - 9}
          width="44"
          height={Math.max(bottom - top + 18, 20)}
          rx="3"
          fill={`${tone}10`}
          stroke={`${tone}50`}
        />
      )}
      {concept === 'wicks' && (
        <>
          <rect x="126" y="30" width="26" height={Math.max(top - 24, 8)} rx="8" fill="#367d7010" />
          <rect
            x="126"
            y={bottom - 5}
            width="26"
            height={Math.max(214 - bottom, 8)}
            rx="8"
            fill="#367d7010"
          />
        </>
      )}
      <g className="diagram-structure" style={{ opacity: wickActive ? 1 : 0.2 }}>
        <line
          x1="139"
          x2="139"
          y1={y(candle.high)}
          y2={top}
          stroke={tone}
          strokeWidth={concept === 'wicks' ? 2.5 : 1.25}
          strokeLinecap="round"
        />
        <line
          x1="139"
          x2="139"
          y1={bottom}
          y2={y(candle.low)}
          stroke={tone}
          strokeWidth={concept === 'wicks' ? 2.5 : 1.25}
          strokeLinecap="round"
        />
      </g>
      <rect
        className="diagram-structure"
        x="129"
        y={doji ? top - 1.5 : top}
        width="20"
        height={Math.max(bottom - top, 3)}
        rx="1"
        fill={tone}
        style={{ opacity: bodyActive ? 0.9 : 0.18 }}
      />
      {concept === 'ohlc' &&
        labels.map((label) => {
          const active = field === null || field === label.field;
          return (
            <g key={label.field} opacity={active ? 1 : 0.3}>
              <path
                d={
                  label.left
                    ? `M 128 ${label.y} H 76`
                    : `M 151 ${label.y} H 175 L 204 ${label.labelY ?? label.y}`
                }
                fill="none"
                stroke={tone}
                strokeOpacity="0.6"
                strokeWidth="0.8"
              />
              <circle cx="139" cy={label.y} r="3" fill="#ffffff" stroke={tone} />
              <text
                x={label.left ? 70 : 211}
                y={(label.labelY ?? label.y) - 5}
                textAnchor={label.left ? 'end' : 'start'}
                fill="#68747d"
                fontSize="11"
              >
                {label.text}
              </text>
              <text
                x={label.left ? 70 : 211}
                y={(label.labelY ?? label.y) + 11}
                textAnchor={label.left ? 'end' : 'start'}
                fill="#26323a"
                fontSize="14"
                className="mono"
              >
                {formatPrice(candle[label.field])}
              </text>
            </g>
          );
        })}
      {concept === 'body' && (
        <>
          <text x="196" y={(top + bottom) / 2 - 2} fill={tone} fontSize="12">
            {doji ? '开盘 = 收盘' : '开盘 ↔ 收盘'}
          </text>
          <text x="196" y={(top + bottom) / 2 + 18} fill="#68747d" fontSize="11">
            价差 {formatPrice(Math.abs(candle.close - candle.open))}
          </text>
          {doji && (
            <text x="139" y="233" textAnchor="middle" fill="#68747d" fontSize="11">
              十字线：实体是一条细线
            </text>
          )}
        </>
      )}
      {concept === 'wicks' && (
        <>
          <text x="178" y={Math.max((40 + top) / 2, 39)} fill={tone} fontSize="12">
            上影线
          </text>
          <text x="178" y={Math.min((bottom + 204) / 2 + 5, 210)} fill={tone} fontSize="12">
            下影线
          </text>
          <text x="139" y="238" textAnchor="middle" fill="#68747d" fontSize="11">
            影线端点：最高价 / 最低价
          </text>
        </>
      )}
    </svg>
  );
}
