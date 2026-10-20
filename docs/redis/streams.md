# Redis Streams: 지우지 않는 로그와 소비자 그룹

> **핵심 질문**: Redis 안에서 메시지를 넘길 때 List나 Pub/Sub 대신 Streams를 쓰면 무엇이 달라지는가? 소비자 그룹, PEL, ACK, 회수, 트리밍은 메시지 유실과 중복을 어떻게 다루며, 무엇을 개발자의 몫으로 남기는가?
>
> **범위**: 단일 Redis 서버의 스트림 키 하나를 기준으로 합니다. 내부 표현(radix tree와 listpack)은 메모리와 삭제 비용을 설명하는 데 필요한 만큼만 다룹니다. 클러스터에서 스트림을 여러 키로 나누는 설계는 다루지 않습니다. 복제·장애 조치는 그룹 상태가 어떻게 전달되고 어떤 명령이 빠질 수 있는지까지만 다루며, 빠진 명령별 결과는 소스와 공식 문서에서 끌어낸 추론입니다.

## 배경 지식

### 이 문서의 예시: 주문 완료 메시지 3건을 스트림 하나에

[메시지 큐 기초](../cs/message-queue.md)와 같은 장면을 씁니다.

- 주문 API가 주문이 끝날 때마다 스트림 키 `order:completed`에 "주문 완료" 엔트리를 하나씩 추가합니다. 엔트리는 #101, #102, #103 세 건이고, #101의 내용은 필드-값 쌍 `orderId 101, memberId 7, amount 30000`입니다.
- 엔트리 ID는 각각 `1000-0`, `1000-1`, `1005-0`입니다. #101과 #102는 같은 밀리초에, #103은 5밀리초 뒤에 추가됐다는 뜻입니다. 실제 ID의 앞부분은 `1760918400000`처럼 13자리 Unix 시각(밀리초)이지만, 이 문서에서는 읽기 쉽게 줄여 씁니다.
- 소비자 그룹은 두 개입니다.
  - `point`: 포인트 적립 워커 P1, P2가 나눠 처리하고, 결과를 포인트 DB에 씁니다.
  - `notify`: 알림 워커 N1이 외부 알림 API를 호출합니다.

```text
스트림 키 order:completed (Redis 메모리 안의 키 하나)

  엔트리:   [1000-0 #101] [1000-1 #102] [1005-0 #103]   ← 추가된 순서대로 뒤에 붙음

  그룹 point  : 어디까지 나눠 줬는가 + 받았지만 끝나지 않은 메시지 목록 + 소비자 P1, P2
  그룹 notify : 어디까지 나눠 줬는가 + 받았지만 끝나지 않은 메시지 목록 + 소비자 N1
```

이 그림에는 화살표가 없습니다. 그룹은 엔트리를 복사해 따로 갖지 않고, 같은 키 안에 붙은 "진행 상황 장부"일 뿐입니다. 엔트리를 메모리에 실제로 어떻게 묶어 두는지는 바로 아래 "스트림은 엔트리를 작은 덩어리로 묶어 저장합니다"에서, 복제본은 "그룹 상태도 복제되지만, 복제는 비동기입니다"에서 다룹니다.

### 메시지 큐의 기본 개념

- **한 줄**: 브로커에 메시지를 맡겨 생산자와 소비자를 분리하는 방식입니다. 처리된 메시지를 지우는 큐형과 읽어도 남기는 로그형이 있습니다. 같은 일을 하는 소비자를 그룹으로 묶으면 그룹 안에서는 나눠 받고 그룹끼리는 모두 받습니다. 소비자가 "처리를 끝냈다"고 알리는 ACK를 처리 후에 보내면 최소 한 번(at-least-once) 전달이 되고, 이때 생기는 중복은 소비자를 멱등하게 만들어 견딥니다. 원리는 [메시지 큐 기초](../cs/message-queue.md)에서 다룹니다.
- **이 주제에서는**: Streams는 로그형입니다. Redis가 소비자 그룹, "받았지만 끝나지 않은 메시지" 장부(PEL), 그 메시지를 다른 소비자에게 넘기는 명령을 제공합니다. ACK를 언제 보낼지, 중복을 어떻게 견딜지, 넘기는 작업을 누가 언제 실행할지, 오래된 엔트리를 언제 지울지는 개발자가 정해야 합니다. 이 문서는 이 경계를 따라갑니다.

### 명령 하나는 끊기지 않지만, 명령 사이에는 틈이 있습니다

- **한 줄**: Redis는 키와 값을 바꾸는 명령을 메인 스레드 하나가 하나씩 끝까지 실행합니다. 그래서 명령 하나는 다른 연결의 명령과 섞이지 않지만, 명령 두 개 사이에는 다른 명령이 들어올 수 있습니다. Redis 쪽 설명은 [Redis 원자성](./atomicity.md)에서, 원리는 [경쟁 상태와 원자성](../cs/race-condition-and-atomicity.md)에서 다룹니다.
- **이 주제에서는**: 소비자 그룹으로 읽는 `XREADGROUP` 명령 하나 안에서 "아직 아무에게도 주지 않은 다음 엔트리 고르기, 그룹의 진행 위치 옮기기, 장부에 '누구에게 줬다' 적기"가 한 덩어리로 실행됩니다. 그래서 P1과 P2가 같은 순간에 읽어도 같은 새 엔트리를 받지 않습니다. 반대로 "`XREADGROUP`으로 받기 → 포인트 DB에 적립 → `XACK`로 완료 알리기"는 Redis 명령 두 개와 다른 저장소에 대한 요청 하나로 이루어지므로 틈이 있습니다.

### 블로킹 읽기: 기다리는 것은 서버가 아니라 그 연결입니다

- **한 줄**: 읽기 명령에 `BLOCK <밀리초>`를 주면, 새 엔트리가 없을 때 Redis는 그 연결의 응답을 미뤄 두었다가 엔트리가 들어오거나 시간이 다 되면 응답합니다.
- **비유**: Redis 원자성 문서에서 쓴 "창구 직원이 한 명인 은행"을 이어 갑니다. 손님 P1이 "새 서류가 오면 불러 주세요"라고 번호표를 받고 대기석에 앉습니다. 직원은 그동안 다른 손님의 일을 계속 처리하고, 새 서류가 들어오면 P1을 부릅니다.
- **실제로는**:
  - 새 엔트리가 없으면 Redis는 그 연결을 "기다리는 클라이언트"로 등록하고 다음 명령으로 넘어갑니다. 메인 스레드는 멈추지 않습니다.
  - 메인 스레드는 "요청이 도착한 연결이 있는지 운영체제에 묻고, 도착한 명령을 실행하고, 응답을 보내는" 일을 끝없이 되풀이합니다. 이 반복을 이벤트 루프라고 합니다.
  - 생산자의 `XADD`가 키에 엔트리를 넣으면, 그 `XADD`를 처리한 메인 스레드가 이벤트 루프의 다음 차례로 넘어가기 전에, 기다리던 연결들의 출력 버퍼(연결마다 보낼 응답을 모아 두는 서버 메모리)에 새 엔트리를 담은 응답을 넣어 둡니다. 실제로 네트워크로 보내는 것은 그 뒤이고, `XADD`를 보낸 생산자의 응답도 이때 같은 방식으로 버퍼에 들어 있습니다(공식 문서 streams "How serving blocked consumers works"). 그래서 소비자는 폴링 간격만큼 늦지 않고, `XADD`의 응답과 비슷한 시점에 엔트리를 받습니다.
  - 비유와 다른 점: 대기석에 앉은 것은 손님이 아니라 **연결 하나**입니다. 그 연결로 다른 명령을 보내더라도 서버는 응답을 기다리는 동안 그 명령을 처리하지 않습니다. 그래서 클라이언트 라이브러리는 블로킹 읽기에 다른 명령과 섞이지 않는 전용 연결을 씁니다. Spring Data Redis의 Lettuce 연동도 `BLOCK`이 있는 읽기를 전용 연결로 실행합니다.
  - `MULTI`로 묶은 명령이나 Lua 스크립트 안에서는 기다릴 수 없습니다. 그 안의 블로킹 읽기는 읽을 새 엔트리가 있으면 평소처럼 돌려주고, 없으면 기다리지 않고 바로 "시간 초과"처럼 빈 결과를 돌려줍니다. `MULTI`는 그 뒤에 보낸 명령을 모아 두었다가 `EXEC`를 받으면 다른 연결의 명령을 끼우지 않고 한꺼번에 실행하는 방식이고, Lua 스크립트는 여러 명령을 서버 안에서 한 번에 실행하는 짧은 프로그램입니다. 둘 다 Redis 원자성 문서에서 다룹니다.
- **이 주제에서는**: P1이 `BLOCK 2000`으로 기다리는 동안에도 같은 Redis의 다른 명령은 막히지 않습니다. 대가는 연결입니다. 기다리는 소비자 하나가 연결 하나를 차지합니다.

### 스트림은 엔트리를 작은 덩어리로 묶어 저장합니다

메모리와 삭제 비용을 이해하려면 저장 방식 하나만 알면 됩니다.

- **한 줄**: 스트림은 엔트리를 하나씩 따로 저장하지 않고, 여러 엔트리를 연속된 메모리 한 덩어리(listpack)에 이어 적습니다. 그리고 이 덩어리들을 첫 엔트리의 ID로 찾을 수 있게 정렬된 트리(radix tree)에 매달아 둡니다.
- **비유**: 공책입니다. 한 페이지(덩어리)에 여러 줄(엔트리)을 이어 적고, 표지의 목차(트리)는 "이 페이지는 몇 번 ID부터"만 가리킵니다. 오래된 기록을 버릴 때 페이지째 찢어 낼 수도 있고, 한 줄만 두 줄을 그어 지울 수도 있습니다. 두 줄을 그어도 그 줄이 차지한 종이는 그대로입니다.
- **실제로는**:
  - **메모리 할당**은 프로그램이 "이만큼 쓸 메모리를 달라"고 요청해 연속된 조각을 받는 일입니다. 조각마다 그 조각을 가리키는 주소, 할당기의 크기 단계로 올리며 버리는 칸, 할당과 해제 호출이 따로 듭니다. 원리는 [메모리 할당](../cs/memory-allocation.md)에서 다룹니다. 스트림은 엔트리마다 조각을 받지 않고 덩어리 단위로 받으므로, 작은 엔트리가 많아도 이 비용을 덩어리 수만큼만 냅니다. 지울 때의 해제 횟수도 덩어리 수를 따릅니다.
  - listpack은 Redis가 쓰는 직렬화 형식으로, 여러 값을 하나의 연속 메모리에 차례로 적습니다. 새 엔트리는 마지막 덩어리에 덧붙이고, 덩어리가 대략 4096바이트(`stream-node-max-bytes` 기본값)에 이르거나 삭제 표시된 것까지 쳐서 엔트리 100개(`stream-node-max-entries` 기본값)를 채우면 새 덩어리를 엽니다.
  - 비유처럼, 한 엔트리를 지우면 그 자리에 "삭제됨" 표시만 합니다. 메모리는 덩어리의 엔트리가 모두 지워져 덩어리째 없앨 때 돌려줍니다. 표시만 된 엔트리를 모아 덩어리를 다시 압축하는 처리는 없습니다.
- **이 주제에서는**: 예시의 #101~#103은 덩어리 하나에 들어갑니다. "트리밍은 덩어리 단위로 하면 싸고, 엔트리 단위로 하면 표시만 남습니다"에서 이 차이가 지우는 비용과 메모리를 가릅니다.

## Redis 안에서 메시지를 넘기는 세 방법 중 Streams는 '남기고 장부를 두는' 쪽입니다

Redis에는 메시지를 넘기는 데 쓸 수 있는 기능이 셋 있습니다. 예시의 P1이 #101을 받은 직후 죽는 상황으로 비교합니다.

| 질문 | Pub/Sub | List (`LPUSH` + `BRPOP`) | Streams |
| --- | --- | --- | --- |
| 메시지를 저장하는가 | 아니요. 그 순간 구독 중인 연결에만 보냅니다 | 예. 꺼내기 전까지 리스트에 있습니다 | 예. 지울 때까지 스트림에 있습니다 |
| 읽으면 어떻게 되나 | 보내면 끝 | 꺼낸 원소는 리스트에서 빠집니다(큐형) | 엔트리는 남고 그룹의 위치만 움직입니다(로그형) |
| P1이 #101을 받은 직후 죽으면 | #101은 사라집니다 | 꺼낸 #101은 사라집니다 | #101은 P1 몫의 "끝나지 않은 메시지"로 남습니다 |
| 같은 메시지를 포인트와 알림이 각자 받으려면 | 둘 다 구독하면 되지만, 둘 다 살아 있을 때만 | 리스트를 목적마다 따로 두고 생산자가 각각 넣어야 합니다 | 그룹을 둘 만들면 됩니다 |
| 재전달 횟수, 처리 중 목록 | 없음 | 없음. 앱이 직접 만듭니다 | Redis가 장부로 관리합니다 |
| 지우기 | 해당 없음 | 꺼내면 지워짐 | 개발자가 트리밍 정책을 정해야 합니다 |

- **Pub/Sub**은 공식 문서가 최대 한 번(at-most-once) 전달이라고 밝힙니다. 구독자가 처리하지 못하면 메시지는 영영 사라집니다(공식 문서 pubsub "Delivery semantics"). 놓쳐도 되는 실시간 알림에 맞습니다.
- **List**는 꺼내는 순간 원소가 빠지므로, 꺼낸 직후 소비자가 죽으면 메시지를 잃습니다. `LMOVE`(블로킹은 `BLMOVE`)로 꺼내면서 "처리 중" 리스트에 옮겨 두고, 처리 뒤 지우는 패턴으로 이를 줄일 수 있습니다(공식 문서 lmove "Pattern: reliable queue"). 하지만 오래 남은 원소를 찾아 되돌리는 감시, 재전달 횟수, 목적별 복제는 모두 앱이 만들어야 합니다.
- **Streams**는 이 장부를 Redis가 대신 들고 있습니다. 대가는 두 가지입니다. 지우지 않으므로 무엇을 언제 지울지 정해야 하고, 장부를 다루는 명령과 개념이 더 많습니다.

정리하면, 놓쳐도 되면 Pub/Sub, 목적이 하나이고 단순한 작업 분배면 List, 빠지면 안 되거나 여러 목적이 같은 메시지를 읽으면 Streams가 맞습니다.

## 엔트리 ID는 '밀리초-순번'이고, 순서와 위치가 됩니다

- **형식**: 엔트리 ID는 두 수, 즉 Unix 시각(밀리초)과 순번입니다. `1000-1`은 "1000밀리초에 두 번째로 들어온 엔트리"라는 뜻입니다.
- **자동 생성(`XADD ... *`)**: Redis는 명령을 실행하는 순간의 서버 시각을 봅니다. 그 시각이 마지막 ID의 밀리초보다 크면 `<시각>-0`을 쓰고, 같거나 작으면 마지막 ID에서 순번만 1 올립니다. 예시의 #101과 #102가 `1000-0`, `1000-1`이 된 이유입니다.
- **언제나 커집니다**: 서버 시계가 뒤로 가도 새 ID는 마지막 ID보다 큽니다. 직접 ID를 지정할 때도 마지막 ID보다 작거나 같으면 거부합니다. 그래서 ID의 밀리초는 "대략 이 무렵에 추가됐다"는 뜻이지 정확한 시각이 아닙니다.
- **무엇에 쓰이나**: ID가 언제나 커지므로 ID만으로 순서와 위치를 말할 수 있습니다. 그룹의 진행 위치는 ID 하나로 기록되고, `XRANGE`로 "이 ID부터 저 ID까지"를 다시 읽을 수 있습니다. 순번을 빼고 밀리초만 쓰면 시간 구간으로 읽을 수도 있습니다(공식 문서 streams "Querying by range"). 복제본에는 ID까지 정해진 `XADD`가 전달되므로 복제본의 ID도 같습니다(같은 절).

**혼동 주의: 엔트리 ID는 업무 ID가 아닙니다.** 주문 API가 #101을 보냈는데 응답을 받지 못해 다시 보내면, Redis는 새 엔트리로 받아 새 ID를 붙입니다. 같은 주문인지 판단하는 멱등 처리의 기준은 엔트리 ID가 아니라 내용 안의 주문 ID 101이어야 합니다. 이유는 메시지 큐 기초 문서의 "중복은 막기보다 견디게 만듭니다"와 같습니다.

## 그룹 없이 읽는 XREAD는 위치를 앱이 들고 다닙니다

`XREAD`는 "이 ID보다 큰 엔트리를 달라"는 명령입니다. 읽는 쪽에 대한 상태를 서버에 남기지 않으므로, 어디까지 읽었는지는 앱이 기억했다가 다음 요청에 넣어야 합니다.

- 특수 ID `$`는 "지금 스트림의 마지막 ID"를 뜻합니다. `XREAD BLOCK 2000 STREAMS order:completed $`는 지금 이후에 들어오는 엔트리만 기다립니다.
- **흔한 실수**: 재시작할 때마다 `$`로 읽기 시작하면, 꺼져 있던 동안 들어온 엔트리는 건너뜁니다. 엔트리는 스트림에 남아 있는데도 Pub/Sub처럼 놓치는 셈입니다. 마지막으로 처리한 ID를 어딘가에 저장해 두고 그 ID부터 읽어야 합니다.
- `XREAD`를 쓰는 연결은 각자 모든 엔트리를 받습니다(모두 받기). 처리 완료 기록이나 나눠 받기는 없습니다. 쓰기를 하지 않는 읽기 명령이므로 복제본에서도 실행할 수 있습니다.

나눠 받기와 처리 완료 기록이 필요하면 소비자 그룹을 씁니다.

## 소비자 그룹은 그룹 안에서는 나눠 주고, 그룹끼리는 모두 받게 합니다

### 그룹 만들기: 어디서부터 줄지 정합니다

`XGROUP CREATE order:completed point 0`처럼 그룹을 만들 때 "어느 ID 다음부터 나눠 줄지"를 정합니다. `0`은 처음부터, `$`는 지금 마지막 엔트리 다음부터입니다. 그룹은 이 위치(last-delivered-id)와 소비자 목록, 끝나지 않은 메시지 장부를 가집니다(공식 문서 streams "Consumer groups").

**흔한 실수**: #101~#103이 이미 들어온 뒤에 `$`로 그룹을 만들면, 그 그룹은 #101~#103을 받지 못합니다. 서비스가 처음 뜰 때 그룹을 만드는 코드라면 시작 위치를 의도대로 골랐는지 확인합니다.

**그룹을 만드는 명령은 두 경우에 에러를 냅니다.** 서비스가 뜰 때 그룹을 만드는 코드라면 둘 다 정상적으로 일어나는 상황으로 다뤄야 합니다.

- **키가 아직 없을 때**: 주문 API가 아직 한 번도 `XADD`하지 않았다면 `order:completed` 키가 없어 `XGROUP CREATE`가 에러를 냅니다. 끝에 `MKSTREAM`을 붙이면 빈 스트림을 함께 만듭니다.
- **그룹이 이미 있을 때**: `point` 그룹이 이미 있으면 `BUSYGROUP` 에러를 냅니다. P1과 P2가 뜰 때마다 그룹을 만든다면, 나중에 실행된 쪽은 반드시 이 에러를 받습니다. 재시작할 때도 마찬가지입니다. 이 에러를 "이미 준비됨"으로 보고 넘어가지 않으면 앱 기동이 실패합니다.

Spring Data Redis의 `StreamOperations.createGroup`은 언제나 `MKSTREAM`을 붙여 보내므로 첫 번째 경우는 생기지 않습니다. 두 번째 경우는 Redis의 에러 응답이 클라이언트에서 예외로 올라옵니다. 또 시작 위치를 받지 않는 `createGroup(key, group)`은 `ReadOffset.latest()`, 즉 `$`로 그룹을 만듭니다. 그래서 위의 흔한 실수가 Spring의 가장 짧은 호출에서 그대로 일어납니다. 이미 쌓인 엔트리부터 처리해야 한다면 시작 위치를 명시합니다.

### XREADGROUP 하나가 '고르기, 위치 옮기기, 장부 적기'를 한 번에 합니다

소비자는 `XREADGROUP GROUP point P1 COUNT 1 STREAMS order:completed >`처럼 읽습니다. 특수 ID `>`는 "이 그룹에서 아직 아무에게도 주지 않은 엔트리"를 뜻합니다. 소비자 이름(P1)은 클라이언트가 정하고, 처음 쓰는 이름이면 그 자리에서 소비자가 만들어집니다.

예시에서 P1과 P2가 거의 동시에 한 건씩 읽으면 이렇게 됩니다.

| 순서 | 실행된 명령 | 받은 엔트리 | 그룹 point의 위치 | point의 끝나지 않은 메시지 |
| --- | --- | --- | --- | --- |
| 1 | P1의 `XREADGROUP ... COUNT 1 >` | `1000-0` (#101) | `1000-0` | `1000-0`(P1) |
| 2 | P2의 `XREADGROUP ... COUNT 1 >` | `1000-1` (#102) | `1000-1` | `1000-0`(P1), `1000-1`(P2) |
| 3 | P1의 `XREADGROUP ... COUNT 1 >` | `1005-0` (#103) | `1005-0` | `1000-0`(P1), `1000-1`(P2), `1005-0`(P1) |

두 명령이 같은 순간에 도착해도 메인 스레드는 하나씩 실행합니다. 그리고 명령 하나 안에서 위치 옮기기와 장부 적기까지 끝내므로, 2번의 P2는 이미 옮겨진 위치 다음을 받습니다. "읽고 나서 위치를 따로 갱신"하는 구조였다면 둘이 같은 엔트리를 받았을 것입니다. Redis 원자성 문서의 "판단을 명령 하나에 담을 수 있으면 그것이 가장 단순합니다"를 Redis가 명령 설계로 해 둔 것입니다.

그룹 notify는 위치와 장부가 따로 있습니다. N1이 `COUNT 10`으로 읽으면 #101~#103을 모두 받습니다. point 그룹이 무엇을 가져갔는지와 상관없습니다.

### 나누는 기준은 '먼저 요청한 소비자'입니다

Redis의 소비자 그룹은 Kafka처럼 파티션을 소비자에게 배정하지 않습니다. 새 엔트리는 그 순간 요청한 소비자에게 갑니다. 그래서 빠른 소비자가 더 많이 받고, 한 소비자가 죽으면 남은 소비자가 새 엔트리를 모두 받습니다(공식 문서 streams "Differences with Kafka (TM) partitions").

대가는 순서입니다. 같은 스트림을 여러 소비자가 나눠 받으면, P2가 #102를 P1의 #101보다 먼저 끝낼 수 있습니다. 같은 회원의 메시지 순서가 중요하면, 그 회원의 메시지가 항상 같은 스트림 키로 가고 그 키를 한 소비자만 읽게 해야 합니다. 공식 문서도 Kafka의 파티션은 Redis에서 스트림 키 N개에 가깝다고 설명합니다(같은 절).

`XREADGROUP`은 그룹 상태를 바꾸는 쓰기 명령이므로, 기본 설정처럼 복제본이 읽기 전용(`replica-read-only yes`)이면 복제본에서 실행할 수 없습니다.

## PEL과 XACK: 받았다와 처리했다를 Redis가 따로 적어 둡니다

### PEL은 '받았지만 끝나지 않은 메시지' 장부입니다

- **한 줄**: PEL(Pending Entries List)은 그룹이 소비자에게 넘겼지만 아직 ACK를 받지 못한 엔트리의 목록입니다.
- **실제로는**: 그룹 전체의 PEL과 소비자별 PEL이 같은 기록을 함께 가리킵니다. 기록 하나에는 누가 가지고 있는지, 마지막으로 넘긴 시각, 몇 번 넘겼는지(전달 횟수)가 있습니다. 처음 넘길 때 전달 횟수는 1입니다.
- **이 주제에서는**: 앞 표의 "point의 끝나지 않은 메시지" 열이 PEL입니다. 메시지 큐 기초 문서에서 "브로커는 ACK가 올 때까지 따로 기록한다"고 한 그 장부입니다.

### XACK는 장부에서 지울 뿐, 엔트리를 지우지 않습니다

P1이 #101의 포인트를 적립한 뒤 `XACK order:completed point 1000-0`을 보내면, Redis는 point 그룹의 PEL과 P1의 PEL에서 `1000-0`을 지웁니다. 엔트리 #101은 스트림에 그대로 남습니다. 알림 그룹이 아직 처리 중일 수 있고, 스트림은 로그이기 때문입니다. 엔트리를 지우는 일은 "트리밍은 덩어리 단위로 하면 싸고, 엔트리 단위로 하면 표시만 남습니다"에서 다룹니다.

`XACK`는 실제로 PEL에서 지운 개수를 돌려줍니다. 이미 ACK한 ID를 다시 ACK하면 0을 받을 뿐 에러가 아니므로, ACK를 두 번 보내는 것은 해가 없습니다.

### ACK 시점이 유실과 중복을 가르는 것은 Streams에서도 같습니다

P1이 #101을 처리하는 일은 "`XREADGROUP`으로 받기 → 포인트 DB에 적립 → `XACK`"입니다. 적립은 포인트 DB에, ACK는 Redis에 보내는 요청이라 하나로 묶이지 않습니다.

- **적립 후 ACK(최소 한 번)**: 적립 직후 P1이 죽으면 `1000-0`이 PEL에 남습니다. 뒤에서 볼 회수를 거쳐 다시 처리되고, 포인트가 두 번 쌓일 수 있습니다. 주문 ID 101로 멱등 처리를 해야 합니다.
- **`NOACK`(최대 한 번)**: `XREADGROUP`에 `NOACK`를 주면 엔트리를 PEL에 넣지 않습니다. 공식 문서는 이것이 "읽는 순간 ACK하는 것과 같다"고 설명합니다(공식 문서 xreadgroup). 받은 직후 죽으면 그 엔트리는 그 그룹에서 다시 받을 방법이 없습니다.

### 재시작한 소비자는 자기 장부부터 다시 읽어야 합니다

P1이 #103(`1005-0`)을 처리하던 중 죽었다가 **같은 이름**으로 다시 떴다고 합시다.

- `>`로 읽으면 새 엔트리만 받습니다. `1005-0`은 P1의 PEL에 남은 채 다시 오지 않습니다.
- `>`가 아닌 ID(예: `0`)로 읽으면, 그 ID보다 큰 **자기 PEL의 엔트리**만 돌려줍니다. 이때 전달 횟수가 1 오릅니다. `BLOCK`과 `NOACK`는 무시됩니다.
- 그래서 소비자는 시작할 때 `0`으로 자기 장부를 먼저 비우고, 빈 응답을 받으면 `>`로 새 엔트리를 읽는 순서를 따릅니다(공식 문서 streams "Creating a consumer group"의 예시 흐름).
- 장부에 있던 엔트리가 그사이 트리밍으로 지워졌다면, ID와 함께 내용 자리에 null이 옵니다. 이 경우는 "트리밍과 장부가 부딪칠 때: KEEPREF, DELREF, ACKED"에서 다룹니다.

### lag과 pending은 다른 것을 셉니다

`XINFO GROUPS`는 그룹마다 두 숫자를 보여 줍니다.

| 지표 | 무엇을 세나 | 예시의 point 그룹, 앞 표 3번 직후 |
| --- | --- | --- |
| `lag` | 스트림에 있지만 **아직 그룹에 넘기지 않은** 엔트리 수 | 0 (#103까지 다 넘겼음) |
| `pending` | 넘겼지만 **아직 ACK를 받지 못한** 엔트리 수 | 3 |

`lag`이 0이라고 처리가 끝난 것이 아닙니다. 위 상태에서 P1이 죽어 있으면 `lag`은 0인데 #101과 #103은 처리되지 않았습니다. 밀린 양은 두 숫자를 함께 봐야 합니다. 또 `lag`은 중간에 지워진 엔트리가 있는 등 계산할 수 없는 경우 null로 나옵니다.

## 죽은 소비자의 메시지는 Redis가 다시 주지 않습니다: 직접 회수합니다

### 시간이 지나도 자동으로 재전달하지 않습니다

메시지 큐 기초 문서에서 본 것처럼, 끝나지 않은 메시지를 다른 소비자에게 넘기는 방식은 시스템마다 다릅니다. Redis는 넘기지 않습니다. P1이 영영 돌아오지 않으면 `1005-0`은 P1 몫으로 PEL에 계속 남습니다. 공식 문서는 다른 소비자가 PEL을 살펴 특정 메시지를 직접 가져와야 하며, 그러지 않으면 서버가 영원히 옛 소비자에게 남겨 둔다고 설명합니다(공식 문서 streams "Recovering from permanent failures").

### XPENDING으로 보고, XCLAIM·XAUTOCLAIM으로 가져옵니다

- **`XPENDING`**: PEL을 보는 읽기 전용 명령입니다. 요약 형태는 끝나지 않은 메시지 수, 가장 작은 ID와 가장 큰 ID, 소비자별 개수를 줍니다. 범위를 주면 메시지마다 소유자, 마지막 전달 뒤 지난 시간(idle), 전달 횟수를 줍니다. `IDLE`로 오래된 것만 거를 수 있습니다.
- **`XCLAIM <key> <group> <consumer> <min-idle-time> <ID...>`**: 지정한 메시지의 소유자를 바꿉니다. 마지막 전달 뒤 `min-idle-time` 밀리초 이상 지난 메시지만 가져오고, 가져오면서 그 시간을 0으로 되돌리고 전달 횟수를 1 올립니다. 다만 `JUSTID`를 주면 전달 횟수를 올리지 않고, `RETRYCOUNT`로 값을 직접 정할 수도 있습니다. `XAUTOCLAIM`의 `JUSTID`도 같습니다.
- **`XAUTOCLAIM <key> <group> <consumer> <min-idle-time> <start> [COUNT n]`**: PEL을 `start`부터 훑으며 오래된 메시지를 한 번에 가져오고 다음에 이어 훑을 위치(커서)를 돌려줍니다. 커서 `0-0`은 PEL 끝까지 한 바퀴 돌았다는 뜻입니다. `COUNT` 기본값은 100이고, 한 번 호출에 최대 `COUNT`의 10배만큼 PEL 기록을 살펴봅니다.

예시에서 P1이 돌아오지 않을 때, P2가 `XAUTOCLAIM order:completed point P2 60000 0-0`을 실행합니다. 그러면 1분 이상 처리되지 않은 `1005-0`(#103)이 P2의 몫이 되고, 전달 횟수는 2가 됩니다.

**두 워커가 동시에 회수해도 한 명만 가져갑니다.** 가져오는 순간 idle 시간이 0이 되므로, 같은 `min-idle-time`으로 뒤이어 실행된 회수는 조건에 맞지 않아 그 메시지를 건너뜁니다(공식 문서 xclaim). 이것도 명령 하나 안에서 "조건 확인과 소유자 변경"을 끝내기 때문에 성립합니다.

### min-idle-time이 처리 제한 시간이고, 회수는 누군가 실행해야 합니다

- `min-idle-time`은 메시지 큐 기초 문서의 "처리 제한 시간"에 해당합니다. 다만 이 시간이 지나도 원래 소비자의 처리가 멈추지는 않습니다. 회수는 PEL의 소유자만 바꾸므로, 느릴 뿐인 P1의 메시지를 P2가 가져가면 둘이 같은 메시지를 처리합니다. 짧게 잡을 때와 길게 잡을 때의 대가와 정하는 기준은 그 문서의 "처리 중에 사라진 소비자의 메시지는 다시 전달됩니다"와 같습니다. 예시의 60000(1분)은 P1의 정상 처리 시간 중 느린 쪽보다 넉넉하다고 가정한 값입니다.
- 회수는 Redis가 알아서 하지 않으므로, 소비자마다 주기적으로 실행하거나 회수만 맡는 별도 작업을 둡니다(공식 문서 streams "Recovering from permanent failures"). 아무도 실행하지 않으면 죽은 소비자의 메시지는 영원히 처리되지 않습니다.

### 계속 실패하는 메시지는 전달 횟수로 걸러 냅니다

#103의 금액 필드가 비어 있어 적립 코드가 매번 예외를 던진다고 합시다. `JUSTID` 없이 회수하면 회수될 때마다 전달 횟수가 오르므로, 정한 횟수를 넘은 메시지를 다른 스트림(예: `order:completed:dead`)에 옮기고 원래 그룹에서는 ACK합니다. Redis에 데드 레터 큐 기능이 따로 있는 것이 아니라, 전달 횟수를 보고 이렇게 옮기는 것이 Streams에서 데드 레터를 구현하는 방법입니다(공식 문서 streams "Claiming and the delivery counter"). 옮기는 `XADD`와 `XACK`를 묶는 방법은 다음 절에서 다룹니다.

### 소비자 이름은 인스턴스마다 고정합니다

- 같은 이름을 쓰는 인스턴스 둘은 PEL 하나를 함께 씁니다. 한쪽이 재시작하며 `0`으로 읽으면 다른 쪽이 처리 중인 메시지까지 받습니다.
- 반대로 시작할 때마다 무작위 이름을 쓰면, 재시작할 때마다 옛 이름의 소비자가 PEL을 쥔 채 그룹에 남습니다. 회수가 이를 거둬 가야 합니다.
- `XGROUP DELCONSUMER`로 소비자를 지우면 그 소비자의 PEL 기록도 함께 지워집니다. 그 메시지들은 그 그룹에서 다시 전달되지 않으므로, 지우기 전에 회수부터 합니다.

## 처리 결과도 Redis에 있으면, Lua로 처리와 ACK를 한 덩어리로 묶을 수 있습니다

메시지 큐 기초 문서에서 "처리와 ACK는 서로 다른 저장소라 묶을 수 없다"고 했습니다. 처리 결과도 **같은 Redis**에 쓴다면 이 전제가 깨집니다. 이 절에서만 예시를 바꿔, 포인트 잔액을 포인트 DB가 아니라 Redis Hash에 둔다고 가정합니다. 묶을 수 있는 조건을 보이기 위해서입니다.

### 소비자 쪽: '이미 처리했나 확인, 적립, 처리 기록, XACK'를 스크립트 하나로

P1이 `XREADGROUP`으로 #101을 받은 뒤, 다음 일을 Lua 스크립트 하나로 보냅니다.

1. "적립을 끝낸 주문 ID" 집합에 101이 있는지 확인합니다. 있으면 3번으로 건너뜁니다.
2. 회원 7의 잔액을 올리고, 집합에 101을 넣습니다.
3. `XACK order:completed point 1000-0`을 실행합니다.

스크립트는 메인 스레드가 끊지 않고 실행하므로, "적립은 됐는데 ACK는 안 된" 중간 상태를 다른 연결이 볼 일이 없습니다. P1이 스크립트를 보내기 전에 죽으면 아무것도 바뀌지 않았고 `1000-0`은 PEL에 남습니다. 스크립트가 끝난 뒤에 죽으면 적립과 ACK가 모두 끝났습니다. 스크립트를 보냈는데 응답을 못 받아 다시 보내도 1번 확인이 두 번째 적립을 막습니다.

[Redis 원자성](./atomicity.md)에서 본 Lua의 대가와 클러스터의 같은 슬롯 조건이 여기에도 그대로 붙습니다.

- **중간 에러는 되돌리지 않습니다.** 2번 뒤에 에러가 나면 적립과 기록은 남고 ACK만 빠집니다. 이 경우 `1000-0`이 PEL에 남아 다시 처리되지만, 1번 확인이 이중 적립을 막습니다. 그래서 순서를 "확인 → 처리와 처리 기록 → ACK"로 둡니다. 처리 기록을 ACK보다 먼저 남기는 것이 핵심입니다. 다만 2번 안에서 잔액을 올린 뒤 처리 기록을 남기기 전에 에러가 나는 경우는 이 순서로도 막지 못합니다.
- **키를 모두 `KEYS`로 넘기고, 클러스터에서는 같은 슬롯에 둬야 합니다.** 스트림, 잔액 Hash, 처리 기록 집합을 같은 해시 태그(예: `{order}`)로 묶어야 합니다. 그러면 이 키들이 모두 한 노드에 모이므로, 그 노드 하나가 이 흐름 전체의 처리량 상한이 됩니다.
- **읽기는 스크립트 밖에서 합니다.** 스크립트 안에서는 `BLOCK`을 줘도 기다리지 않습니다. 새 엔트리가 없으면 바로 빈 결과가 옵니다(배경 지식의 블로킹 읽기). 스크립트가 길어지면 그동안 모든 명령이 기다리므로, 스크립트에는 메시지 하나의 처리와 ACK만 넣습니다.

### 판단이 필요 없으면 MULTI/EXEC로 충분합니다

데드 레터로 옮기는 "`XADD order:completed:dead ...` + `XACK order:completed point 1005-0`"은 앞 명령의 결과를 보고 분기할 필요가 없습니다. Redis 원자성 문서에서 본 대로 MULTI/EXEC는 중간 결과로 판단할 수 없지만, 판단이 필요 없는 묶음에는 맞습니다. 묶지 않으면 옮긴 직후 죽었을 때 같은 메시지가 데드 레터 스트림에 두 번 들어갈 수 있습니다. 클러스터라면 앞 절의 스크립트처럼 두 키가 같은 슬롯에 있어야 합니다. `EXEC`에 모인 명령의 키가 여러 슬롯에 걸치면 실행이 거부되므로, 두 키를 같은 해시 태그로 묶습니다.

### 생산자 쪽: 상태 변경과 XADD도 같은 방식으로 묶을 수 있습니다

주문 상태도 Redis에 있다면 "주문 상태를 완료로 바꾸기 + `XADD`"를 한 스크립트나 MULTI/EXEC로 묶어, 상태만 바뀌고 메시지는 없는 틈을 없앨 수 있습니다. 주문이 DB에 있다면(대부분 그렇습니다) 이 틈은 남습니다. 그때는 메시지 큐 기초 문서의 트랜잭셔널 아웃박스 같은 별도 설계가 필요합니다. 보낼 메시지를 주문과 같은 DB 트랜잭션으로 기록해 두고, 별도 과정이 그 기록을 읽어 `XADD`하는 방식입니다. 이 과정도 실패하면 다시 보내므로 같은 메시지가 두 번 들어갈 수 있습니다.

### 묶을 수 없는 경우가 더 흔합니다

포인트가 DB에 있거나 알림처럼 외부 API를 부르는 일은 Redis 스크립트로 묶을 수 없습니다. 그때는 처리 후 ACK(최소 한 번)와 주문 ID 기준의 멱등 처리로 돌아갑니다. Lua로 묶는 것은 "처리 결과가 같은 Redis의 같은 슬롯에 있을 때"만 쓸 수 있는 선택지입니다.

## 트리밍은 덩어리 단위로 하면 싸고, 엔트리 단위로 하면 표시만 남습니다

### ACK해도 지워지지 않으므로, 지우는 정책을 따로 둡니다

`XACK`는 엔트리를 지우지 않고, 엔트리가 0개가 된 스트림 키도 사라지지 않습니다. 그룹 상태를 잃지 않기 위해서입니다(공식 문서 streams "Zero length streams"). 그래서 스트림은 트리밍하지 않으면 메모리에서 계속 커집니다. 트리밍은 `XADD`에 옵션으로 붙이거나 `XTRIM`으로 따로 실행합니다.

- **`MAXLEN n`**: 엔트리를 최대 n개만 남깁니다.
- **`MINID id`**: 그 ID보다 작은 엔트리를 지웁니다. ID가 자동 생성이라면 ID의 앞부분이 밀리초이므로, "24시간 전 시각"을 ID로 주어 시간 기준 보관을 흉내 낼 수 있습니다. 이것은 ID 형식에서 끌어낸 사용법이고, 직접 ID를 지정하는 스트림에서는 맞지 않습니다.

얼마나 남길지는 소비자가 멈출 수 있는 시간으로 정합니다. 기본값 `KEEPREF`의 트리밍은 그룹이 아직 받아 가지 않았거나 ACK하지 않은 엔트리도 오류 없이 지우기 때문입니다. 그래서 남기는 양은 "생산 속도 × 소비자가 가장 오래 멈춰 있을 수 있는 시간(장애 복구, 긴 배포)"보다 크게 잡고, 그만큼의 메모리를 예산에 넣습니다. 원리는 [메시지 큐 기초](../cs/message-queue.md)의 "소비가 생산보다 느리면 밀린 양이 쌓이고, 결국 무언가를 잘라 내야 합니다"에서 다룹니다.

### `~`는 덩어리째 지우고, 정확한 트리밍은 표시만 남길 수 있습니다

| 방식 | 하는 일 | 비용과 결과 |
| --- | --- | --- |
| `MAXLEN ~ n` (근사) | 덩어리 전체를 없앨 수 있을 때만 덩어리째 지웁니다(기본값 `KEEPREF`일 때) | 해제 한 번으로 엔트리 여러 개가 사라집니다. 대신 n개보다 많이 남을 수 있습니다. 한 번에 지우는 양에 기본 상한이 있고 `LIMIT`으로 바꿀 수 있습니다 |
| `MAXLEN n` (정확) | 덩어리째 지울 수 없으면 덩어리 안의 엔트리에 "삭제됨" 표시를 합니다 | 정확히 n개가 남습니다. 지울 양에 제한이 없어 한 번에 많이 지우면 그 시간 동안 메인 스레드가 다른 명령을 처리하지 못합니다. 표시만 한 자리의 메모리는 덩어리째 사라질 때까지 남습니다 |

`~`가 덩어리째 지우는 것은 기본값 `KEEPREF`일 때만입니다. 아래에서 볼 `ACKED`·`DELREF`와 함께 쓰면, 덩어리째 없앨 수 있는 덩어리라도 엔트리마다 그룹 장부를 확인해야 하므로 엔트리마다 "삭제됨" 표시를 합니다. 덩어리의 엔트리가 모두 지워졌을 때만 덩어리를 해제합니다.

예시로 따라가면(기본값 `KEEPREF`), #101~#103은 덩어리 하나에 있습니다.

- `XTRIM order:completed MAXLEN ~ 2`는 **아무것도 지우지 않습니다.** 덩어리째 지우면 0개가 남아 2개를 지킬 수 없기 때문입니다.
- `XTRIM order:completed MAXLEN 2`는 #101에 "삭제됨" 표시를 합니다. `XLEN`은 2가 되지만, 덩어리가 차지한 메모리는 그대로입니다.

`XDEL`로 특정 엔트리를 지울 때도 같습니다. 표시만 하므로 개인정보 때문에 지웠더라도 메모리 사용량은 바로 줄지 않습니다.

### 트리밍과 장부가 부딪칠 때: KEEPREF, DELREF, ACKED

아직 PEL에 있는 엔트리를 트리밍하면 어떻게 될까요? 예시에서 point 그룹의 P1이 `1000-0`(#101)을 아직 ACK하지 않았는데 `XTRIM order:completed MAXLEN 2`가 실행됐다고 합시다. 이때의 동작은 트리밍 옵션으로 고릅니다.

| 옵션 | #101은 | point 그룹의 PEL에서 `1000-0`은 | 결과 |
| --- | --- | --- | --- |
| `KEEPREF` (기본값) | 지웁니다 | 그대로 남습니다 | P1이 자기 장부를 다시 읽으면 ID와 null을 받습니다. `XAUTOCLAIM`은 이런 기록을 PEL에서 지우고 "지워진 ID" 목록으로 돌려줍니다. #101의 포인트는 적립되지 않습니다 |
| `DELREF` | 지웁니다 | 모든 그룹의 PEL에서 지웁니다 | 장부까지 깨끗해지지만, #101은 처리되지 않은 채 흔적 없이 사라집니다 |
| `ACKED` | **지우지 않습니다** | 그대로 | 모든 그룹이 받아 갔고 ACK까지 한 엔트리만 지웁니다 |

`ACKED`가 말하는 "모든 그룹이 끝냈다"는 두 조건입니다. 모든 그룹의 진행 위치가 그 엔트리를 지났고, 어느 그룹의 PEL에도 없어야 합니다. 그룹이 하나도 없으면 아무 엔트리도 붙잡혀 있지 않으므로 평소처럼 지웁니다.

`ACKED`의 대가는 무엇이 엔트리를 붙잡고 있느냐에 따라 막히는 범위가 다릅니다.

- **진행 위치가 뒤처진 그룹**: "아직 받아 가지 않았다"는 판단은 모든 그룹의 진행 위치 중 가장 작은 값과 비교해 내립니다. 그래서 더 이상 아무도 읽지 않는 그룹이 하나 남아 있으면, 그 그룹의 위치 뒤의 엔트리는 모두 지울 수 없어 그 지점부터 트리밍이 막힙니다. 스트림이 계속 커집니다.
- **회수되지 않은 PEL 기록**: 그 엔트리 하나만 건너뛰고, 같은 덩어리의 다른 엔트리와 다음 덩어리는 계속 지웁니다. 다만 남은 엔트리 때문에 그 덩어리는 덩어리째 해제되지 못해 덩어리의 메모리가 남습니다.
- **CPU 비용(추론, 재현하지 않음)**: 뒤처진 그룹이 있으면 `ACKED` 트리밍은 지우지 못하는 엔트리를 호출마다 끝까지 훑을 수 있습니다. 한 번에 하는 일의 제한(`LIMIT`)은 살펴본 양이 아니라 지운 양을 기준으로 세므로, 아무것도 지우지 못하면 근사 트리밍의 기본 제한에도 걸리지 않기 때문입니다. 소스를 읽고 끌어낸 결론이며 실측하지 않았습니다.

그래서 쓰지 않는 그룹은 `XGROUP DESTROY`로 지우고, 오래된 PEL 기록은 회수합니다.

ACK와 삭제를 한 명령으로 하는 `XACKDEL`과, 같은 세 옵션을 받는 삭제 명령 `XDELEX`도 있습니다.

### 키째 사라지면 그룹과 장부도 사라지고, 메모리가 차면 생산부터 막힙니다

오래된 엔트리를 지우려고 트리밍 대신 키에 만료 시간(TTL)을 걸면 안 됩니다. 그룹, 소비자, PEL은 모두 스트림 키의 값 안에 들어 있어서, 키가 사라지면 함께 사라집니다. 키가 사라지는 경로는 셋입니다.

- **만료**: 키에 만료 시간을 걸면 그 시각이 지난 뒤 키 전체가 지워집니다. 오래된 엔트리 일부가 아니라 그룹까지 통째입니다.
- **삭제**: `DEL`·`UNLINK`로 키를 지우거나 데이터 전체를 지울 때입니다.
- **축출**: Redis는 메모리가 상한(`maxmemory`)에 닿으면 축출 정책(`maxmemory-policy`)에 따라 키를 골라 지웁니다. `allkeys-*` 정책이면 스트림 키도 후보가 됩니다. `volatile-*` 정책은 만료 시간이 있는 키만 지웁니다(공식 문서 eviction).

키가 사라진 뒤 P1이 `XREADGROUP`을 보내면 `NOGROUP` 에러를 받습니다. Spring 컨테이너는 리스너의 예외뿐 아니라 읽기에서 난 예외에도 기본으로 구독을 취소합니다. 그래서 소비가 조용히 멈춥니다. 주문 API의 다음 `XADD`가 키를 새로 만들어도 그룹은 없으므로, 그룹을 다시 만들기 전까지 아무도 읽지 않습니다.

반대로 기본 정책인 `noeviction`에서는 키를 지우지 않습니다. 대신 메모리 상한에 닿으면 메모리를 더 쓰는 명령을 거부합니다(공식 문서 eviction).

- `XADD`는 이런 명령(`DENYOOM` 플래그)이라 거부되고, 주문 API는 에러를 받습니다.
- `XREADGROUP`, `XACK`, `XTRIM`에는 이 플래그가 없어 그대로 실행됩니다.
- 그래서 메모리가 차면 소비는 계속되는데 생산만 실패합니다. 이때 `XTRIM`으로 공간을 만듭니다. 기본값 `KEEPREF`에서는 `~`를 붙이든 안 붙이든 통째로 지울 수 있는 덩어리는 해제하므로 메모리가 돌아옵니다. 다만 정확한 트리밍은 경계에 걸린 덩어리에 "삭제됨" 표시를 남기고, 한 번에 지우는 양에 상한이 없어 많이 지우면 그동안 메인 스레드를 잡습니다. 그래서 `~`를 붙여 여러 번 나눠 지우는 쪽이 안전합니다.
- 생산자 쪽의 이 실패는 메시지 큐 기초 문서의 "보내는 쪽과 브로커에도 같은 틈이 있습니다"에서 본 유실 지점입니다. 주문 API에 재시도와 경보를 둬야 합니다.

## 요청과 데이터가 늘면 무엇이 달라지는가

- **추가는 싸지만, 트리밍이 붙으면 지우는 양만큼 일이 늘어납니다.** `XADD`의 공식 복잡도는 추가만 하면 O(1), 트리밍을 하면 지운 엔트리 수 N에 대해 O(N)입니다. 여기서 O(1)은 "엔트리 수와 상관없이 거의 일정한 일", O(N)은 "N에 비례하는 일"이라는 뜻입니다. 근사 트리밍은 덩어리 해제 한 번에 여러 엔트리를 지우므로 엔트리 수에 비해 실제 일이 작습니다.
- **기다리는 소비자가 많을수록 `XADD`가 그들을 깨우는 일을 합니다.** 공식 복잡도 설명은 N개의 연결이 기다리면 `XADD`가 O(N)을 치른다고 적습니다. 이 일은 `XADD`를 처리한 메인 스레드가 이벤트 루프로 돌아가기 전에 합니다.
- **ACK하지 않은 메시지마다 장부 기록이 생깁니다.** PEL은 엔트리와 별개로 메시지마다 기록을 하나씩 따로 둡니다. 엔트리처럼 덩어리로 묶이지 않으므로, ACK가 밀리면 엔트리 크기와 별개로 메모리가 늘어납니다. `XPENDING`에 `IDLE`을 주면 살펴본 기록 수에 비례하는 시간이 듭니다.
- **스트림 키 하나는 메인 스레드 하나, 클러스터라면 노드 하나에 묶입니다.** 소비자를 늘려도 그 키에 대한 `XADD`, `XREADGROUP`, `XACK`는 모두 그 노드의 메인 스레드가 차례로 실행합니다. 이 한계를 넘으려면 스트림 키를 여러 개로 나눠야 하고, 그러면 순서는 키 안에서만 지켜집니다.
- **서버 실행 시간과 소비자가 느끼는 지연은 다릅니다.** 공식 문서의 지연 측정 결과는 "생산부터 소비자 수신까지"를 잰 것이고, 테스트 환경에 묶인 수치입니다(공식 문서 streams "Latency tests results"). 실제 지연은 네트워크 왕복, 소비자의 처리 시간, 회수까지의 `min-idle-time`을 더해서 봐야 합니다.

## 그룹 상태도 복제되지만, 복제는 비동기입니다

이 절에 필요한 용어부터 정리합니다.

- **복제**: 쓰기를 받는 Redis 서버(**주 서버**)가 데이터를 바꾼 명령의 흐름을 다른 Redis 서버(**복제본**)에 보내, 복제본이 주 서버와 같은 데이터를 갖게 하는 기능입니다(공식 문서 replication).
- **장애 조치**: 주 서버가 죽었을 때 복제본 하나를 새 주 서버로 올리는 일입니다. Sentinel이나 Redis Cluster가 맡습니다(공식 문서 wait "Consistency and WAIT").
- **RDB와 AOF**: Redis가 메모리의 데이터를 디스크에 남기는 두 방식입니다. RDB는 정해 둔 간격마다 그 시점의 데이터 전체를 파일 하나로 떠 둡니다. AOF는 받은 쓰기 명령을 파일 끝에 계속 덧붙여 두었다가, 재시작할 때 다시 실행해 데이터를 되살립니다(공식 문서 persistence). AOF는 기본으로 꺼져 있습니다.
- **fsync**: 운영체제가 메모리에 잠시 모아 둔 파일 쓰기를 디스크에 실제로 내려 쓰게 하는 호출입니다. AOF는 이를 쓰기마다(`always`), 1초마다(`everysec`, 기본값), 운영체제에 맡기기(`no`) 중에서 고릅니다. `everysec`이면 정전 같은 장애 때 보통 1초 안팎의 쓰기를 잃을 수 있고, 디스크가 느리면 더 늘 수 있습니다(공식 문서 persistence "How durable is the append only file?", [Redis 영속성](./persistence.md)).
- **`WAIT`**: 이 연결로 지금까지 보낸 쓰기를 지정한 수의 복제본이 받았다고 확인할 때까지, 또는 시간이 다 될 때까지 응답을 미루는 명령입니다(공식 문서 wait).

이제 스트림에 적용합니다.

- 스트림과 그룹 상태는 다른 자료형처럼 RDB·AOF에 저장되고 복제본에 전달됩니다. 소비자 그룹으로 읽어 PEL과 그룹 위치가 바뀌면, 그 변화도 복제본과 AOF에 전달됩니다. 그래서 주 서버의 PEL이 복제본에도 생깁니다(공식 문서 streams "Persistence, replication and message safety").
- 하지만 기본 복제는 비동기입니다. 주 서버는 복제본이 명령을 받았는지 매번 기다리지 않고 클라이언트에 응답합니다(공식 문서 replication). 그래서 장애 조치 뒤 새 주 서버에는 `XADD`나 그룹 상태 변경 같은 마지막 명령이 없을 수 있습니다(공식 문서 streams 같은 절).
- 빠진 명령에 따라 결과가 다릅니다. 아래 세 줄은 각 명령이 바꾸는 상태(엔트리, PEL, 그룹 위치)에서 끌어낸 **추론**이며, 장애 조치를 재현해 확인하지는 않았습니다.
  - `XADD`가 빠지면 메시지가 사라집니다.
  - `XACK`가 빠지면 끝난 메시지가 PEL에 다시 나타나 한 번 더 처리됩니다.
  - `XREADGROUP`의 장부 기록이 빠지면, 이미 넘긴 메시지를 새 주 서버는 "아직 안 넘김"으로 알고 다시 줍니다.
- `WAIT`로 복제를 기다리면 잃을 가능성이 크게 줄지만, 공식 문서는 장애 조치가 가장 최신 복제본을 고르는 것을 최선 노력으로만 한다고 밝힙니다. 메시지를 잃으면 안 된다면 AOF를 켜고 fsync 정책을 강하게 두는 것도 필요합니다(공식 문서 streams 같은 절).

결국 Streams를 써도 "소비자는 같은 메시지를 다시 받을 수 있다"는 전제는 그대로입니다. 장애 조치까지 생각하면 멱등 처리가 더 필수가 됩니다.

## 흔한 오해 모아 보기

| 오해 | 실제 | 본문 |
| --- | --- | --- |
| `XACK`하면 엔트리가 지워진다 | PEL에서만 지웁니다. 엔트리는 트리밍하거나 `XDEL`·`XACKDEL`로 지울 때까지 남습니다 | PEL과 XACK |
| 처리하지 못한 메시지는 시간이 지나면 다시 온다 | Redis는 자동으로 재전달하지 않습니다. `XCLAIM`·`XAUTOCLAIM`을 누군가 실행해야 합니다 | 직접 회수 |
| `BLOCK`으로 기다리면 Redis 서버가 막힌다 | 기다리는 것은 그 연결뿐입니다. 메인 스레드는 다른 명령을 계속 처리합니다 | 블로킹 읽기 |
| `lag`이 0이면 다 처리됐다 | `lag`은 그룹에 아직 넘기지 않은 수입니다. 넘겼지만 끝나지 않은 수는 `pending`입니다 | lag과 pending |
| 엔트리 ID로 중복을 거를 수 있다 | 생산자가 다시 보내면 새 ID가 붙습니다. 업무 ID로 거릅니다 | 엔트리 ID |
| `XDEL`하면 메모리가 바로 준다 | 덩어리 안에 "삭제됨" 표시만 합니다. 덩어리째 비어야 메모리를 돌려줍니다 | 트리밍 |
| `MAXLEN ~ n`이면 n개 근처로 줄어든다 | 기본값 `KEEPREF`에서는 덩어리째 지울 수 있을 때만 지우므로, 작은 스트림에서는 아무것도 지우지 않을 수 있습니다 | 트리밍 |
| 트리밍은 끝나지 않은 메시지를 건드리지 않는다 | 기본값 `KEEPREF`는 PEL에 있는 엔트리도 지웁니다. 지키려면 `ACKED`를 씁니다 | KEEPREF, DELREF, ACKED |
| 소비자 그룹은 Kafka처럼 파티션을 나눈다 | 먼저 요청한 소비자에게 줍니다. 스트림 키 하나는 노드 하나에 있습니다 | 나누는 기준 |
| 스트림 키에 TTL을 걸면 오래된 메시지만 정리된다 | 키 전체가 지워져 그룹과 PEL도 사라집니다. 오래된 엔트리는 트리밍으로 지웁니다 | 키째 사라지면 |
| 그룹 상태가 복제되니 장애 조치에도 안전하다 | 복제는 비동기라 마지막 `XADD`·`XACK`가 빠질 수 있습니다 | 복제 |

## 근거

Redis 8.2.10 소스(태그 `8.2.10`, 커밋 `86b20b1b9e5f`). 본문에서 버전 없이 적은 소스 파일은 모두 이 태그입니다:

- [`src/stream.h`](https://github.com/redis/redis/blob/8.2.10/src/stream.h): `streamID`(밀리초와 순번), `stream`, `streamCG`(`last_id`, `pel`, `consumers`), `streamConsumer`, `streamNACK`(`delivery_time`, `delivery_count`, `consumer`)
- [`src/t_stream.c`](https://github.com/redis/redis/blob/8.2.10/src/t_stream.c):
  - `streamNextID`, `streamAppendItem`: ID 자동 생성, 마지막 ID보다 커야 함, master entry와 `STREAM_ITEM_FLAG_SAMEFIELDS`로 필드 이름 공유, 새 덩어리를 여는 조건(기존 덩어리 크기 + 새 엔트리의 필드·값 길이, 삭제 표시 포함 엔트리 수), `STREAM_LISTPACK_MAX_SIZE`
  - `streamIteratorRemoveEntry`: 삭제 표시, 덩어리가 비면 덩어리째 해제
  - `streamTrim`, `streamParseAddOrTrimArgsOrReply`: 근사 트리밍은 `KEEPREF`일 때만 덩어리째(`approx && delete_strategy == DELETE_STRATEGY_KEEPREF`), `ACKED`·`DELREF`는 엔트리마다 판단, 근사 모드는 덩어리를 없앤 뒤 남는 수가 `maxlen` 미만이면 멈춤, `LIMIT` 기본값과 판단식, `KEEPREF` 기본값, `TODO: perform a garbage collection`
  - `streamEntryIsReferenced`: `ACKED`의 판단 조건(모든 그룹의 진행 위치 중 가장 작은 값 `min_cgroup_last_id`, PEL 참조)
  - `xreadCommand`: `>`와 그 밖의 ID(`serve_history`면 자기 PEL을 다시 읽고 `BLOCK`·`NOACK` 무시), 소비자 자동 생성(`streamCreateConsumer`), `$` 처리, `CLIENT_DENY_BLOCKING`이면 null, `blockForKeys`, 키나 그룹이 없으면 `NOGROUP` 에러, `COUNT`가 없으면 0(제한 없음)
  - `xgroupCommand`: `MKSTREAM` 없이 키가 없으면 에러, 그룹이 이미 있으면 `BUSYGROUP`
  - `freeStream`: 키를 해제할 때 엔트리 덩어리와 함께 그룹(`cgroups`)도 해제
  - `streamReplyWithRange`: 그룹 위치 갱신, PEL 기록, `NOACK`, `XCLAIM` 형태로 전파, `count`가 0이면 개수 제한 없음
  - `streamReplyWithRangeFromConsumerPEL`: 자기 PEL 다시 읽기, 전달 횟수 증가, 지워진 엔트리는 null
  - `xackCommand`: PEL에서만 지우고 엔트리는 남김
  - `xclaimCommand`: `JUSTID`면 전달 횟수 유지, `RETRYCOUNT`로 직접 지정
  - `xautoclaimCommand`: `COUNT` 기본 100(`count = 100`), 한 번에 살펴보는 PEL 기록은 `COUNT`의 10배(`attempts_factor = 10`), 지워진 엔트리 정리
  - `streamCreateNACK`: 처음 넘길 때 전달 횟수 1
  - `streamDelConsumer`: 소비자를 지우면 그 PEL 기록도 지움
  - `streamReplyWithCGLag`: `lag`을 계산할 수 없으면 null
  - `streamPropagateXCLAIM`, `streamPropagateGroupID`: PEL 변화를 `XCLAIM`·`XGROUP SETID` 형태로 복제본과 AOF에 전달
- [`src/config.c`](https://github.com/redis/redis/blob/8.2.10/src/config.c): `stream-node-max-bytes` 4096, `stream-node-max-entries` 100, `appendonly` 기본값 `no`, `appendfsync` 기본값 `everysec`, `replica-read-only` 기본값 `yes`, `maxmemory-policy` 기본값 `noeviction`
- [`src/server.c`](https://github.com/redis/redis/blob/8.2.10/src/server.c)의 `processCommand`: 메모리 상한에서 축출로 공간을 못 만들면 `DENYOOM` 명령을 거부
- [`src/cluster.c`](https://github.com/redis/redis/blob/8.2.10/src/cluster.c)의 `getNodeByQuery`: `EXEC`에 모인 명령의 키가 여러 슬롯에 걸치면 `CLUSTER_REDIR_CROSS_SLOT`
- [`src/ae.c`](https://github.com/redis/redis/blob/8.2.10/src/ae.c): `aeMain`이 `aeProcessEvents`를 되풀이하는 이벤트 루프
- [`src/multi.c`](https://github.com/redis/redis/blob/8.2.10/src/multi.c)의 `execCommand`, [`src/eval.c`](https://github.com/redis/redis/blob/8.2.10/src/eval.c)의 `scriptingInit`: `CLIENT_DENY_BLOCKING` 설정
- [`src/commands/`](https://github.com/redis/redis/tree/8.2.10/src/commands): `xadd.json`, `xtrim.json`, `xread.json`, `xreadgroup.json`, `xack.json`, `xackdel.json`, `xdelex.json`, `xclaim.json`, `xautoclaim.json`, `xpending.json`, `xinfo-groups.json`, `xinfo-consumers.json`의 복잡도, 플래그(`xadd.json`·`xgroup-create.json`에만 `DENYOOM`)

공식 문서(redis/docs 커밋 `e53775d`. 8.2보다 나중 버전의 기능도 함께 실려 있으므로, 명령 문법과 동작은 8.2.10 소스를 따릅니다):

- [streams](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/develop/data-types/streams/_index.md): "Consumer groups", "Creating a consumer group", "Recovering from permanent failures", "Claiming and the delivery counter", "Enhanced deletion control in Redis 8.2", "Differences with Kafka (TM) partitions", "Capped Streams", "Persistence, replication and message safety", "Zero length streams", "How serving blocked consumers works", "Latency tests results"
- [xreadgroup](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/xreadgroup.md): `NOACK`는 읽는 순간 ACK하는 것과 같다는 설명, `>`와 그 밖의 ID
- [xclaim](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/xclaim.md): 동시에 회수해도 하나만 성공한다는 설명
- [pubsub](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/develop/pubsub/_index.md): "Delivery semantics"
- [lmove](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/lmove.md): "Pattern: reliable queue"
- [replication](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/operate/oss_and_stack/management/replication.md): 주 서버가 복제본에 명령의 흐름을 보내는 방식, 기본은 비동기 복제
- [persistence](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/operate/oss_and_stack/management/persistence.md): RDB와 AOF의 정의, "How durable is the append only file?"의 `appendfsync` 세 정책
- [wait](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/wait.md): `WAIT`의 동작, "Consistency and WAIT"의 Sentinel·Cluster 장애 조치와 최선 노력
- [eviction](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/develop/reference/eviction/index.md): `noeviction`은 키를 지우지 않고 메모리를 더 쓰는 명령에 에러, `allkeys-*`는 모든 키, `volatile-*`는 만료 시간이 있는 키만 축출
- [info](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/info.md): `evicted_keys`

Spring Data Redis 3.5.13(4.0.7의 같은 파일에서 `autoAck` 기본값, `cancelSubscriptionOnError` 기본값, `pollTimeout` 기본값, 리스너 예외 처리, `batchSize`와 `executor` 기본값, 읽기 예외 처리, `createGroup`의 `MKSTREAM`과 기본 시작 위치가 같음을 확인):

- [`StreamMessageListenerContainer.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/stream/StreamMessageListenerContainer.java): `receive`는 `autoAcknowledge(false)`, `receiveAutoAck`는 `autoAcknowledge(true)`, `ConsumerStreamReadRequestBuilder`의 `autoAck = true`, `cancelSubscriptionOnError = t -> true`, `pollTimeout = Duration.ofSeconds(2)`, `StreamMessageListenerContainerOptionsBuilder`의 `batchSize`(기본 없음)와 `executor = new SimpleAsyncTaskExecutor()`
- [`DefaultStreamMessageListenerContainer.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/stream/DefaultStreamMessageListenerContainer.java): `getReadFunction`(`autoAck`이면 `autoAcknowledge()` 읽기 옵션), `getStreamReadOptions`(`pollTimeout`을 `BLOCK`으로, `batchSize`가 있을 때만 `COUNT`)
- [`StreamReadOptions.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/stream/StreamReadOptions.java): `autoAcknowledge()`가 `NOACK` 플래그
- [`StreamPollTask.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/stream/StreamPollTask.java): `doLoop`(읽기와 리스너 호출을 차례로 반복, 읽기 예외에도 `cancelSubscriptionOnError`로 구독 취소 판단), `deserializeAndEmitRecords`(받은 레코드를 반복문으로 리스너에 넘김, 리스너 예외 시 구독 취소 판단). 기본 에러 핸들러 `LoggingErrorHandler`는 로그만 남김
- [`ReadOffsetStrategy.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/stream/ReadOffsetStrategy.java), [`ReadOffset.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/stream/ReadOffset.java): 그룹으로 `lastConsumed()`(`>`)를 쓰면 다음 읽기도 계속 `>`
- [`LettuceStreamCommands.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceStreamCommands.java): `xReadGroup`이 `BLOCK`이면 전용 연결 사용, `xGroupCreate`가 `mkstream` 인자를 그대로 전달
- [`DefaultStreamOperations.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/core/DefaultStreamOperations.java)의 `createGroup`(`xGroupCreate(..., true)`로 언제나 `MKSTREAM`), [`StreamOperations.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/core/StreamOperations.java)의 `createGroup(key, group)`(`ReadOffset.latest()`), `ReadOffset.latest()`는 `$`

## 실무 포인트

### 어떤 방법을 고를까

| 요구사항 | 선택 | 고르지 않은 쪽과 이유 |
| --- | --- | --- |
| 놓쳐도 되는 실시간 알림(접속자 수, 화면 갱신 신호) | Pub/Sub | Streams는 저장과 트리밍 관리가 필요합니다. 놓쳐도 되면 그 비용이 필요 없습니다 |
| 목적 하나, 단순한 작업 분배, 가끔의 유실 허용 | List | Streams의 그룹·PEL·회수 운영이 과합니다. 유실을 줄이려면 `LMOVE` 패턴을 직접 만들어야 합니다 |
| 빠지면 안 되고, 처리 중 죽은 메시지를 되찾아야 함 | Streams + 처리 후 `XACK` + 회수 + 멱등 처리 | List로는 장부와 회수를 앱이 다 만들어야 합니다 |
| 같은 이벤트를 여러 서비스가 각자 처리 | Streams, 서비스마다 그룹 하나 | List는 목적마다 리스트를 따로 두고 생산자가 각각 넣어야 합니다 |
| 오래 보관하고, 장애 조치에도 유실을 거의 허용하지 않으며, 처리량이 노드 하나를 넘음 | Redis 밖의 전용 브로커를 검토 | 스트림 키는 메모리에 있고 복제는 비동기이며 키 하나가 노드 하나에 묶입니다 |

### 중요도별 점검 항목

| 중요도 | 항목 | 이유 |
| --- | --- | --- |
| 필수 | 처리 후 `XACK`, `NOACK`·자동 ACK를 쓰는지 확인 | 자동 ACK는 PEL에 남기지 않으므로, 소비자가 죽으면 회수할 수 없습니다 |
| 필수 | 주문 ID 같은 업무 ID로 멱등 처리 | 회수, 재시작, 장애 조치 모두 같은 메시지를 다시 줄 수 있습니다 |
| 필수 | 회수(`XAUTOCLAIM`)를 누가 언제 실행하는지 정하기 | Redis는 자동으로 재전달하지 않습니다 |
| 필수 | 트리밍 정책(`MAXLEN ~` 또는 `MINID ~`)과 메모리 상한 정하기 | ACK해도 지워지지 않으므로 메모리에서 계속 커집니다 |
| 필수 | `lag`과 `pending`을 함께 관찰 | 한쪽만 보면 "다 넘겼지만 처리 안 됨"이나 "넘기지도 못함"을 놓칩니다 |
| 필수 | Spring 컨테이너에 `batchSize` 지정 | 지정하지 않으면 밀린 엔트리를 한 번에 PEL로 가져와 한 스레드가 차례로 처리합니다. 뒤쪽 엔트리가 회수되어 두 번 처리될 수 있습니다(추론) |
| 필수 | 스트림 키에 TTL을 걸지 않고, 축출 정책과 메모리 상한 확인 | 키가 사라지면 그룹과 PEL도 사라져 소비가 멈춥니다. `noeviction`에서는 메모리가 차면 `XADD`가 거부됩니다 |
| 알면 유리 | 그룹 생성 코드에서 `BUSYGROUP`을 정상으로 처리하고 시작 위치를 명시 | 두 번째로 뜨는 인스턴스부터는 항상 이 에러를 받습니다. Spring의 `createGroup(key, group)`은 `$`로 만들어 이미 쌓인 엔트리를 건너뜁니다 |
| 알면 유리 | 재시작 시 `0`으로 자기 PEL부터 읽기 | `>`만 읽으면 자기 PEL의 메시지를 다시 보지 않습니다 |
| 알면 유리 | 전달 횟수로 독 메시지를 데드 레터 스트림에 옮기기(MULTI/EXEC로 묶기, 클러스터에서는 두 키를 같은 슬롯에) | 계속 실패하는 메시지가 회수될 때마다 워커 시간을 씁니다 |
| 알면 유리 | 소비자 이름을 인스턴스마다 고정, 지우기 전에 회수 | 무작위 이름은 PEL을 쥔 소비자를 쌓고, `DELCONSUMER`는 그 PEL을 지웁니다 |
| 알면 유리 | `ACKED` 트리밍과 쓰지 않는 그룹 정리 | 끝나지 않은 메시지를 지키는 대신, 뒤처진 그룹 하나가 그 위치부터의 트리밍을 막을 수 있습니다. 회수되지 않은 PEL 기록은 그 엔트리가 든 덩어리를 남깁니다 |
| 참고 | 처리 결과가 같은 Redis에 있으면 Lua로 처리와 ACK 묶기 | 같은 슬롯 제약과 스크립트 시간 대가가 따릅니다 |

### Spring에서 확인할 것

- **자동 ACK가 기본인 경로가 있습니다.** `receiveAutoAck(...)`, 그리고 소비자를 지정하되 `autoAcknowledge`를 지정하지 않고 만든 `StreamReadRequest`는 자동 ACK입니다. 자동 ACK는 `XREADGROUP`에 `NOACK`를 붙여 읽습니다. 즉 메시지가 PEL에 들어가지 않으므로 최대 한 번이고, `XAUTOCLAIM`으로도 되찾을 수 없습니다. 빠지면 안 되는 일에는 `receive(...)`나 `autoAcknowledge(false)`를 쓰고, 처리 뒤 `StreamOperations.acknowledge`로 직접 ACK합니다.
- **리스너에서 예외가 나면 기본으로 구독이 멈춥니다.** 기본 `cancelOnError`가 모든 예외에 `true`라서, 리스너가 예외를 던지면 그 구독은 취소되고 기본 에러 핸들러는 로그만 남깁니다. 애플리케이션은 살아 있는데 소비만 조용히 멈추는 증상이 됩니다. 메시지 하나의 실패로 멈추지 않게 하려면 `cancelOnError`를 바꾸고, `Subscription.isActive()`를 상태 점검에 넣습니다.
- **`ReadOffset.lastConsumed()`는 계속 `>`로 읽습니다.** 그룹과 함께 쓰면 다음 읽기도 언제나 `>`입니다. 재시작한 인스턴스의 자기 PEL을 다시 읽어 주지 않으므로, 시작할 때 자기 PEL을 처리하거나 `XAUTOCLAIM` 작업을 따로 둬야 합니다.
- **기다리는 동안 구독마다 전용 연결을 하나씩 씁니다.** 기본 `pollTimeout`은 2초이고 이것이 `XREADGROUP`의 `BLOCK 2000`이 됩니다. Lettuce에서는 블로킹 읽기를 다른 명령과 섞이지 않는 전용 연결로 실행하므로, 동시에 기다리는 구독 수만큼 연결이 쓰입니다. 연결 풀을 쓴다면 풀 크기를 이 수에 맞춰 봅니다.
- **`batchSize`를 지정하지 않으면 밀린 엔트리를 한 번에 모두 받아, 한 스레드에서 차례로 처리합니다.**
  - 컨테이너 옵션에 `batchSize`가 없으면 `XREADGROUP`에 `COUNT`를 붙이지 않습니다. Redis는 `COUNT`가 없으면 개수 제한 없이 돌려주므로, 그 순간 밀려 있던 새 엔트리가 모두 한 응답에 담기고 모두 그 소비자의 PEL에 올라갑니다.
  - 받은 엔트리는 구독마다 하나인 폴링 작업이 반복문으로 하나씩 리스너에 넘기고, 다 넘긴 뒤에야 다음 `XREADGROUP`을 보냅니다. 기본 실행기는 작업마다 스레드를 만드는 `SimpleAsyncTaskExecutor`라서, 구독마다 스레드 하나가 이 반복을 맡습니다.
  - 예시로 따라가면, P1과 P2가 모두 배포로 멈춘 사이 주문이 쌓였다가 P1이 먼저 돌아오면 P1이 밀린 엔트리를 한 번에 가져갑니다. 예시처럼 세 건이면 문제가 없지만, 수천 건이면 사정이 다릅니다.
  - **추론, 재현하지 않음**: 한 번에 받은 엔트리를 모두 처리하는 데 `min-idle-time`보다 오래 걸리면, P1이 아직 손대지 않은 뒤쪽 엔트리를 P2의 `XAUTOCLAIM`이 가져갑니다. P1은 메모리에 이미 받아 둔 그 엔트리도 차례가 오면 처리하므로 같은 메시지가 두 번 처리됩니다. 또 일이 P1에 몰려 P2는 새 엔트리를 받지 못하고 놀 수 있습니다.
  - 그래서 `batchSize`를 지정하고, "한 번에 받는 수 × 메시지 하나의 처리 시간"이 `min-idle-time`보다 충분히 작게 잡습니다. 너무 작으면 `XREADGROUP` 왕복이 늘어납니다. 그래도 생기는 중복은 멱등 처리로 견딥니다.
- **필드 값의 직렬화를 확인합니다.** 스트림 엔트리는 필드-값 쌍이고, 객체를 넣으면 템플릿의 직렬화 설정에 따라 바이트로 바뀝니다. 생산자와 소비자가 다른 직렬화 설정을 쓰면 소비자에서 변환 예외가 나고, 위의 "구독이 멈춤"으로 이어집니다.

### 관찰할 것

- `XINFO GROUPS <key>`: 그룹마다 `lag`(아직 안 넘긴 수), `pending`(넘겼지만 끝나지 않은 수), `last-delivered-id`.
- `XPENDING <key> <group>`: 가장 작은 끝나지 않은 ID. ID가 자동 생성이라면 그 ID의 밀리초와 현재 시각의 차이가 "가장 오래 처리되지 않은 메시지가 기다린 시간"의 근사값입니다. 범위 형태로는 메시지별 idle 시간과 전달 횟수를 봅니다.
- `XINFO CONSUMERS <key> <group>`: 소비자별 `pending`, `idle`, `inactive`. 오래 `inactive`인데 `pending`이 남은 소비자가 회수 대상입니다.
- `XLEN <key>`와 키의 메모리 사용량: 트리밍이 생산 속도를 따라가는지 봅니다. 정확한 트리밍이나 `XDEL`을 쓰면 `XLEN`이 줄어도 메모리는 덜 줄 수 있습니다.
- 앱 지표: 처리 성공·실패 수, 데드 레터로 옮긴 수, 회수한 수.

### 증상과 대응

| 증상 | 의심할 원인 | 확인 | 대응과 부작용 |
| --- | --- | --- | --- |
| `pending`이 계속 늘고 `lag`은 0 | 소비자가 죽었거나 ACK를 빠뜨림 | `XINFO CONSUMERS`의 `inactive`, `XPENDING` 범위 조회의 소유자와 idle | 회수 작업 추가. `min-idle-time`이 짧으면 중복 처리가 늘어납니다 |
| `lag`이 계속 늚 | 소비가 생산을 못 따라감 | 소비자 수, 처리 시간, 처리 실패율 | 소비자 추가. 순서가 더 섞이고 공유 DB 부하가 늘어납니다 |
| 앱은 정상인데 소비가 멈춤 | 리스너 예외로 Spring 구독이 취소됨 | 에러 로그, `Subscription.isActive()` | `cancelOnError` 조정, 독 메시지 격리 |
| 소비가 멈추고 `NOGROUP` 에러가 남음 | 스트림 키가 만료·삭제·축출되어 그룹이 사라짐 | 키의 TTL, `maxmemory-policy`, `INFO stats`의 `evicted_keys` | TTL 제거, 축출 정책 재검토, 그룹 다시 만들기. 다시 만들 때 시작 위치를 정해야 하며, 사라진 PEL의 메시지는 되찾을 수 없습니다 |
| 같은 메시지를 두 인스턴스가 처리하고, 일이 한 인스턴스에 몰림 | Spring `batchSize`를 지정하지 않아 한 번에 너무 많이 받음 | 소비자별 `pending`(`XINFO CONSUMERS`), 회수한 수 | `batchSize` 지정. 너무 작으면 왕복이 늘어납니다 |
| 주문 API의 `XADD`가 메모리 부족 에러를 받음 | `noeviction`에서 메모리 상한에 닿음 | `used_memory`와 `maxmemory`, `XLEN`, 트리밍 설정 | 근사 트리밍으로 나눠 지워 공간 확보, 상한 재검토. 정확한 트리밍으로 한 번에 많이 지우면 그동안 다른 명령이 기다립니다 |
| 두 번째 인스턴스가 기동 중 실패 | 이미 있는 그룹을 다시 만들다 `BUSYGROUP` 예외 | 기동 로그 | `BUSYGROUP`을 정상 상황으로 처리 |
| 포인트가 두 번 적립됨 | 회수·재시작·장애 조치로 인한 재전달 | 같은 주문 ID 처리 로그, 전달 횟수 | 업무 ID 기준 멱등 처리. 처리 기록과 처리를 같은 원자적 단위로 묶습니다 |
| 포인트가 적립되지 않았는데 PEL에도 없음 | 자동 ACK(`NOACK`), 트리밍(`KEEPREF`·`DELREF`)으로 지워짐, `DELCONSUMER` | 리스너 설정, 트리밍 옵션, `XAUTOCLAIM`의 지워진 ID 목록 | 처리 후 ACK로 바꾸기, `ACKED` 트리밍 검토 |
| 메모리가 계속 늚 | 트리밍 없음, `ACKED`를 막는 버려진 그룹, 쌓이는 PEL | `XLEN`, `XINFO GROUPS`, 키 메모리 사용량 | 근사 트리밍 추가, 쓰지 않는 그룹 `XGROUP DESTROY`, 회수 |
| `XADD`·`XTRIM` 직후 다른 명령의 지연이 튐 | 정확한 트리밍으로 한 번에 많이 지움 | 느린 명령 기록(`SLOWLOG`) | `~`로 바꾸기. 대신 지정한 개수보다 조금 더 남습니다 |

## 말로 설명하기

### 30초 버전

Redis Streams는 메시지를 지우지 않고 뒤에 덧붙이는 로그입니다. 엔트리마다 밀리초와 순번으로 된 ID가 붙어 순서와 위치가 됩니다. 소비자 그룹을 만들면 그룹 안의 소비자는 메시지를 나눠 받고, 그룹끼리는 모두 받습니다. 넘겼지만 ACK를 받지 못한 메시지는 PEL이라는 장부에 남습니다. 그래서 소비자가 죽어도 메시지를 잃지 않습니다. 다만 Redis가 자동으로 다시 주지 않으므로 `XAUTOCLAIM`으로 직접 회수해야 하고, 재전달로 생기는 중복은 업무 ID로 멱등 처리합니다. 또 ACK해도 엔트리가 지워지지 않으므로 트리밍 정책이 필요합니다.

### 2분 버전

Redis에서 메시지를 넘기는 방법은 셋입니다. Pub/Sub은 저장하지 않아 듣지 않던 소비자는 메시지를 놓칩니다. List는 꺼내면 빠지므로 꺼낸 직후 죽으면 잃습니다. Streams는 엔트리를 남기고, 그룹마다 "어디까지 나눠 줬는지"와 "누가 무엇을 처리 중인지"를 장부로 둡니다.

`XREADGROUP`에 `>`를 주면 그 그룹에서 아직 아무에게도 주지 않은 엔트리를 받습니다. 이때 다음 엔트리 고르기, 위치 옮기기, PEL 기록이 명령 하나 안에서 끝나므로 두 소비자가 동시에 읽어도 같은 메시지를 받지 않습니다. 나누는 기준은 파티션이 아니라 먼저 요청한 소비자입니다.

처리가 끝나면 `XACK`로 PEL에서 지웁니다. 처리와 ACK는 보통 다른 저장소라 묶이지 않으므로, 처리 후 ACK를 쓰면 최소 한 번이 되고 중복을 멱등 처리로 견딥니다. 처리 결과도 같은 Redis에 있다면 Lua 스크립트로 "확인, 처리, 처리 기록, ACK"를 한 덩어리로 묶을 수 있습니다. 하지만 같은 슬롯 제약과 스크립트 시간 대가가 따릅니다.

소비자가 죽으면 그 PEL 메시지는 그대로 남고, 누군가 `XAUTOCLAIM`으로 일정 시간 이상 처리되지 않은 메시지를 가져와야 합니다. 전달 횟수가 쌓인 메시지는 데드 레터 스트림으로 옮깁니다. 스트림은 ACK해도 지워지지 않으므로 `MAXLEN ~`나 `MINID ~`로 덩어리째 트리밍합니다. 기본값은 끝나지 않은 메시지도 지우므로, `ACKED`로 모든 그룹이 끝낸 엔트리만 지울 수 있습니다. 이때는 엔트리마다 장부를 확인하므로 `~`를 붙여도 덩어리째 지우는 이점이 줄고, 뒤처진 그룹이 있으면 그 지점부터 트리밍이 막힙니다. 마지막으로 복제가 비동기라 장애 조치 뒤 메시지를 다시 받거나 잃을 수 있습니다.

### 예상 질문과 꼬리 질문

**Q. Redis에서 메시지 큐를 만들 때 List 대신 Streams를 쓰는 이유는 무엇인가요?**
List는 꺼내는 순간 원소가 빠지므로, 꺼낸 직후 소비자가 죽으면 메시지를 잃습니다. 처리 중 목록, 재전달 횟수, 오래 남은 메시지 되돌리기를 모두 앱이 만들어야 합니다. Streams는 엔트리를 남기고, 그룹별 진행 위치와 PEL을 Redis가 관리합니다. 그래서 죽은 소비자의 메시지를 회수할 수 있고, 같은 메시지를 여러 그룹이 각자 읽을 수 있습니다. 대신 트리밍 정책과 회수 작업을 직접 운영해야 합니다. (본문: "Redis 안에서 메시지를 넘기는 세 방법 중 Streams는 '남기고 장부를 두는' 쪽입니다")

- **꼬리: 그럼 Pub/Sub은 언제 쓰나요?**
  놓쳐도 되는 실시간 신호입니다. Pub/Sub은 저장하지 않고 최대 한 번 전달이라, 구독자가 끊겨 있던 동안의 메시지는 영영 받지 못합니다.

**Q. 같은 그룹의 소비자 두 대가 동시에 읽으면 같은 메시지를 받을 수 있나요?**
새 메시지(`>`)는 받지 않습니다. `XREADGROUP` 하나 안에서 다음 엔트리를 고르고, 그룹 위치를 옮기고, PEL에 소유자를 적는 일이 메인 스레드에서 끊기지 않고 끝나기 때문입니다. 다만 회수나 재시작으로 같은 메시지를 다시 받는 것은 정상 동작이므로, 처리 쪽은 중복을 견뎌야 합니다. (본문: "XREADGROUP 하나가 '고르기, 위치 옮기기, 장부 적기'를 한 번에 합니다")

**Q. 서비스가 뜰 때 소비자 그룹은 어떻게 만들어야 하나요?**
시작 위치와 에러 두 가지를 정해야 합니다. 시작 위치는 이미 쌓인 엔트리를 처리할지에 따라 `0`이나 `$`를 고르는데, Spring의 `createGroup(key, group)`은 `$`로 만들어 쌓인 엔트리를 건너뜁니다. 에러는 키가 아직 없을 때와 그룹이 이미 있을 때(`BUSYGROUP`) 납니다. 앞의 것은 `MKSTREAM`으로 피하고(Spring의 `createGroup`은 항상 붙임), 뒤의 것은 두 번째로 뜨는 인스턴스부터 항상 받으므로 정상 상황으로 처리해야 기동이 실패하지 않습니다. (본문: "그룹 만들기: 어디서부터 줄지 정합니다")

**Q. 엔트리 ID는 어떻게 만들어지고, 무엇을 보장하나요?**
밀리초 시각과 순번입니다. 자동 생성이면 서버 시각이 마지막 ID보다 클 때 `<시각>-0`, 아니면 마지막 ID의 순번을 1 올립니다. 그래서 시계가 뒤로 가도 ID는 언제나 커지고, ID만으로 순서와 위치를 말할 수 있습니다. 대신 밀리초는 정확한 추가 시각이 아닙니다. (본문: "엔트리 ID는 '밀리초-순번'이고, 순서와 위치가 됩니다")

- **꼬리: 엔트리 ID로 중복 메시지를 거르면 안 되나요?**
  생산자가 응답을 못 받아 다시 보내면 새 엔트리가 되어 새 ID가 붙습니다. 같은 일인지는 주문 ID 같은 업무 ID로 판단해야 합니다.

**Q. `XACK`는 무엇을 하나요? ACK하면 메모리가 줄어드나요?**
그룹과 소비자의 PEL에서 그 ID를 지웁니다. 엔트리는 지우지 않으므로 메모리는 그대로입니다. 다른 그룹이 아직 처리 중일 수 있고, 스트림은 로그이기 때문입니다. 엔트리를 지우려면 트리밍이나 `XDEL`, `XACKDEL`을 씁니다. (본문: "XACK는 장부에서 지울 뿐, 엔트리를 지우지 않습니다")

**Q. 소비자가 메시지를 받고 처리하다 죽으면 그 메시지는 어떻게 되나요?**
그 소비자 몫으로 PEL에 남습니다. Redis는 시간이 지나도 자동으로 다시 주지 않습니다. 같은 이름으로 재시작하면 `0`으로 자기 PEL을 다시 읽어 처리할 수 있고, 돌아오지 않으면 다른 소비자가 `XAUTOCLAIM`이나 `XCLAIM`으로 일정 시간 이상 처리되지 않은 메시지를 가져와야 합니다. (본문: "죽은 소비자의 메시지는 Redis가 다시 주지 않습니다: 직접 회수합니다")

- **꼬리: 두 소비자가 같은 메시지를 동시에 회수하면요?**
  하나만 성공합니다. 회수하는 순간 idle 시간이 0으로 돌아가므로, 같은 `min-idle-time`으로 뒤이어 실행된 회수는 조건에 맞지 않습니다.
- **꼬리: `min-idle-time`은 어떻게 정하나요?**
  처리 제한 시간과 같습니다. 짧으면 느릴 뿐인 소비자의 메시지를 빼앗아 중복 처리가 생기고, 길면 죽은 소비자의 메시지가 그만큼 늦게 처리됩니다. 정상 처리 시간 중 느린 쪽보다 넉넉하게 잡고, 중복은 멱등 처리로 견딥니다.
- **꼬리: 계속 실패하는 메시지는요?**
  `JUSTID` 없이 회수하면 회수될 때마다 전달 횟수가 오르므로, 정한 횟수를 넘으면 데드 레터용 스트림에 옮기고 원래 그룹에서는 ACK합니다. 옮기기와 ACK는 MULTI/EXEC로 묶으면 중간에 죽어도 두 번 옮겨지지 않습니다. 클러스터라면 두 스트림 키가 같은 슬롯에 있어야 묶을 수 있습니다.

**Q. `NOACK`는 언제 쓰나요?**
메시지를 PEL에 넣지 않고 읽는 순간 ACK한 것으로 치는 옵션이라, 최대 한 번 전달이 됩니다. 일부 유실을 받아들이고 PEL 관리 비용을 없애고 싶을 때 씁니다. 받은 직후 소비자가 죽으면 그 그룹에서는 되찾을 수 없습니다. (본문: "ACK 시점이 유실과 중복을 가르는 것은 Streams에서도 같습니다")

**Q. Lua로 정확히 한 번 처리를 만들 수 있나요?**
처리 결과가 같은 Redis의 같은 슬롯에 있을 때만 가깝게 만들 수 있습니다. "이미 처리했나 확인, 처리와 처리 기록, `XACK`"를 스크립트 하나로 실행하면 메인 스레드가 끊지 않고 실행하므로 "처리됐는데 ACK 안 됨" 상태가 남지 않고, 다시 보내도 확인 단계가 이중 처리를 막습니다. 처리 결과가 DB나 외부 API에 있으면 묶을 수 없으므로 최소 한 번과 멱등 처리로 돌아갑니다. (본문: "처리 결과도 Redis에 있으면, Lua로 처리와 ACK를 한 덩어리로 묶을 수 있습니다")

- **꼬리: 스크립트 중간에 에러가 나면요?**
  Redis는 이미 실행한 쓰기를 되돌리지 않습니다. 그래서 처리 기록을 ACK보다 먼저 남깁니다. 에러로 ACK만 빠지면 메시지가 다시 오지만, 처리 기록이 이중 처리를 막습니다. 처리와 처리 기록 사이에서 난 에러까지 막지는 못합니다.
- **꼬리: 스크립트 안에서 `XREADGROUP BLOCK`으로 기다리면 안 되나요?**
  스크립트 안에서는 기다리는 것이 허용되지 않습니다. 읽을 새 엔트리가 있으면 평소처럼 받지만, 없으면 기다리지 않고 바로 빈 결과가 돌아옵니다. 기다릴 수 있다고 해도 그동안 메인 스레드를 차지해 모든 명령이 멈춥니다. 읽기는 스크립트 밖에서 하고, 스크립트에는 메시지 하나의 처리와 ACK만 넣습니다.
- **꼬리: 클러스터에서는요?**
  스크립트가 쓰는 키를 모두 `KEYS`로 넘기고 같은 슬롯에 둬야 합니다. 스트림과 처리 결과 키를 같은 해시 태그로 묶으면 모두 한 노드에 모여, 그 노드가 처리량 상한이 됩니다.

**Q. 스트림 메모리는 어떻게 관리하나요?**
ACK해도 지워지지 않으므로 `MAXLEN`이나 `MINID`로 트리밍합니다. 기본값 `KEEPREF`에서 `~`를 붙이면 덩어리를 통째로 지울 수 있을 때만 지워 싸지만, 지정한 개수보다 조금 더 남습니다. 정확한 트리밍은 경계에 걸린 덩어리에, `XDEL`은 지운 엔트리마다 삭제 표시만 해서, 그 덩어리가 통째로 비기 전까지 그 자리의 메모리를 돌려주지 않습니다. (본문: "트리밍은 덩어리 단위로 하면 싸고, 엔트리 단위로 하면 표시만 남습니다")

- **꼬리: 아직 처리되지 않은 메시지가 트리밍되면요?**
  기본값 `KEEPREF`는 PEL에 있는 엔트리도 지우고 장부 기록만 남깁니다. 그 메시지는 처리되지 않은 채 사라지고, 회수할 때 지워진 ID로 돌아옵니다. `ACKED`를 쓰면 모든 그룹이 받아 가서 ACK까지 한 엔트리만 지울 수 있습니다.
- **꼬리: `ACKED`의 단점은요?**
  붙잡는 쪽에 따라 다릅니다. 아무도 읽지 않아 진행 위치가 뒤처진 그룹이 하나라도 있으면, 그 위치부터 지워지지 않아 스트림이 계속 커집니다. 회수되지 않은 PEL 기록은 그 엔트리만 남기지만, 그 엔트리가 든 덩어리를 해제하지 못하게 합니다. 쓰지 않는 그룹을 정리하고 오래된 PEL은 회수해야 합니다. 뒤처진 그룹이 있을 때 트리밍이 호출마다 엔트리를 훑는 CPU 비용도 생길 수 있지만, 이것은 소스에서 끌어낸 추론입니다.
- **꼬리: 스트림 키에 TTL을 걸어 오래된 메시지를 정리하면 안 되나요?**
  안 됩니다. 만료는 오래된 엔트리가 아니라 키 전체를 지우고, 그룹과 PEL도 키 안에 있어 함께 사라집니다. 그 뒤 소비자는 `NOGROUP` 에러를 받고, Spring 기본 설정에서는 구독이 취소되어 소비가 조용히 멈춥니다. `allkeys-*` 축출 정책에서도 같은 일이 생길 수 있습니다. 오래된 엔트리는 트리밍으로 지웁니다.
- **꼬리: 메모리가 상한에 닿으면 어떻게 되나요?**
  기본 정책 `noeviction`에서는 `XADD`처럼 메모리를 더 쓰는 명령만 거부되고, `XREADGROUP`·`XACK`·`XTRIM`은 실행됩니다. 그래서 소비는 계속되는데 생산자만 실패합니다. 공간은 트리밍으로 덩어리가 통째로 해제될 때 생깁니다. 한 번에 많이 지우다 메인 스레드를 잡지 않도록 근사 트리밍으로 나눠 지우고, 생산자에는 재시도와 경보를 둡니다.

**Q. `lag`이 0인데 처리가 밀렸다는 민원이 왔습니다. 어떻게 된 걸까요?**
`lag`은 그룹에 아직 넘기지 않은 엔트리 수입니다. 이미 넘겼지만 ACK되지 않은 메시지는 `pending`에 있습니다. 소비자가 죽었거나 ACK를 빠뜨렸다면 `lag`은 0이어도 `pending`이 쌓입니다. `XINFO CONSUMERS`에서 오래 `inactive`인 소비자의 `pending`을 보고, 회수 작업이 도는지 확인합니다. (본문: "lag과 pending은 다른 것을 셉니다")

**Q. Spring `StreamMessageListenerContainer`로 소비할 때 주의할 점은 무엇인가요?**
네 가지입니다.
- `receiveAutoAck`나 `autoAcknowledge`를 지정하지 않은 소비자 요청은 `NOACK`로 읽습니다. 그래서 PEL에 남지 않고 최대 한 번이 됩니다.
- 기본 설정에서는 리스너 예외 하나로 구독이 취소되고 로그만 남아, 소비가 조용히 멈출 수 있습니다. 스트림 키가 사라져 읽기에서 `NOGROUP` 에러가 나도 마찬가지입니다.
- `ReadOffset.lastConsumed()`는 계속 `>`로 읽으므로, 재시작한 인스턴스의 자기 PEL을 다시 처리해 주지 않습니다.
- `batchSize`를 지정하지 않으면 `COUNT` 없이 읽어 밀린 엔트리를 한 번에 모두 PEL로 가져오고, 구독마다 한 스레드가 차례로 처리합니다. 처리가 `min-idle-time`보다 오래 걸리면 뒤쪽 엔트리를 다른 인스턴스가 회수해 두 번 처리할 수 있습니다. 이 마지막 결과는 소스에서 끌어낸 추론입니다.

빠지면 안 되는 일이라면 `receive`로 받아 처리 후 직접 ACK하고, `cancelOnError`를 조정하고, `batchSize`를 정하고, 회수 작업을 따로 둡니다. (본문: "Spring에서 확인할 것")

**Q. Redis Streams의 소비자 그룹은 Kafka의 소비자 그룹과 무엇이 다른가요?**
Kafka는 토픽을 파티션으로 나누고 파티션을 그룹 안의 소비자에게 배정합니다. Redis는 스트림 키 하나 안에서 그때 요청한 소비자에게 엔트리를 하나씩 줍니다. 그래서 빠른 소비자가 더 많이 받고 소비자가 죽어도 새 메시지는 남은 소비자가 받지만, 같은 키 안의 순서는 소비자 사이에서 섞입니다. 노드를 넘어 나누려면 스트림 키를 여러 개 둬야 하는데, 이것이 Kafka의 파티션에 가깝습니다. (본문: "나누는 기준은 '먼저 요청한 소비자'입니다")

**Q. 장애 조치가 일어나면 Streams 메시지는 안전한가요?**
스트림과 그룹 상태 모두 복제되지만, 주 서버가 복제본을 기다리지 않고 응답하는 비동기 복제라 장애 조치 뒤 마지막 명령이 빠질 수 있습니다. 각 명령이 바꾸는 상태로 따져 보면 `XADD`가 빠지면 메시지가 사라지고, `XACK`가 빠지면 끝난 메시지를 다시 처리하고, 장부 기록이 빠지면 넘긴 메시지를 다시 줍니다. 이 세 가지는 재현하지 않은 추론입니다. `WAIT`와 AOF의 강한 fsync 정책으로 줄일 수는 있지만 없앨 수는 없으므로, 소비자는 멱등해야 하고 유실을 전혀 허용하지 않는 요구라면 Redis 밖의 저장을 검토합니다. (본문: "그룹 상태도 복제되지만, 복제는 비동기입니다")
