# 그래프와 실제 게임 화면

[English](README.md) | [한국어](README.ko.md)

`en/`과 `ko/`는 같은 측정 결과의 영어판과 한국어판입니다. 마을 분위기에 맞춰 짙은 녹색과 구리색, 따뜻한 배경색을 사용했습니다. SVG의 글자는 경로로 변환해 보는 환경의 글꼴에 영향을 받지 않도록 했습니다. 언어별 `figures-manifest.json`에 렌더러, 원시 보고서와 이미지의 해시를 기록합니다.

소스 번들 루트에서 그래프를 다시 생성할 수 있습니다. 선택 사항이며 Matplotlib이 필요합니다.

```powershell
python tools/generate_figures.py --reports-dir reports --output-dir figures/en --language en --profile-batch reports/final-profile-batch.json
python tools/generate_figures.py --reports-dir reports --output-dir figures/ko --language ko --profile-batch reports/final-profile-batch.json
```

한국어 생성은 기본적으로 Windows의 맑은 고딕을 사용합니다. 다른 환경에서는 `--font`로 설치된 한국어 글꼴을 지정할 수 있습니다. 그래프 생성은 게임 복원 및 실행 검증과 별도입니다.

상태 막대는 같은 기대 동작 78개를 비교합니다. 확률 그래프의 오차 막대는 Wilson 95% 구간입니다. 부하 그래프의 오차 막대는 세 실행 중앙값의 최솟값과 최댓값이며 신뢰구간과 구분합니다. 월드 처리 구간과 에디터 프레임 간격도 서로 다른 측정값입니다.

`screenshots/`는 실제 PIE에서 캡처한 1280 × 720 게임, 대화와 거래 화면입니다. 원래 V 입력으로 1인칭 시점을 선택하고 촬영을 위해 시간과 위치를 고정했습니다. 원래 영어 UI를 포함한 실제 렌더링 결과를 그대로 사용했습니다. [화면 캡처 근거](../evidence/rendered-capture.json)와 overlay의 공개 `NpcVisualTests.cpp`에서 범위를 확인할 수 있습니다.
