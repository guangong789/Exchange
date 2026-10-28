/** Both time-aligned series use the same CSS width and bitmap rounding rules. */
export function barGeometry(x: number, spacing: number, ratio: number) {
  const center = Math.round(x * ratio);
  // Fill each time slot, leaving only a small, device-pixel-aligned gap.
  // A fixed body-width cap makes sparse lesson data look like isolated bars.
  const gap = Math.max(1, Math.round(1.25 * ratio));
  const width = Math.max(1, Math.floor(spacing * ratio) - gap);
  return { center, width, left: center - Math.floor(width / 2) };
}

export function barColor(falling: boolean, selected: boolean, volume = false): string {
  if (selected) return falling ? '#a25350' : '#26685e';
  if (volume) return falling ? '#d9aaa4' : '#9abdaf';
  return falling ? '#be8580' : '#6b9b8e';
}

export function selectionGeometry(x: number, spacing: number, ratio: number) {
  const center = Math.round(x * ratio);
  const width = Math.max(1, Math.round(spacing * ratio));
  return {
    left: center - Math.floor(width / 2),
    width,
  };
}
