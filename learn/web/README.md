# Exchange Learn · Lesson 01

认识一根 K 线：一个可以点选、拆解和答题的中文图表实验室。

这是独立的浏览器应用。无需 CMake、C++ 编译器、账号、后端、行情服务或 API key。

## 开始

需要 Node.js `20.19+`（20.x）或 `22.12+`，以及 npm。

```bash
cd learn/web
npm ci
npm run dev
```

打开 **http://127.0.0.1:5173/**。端口占用时命令会报错，不会悄悄换端口。

```bash
npm run build
npm run preview
```

生产预览为 **http://127.0.0.1:4173/**。`dist/` 可以由普通静态服务器提供。

## 试一遍

1. 点击图表中的一根蜡烛，或使用图表下方滑块、左右按钮。滑块支持方向键和 Home/End。
2. 查看四个价格与成交量。点击某个价格，可以单独突出它；再次点击取消突出。
3. 切换 OHLC / Body / Wicks，对照同步 SVG 结构图。
4. 点击阳线、阴线、十字线示例，观察开盘与收盘的关系。
5. 回答下方两道题。第一题随选中蜡烛更新；答错后可以重新选择。
6. 两题答对即完成。本地存储可用时，刷新会保留进度；“重新开始”重置进度和界面。

## 实现边界

- React + TypeScript：少量组件与局部状态；没有全局状态库或路由依赖。
- Vite：独立开发与静态构建；Vitest 测试纯数据/逻辑；Biome 检查与格式化；Playwright 检查浏览器流程。
- Lightweight Charts：真实数据蜡烛图和成交量位于同一图表的两个独立窗格，共用时间轴。`SlenderCandles.ts` 与 `MatchedVolume.ts` 使用公开 custom-series 接口；`chartGeometry.ts` 统一宽度（填满时间槽，仅保留约 1–2 CSS px 间隙）、位置取整和选中配色。保留库提供的坐标轴、适配与点击事件，不操作私有 DOM，也不修改 OHLCV。
- SVG companion：同步呈现选中蜡烛的实体、影线与价格标签；结构来自同一条 OHLCV 数据。
- `src/lesson.ts`：24 根固定日线、概念文案、形态分类与确定性题目。
- `src/progress.ts`：版本化本地进度；无效 JSON、未知版本、存储拒绝和容量异常均安全退回内存内操作。
- `src/App.tsx`：课程组合、选择与完成状态；`CandleChart.tsx`、`CandleDiagram.tsx`、`QuizCard.tsx` 分担图表、结构与答题交互。

图表是合成的教学价格观测，**不是订单簿流动性**。没有订单、资金账户、撮合或收益模拟，不能用 high/low 推断限价单成交。没有调用任何外部数据或 AI 服务。

后续 C++ execution evidence 应经单独 adapter 进入教学层。当前 `Candle` 是观测数据，不应被扩展为隐含成交规则，也不应由前端改写执行层权威状态。

进度键为 `exchange-learn:lesson-01:v1`，只保存已掌握题目，不保存个人资料。存储不可用时，本次页面仍可完成，但刷新后不保证恢复。课程完成不要求先依次点击三个概念。

## 验证

```bash
npm run lint
npm run typecheck
npm test
npm run build
```

浏览器测试会启动生产预览，因此先执行 build。**从 Sprint 1.6 起，本轮及后续 UI 精修只以桌面 Chrome 为正式视觉 QA 目标。** 配置仅保留 desktop 项目，不运行移动浏览器。

本机已安装 Google Chrome 时，可以直接运行 `npm run test:browser`（使用 Playwright 的 `chrome` channel）。也可指定 Chrome / Chrome for Testing 可执行文件：

```bash
PLAYWRIGHT_CHROMIUM_EXECUTABLE_PATH=/absolute/path/to/chrome npm run test:browser
```

测试覆盖选择、三种概念、题目重试、完成/刷新/重置、存储异常、键盘和 reduced motion。响应式布局的窗口缩放检查也在桌面 Chrome 内执行，不代表移动浏览器 QA。截图像素检查在 1×、1.25×、2× 像素比下比较阳线、阴线、十字线与其成交量柱的水平边缘。共享绘制坐标与宽度严格相同；Chrome 合成极细线时可能出现一 bitmap pixel 的抗锯齿边缘差异，截图检查对此保留限定容差。失败 trace 保存在忽略的 `test-results/`。

自动化配置以专用端口 4173 启动自己的服务器，运行测试前请停止占用该端口的 preview。字体随静态资源提供，不依赖外部字体服务或 CDN。

## 视觉系统与 Sprint 1.6 调整

- 浅中性背景上的图表主模块、独立解剖面板、下一阶段小测模块。数据栏在主图底部单独分区；小测的进度与完成反馈收进同一模块。依靠模块边界、内部对齐和间距组织层级，不通过缩小文字制造留白。
- 中文优先的 sans-serif 字体、400/500 字重、等宽数字特性。自托管 Noto Sans SC 字体子集约 100 KB，中文与拉丁字符共用一套字形，保留系统 fallback。
- 主图烛体随时间槽宽度变化，不再使用固定 12px 上限，相邻烛体只留约 1–2px 的细缝；影线仍约 1 CSS px。量柱与烛体使用同一宽度、中心和取整规则；选中时上下两窗格同时加深，背景列、右侧日期和 OHLCV 同步响应。完整 OHLCV 放在图外；仅点选某个价格时出现一条辅助线，不向轴堆叠四个价格标签。
- 解剖图采用细实体和引导线，近邻价格标签独立排布，避免最高/收盘等标签靠得过近。OHLC、实体和影线仍来自同一条数据。
- 配色与字号分别为主文字、辅助信息和交互状态设定；保留 focus、文本形态标签、键盘操作和 reduced motion。

字体来源、字符覆盖和子集制作说明在 `public/fonts/README.md`，完整 SIL OFL 1.1 许可在 `public/fonts/OFL.txt`。字体子集覆盖当前课程；以后新增中文文案时可能需要扩充，未覆盖字形会回退至系统字体。

## 第三方署名

图表使用 [TradingView Lightweight Charts™](https://github.com/tradingview/lightweight-charts) 5.2.1，Apache-2.0。

上游 NOTICE：

```text
TradingView Lightweight Charts™
Copyright (с) 2025 TradingView, Inc. https://www.tradingview.com/
```

页面底部保留完整 NOTICE、TradingView 链接和公开许可证入口。按库公开选项关闭可选的图内 attribution logo，避免遮挡早期日期的成交量柱；不移除页面署名。完整 LICENSE 和 NOTICE 位于 `public/third-party/`，构建时一起复制到 `dist/`。LICENSE 来自安装包；NOTICE 来自对应 `v5.2.1` tag。

这说明图表依赖的许可，不为整个 Exchange 仓库或新增项目代码选择许可证。
