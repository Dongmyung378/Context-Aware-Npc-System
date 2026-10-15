# Figures and actual game captures

[English](README.md) | [한국어](README.ko.md)

`en/` and `ko/` contain the same reported results in two languages. Forest green, copper and a warm background reflect the village environment. SVG text is exported as paths for portable viewing. Each language's `figures-manifest.json` records the renderer, source-report hashes and artifact hashes.

From the source bundle root, optional figure regeneration requires Matplotlib:

```powershell
python tools/generate_figures.py --reports-dir reports --output-dir figures/en --language en --profile-batch reports/final-profile-batch.json
python tools/generate_figures.py --reports-dir reports --output-dir figures/ko --language ko --profile-batch reports/final-profile-batch.json
```

Korean generation uses Windows Malgun Gothic by default. Supply `--font` with a suitable installed Korean font on another system. Figure generation is separate from restoring and testing the game.

State bars compare the same 78 expectations. Probability whiskers are Wilson 95% intervals. Profile whiskers are the minimum and maximum of three run medians, rather than confidence intervals. The world processing span and editor interval describe different measurements.

`screenshots/` contains actual 1280 × 720 PIE captures of gameplay, dialogue and trade. The original V input selects first-person view; game time and position are fixed for capture. These are rendered game images, with no image-generation or image-editing step. See the [render receipt](../evidence/rendered-capture.json) and the public `NpcVisualTests.cpp` in the overlay for their scope.
