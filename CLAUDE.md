# Redis 소스 스터디 — Claude 작업 지침

이 저장소는 Redis 소스 스터디 노트입니다. 기준 소스는 `redis/redis` unstable 브랜치 커밋 `b540ca4`(2026-09-30)입니다.

## 사용자와 작업 방식

- 사용자는 Spring(Kotlin) 백엔드 개발자입니다. 설명은 한국어로 합니다.
- **C 코드를 직접 읽지 않습니다.** 코드 없는 흐름도 문서(07~09)와 주석만 봅니다.
- 계획과 문서에 과투자하고 실행을 미루는 경향이 있다고 스스로 밝혔습니다. 선택지를 늘어놓지 말고 하나를 골라 이유와 함께 제안하고, 실행(실험) 쪽으로 이끕니다. 이미 합의한 사항을 다시 설계하려 하면 아래 "합의된 조정"을 근거로 실행 쪽으로 돌립니다.
- 응답 끝에는 지금 바로 할 구체적인 행동 하나를 한 줄로 적습니다.
- 사용자가 요청하지 않은 코드는 쓰지 않습니다. 접근 방법을 먼저 제안합니다.

## 문서 작성 규칙

- **배경지식이 없는 사람 기준으로 씁니다.** 소스 주석을 옮기듯 쓰지 않습니다. 할당, 포인터, 캐시, sds, kvobj, embstr 같은 용어와 전제를 먼저 설명합니다. 새 문서나 절의 앞쪽에 "읽기 전에" 절(기본 그림 + 용어 사전)을 둡니다. 그림에서 빠진 요소가 있으면(예: 키의 위치) 왜 빠졌는지 적습니다.
  (2026-10-13 사용자 지적: "09 전체가 배경지식 있는 사람 기준이라 kvobj·embstr·kvBits 같은 단어부터 생소하다, 키가 헤더 어디에도 없어 혼란스러웠다")
- 코드 없는 흐름도에서는 함수 이름을 괄호 안에만 적습니다.
- 문서의 주장은 upstream 원본으로 검증합니다. 방식: 컨텍스트를 새로 받은 에이전트가 원본과 대조해 `검증_주제N.md`를 쓰고(발췌 코드 일치, 문서 주장, 주석), 사용자 확인 후 반영합니다. 반영할 때 주석만 바뀌고 코드는 그대로인지 확인합니다.

## 커리큘럼

주제 10개를 **주제 2개씩 실제 1주에** 묶어 진행합니다. "실제 주차"와 "주제 번호"를 구분해서 말합니다.

| 실제 주차 | 주제 | 핵심 질문 | 소스 포인트 |
|---|---|---|---|
| 1주 | 1 Event Loop | 싱글 스레드인데 왜 빠른가? | ae.c, networking.c, server.c |
| 1주 | 2 Dict / SDS | Key를 어떻게 빠르게 찾는가? | dict.c, sds.c |
| 2주 | 3 Object / Encoding | 같은 자료형의 내부 표현은 왜 달라지는가? | object.c, listpack.c, t_hash.c |
| 2주 | 4 ZSET / Skip List | ZRANK는 어떻게 빠르게 순위를 계산하는가? | t_zset.c, skiplist |
| 3주 | 5 Atomicity / Lua | Lua는 왜 atomic한가? | scripting, multi.c |
| 3주 | 6 Redis Streams | Consumer가 처리 중 죽으면? | t_stream.c |
| 4주 | 7 Persistence | Redis가 죽으면 데이터는 얼마나 보존되는가? | rdb.c, aof.c |
| 4주 | 8 Replication / Distributed Lock | Redis Lock만으로 정합성을 보장할 수 있는가? | replication, expiration |
| 5주 | 9, 10 | (아직 공유받지 못함) | |

검증 가설:
1. 오래 걸리는 명령 하나가 이벤트 루프를 점유하면, 그 명령과 무관한 요청의 p99도 오른다.
2. 조회는 평균 O(1)을 유지하지만, resize나 점진적 rehash 구간에서는 tail latency가 오른다.
3. 인코딩 전환과 Big Key 구간에서는 메모리뿐 아니라 latency에도 유의미한 변화가 있다.
4. 10만 → 300만 명으로 늘어도 ZRANK는 선형으로 늘지 않는다. 다만 ZADD와 ZRANK가 한 키에 몰리면(Hot Key) p99가 오른다.
5. 조회와 차감을 나누면 race가 생기고, Lua로 묶으면 막힌다. 대신 스크립트가 길어질수록 p99가 오른다.
6. PEL로 복구는 되지만 ACK 전에 처리가 끝나면 재처리가 생기므로 멱등성이 필요하다.
7. 보존 수준을 높일수록 throughput과 latency 비용이 생기고, 그래도 성공 응답을 받은 데이터 일부를 잃을 수 있다.
8. TTL 초과나 failover가 나면 Worker 둘이 동시에 critical section에 들어갈 수 있으므로, DB 쪽 방어 수단이 함께 필요하다.

## 현재 상태 (2026-10-13)

- **주제 1~4 문서화와 검증 끝.** 코드 없는 흐름도: 07(주제 1), 08(주제 2), 09(주제 3·4). 검증 보고서: `검증_주제1~3.md`, 모두 문서에 반영함.
- **가설 실험은 아직 0건. 다음 할 일은 09 8절 예측표대로 실험**(①~④). 주제 1 가설은 ④에서, 주제 2 가설은 ②에서 큰 ZSET을 채우며 같이 확인합니다.
- 실험 환경: `os/README.md`의 Docker(OrbStack, `redis:8.0` 이미지). 리눅스라 epoll과 embstr 기준(CACHE_LINE_SIZE 64)이 운영 환경과 같습니다. Apple Silicon Mac에서 직접 빌드하면 CACHE_LINE_SIZE가 128이라 결과가 달라집니다. `redis:8.0`의 ZSET은 8.6 이전 구조입니다(복잡도는 같음).
- 검증에서 나온 실험 후보: (1) rehash 도중 BGSAVE가 시작되면 자식 종료까지 load factor 상한이 없음(주제 2·7), (2) io-threads > 1 + appendfsync always에서 fsync 전에 응답이 나갈 가능성(upstream 동작 의심, 주제 7).

## 합의된 조정

- 3·4는 대기열 ZSET 하나로 실험을 묶는다: 128→129개 인코딩 전환, 10만/100만/300만 ZRANK `usec_per_call`, DEL vs UNLINK, ZADD+ZRANK 동시 부하 p99.
- 3·4는 문서 1개(09)로 끝냈다. t_zset.c 주석 발췌는 만들지 않는다. **문서를 더 늘리지 말고 실험한다.**
- 5~8은 소스 분석보다 Spring 쪽 실험에 무게를 둔다 (DECR/Lua/MULTI/WATCH 비교, Cluster CROSSSLOT, Redisson watchdog, fencing token).
- 6(Streams)은 축소한다: t_stream.c 분석은 빼고 XAUTOCLAIM 재처리 실험 하나만. 국내 Spring 환경은 사실상 Kafka라 가치가 낮다.
