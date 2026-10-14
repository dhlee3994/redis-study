# Redis 소스 공부 노트

## 읽는 순서

### 0부: 배경지식

| 파일 | 내용 |
|---|---|
| [00_배경지식_1-4주.md](00_배경지식_1-4주.md) | **실제 1~4주(주제 1~8)를 읽기 전 배경지식.** 메모리·포인터·캐시·스레드·복잡도·p99부터 epoll, 해시 테이블, 스킵리스트, race condition, ACK/멱등성, fsync, 복제, 분산 락까지 비유로 설명. 맨 위 표에서 지금 주제의 절만 골라 읽고, 모르는 단어는 5절 용어 사전에서 찾기 |
| [09_통합흐름도_배경지식.md](09_통합흐름도_배경지식.md) | **3부(09)를 읽기 전 배경지식.** 놀이공원 대기 명단 비유로 키·값, 메모리, 타입과 인코딩, SDS·kvobj·kvBits, listpack·intset·quicklist, dict, skiplist와 span, 인코딩 전환, 실험표(p99·usec_per_call) 읽는 법까지 설명. 09에서 막히면 18절 표에서 대응하는 절을 찾기 |

### 1부: 이벤트루프

| 순서 | 파일 | 내용 |
|---|---|---|
| 0 | [07_이벤트루프_통합흐름도.md](07_이벤트루프_통합흐름도.md) | **코드 없이 흐름만.** ae·networking·server를 그림 한 벌로 합친 요약, 루프가 멈추는 원인, 면접용 30초 설명. 이것만 읽어도 1부 핵심은 충분 |
| 1 | [01_C문법_최소가이드.md](01_C문법_최소가이드.md) | 노트 전체(1~3부)를 읽는 데 필요한 C 문법을 Kotlin과 비교해 정리. 맨 위 표에서 부별로 필요한 절만 골라 보면 됨 |
| 2 | [02_이벤트루프_흐름지도.md](02_이벤트루프_흐름지도.md) | 루프 한 바퀴의 흐름, 자료구조 그림, 주의할 점 |
| 3 | [ae/ae.h](ae/ae.h) | 구조체와 상수 (원본 + 한국어 주석) |
| 4 | [ae/ae.c](ae/ae.c) | `aeMain` → `aeProcessEvents` → 타이머 → 등록 순으로 재배치 (원본 + 한국어 주석) |
| 5 | [ae/ae_epoll.c](ae/ae_epoll.c) | 리눅스 epoll과 만나는 부분 (원본 + 한국어 주석) |
| 6 | [03_networking_흐름지도.md](03_networking_흐름지도.md) | 클라이언트의 일생(접속 → 읽기 → 파싱 → 실행 → 응답 → 종료), 핵심 설계 6가지 |
| 7 | [networking/networking_발췌.c](networking/networking_발췌.c) | networking.c 6131줄 중 이벤트루프 경로 함수만 실행 순서대로 발췌 (원본 + 한국어 주석) |
| 8 | [04_server_흐름지도.md](04_server_흐름지도.md) | 서버 시작 순서, 메인 루프에 등록되는 것 전부, **세 파일을 합친 루프 한 바퀴 그림**, beforeSleep 순서의 이유, serverCron |
| 9 | [server/server_발췌.c](server/server_발췌.c) | server.c 8615줄 중 main, initServer, serverCron, beforeSleep, afterSleep, processCommand, call 발췌 (원본 + 한국어 주석) |

### 2부: 해시 테이블 (dict)

| 순서 | 파일 | 내용 |
|---|---|---|
| 0 | [08_dict_통합흐름도.md](08_dict_통합흐름도.md) | **코드 없이 흐름만.** 평균 O(1)의 이유, 점진적 rehash, tail latency 지점, SDS가 필요한 이유, 면접용 30초 설명. 이것만 읽어도 2부 핵심은 충분 |
| 10 | [05_dict_흐름지도.md](05_dict_흐름지도.md) | 키가 늘어도 평균 조회가 O(1)인 이유, resize 조건, 점진적 rehash, **tail latency가 생기는 지점 9가지**, 직접 해 보는 실험 |
| 11 | [dict/dict.h](dict/dict.h) | 원본 전체 (원본 + 한국어 주석). `struct dict`의 테이블 2개와 `rehashidx` |
| 12 | [dict/dict_발췌.c](dict/dict_발췌.c) | dict.c 2397줄 중 조회, 삽입, 확장/축소, 점진적 rehash 발췌 (원본 + 한국어 주석) |
| 13 | [dict/kvstore_발췌.c](dict/kvstore_발췌.c) | kvstore.c 1176줄 중 rehash 중인 dict 목록과 cron 진입점 발췌 (원본 + 한국어 주석) |
| 14 | [dict/주변_발췌.c](dict/주변_발췌.c) | server.h, server.c, db.c, t_string.c, object.c, sds.h에서 dict와 맞닿은 부분 (해시 함수, resize 정책, databasesCron, GET 경로, sdslen) |

### 3부: 인코딩 (주제 3·4)

| 순서 | 파일 | 내용 |
|---|---|---|
| 0 | [09_인코딩_ZSET_통합흐름도.md](09_인코딩_ZSET_통합흐름도.md) | **코드 없이 흐름만 (주제 3·4).** 타입별 인코딩과 변환, Big Key 해제 비용, ZSET의 dict+skiplist와 span, ZRANK·ZADD 경로, **실험 예측표** |
| 15 | [06_hash_set_인코딩_흐름지도.md](06_hash_set_인코딩_흐름지도.md) | 작을 때 listpack/intset을 쓰는 이유, dict로 바뀌는 조건과 비용, **큰 Hash/Set 안의 dict가 메인 키 공간과 다른 점**, 실험 |
| 16 | [hashset/t_set_발췌.c](hashset/t_set_발췌.c) | t_set.c 2047줄 중 인코딩 선택, 변환, 조회 + intset.c 부록 (원본 + 한국어 주석) |
| 17 | [hashset/t_hash_발췌.c](hashset/t_hash_발췌.c) | t_hash.c 6910줄 중 listpack → dict 변환과 HSET/HGET 경로 (원본 + 한국어 주석) |
| 18 | [encoding/object.h](encoding/object.h) | 원본 전체 (원본 + 한국어 주석). 값 객체(robj)와 kvobj 구조체, 타입별 인코딩 표 |
| 19 | [encoding/object_발췌.c](encoding/object_발췌.c) | object.c 2028줄 중 객체 생성, String 인코딩 선택(int/embstr/raw), 해제, `OBJECT ENCODING` 발췌 (원본 + 한국어 주석) |
| 20 | [encoding/t_string_발췌.c](encoding/t_string_발췌.c) | t_string.c 1678줄 중 `SET` 한 번에 값의 인코딩이 정해지고 바뀌는 길 + networking.c, db.c 일부 (원본 + 한국어 주석) |
| 21 | [encoding/listpack_발췌.c](encoding/listpack_발췌.c) | listpack.c 3588줄 중 형식, 찾기, 삽입·삭제 (원본 + 한국어 주석) |
| 22 | [encoding/t_list_발췌.c](encoding/t_list_발췌.c) | t_list.c 1756줄 중 listpack ↔ quicklist 양방향 변환과 히스테리시스 (원본 + 한국어 주석) |
| 23 | [encoding/lazyfree_발췌.c](encoding/lazyfree_발췌.c) | lazyfree.c 391줄 + db.c 중 DEL/UNLINK 경로와 lazyfree 설정. Big Key 해제 비용 (원본 + 한국어 주석) |

`ae/` 폴더의 파일은 원본 코드를 그대로 두고, 원본의 영문 주석을 한국어로 옮겼습니다(저작권·라이선스 문구만 원문 유지). `▶`로 시작하는 주석은 추가한 해설입니다. `dict/dict.h`는 원본 코드와 영문 주석을 그대로 두고 `▶` 주석만 추가했습니다. 두 경우 모두 주석을 걷어낸 코드는 원본과 같은지 확인했습니다. 단 `ae/ae.c`는 함수 블록을 읽는 순서(호출 순서)로 옮겼습니다(블록 내용은 원본과 같음).

`networking/`, `server/`, `dict/`, `hashset/`, `encoding/`의 발췌 파일은 함수를 골라 순서를 바꿨고, 긴 부분은 `▶ [생략]`으로 표시하고 뺐습니다. 뺀 부분 외의 코드는 원본과 같은지, 적어둔 원본 줄 번호가 맞는지 확인했습니다.

### 부록: OS 개념

| 파일 | 내용 |
|---|---|
| [os/README.md](os/README.md) | 소스를 읽다 막히는 OS 개념 14개(fd, epoll, jemalloc, 단편화, bio, fork, COW, fsync 등). **주제별로 언제 읽을지** 표와 Docker 실험 환경. 처음부터 다 읽지 말고 지금 주제의 줄만 |

## 다음 단계 (선택)

- iothread.c: I/O 스레드와 메인 스레드가 클라이언트를 주고받는 방법, 8.0의 I/O 스레드별 이벤트루프
- blocked.c: BLPOP 같은 블로킹 명령
- expire.c `activeExpireCycle`: "조금씩" 원칙의 대표 예
- aof.c `flushAppendOnlyFile`: appendfsync 정책별 차이
- memory_prefetch.c: 여러 명령의 조회를 모아 메모리를 미리 불러오는 기능
