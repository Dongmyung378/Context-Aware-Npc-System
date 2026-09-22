# Unreal 에셋 안내

[English](README.md) | [한국어](README.ko.md) | [프로젝트 소개](../README.ko.md)

이 디렉터리는 제출용 프로젝트 패키지에 포함된 Unreal Engine 5.4.4 핵심 에셋을 공개합니다. 포트폴리오 저장소에서 구현 시스템과 명명 구조를 확인할 수 있도록 제공하는 자료이며, 단독으로 실행할 수 있는 프로젝트는 아닙니다.

## 에셋 분류

| 분류 | 대표 에셋 | 역할 |
| --- | --- | --- |
| 캐릭터 | `BP_NPC_Standard`, `BP_ThirdPersonCharacter` | NPC 행동과 플레이어 상호작용 상태 |
| AI | `BB_NPC`, `BT_NPC`, `BTTask_*`, `BTD_RandomChance`, `My_AIController` | Blackboard 상태, 순찰·동행 태스크, 행동 선택 |
| 게임플레이 | `BP_GameMode`, `BPI_Interaction`, `BP_Item_Master`, `BP_Mushroom` | 공통 상호작용, 아이템, 게임 상태 로직 |
| 데이터 | `DT_*`, `ST_*`, `E_*` | NPC, 대화, 퀘스트, 아이템 정의 |
| UI | `WBP_*` | 대화, 인벤토리, 상점, 아이템 획득, HUD 위젯 |

## 참고 사항

- 제출용 핵심 에셋 압축 파일의 원래 디렉터리 구조를 유지했습니다.
- 바이너리 `.uasset` 파일을 열려면 Unreal Engine이 필요하며, 전체 프로젝트의 경로나 다른 에셋에 의존할 수 있습니다.
- 실행 가능한 버전은 [전체 프로젝트 다운로드](https://drive.google.com/file/d/1qsdGAzKU3myKQVk04MfW-EngDHKKAqmW/view?usp=sharing)를 이용해 주세요.
- 동일한 패키지의 압축 파일은 [`downloads/npc-core-assets.zip`](../downloads/npc-core-assets.zip)에서 확인할 수 있습니다.
