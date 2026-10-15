# 소스 출처와 원본 보존

[English](source-provenance.md) | [한국어](source-provenance.ko.md)

초기 프로젝트는 해시로 고정한 전체 압축파일을 기준으로 식별합니다. 현재 실행용 프로젝트는 이 원본에 명시된 소스 오버레이를 적용해 복원합니다. 초기 검토용 에셋, 수정한 실행용 에셋, 실제 실행 결과는 각각 역할을 구분해 보존합니다.

## 초기 프로젝트의 기준

| 항목 | 확인한 값 |
| --- | --- |
| 원본 전체 압축파일 | [Google Drive의 Project.zip](https://drive.google.com/file/d/1qsdGAzKU3myKQVk04MfW-EngDHKKAqmW/view?usp=sharing) |
| 압축파일 크기 | 2,257,866,122바이트 |
| 압축파일 SHA-256 | `ba8e4ba770461e5272ff6cfaeaf4be9fe0219ec3fa630a404572c627e8080bfe` |
| 복원 대상으로 지정한 초기 소스 파일 | 2,349개 |
| 초기 `Content/` 파일 | 2,334개 |
| 초기 저장소 커밋 | `a3a518e62ac576dad6b9f586f0aa4fbb21b671ac` |

초기 전체 프로젝트의 기준은 이 압축파일입니다. 해시가 고정되지 않은 로컬 프로젝트 폴더를 초기 기준으로 대신 사용하지 않습니다. 생성된 바이너리, 에디터 캐시, 저장된 세션 상태는 복원 대상 소스에 포함하지 않습니다. 복원 도구는 압축파일의 크기나 해시가 다르면 처리를 중단합니다.

공개된 초기 핵심 에셋 30개를 전체 원본 압축파일 및 보존된 핵심 에셋 ZIP과 대조한 결과, **30개 모두 일치**했습니다. 작은 핵심 에셋 묶음은 소스 검토용입니다. 전체 마을 맵을 복원하고 실행하는 데 필요한 모든 파일이 들어 있지는 않습니다.

## 보존한 초기 자료

다음 링크는 초기 저장소 커밋을 가리킵니다. 따라서 당시 자료와 현재 수정한 소스를 구분해 확인할 수 있습니다.

| 자료 | 용도 |
| --- | --- |
| [초기 프로젝트 보고서](https://github.com/Dongmyung378/Context-Aware-Npc-System/blob/a3a518e62ac576dad6b9f586f0aa4fbb21b671ac/docs/project-report.pdf) | 당시 설계, 실험 및 한계 |
| [초기 발표 자료](https://github.com/Dongmyung378/Context-Aware-Npc-System/blob/a3a518e62ac576dad6b9f586f0aa4fbb21b671ac/docs/project-presentation.pptx) | 당시 프로젝트 발표 |
| [초기 핵심 에셋 ZIP](https://github.com/Dongmyung378/Context-Aware-Npc-System/blob/a3a518e62ac576dad6b9f586f0aa4fbb21b671ac/downloads/npc-core-assets.zip) | 초기 에셋 30개를 보존한 검토용 묶음 |
| [초기 핵심 에셋 디렉터리](https://github.com/Dongmyung378/Context-Aware-Npc-System/tree/a3a518e62ac576dad6b9f586f0aa4fbb21b671ac/unreal-assets) | 개별 초기 에셋 검토 |

원본 저장소에서 이 자료들을 보존합니다. 경량 소스 묶음은 보고서, 발표 자료와 초기 핵심 에셋 ZIP을 중복해서 넣는 대신 원본 링크를 제공합니다. `unreal-assets` 자료는 초기 소스 검토에 사용하고, 현재 버전의 실행에는 전체 원본 압축파일과 수정 오버레이를 사용합니다.

## 현재 소스에 적용하는 오버레이

소스 묶음 버전 `1`은 **교체 7개와 추가 22개, 총 29개 파일**을 명시합니다. 초기 소스 2,349개에 적용하면 **최종 소스는 2,371개**가 됩니다. 추가 파일은 에디터 전용 `NpcAudit` 플러그인과 검증 소스입니다. 초기 소스 파일을 삭제하지 않습니다.

| 교체하는 소스 | 수정 범위 |
| --- | --- |
| `Content/ThirdPerson/Blueprints/BP_NPC_Standard.uasset` | 중요한 일반 대사의 우선순위와 상호작용 중 새 귀가 명령을 제한하는 조건 |
| `Content/AdvancedVillagePack/Blueprints/BP_Butterfly.uasset` | 외부 제공 Blueprint의 Construction 호환성 |
| `Content/ThirdPerson/Maps/ThirdPersonMap.umap` | 내비게이션 복구와 관련된 맵 상태 |
| `Content/__ExternalActors__/ThirdPerson/Maps/ThirdPersonMap/6/Y8/B5W8N6X01MFMWF4ZO4QWYL.uasset` | 맵의 Recast 내비게이션 저장 데이터 |
| `Config/DefaultEngine.ini` | Android File Server의 네트워크 시작 비활성화 등을 포함한 에디터 환경 설정 |
| `Config/DefaultGame.ini` | 시작할 때 StarterContent를 반복 삽입하는 동작 비활성화 |
| `NPC.uproject` | 검토 및 네이티브 검증에 사용하는 에디터 도구 활성화 |

원본 Data Table과 `BT_NPC`는 고정한 전체 압축파일과 바이트 단위로 일치합니다. 아이템 값, NPC 일정, 신뢰도 기준, 퀘스트 행과 실제 트리 설정을 보존했습니다. 추적 태스크의 클래스 기본 속도는 300이지만 트리에 저장된 인스턴스 값은 150입니다. 검증은 실제 태스크 인스턴스 값을 기준으로 하며, 추종 속도를 바꾸는 수정은 적용하지 않았습니다.

GameMode 조회를 통합한 성능 후보는 채택하지 않았습니다. 실험 결과와 검토용 도구 소스는 남겼으며, 현재 NPC 에셋의 조회 구조는 초기 구조를 유지합니다. [Blueprint 검토 안내](blueprint-inspection.ko.md)에서 그래프 내보내기, 테이블 행, 태스크 인스턴스 근거를 확인할 수 있습니다.

소스 묶음의 `manifests/original-project.json`, `manifests/overlay.json`, `manifests/final-project.json`은 복원 전후 파일을 식별합니다. 오버레이에는 적용할 파일의 해시와 교체 대상 원본의 해시를 함께 기록합니다. 전체 파일 목록의 기준은 이 매니페스트이며, 이 문서는 변경 범위를 설명합니다.

## 복원과 실제 실행 근거

고정된 전체 압축파일을 직접 내려받은 뒤 경량 소스 묶음의 검증 및 복원 도구를 사용합니다. [재현 안내](reproduce.ko.md)에 Windows Unreal Engine 5.4.4 빌드와 네이티브 검증 명령이 있습니다. 새 디렉터리에 복원하고 최종 소스 2,371개를 확인한 다음 프로젝트 바이너리를 생성합니다.

실제로 새 디렉터리에 복원한 후 소스 빌드와 네이티브 에디터 검증을 수행했습니다. [새 복원 프로젝트 실행 기록](../reports/fresh-restoration/run.json)에는 실행 도구의 빌드 확인, Blueprint 사전 검사와 게임플레이 검증 묶음 7개의 결과가 있습니다. 에디터 프로세스 종료 코드만으로 성공을 판단하지 말고 개별 보고서의 검증 결과도 함께 확인해야 합니다. 처음 전체 소스를 컴파일한 시간과 빌드가 최신 상태인지 다시 확인하는 시간은 서로 다르므로, 실행 기록의 후속 빌드 확인 시간을 최초 컴파일 시간으로 해석하지 않습니다.

엔진 실행 근거는 유지보수에 사용한 PC의 네이티브 Windows 환경에서 확보했습니다. Windows와 Linux에서 수행한 Python 복원 도구 검증은 별도 범위입니다. 도구 검증이나 새 디렉터리 복원이 독립된 다른 컴퓨터의 인증 또는 Linux Unreal 게임 실행 지원을 의미하지는 않습니다. [실행 보고서 안내](../reports/README.ko.md)에서 파일 확인, 게임 동작 검증, 성능 측정의 범위를 구분해 설명합니다.
