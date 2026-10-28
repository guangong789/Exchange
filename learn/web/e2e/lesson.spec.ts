import { expect, test } from '@playwright/test';

test('explores actual candles and all three synchronized concepts', async ({ page }) => {
  const errors: string[] = [];
  page.on('pageerror', (error) => errors.push(error.message));
  await page.goto('/');
  await expect(page.getByRole('heading', { name: '认识一根 K 线。' })).toBeVisible();
  await expect(page.getByTestId('price-open')).toHaveText('108.00');
  await expect(page.getByTestId('price-high')).toHaveText('116.00');
  await expect(page.getByTestId('price-low')).toHaveText('106.00');
  await expect(page.getByTestId('price-close')).toHaveText('113.00');
  await expect(page.getByTestId('volume')).toContainText('2,860');
  await page.getByRole('button', { name: '阴线示例' }).click();
  await expect(page.getByTestId('candle-kind')).toContainText('阴线');
  await expect(page.getByTestId('price-open')).toHaveText('118.00');
  await expect(page.getByTestId('price-close')).toHaveText('112.00');
  await page.getByRole('button', { name: '十字线示例' }).click();
  await expect(page.getByTestId('candle-kind')).toContainText('十字线');
  await expect(page.getByTestId('price-open')).toHaveText('119.00');
  await expect(page.getByTestId('price-close')).toHaveText('119.00');
  await page.getByRole('button', { name: '实体 Body', exact: true }).click();
  await expect(page.getByRole('img', { name: '选中 K 线的实体' })).toBeVisible();
  await page.getByRole('button', { name: '影线 Wicks', exact: true }).click();
  await expect(page.getByRole('img', { name: '选中 K 线的影线' })).toBeVisible();
  await page.getByRole('button', { name: '四个价格 OHLC', exact: true }).click();
  await expect(page.getByRole('img', { name: '选中 K 线的四个价格' })).toBeVisible();
  await expect(page.getByTestId('market-chart').locator('canvas').first()).toBeVisible();
  expect(errors).toEqual([]);
});

test('selects candles with pointer and keyboard; updates text and quiz together', async ({
  page,
}) => {
  await page.goto('/');
  const slider = page.getByRole('slider', { name: '选择 K 线' });
  await slider.focus();
  await slider.press('ArrowRight');
  await expect(page.getByTestId('price-high')).toHaveText('118.00');
  await expect(page.getByRole('button', { name: 'C 118.00', exact: true })).toBeVisible();
  await slider.press('Home');
  await expect(page.getByRole('button', { name: '上一根 K 线' })).toBeDisabled();
  await slider.press('End');
  await expect(page.getByRole('button', { name: '下一根 K 线' })).toBeDisabled();
  await page.getByRole('button', { name: '阳线示例' }).click();
  const chart = page.getByTestId('market-chart');
  const box = await chart.boundingBox();
  if (!box) throw new Error('Chart has no layout');
  await chart.click({ position: { x: box.width * 0.68, y: box.height * 0.4 } });
  await expect(slider).not.toHaveValue('8');
});

test('retries, completes, persists, and resets the lesson', async ({ page }) => {
  await page.goto('/');
  await page.getByRole('button', { name: 'A 106.00', exact: true }).click();
  await expect(page.getByText('再看一眼最高的端点，试试看。')).toBeVisible();
  await page.getByRole('button', { name: '重新选择 ↻' }).click();
  await page.getByRole('button', { name: 'C 116.00', exact: true }).focus();
  await page.keyboard.press('Enter');
  await page.getByRole('button', { name: 'A 开盘价与收盘价', exact: true }).click();
  await expect(page.getByText('再看一眼实体上方的细线，试试看。')).toBeVisible();
  await page.getByRole('button', { name: '重新选择 ↻' }).click();
  await page.getByRole('button', { name: 'B 最高价与实体顶部', exact: true }).click();
  await expect(page.getByRole('status')).toContainText('第一课，完成！');
  await page.reload();
  await expect(page.getByRole('status')).toContainText('第一课，完成！');
  await page.getByRole('button', { name: '重新开始', exact: false }).click();
  await expect(page.getByRole('status')).not.toContainText('第一课，完成！');
  await expect(page.getByRole('button', { name: 'C 116.00', exact: true })).toBeEnabled();
  await page.reload();
  await expect(page.getByRole('status')).not.toContainText('第一课，完成！');
});

test('tolerates malformed and inaccessible local storage', async ({ page }) => {
  await page.addInitScript(() => {
    localStorage.setItem('exchange-learn:lesson-01:v1', '{broken');
  });
  await page.goto('/');
  await expect(page.getByRole('status')).not.toContainText('第一课，完成！');
  await page.addInitScript(() => {
    Object.defineProperty(window, 'localStorage', {
      get() {
        throw new Error('denied');
      },
    });
  });
  await page.reload();
  await page.getByRole('button', { name: 'C 116.00', exact: true }).click();
  await page.getByRole('button', { name: 'B 最高价与实体顶部', exact: true }).click();
  await expect(page.getByRole('status')).toContainText('第一课，完成！');
});

test('keeps layout in bounds, adapts on resize, and respects reduced motion', async ({ page }) => {
  await page.emulateMedia({ reducedMotion: 'reduce' });
  await page.goto('/');
  const inBounds = () =>
    page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth);
  expect(await inBounds()).toBe(true);
  await expect(page.getByTestId('market-chart').locator('canvas').first()).toBeVisible();
  await page.setViewportSize({ width: 320, height: 780 });
  await expect.poll(inBounds).toBe(true);
  await page.getByRole('button', { name: '实体 Body', exact: true }).focus();
  await page.keyboard.press('Enter');
  await expect(page.getByRole('button', { name: '实体 Body', exact: true })).toHaveAttribute(
    'aria-pressed',
    'true',
  );
  expect(
    await page
      .getByRole('button', { name: '实体 Body', exact: true })
      .evaluate((el) => getComputedStyle(el).transitionDuration),
  ).toBe('0s');
  await page.setViewportSize({ width: 1024, height: 768 });
  await expect.poll(inBounds).toBe(true);
});

test('loads without external requests and distributes chart attribution', async ({
  page,
  request,
}) => {
  const external: string[] = [];
  page.on('request', (req) => {
    if (!req.url().startsWith('http://127.0.0.1:4173/')) external.push(req.url());
  });
  await page.goto('/');
  await expect(page.getByTestId('market-chart').locator('canvas').first()).toBeVisible();
  expect(external).toEqual([]);
  const license = await request.get('/third-party/lightweight-charts-LICENSE.txt');
  expect(license.status()).toBe(200);
  expect(await license.text()).toContain('Apache License');
  const notice = await request.get('/third-party/lightweight-charts-NOTICE.txt');
  expect(notice.status()).toBe(200);
  expect(await notice.text()).toContain('Copyright (с) 2025 TradingView');
  await expect(page.getByRole('link', { name: 'TradingView ↗' })).toHaveAttribute(
    'href',
    'https://www.tradingview.com/',
  );
});

test('keeps the light typography loaded and price exploration accessible', async ({
  page,
  request,
}) => {
  await page.goto('/');
  await page.evaluate(() => document.fonts.ready);
  expect(
    await page.locator('.market-panel').evaluate((el) => getComputedStyle(el).backgroundColor),
  ).toBe('rgb(255, 255, 255)');
  expect(await page.evaluate(() => document.fonts.check('400 14px "Learn Sans"'))).toBe(true);
  const font = await request.get('/fonts/learn-sans-sc.woff2');
  expect(font.status()).toBe(200);
  expect((await font.body()).subarray(0, 4).toString()).toBe('wOF2');
  const license = await request.get('/fonts/OFL.txt');
  expect(license.status()).toBe(200);
  expect(await license.text()).toContain('SIL OPEN FONT LICENSE');
  const high = page.getByTestId('price-high').locator('..');
  await high.click();
  await expect(high).toHaveAttribute('aria-pressed', 'true');
  await expect(page.getByRole('img', { name: '选中 K 线的四个价格' })).toBeVisible();
  await high.click();
  await expect(high).toHaveAttribute('aria-pressed', 'false');
  await page.getByRole('button', { name: '影线 Wicks', exact: true }).click();
  await expect(page.getByRole('img', { name: '选中 K 线的影线' })).toBeVisible();
});

// Inspect our chart's rendered PNG, not the chart library's private DOM or state.
for (const ratio of [1, 1.25, 2]) {
  test(`matches candle and volume bitmap edges in desktop Chrome at DPR ${ratio}`, async ({
    browser,
  }) => {
    const context = await browser.newContext({
      viewport: { width: 1440, height: 1000 },
      deviceScaleFactor: ratio,
    });
    const page = await context.newPage();
    try {
      await page.goto('http://127.0.0.1:4173/');
      await page.evaluate(() => document.fonts.ready);
      for (const example of [
        { name: '阳线示例', color: [38, 104, 94] },
        { name: '阴线示例', color: [162, 83, 80] },
        { name: '十字线示例', color: [38, 104, 94] },
      ]) {
        await page.getByRole('button', { name: example.name }).click();
        await page.evaluate(
          () =>
            new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve))),
        );
        const png = await page.getByTestId('market-chart').screenshot();
        const groups = await page.evaluate(
          async ({ base64, color, ratio }) => {
            const bytes = Uint8Array.from(atob(base64), (char) => char.charCodeAt(0));
            const image = await createImageBitmap(new Blob([bytes], { type: 'image/png' }));
            const canvas = new OffscreenCanvas(image.width, image.height);
            const ctx = canvas.getContext('2d');
            if (!ctx) throw new Error('Missing image context');
            ctx.drawImage(image, 0, 0);
            const { data } = ctx.getImageData(0, 0, image.width, image.height);
            const result: { top: number; bottom: number; left: number; right: number }[] = [];
            for (let y = 0; y < image.height; y++) {
              let left = image.width;
              let right = -1;
              for (let x = 0; x < image.width; x++) {
                const i = (y * image.width + x) * 4;
                if (data[i] === color[0] && data[i + 1] === color[1] && data[i + 2] === color[2]) {
                  left = Math.min(left, x);
                  right = Math.max(right, x);
                }
              }
              if (right < 0) continue;
              const last = result[result.length - 1];
              if (!last || y > last.bottom + 1) result.push({ top: y, bottom: y, left, right });
              else {
                last.bottom = y;
                last.left = Math.min(last.left, left);
                last.right = Math.max(last.right, right);
              }
            }
            const anchor = result[result.length - 1];
            if (!anchor) throw new Error('Selected volume is not visible');
            // Thin dojis can be composited entirely as partial-coverage pixels.
            // Isolate the selected column using its solid volume, then include
            // pixels on the same foreground/background blend (not private DOM).
            const coverage: typeof result = [];
            const background = [238, 243, 242];
            const start = Math.max(0, anchor.left - Math.ceil(2 * ratio));
            const end = Math.min(image.width - 1, anchor.right + Math.ceil(2 * ratio));
            const width = end - start + 1;
            const mask = new Uint8Array(width * image.height);
            for (let y = 0; y < image.height; y++) {
              for (let x = start; x <= end; x++) {
                const i = (y * image.width + x) * 4;
                const alpha = color.map(
                  (c, channel) =>
                    (background[channel] - data[i + channel]) / (background[channel] - c),
                );
                if (
                  Math.min(...alpha) > 0.2 &&
                  Math.max(...alpha) <= 1.03 &&
                  Math.max(...alpha) - Math.min(...alpha) < 0.025
                ) {
                  mask[y * width + x - start] = 1;
                }
              }
            }
            // With tightly packed bars, nearby antialiased edges can share the
            // same blend. Measure connected shapes crossing the selected center,
            // rather than merging disconnected neighboring pixels by row.
            const center = (anchor.left + anchor.right) / 2;
            for (let seed = 0; seed < mask.length; seed++) {
              if (!mask[seed]) continue;
              const shape = { top: image.height, bottom: -1, left: end, right: start };
              const queue = [seed];
              mask[seed] = 0;
              for (let head = 0; head < queue.length; head++) {
                const point = queue[head];
                const x = point % width;
                const y = Math.floor(point / width);
                shape.top = Math.min(shape.top, y);
                shape.bottom = Math.max(shape.bottom, y);
                shape.left = Math.min(shape.left, x + start);
                shape.right = Math.max(shape.right, x + start);
                const neighbors = [point - width, point + width];
                if (x > 0) neighbors.push(point - 1);
                if (x + 1 < width) neighbors.push(point + 1);
                for (const neighbor of neighbors) {
                  if (neighbor >= 0 && neighbor < mask.length && mask[neighbor]) {
                    mask[neighbor] = 0;
                    queue.push(neighbor);
                  }
                }
              }
              if (shape.left <= center && shape.right >= center) coverage.push(shape);
            }
            image.close();
            return coverage;
          },
          { base64: png.toString('base64'), color: example.color, ratio },
        );
        expect(groups, example.name).toHaveLength(2);
        // Browser compositing at fractional device coordinates can add a
        // one-bitmap-pixel fringe; the shared drawing geometry is exact.
        const fringe = ratio === 1 ? 0 : 1;
        expect(Math.abs(groups[1].left - groups[0].left), example.name).toBeLessThanOrEqual(fringe);
        expect(Math.abs(groups[1].right - groups[0].right), example.name).toBeLessThanOrEqual(
          fringe,
        );
        expect(groups[0].right - groups[0].left + 1).toBeGreaterThanOrEqual(3 * ratio);
      }
    } finally {
      await context.close();
    }
  });
}
