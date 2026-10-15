# 블루프린트 구현 검토

[English](blueprint-inspection.md) | [한국어](blueprint-inspection.ko.md)

[검토 자료](../inspections/README.ko.md)는 바이너리 언리얼 에셋의 구현을 에디터 밖에서도 읽을 수 있도록 정리한 자료입니다. Unreal Engine 5.4.4에서 읽은 실제 노드 GUID, 함수 참조, 핀 타입, 기본값과 연결을 담았습니다. 개인 작업 일지나 NPC 규칙을 별도로 재구현한 코드가 아닙니다.

전체 게임 복원은 [재현 안내](reproduce.ko.md), 실행한 검증의 범위는 [결과 문서](results.ko.md)를 확인하세요. 그래프 자료는 무엇을 검토했는지 보여주며, 실제 동작을 실행했는지는 PIE 보고서로 구분합니다.

## 파일 구성과 출처

| 자료 | 확인할 내용 |
| --- | --- |
| [목록과 해시](../inspections/index.json) | 공개 파일과 원본 내보내기의 해시, 자료 단계와 이름이 다른 중복 파일입니다. |
| [초기 NPC 그래프](../inspections/blueprints/initial/BP_NPC_Standard.json) | 초기 상호작용, 대화, 일정과 추종 구현입니다. |
| [수정 후 NPC 그래프](../inspections/blueprints/maintained/BP_NPC_Standard.json) | 대화 우선순위와 상호작용 중 귀가 지시를 수정한 그래프입니다. |
| [실행 연결 비교](../inspections/blueprints/npc-execution-changes.json) | 추가하거나 제거한 실행 연결을 노드 GUID와 핀 이름으로 확인합니다. |
| [플레이어 그래프](../inspections/blueprints/initial/BP_ThirdPersonCharacter.json) | 입력 연결, 대상 선택과 스태미나 처리입니다. |
| [아이템 그래프](../inspections/blueprints/initial/BP_Item_Master.json) | 희귀도 선택, 겹침으로 활성화되는 E 입력, 인벤토리 갱신과 재등장 처리입니다. |
| [상점 그래프](../inspections/blueprints/initial/WBP_ShopMain.json) | 실제 상품 목록 구성과 화면 표시 조건입니다. |
| [BT task 설정](../inspections/behavior-tree/BT_NPC-task-settings.json) | 클래스 기본값과 구분한 실제 배치 task 설정입니다. |
| [나비 에셋 비교](../inspections/vendor/butterfly-construction-change.json) | 초기와 수정 후 생성 스크립트 및 보존 확인 범위입니다. |

초기 폴더에는 처음 내보낸 이름의 파일 18개를 모두 보존합니다. 실제 해석된 블루프린트는 16개입니다. `BP_Mushroom`은 `BP_Item_Master`, `WBP_DialogueEnd`는 `WBP_Dialogue`로 해석되어 같은 결과가 초기 이름으로도 남아 있습니다. 선택한 핵심 그래프의 자료이며 프로젝트와 외부 콘텐츠의 모든 블루프린트를 포함한다는 뜻은 아닙니다. 별도로 내보낸 초기 나비 그래프는 수정 전후 폴더로 구분합니다.

복원에 사용하는 초기 전체 프로젝트 압축파일의 식별값은 다음과 같습니다.

```text
파일명:  Project.zip
바이트:  2257866122
SHA-256: ba8e4ba770461e5272ff6cfaeaf4be9fe0219ec3fa630a404572c627e8080bfe
```

압축파일은 [재현 안내](reproduce.ko.md)를 따라 별도로 받습니다. 릴리즈의 `manifests/original-project.json`과 `manifests/final-project.json`이 초기와 최종 소스의 파일 식별값을 기록합니다. 초기 기준은 이 압축파일에서 복원한 프로젝트입니다. 검토 JSON의 해시는 별도 목록에 기록합니다. 그래프 JSON은 실행 가능한 `.uasset`이나 전체 압축파일을 대체하지 않습니다.

초기 내보내기 18개와 수정 후 그래프 2개는 바이트 단위로 복사했습니다. 로컬 경로를 가린 경우 목록에 별도로 기록하며, 이번 자료에는 치환이 필요하지 않았습니다. `/Game/`, `/Script/`는 에셋과 엔진 타입의 식별 경로이므로 유지합니다. 결과의 `asset_saved: false`는 내보내기 작업 자체가 에셋을 저장하지 않았다는 뜻입니다. 별도 유지보수 절차에서 수정한 에셋을 저장하지 않았다는 뜻은 아닙니다.

## 그래프 자료 읽는 방법

`graphs[].name`에서 그래프를 고르고 `nodes[].guid`나 `title`로 노드를 찾습니다. 각 핀에는 방향, 타입, 직접 지정한 기본값과 연결된 노드 및 핀 정보가 있습니다. 함수 호출은 제공되는 `function_reference`로 확인합니다. 출력 실행 핀을 따라 제어 흐름을 읽고, 조건과 값은 연결된 데이터 핀까지 확인해야 합니다.

연결 요약은 안정적인 노드 GUID와 핀 이름으로 실행 연결을 비교합니다. 모든 데이터 연결, 좌표와 직렬화된 핀 ID까지 비교하는 자료는 아닙니다. 해당 내용은 전체 내보내기 결과로 검토할 수 있습니다. `complete: true`와 잘못된 연결 수 0은 내보내기의 완전성에 관한 값이며 모든 게임 동작의 정상 작동을 뜻하지 않습니다.

## 초기 데이터 테이블

| 테이블 | 행 수 | 내용 |
| --- | ---: | --- |
| [DT_NPC_Data](../inspections/tables/initial/DT_NPC_Data.json) | 5 | 이름, 일정, 집 위치, 초기 신뢰도와 추종 조건입니다. |
| [DT_Item_Data](../inspections/tables/initial/DT_Item_Data.json) | 38 | 아이템 종류, 수량, 가격, 효과, 신뢰도 조건과 표현 에셋 참조입니다. |
| [DT_QuestList](../inspections/tables/initial/DT_QuestList.json) | 15 | 수집 또는 골드 조건과 정해진 보상입니다. |

추출본은 `NSLOCTEXT` 같은 언리얼 내보내기 문자열까지 원래 행 값을 유지합니다. 이는 직렬화된 텍스트 표현이며 깨진 마크다운이 아닙니다. 각 파일에 초기 에셋과 해시를 기록했습니다. 초기 압축파일의 해당 멤버를 직접 읽어 테이블 3개와 현재 소스의 바이트 해시가 일치하는지도 확인했습니다. 이 자료를 만들면서 테이블이나 기본값은 수정하지 않았습니다.

| NPC | 활동 시작 분 | 활동 종료 분 | 추종 신뢰도 조건 |
| --- | ---: | ---: | ---: |
| Charles | 420 | 1140 | 80 |
| George | 330 | 1050 | 40 |
| Leon | 390 | 1110 | 60 |
| Lucia | 0 | 720 | 60 |
| Rose | 420 | 1140 | 20 |

활동 구간은 `Start_Time <= CurrentTime < End_Time`입니다. 시계는 1,440분을 기준으로 나머지를 계산하며 경계를 넘은 소수 부분도 유지합니다. 5개 행은 공통 구현에 다른 설정을 제공합니다. George의 초기 신뢰도는 39입니다.

## 설명과 구현을 구분해야 하는 부분

**대상 선택 방향.** `SendInteractionSignal`은 실제 겹침 범위의 NPC 가운데 플레이어 Actor의 전방 방향과 `GetDotProductTo` 값으로 대상을 고릅니다. 카메라 전방 벡터를 사용하는 구현은 아닙니다. 정확히 같은 점수의 우선순위나 별도의 전방 각도 제한도 보장된 조건으로 추가하지 않습니다.

**스태미나.** 상태에 따른 대화는 `Current_Stamina <= 30`을 확인합니다. 별도 체력 변수가 아닙니다. 이동 속도 감소는 20 미만이므로 20부터 30까지는 평소 이동 속도와 피로 대화가 함께 나타날 수 있습니다.

**아이템 수명 주기.** 원래 아이템 Actor의 E 입력은 플레이어와의 겹침으로 활성화됩니다. 획득에 성공하면 인벤토리 수량을 추가하거나 늘리고 Actor를 숨기며 충돌을 끕니다. 원래 60초 Delay가 끝나면 다시 표시하고 충돌을 켜며 희귀도를 다시 결정합니다. 획득 시 Actor를 파괴하는 구조가 아닙니다.

**상점 표시 조건.** Charles의 `ItemsToSell`에는 Juice, Beer, Whiskey, Titan Blood가 있습니다. `NPC_SalesList`는 상품 4개의 원본 목록을 유지하고, `WBP_ShopMain.UpdateShopUI`가 `Required_Trust`로 화면의 `NPC_ShopList` 행을 거릅니다. 신뢰도 99에서는 일반 상품 3개, 100과 101에서는 Titan Blood까지 표시합니다. 신뢰도 99에서 원본 목록이 4행인 것은 표시 조건과 모순되지 않습니다.

## 클래스 기본값과 배치 task 설정

[BT 설정 추출본](../inspections/behavior-tree/BT_NPC-task-settings.json)은 task 객체 경로, 클래스, 상위 composite와 매개변수를 기록합니다.

| 설정 | 블루프린트 클래스 기본값 | BT_NPC에 배치된 task 값 |
| --- | ---: | ---: |
| `BTTask_ChasePlayer.ChaseSpeed` | 300 | 150 |
| `BTTask_PatrolRandom.Patrol Speed` | 150 | 150 |
| `BTTask_PatrolRandom.PatrolRadius` | 1500 | 1500 |

기존 `Update Walk Speed(NewSpeed)`는 인자로 받은 값을 이동 속도에 대입합니다. 함수 자체가 `IsFollowing`을 보고 300과 150을 고르는 것은 아닙니다. 추종 task는 배치 인스턴스의 설정을 전달하므로 클래스 기본값이 300이어도 실제 트리는 속도 150으로 추종할 수 있습니다.

초기 이동 검증은 클래스 기본값을 추종 속도의 기대값으로 사용했습니다. 실제 이동과 task 객체를 함께 확인하면서 검증 기준의 오류를 찾았습니다. 수정한 테스트는 `BT_NPC.RootNode`와 `Children.ChildTask`를 순회해 기대값을 읽고 실제 Brain의 `NodeInstances` 설정과 대조합니다. 이동거리, 경로와 시간 조건은 별도로 유지합니다. 속도를 바꾸는 게임 기능 패치는 적용하지 않았으며, 초기와 유지보수 후 `BT_NPC.uasset`의 해시도 일치합니다.

## 유지한 그래프 수정

초기 Talk 경로는 진행 중인 퀘스트가 없을 때 새 퀘스트 확률 검사를 일반 스태미나 및 골드 검사보다 먼저 실행했습니다. 수정 후에는 낮은 스태미나, 골드 부족, 원래의 새 퀘스트 확률 검사 순으로 확인합니다. 진행 중인 퀘스트 처리는 가장 먼저 수행하며 기존 조건과 보상을 유지합니다.

일정 처리는 근무 시간 밖에서 귀가 의도를 설정하기 전에 `IsInteracting` 분기 하나를 추가했습니다. NPC 활동 시간과 뒤쪽 이동 흐름을 유지하며 상호작용 중 새로운 귀가 지시를 막습니다. 이미 발행된 모든 이동 요청까지 취소한다고 주장하지 않습니다. [실행 연결 비교](../inspections/blueprints/npc-execution-changes.json)에 제거한 연결 4개, 추가한 연결 5개, 새 분기 노드 `15C20261-0C15-4F2A-AB80-1D0180000001`을 기록합니다.

[나비 에셋 비교](../inspections/vendor/butterfly-construction-change.json)는 생성 스크립트의 `PlayAnimation`, `SetPlayRate`를 생성 시점에 사용할 수 있는 `USkeletalMeshComponent::OverrideAnimationData`로 교체한 내용입니다. 원래 애니메이션 선택과 재생 속도 식을 유지했고 변경 대상 밖 그래프의 비교 해시도 전후가 같습니다. 기록된 메모리 내 컴파일은 오류 0개, 경고 0개입니다. 실제 화면과 실행 중 애니메이션은 별도 엔진 검증으로 확인해야 합니다.

검토 자료는 유지한 소스와 매니페스트에 함께 제공됩니다. 실제 이동, 입력, UI와 동작 범위는 에디터와 제공된 검증 도구로 확인할 수 있습니다.
