import { useCallback, useEffect, useState } from 'react';
import { CandleChart } from './CandleChart';
import { CandleDiagram } from './CandleDiagram';
import {
  type Concept,
  candles,
  classify,
  concepts,
  examples,
  formatPrice,
  highQuestion,
  kindLabels,
  type PriceField,
  priceLabels,
  wickQuestion,
} from './lesson';
import { initialProgress, isComplete, loadProgress, passQuestion, saveProgress } from './progress';
import { QuizCard } from './QuizCard';

export function App() {
  const [selected, setSelected] = useState(8);
  const [concept, setConcept] = useState<Concept>('ohlc');
  const [field, setField] = useState<PriceField | null>(null);
  const [progress, setProgress] = useState(loadProgress);
  const [generation, setGeneration] = useState(0);
  const selectCandle = useCallback((index: number) => {
    setSelected(Math.max(0, Math.min(candles.length - 1, index)));
    setField(null);
  }, []);
  const candle = candles[selected];
  const kind = classify(candle);
  const currentConcept = concepts.find((c) => c.id === concept) ?? concepts[0];
  const complete = isComplete(progress);

  useEffect(() => {
    saveProgress(progress);
  }, [progress]);

  function restart() {
    setProgress(initialProgress());
    setGeneration((n) => n + 1);
    selectCandle(8);
    setConcept('ohlc');
  }

  return (
    <>
      <a href="#explore" className="skip-link">
        跳到图表探索
      </a>
      <header className="site-header" id="top">
        <a className="brand" href="#top" aria-label="Exchange Learn 页首">
          <svg viewBox="0 0 36 36" width="36" height="36" aria-hidden="true">
            <g stroke="currentColor" strokeWidth="1.5">
              <path d="M12 8v21M23 6v23" />
              <path d="M9 14h6v9H9zM20 11h6v10h-6z" fill="currentColor" />
            </g>
          </svg>
          <span>
            Exchange <span className="brand-light">Learn</span>
          </span>
        </a>
        <span className="header-caption">图表入门</span>
        <div className="header-progress">
          <span className="sr-only">本课进度：{progress.passed.length} / 2 题</span>
          <span className="progress-caption">本课进度</span>
          <div className="progress-track">
            <span style={{ width: `${progress.passed.length * 50}%` }} />
          </div>
          <span className="mono">{progress.passed.length}/2</span>
        </div>
        <button type="button" className="restart-button" onClick={restart}>
          <span aria-hidden="true">↻</span> 重新开始
        </button>
      </header>

      <main>
        <section className="lesson-intro">
          <div>
            <div className="eyebrow">
              基础图表 <span className="lesson-divider">/</span> 第一课
            </div>
            <h1>
              认识一根 <span>K 线</span>
              <span className="title-dot">。</span>
            </h1>
            <p>选一根，看看四个价格如何记录一天。</p>
          </div>
          <a className="quiz-link" href="#quiz">
            试两道小题 <span aria-hidden="true">↗</span>
          </a>
        </section>

        <section
          className="workspace"
          id="explore"
          aria-label="K 线探索实验室"
          data-candle-kind={kind}
        >
          <div className="market-panel">
            <div className="market-header">
              <div className="market-name">
                <div>
                  <div className="module-label">
                    <span>01</span> 图表探索
                  </div>
                  <h2>24 天，24 根 K 线</h2>
                  <p>合成日线 · 价格单位：点</p>
                </div>
              </div>
              <span className="chart-hint">点选图表，或用 ← → 切换</span>
            </div>
            <div className="chart-legend">
              <span>
                <i className="legend-up" />
                阳线 ↗
              </span>
              <span>
                <i className="legend-down" />
                阴线 ↘
              </span>
              <span>＋ 十字线</span>
            </div>
            <CandleChart
              selected={selected}
              concept={concept}
              field={field}
              onSelect={selectCandle}
            />
            <section className="candle-details" aria-label="选中 K 线">
              <div className="chart-selection">
                <div className="selection-date">
                  <span className="selection-dot" />
                  <span className="detail-label">选中 K 线</span>
                  <strong className="mono">{candle.time}</strong>
                  <span className="selection-count mono">
                    {String(selected + 1).padStart(2, '0')} / {candles.length}
                  </span>
                </div>
                <div className="candle-navigation">
                  <button
                    type="button"
                    aria-label="上一根 K 线"
                    disabled={selected === 0}
                    onClick={() => selectCandle(selected - 1)}
                  >
                    ←
                  </button>
                  <label className="sr-only" htmlFor="candle-selector">
                    选择 K 线
                  </label>
                  <input
                    id="candle-selector"
                    type="range"
                    min="0"
                    max={candles.length - 1}
                    value={selected}
                    aria-valuetext={`第 ${selected + 1} 根，${candle.time}，${kindLabels[kind]}，开盘 ${candle.open}，最高 ${candle.high}，最低 ${candle.low}，收盘 ${candle.close}，成交量 ${candle.volume}`}
                    onChange={(event) => selectCandle(Number(event.target.value))}
                  />
                  <button
                    type="button"
                    aria-label="下一根 K 线"
                    disabled={selected === candles.length - 1}
                    onClick={() => selectCandle(selected + 1)}
                  >
                    →
                  </button>
                </div>
              </div>
              <section className="candle-data" aria-label="选中 K 线的价格">
                {priceLabels.map((p) => (
                  <button
                    type="button"
                    key={p.field}
                    className={field === p.field ? 'price-selected' : ''}
                    aria-pressed={field === p.field}
                    onClick={() => {
                      setConcept('ohlc');
                      setField(field === p.field ? null : p.field);
                    }}
                  >
                    <span>
                      {p.chinese}
                      <span className="data-english">{p.english}</span>
                    </span>
                    <strong className="mono" data-testid={`price-${p.field}`}>
                      {formatPrice(candle[p.field])}
                    </strong>
                  </button>
                ))}
                <div className="volume-data">
                  <span>
                    成交量 <span className="data-english">Volume</span>
                  </span>
                  <strong className="mono" data-testid="volume">
                    {candle.volume.toLocaleString('zh-CN')} <small>单位</small>
                  </strong>
                </div>
              </section>
            </section>
            <div className="chart-footnote">
              <span>下方柱形为成交量。合成图表不代表可成交的挂单。</span>
            </div>
          </div>

          <aside className="inspector" aria-labelledby="inspector-heading">
            <div className="inspector-header">
              <span className="eyebrow">选中 K 线</span>
              <span className="inspector-date">{candle.time.slice(5).replace('-', ' / ')}</span>
            </div>
            <h2 id="inspector-heading">拆开这一根</h2>
            <fieldset className="concept-controls">
              <legend className="sr-only">探索概念</legend>
              {concepts.map((c) => (
                <button
                  type="button"
                  key={c.id}
                  aria-label={`${c.label} ${c.english}`}
                  aria-pressed={concept === c.id}
                  onClick={() => {
                    setConcept(c.id);
                    setField(null);
                  }}
                >
                  <span>{c.id === 'ohlc' ? '四个价格' : c.label}</span>
                  <span className="concept-english">{c.english}</span>
                </button>
              ))}
            </fieldset>
            <CandleDiagram candle={candle} concept={concept} field={field} />
            <div className={`kind-tag kind-${kind}`} data-testid="candle-kind">
              {kind === 'bullish' ? '↗' : kind === 'bearish' ? '↘' : '＋'} {kindLabels[kind]}
            </div>
            <div className="concept-copy" aria-live="polite">
              <h3>
                {currentConcept.label}
                <span className="concept-english">{currentConcept.english}</span>
              </h3>
              <p>{currentConcept.hint}</p>
            </div>
            <div className="example-picker">
              <span>比较不同形态</span>
              <div>
                {examples.map((example) => (
                  <button
                    type="button"
                    key={example.index}
                    aria-label={`${example.label}示例`}
                    aria-pressed={selected === example.index}
                    onClick={() => selectCandle(example.index)}
                  >
                    <span aria-hidden="true">{example.symbol}</span> {example.label}
                  </button>
                ))}
              </div>
            </div>
          </aside>
        </section>

        <section className="quiz-section" id="quiz" aria-labelledby="quiz-heading">
          <div className="quiz-intro">
            <div className="eyebrow">
              <span className="stage-number">02</span> 小练习
            </div>
            <h2 id="quiz-heading">把观察变成理解</h2>
            <p>两道题，答案就在图里。</p>
            <span className="quiz-counter mono">
              {String(progress.passed.length).padStart(2, '0')}{' '}
              <span className="quiz-total">/ 02</span>
            </span>
          </div>
          <div className="quiz-questions">
            <QuizCard
              key={`high-${selected}-${generation}`}
              question={highQuestion(candle)}
              number={1}
              passed={progress.passed.includes('high')}
              onPass={() => setProgress((p) => passQuestion(p, 'high'))}
            />
            <QuizCard
              key={`wick-${generation}`}
              question={wickQuestion}
              number={2}
              passed={progress.passed.includes('wick')}
              onPass={() => setProgress((p) => passQuestion(p, 'wick'))}
            />
          </div>
          <div className={`lesson-status ${complete ? 'lesson-complete' : ''}`} role="status">
            <span className="status-icon" aria-hidden="true">
              {complete ? '✓' : '○'}
            </span>
            <div>
              <strong>{complete ? '第一课，完成！' : '慢慢观察，不用着急。'}</strong>
              <p>
                {complete
                  ? '你已认识 OHLC、实体和影线。随时回来再练习。'
                  : '完成两道小题，点亮你的第一课。'}
              </p>
            </div>
            {complete && (
              <button type="button" className="text-button" onClick={restart}>
                再探索一次 ↗
              </button>
            )}
          </div>
        </section>
      </main>
      <footer className="site-footer">
        <span>
          Exchange Learn <span className="footer-divider">/</span> 图表里的第一步
        </span>
        <span className="attribution">
          TradingView Lightweight Charts™
          <br />
          Copyright (с) 2025 TradingView, Inc.{' '}
          <a href="https://www.tradingview.com/" target="_blank" rel="noreferrer">
            TradingView ↗
          </a>
          <span> · </span>
          <a href="/third-party/lightweight-charts-LICENSE.txt">Apache-2.0</a>
        </span>
      </footer>
    </>
  );
}
