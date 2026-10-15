# 프로젝트 재현 방법

[English](reproduce.md) | 한국어

원본 전체 프로젝트를 복원하고 소스 번들 버전 `1`의 변경 파일을 적용한 뒤, C++ 모듈을 빌드하고 실제 Unreal Engine 검증을 실행하는 가이드입니다. 다운로드한 원본 압축 파일과 기존 작업을 보존할 수 있도록 별도의 프로젝트 폴더를 사용합니다.

## 필요한 다운로드

| 다운로드 | 용도 |
| --- | --- |
| 현재 소스 번들 | 유지보수 변경 파일, 검증 플러그인 소스, 복원 도구, manifest와 문서입니다. 압축을 풀면 루트에 `bundle.json`과 `tools/`가 있어야 합니다. |
| [원본 전체 `Project.zip`](https://drive.google.com/file/d/1qsdGAzKU3myKQVk04MfW-EngDHKKAqmW/view?usp=sharing) | 맵, 외부 Actor 패키지와 콘텐츠 자산을 제공하는 전체 프로젝트 의존성입니다. Drive에서 직접 다운로드합니다. |
| [보존한 핵심 자산 ZIP](https://github.com/Dongmyung378/Context-Aware-Npc-System/blob/main/downloads/npc-core-assets.zip) | GitHub에 보존한 원래 핵심 자산 30개의 검토용 배포물입니다. 초기 증거로 보존하며 전체 복원에는 원본 전체 프로젝트 압축 파일을 사용합니다. |

현재 소스 다운로드는 변경 파일과 도구를 담은 번들입니다. 약 2.26GB인 원본 전체 압축 파일은 별도로 받습니다. 원래 보고서와 발표 자료도 당시 결과를 담은 자료로 보존합니다.

확인한 원본 전체 압축 파일의 정보는 다음과 같습니다.

```text
파일명:   Project.zip
바이트:   2257866122
SHA-256: ba8e4ba770461e5272ff6cfaeaf4be9fe0219ec3fa630a404572c627e8080bfe
```

복원 도구는 압축 파일의 크기와 해시를 확인한 뒤 소스 파일과 최종 변경 파일을 manifest와 대조합니다. 다른 시점의 프로젝트가 들어오면 자동으로 대체하지 않고 중단합니다.

## 요구 사항과 기록한 환경

| 요구 사항 | 기록한 구성 |
| --- | --- |
| 엔진 | Unreal Engine 5.4.4 |
| 실제 게임 실행 | Windows의 네이티브 Unreal Editor와 명령행 에디터 |
| C++ 빌드 | C++ 도구를 설치한 Visual Studio 2022 |
| 컴파일러 선택 | MSVC 설치 폴더 `14.38.33130`, 기록한 컴파일러 빌드 `14.38.33145` |
| Windows SDK | `10.0.26100.0` |
| 복원과 실행 도구 | Python 3.10 이상, 표준 라이브러리 사용 |
| 렌더링 검증과 성능 측정 | DirectX 렌더링 환경이며, 기록한 프로필 조건은 D3D12와 1280 × 720 해상도 |

MSVC 설치 폴더 번호와 컴파일러 빌드 번호는 설치된 도구의 서로 다른 식별자입니다. 실행 도구의 `--compiler-version`은 설치 폴더를 선택하므로, 기록한 설치 구성에서는 `14.38.33130`을 사용합니다.

빌드하기 전에 엔진과 C++ 도구를 설치합니다. 빌드 결과, 셰이더 데이터와 에디터 캐시를 위한 추가 저장 공간도 필요합니다. 이 절차는 네이티브 Unreal 설치를 직접 사용하며 Docker는 필요하지 않습니다. 선택적인 문서 그림 생성은 별도로 Matplotlib을 사용하지만, 복원과 게임 검증 스크립트에는 외부 Python 패키지가 필요하지 않습니다.

## 새 폴더로 검증하고 복원하기

다음 PowerShell 예시는 아래 경로를 사용합니다.

- 소스 번들을 푼 경로는 `D:/NPC-Source`입니다.
- 원본 압축 파일은 `D:/Downloads/Project.zip`에 있습니다.
- `D:/NPC-Verify`는 존재하지 않아야 합니다. 상위 경로인 `D:/`는 있어야 합니다.

PowerShell에서 소스 번들 루트로 이동합니다.

```powershell
Set-Location "D:/NPC-Source"
python --version
python -X utf8 -B tools/verify_bundle.py --expected-source-version 1
```

번들 검증은 선언된 소스, 메타데이터와 변경 파일의 해시를 확인합니다. 출력 JSON에는 압축 파일이나 게임을 실행하지 않았다는 범위도 기록됩니다.

전체 프로젝트를 복원합니다.

```powershell
python -X utf8 -B tools/restore_project.py --archive "D:/Downloads/Project.zip" --output "D:/NPC-Verify" --expected-source-version 1
```

도구는 원본 압축 파일을 확인하고 임시 작업 폴더에 선언된 소스를 추출한 뒤, 검증한 변경 파일을 적용합니다. 최종 파일 구성과 해시를 확인하면 새 프로젝트 폴더를 확정합니다. 성공 출력에는 `status: passed_source_restoration`, 파일 수와 원본 압축 파일 해시가 들어 있습니다. 여기까지는 복원 파일의 일치 여부를 확인하며, 실행 동작은 다음 엔진 검증에서 확인합니다.

출력 경로가 이미 있으면 빈 폴더여도 거절합니다. 다시 복원할 때는 다른 새 경로를 선택합니다. 추출이나 변경 적용이 실패하면 확인할 수 있도록 임시 폴더를 남기고 위치를 안내할 수 있습니다. 기존 사용자 프로젝트를 덮어쓰지 않습니다.

## 소스 빌드와 실제 엔진 검증

복원한 프로젝트를 에디터에서 열어 두었다면 닫은 뒤 빌드합니다. `D:/NPC-Source`에서 다음 명령을 실행합니다.

```powershell
python -X utf8 -B tools/run_unreal_checks.py --engine "D:/Epic Games/UE_5.4" --project "D:/NPC-Verify/NPC.uproject" --output "D:/NPC-Checks" --build --compiler-version 14.38.33130
```

5.4.4 설치 위치가 다르면 엔진 경로를 바꿉니다. `D:/NPC-Checks`도 새 출력 폴더여야 합니다. 도구는 엔진에 포함된 .NET SDK와 UnrealBuildTool로 `NPCEditor`를 소스에서 빌드합니다. 기본 선택인 `all`은 게임 검증 7개 전에 실제 에디터에서 `inspect_blueprints.py`를 실행합니다. `/Game` 아래 프로젝트 Blueprint 46개를 로드하고 `SkipSave`로 컴파일해 오류와 경고가 모두 0인지 확인하며 `blueprints.json`을 남깁니다. 이후 실제 명령행 에디터에서 선택한 게임 검증을 실행합니다.

| 선택 인자 | 네이티브 자동화 시험 | 범위 |
| --- | --- | --- |
| `world` | `NPC.Gameplay.WorldInventory` | 원래 맵, 플레이어 조종 대상과 원래 NPC 인스턴스 |
| `states` | `NPC.Gameplay.OriginalContracts` | 시계, 대화 경계, 일정, 동행과 관련 상태 규칙 |
| `interactions` | `NPC.Gameplay.InteractionContracts` | 대상 선택 행렬, 실제 UMG 거래, 음식과 아이템 선택 확률 |
| `quests` | `NPC.Gameplay.QuestContracts` | 실제 퀘스트 조건과 보상에 따른 상태 변화 |
| `probability` | `NPC.Gameplay.QuestProbability` | 시험 조건과 표본 수를 명시한 퀘스트 제안 호출 |
| `navigation` | `NPC.Gameplay.NavigationContracts` | 배포한 검증이 구현한 실제 내비게이션과 이동 확인 |
| `inventory-input` | `NPC.Gameplay.InventoryInputContracts` | 원래 인벤토리 입력과 UI 생명주기 |

기본 게임 검증은 NullRHI에서 상태를 실행합니다. 결과는 실제 자동화 로그와 JSON으로 확인합니다. 도구는 자동화 성공 기록과 `assertions_passed: true`를 모두 요구하므로 Unreal 프로세스의 종료 코드 0만으로 성공 처리하지 않습니다. 보고서가 없거나 불완전한 경우, 상태 검증이 실패한 경우, 프로세스가 실패한 경우에는 실행 요약도 실패로 기록합니다.

출력에는 `summary.json`, 시험별 JSON, 엔진 로그, 콘솔 로그와 자동화 내보내기 폴더가 들어 있습니다. 실행 준비가 성공한 것과 게임 상태 검증이 통과한 것을 구분할 수 있는 기록입니다. 실패를 확인할 때는 출력 전체를 보존합니다.

`inventory-input` 검증은 원래 아이템 재생성 시간인 60초를 실제로 기다리므로 1분 이상 걸릴 수 있습니다. 게임 검증 하나만 선택하면 `all`의 Blueprint 사전 검사를 생략하며, 기본 전체 실행에는 이 검사가 포함됩니다.

빌드를 마친 뒤 특정 검증만 다시 실행하려면 종류를 선택하고 새 출력 경로를 사용합니다.

```powershell
python -X utf8 -B tools/run_unreal_checks.py --engine "D:/Epic Games/UE_5.4" --project "D:/NPC-Verify/NPC.uproject" --output "D:/NPC-Checks-Interactions" --suite interactions
```

## 렌더링 검증과 선택적 성능 측정

별도 렌더링 실행에는 실제 렌더링 경로를 사용합니다.

```powershell
python -X utf8 -B tools/run_unreal_checks.py --engine "D:/Epic Games/UE_5.4" --project "D:/NPC-Verify/NPC.uproject" --output "D:/NPC-Checks-Rendered" --rendered
```

1280 × 720 해상도로 오프스크린 PIE 렌더링을 실행합니다. 렌더링 조건은 실제 기록한 RHI와 뷰포트 값으로 확인해야 합니다. 상태 검증이 통과했다는 사실만으로 GPU 렌더링까지 수행했다고 추정하지 않습니다.

NPC 수에 따른 추가 부하 측정은 다음과 같습니다.

```powershell
python -X utf8 -B tools/run_unreal_checks.py --engine "D:/Epic Games/UE_5.4" --project "D:/NPC-Verify/NPC.uproject" --output "D:/NPC-Checks-Profile" --profile --repetitions 3
```

성능 측정은 렌더링 상태에서 준비 시간 10초와 수집 시간 15초를 사용하며, NPC 5개, 20개, 50개 조건을 각각 세 번 실행합니다. 기록한 RHI, 뷰포트, 컨트롤러와 행동 트리 실행 수, 이동 및 경로 관측을 함께 확인합니다. NPC의 대기는 원래 행동일 수 있으므로 이동 관측의 분모를 명시하며 모든 NPC가 계속 걷는다고 해석하지 않습니다.

월드 Actor Tick 구간은 모든 Actor와 중간 작업을 포함합니다. 에디터 프레임 종료 간격은 에디터, PIE, 렌더링 대기와 측정 비용을 포함합니다. 패키징한 단독 게임의 FPS가 아닙니다. 실행별 중앙값 세 개와 변동 범위를 보고하고, 소스, 내비게이션 상태와 작업 부하가 같은 실행끼리 비교해야 합니다. 이 명령은 재현 절차이며 성능 보장이나 특정 실행의 성공 결과를 의미하지 않습니다.

## 전체 게임 열어 보기

복원과 소스 빌드를 마친 뒤 `D:/NPC-Verify/NPC.uproject`를 Unreal Engine 5.4.4에서 엽니다. `/Game/ThirdPerson/Maps/ThirdPersonMap`을 사용하고 에디터에서 플레이를 시작합니다.

| 입력 | 동작 |
| --- | --- |
| `W` `A` `S` `D` | 이동 |
| `Left Shift` | 달리기 |
| `V` | 시점 전환 |
| `E` | 상황에 따라 NPC 상호작용, 아이템 획득 또는 대화 닫기 |
| `R` | 대화 또는 대화 이어가기 |
| `T` | 거래 화면 열기 |
| `F` | NPC 조건을 충족했을 때 동행 요청 또는 해제 |
| `I` | 인벤토리 열기 또는 닫기 |

직접 실행하면 자동화 상태 검증과 버튼 이벤트 검증에 더해 장면, 카메라, UI, 애니메이션과 입력을 확인할 수 있습니다. 원래 다섯 NPC의 일과, 대화, 신뢰도, 퀘스트, 거래와 동행이 주요 확인 대상입니다.

## 환경 검증을 해석하는 범위

복원 도구는 시험용 압축 파일과 변경 파일 사례를 사용해 Windows와 WSL의 Ubuntu를 포함한 Linux에서 검증했습니다. 이는 Python 도구의 파일 검사와 복원 동작에 대한 근거이며, 원본 전체 압축 파일과 게임을 Linux에서 실행했다는 의미는 아닙니다. Linux의 Unreal 게임 실행, 그래픽이나 C++ 게임 빌드를 인증한 결과도 아닙니다. 게임 실행 도구는 기록한 Windows Unreal 5.4.4 환경을 대상으로 합니다.

유지보수 PC의 새 폴더에서 복원하고 소스부터 빌드하면 복사한 프로젝트 바이너리에 의존하지 않고 확인할 수 있습니다. 다만 같은 컴퓨터에서 수행한 근거이며, 독립된 컴퓨터나 완전한 가상 머신에서 인증한 결과는 아닙니다. 원본 압축 파일의 식별 정보, manifest, 소스 빌드 로그와 실제 엔진 보고서를 통해 다른 사용자도 같은 절차를 실행하고 자기 환경의 차이를 확인할 수 있습니다.

버전이나 해시 불일치, C++ 도구 누락, 5.4.4 이외의 엔진, 게임 콘텐츠 누락, 유효하지 않은 내비게이션 데이터와 게임 상태 검증 실패는 서로 다른 문제입니다. 로그를 보존하고 확인된 선행 조건이나 소스 문제를 수정해야 합니다. 파일 무결성 검증의 통과를 해당 엔진 실행 결과로 대신하지 않습니다.
