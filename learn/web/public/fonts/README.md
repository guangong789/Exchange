# Learn Sans SC

Self-hosted variable WOFF2 subset of Noto Sans SC, distributed under the SIL Open
Font License 1.1. See [OFL.txt](./OFL.txt) for the full copyright and license.
The derivative's primary family name is **Learn Sans SC**. No font is requested
from Google or a CDN at runtime.

Upstream: https://github.com/google/fonts/tree/main/ofl/notosanssc

Source TTF: https://raw.githubusercontent.com/google/fonts/main/ofl/notosanssc/NotoSansSC%5Bwght%5D.ttf

Source TTF SHA-256:
`a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da`

This subset includes printable ASCII, the current Lesson 01 source text, and
Chinese calendar labels. The weight axis is retained. It is a 100,512-byte asset,
not a general-purpose full Chinese font. Uncovered glyphs use the system fallback.

Asset creation used FontTools `subset.Subsetter`, with `name_IDs=['*']`,
`name_legacy=True`, `name_languages=['*']`, and `layout_features=['*']`.
Glyph input was the concatenation of printable ASCII (U+0020–U+007E),
`年月日星期一二三四五六天`, and `src/*.ts` / `src/*.tsx` text. Name records
1/4/16 were renamed `Learn Sans SC`, 6/25 `LearnSansSC`, and 3
`ExchangeLearn-NotoSC-Subset-v1`. Copyright and license metadata are preserved.

When changing lesson copy, regenerate or expand the subset as needed. FontTools
and Brotli are asset-maintenance tools only; building and running the application
needs neither Python nor a font download.
