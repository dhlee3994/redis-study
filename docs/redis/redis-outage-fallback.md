# Redis가 느리거나 멈출 때: DB로 우회할 것인가, 빨리 실패할 것인가, 기능을 줄일 것인가

> **핵심 질문**: Redis가 느리거나 멈추면 앱은 DB로 우회해도 되는가? DB가 그 부하를 받아낼 수 있는지는 무엇으로 판단하는가? 우회, 빠른 실패, 기능 축소 가운데 무엇을 어떻게 고르며, Redis가 돌아온 뒤에는 무엇을 조심해야 하는가?
>
> **범위**: Spring MVC(서블릿) 앱이 Spring Data Redis와 Lettuce로 Redis 한 대(또는 Sentinel로 장애 조치하는 한 묶음)를 쓰는 경우를 다룹니다. Redis를 어떻게 쓰는지에 따라 장애 때의 답이 달라지므로, 용도를 나누는 기준과 캐시의 우회 판단을 중심에 둡니다. 세션·처리율 제한·멱등 키·분산 락·카운터의 장애 대응 세부는 각 문서에 있어, 여기서는 결론과 연결만 둡니다. 장애 조치가 몇 초 걸리는지는 [Redis 장애 조치와 클라이언트](./failover-client.md)에서, Redis가 느려진 원인을 찾는 일은 [Redis 지연 진단](./latency-diagnosis.md)에서 다룹니다.
>
> **출처와 설계의 구분**: Spring·Lettuce·Resilience4j의 기본 동작과 기본값은 "근거"에 적은 소스로 확인했습니다. 용도별 선택, 우회 여부를 계산하는 방법, 예시의 숫자는 이 문서의 판단과 가상 수치입니다. 장애를 재현해 보지는 않았습니다.

## 배경 지식

### 이 문서의 예시: 쇼핑몰 API 서버 4대가 Redis 하나를 다섯 가지로 씁니다

문서 끝까지 다음 장면 하나를 씁니다. **모든 숫자는 설명을 위한 가상 수치입니다.**

- 쇼핑몰 API 서버 **4대**가 있고, 서버마다 Tomcat 요청 스레드 **200개**와 DB 연결 풀(HikariCP) **20개**를 둡니다. DB 연결은 모두 합쳐 80개입니다.
- 앱은 Redis 하나를 다음 다섯 가지 용도로 씁니다.

| 용도 | 무엇을 두는가 | 피크 요청량(4대 합계) | Redis 없이 일어나는 일 |
| --- | --- | --- | --- |
| ① 상품 상세 캐시 `product` | DB에서 읽은 상품 정보 | 초당 2,000건, 적중률 98% | 미스(캐시에 값이 없어 DB를 읽는 경우)마다 DB 조회 10ms |
| ② 카테고리 순위 캐시 `ranking` | DB 집계 결과 | 초당 100건, 적중률 99% | 미스마다 DB 집계 400ms |
| ③ 로그인 세션 | 로그인 상태(Spring Session) | 로그인한 사용자의 모든 요청 | DB에 같은 정보가 없음 |
| ④ 사용자별 처리율 제한 | 사용자마다 센 요청 수 | 모든 API 요청 | DB에 같은 정보가 없음 |
| ⑤ 주문 멱등 키 | "이 주문 요청은 처리했다"는 기록 | 주문 요청 | 주문 테이블에 (사용자, 멱등 키) 고유 제약이 있음 |

- DB에는 캐시와 무관한 API(주문, 회원 등)도 쿼리를 보냅니다. 피크 때 이 쿼리들이 쓰는 DB 연결은 평균 **30개**입니다.
- 부하 시험에서 이 DB는 동시에 실행되는 쿼리가 **약 60개**까지는 쿼리 시간을 유지했고, 그보다 많으면 쿼리 시간이 눈에 띄게 늘었습니다.
- Redis 명령 타임아웃은 처음에 따로 정하지 않았습니다. 이때 Spring Boot와 Lettuce의 기본값 **60초**가 적용됩니다.

```text
           사용자 요청
                │
      ┌─────────┼─────────┬─────────┐
      ▼         ▼         ▼         ▼
   앱 서버1  앱 서버2  앱 서버3  앱 서버4   (각자 요청 스레드 200, DB 연결 풀 20)
      │  \      │  \      │  \      │  \
      │   \─────┼───\─────┼───\─────┼───\──▶ Redis  ①②③④⑤
      ▼         ▼         ▼         ▼
     DB (연결 80개까지 받음. 동시 실행 쿼리 약 60개까지 쿼리 시간 유지)
```

화살표는 네트워크로 보내는 요청의 방향입니다. Redis의 복제본과 Sentinel은 그림에서 뺐습니다. 장애 조치 동안 클라이언트가 겪는 일은 [Redis 장애 조치와 클라이언트](./failover-client.md)에서 다룹니다. 이 문서의 질문은 "그 몇 초 또는 몇 분 동안, 그리고 장애 조치가 안 되는 장애 동안 앱이 무엇을 할 것인가"입니다.

### 캐시와 원본: 지워져도 다시 만들 수 있는가

- **한 줄**: 원본은 그 데이터의 정답을 가진 저장소이고, 캐시는 원본에서 읽은 값을 빨리 주려고 다른 곳에 둔 사본입니다. 사본은 지워져도 원본에서 다시 만들 수 있습니다.
- **비유**: 도서관의 책이 원본이고, 책상 위에 펼쳐 둔 복사본이 캐시입니다. 복사본을 잃으면 도서관에 다시 가면 됩니다. 시간이 더 걸릴 뿐입니다. 반면 책상 위의 메모장에만 적어 둔 전화번호(원본이 메모장)는 잃으면 다시 구할 곳이 없습니다.
- **실제로는**: 같은 Redis 안에서도 키마다 역할이 다릅니다. 예시의 ①② 키는 DB가 원본인 사본입니다. ③④의 키는 Redis에만 있는 데이터이므로 Redis가 원본입니다. ⑤는 둘이 섞여 있습니다. Redis 기록이 빠른 판정을 맡고, DB의 고유 제약이 최종 판정을 맡습니다. 비유와 다른 점은, 도서관(DB)에 다시 가는 사람이 혼자가 아니라 **초당 수천 명**이라는 것입니다. 복사본을 잃은 모두가 한꺼번에 도서관으로 가면 도서관 입구가 막힙니다.
- **이 주제에서는**: "DB로 우회한다"는 말은 **DB가 원본인 데이터에만** 뜻이 있습니다. Redis가 원본인 데이터는 DB에 물어도 답이 없습니다. 그리고 DB가 원본인 경우에도, 그 사본이 막아 주던 읽기를 DB가 다시 받아낼 수 있어야만 우회가 서비스를 지킵니다.

### 타임아웃, 그리고 기다리는 동안 붙잡히는 자원

- **한 줄**: 타임아웃은 응답을 기다리는 시간의 상한입니다. 응답을 기다리는 요청 스레드는 일을 하지 않아도 자리를 차지하므로, 기다림이 길수록 동시에 붙잡히는 스레드가 늘어납니다.
- **이 주제에서는**: 동시에 붙잡히는 스레드 수는 평균적으로 "초당 요청 수 × 한 요청이 머무는 시간"입니다(리틀의 법칙). Redis가 응답하지 않으면 머무는 시간이 명령 타임아웃만큼 늘고, 그만큼 스레드가 묶입니다. 이 관계와 값을 정하는 기준은 [타임아웃](../cs/timeout.md)의 "처리량, 머무는 시간, 동시에 머무는 수"와 "기다림은 자원을 붙잡습니다"에서 다룹니다. 이 문서는 같은 식으로 **DB 연결 수**도 셉니다. "초당 DB 조회 수 × 쿼리 하나의 시간"이 평균적으로 동시에 필요한 DB 연결 수입니다.

### 서킷 브레이커

- **한 줄**: 최근 호출의 실패가 기준을 넘으면, 한동안 상대를 부르지 않고 곧바로 실패를 돌려주는 장치입니다. 닫힘(평소대로 부름) → 열림(부르지 않음) → 반열림(시험 삼아 몇 개만 부름)의 세 상태를 오갑니다.
- **이 주제에서는**: Redis가 응답하지 않을 때 요청마다 타임아웃까지 기다리는 대신, 브레이커가 열려 Redis를 부르지 않고 바로 대체 동작으로 넘어가게 합니다. 개념, 대가, 무엇을 실패로 셀지는 [재시도와 서킷 브레이커](../cs/retry-and-circuit-breaker.md)의 "서킷 브레이커: 실패가 이어지면 한동안 부르지 않습니다"에 있습니다.

### 동시 실행 상한(벌크헤드)

- **한 줄**: 어떤 작업을 동시에 몇 개까지만 실행하게 하고, 넘는 요청은 기다리게 하거나 바로 거절하는 장치입니다. 흔히 벌크헤드(bulkhead)라고 부릅니다.
- **비유**: 배의 격벽입니다. 배 안을 여러 칸으로 나눠 두면 한 칸에 물이 차도 다른 칸은 뜹니다. 한 기능이 DB 연결을 모두 차지하지 못하게 칸을 나누는 것입니다.
- **실제로는**: 허가증 N장을 두고(세마포어), 작업을 시작할 때 한 장을 받고 끝나면 돌려줍니다. 허가증이 없으면 정해 둔 시간만큼 기다리거나 곧바로 실패합니다. 비유와 다른 점은, 격벽은 물을 막을 뿐이지만 벌크헤드는 **넘친 요청을 실패시킨다**는 것입니다. 무엇을 지키는 대신 누구를 실패시킬지 고르는 장치입니다. 또 허가증은 보통 **앱 서버(프로세스)마다** 따로 있습니다. 서버마다 6장이면 4대 합계는 24장이고, 서버가 8대로 늘면 48장이 됩니다. Resilience4j의 `Bulkhead`가 이 방식입니다.
- **이 주제에서는**: DB 연결 풀도 동시 실행 상한이지만, 풀은 **모든 API가 함께 쓰는** 상한입니다. 캐시 미스가 풀을 다 차지하면 주문 API도 연결을 얻지 못합니다. 그래서 "캐시 미스로 DB에 가는 조회"에만 풀보다 작은 상한을 따로 둡니다. 이 생각은 [캐시 스탬피드](./cache-stampede.md)의 "마지막 방어선: DB에 동시에 들어가는 수를 제한합니다"와 같습니다.

### Redis 장애의 두 모양: 거절하는 장애와 대답하지 않는 장애

앱이 보는 Redis 장애는 크게 두 모양입니다. 둘은 실패를 알게 되는 속도가 다릅니다.

| 모양 | 예 | 앱이 실패를 아는 시점 |
| --- | --- | --- |
| **거절**: 연결이 끊겼다는 것을 클라이언트가 안다 | Redis 프로세스가 내려가 포트가 닫힘, 연결이 거부됨 | 원칙적으로 곧바로. 다만 아래처럼 Lettuce 기본 설정은 끊긴 동안 명령을 쌓아 두므로, 다시 연결되지 않는 동안 명령은 타임아웃까지 기다립니다 |
| **무응답**: 연결은 살아 있는 것처럼 보이는데 답이 오지 않는다 | Redis 프로세스가 멈춤, 긴 명령이 메인 스레드를 차지, 네트워크가 패킷을 버림 | 명령 타임아웃이 지나야 |
| **느림**: 답은 오지만 늦다 | 메모리 부족, 큰 키 삭제, 부하 | 실패가 아니므로 알 수 없음. 느린 호출을 따로 세야 함 |

셋째 줄 "느림"은 엄밀히 장애가 아니지만, 앱에는 가장 다루기 어렵습니다. 호출이 성공하므로 실패 비율만 세는 장치는 아무것도 하지 않는데, 요청마다 기다림은 늘기 때문입니다.

## 용도마다 답이 다릅니다: DB로 우회할 수 있는 것은 캐시뿐입니다

"Redis가 멈추면 DB로 넘긴다"는 한 문장은 캐시에만 성립합니다. 나머지 용도는 Redis가 원본이므로 DB에 같은 데이터가 없습니다. 그래서 용도마다 "Redis 없이 무엇을 할 수 있는가"부터 따로 답합니다.

| 용도 | 원본 | Redis 없이 가능한 동작 | 예시의 선택(이 문서의 판단) | 세부 |
| --- | --- | --- | --- | --- |
| ① 상품 상세 캐시 | DB | DB에서 직접 읽기 | 상한을 둔 DB 우회 | 아래 "캐시를 DB로 돌리면 DB가 받는 부하는 몇 배가 되는가" |
| ② 카테고리 순위 캐시 | DB | DB 집계, 또는 앱 메모리의 마지막 사본 | DB로 보내지 않고, 마지막 사본을 주거나 순위 영역을 숨김 | 같은 절 |
| ③ 로그인 세션 | Redis | 없음. DB에 세션이 없음 | 빨리 실패(로그인이 필요한 화면은 오류) | [Redis 세션 저장소](./session-store.md)의 "Redis가 느리거나 멈추면 사용자에게 무엇이 일어나는가" |
| ④ 처리율 제한 | Redis | 제한 없이 통과(열기) 또는 거절(닫기), 서버별 근사 | 열어 두고, 판단하지 못한 요청 수를 셈 | [Redis 처리율 제한](./rate-limiting.md)의 "Redis가 느리거나 멈추면: 열어 둘 것인가, 닫을 것인가" |
| ⑤ 주문 멱등 키 | Redis(빠른 판정) + DB 고유 제약(최종 판정) | DB 고유 제약만으로 중복 주문을 막음 | Redis 확인을 건너뛰고 진행. 대신 동시 재시도가 결제 같은 앞단 작업까지 겹칠 수 있음을 받아들일지 판단 | [Redis 멱등 키](./idempotency-key.md)의 "Redis만으로는 부족한 곳: DB 고유 제약" |
| 분산 락(예시에는 없음) | Redis | 효율용 락이면 락 없이 진행(중복 작업 감수), 정확성용이면 거절 | | [Redis 분산 락](./distributed-lock.md) |
| 모았다가 반영하는 카운터(예시에는 없음) | 반영 전까지는 Redis | 그 증가분을 잃거나, DB에 바로 쓰기 | 조회수처럼 잃어도 되는 수면 잃음 | [Redis 카운터 write-back](./counter-write-back.md)의 "Redis가 데이터를 잃으면 무엇을 잃는가" |

이 표에서 읽을 것은 두 가지입니다.

- **Redis가 원본인 용도의 "우회"는 다른 기능을 쓰는 것입니다.** 세션을 DB로 우회하려면 세션을 DB에도 저장하고 있어야 합니다. 그것은 장애 대응이 아니라 저장소를 둘 두는 설계이고, 두 저장소가 엇갈리는 문제를 새로 만듭니다. 장애 순간에 코드 몇 줄로 만들 수 있는 것이 아닙니다.
- **카운터를 DB로 우회하면 Redis를 둔 이유를 되돌립니다.** 모았다가 반영하는 카운터는 DB가 그 쓰기를 감당하지 못해서 Redis에 둔 것입니다. 우회는 DB가 받지 못한다던 바로 그 부하를 보냅니다.

그래서 이 문서의 나머지는 주로 ①② 같은 캐시를 다룹니다. ③④⑤는 위 표의 선택을 따르되, "Redis 장애를 빨리 알아채는 법"과 "Redis가 돌아온 뒤: 빈 캐시와 낡은 캐시"는 모든 용도에 똑같이 필요합니다.

## 캐시를 DB로 돌리면 DB가 받는 부하는 몇 배가 되는가

### 미스율이 2%에서 100%가 됩니다

캐시가 있을 때 DB가 받는 읽기는 미스뿐입니다. Redis가 멈추고 모든 요청을 DB로 보내면 미스율이 100%가 됩니다. 그래서 DB 읽기는 **1 ÷ (평소 미스율)** 배가 됩니다.

| | 평소 DB 조회 | Redis 장애 때 DB 조회 | 배수 |
| --- | --- | --- | --- |
| ① `product` (적중률 98%) | 2,000 × 0.02 = 초당 40건 | 초당 2,000건 | 50배 |
| ② `ranking` (적중률 99%) | 100 × 0.01 = 초당 1건 | 초당 100건 | 100배 |

적중률이 높을수록 배수가 큽니다. 캐시가 잘 일할수록, 그 캐시가 사라졌을 때 DB가 받는 충격도 큽니다. 적중률 99.9%의 캐시는 1,000배입니다.

### 횟수가 아니라 동시에 필요한 DB 연결 수로 비교합니다

초당 조회 수만 보면 `product`의 초당 2,000건이 `ranking`의 100건보다 훨씬 무거워 보입니다. 하지만 DB에 부담이 되는 것은 쿼리 하나가 연결과 DB 자원을 쥐고 있는 시간까지 곱한 양입니다. 배경 지식의 "초당 조회 수 × 쿼리 하나의 시간"으로 바꿉니다. 여기서 쿼리 시간은 연결을 얻은 뒤부터 쿼리가 끝날 때까지이고, 풀에서 연결을 기다린 시간은 넣지 않습니다. 또 예시는 미스 조회가 연결을 얻어 쿼리 하나만 실행하고 바로 돌려준다고 가정해, 연결을 쥐는 시간을 쿼리 시간과 같게 놓습니다. 트랜잭션 안에서 다른 일을 하느라 연결을 더 오래 쥔다면 그 시간을 씁니다.

| | 평소 | Redis 장애 때(쿼리 시간이 그대로라면) |
| --- | --- | --- |
| ① `product` | 40 × 0.01초 = 0.4개 | 2,000 × 0.01초 = **20개** |
| ② `ranking` | 1 × 0.4초 = 0.4개 | 100 × 0.4초 = **40개** |
| 다른 API | 30개 | 30개 |
| 합계 | 약 31개 | **90개** |

평소에는 두 캐시가 거의 같은 무게(0.4개)였지만, 장애 때는 `ranking`이 `product`의 두 배를 요구합니다. 쿼리가 무겁기 때문입니다. 그리고 합계 90개는 연결 풀 합계 80개도, DB가 쿼리 시간을 유지하는 동시 실행 약 60개도 넘습니다. 이 계산은 [캐시 효과 재기](./cache-effectiveness.md)의 "DB가 덜 받은 부하는 동시 연결 수로 읽습니다"와 같은 방법입니다.

### 여유는 연결 풀 크기가 아니라 DB가 버티는 동시 실행 수에서 옵니다

여기서 비교 대상을 잘못 고르기 쉽습니다. 연결 풀 80개는 **앱이 DB에 보낼 수 있는 상한**일 뿐, DB가 그만큼을 감당한다는 뜻이 아닙니다. DB가 감당하는 양은 DB의 CPU, 디스크, 잠금 경합이 정하고, 이 값은 부하 시험이나 운영 지표(쿼리 시간이 늘기 시작하는 동시 실행 수)로 재야 합니다.

예시에서는 그 값이 약 60개이고, 다른 API가 이미 30개를 쓰므로 캐시 우회에 쓸 수 있는 **여유는 약 30개**입니다.

- ① `product`만 우회하면 20개로 여유 안입니다. 다만 여유의 3분의 2를 쓰므로 남는 폭이 좁습니다.
- ② `ranking`만 우회해도 40개로 여유를 넘습니다.
- 둘 다 우회하면 60개로 여유의 두 배입니다.

그래서 예시의 판단은 "①은 우회하되 상한을 두고, ②는 DB로 보내지 않는다"입니다. 상한과 ②의 대체 동작은 아래 "고를 수 있는 길과 대가"에서 정합니다.

### 몰리면 쿼리가 느려지고, 느려지면 더 몰립니다

위 표는 "쿼리 시간이 그대로라면"이라는 가정 위에 있습니다. 실제로는 이 가정이 가장 먼저 깨집니다.

1. 동시 실행이 60개를 넘으면 쿼리끼리 CPU와 디스크를 나눠 쓰므로 쿼리 하나의 시간이 늘어납니다.
2. 쿼리 시간이 10ms에서 20ms로 늘면, 같은 초당 2,000건이 요구하는 연결은 20개에서 40개로 늘어납니다.
3. 연결이 더 필요해지니 동시 실행이 더 늘고, 1번으로 돌아갑니다.

이 되먹임 때문에 DB는 여유를 조금 넘는 지점에서 서서히 느려지는 것이 아니라, 어느 순간 급격히 무너질 수 있습니다. 쿼리 시간이 늘면 **캐시와 무관한 주문 API의 쿼리도** 같이 느려집니다. 캐시 우회가 "상품 페이지가 조금 느려지는 일"로 끝나지 않고 주문 장애가 되는 경로입니다. 그래서 여유를 계산할 때는 평균이 아니라 피크와, 쿼리 시간이 늘어난 상황까지 넣습니다.

## 아무 준비 없이 넘기면 Redis 장애가 전체 장애가 되는 길

예시 앱이 처음 상태(명령 타임아웃 기본값, 브레이커 없음, 상한 없음)에서 Redis가 **응답하지 않게** 됐다고 하겠습니다. 장애가 번지는 길은 네 갈래입니다.

### 첫째: 요청마다 타임아웃까지 기다립니다

Spring Boot에서 `spring.data.redis.timeout`을 정하지 않으면 Lettuce의 기본 명령 타임아웃 60초가 쓰입니다. 서버 한 대에 `product` 요청이 초당 500건 들어오고 각자 60초씩 기다린다면, 필요한 스레드는 500 × 60 = 30,000개입니다. 스레드는 200개뿐이므로 약 0.4초 만에(200 ÷ 500) 바닥납니다. 그 뒤로는 **Redis를 쓰지 않는 API도** 스레드를 받지 못합니다. 이 계산의 모양은 [타임아웃](../cs/timeout.md)의 "기다림은 자원을 붙잡습니다"와 같습니다.

게다가 예시의 요청은 Redis를 한 번만 부르지 않습니다. 처리율 제한(④)이 모든 요청 앞에서, 세션(③)이 로그인한 요청마다 Redis를 부릅니다. 요청 하나가 Redis 명령 여러 개를 차례로 기다리면 머무는 시간은 그 합입니다.

### 둘째: 끊긴 연결에서도 기다립니다

"Redis 프로세스가 내려가 연결이 끊겼다면 바로 실패하겠지"라고 기대하기 쉽습니다. Lettuce의 기본 설정은 그렇지 않습니다. 자동 재연결이 켜져 있고(기본), 끊긴 상태의 동작이 기본값(`DisconnectedBehavior.DEFAULT`)이면 **끊긴 동안 들어온 명령을 거절하지 않고 받아 둡니다.** 다시 연결되면 보내려는 것입니다. 받아 둘 수 있는 명령 수의 기본 상한은 아주 큰 값이라 사실상 제한이 없습니다. 그래서 Redis가 돌아오지 않는 동안, 이미 맺어 둔 연결로 보낸 동기 호출은 연결이 끊긴 장애에서도 명령 타임아웃까지 기다립니다. 그 사이에 다시 연결되면 받아 둔 명령이 나가 응답을 받으므로 더 일찍 끝납니다. 거절하게 하려면 `ClientOptions`의 `disconnectedBehavior`를 `REJECT_COMMANDS`로 바꿉니다. 이렇게 바꾸면 끊긴 것을 클라이언트가 아는 동안에는 바로 실패하지만, 짧은 끊김에서 다시 연결해 이어 갈 기회를 잃습니다. 세부와 재전송 때문에 생기는 중복 실행은 [Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)에서 다룹니다.

이 설정은 "무응답" 모양에는 효과가 없습니다. 연결이 살아 있는 것처럼 보이면 클라이언트는 끊겼다고 판단하지 않기 때문입니다.

### 셋째: 기다림을 넘긴 요청은 막는 장치 없이 DB로 갑니다

Spring의 `@Cacheable`은 기본 설정에서 Redis 예외를 DB로 넘기지 않고 그대로 던집니다(기본 에러 처리기 `SimpleCacheErrorHandler`). 그래서 기본 설정의 예시 앱에서 Redis 장애는 "상품 조회 실패"로 나타납니다. 많은 팀이 이것을 보고 처리기를 `LoggingCacheErrorHandler`로 바꿉니다. 그러면 읽기 예외는 경고 로그를 남기고 미스로 처리되어 메서드(DB 조회)가 실행되고, 쓰기·삭제 예외도 로그만 남깁니다. 이 동작과 삭제 실패를 삼키는 문제는 [`@Cacheable`이 캐시하지 않거나, 엉뚱한 값을 주거나, 너무 오래 둘 때](./spring-cacheable.md)의 "Redis가 실패하면: 기본은 DB로 넘어가지 않고 실패합니다"에서 다룹니다.

처리기만 바꾸면 앞 절의 계산이 그대로 일어납니다. ①과 ② 모두 미스가 되어 DB 연결 90개를 요구하고, 그 앞에는 서버마다 20개짜리 연결 풀 말고는 아무 상한이 없습니다. 풀이 차면 주문 API도 연결을 기다립니다. "Redis 장애에 대비해 DB로 넘기게 했다"는 설정이 Redis 장애를 DB 장애로 바꿉니다.

### 넷째: 헬스 체크가 멀쩡한 서버까지 빼 버립니다

Spring Boot Actuator는 `RedisConnectionFactory` 빈이 있으면 Redis 헬스 지표를 자동으로 등록합니다. Redis에 닿지 못하면 이 지표가 DOWN이 되고, 전체 상태는 가장 나쁜 상태를 따르므로 `/actuator/health`가 DOWN, HTTP 503을 돌려줍니다.

로드 밸런서나 Kubernetes의 준비 상태 확인(readiness probe)이 `/actuator/health`를 보고 있으면, Redis 장애 순간 **4대 모두가 "준비 안 됨"으로 빠집니다.** 앱에서 DB 우회를 아무리 잘 만들어 두어도 요청이 앱에 오지 않습니다. 생존 확인(liveness probe)이 같은 주소를 보면 Kubernetes가 앱을 다시 시작시키기까지 합니다.

Spring Boot는 Kubernetes에서 실행될 때 생존·준비 상태 그룹(`/actuator/health/liveness`, `/actuator/health/readiness`)을 자동으로 만들고, 이 그룹에는 기본으로 앱 자신의 생존·준비 상태만 들어 있습니다. 그래서 확인 주소를 이 그룹으로 두면 Redis 상태가 빠집니다. 이 그룹이 자동으로 생기는 것은 Kubernetes나 Cloud Foundry에서 실행될 때, 또는 `management.endpoint.health.probes.enabled`를 켰을 때입니다. 판단 기준은 "Redis 없이도 이 서버가 받아서 할 수 있는 일이 있는가"입니다. 있다면 Redis 상태는 준비 상태에서 빼고 지표와 경보로만 봅니다. 세션처럼 Redis 없이는 할 수 있는 일이 거의 없는 서버라도, 모든 서버를 빼면 "오류 응답"조차 주지 못하고 연결이 거부되므로 대개 빼는 편이 낫습니다(이 문서의 판단).

## Redis 장애를 빨리 알아채는 법

### 명령 타임아웃: 한 요청이 기다리는 상한

첫 번째 장치는 명령 타임아웃을 서비스의 기한에 맞춰 줄이는 것입니다. 기준은 앱 쪽에서 잰 정상 Redis 호출의 응답 시간 분포이고, 그 꼬리 바로 위에 둡니다. 예시에서 정상 호출이 대부분 1ms 안팎이고 p99.9가 5ms라면(가상) 명령 타임아웃을 50ms로 둡니다. 정하는 기준은 [Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)의 "명령 타임아웃 값은 무엇을 기준으로 정하는가"에 있습니다.

타임아웃만 줄인 예시 앱이 Redis 무응답 장애를 만나면, `product` 요청 하나는 다음만큼 머뭅니다(처리율 제한은 열어 두기로 했다고 가정).

| 단계 | 시간 |
| --- | --- |
| 처리율 제한 호출 → 타임아웃 | 50ms |
| 캐시 읽기 → 타임아웃 → 미스로 처리 | 50ms |
| DB 조회 | 10ms |
| 캐시 쓰기 → 타임아웃 | 50ms |
| 합계 | 약 160ms |

서버 한 대에 초당 500건이면 스레드 약 500 × 0.16 = 80개가 묶입니다. 200개 안이라 서버는 버팁니다. 요청 하나가 약 160ms 머물고, 그중 150ms는 Redis 타임아웃을 기다리는 시간입니다. 그리고 DB 부하는 줄지 않습니다. 타임아웃은 **한 요청의 손해에 상한을 둘 뿐, 장애 동안 모든 요청이 그 손해를 치르는 것은 막지 못합니다.**

### 서킷 브레이커: 기다림 자체를 없앱니다

서킷 브레이커가 열리면 Redis를 부르지 않고 곧바로 "열려 있음" 예외를 돌려주므로, 위 표의 50ms 세 번이 0이 됩니다. 앱은 그 예외를 받고 용도별 대체 동작(DB 우회, 마지막 사본, 거절)으로 갑니다.

Resilience4j를 쓴다면 기본값이 무엇을 뜻하는지 알고 바꿔야 합니다.

| 설정 | 기본값 | 예시에서 뜻하는 것 |
| --- | --- | --- |
| 판정 창(`slidingWindowType`, `slidingWindowSize`) | 최근 호출 100건 | 서버당 초당 500건이면 0.2초 분량. 초당 1건인 관리자 API라면 100초 분량 |
| 판정 최소 호출 수(`minimumNumberOfCalls`) | 100건 | 100건이 모이기 전에는 실패율을 계산하지 않음. 요청이 적은 서버는 늦게 열림 |
| 실패율 기준(`failureRateThreshold`) | 50% | 창 안의 절반이 실패하면 열림 |
| 열림 유지(`waitDurationInOpenState`) | 60초 | Redis가 10초 만에 돌아와도 60초 동안은 대체 동작으로 처리. 그 동안 DB 우회가 이어짐 |
| 반열림 시험 호출 수(`permittedNumberOfCallsInHalfOpenState`) | 10건 | 10건의 결과로 닫을지 다시 열지 정함 |
| 열림 → 반열림 자동 전환(`automaticTransitionFromOpenToHalfOpenEnabled`) | 꺼짐 | 대기 시간이 지난 뒤 **첫 호출이 올 때** 반열림으로 바뀜. 호출이 없으면 상태 지표는 계속 "열림" |
| 느린 호출 기준(`slowCallDurationThreshold`, `slowCallRateThreshold`) | 60초, 100% | 느린 호출도 기본으로 기록하지만, 60초를 넘는 호출만 느린 호출이고 그 비율이 100%여야 열리므로 사실상 느림으로는 열리지 않음. 아래에서 따로 봄 |

열림 유지 시간은 장애 동안의 손해와 회복 후의 손해를 맞바꿉니다. 짧게 두면 Redis가 돌아온 것을 빨리 알지만, 아직 아픈 Redis에 시험 호출을 자주 보냅니다. 길게 두면 회복 뒤에도 대체 동작(예시라면 DB 우회와 순위 숨김)이 오래 이어집니다. 예시의 장애 조치가 수십 초 안에 끝난다고 보면, 10초 정도로 줄여 회복을 빨리 알아채는 쪽을 고를 수 있습니다(이 문서의 판단).

브레이커를 **어느 단위로 둘지**도 정해야 합니다. 예시의 다섯 용도는 같은 Redis에 있으므로 연결 실패와 타임아웃은 원인이 하나입니다. 용도마다 브레이커를 따로 두면 요청이 적은 용도(⑤ 주문)는 판정에 필요한 호출이 늦게 모여 늦게 열립니다. 그래서 "연결 실패와 타임아웃을 세는 브레이커 상태는 Redis 하나에 하나, 열렸을 때의 대체 동작은 용도마다"가 자연스럽습니다(이 문서의 판단). 느린 호출을 셀 때는 이 공유 브레이커와 따로 둡니다. 이유는 아래 "느린 Redis는 기본 기준으로는 브레이커를 열지 못합니다"에 있습니다. 브레이커 상태는 앱 서버마다 따로이므로, 4대는 각자 조금씩 다른 시각에 열리고 닫힙니다.

### 느린 Redis는 기본 기준으로는 브레이커를 열지 못합니다

Redis가 멈추지 않고 느려지면(정상 1ms가 30ms로) 호출은 모두 성공합니다. 실패율은 0%이고, 30ms는 기본 느린 호출 기준 60초에 한참 못 미치므로 느린 호출 비율도 0%입니다. 브레이커는 닫혀 있고, 요청마다 Redis 호출 수 × 30ms가 붙습니다. 위 표의 세 번이면 약 90ms입니다. 서버당 초당 500건이면 스레드 약 50개가 Redis를 기다립니다.

이것까지 다루려면 느린 호출의 기준을 낮춥니다. Resilience4j는 실패율과 느린 호출 비율을 따로 계산하고, 둘 중 하나라도 자기 기준 이상이면 브레이커를 엽니다. 느린 호출은 성공했더라도 정해 둔 시간을 넘긴 호출이므로, 느린 호출이 늘어도 실패율은 오르지 않습니다. 예를 들어 느린 호출 기준을 명령 타임아웃보다 짧은 20ms로, 느린 호출 비율 기준을 50%로 두면, 판정 최소 호출 수가 모인 뒤 20ms를 넘은 호출이 50% 이상일 때 브레이커가 열립니다. 대가는 분명합니다. **느리지만 답하던 Redis를 버리고 DB로 우회**하므로, 느림이 짧게 지나가는 경우(예: 큰 키 하나를 지우는 동안)에도 DB에 50배의 부하를 보낼 수 있습니다. 그래서 느린 호출로 브레이커를 열 용도는 "DB 우회"가 아니라 "마지막 사본"이나 "기능 축소"처럼 DB에 부담을 주지 않는 대체 동작이 있는 용도로 한정하는 편이 안전합니다(이 문서의 판단).

이 한정은 공유 브레이커 하나로는 할 수 없습니다. 느린 호출 기준은 브레이커 하나에 거는 설정이라, 공유 브레이커에 걸면 느림으로 열릴 때 ① `product`도 함께 DB로 우회하기 때문입니다. 그래서 공유 브레이커는 연결 실패와 타임아웃만 세고, 느린 호출 기준은 그 기준으로 열어도 되는 용도(예시의 ② `ranking`)의 Redis 호출에만 따로 둔 브레이커에 겁니다(이 문서의 판단). ②의 Redis 호출은 공유 브레이커를 거치지 않고 이 브레이커 하나만 거칩니다. 이 브레이커도 연결 실패와 타임아웃을 실패로 세므로, 연결이 끊기거나 무응답일 때도 ②는 자기 브레이커의 판정으로 열립니다. 열린 뒤 실제로 마지막 사본에 가려면 그 분기를 DB 집계보다 앞에 두어야 하며, 어디에 두는지는 아래 "무엇을 실패로 셀 것인가"에 있습니다. 두 브레이커는 판정 최소 호출 수와 기준을 각자 채워야 열리므로 열리는 시각이 다를 수 있고, 어느 쪽이 열리든 그 브레이커를 거치는 용도만 대체 동작으로 갑니다. 느리기만 하면 느린 호출 기준이 있는 ②의 브레이커만 열립니다.

### 무엇을 실패로 셀 것인가

브레이커는 **Redis가 아프다는 신호**만 실패로 세야 합니다. Spring Data Redis는 Lettuce 예외를 다음처럼 바꿔 던집니다.

| Lettuce 쪽 원인 | Spring 예외 | 셀 것인가 |
| --- | --- | --- |
| 연결 실패(`RedisConnectionException`, 채널 예외) | `RedisConnectionFailureException` | 셉니다 |
| 명령 타임아웃(`RedisCommandTimeoutException`) | `QueryTimeoutException` | 셉니다 |
| Redis가 돌려준 오류 응답 | 원인 예외를 풀어서 변환, 대개 `RedisSystemException` | 오류 접두어로 나눕니다. `LOADING`은 Redis 전체가 일을 못 받는 상태이므로 셉니다. `WRONGTYPE`처럼 요청이 잘못된 오류는 세지 않습니다. 쓰기만 거부하는 `MISCONF`·OOM은 쓰기 쪽 동작만 막아야 합니다 |

`@Cacheable`의 값 직렬화 실패, 키 객체를 Redis 키 문자열로 바꾸는 실패도 Redis 캐시의 읽기·쓰기 안에서 일어나 같은 처리기로 넘어오지만 Redis의 상태와 무관하므로 세지 않습니다. 여기서 "세지 않는다"는 Resilience4j의 `ignoreExceptions`처럼 판정에서 아예 빼는 것입니다. 기록할 예외 목록에서만 빼면 그 호출은 성공으로 기록되어, 실패율의 분모를 늘리고 느린 호출 판정에도 들어갑니다. 오류 접두어별 판단은 [Redis 오류별 재시도 정책](./retry-policy.md)의 "서킷 브레이커는 무엇을 실패로 세는가"에 있습니다.

무엇을 셀지와 함께 **브레이커를 어디에 둘지**가 정해져야 위 표가 뜻을 가집니다. `@Cacheable`에서 캐시를 읽고 쓰다 난 예외는 모두 캐시 에러 처리기로 넘어갑니다. `LoggingCacheErrorHandler`라면 그 예외는 로그만 남기고 사라집니다. 그래서 브레이커를 `@Cacheable` 메서드 바깥에 두면 Redis 실패는 보지 못하고, 미스로 넘어간 DB 조회의 결과만 봅니다. Redis가 멈춰도 DB가 답하면 성공으로 셉니다. 반대로 DB 조회가 예외로 끝나면 그 예외가 브레이커가 기록하기로 한 예외에 드는 경우(Resilience4j 기본은 모든 예외) 실패율에 들어가고, DB가 느리면 느린 호출 기준을 둔 경우 느린 호출 비율에 들어갑니다. 어느 쪽이든 DB의 상태가 Redis 판정에 섞입니다. 브레이커는 캐시 안쪽, 곧 Redis 호출 자체를 감싸야 Redis의 결과를 셉니다. 애너테이션의 순서로 정할 수 있는 일이 아니라, 캐시가 Redis를 읽고 쓰는 부품인 `RedisCacheWriter`를 바꾸는 일입니다. 기본 구현(`RedisCacheWriter.nonLockingRedisCacheWriter(...)`)을 감싸 읽기·쓰기·삭제를 브레이커에 통과시키는 구현을 만들고, `RedisCacheManager.builder(RedisCacheWriter)`로 캐시 매니저에 넣습니다. 이 부품의 메서드는 캐시 이름을 함께 받으므로, `ranking`이면 ②의 브레이커를 고르는 식으로 나눌 수 있습니다(이 문서의 판단, 코드는 다루지 않음). 이때 `@Cacheable(sync = true)`가 쓰는 읽기는 기본 구현 안에서 Redis 읽기, 메서드(DB 조회) 실행, Redis 쓰기를 한 번에 합니다. 이 호출을 통째로 감싸면 DB의 시간과 예외가 Redis 호출 하나로 섞이므로, 읽기와 쓰기만 브레이커에 통과시키고 메서드 실행은 그 바깥에 둡니다.

이렇게 두면 브레이커가 열려 던지는 "열려 있음" 예외도 같은 처리기로 넘어가 미스가 되므로, `product`는 처리기를 거쳐 DB 우회로 갑니다. ②는 그대로 두면 안 됩니다. 미스가 되면 순위 메서드가 DB 집계를 실행하기 때문입니다. 캐시 에러 처리기는 어느 캐시의 오류인지 함께 받으므로, `ranking`의 오류는 삼키지 않고 다시 던지게 하고, 순위를 부르는 쪽이 그 예외를 받아 마지막 사본을 주거나 순위 영역을 숨깁니다(이 문서의 판단). 여기까지는 Spring 소스로 추론한 것이고, 재현해 보지는 않았습니다.

## 고를 수 있는 길과 대가

브레이커가 열렸거나 Redis 호출이 실패했을 때, 용도마다 다음 가운데 하나를 고릅니다.

| 선택 | 동작 | 얻는 것 | 잃는 것 | 맞는 경우 |
| --- | --- | --- | --- | --- |
| **빨리 실패** | 바로 오류를 돌려줌(예: 503) | 상태가 단순하고, 다른 저장소에 부하가 번지지 않음 | 그 기능이 장애 동안 멈춤 | Redis가 원본이고 대체 수단이 없음(③), DB가 우회 부하를 받지 못함 |
| **상한을 둔 DB 우회** | DB에서 읽되, 동시 실행 수를 벌크헤드로 제한. 넘치면 빨리 실패 | 상한 안의 요청은 정상 응답 | 넘친 요청은 실패. 상한을 잘못 잡으면 DB가 무너짐 | DB가 원본이고 여유가 계산상 남는 캐시(①) |
| **마지막 사본(낡은 값)** | 앱 메모리에 남겨 둔 마지막 값을 줌 | DB 부하 없음, 응답 빠름 | 장애 동안 값이 낡음. 서버마다 사본 시각이 다름 | 조금 낡아도 되는 작은 데이터(②) |
| **기능 축소** | 해당 영역을 숨기거나 기본값을 줌 | 페이지의 나머지는 정상 | 그 기능이 보이지 않음 | 없어도 핵심 흐름이 이어지는 기능(추천, 순위) |
| **쓰기를 모아 두기** | 앱 메모리나 다른 저장소에 쌓았다가 나중에 반영 | 쓰기 기능이 계속됨 | 서버가 죽으면 쌓인 것을 잃음. 나중에 순서·중복 문제 | 잃어도 되는 수(조회수). 정확해야 하는 쓰기에는 맞지 않음 |

### 예시에 대입하면

- **① `product`: 상한을 둔 DB 우회.** 여유 30개 가운데 24개를 ①에 배정하고 6개는 피크와 쿼리 시간 증가에 대비해 남깁니다. 벌크헤드는 서버마다 걸리므로 서버당 24 ÷ 4 = 6개입니다. 평균 수요가 서버당 5개(20 ÷ 4)라 상한과의 차이가 1개뿐입니다. 순간 몰림이나 쿼리 시간 증가로 넘친 요청은 기다리지 않고 바로 실패하므로, 얼마나 거절되는지는 실제 요청 분포에 달려 있고 훈련에서 거절 수를 보고 조정합니다. 상품 상세에는 줄여서 보여 줄 값이 없으므로 넘친 요청은 오류입니다. 벌크헤드는 쿼리 시간이 아니라 미스 조회가 DB에 동시에 들어가는 수를 허가증 수로 묶어 두므로, 쿼리가 느려져도 DB에 동시에 들어가는 수는 24개를 넘지 않습니다. 앞의 되먹임이 여기서 끊깁니다.
- **② `ranking`: 마지막 사본, 너무 낡으면 숨김.** 카테고리가 100개쯤이면 각 서버가 마지막으로 읽은 순위를 앱 메모리에 들고 있어도 부담이 작습니다. Redis 장애 동안 이 사본을 주고, 사본이 정해 둔 시간(예: 30분)보다 오래되면 순위 영역을 숨깁니다. 서버마다 사본을 읽은 시각이 달라 서버마다 다른 순위가 보일 수 있습니다. 로컬 사본의 메모리와 서버 사이의 불일치는 [2단 캐시](./two-tier-cache.md)에서 다룹니다. 평소에도 로컬 캐시를 두는 2단 구성이라면, 장애 동안에만 로컬 항목의 만료를 늦추는 식으로 같은 효과를 낼 수 있습니다.
- **③ 세션: 빨리 실패.** 세션 저장소의 Redis 호출도 같은 브레이커로 감싸 두면, 브레이커가 열린 동안 세션 조회가 곧바로 실패하므로, 로그인이 필요한 화면은 바로 오류를 받고 스레드가 묶이지 않습니다. 공개 화면은 정상입니다.
- **④ 처리율 제한: 열어 두기.** 판단하지 못한 요청 수를 따로 셉니다. 건당 비용이 드는 외부 호출의 제한이라면 닫기를 고릅니다.
- **⑤ 멱등 키: Redis 확인을 건너뛰고 DB 고유 제약에 맡깁니다.** 같은 키의 두 번째 주문은 고유 제약 위반으로 거절됩니다. 다만 Redis의 "처리 중" 기록이 없으므로, 동시에 온 재시도가 고유 제약에 닿기 전에 하는 일(외부 결제 호출 등)은 겹칠 수 있습니다. 이것을 받아들일 수 없다면 장애 동안 주문 API를 빨리 실패로 둡니다.

### 상한은 서버마다 걸린다는 것을 잊기 쉽습니다

벌크헤드의 허가증 6개는 서버 한 대의 값입니다. 자동 확장으로 서버가 8대가 되면 합계는 48개로 여유 30개를 넘습니다. 대응은 두 가지입니다.

- 서버 수가 바뀌어도 넘지 않게, 예상하는 최대 서버 수로 나눠 정합니다. 서버가 적을 때는 여유를 덜 씁니다.
- DB 쪽에서 앱 사용자(계정)별 동시 실행 수나 연결 수를 제한하는 기능이 있다면 그것을 마지막 상한으로 둡니다. DB 제품마다 다르므로 쓰는 DB의 문서로 확인합니다.

같은 이유로, Redis 장애는 대개 모든 서버에서 동시에 일어나므로 "서버 한 대에서는 견딘다"는 시험은 의미가 없습니다. 모든 서버의 합계로 계산합니다.

## Redis가 돌아온 뒤: 빈 캐시와 낡은 캐시

장애 대응은 Redis가 돌아오는 순간 끝나지 않습니다. 돌아온 Redis가 **비어 있는지, 데이터를 안고 있는지**에 따라 다른 문제가 생깁니다.

### 비어서 돌아오면: 장애 때와 같은 부하가 이번에는 상한 없이 옵니다

영속성 없이 다시 시작한 Redis는 비어 있습니다. 브레이커가 반열림에서 시험 호출 10건을 보내면, 캐시 읽기는 "키 없음"(미스)이라는 **정상 응답**을 받습니다. 미스는 실패가 아니므로 브레이커는 닫힙니다.

그러면 모든 요청이 평소 경로로 돌아가 미스를 보고 DB로 갑니다. DB가 받는 부하는 장애 때와 같은 90개 수요인데, 문제는 **장애 때 걸어 둔 벌크헤드가 "브레이커가 열렸을 때의 대체 동작"에만 있다면, 평소 경로에는 상한이 없다**는 것입니다. 장애를 잘 넘긴 직후에 DB가 무너지는 이유입니다.

- 벌크헤드를 대체 동작이 아니라 **캐시 미스로 DB에 가는 경로 자체**에 둡니다. 그러면 장애 중이든, 빈 캐시로 돌아온 직후든, 인기 키가 만료된 순간이든 같은 상한이 DB를 지킵니다.
- 빈 캐시가 채워지는 동안의 몰림과 대응(같은 키의 조회 합치기, 미리 채우기와 그 함정)은 [캐시 스탬피드](./cache-stampede.md)의 "캐시가 통째로 비는 경우도 같은 모양입니다"에서 다룹니다.

데이터를 디스크에서 읽으며 다시 시작하는 Redis는 읽는 동안 데이터를 다루는 명령에 `LOADING` 오류를 돌려줍니다. 이 오류를 실패로 세면 브레이커는 반열림에서 다시 열리고, 읽기가 끝난 뒤에 닫힙니다. 원하는 동작입니다. `LOADING`이 얼마나 이어지는지는 [Redis 오류별 재시도 정책](./retry-policy.md)의 "`LOADING`: 데이터를 읽는 동안 데이터를 다루는 명령을 거부합니다"에 있습니다.

### 데이터를 안고 돌아오면: 장애 동안 지우지 못한 값이 남습니다

장애 조치로 복제본이 주 서버가 됐거나 AOF·RDB로 데이터를 되살린 Redis에는 장애 전의 캐시 값이 있습니다. 그런데 장애 동안 상품 가격이 바뀌었다면 어떻게 될까요?

1. 장애 중에 관리자가 상품 42의 가격을 바꿉니다. DB는 바뀝니다.
2. `@CacheEvict`가 Redis 오류로 실패합니다. 처리기가 로그만 남기거나, 브레이커가 열려 있어 아예 Redis를 부르지 않았습니다.
3. Redis가 돌아오고, 그 안에는 상품 42의 **옛 가격**이 남아 있습니다.
4. 이 값은 TTL이 끝날 때까지 정답처럼 읽힙니다. TTL이 없는 캐시라면 누가 지울 때까지입니다.

읽기 실패를 삼킨 대가는 "느려짐"이지만, 삭제 실패를 삼킨 대가는 "틀린 값"입니다. 이 틈은 [Cache-aside 정합성](./cache-aside-consistency.md)의 "삭제가 실패하면 틈이 닫히지 않습니다"와 같은 원리이고, 장애 동안의 모든 쓰기에서 한꺼번에 생긴다는 점만 다릅니다.

대응은 틀린 값이 얼마나 오래 남아도 되는가로 고릅니다.

| 대응 | 동작 | 대가 |
| --- | --- | --- |
| TTL에 맡김 | 짧은 TTL이 틀린 값의 수명을 묶음 | TTL 동안 틀린 값. TTL이 길면 받아들이기 어려움 |
| 못 지운 키를 기록했다가 다시 지움 | 삭제가 실패하거나 건너뛴 키를 모아 두고, 브레이커가 닫히면 지움 | 앱 메모리에 모으면 그 서버가 재시작할 때 잃음. 잃지 않으려면 DB의 변경 기록(아웃박스)에 두어야 함 |
| 돌아오면 그 캐시를 통째로 비움 | 장애가 길었거나 바뀐 키를 모르면 캐시 이름 단위로 지움 | 빈 캐시로 돌아온 것과 같은 몰림. 앞 절의 상한이 있어야 함 |

통째로 비우는 길을 고를 때 조심할 점이 있습니다. Spring Boot가 자동으로 만든 Redis 캐시 매니저의 `Cache.clear()`(`@CacheEvict(allEntries = true)`도 이 경로)는 기본으로 **`KEYS` 명령으로 그 캐시의 키를 찾은 뒤 `DEL`로 지웁니다.** `KEYS`는 데이터베이스의 키 전체를 훑는 동안 Redis 메인 스레드를 붙잡으므로, 막 돌아온 Redis를 다시 멈추게 할 수 있습니다. 캐시 매니저를 만들 때 `BatchStrategies.scan(...)`으로 바꾸면 `SCAN`으로 여러 번에 나눠 훑고 나눠 지웁니다. 한 번의 명령이 메인 스레드를 붙잡는 시간은 줄지만, 왕복이 늘고 다 지우기까지는 더 오래 걸립니다. `KEYS`와 `SCAN`의 비용은 [이벤트 루프](./event-loop.md)에서 다룹니다.

## 데이터와 요청이 늘면 무엇이 달라지는가

- **트래픽이 늘면**: 우회 부하는 트래픽에 정비례하지만, DB 여유는 다른 API의 성장으로 오히려 줄어듭니다. 작년에 "우회해도 된다"고 계산한 캐시가 올해는 아닐 수 있으므로, 이 계산은 피크 트래픽이 바뀔 때마다 다시 합니다.
- **적중률이 오르면**: 평소 DB 부하는 줄지만 장애 때의 배수(1 ÷ 미스율)는 커집니다. 적중률 개선이 장애 대비를 약하게 만들 수 있습니다.
- **캐시하는 쿼리가 무거워지면**: 동시 연결 수요는 쿼리 시간에 비례합니다. 같은 초당 100건이라도 집계가 400ms에서 1초가 되면 수요는 40개에서 100개가 됩니다.
- **서버 수가 늘면**: 서버마다 거는 상한의 합계가 늘어납니다. "상한은 서버마다 걸린다는 것을 잊기 쉽습니다"에서 본 대로 다시 나눕니다.
- **Redis를 쓰는 용도가 늘면**: 요청 하나가 부르는 Redis 명령 수가 늘어, 무응답 장애에서 요청이 머무는 시간이 명령 수 × 타임아웃만큼 늡니다. 브레이커가 열리기 전의 손해가 커집니다.
- **키가 많아지면**: 회복 뒤 캐시를 통째로 비우는 비용과, 빈 캐시가 다시 채워지는 시간이 늘어납니다.

## 흔한 오해 모아 보기

| 오해 | 실제 | 처음 바로잡은 곳 |
| --- | --- | --- |
| Redis가 죽으면 DB로 읽으면 된다 | 그것은 캐시에만 뜻이 있습니다. 세션, 처리율 제한, 락처럼 Redis가 원본인 데이터는 DB에 없습니다 | 용도마다 답이 다릅니다: DB로 우회할 수 있는 것은 캐시뿐입니다 |
| DB 부하는 초당 조회 수로 비교하면 된다 | 쿼리 시간을 곱한 동시 연결 수로 비교해야 합니다. 예시에서 초당 100건의 순위가 초당 2,000건의 상품보다 무겁습니다 | 횟수가 아니라 동시에 필요한 DB 연결 수로 비교합니다 |
| 연결 풀이 80개면 DB는 80개까지 견딘다 | 풀은 앱이 보낼 수 있는 상한입니다. DB가 견디는 양은 따로 재야 합니다 | 여유는 연결 풀 크기가 아니라 DB가 버티는 동시 실행 수에서 옵니다 |
| `@Cacheable`은 Redis가 죽으면 알아서 DB를 읽는다 | 기본 처리기는 예외를 다시 던집니다. 처리기를 바꿔도 타임아웃은 기다립니다 | 셋째: 기다림을 넘긴 요청은 막는 장치 없이 DB로 갑니다 |
| 연결이 끊기면 Lettuce 명령은 바로 실패한다 | 기본 설정은 끊긴 동안 명령을 받아 두므로, 다시 연결되지 않으면 타임아웃까지 기다립니다 | 둘째: 끊긴 연결에서도 기다립니다 |
| 타임아웃을 짧게 하면 충분하다 | 요청 하나의 손해에 상한을 둘 뿐, 모든 요청이 그 손해를 치릅니다. 기다림을 없애는 것은 브레이커입니다 | 명령 타임아웃: 한 요청이 기다리는 상한 |
| 브레이커가 있으면 느린 Redis도 막는다 | 기본 느린 호출 기준은 60초·100%라 느림으로는 사실상 열리지 않습니다. 기준을 낮춰야 하고, 그 대가가 있습니다 | 느린 Redis는 기본 기준으로는 브레이커를 열지 못합니다 |
| Redis가 돌아오면 장애가 끝난다 | 비어서 돌아오면 같은 부하가 상한 없이 오고, 데이터를 안고 돌아오면 못 지운 낡은 값이 남습니다 | Redis가 돌아온 뒤: 빈 캐시와 낡은 캐시 |
| 헬스 체크에 Redis를 넣는 것이 안전하다 | 준비 상태 확인이 Redis를 보면, 우회를 만들어 두어도 모든 서버가 트래픽에서 빠집니다 | 넷째: 헬스 체크가 멀쩡한 서버까지 빼 버립니다 |

## 근거

기준: Spring Boot [`v3.5.16`](https://github.com/spring-projects/spring-boot/tree/v3.5.16)이 관리하는 Spring Framework [`v6.2.19`](https://github.com/spring-projects/spring-framework/tree/v6.2.19), Spring Data Redis [`3.5.13`](https://github.com/spring-projects/spring-data-redis/tree/3.5.13), Lettuce [`6.6.0.RELEASE`](https://github.com/redis/lettuce/tree/6.6.0.RELEASE). Resilience4j는 Spring Boot가 관리하지 않으므로 확인 시점의 최신 릴리스 [`v2.4.0`](https://github.com/resilience4j/resilience4j/tree/v2.4.0)을 읽었습니다. Redis [`8.2.10`](https://github.com/redis/redis/tree/8.2.10) 소스, 공식 문서 [redis/docs `e53775dd`](https://github.com/redis/docs/tree/e53775dde0ee7e39ebe327e7ffff583caa9fbd65). 버전은 Spring Boot 3.5.16의 `spring-boot-dependencies` POM(`spring-framework.version` 6.2.19, `lettuce.version` 6.6.0.RELEASE)으로 확인했습니다.

**Spring Framework 6.2.19 (`spring-context`)**

- [`SimpleCacheErrorHandler.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/cache/interceptor/SimpleCacheErrorHandler.java): `handleCacheGetError` 등이 받은 예외를 그대로 `throw`("`@Cacheable`은 기본 설정에서 Redis 예외를 DB로 넘기지 않고 그대로 던집니다")
- [`LoggingCacheErrorHandler.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/cache/interceptor/LoggingCacheErrorHandler.java): `handleCacheGetError`·`handleCachePutError`·`handleCacheEvictError`·`handleCacheClearError` 모두 `logCacheError`만 호출("읽기 예외는 경고 로그를 남기고 미스로 처리되어", "쓰기·삭제 예외도 로그만 남깁니다")

**Spring Boot 3.5.16**

- [`LettuceConnectionConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/data/redis/LettuceConnectionConfiguration.java): `getProperties().getTimeout()`이 `null`이 아닐 때만 `commandTimeout`을 설정("`spring.data.redis.timeout`을 정하지 않으면 Lettuce의 기본 명령 타임아웃 60초가 쓰입니다", 아래 Spring Data Redis·Lettuce 항목과 함께)
- [`RedisHealthContributorAutoConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-actuator-autoconfigure/src/main/java/org/springframework/boot/actuate/autoconfigure/data/redis/RedisHealthContributorAutoConfiguration.java): `@ConditionalOnBean(RedisConnectionFactory.class)`, `@ConditionalOnEnabledHealthIndicator("redis")`("`RedisConnectionFactory` 빈이 있으면 Redis 헬스 지표를 자동으로 등록합니다")
- [`SimpleStatusAggregator.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-actuator/src/main/java/org/springframework/boot/actuate/health/SimpleStatusAggregator.java): 기본 순서 `DOWN`, `OUT_OF_SERVICE`, `UP`, `UNKNOWN`("전체 상태는 가장 나쁜 상태를 따르므로")
- [`SimpleHttpCodeStatusMapper.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-actuator/src/main/java/org/springframework/boot/actuate/health/SimpleHttpCodeStatusMapper.java): `DOWN`·`OUT_OF_SERVICE` → `STATUS_SERVICE_UNAVAILABLE`("HTTP 503을 돌려줍니다")
- [`AvailabilityProbesHealthEndpointGroups.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-actuator-autoconfigure/src/main/java/org/springframework/boot/actuate/autoconfigure/availability/AvailabilityProbesHealthEndpointGroups.java): `liveness` 그룹은 `livenessState`, `readiness` 그룹은 `readinessState`만 포함("이 그룹에는 기본으로 앱 자신의 생존·준비 상태만 들어 있습니다")
- [`AvailabilityProbesAutoConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-actuator-autoconfigure/src/main/java/org/springframework/boot/actuate/autoconfigure/availability/AvailabilityProbesAutoConfiguration.java): `ProbesCondition`이 `management.endpoint.health.probes.enabled`, 또는 Kubernetes·Cloud Foundry 플랫폼에서 일치("Kubernetes나 Cloud Foundry에서 실행될 때, 또는 `management.endpoint.health.probes.enabled`를 켰을 때")
- [`RedisCacheConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/cache/RedisCacheConfiguration.java): `RedisCacheManager.builder(redisConnectionFactory)`로 캐시 매니저를 만듦("Spring Boot가 자동으로 만든 Redis 캐시 매니저", 아래 Spring Data Redis 항목과 함께)

**Spring Data Redis 3.5.13**

- [`LettuceClientConfiguration.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceClientConfiguration.java): 빌더의 기본 `timeout = Duration.ofSeconds(RedisURI.DEFAULT_TIMEOUT)`("Lettuce의 기본 명령 타임아웃 60초")
- [`LettuceExceptionConverter.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceExceptionConverter.java): `convert`가 `ChannelException`·`RedisConnectionException` → `RedisConnectionFailureException`, `TimeoutException`·`RedisCommandTimeoutException` → `QueryTimeoutException`, `RedisCommandExecutionException`은 원인 예외를 다시 변환하고 원인이 없으면 `RedisSystemException`("Spring Data Redis는 Lettuce 예외를 다음처럼 바꿔 던집니다" 표)
- [`RedisCacheManager.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/RedisCacheManager.java): `builder(RedisConnectionFactory)` → `RedisCacheManagerBuilder.fromConnectionFactory` → `RedisCacheWriter.nonLockingRedisCacheWriter(connectionFactory)`. 직접 만든 쓰기 부품을 받는 `builder(RedisCacheWriter)`도 있음("`RedisCacheManager.builder(RedisCacheWriter)`로 캐시 매니저에 넣습니다"). [`RedisCacheWriter.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/RedisCacheWriter.java)는 인터페이스이고 `get`·`put`·`putIfAbsent`·`remove`·`clean`이 첫 인자로 캐시 이름을 받음("기본 구현을 감싸", "캐시 이름을 함께 받으므로"). [`DefaultRedisCacheWriter.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/DefaultRedisCacheWriter.java)의 `get(name, key, valueLoader, ttl, timeToIdleEnabled)`는 한 연결 작업 안에서 `doGet` 뒤 `valueLoader.get()`과 `doPut`을 실행하고, Spring Framework [`CacheAspectSupport.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/cache/interceptor/CacheAspectSupport.java)의 `executeSynchronized`가 `sync = true`일 때 값 로더를 넘겨 이 경로를 씀("`@Cacheable(sync = true)`가 쓰는 읽기는 기본 구현 안에서 … 한 번에 합니다"). `CacheErrorHandler`의 `handleCacheGetError`는 예외와 함께 `Cache`를 받음("어느 캐시의 오류인지 함께 받으므로")
- [`RedisCacheWriter.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/RedisCacheWriter.java): `nonLockingRedisCacheWriter(connectionFactory)`가 `BatchStrategies.keys()`를 씀("기본으로 `KEYS` 명령으로")
- Spring Framework [`AbstractCacheInvoker.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/cache/interceptor/AbstractCacheInvoker.java): `doGet`·`doPut`이 `cache.get`·`cache.put`에서 난 `RuntimeException`을 잡아 `handleCacheGetError`·`handleCachePutError`로 넘기고, 읽기는 처리기가 던지지 않으면 미스(`null`)로 돌려줌. Resilience4j [`CallNotPermittedException.java`](https://github.com/resilience4j/resilience4j/blob/v2.4.0/resilience4j-circuitbreaker/src/main/java/io/github/resilience4j/circuitbreaker/CallNotPermittedException.java)는 `RuntimeException`을 상속("`@Cacheable`에서 캐시를 읽고 쓰다 난 예외는 모두 캐시 에러 처리기로 넘어갑니다", "'열려 있음' 예외도 같은 처리기로 넘어가 미스가 되므로". 브레이커 위치에 따른 결과는 이 두 소스로 추론한 것)
- Spring Framework [`AbstractCacheInvoker.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/cache/interceptor/AbstractCacheInvoker.java) `doClear`가 `invalidate()` 또는 `clear()`를 부르고, [`Cache.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/cache/Cache.java)의 기본 `invalidate()`는 `clear()`를 부름. `RedisCache`는 `invalidate()`를 재정의하지 않음("`@CacheEvict(allEntries = true)`도 이 경로")
- [`RedisCache.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/RedisCache.java): `clear()` → `clear("*")` → `cacheWriter.clean(name, pattern)`, [`DefaultRedisCacheWriter.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/DefaultRedisCacheWriter.java)의 `clean`이 `batchStrategy.cleanCache` 호출
- [`BatchStrategies.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/BatchStrategies.java): `Keys`가 `keys(pattern)` 뒤 `del`, 주석에 `KEYS`가 키 공간 전체를 훑어 오래 막을 수 있다고 적음, `scan(int)` 전략 제공("`KEYS` 명령으로 그 캐시의 키를 찾은 뒤 `DEL`로 지웁니다", "`BatchStrategies.scan(...)`으로 바꾸면")

**Lettuce 6.6.0.RELEASE**

- [`RedisURI.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/RedisURI.java): `DEFAULT_TIMEOUT = 60`(초)
- [`ClientOptions.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/ClientOptions.java): `DEFAULT_AUTO_RECONNECT = true`, `DEFAULT_DISCONNECTED_BEHAVIOR = DisconnectedBehavior.DEFAULT`, `DEFAULT_REQUEST_QUEUE_SIZE = Integer.MAX_VALUE`, `DisconnectedBehavior.DEFAULT`의 주석 "자동 재연결이 켜져 있으면 명령을 받고, 꺼져 있으면 거절", `REJECT_COMMANDS`("끊긴 동안 들어온 명령을 거절하지 않고 받아 둡니다", "기본 상한은 아주 큰 값이라 사실상 제한이 없습니다", "`REJECT_COMMANDS`로 바꿉니다")

**Resilience4j 2.4.0**

- [`CircuitBreakerConfig.java`](https://github.com/resilience4j/resilience4j/blob/v2.4.0/resilience4j-circuitbreaker/src/main/java/io/github/resilience4j/circuitbreaker/CircuitBreakerConfig.java): `DEFAULT_FAILURE_RATE_THRESHOLD = 50`, `DEFAULT_SLOW_CALL_RATE_THRESHOLD = 100`, `DEFAULT_WAIT_DURATION_IN_OPEN_STATE = 60`(초), `DEFAULT_PERMITTED_CALLS_IN_HALF_OPEN_STATE = 10`, `DEFAULT_MINIMUM_NUMBER_OF_CALLS = 100`, `DEFAULT_SLIDING_WINDOW_SIZE = 100`, `DEFAULT_SLOW_CALL_DURATION_THRESHOLD = 60`(초), `DEFAULT_SLIDING_WINDOW_TYPE = COUNT_BASED`, `automaticTransitionFromOpenToHalfOpenEnabled = false`(서킷 브레이커 기본값 표). `slowCallRateThreshold`·`slowCallDurationThreshold`가 설정 객체 하나의 필드라 브레이커 하나에 한 값만 가짐("느린 호출 기준은 브레이커 하나에 거는 설정이라"). `DEFAULT_RECORD_EXCEPTION_PREDICATE = throwable -> true`("Resilience4j 기본은 모든 예외")
- [`CircuitBreakerStateMachine.java`](https://github.com/resilience4j/resilience4j/blob/v2.4.0/resilience4j-circuitbreaker/src/main/java/io/github/resilience4j/circuitbreaker/internal/CircuitBreakerStateMachine.java): 열림 상태의 `tryAcquirePermission`이 대기 시간이 지났으면 `toHalfOpenState()`로 바꾼 뒤 허가 여부를 판단, 허가하지 않으면 `CallNotPermittedException`("대기 시간이 지난 뒤 첫 호출이 올 때 반열림으로 바뀜", "곧바로 '열려 있음' 예외를 돌려주므로"). `handleThrowable`이 `ignoreExceptionPredicate`에 맞는 예외는 기록하지 않고, `recordExceptionPredicate`에 맞지 않는 예외는 `onSuccess`로 성공 기록("'세지 않는다'는 … 판정에서 아예 빼는 것", "성공으로 기록되어")
- [`CircuitBreakerMetrics.java`](https://github.com/resilience4j/resilience4j/blob/v2.4.0/resilience4j-circuitbreaker/src/main/java/io/github/resilience4j/circuitbreaker/internal/CircuitBreakerMetrics.java): `onSuccess`·`onError`가 걸린 시간이 `slowCallDurationThreshold`를 넘으면 느린 호출로 기록, `checkIfThresholdsExceeded`가 실패율과 느린 호출 비율을 각각 기준과 `>=`로 비교("실패율과 느린 호출 비율을 따로 계산하고, 둘 중 하나라도 자기 기준 이상이면", "느린 호출도 기본으로 기록하지만")
- [`BulkheadConfig.java`](https://github.com/resilience4j/resilience4j/blob/v2.4.0/resilience4j-bulkhead/src/main/java/io/github/resilience4j/bulkhead/BulkheadConfig.java): `DEFAULT_MAX_CONCURRENT_CALLS = 25`, `DEFAULT_MAX_WAIT_DURATION = 0`초, `maxConcurrentCalls`·`maxWaitDuration` 설정("허가증이 없으면 정해 둔 시간만큼 기다리거나 곧바로 실패합니다", "Resilience4j의 `Bulkhead`가 이 방식입니다")

**Redis 8.2.10과 공식 문서**

- [`src/server.c`](https://github.com/redis/redis/blob/8.2.10/src/server.c) `processCommand`: `server.loading && !server.async_loading && is_denyloading_command`이면 `shared.loadingerr`로 거절("읽는 동안 데이터를 다루는 명령에 `LOADING` 오류를 돌려줍니다")
- 공식 문서 [`KEYS`](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/keys.md): 운영 환경에서 큰 데이터베이스에 쓰면 성능을 떨어뜨릴 수 있다는 경고("`KEYS`는 데이터베이스의 키 전체를 훑는 동안 Redis 메인 스레드를 붙잡으므로")
- 공식 문서 [`CLIENT PAUSE`](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/client-pause.md): 정해 둔 시간 동안 일반 클라이언트의 명령 처리를 멈추고, `ALL`(기본)은 모든 명령을, `WRITE`는 쓰기 명령만 막음(아래 "장애 훈련: 두 모양을 따로 일으켜 봅니다")

## 실무 포인트

### 중요도별 점검 항목

| 중요도 | 항목 | 이유 |
| --- | --- | --- |
| 필수 | Redis를 쓰는 용도를 나열하고, 용도마다 "원본이 어디인가"와 장애 때의 동작을 정한다 | 우회는 캐시에만 뜻이 있고, 나머지는 기능마다 다른 선택이 필요합니다. 정하지 않으면 장애 순간의 기본 동작(예외, 60초 대기)이 대신 정합니다 |
| 필수 | 명령 타임아웃을 서비스 기한에 맞춰 정한다 | 기본 60초는 무응답 장애에서 요청 스레드를 1초 안에 바닥냅니다 |
| 필수 | 준비 상태 확인(readiness)에서 Redis 상태를 뺄지 정한다 | 빼지 않으면 우회를 만들어도 모든 서버가 트래픽에서 빠집니다 |
| 필수 | 캐시를 DB로 우회하기 전에 "동시 연결 수요 vs DB 여유"를 계산한다 | 계산 없는 우회는 Redis 장애를 DB 장애로 바꿉니다 |
| 필수 | 캐시 미스로 DB에 가는 경로에 상한을 둔다 | 장애 중, 빈 캐시로 돌아온 직후, 인기 키 만료 모두를 같은 장치로 막습니다 |
| 알면 유리 | 서킷 브레이커를 Redis 단위로 두고 기본값(최소 호출 100건, 열림 60초)을 서비스에 맞게 바꾼다 | 기본값은 요청이 적은 서버에서 늦게 열리고, 회복을 60초 늦게 압니다 |
| 알면 유리 | 장애 동안 지우지 못한 캐시 키를 어떻게 처리할지 정한다 | 데이터를 안고 돌아온 Redis가 틀린 값을 줍니다 |
| 알면 유리 | 느린 호출을 셀지, 센다면 어떤 용도에서 열지 정한다 | 느린 Redis는 실패율에 잡히지 않지만, 느림으로 열면 DB에 부하를 보낼 수 있습니다. 느린 호출 기준은 공유 브레이커가 아니라 그 용도에만 둔 브레이커에 겁니다 |
| 참고 | `disconnectedBehavior = REJECT_COMMANDS` | 끊김을 아는 경우만 빨라집니다. 무응답에는 효과가 없고 짧은 끊김의 이어 가기를 잃습니다 |

### 개발자가 정할 수 있는 것과 관찰해야 하는 것

| 정할 수 있는 것 | 관찰·검증해야 하는 것 |
| --- | --- |
| 용도별 장애 동작(빨리 실패, 우회, 사본, 축소) | DB가 쿼리 시간을 유지하는 동시 실행 수(부하 시험) |
| 명령 타임아웃, 브레이커 판정 기준과 열림 시간 | 피크 때 캐시와 무관한 API가 쓰는 DB 연결 수 |
| 미스 경로의 동시 실행 상한(서버당 값) | 캐시별 초당 요청 수와 적중률, 미스 쿼리 시간 |
| 헬스 체크 구성 | 정상 Redis 호출의 응답 시간 분포(타임아웃의 하한) |
| 장애 후 캐시 정리 방식 | 실제 장애에서 서버가 몇 대였는지(상한의 합계) |

### 관찰할 것

- **브레이커 상태와 거절된 호출 수**: 서버마다 봅니다. 한 대만 열려 있다면 그 서버와 Redis 사이의 네트워크를 의심합니다.
- **캐시 에러 처리기가 삼킨 예외 수**: 처리기를 바꾸면 에러율 지표에 드러나지 않으므로 따로 셉니다. 읽기·쓰기·삭제를 나눠 셉니다. 삭제 실패는 틀린 값의 신호입니다.
- **DB 연결 풀의 사용 중 연결 수와 연결을 기다리는 수**: 우회가 DB를 압박하는지 가장 먼저 보입니다.
- **벌크헤드가 거절한 요청 수**: 장애 동안 의도대로 넘친 요청을 끊고 있는지, 평소에도 거절이 생기는지(상한이 너무 낮은지) 봅니다.
- **요청 스레드 사용 수와 Redis 명령 지연 분포**: 느린 Redis를 실패율보다 먼저 보여 줍니다.
- **판단하지 못한 처리율 제한 요청 수, 마지막 사본을 준 응답 수**: 기능이 조용히 축소된 상태로 계속 운영되는지 봅니다.

### 장애 훈련: 두 모양을 따로 일으켜 봅니다

계산과 설정은 실제로 장애를 일으켜 봐야 맞는지 압니다. 운영 환경이 아니라 스테이징에서, 피크 비슷한 부하를 건 상태로 합니다.

| 훈련 | 일으키는 방법 | 확인할 것 |
| --- | --- | --- |
| 거절 | Redis 프로세스를 내림 | 요청이 타임아웃까지 기다리는가, 바로 실패하는가. 브레이커가 몇 초 만에 열리는가 |
| 무응답 | Redis 컨테이너를 일시 정지하거나, `CLIENT PAUSE`로 정해 둔 시간 동안 명령 처리를 멈춤 | 요청 스레드가 얼마나 묶이는가. 헬스 체크가 서버를 빼는가 |
| 빈 캐시로 회복 | 영속성 없이 다시 시작하거나 캐시 키를 지운 뒤 브레이커가 닫히게 둠 | DB 연결 수요가 상한에서 멈추는가 |
| 낡은 캐시로 회복 | 장애 중에 상품 정보를 바꾸고, 데이터가 남은 Redis로 되돌림 | 바뀐 값이 언제 보이는가 |

`CLIENT PAUSE`는 Redis가 응답하지 않는 상황을 흉내 내기 좋지만, 연결은 살아 있고 시간이 지나면 쌓인 명령을 한꺼번에 처리합니다. 실제 무응답과 같다고 보지 말고, "응답이 늦게 오거나 오지 않을 때 앱이 어떻게 하는가"를 보는 용도로 씁니다.

### 증상과 확인 순서

| 증상 | 먼저 볼 것 | 그다음 | 대응과 부작용 |
| --- | --- | --- | --- |
| Redis 장애와 함께 모든 API가 멈춤 | 요청 스레드 사용 수 | 명령 타임아웃 값, 브레이커 유무 | 타임아웃을 줄이고 브레이커를 둠. 너무 짧으면 정상 요청도 실패 |
| Redis 장애 뒤 주문 API까지 느려짐 | DB 연결을 기다리는 수, DB 쿼리 시간 | 캐시 에러 처리기가 미스로 넘기는지, 미스 경로의 상한 | 우회에 상한을 두거나 무거운 캐시는 우회하지 않음. 넘친 요청은 실패 |
| Redis 장애 때 로드 밸런서가 모든 서버를 뺌 | 준비 상태 확인 주소와 그 그룹의 구성 | Redis 헬스 지표가 그 그룹에 들어 있는지 | 준비 상태에서 Redis를 뺌. Redis 장애는 지표와 경보로 봄 |
| Redis 회복 직후 DB가 무너짐 | 브레이커가 닫힌 시각과 DB 연결 수요 | 상한이 대체 동작에만 있는지 | 상한을 미스 경로 자체로 옮김 |
| 장애 뒤 일부 상품이 옛 가격을 보임 | 장애 동안의 캐시 삭제 실패 수 | 캐시 TTL, 데이터가 남은 채 돌아왔는지 | 그 키를 지우거나 캐시를 비움. 비우면 몰림이 따라옴 |

## 말로 설명하기

### 30초 버전

Redis가 멈췄을 때 DB로 우회해도 되는 것은 DB가 원본인 캐시뿐입니다. 세션이나 처리율 제한처럼 Redis가 원본인 데이터는 DB에 없으므로 빨리 실패하거나 기능을 줄여야 합니다. 캐시도 그냥 우회하면 DB 읽기가 1 ÷ 미스율 배로 늘어, 쿼리 시간을 곱한 동시 연결 수가 DB 여유를 넘으면 Redis 장애가 DB 장애가 됩니다. 그래서 타임아웃과 서킷 브레이커로 빨리 알아채고, 우회에는 동시 실행 상한을 두며, Redis가 돌아온 뒤의 빈 캐시와 낡은 캐시까지 대비합니다.

### 2분 버전

먼저 Redis를 어디에 쓰는지 나눕니다. 상품 캐시처럼 DB가 원본이면 DB에서 다시 읽을 수 있지만, 세션·처리율 제한·락처럼 Redis가 원본이면 DB에 물을 데이터가 없습니다. 그런 용도는 빨리 실패하거나(세션), 열어 두거나 닫거나(처리율 제한), DB 고유 제약 같은 다른 최종 장치에 맡깁니다(멱등 키).

캐시를 우회할지는 계산으로 정합니다. 적중률 98%인 캐시가 사라지면 DB 읽기는 50배가 됩니다. 이것을 초당 조회 수 × 쿼리 시간, 곧 동시에 필요한 DB 연결 수로 바꿔, 연결 풀 크기가 아니라 부하 시험으로 잰 DB의 여유와 비교합니다. 예시에서 가벼운 상품 캐시는 여유 안이라 상한을 두고 우회하고, 무거운 순위 집계는 여유를 넘어 마지막 사본을 주거나 숨깁니다. 여유를 넘으면 쿼리가 느려지고 그만큼 연결이 더 필요해지는 되먹임으로 DB가 급격히 무너지고, 주문 같은 무관한 API도 함께 느려집니다.

빨리 알아채는 것도 필요합니다. Lettuce의 기본 명령 타임아웃 60초에서는 요청 스레드가 1초도 안 돼 바닥나고, 연결이 끊긴 경우에도 Lettuce가 명령을 받아 두므로 다시 연결되지 않는 한 타임아웃까지 기다립니다. 타임아웃을 줄이면 한 요청의 손해에 상한이 생기고, 서킷 브레이커가 열리면 기다림 자체가 없어집니다. 헬스 체크에 Redis가 들어 있으면 모든 서버가 트래픽에서 빠지므로 준비 상태 확인에서는 뺍니다.

마지막으로 회복입니다. 비어서 돌아온 Redis는 브레이커를 닫자마자 장애 때와 같은 부하를 DB에 보내므로, 상한은 대체 동작이 아니라 미스 경로 자체에 둡니다. 데이터를 안고 돌아온 Redis에는 장애 동안 지우지 못한 옛 값이 남으므로, 그 키를 다시 지우거나 캐시를 비웁니다.

### 예상 질문과 꼬리 질문

**Q1. Redis가 멈추면 DB에서 읽으면 되지 않나요?**

캐시에만 해당합니다. DB가 원본이라 다시 읽을 수 있기 때문입니다. 세션, 처리율 제한 카운터, 분산 락은 Redis가 원본이라 DB에 같은 데이터가 없습니다. 그리고 캐시라도 DB가 그 부하를 받아낼 수 있을 때만 우회가 서비스를 지킵니다. (용도마다 답이 다릅니다: DB로 우회할 수 있는 것은 캐시뿐입니다)

- **꼬리: 세션을 DB에도 같이 저장해 두면 우회할 수 있지 않나요?** 가능하지만 그것은 장애 대응이 아니라 저장소를 둘 두는 설계입니다. 매 요청의 세션 쓰기가 DB에도 가고, 두 저장소가 엇갈릴 때 어느 쪽이 맞는지 정해야 합니다. 세션을 Redis에 둔 이유가 DB 부하였다면 그 이유를 되돌립니다. 세션 장애 대응의 선택지는 [Redis 세션 저장소](./session-store.md)에서 다룹니다.

**Q2. DB가 우회 부하를 견딜지는 어떻게 판단하나요?**

평소 미스율이 100%가 된다고 보고, 초당 조회 수 × 쿼리 하나의 시간으로 동시에 필요한 연결 수를 구합니다. 이것을 "DB가 쿼리 시간을 유지하는 동시 실행 수 − 다른 API가 피크에 쓰는 수"와 비교합니다. 예시에서 상품 캐시는 20개로 여유 30개 안이고, 순위 캐시는 40개로 넘습니다. (횟수가 아니라 동시에 필요한 DB 연결 수로 비교합니다)

- **꼬리: 왜 초당 조회 수로 비교하지 않나요?** 쿼리 무게가 빠지기 때문입니다. 순위는 초당 100건이지만 400ms짜리라 초당 2,000건의 10ms 조회보다 두 배의 연결을 요구합니다.
- **꼬리: 연결 풀이 80개인데 왜 60개를 기준으로 하나요?** 풀은 앱이 보낼 수 있는 상한일 뿐, DB가 감당하는 양이 아니기 때문입니다. 예시의 DB는 60개를 넘으면 쿼리 시간이 늘었습니다. 이 값은 부하 시험이나 운영 지표로 잽니다. (여유는 연결 풀 크기가 아니라 DB가 버티는 동시 실행 수에서 옵니다)
- **꼬리: 계산상 여유 안이면 안전한가요?** 쿼리 시간이 그대로라는 가정 위의 계산입니다. 몰리면 쿼리가 느려지고 필요한 연결이 다시 늘어나는 되먹임이 있으므로, 여유의 일부만 배정하고 상한으로 묶습니다. (몰리면 쿼리가 느려지고, 느려지면 더 몰립니다)

**Q3. `@Cacheable`에 `LoggingCacheErrorHandler`를 넣었으니 Redis 장애에 대비한 것 아닌가요?**

DB로 넘어가게 만들었을 뿐, 세 가지가 남습니다. 첫째, 넘어가기 전에 명령 타임아웃까지 기다립니다. 기본값이면 60초입니다. 둘째, 넘어간 요청을 막는 상한이 없어 DB 부하가 그대로 갑니다. 셋째, 삭제 실패도 로그만 남겨, Redis가 데이터를 안고 돌아오면 옛 값이 보입니다. (셋째: 기다림을 넘긴 요청은 막는 장치 없이 DB로 갑니다, 데이터를 안고 돌아오면: 장애 동안 지우지 못한 값이 남습니다)

- **꼬리: 그럼 기본 처리기(예외를 던짐)가 더 낫나요?** DB가 우회 부하를 받지 못하는 캐시라면 그렇습니다. 빨리 실패하는 편이 DB와 다른 API를 지킵니다. 다만 기본 처리기도 타임아웃까지 기다린 뒤 던지므로, 타임아웃과 브레이커는 어느 쪽이든 필요합니다.

**Q4. Redis 프로세스가 죽어서 연결이 끊겼다면 Lettuce가 바로 실패시키지 않나요?**

기본 설정에서는 아닙니다. 자동 재연결이 켜져 있으면 끊긴 동안의 명령을 거절하지 않고 받아 두므로, Redis가 돌아오지 않는 동안 동기 호출은 명령 타임아웃까지 기다립니다. 그 사이 다시 연결되면 받아 둔 명령이 나가 더 일찍 끝납니다. `disconnectedBehavior`를 `REJECT_COMMANDS`로 바꾸면 끊김을 아는 동안은 바로 실패합니다. (둘째: 끊긴 연결에서도 기다립니다)

- **꼬리: 그럼 항상 `REJECT_COMMANDS`로 두면 되나요?** 연결은 살아 보이는데 답이 없는 장애에는 효과가 없고, 짧은 끊김을 다시 연결로 넘길 기회를 잃습니다. 무응답까지 다루는 장치는 타임아웃과 브레이커입니다.

**Q5. 명령 타임아웃을 50ms로 줄였는데 서킷 브레이커가 왜 또 필요한가요?**

타임아웃은 요청 하나가 기다리는 시간에 상한을 둘 뿐이고, 장애 동안 모든 요청이 그 50ms를 치릅니다. 예시에서 요청 하나가 Redis를 세 번 부르면 150ms가 붙고, 서버당 스레드 80개가 묶입니다. 브레이커가 열리면 Redis를 부르지 않으므로 이 기다림이 없어집니다. (명령 타임아웃: 한 요청이 기다리는 상한, 서킷 브레이커: 기다림 자체를 없앱니다)

- **꼬리: Resilience4j 기본값 그대로 쓰면 되나요?** 최근 100건 중 50% 실패면 열리고, 열리면 60초 동안 유지합니다. 요청이 적은 서버는 100건이 모이는 데 오래 걸려 늦게 열리고, Redis가 10초 만에 돌아와도 60초 동안 대체 동작을 계속합니다. 서비스의 요청량과 장애 조치 시간에 맞춰 바꿉니다.
- **꼬리: 용도마다 브레이커를 따로 두면 안 되나요?** 연결 실패와 타임아웃은 같은 Redis의 문제라 원인이 하나입니다. 따로 두면 요청이 적은 용도가 늦게 열립니다. 연결 실패와 타임아웃을 세는 상태는 Redis 하나에 하나로 두고, 열렸을 때의 대체 동작만 용도마다 다르게 하는 편이 자연스럽습니다. 느린 호출을 세는 브레이커는 예외입니다. 느림으로 열어도 되는 용도에만 따로 둡니다. 공유 브레이커에 느린 호출 기준을 걸면 DB로 우회하는 용도까지 함께 열리기 때문입니다. 또 쓰기만 거부하는 오류(`MISCONF`, OOM)는 쓰기 쪽만 막아야 하므로 같은 브레이커에서 세지 않습니다.
- **꼬리: 무엇을 실패로 세나요?** 연결 실패, 타임아웃, `LOADING`처럼 Redis 전체가 일을 못 받는 신호입니다. 직렬화 실패나 `WRONGTYPE`처럼 앱 쪽 문제는 세지 않습니다. 세면 멀쩡한 Redis를 끊습니다. (무엇을 실패로 셀 것인가)

**Q6. Redis가 느려지기만 하면 브레이커가 도와주나요?**

기본값으로는 아닙니다. 호출이 성공하므로 실패율이 오르지 않고, 느린 호출도 기록은 하지만 기본 기준이 60초·100%라 30ms 같은 느림은 느린 호출로 세지 않습니다. 느린 호출 기준을 명령 타임아웃보다 짧게 두고 비율 기준을 낮추면 열 수 있습니다. 대가는 느리지만 답하던 Redis를 버리고 DB로 우회한다는 것입니다. 잠깐의 느림에도 DB에 수십 배의 부하가 갈 수 있으므로, 느림으로 열 용도는 마지막 사본이나 기능 축소처럼 DB에 부담을 주지 않는 대체 동작이 있는 곳으로 한정하고, 그 용도에만 느린 호출 기준을 건 브레이커를 따로 둡니다. (느린 Redis는 기본 기준으로는 브레이커를 열지 못합니다)

**Q7. 상한을 서버당 6개로 정했는데, 서버가 늘면 어떻게 되나요?**

상한은 서버마다 걸리므로 합계가 서버 수에 비례합니다. 8대면 48개로 여유 30개를 넘습니다. 예상하는 최대 서버 수로 나눠 정하거나, DB 쪽에서 계정별 동시 실행 제한 같은 마지막 상한을 둡니다. (상한은 서버마다 걸린다는 것을 잊기 쉽습니다)

- **꼬리: 서버 한 대로 부하 시험하면 되지 않나요?** Redis 장애는 대개 모든 서버에서 동시에 일어나므로, 한 대의 결과로는 DB가 받는 합계를 알 수 없습니다.

**Q8. Redis가 돌아오면 무엇을 조심하나요?**

두 가지입니다. 비어서 돌아오면 반열림의 시험 호출이 미스(정상 응답)를 받아 브레이커가 닫히고, 장애 때와 같은 부하가 이번에는 평소 경로로 DB에 갑니다. 상한이 대체 동작에만 있으면 막을 것이 없습니다. 데이터를 안고 돌아오면 장애 동안 바뀐 데이터의 캐시가 지워지지 않은 채 남아 옛 값이 보입니다. (Redis가 돌아온 뒤: 빈 캐시와 낡은 캐시)

- **꼬리: 그럼 회복하자마자 캐시를 전부 비우면 되나요?** 낡은 값은 없어지지만 빈 캐시의 몰림을 스스로 만듭니다. 그리고 Spring Boot 기본 캐시 매니저의 전체 삭제는 `KEYS`로 키를 찾으므로, 키가 많으면 막 돌아온 Redis의 메인 스레드를 오래 붙잡습니다. 바뀐 키를 기록해 두었다가 그것만 지우는 편이 부작용이 작고, 전체를 비운다면 `SCAN` 방식으로 바꾸고 미스 경로의 상한을 갖춘 뒤에 합니다.
- **꼬리: 반열림 시험 호출이 성공했는데 왜 문제인가요?** 시험 호출은 "Redis가 응답한다"만 확인합니다. "캐시가 채워져 있다"나 "DB가 몰림을 견딘다"는 확인하지 않습니다.

**Q9. 헬스 체크에서 Redis를 빼면 Redis 장애를 어떻게 알아채나요?**

헬스 체크는 "이 서버에 트래픽을 보낼까"를 정하는 장치이지 장애 알림 장치가 아닙니다. Redis 장애는 브레이커 상태, Redis 명령 지연, 캐시 에러 수 같은 지표와 경보로 봅니다. 준비 상태에 Redis를 넣으면 Redis 장애 순간 모든 서버가 빠져, 앱이 할 수 있던 우회나 오류 응답조차 못 하게 됩니다. (넷째: 헬스 체크가 멀쩡한 서버까지 빼 버립니다)

- **꼬리: 세션처럼 Redis 없이는 거의 아무것도 못 하는 서버라면 넣어도 되지 않나요?** 그래도 공개 화면은 처리할 수 있고, 빼지 않으면 사용자는 오류 응답 대신 연결 거부를 받습니다. 또 모든 서버가 동시에 빠졌다가 동시에 돌아오면 그 순간 몰림이 생깁니다. 이 문서는 빼는 쪽을 권하지만, 서비스의 성격에 따라 판단할 일입니다.

**Q10. 장애 대응을 설계했다는 것을 어떻게 확인하나요?**

스테이징에서 피크 비슷한 부하를 건 채로 "거절"(프로세스 종료)과 "무응답"(컨테이너 일시 정지, `CLIENT PAUSE`)을 따로 일으키고, 빈 캐시와 낡은 캐시로 회복하는 경우까지 시험합니다. 볼 것은 요청 스레드 사용 수, 브레이커가 열리는 데 걸린 시간, DB 연결을 기다리는 수, 헬스 체크 결과, 바뀐 값이 보이기까지의 시간입니다. (장애 훈련: 두 모양을 따로 일으켜 봅니다)

- **꼬리: `CLIENT PAUSE`면 실제 무응답과 같나요?** 아닙니다. 연결은 살아 있고, 정해 둔 시간이 지나면 쌓인 명령을 한꺼번에 처리합니다. "응답이 늦게 오거나 오지 않을 때 앱이 어떻게 하는가"를 보는 흉내로 씁니다.
