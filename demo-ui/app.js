const societyView = document.querySelector('#society-view');
const dashboard = document.querySelector('#dashboard');
const replayEvidence = document.querySelector('#replay-evidence');
const rawDetails = document.querySelector('#raw-details');
const raw = document.querySelector('#raw');
const statusLine = document.querySelector('#status');
const stream = document.querySelector('#activity-stream');
const streamStatus = document.querySelector('#stream-status');
const objectiveLabel = document.querySelector('#objective-label');
const objectiveLine = document.querySelector('#objective-line');
const startButton = document.querySelector('#start');
const stopButton = document.querySelector('#stop');
const replayButton = document.querySelector('#run-replay');
const modeButtons = document.querySelectorAll('.mode-button');
const simulationControls = document.querySelector('#simulation-controls');
const liveEvidence = document.querySelector('#live-x402-evidence');
const liveStatus = document.querySelector('#live-status');
const liveMessage = document.querySelector('#live-message');
const liveRunButton = document.querySelector('#run-live-x402');

let selectedMode = 'society';
let timer = null;
let lastSnapshot = null;
let lastLiveEvidence = {status: 'NOT_RUN'};

function value(input) { return input === null || input === undefined || input === '' ? '—' : String(input); }
function badgeKind(input) {
  if (['APPROVED', 'PreviewAuthorized', 'Accepted', 'PASS', 'EXACT', 'CONNECTED', 'true'].includes(input)) return 'approved';
  if (['InsufficientFunds', 'FAIL', 'MISMATCH', 'ERROR', 'DISCONNECTED', 'false'].includes(input)) return 'rejected';
  return 'neutral';
}
function renderRows(target, rows) {
  target.replaceChildren();
  rows.forEach((row) => {
    const dt = document.createElement('dt'); dt.textContent = row.label;
    const dd = document.createElement('dd');
    if (row.badge) {
      const badge = document.createElement('span');
      badge.className = `badge badge--${badgeKind(value(row.value))}`;
      badge.textContent = value(row.value);
      dd.append(badge);
    } else dd.textContent = value(row.value);
    if (row.tone) { dt.classList.add(`row--${row.tone}`); dd.classList.add(`row--${row.tone}`); }
    if (row.wide) { dt.classList.add('row--wide'); dd.classList.add('row--wide'); }
    target.append(dt, dd);
  });
}
function terminal(status) { return ['GOAL_ACHIEVED', 'USER_STOPPED', 'MAX_ROUNDS', 'ERROR'].includes(status); }
function actionMatch(action) { return /^SUBMIT (BUY|SELL) (\d+) @ (\d+)$/.exec(action); }
function decisionText(action) {
  const match = actionMatch(action);
  if (match) return `${match[1] === 'BUY' ? '买入' : '卖出'} ${match[2]} @ ${match[3]}`;
  if (action && action.startsWith('CANCEL ')) return `取消订单 ${action.slice(7)}`;
  return action === 'HOLD' ? 'HOLD（本轮不交易）' : value(action);
}
function submittedActionText(action) {
  const match = actionMatch(action);
  return match ? `提交${match[1] === 'BUY' ? '买单' : '卖单'} ${match[2]} @ ${match[3]}` : decisionText(action);
}
function orderText(order) { return order ? `${order.side === 'BUY' ? '买入' : '卖出'} ${order.quantity} @ ${order.price}` : '无'; }
function balanceText(balance) { return balance ? `可用 ${balance.available} / 预留 ${balance.reserved}` : '—'; }
function episodeOutcome(status) { return ({GOAL_ACHIEVED:'目标已完成', USER_STOPPED:'已在安全边界停止', MAX_ROUNDS:'达到最大轮次', ERROR:'运行异常'})[status] || status; }

function appendFlowCue(target) {
  const cue = document.createElement('div'); cue.className = 'flow-cue'; cue.textContent = '↓'; target.append(cue);
}
function appendPhase(target, title, facts, kind = '', notes = []) {
  const phase = document.createElement('section'); phase.className = `round-phase${kind ? ` round-phase--${kind}` : ''}`;
  if (title && title.trim()) {
    const heading = document.createElement('h3'); heading.textContent = title; phase.append(heading);
  }
  const list = document.createElement('dl'); list.className = 'phase-facts';
  facts.forEach(([label, fact, tone]) => {
    const dt = document.createElement('dt'); dt.textContent = label;
    const dd = document.createElement('dd'); dd.textContent = value(fact);
    if (tone) { dt.classList.add(`row--${tone}`); dd.classList.add(`row--${tone}`); }
    list.append(dt, dd);
  });
  phase.append(list);
  notes.forEach((note) => { const item = document.createElement('p'); item.className = 'phase-note'; item.textContent = note; phase.append(item); });
  target.append(phase);
}
function roundAnnotations(evidence) {
  const annotations = [];
  if (evidence.action === 'HOLD') annotations.push('HOLD');
  if (evidence.trades.length > 0 && evidence.resting_order) annotations.push('PARTIAL FILL');
  if (evidence.trades.length > 1) annotations.push('MULTI-TRADE');
  if (evidence.action !== 'HOLD' && evidence.submit_result !== 'Accepted') annotations.push('REJECTED');
  return annotations;
}
function coreSummary(evidence) {
  if (evidence.action === 'HOLD') return '无订单提交 · 状态未改变';
  if (evidence.trades.length === 0) return evidence.submit_result;
  const filledQuantity = evidence.trades.reduce((total, trade) => total + trade.quantity, 0);
  return evidence.resting_order
    ? `成交 ${filledQuantity} · 剩余挂单 ${evidence.resting_order.quantity}`
    : `成交 ${filledQuantity}`;
}
function renderActivities(roundEvidence, snapshot) {
  stream.replaceChildren();
  const structured = new Map(roundEvidence.map((round) => [round.round, round]));
  const lastRound = Math.max(0, ...structured.keys());
  roundEvidence.forEach((evidence, index) => {
    const item = document.createElement('details');
    const isCurrent = evidence.round === lastRound && snapshot.status === 'RUNNING';
    const annotations = roundAnnotations(evidence);
    item.className = `round-record${isCurrent ? ' round-record--active' : ''}${annotations.length ? ' round-record--meaningful' : ''}`;
    item.open = isCurrent;

    const summary = document.createElement('summary');
    const heading = document.createElement('div'); heading.className = 'round-record-heading';
    const title = document.createElement('h3'); title.textContent = `第 ${evidence.round} 轮`;
    heading.append(title);
    if (annotations.length || isCurrent) {
      const markers = document.createElement('span'); markers.className = 'round-markers';
      markers.textContent = annotations.join(' · ') || 'RUNNING'; heading.append(markers);
    }
    const compact = document.createElement('p'); compact.className = 'round-summary';
    [
      `${evidence.analyst.signal} ${evidence.analyst.confidence}%`,
      `Risk ≤ ${evidence.risk.max_recommended_quantity}`,
      decisionText(evidence.action),
      coreSummary(evidence),
      `Score ${evidence.score.delta >= 0 ? '+' : ''}${evidence.score.delta} · Objective ${evidence.objective.current} / ${evidence.objective.target}`,
    ].forEach((fact, factIndex) => {
      if (factIndex > 0) { const arrow = document.createElement('i'); arrow.textContent = '→'; compact.append(arrow); }
      const text = document.createElement('span'); text.textContent = fact; compact.append(text);
    });
    const disclosure = document.createElement('span'); disclosure.className = 'round-disclosure'; disclosure.textContent = '展开详情';
    summary.append(heading, compact, disclosure);

    const phases = document.createElement('div'); phases.className = 'activity-phases';
    appendPhase(phases, '分析 Agent', [
      ['市场信号', evidence.analyst.signal], ['置信度', `${evidence.analyst.confidence}%`],
      ['本地最优卖价', evidence.risk.local_best_ask], ['外部参考', `${evidence.external_market.best_bid} / ${evidence.external_market.best_ask}`],
      ['判断', evidence.analyst.reason],
    ], 'analyst');
    appendFlowCue(phases);
    appendPhase(phases, '风险 Agent', [
      ['可用 QUOTE', evidence.risk.available_quote], ['本地最优卖价', evidence.risk.local_best_ask],
      ['风险预算', `${evidence.risk.risk_budget_bps / 100}% / ${evidence.risk.risk_budget_bps} bps`],
      ['可负担数量', evidence.risk.affordable_quantity], ['风险预算数量', evidence.risk.risk_budget_quantity],
      ['目标剩余', evidence.risk.objective_remaining], ['建议最大数量', evidence.risk.max_recommended_quantity, 'recommended'],
    ], 'risk', index === 0 ? ['建议最大数量 = min(可负担数量, 风险预算数量, 目标剩余)'] : []);
    appendFlowCue(phases);
    const previous = structured.get(evidence.round - 1);
    appendPhase(phases, '交易 Agent · DeepSeek', [
      ['Analyst', `${evidence.analyst.signal} · ${evidence.analyst.confidence}%`],
      ['Risk 建议', `≤ ${evidence.risk.max_recommended_quantity}`], ['当前最优卖价', evidence.risk.local_best_ask],
      ['目标剩余', evidence.risk.objective_remaining], ['上一轮结果', previous ? previous.submit_result : '无'],
      ['最终决策', decisionText(evidence.action), 'action'],
    ], 'trader');
    appendFlowCue(phases);
    if (evidence.action === 'HOLD') {
      appendPhase(phases, '确定性经济执行', [['订单提交', '本轮无订单提交'], ['成交', '无'], ['经济状态', '未改变']], 'core');
    } else {
      const coreFacts = [['SubmitResult', evidence.submit_result, 'result'], ['提交', submittedActionText(evidence.action)]];
      evidence.trades.forEach((trade, tradeIndex) => coreFacts.push([tradeIndex === 0 ? '已成交' : '成交', `${trade.quantity} @ ${trade.price}`]));
      if (evidence.trades.length > 1) coreFacts.push(['成交笔数', evidence.trades.length]);
      if (evidence.resting_order) coreFacts.push(['剩余挂单', orderText(evidence.resting_order)], ['剩余预留', `${evidence.reserved_quote_after} QUOTE`]);
      else if (evidence.trades.length) coreFacts.push(['状态', '已全部成交']);
      else if (evidence.submit_result !== 'Accepted') coreFacts.push(['经济执行', '被拒绝，状态未改变']);
      appendPhase(phases, '确定性经济执行', coreFacts, 'core');
    }
    appendFlowCue(phases);
    appendPhase(phases, 'Episode Score', [
      ['本轮计分', `${evidence.score.before} → ${evidence.score.after}`], ['变化', `${evidence.score.delta >= 0 ? '+' : ''}${evidence.score.delta}`],
    ], 'score', evidence.score.reasons.length ? evidence.score.reasons : ['本轮无分数变化']);
    appendFlowCue(phases);
    appendPhase(phases, '目标进度', [['BASE', `${evidence.objective.current} / ${evidence.objective.target}`]], 'objective');
    item.append(summary, phases); stream.append(item);
  });
}

function renderEpisode(snapshot) {
  const summary = snapshot.summary;
  streamStatus.textContent = snapshot.status;
  const metadata = [
    {label:'Seed', value:snapshot.seed}, {label:'Objective', value:`${snapshot.objective.current} / ${snapshot.objective.target} BASE`},
    {label:'Rounds', value:`${snapshot.round.current} / ${snapshot.round.max}`},
  ];
  if (!summary) metadata.splice(2, 0, {label:'Episode Score', value:snapshot.score.total});
  renderRows(document.querySelector('#episode-meta'), metadata);
  const setupLine = snapshot.activities.find((activity) => !activity.round && activity.detail.includes('sell ladder='));
  const ladder = setupLine ? setupLine.detail.match(/sell ladder=(.+)$/) : null;
  renderRows(document.querySelector('#scenario-facts'), [
    {label:'Seed', value:snapshot.seed}, {label:'Objective', value:`获得 ${snapshot.objective.target} BASE`},
    {label:'流动性阶梯', value:ladder ? ladder[1] : null, wide:true},
    {label:'执行方式', value:'Account-backed'}, {label:'最大轮次', value:snapshot.round.max},
  ]);
  objectiveLabel.textContent = `目标进度 ${snapshot.objective.current} / ${snapshot.objective.target} BASE`;
  objectiveLine.style.width = `${Math.min(100, Math.max(0, (Number(snapshot.objective.current) / Number(snapshot.objective.target || 1)) * 100))}%`;
  renderActivities(snapshot.round_evidence || [], snapshot);

  const completion = document.querySelector('#episode-completion'); completion.hidden = !summary;
  if (!summary) return;
  document.querySelector('#episode-outcome').textContent = episodeOutcome(summary.end_reason);
  renderRows(document.querySelector('#episode-highlights'), [
    {label:'轮次', value:summary.rounds_completed, tone:'headline'}, {label:'Episode Score', value:summary.score.total, tone:'headline'},
    {label:'Objective', value:`${summary.final_base} / ${summary.objective_target} BASE`, tone:'headline'}, {label:'已成交', value:`${summary.quote_spent} QUOTE`, tone:'headline'},
    {label:'Reserved', value:`${summary.current_reserved_quote} QUOTE`}, {label:'Trade records', value:summary.trades},
    {label:'BASE filled', value:summary.filled_base_quantity}, {label:'DeepSeek decisions', value:summary.deepseek_calls},
  ]);
}

function renderExecutionExample(roundEvidence) {
  const target = document.querySelector('#execution-example');
  const flow = document.querySelector('#execution-example-flow');
  const example = roundEvidence.find((round) => round.trades.length > 0 && round.resting_order)
    || roundEvidence.find((round) => round.trades.length > 1)
    || roundEvidence.find((round) => round.trades.length > 0);
  target.hidden = !example; flow.replaceChildren();
  if (!example) return;
  const steps = [['Submitted', decisionText(example.action)]];
  example.trades.forEach((trade, index) => steps.push([index === 0 ? 'Executed' : 'Executed · continued', `${trade.quantity} @ ${trade.price}`]));
  if (example.resting_order) steps.push(['Resting', orderText(example.resting_order)], ['Reserved', `${example.reserved_quote_after} QUOTE`]);
  steps.forEach(([label, fact], index) => {
    if (index > 0) { const arrow = document.createElement('i'); arrow.textContent = '↓'; flow.append(arrow); }
    const step = document.createElement('span'); const small = document.createElement('small'); small.textContent = label;
    const strong = document.createElement('strong'); strong.textContent = fact; step.append(small, strong); flow.append(step);
  });
}
function renderRuntime(snapshot) {
  const summary = snapshot.summary;
  const rounds = snapshot.round_evidence || [];
  const submitted = rounds.filter((round) => round.action !== 'HOLD').length;
  const accepted = rounds.filter((round) => round.action !== 'HOLD' && round.submit_result === 'Accepted').length;
  const partial = rounds.filter((round) => round.trades.length > 0 && round.resting_order).length;
  const multiTrade = rounds.filter((round) => round.trades.length > 1).length;
  const submittedCount = summary ? summary.orders_submitted : submitted;
  const acceptedCount = summary ? summary.accepted_actions : accepted;
  const rejectedCount = summary ? summary.rejected_actions : snapshot.runtime.rejected_actions;
  const orderFacts = [`${submittedCount} submitted`, `${acceptedCount} accepted`, `${snapshot.runtime.active_orders} active`];
  if (rejectedCount > 0) orderFacts.push(`${rejectedCount} rejected`);
  document.querySelector('#runtime-orders').textContent = orderFacts.join(' · ');

  const executionFacts = [`${snapshot.runtime.trades} Trade records`];
  if (summary) executionFacts.push(`${summary.filled_base_quantity} BASE filled`);
  if (partial > 0) executionFacts.push(`${partial} partial fill`);
  if (multiTrade > 0) executionFacts.push(`${multiTrade} multi-trade`);
  document.querySelector('#runtime-execution').textContent = executionFacts.join(' · ');

  const financialFacts = [];
  if (summary) financialFacts.push(`${summary.quote_spent} QUOTE executed`);
  financialFacts.push(`${snapshot.runtime.trader_quote.reserved} QUOTE reserved`, `${snapshot.runtime.ledger_entries} Ledger entries`);
  if (summary) financialFacts.push(`${summary.invalid_state_mutations} invalid mutations`);
  document.querySelector('#runtime-financial').textContent = financialFacts.join('\n');
  document.querySelector('#runtime-policy').textContent = `${snapshot.match_engine.price_time_priority ? 'Price-Time Priority' : '—'}\nAccount-backed`;
  renderExecutionExample(rounds);
}

function renderReplay(replay, snapshot) {
  const worldsSection = document.querySelector('#replay-worlds-section');
  const worlds = document.querySelector('#replay-worlds');
  const inputSection = document.querySelector('#replay-input-section');
  const inputs = document.querySelector('#replay-inputs');
  const pipelineSection = document.querySelector('#replay-pipeline-section');
  const finalSection = document.querySelector('#replay-final-section');
  const comparison = document.querySelector('#replay-comparison-body');
  const verdict = document.querySelector('#replay-verdict');
  const hint = document.querySelector('#replay-hint');
  document.querySelector('#replay-status-label').textContent = replay.status;

  const hasStarted = replay.status !== 'NOT_RUN';
  worldsSection.hidden = !hasStarted; inputSection.hidden = !hasStarted; pipelineSection.hidden = !hasStarted;
  hint.textContent = 'Replay 不重新调用 DeepSeek、Analyst 服务或 x402 / Wallet integration；仅在 fresh world 中重放已捕获的确定性经济输入。';
  replayButton.hidden = hasStarted; replayButton.disabled = hasStarted;

  worlds.replaceChildren(); inputs.replaceChildren();
  if (hasStarted) {
    const originalModelCalls = snapshot.summary ? snapshot.summary.deepseek_calls : replay.deepseek_calls_original;
    const originalServiceCalls = snapshot.summary ? snapshot.summary.analyst_service_accesses : replay.payment_service_calls_original;
    [
      ['DeepSeek calls', originalModelCalls, replay.deepseek_calls_replay],
      ['Analyst 服务', originalServiceCalls, replay.payment_service_calls_replay],
      ['确定性经济输入', replay.captured_economic_inputs.length, replay.captured_economic_inputs.length],
    ].forEach(([label, original, replayValue]) => {
      const row = document.createElement('tr');
      [label, original, replayValue].forEach((cellValue) => { const cell = document.createElement('td'); cell.textContent = cellValue; row.append(cell); });
      worlds.append(row);
    });
    if (replay.captured_economic_inputs.length === 0) {
      const empty = document.createElement('li'); empty.textContent = '无非 HOLD 的经济输入'; inputs.append(empty);
    } else replay.captured_economic_inputs.forEach((input, index) => {
      const item = document.createElement('li'); item.textContent = `#${index + 1}  ${decisionText(input)}`; inputs.append(item);
    });
  }

  const terminalReplay = replay.status === 'EXACT' || replay.status === 'MISMATCH';
  finalSection.hidden = !replay.original_final_state || !replay.replay_final_state;
  comparison.replaceChildren(); verdict.hidden = true; verdict.textContent = '';
  if (!finalSection.hidden) {
    const original = replay.original_final_state;
    const replayed = replay.replay_final_state;
    [
      ['BASE available', original.trader_base.available, replayed.trader_base.available, replay.balance_parity],
      ['QUOTE available', original.trader_quote.available, replayed.trader_quote.available, replay.balance_parity],
      ['QUOTE reserved', original.trader_quote.reserved, replayed.trader_quote.reserved, replay.balance_parity],
      ['Trade records', original.trade_records, replayed.trade_records, replay.trade_parity],
      ['Active orders', original.active_orders, replayed.active_orders, replay.order_parity],
      ['Ledger entries', original.ledger_entries, replayed.ledger_entries, replay.ledger_parity],
      ['Objective', `${original.objective.current} / ${original.objective.target}`, `${replayed.objective.current} / ${replayed.objective.target}`, replay.objective_parity],
    ].forEach(([label, originalValue, replayValue, exact]) => {
      const row = document.createElement('tr');
      [label, originalValue, replayValue].forEach((cellValue) => { const cell = document.createElement('td'); cell.textContent = cellValue; row.append(cell); });
      const result = document.createElement('td'); result.textContent = exact ? 'EXACT' : 'MISMATCH'; result.className = exact ? 'comparison-exact' : 'comparison-mismatch'; row.append(result);
      comparison.append(row);
    });
  }
  if (terminalReplay) {
    verdict.hidden = false;
    verdict.className = `replay-verdict${replay.status === 'MISMATCH' ? ' replay-verdict--mismatch' : ''}`;
    verdict.textContent = replay.status === 'EXACT' ? 'REPLAY VERIFIED · EXACT MATCH' : 'REPLAY VERIFIED · MISMATCH';
  }
}

function render(snapshot) {
  lastSnapshot = snapshot;
  if (selectedMode !== 'society') return;
  dashboard.hidden = false; rawDetails.hidden = false; raw.textContent = JSON.stringify(snapshot, null, 2);
  renderEpisode(snapshot); renderRuntime(snapshot); renderReplay(snapshot.replay, snapshot);
  replayEvidence.hidden = !terminal(snapshot.status);
  const busy = snapshot.status === 'RUNNING' || snapshot.status === 'STOP_REQUESTED';
  startButton.disabled = busy; stopButton.disabled = snapshot.status !== 'RUNNING'; modeButtons.forEach((button) => { button.disabled = busy; });
  if (terminal(snapshot.status)) { window.clearTimeout(timer); timer = null; }
}

async function command(action, scenario) {
  const payload = {action}; if (scenario) payload.scenario = scenario;
  const response = await fetch('/api/simulation', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify(payload), cache:'no-store'});
  const data = await response.json(); if (!response.ok || data.error) throw new Error(data.error || 'simulation command failed'); return data;
}
function scheduleSimulation(snapshot) {
  window.clearTimeout(timer);
  if (snapshot.status === 'RUNNING' || snapshot.status === 'STOP_REQUESTED') timer = window.setTimeout(async () => {
    try { const next = await command('advance'); render(next); scheduleSimulation(next); } catch (error) { showError(error); }
  }, 700);
}
function scheduleReplay(snapshot) {
  window.clearTimeout(timer);
  if (snapshot.replay.status === 'RUNNING') timer = window.setTimeout(async () => {
    try { const next = await command('replay-advance'); render(next); scheduleReplay(next); } catch (error) { showError(error); }
  }, 360);
}
function showError(error) { window.clearTimeout(timer); statusLine.className = 'error'; statusLine.textContent = `场景执行失败：${error.message}`; }

function shortAddress(input) { return input && input.length > 14 ? `${input.slice(0, 6)}…${input.slice(-4)}` : value(input); }
function renderLiveEvidence(evidence) {
  lastLiveEvidence = evidence;
  const status = evidence.status || 'NOT_RUN';
  const requirement = evidence.requirement && evidence.requirement.x402_version ? evidence.requirement : null;
  const protocolFlow = document.querySelector('#live-protocol-flow');
  const details = document.querySelector('#live-provider-details');
  const rawProvider = document.querySelector('#live-provider-raw');
  liveStatus.textContent = status; protocolFlow.hidden = !requirement;
  if (requirement) {
    renderRows(document.querySelector('#live-requirement-facts'), [
      {label:'Network', value:requirement.network === 'eip155:56' ? `BSC · ${requirement.network}` : requirement.network},
      {label:'Asset', value:requirement.extra ? requirement.extra.name : shortAddress(requirement.asset)},
      {label:'Amount', value:requirement.amount}, {label:'Scheme', value:requirement.scheme},
    ]);
    renderRows(document.querySelector('#live-requirement-full'), [
      {label:'x402 version', value:requirement.x402_version}, {label:'Asset address', value:requirement.asset, wide:true},
      {label:'payTo', value:requirement.pay_to, wide:true},
      {label:'extra.name', value:requirement.extra ? requirement.extra.name : null},
      {label:'extra.version', value:requirement.extra ? requirement.extra.version : null},
    ]);
    renderRows(document.querySelector('#live-wallet-preview-facts'), [
      {label:'Wallet', value:evidence.wallet_status, badge:true}, {label:'Provider', value:evidence.provider},
      {label:'Preview', value:evidence.provider_status, badge:true},
      {label:'Reason', value:(evidence.reasons || []).join(', ') || '—', wide:true},
      {label:'Duration', value:Number.isFinite(evidence.duration_ms) ? `${(evidence.duration_ms / 1000).toFixed(3)} s` : '—'},
    ]);
    renderRows(document.querySelector('#live-boundary-facts'), [
      {label:'Preview', value:evidence.preview_performed ? 'YES' : 'NO'}, {label:'Signable', value:evidence.payment_signable ? 'YES' : 'NO'},
      {label:'Signed', value:evidence.signed ? 'YES' : 'NO'}, {label:'Broadcast', value:evidence.broadcast ? 'YES' : 'NO'},
      {label:'Settlement', value:evidence.settlement_performed ? 'YES' : 'NO'}, {label:'Funds moved', value:evidence.funds_moved === undefined ? 0 : evidence.funds_moved},
      {label:'Service', value:evidence.service_unlocked ? 'UNLOCKED' : 'LOCKED'},
    ]);
  }
  const sanitized = {wallet_status:evidence.wallet_status, provider:evidence.provider, provider_status:evidence.provider_status, reasons:evidence.reasons, error_code:evidence.error_code, started_at_unix_ms:evidence.started_at_unix_ms, completed_at_unix_ms:evidence.completed_at_unix_ms, duration_ms:evidence.duration_ms};
  details.hidden = status === 'NOT_RUN' || status === 'RUNNING'; rawProvider.textContent = JSON.stringify(sanitized, null, 2);
  if (status === 'RUNNING') liveMessage.textContent = '正在调用 Binance Agentic Wallet preview…';
  else if (status === 'ERROR') liveMessage.textContent = `${evidence.error_code || 'ERROR'}：${evidence.error_message || '检查未完成。'}`;
  else if (status === 'COMPLETE') liveMessage.textContent = evidence.payment_signable ? 'Preview 已完成；未执行签名、广播或结算。' : 'Preview 已到达支付边界；未执行签名、广播或结算。';
  else liveMessage.textContent = '尚未执行 Wallet 检查。';
}
async function runLiveX402() {
  try {
    liveRunButton.disabled = true; modeButtons.forEach((button) => { button.disabled = true; });
    renderLiveEvidence({status:'RUNNING'});
    const response = await fetch('/api/live-x402', {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({action:'run'}), cache:'no-store'});
    const data = await response.json(); if (!response.ok || data.error) throw new Error(data.error || 'live x402 check failed');
    renderLiveEvidence(data);
  } catch (error) { renderLiveEvidence({status:'ERROR', error_code:'BRIDGE_ERROR', error_message:error.message}); }
  finally { liveRunButton.disabled = false; modeButtons.forEach((button) => { button.disabled = false; }); }
}
function selectMode(mode) {
  selectedMode = mode; window.clearTimeout(timer); timer = null;
  const live = mode === 'live-x402';
  modeButtons.forEach((item) => item.classList.toggle('mode-button--active', item.id === `mode-${mode}`));
  simulationControls.hidden = live; societyView.hidden = live; liveEvidence.hidden = !live;
  statusLine.className = ''; statusLine.textContent = live ? 'Live x402 Evidence 已就绪；仅在点击后执行 preview。' : 'Society Simulation 已就绪。';
  if (live) renderLiveEvidence(lastLiveEvidence);
  else if (lastSnapshot) render(lastSnapshot);
}

modeButtons.forEach((button) => button.addEventListener('click', () => selectMode(button.id === 'mode-live-x402' ? 'live-x402' : 'society')));
startButton.addEventListener('click', async () => {
  try { statusLine.className = ''; statusLine.textContent = '正在初始化模拟…'; const snapshot = await command('start', 'normal'); render(snapshot); statusLine.className = 'ok'; statusLine.textContent = '模拟已启动。'; scheduleSimulation(snapshot); }
  catch (error) { showError(error); }
});
stopButton.addEventListener('click', async () => {
  try { const snapshot = await command('stop'); render(snapshot); statusLine.textContent = '已请求停止；将在当前轮次的安全边界完成。'; scheduleSimulation(snapshot); }
  catch (error) { showError(error); }
});
replayButton.addEventListener('click', async () => {
  try { const snapshot = await command('replay-start'); render(snapshot); statusLine.className = 'ok'; statusLine.textContent = 'Deterministic Replay 已开始；不会调用外部服务。'; scheduleReplay(snapshot); }
  catch (error) { showError(error); }
});
liveRunButton.addEventListener('click', runLiveX402);
