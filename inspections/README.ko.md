# 블루프린트 검토 자료

[English](README.md) | [한국어](README.ko.md)

이 자료는 바이너리 언리얼 에셋의 실제 그래프 노드, 핀, 함수 참조, 기본값과 연결을 확인하기 위한 엔진 내보내기 결과입니다. 게임을 별도 규칙으로 재구현한 코드나 개인 개발 일지가 아니며, 자료 자체가 게임을 실행하지는 않습니다.

[검토 안내](../docs/blueprint-inspection.ko.md), [파일 해시와 출처](index.json), [NPC 실행 연결 비교](blueprints/npc-execution-changes.json)부터 확인하세요.

| 폴더 | 내용 |
| --- | --- |
| `blueprints/initial/` | 초기 이름으로 내보낸 그래프 파일 18개입니다. 실제 해석된 블루프린트는 16개입니다. |
| `blueprints/maintained/` | 대화 우선순위와 상호작용 중 귀가 지시를 수정한 NPC 그래프입니다. |
| `tables/initial/` | 초기 NPC, 아이템, 퀘스트 테이블과 해당 에셋의 해시입니다. |
| `behavior-tree/` | 실제 BT_NPC 배치 task의 속도 설정과 별도로 구분한 클래스 기본값입니다. |
| `vendor/initial/`, `vendor/maintained/` | 나비 에셋의 초기 그래프와 호환성 수정 후 그래프입니다. |

`BP_Mushroom.json`은 `BP_Item_Master`, `WBP_DialogueEnd.json`은 `WBP_Dialogue`로 해석된 결과입니다. 초기 파일명을 보존한 중복 자료이며 서로 다른 알고리즘으로 세지 않습니다.

원본 내보내기 파일은 로컬 파일 경로를 가려야 하는 경우를 제외하고 바이트 단위로 복사합니다. `index.json`에 원본과 공개 사본의 해시 및 경로 치환 여부를 기록합니다. `/Game/`, `/Script/`는 에셋과 엔진 타입을 식별하므로 유지합니다. 실행 가능한 프로젝트의 복원 기준은 전체 초기 압축파일과 최종 소스 매니페스트이며, 그래프 JSON으로 `.uasset`을 대체할 수는 없습니다.

초기 기준은 식별값을 고정한 `Project.zip`에서 복원한 프로젝트입니다.
