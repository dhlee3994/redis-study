# Redis 메모리 지표: 숫자마다 무엇을 세는가

> **핵심 질문**: `used_memory`, `used_memory_rss`, `mem_fragmentation_ratio`, `MEMORY USAGE`처럼 Redis 메모리 숫자가 서로 다른 값을 가리킬 때, 각각은 무엇을 세는가? `maxmemory`는 어느 숫자로 판단하며, "메모리가 찼다"는 증상은 무엇부터 확인하는가?
>
> **범위**: 단일 Redis 서버의 메모리 지표를 다룹니다. 수치 계산은 Linux x86-64에서 Redis가 함께 배포하는 jemalloc으로 빌드했을 때를 기준으로 합니다. 메모리 한도에 닿았을 때 키를 지우는 축출의 동작은 [TTL과 축출](./ttl-and-eviction.md)에서, 큰 키를 찾는 도구의 세부는 Hot Key와 Big Key에서, 자료형마다의 내부 표현은 [String과 Hash](./string-vs-hash.md)에서 다룹니다.

## 배경 지식

### 이 문서의 예시: 상품 캐시 서버 한 대

문서 끝까지 다음 서버 하나를 씁니다.

- Spring 애플리케이션이 상품 상세를 JSON으로 직렬화해 `RedisTemplate`으로 String에 저장합니다. 키는 `product:1001`처럼 12바이트이고, 값은 300바이트, TTL은 1시간입니다.
- 키가 100만 개 있습니다. 키 이름과 값의 바이트만 더하면 (12 + 300) × 100만 = 3억 1,200만 바이트입니다. 이것을 이 문서에서 **데이터 자체 크기**라고 부릅니다. 이 문서에서 예시 계산의 MB는 100만 바이트입니다. 반면 Redis 설정의 `mb`·`gb`와 이 문서가 옮긴 설정 기본값(256MB, 100MB 등)은 1,024 기반이라 1MB = 1,048,576바이트입니다. 컨테이너 한도의 1GiB도 1,024 기반입니다. 예시의 312MB는 약 298MiB입니다.
- Redis는 메모리 한도가 1GiB인 컨테이너에서 돕니다. 주 서버이고 복제본이 하나 붙어 있으며, RDB 저장(메모리의 키와 값을 한 시점 기준으로 파일에 떠 두는 영속화)을 켜 두었습니다. 상품이 바뀌면 다른 애플리케이션 서버에 알리려고 Pub/Sub 채널 하나에 메시지를 보냅니다.

이 서버에서 다음 네 장면이 차례로 일어난다고 하고, 장면마다 숫자가 어떻게 움직이는지 봅니다.

1. 100만 개를 적재합니다.
2. 단종 상품 50만 개를 지웁니다.
3. RDB 저장이 시작되어 자식 프로세스가 도는 동안 쓰기가 몰립니다.
4. Pub/Sub 구독자 하나가 느려져 메시지를 제때 읽지 못합니다.

### 메모리 할당과 할당기

- **한 줄**: 프로그램은 운영체제에서 직접 조금씩 받지 않고, 할당기가 운영체제에서 크게 받아 둔 메모리를 정해 둔 크기 단계의 조각으로 나눠 받습니다. 그래서 30바이트를 요청하면 32바이트를 받는 식으로 올림이 생기고, 해제한 조각은 운영체제가 아니라 할당기로 돌아갑니다.
- **이 주제에서는**: Redis가 세는 숫자(`used_memory`), 할당기가 쥔 숫자, 운영체제가 보는 숫자가 서로 다른 이유가 이것입니다. 크기 단계, 내부·외부 단편화, 해제해도 운영체제에 바로 돌아가지 않는 이유는 [메모리 할당](../cs/memory-allocation.md)에 있습니다.

### RSS: 운영체제가 본 프로세스의 메모리

- **한 줄**: RSS(Resident Set Size)는 프로세스가 지금 실제 메모리(RAM)에 올려 둔 페이지의 합입니다. `top`, `ps`가 보여 주는 숫자이고, Redis에서는 `used_memory_rss`입니다.
- **비유**: 창고 관리인(할당기)이 건물주(운영체제)에게서 빌려 실제로 쓰고 있는 창고 면적입니다. 안에 짐이 얼마나 찼는지와 상관없이, 빌린 면적이 RSS입니다.
- **실제로는**: RSS에는 할당기가 쥔 메모리 말고도 프로그램 코드, 공유 라이브러리, 스택처럼 할당기를 거치지 않은 메모리가 들어갑니다. 운영체제는 메모리를 페이지(흔히 4KiB) 단위로 내주므로, 페이지 안에 쓰는 조각이 하나라도 남아 있으면 그 페이지 전체가 RSS에 남습니다. 반대로 스왑(메모리 부족 때 페이지를 디스크로 내보내는 것)으로 내보낸 페이지는 RSS에서 빠집니다.
- **이 주제에서는**: 컨테이너의 메모리 한도와 커널의 강제 종료가 보는 것은 Redis가 세는 숫자가 아니라 이쪽에 가까운 실제 메모리입니다. 그래서 Redis 안의 숫자만 보고 "아직 여유가 있다"고 판단하면 틀릴 수 있습니다.

### fork와 copy-on-write

- **한 줄**: `fork`는 실행 중인 프로세스를 복제해 자식 프로세스를 만들고, 두 프로세스는 처음에 같은 메모리 페이지를 함께 씁니다. 어느 쪽이 페이지에 쓰는 순간 그 페이지만 복사됩니다(copy-on-write).
- **이 주제에서는**: Redis는 세 작업에서 자식 프로세스를 만듭니다. RDB 저장, AOF 다시 쓰기(실행한 쓰기 명령을 덧붙여 온 AOF 파일이 커지면 현재 데이터만으로 새 기준 파일을 만드는 일), 복제본의 전체 동기화(복제본이 따라잡을 수 없을 때 주 서버가 데이터 전체를 한 시점으로 떠 보내는 일)입니다. 셋 다 요청을 계속 처리하면서 바뀌지 않는 한 시점의 데이터를 훑어야 해서 `fork`를 씁니다. 자식이 사는 동안 부모가 쓴 페이지만큼 실제 메모리가 늘지만, 이 증가분은 Redis가 할당한 것이 아니라서 `used_memory`에 나타나지 않습니다. 원리와 조건은 [fork와 copy-on-write](../cs/fork-and-copy-on-write.md)에 있습니다.

### 클라이언트 버퍼

- **한 줄**: Redis는 연결마다 받은 명령이 처리를 기다리는 입력 버퍼와, 만든 응답이 전송을 기다리는 출력 버퍼를 둡니다. 둘 다 Redis가 할당한 메모리입니다.
- **비유**: 창구마다 놓인 서류함입니다. 손님이 서류를 빨리 가져가면 비어 있지만, 손님이 자리를 비운 채 서류가 계속 오면 서류함이 창고 공간을 차지합니다.
- **실제로는**: 보통의 요청·응답 연결은 응답을 곧 가져가므로 출력 버퍼가 작게 유지됩니다. 문제는 클라이언트가 요청하지 않아도 서버가 데이터를 밀어 보내는 연결입니다. Pub/Sub 구독 연결과 복제본 연결이 그렇습니다. 받는 쪽이 느리면 보낼 데이터가 서버의 출력 버퍼에 쌓입니다. 큰 응답 하나(`HGETALL`로 필드 100만 개를 읽는 것처럼)도 다 나갈 때까지 출력 버퍼를 차지합니다.
- **이 주제에서는**: 클라이언트 버퍼는 데이터가 아니지만 `used_memory`에 들어가고, `maxmemory` 판단에도 대부분 들어갑니다. 그래서 키 데이터는 그대로인데 버퍼가 커져 키가 축출되는 일이 생깁니다. 연결과 응답 버퍼가 이벤트 루프에서 어떻게 쓰이는지는 [Redis 이벤트 루프](./event-loop.md)에 있습니다.

### 숫자 다섯 층

이 문서의 숫자는 다음 층 중 하나를 셉니다. 아래로 갈수록 바깥에서 본 크기입니다.

```text
① 데이터 자체 크기        키 이름 + 값 바이트의 합             (Redis에 지표 없음. 애플리케이션이 아는 크기)
② MEMORY USAGE key       키 하나의 객체·값 조각 크기 추정        (키 공간 표의 칸은 빠짐)
③ used_memory            Redis가 할당기에서 받은 조각의 합       (데이터 + 표 + 버퍼 + 스크립트 …)
④ allocator_active/      할당기가 쓰는 중인 페이지, 실제로 올린 페이지
   allocator_resident    (조각 사이 빈칸 포함)
⑤ used_memory_rss        운영체제가 본 Redis 프로세스의 실제 메모리 (할당기 밖의 코드·라이브러리 포함)
   ─────────────────
   컨테이너 한도         그 프로세스 묶음 전체의 실제 메모리   (fork한 자식의 copy-on-write 복사분도 여기에)
```

위아래는 포함 관계를 대략 나타낼 뿐, 숫자가 항상 아래로 갈수록 커진다는 뜻은 아닙니다. 예를 들어 스왑으로 페이지가 내보내지면 ⑤가 ③보다 작아질 수 있습니다. 각 층이 왜 갈리는지는 아래 절들에서 예시로 봅니다.

## 데이터 자체 크기와 Redis가 쓰는 메모리는 왜 다른가

예시의 312MB를 적재하면 `used_memory`는 312MB보다 큽니다. 키 하나가 Redis 안에서 무엇을 차지하는지 8.2.10 소스로 계산하면 다음과 같습니다. 측정이 아니라 소스와 jemalloc 크기 단계에서 끌어낸 계산입니다.

| 무엇 | 크기 | 왜 |
| --- | --- | --- |
| 키 객체 | 39바이트를 요청해 40바이트 조각 | Redis는 키 하나를 객체 하나로 만들고, 그 안에 객체 머리말 16바이트, 만료 시각 8바이트, 키 이름(길이 정보 포함 15바이트)을 함께 담습니다 |
| 값 | 306바이트를 요청해 320바이트 조각 | 300바이트 문자열에 길이 정보와 끝 표시를 붙인 것을 따로 할당합니다. 크기 단계가 256 다음 320이라 14바이트가 올림으로 버려집니다 |
| 키 공간 해시 표의 칸 | 키당 약 8바이트 | 키 이름으로 키 객체를 찾는 해시 표(Kotlin의 `HashMap`과 같은 구조)가 따로 있습니다. 키 이름에서 계산한 번호의 칸에 키 객체 자체가 아니라 그 객체의 주소(8바이트)를 둡니다. 표 크기는 2의 거듭제곱이라 100만 키에는 1,048,576칸이 잡힙니다. 서로 다른 키가 같은 칸에 배정되면(충돌) 칸 하나에 주소를 둘 둘 수 없으므로, 뒤에 온 키마다 16바이트 항목을 더 할당해 이어 붙입니다 |
| 만료 표의 칸 | 키당 약 8바이트 | TTL이 있는 키는 만료 시각을 찾기 위한 두 번째 해시 표에도 들어갑니다. 구조는 위와 같습니다 |

키 하나에 312바이트를 넣었는데, 객체와 값만 360바이트, 두 표의 칸까지 더하면 약 376바이트 이상입니다. 100만 개면 `used_memory`의 데이터 부분이 약 376MB 이상이 되고, 여기에 서버가 시작할 때 쓴 메모리와 버퍼가 더해집니다. 이 차이는 [메모리 할당](../cs/memory-allocation.md)의 "원소마다 따로 담으면, 원소 수만큼 비용이 붙습니다"와 같은 원리입니다. 키마다 객체 머리말, 주소, 크기 단계 올림이 붙으므로 **키가 작고 많을수록 데이터 자체 크기와의 비율 차이가 커집니다.** 값이 30바이트였다면 같은 고정 비용이 값보다 커집니다.

같은 데이터라도 Redis가 고르는 내부 표현에 따라 메모리가 달라집니다.

- 짧은 문자열은 값을 키 객체와 같은 조각에 함께 담아 조각 하나를 아낍니다. 예시의 300바이트는 이 조건에 들지 않습니다.
- 작은 Hash는 필드를 한 덩어리에 이어 담고, 기준을 넘으면 필드마다 따로 담는 표현으로 바뀝니다. 바뀐 뒤에는 필드마다 고정 비용이 붙어 메모리가 크게 늘 수 있고, 작아져도 돌아오지 않습니다([String과 Hash](./string-vs-hash.md)). 다른 자료형도 크기에 따라 표현이 바뀌지만, 바뀐 모양과 되돌아가는지는 자료형마다 다릅니다. 예를 들어 List는 커져도 원소를 따로 담지 않고 여러 덩어리를 잇는 구조가 되며, 다시 작아지면 한 덩어리로 돌아갈 수 있습니다.

그래서 메모리 용량은 "값 크기 × 개수"로 추정하지 않습니다. 대표 데이터를 실제로 넣어 보고 `used_memory`의 증가량을 키 수로 나눈 값(키당 메모리)으로 추정합니다.

## `INFO memory`의 숫자는 누가 본 크기인가

### `used_memory`: Redis가 세는 숫자

- **무엇을 세는가**: Redis가 할당기에서 받은 조각 크기의 합입니다. jemalloc에서는 요청한 크기가 아니라 크기 단계로 올린 크기를 더합니다. Redis가 할당하고 해제할 때마다 직접 더하고 빼므로, `INFO`를 부르는 순간의 값입니다.
- **무엇이 들어가는가**: 키와 값만이 아닙니다. 키 공간 해시 표, 클라이언트 버퍼, 복제 백로그와 복제 버퍼, AOF 버퍼, 스크립트 캐시, 그리고 8.2.10 소스에서는 Lua 스크립트 엔진이 쓰는 메모리도 들어갑니다(공식 `INFO` 문서는 Lua 엔진 메모리가 `used_memory`에 들지 않는다고 적지만, 8.2.10 소스에서는 Lua의 할당도 같은 집계를 거칩니다. "근거" 절 참고).
- **무엇이 빠지는가**: 할당기 안의 빈칸, 프로그램 코드와 라이브러리, fork한 자식 프로세스가 일으킨 copy-on-write 복사분입니다.

### `used_memory_peak`: 지금까지의 최고치

`used_memory`가 지금까지 가장 컸던 값과 그 시각(`used_memory_peak_time`)입니다. RSS의 최고치가 아니라 `used_memory`의 최고치입니다. 장면 2에서 50만 개를 지우면 `used_memory`는 줄어도 peak는 그대로 남습니다. 할당기는 최고치 때 받은 페이지를 대부분 쥐고 있으므로, **peak가 현재 `used_memory`보다 훨씬 크면 RSS가 큰 이유는 대개 과거의 최고치입니다.** `used_memory_peak_perc`는 현재 값이 최고치의 몇 %인지입니다.

### `used_memory_overhead`와 `used_memory_dataset`: 뺄셈으로 나눈 두 몫

- `used_memory_overhead`: 서버가 데이터를 관리하려고 쓰는 메모리의 합입니다. 시작 시 메모리(`used_memory_startup`), 복제 백로그와 복제 버퍼, 클라이언트 버퍼, AOF 버퍼, 스크립트 캐시와 엔진, 키 공간·만료 해시 표, 그리고 키마다 객체 머리말 16바이트를 더한 값입니다. 이 중 해시 표 몫은 실제 할당을 센 것이 아니라 **추정값**입니다.
- `used_memory_dataset`: `used_memory`에서 overhead를 뺀 나머지입니다. 따로 센 값이 아니라 **뺄셈 결과**입니다.

그래서 dataset은 "순수 데이터 바이트"와 정확히 같지 않습니다. overhead 쪽 추정이 실제 할당과 어긋난 만큼 dataset이 반대로 어긋납니다. 판단에 쓸 때는 정확한 값보다 **변화**를 봅니다. `used_memory_dataset_perc`(시작 시 메모리를 뺀 사용량 중 dataset의 비율)가 떨어졌을 때, `used_memory_overhead`의 절대량이 함께 늘었다면 데이터가 아닌 무언가(대개 버퍼)가 늘어난 것입니다. 데이터를 지워 dataset만 줄어도 비율은 떨어지므로 비율 하나로 판단하지 않습니다.

### `used_memory_rss`와 할당기 숫자: 바깥에서 본 크기

| 지표 | 무엇을 세는가 |
| --- | --- |
| `allocator_allocated` | 할당기가 내준 조각의 합. 보통 `used_memory`와 거의 같습니다 |
| `allocator_active` | 할당기가 쓰는 중인 페이지의 합. 조각 사이의 빈칸(외부 단편화)이 들어갑니다 |
| `allocator_resident` | 할당기가 실제 메모리에 올려 두었다고 집계한 페이지의 합. 비었지만 아직 운영체제에 돌려주지 않은 페이지와 할당기 자신의 관리 정보도 들어갑니다. jemalloc은 이 값을 정확한 측정이 아니라 상한에 가까운 집계로 설명합니다 |
| `used_memory_rss` | 운영체제가 본 Redis 프로세스의 RSS. 할당기 밖의 코드·라이브러리·스택이 더해집니다 |

이 숫자들은 명령마다가 아니라 서버의 주기 작업에서 약 100밀리초마다 다시 잽니다. `used_memory`와 RSS를 같은 순간에 잰 값으로 비율을 내야 하기 때문입니다.

이웃한 층끼리의 비율이 문제의 위치를 알려 줍니다.

| 지표 | `_ratio`는 나눗셈, `_bytes`는 뺄셈 | 크면 |
| --- | --- | --- |
| `allocator_frag_ratio`, `allocator_frag_bytes` | 8.2.10에서는 작은 크기 단계 조각들 사이의 빈칸 ÷ 할당량 + 1, 그리고 그 빈칸의 바이트(Lua 엔진 몫 제외) | 작은 조각 사이 빈칸이 많습니다(외부 단편화). 공식 문서가 "진짜 단편화 지표"로 부르는 것입니다. 1.1이면 할당량의 10%만큼 빈칸이 있다는 뜻입니다 |
| `allocator_rss_ratio`, `allocator_rss_bytes` | `allocator_resident` ÷ `allocator_active`, 그리고 그 차이 | 비었지만 아직 돌려주지 않은 페이지가 많습니다. 공식 문서는 곧 돌려줄 가능성이 큰 페이지라고 설명합니다 |
| `rss_overhead_ratio`, `rss_overhead_bytes` | `used_memory_rss` ÷ `allocator_resident`, 그리고 그 차이 | 할당기가 집계하지 않는 메모리(코드, 라이브러리, 스택 등)가 큽니다 |
| `mem_fragmentation_ratio`, `mem_fragmentation_bytes` | `used_memory_rss` ÷ `used_memory`, 그리고 그 차이 | 위 셋이 모두 섞인 값입니다 |

이 분해는 원인의 위치를 가리키는 어림입니다. `allocator_resident`가 상한에 가까운 집계라 세 층의 경계가 정확히 나뉘지는 않습니다.

`mem_fragmentation_ratio`는 이름과 달리 단편화만 재지 않습니다. 할당기 밖 메모리와 과거 최고치의 흔적까지 섞인 RSS 전체의 추가분입니다. 그래서 이 값이 크면 위 세 비율을 보고 어느 층이 큰지 나눕니다.

## `maxmemory`는 RSS가 아니라 `used_memory`로 판단합니다

`maxmemory`를 정하면 Redis는 명령을 실행하기 전마다 메모리가 한도를 넘었는지 확인합니다. 이때 비교하는 값은 다음과 같습니다.

```text
maxmemory와 비교하는 값 = used_memory − mem_not_counted_for_evict
mem_not_counted_for_evict ≈ 복제 버퍼 중 백로그 몫을 넘는 부분 + AOF 버퍼
```

여기서 "백로그 몫"은 설정한 백로그 크기에 그 버퍼 블록의 관리 비용 추정치를 더한 값이라, 아래 `mem_clients_slaves`와 정확히 같지는 않습니다. 등호 대신 ≈로 쓴 이유입니다.

빼는 버퍼는 축출로 키를 지우면 그 삭제가 다시 쌓이는 곳이라, 세면 지울수록 늘어나는 악순환이 생기기 때문에 뺍니다. 그 이유와 축출 동작은 [TTL과 축출](./ttl-and-eviction.md)의 "Redis가 세는 메모리: `used_memory`와 `maxmemory`"에서 다룹니다. 여기서 판단에 필요한 결론은 두 가지입니다.

- **RSS는 `maxmemory` 판단에 들어가지 않습니다.** 할당기 안의 빈칸, 코드·라이브러리, copy-on-write 복사분은 `maxmemory` 밖에 있습니다. `maxmemory`를 1GiB로 정해도 프로세스 묶음의 실제 메모리는 1GiB를 넘을 수 있습니다.
- **일반 클라이언트와 Pub/Sub 연결의 버퍼, 복제 백로그는 `maxmemory` 안에 있습니다.** 버퍼가 커지면 그만큼 키가 축출되거나, `noeviction`이면 쓰기가 OOM 에러를 받습니다.

공식 문서도 Redis가 사용자 키에 `maxmemory`만큼까지 할당한다고 쓰면서 "작은 추가 할당이 있을 수 있다"고 단서를 답니다(공식 문서 memory-optimization, "Memory allocation"). `maxmemory`는 Redis 프로세스 전체의 메모리 한도가 아니라 **축출을 시작하는 기준선**입니다.

## 지운 뒤에도 RSS가 줄지 않는 이유와 단편화 비율 읽기

### 장면 2: 50만 개를 지우면

단종 상품 50만 개를 지우면 다음처럼 움직입니다.

| 지표 | 움직임 | 이유 |
| --- | --- | --- |
| `used_memory` | 대략 절반으로 줄어듭니다 | Redis가 해제한 조각만큼 바로 뺍니다 |
| `used_memory_peak` | 그대로 | 최고치 기록입니다 |
| `used_memory_rss` | 거의 줄지 않을 수 있습니다 | 지운 키와 남은 키가 같은 페이지에 섞여 있으면 페이지를 돌려줄 수 없습니다 |
| `mem_fragmentation_ratio` | 크게 오릅니다(예: 1.1에서 2 가까이) | 분모 `used_memory`만 줄었습니다 |

이 비율 상승은 대개 문제가 아닙니다. 할당기는 빈칸을 다음 할당에 다시 씁니다. 공식 문서는 5GB 중 2GB를 지운 뒤 다시 2GB까지 채우는 동안 RSS가 더 늘지 않는다고 설명합니다(공식 문서 memory-optimization, "Memory allocation"). 상품이 다시 채워질 서버라면 그대로 둬도 됩니다. 문제가 되는 것은 **그 메모리를 다른 프로세스가 써야 하거나, 컨테이너 한도에 닿아 있는 경우**입니다.

### 비율이 1보다 클 때와 작을 때

- **1보다 조금 크다**: 정상입니다. 할당기 밖의 코드·라이브러리와 약간의 빈칸이 더해진 것입니다.
- **크지만 바이트 수가 작다**: 데이터가 거의 없는 서버에서는 시작 시 메모리와 코드가 비율을 키웁니다. 공식 `INFO` 문서는 `mem_fragmentation_bytes`가 수 MB로 작으면 1.5 이상의 비율도 문제의 신호가 아니라고 적습니다. **비율과 바이트 수를 함께 봅니다.**
- **크고 바이트 수도 크다**: `used_memory_peak`와 비교합니다. peak가 현재보다 훨씬 크면 과거 최고치의 흔적입니다. 그렇지 않으면 `allocator_frag_ratio`(할당기 안의 빈칸), `allocator_rss_ratio`(돌려줄 예정인 페이지), `rss_overhead_ratio`(할당기 밖) 중 무엇이 큰지 봅니다.
- **1보다 작다**: RSS가 Redis가 할당한 것보다 작다는 뜻이고, 공식 `INFO` 문서는 `used_memory`가 RSS보다 훨씬 큰 경우를 운영체제가 Redis 메모리 일부를 스왑으로 내보낸 신호로 설명합니다. 비율만으로 확정하지 말고 운영체제의 스왑 사용량을 확인합니다. 스왑된 페이지에 접근할 때마다 디스크를 읽으므로 명령 지연이 크게 늘 수 있습니다. 단편화보다 급한 문제입니다.

### 되찾는 방법과 그 대가

| 방법 | 무엇을 하는가 | 대가 |
| --- | --- | --- |
| 그대로 두기 | 빈칸을 다음 데이터가 재사용합니다 | 그동안 RSS가 높게 남습니다 |
| `MEMORY PURGE` | 할당기에 비어 있는 페이지를 운영체제에 돌려주라고 요청합니다 | 빈 페이지만 돌려줍니다. 쓰는 조각이 섞인 페이지는 그대로라, `allocator_rss_ratio` 쪽 낭비에는 효과가 있어도 `allocator_frag_ratio` 쪽 빈칸은 줄이지 못합니다 |
| `activedefrag yes` | 빈칸이 많은 페이지에 있는 조각을 새로 할당해 내용을 복사하고 옛 조각을 해제해, 페이지를 비웁니다 | 메인 스레드가 이벤트 루프의 타이머 작업으로 조금씩 하므로, 그 CPU 시간만큼 다른 명령과 시간을 나눠 씁니다. 함께 배포하는 jemalloc으로 빌드했을 때만 켤 수 있습니다 |
| 재시작(복제본 전환 포함) | 데이터를 다시 적재하므로, 그동안의 할당과 해제로 생긴 빈칸이 사라집니다. 크기 단계 올림 같은 기본 낭비는 남습니다 | 재시작 동안의 가용성, 적재 시간. 영속성 설정에 따라 데이터를 잃을 수 있습니다([영속성](./persistence.md)) |

active defrag의 기본값은 꺼짐입니다. 켜도 할당기 안의 빈칸이 `active-defrag-ignore-bytes`(기본 100MB) 미만이거나, 그 빈칸이 할당량의 `active-defrag-threshold-lower`(기본 10%) 미만이면 시작하지 않습니다. 자식 프로세스(RDB 저장 등)가 도는 동안에도 시작하지 않고, 진행 중이던 작업은 자식이 끝날 때까지 멈춥니다. 빈칸 비율이 클수록 CPU를 더 쓰는데, 기본값은 최소 1%(`active-defrag-cycle-min`)에서 최대 25%(`active-defrag-cycle-max`)입니다. 이 CPU는 명령을 처리하는 메인 스레드의 시간이므로, 지연에 민감한 서버라면 켠 뒤 명령 지연을 함께 봅니다. `redis.conf`도 단편화 문제가 없으면 켤 필요가 없다고 적습니다.

## 데이터 밖에서 늘어나는 메모리

### 장면 3: RDB 저장 중의 copy-on-write

RDB 저장이 시작되면 Redis는 `fork`하고, 자식이 데이터(키, 값, 만료 시각)를 훑어 파일로 씁니다. 자식이 사는 동안 부모가 쓰기를 처리하면 쓴 페이지가 복사됩니다.

- **copy-on-write 복사분은 `used_memory`에 더해지지 않습니다.** 복사는 커널이 페이지 단위로 하는 일이고 Redis가 할당한 것이 아니기 때문입니다. 따라서 `maxmemory` 판단에도 들어가지 않습니다. 저장 중에도 쓰기와 버퍼로 `used_memory`는 평소처럼 움직이지만, 그 움직임에 복사분은 없습니다.
- **부모의 RSS도 크게 바뀌지 않습니다.** 페이지 수는 같고, 바뀐 것은 부모와 자식이 더 이상 그 페이지를 공유하지 않는다는 점입니다. 두 프로세스의 RSS를 더하면 공유 페이지를 두 번 세므로, 합이 실제 사용량도 아닙니다.
- **늘어난 실제 메모리는 `INFO persistence`의 `current_cow_size`(진행 중), `rdb_last_cow_size`·`aof_last_cow_size`(마지막 저장 중 보고된 최고치)로 어림합니다.** Linux에서 이 값은 자식이 보고한 자기 전용 페이지의 합입니다. 대부분은 부모와 더 이상 공유하지 않게 된 페이지지만 자식이 새로 할당한 메모리도 섞이므로, 복사분과 정확히 같지는 않습니다.

컨테이너 한도는 자식 프로세스까지 포함한 묶음 전체를 세므로, 이 복사분은 컨테이너 한도 안에서 늘어납니다. 쓰기가 많고 흩어져 있으면 최악의 경우 데이터 전체에 가깝게 복사될 수 있습니다. 예시에서 `used_memory`가 900MB라면, `maxmemory`를 지키고 있어도 저장 중 실제 메모리가 1GiB 한도를 넘을 수 있습니다. 같은 일이 AOF 다시 쓰기와 복제본의 전체 동기화에서도 일어납니다([영속성](./persistence.md), 복제).

### 장면 4: 느린 구독자의 출력 버퍼

상품 변경 메시지를 받는 구독자 하나가 느려지면, Redis는 그 연결로 보낼 메시지를 출력 버퍼에 쌓습니다.

- 이 버퍼는 `used_memory`의 `mem_clients_normal`(일반 클라이언트와 Pub/Sub 연결의 합)에 들어가고, `maxmemory` 판단에도 들어갑니다. 버퍼가 커지면 키 데이터는 그대로인데 상품 캐시 키가 축출되기 시작합니다.
- 한도가 있습니다. 기본값 `client-output-buffer-limit pubsub 32mb 8mb 60`은 버퍼가 32MB에 닿거나, 8MB 이상인 상태가 60초를 넘겨 이어지면 그 연결을 끊습니다. 끊긴 횟수는 `INFO stats`의 `client_output_buffer_limit_disconnections`로 셉니다.
- 일반 요청·응답 연결은 기본 한도가 없습니다(`client-output-buffer-limit normal 0 0 0`). 큰 응답을 받는 연결이 많거나 클라이언트가 응답을 늦게 읽으면 그만큼 계속 쌓입니다.
- 일반·Pub/Sub 연결 전체의 메모리에 한도를 두려면 `maxmemory-clients`(기본 0, 꺼짐)를 씁니다. 복제본 연결은 대상이 아니고, `CLIENT NO-EVICT on`으로 표시한 연결도 빠집니다. 연결마다 버퍼뿐 아니라 연결 구조체, 받은 명령 인자, 트랜잭션 상태까지 세어 합이 한도를 넘으면 메모리를 가장 많이 쓰는 연결부터 끊습니다. 끊긴 수는 `INFO stats`의 `evicted_clients`입니다. 대가는 끊긴 클라이언트가 오류를 받고 다시 연결해야 한다는 것입니다.

어느 연결이 버퍼를 차지하는지는 `CLIENT LIST`의 `omem`(출력 버퍼)과 `tot-mem`(그 연결 전체)으로 찾습니다. `INFO clients`의 `client_recent_max_output_buffer`는 최근 가장 큰 출력 버퍼입니다.

### 복제 백로그와 복제 버퍼

- `mem_replication_backlog`: 복제본이 잠깐 끊겼다 돌아왔을 때 이어 보내려고 들고 있는 최근 흐름입니다. 크기는 `repl-backlog-size`(기본 1MB)로 정하고, 흐름이 쌓이면서 그 크기까지 차오릅니다. 이 몫은 `maxmemory` 안에서 셉니다. 표시되는 값에는 블록 색인의 관리 비용도 더해집니다.
- `mem_clients_slaves`: 복제본이 있을 때, 복제 흐름 버퍼 중 백로그 설정 크기를 넘는 부분입니다. 백로그와 복제본 연결은 버퍼 하나를 함께 쓰므로, 복제본이 따라오는 동안에는 0일 수 있습니다. 복제본 하나가 뒤처지면 이 값이 커지고, 이 부분은 `maxmemory` 판단에서 빠집니다.
- `mem_total_replication_buffers`: 복제 흐름 버퍼 전체입니다.

복제본이 뒤처질 때 이 버퍼가 커지는 과정과 한도(`client-output-buffer-limit replica`)는 복제의 "버퍼 한도를 넘으면 끊기고, 처음부터 다시 시작할 수 있습니다"에 있습니다. 여기서 기억할 것은, 이 부분이 `maxmemory`에서 빠지는 만큼 **`maxmemory`를 지키고 있어도 `used_memory`와 RSS는 그 위로 커질 수 있다**는 점입니다.

## `MEMORY` 명령과 `redis-cli` 스캔으로 알 수 있는 것과 없는 것

| 도구 | 알 수 있는 것 | 모르는 것 |
| --- | --- | --- |
| `MEMORY USAGE key` | 키 하나의 객체(키 이름 포함)와 값 조각 크기. 크기 단계로 올린 크기입니다 | 키 공간·만료 해시 표의 칸, 할당기 안의 빈칸. 원소마다 따로 담은 컬렉션은 기본 5개 표본으로 추정합니다 |
| `MEMORY STATS` | `INFO memory`의 overhead 항목을 나눠 보여 줌(DB마다 해시 표 크기, `keys.bytes-per-key` 등) | 어느 키가 큰지 |
| `MEMORY DOCTOR` | 정해 둔 조건에 걸린 문제를 문장으로 알려 줌 | 조건 밖의 문제. 원인 확정 |
| `redis-cli --bigkeys` / `--memkeys` | 원소 수 또는 `MEMORY USAGE` 기준으로 큰 키, 자료형별 합계·평균 | 접두사(키 종류)별 분포, 버퍼 같은 키 아닌 메모리 |

- **`MEMORY USAGE`**: 예시 키에 보내면 40 + 320 = 360을 돌려준다고 계산됩니다. 데이터 자체 크기 312바이트보다 크고, 키당 실제 몫(약 376바이트 이상)보다는 작습니다. 예시처럼 표본 추정이 없는 String이라면 `MEMORY USAGE`의 합은 `used_memory`의 데이터 부분보다 작게 나옵니다. 큰 컬렉션은 표본 평균으로 전체를 추정하므로 표본이 큰 원소에 걸리면 크게, 작은 원소에 걸리면 작게 빗나갈 수 있고, `SAMPLES 0`(전부 재기)은 원소 수에 비례하는 일을 메인 스레드에서 합니다.
- **`MEMORY STATS`**: `keys.bytes-per-key`는 dataset ÷ 키 수입니다. dataset이 뺄셈 결과이고 객체 머리말과 해시 표의 추정 비용이 overhead로 가 있으므로, 이 값으로 키당 메모리를 추정하면 실제보다 조금 작습니다. 용량 추정에는 `used_memory` 증가량을 키 수로 나누는 쪽이 낫습니다.
- **`MEMORY DOCTOR`**: 최고치와 현재 값의 차이, 단편화 관련 비율, 일반 클라이언트와 복제본의 버퍼, 캐시된 스크립트 수를 정해 둔 기준과 비교합니다. 메모리가 아주 작은 서버는 판단하지 않습니다. 조건에 걸리면 원인 후보와 대응(예: `MEMORY PURGE`, `activedefrag`, `CLIENT LIST`)을 문장으로 알려 줍니다. 판정 기준이 고정되어 있으므로 "문제 없음"은 이 조건들에 안 걸렸다는 뜻일 뿐입니다.
- **`--bigkeys` / `--memkeys`**: 자료형마다 가장 큰 키와 함께, 자료형별 키 개수·크기 합계·평균을 마지막에 요약합니다. `--memkeys`라면 예시의 작은 String 100만 개가 모인 크기도 String 합계로 보입니다. 다만 자료형으로만 묶으므로 한 번 돌려서 접두사별로 나눠 주지는 않고, 표의 칸이나 버퍼 같은 키 아닌 메모리는 보이지 않습니다. 접두사 하나의 개수와 크기는 `--pattern 'product:*'`처럼 접두사를 주면 그 키만 재므로 얻을 수 있습니다. 이때도 서버의 키 전체를 훑는 비용은 같습니다. 부하와 클러스터에서의 주의는 Hot Key와 Big Key의 "빅키: 키를 훑으며 크기를 잽니다"에 있습니다.

## 컨테이너와 관리형 환경에서 `maxmemory`를 어떻게 잡는가

컨테이너 한도와 `maxmemory`는 서로 다른 것을 셉니다.

- `maxmemory`는 Redis가 셀 수 있는 `used_memory`(일부 버퍼 제외)의 기준선입니다.
- 컨테이너 한도는 커널이 그 프로세스 묶음 전체의 실제 메모리에 거는 한도이고, 넘어서 줄일 수 없으면 묶음 안의 프로세스가 강제 종료됩니다([fork와 copy-on-write](../cs/fork-and-copy-on-write.md)의 "대가 3: 커널이 메모리를 얼마나 약속하느냐에 따라 fork가 실패하거나, 나중에 프로세스가 죽습니다").

Redis는 컨테이너 한도를 모릅니다. `INFO memory`의 `total_system_memory`는 Linux에서 시스템의 물리 메모리 크기를 읽은 값이라, 컨테이너 안에서는 호스트의 메모리를 보여 줄 수 있습니다. 그래서 다음을 더한 값이 컨테이너 한도 안에 들어가야 합니다.

```text
컨테이너 한도 ≥ maxmemory
             + maxmemory 판단에서 빠지는 버퍼(뒤처진 복제본의 버퍼, AOF 버퍼)
             + 할당기의 빈칸과 할당기 밖 메모리(RSS − used_memory)
             + fork 중 copy-on-write 복사분(RDB·AOF 다시 쓰기·전체 동기화를 쓰면)
```

이 식은 Redis 쪽에서 알 수 있는 항목만 모은 것이고 완전한 산식이 아닙니다. 컨테이너 한도는 커널이 그 묶음에 매기는 다른 메모리(예: 파일 캐시, 페이지 테이블)도 셀 수 있으므로, 마지막 확인은 컨테이너 메모리 지표의 실제 최고치로 합니다.

예시의 1GiB 컨테이너에 `maxmemory 1gb`를 주는 것은 위 항목에 여유를 전혀 남기지 않은 설정입니다. 평소에는 문제가 없다가, RDB 저장 중 쓰기가 몰리거나 복제본이 뒤처지는 순간 컨테이너가 강제 종료될 수 있습니다. 강제 종료는 OOM 에러를 받는 것과 달리 프로세스 자체가 사라지는 장애입니다.

몇 %로 잡으라는 고정값은 이 문서에서 제시하지 않습니다. 남길 몫이 쓰기량, 영속성 설정, 복제 구성, 데이터 모양에 따라 크게 달라지기 때문입니다. 대신 다음 순서로 정합니다.

1. 운영과 같은 쓰기 부하에서 RDB 저장이나 AOF 다시 쓰기를 일으키고 `current_cow_size`(또는 저장 뒤 `rdb_last_cow_size`)를 잽니다. 영속성과 복제를 모두 끈 순수 캐시라면 이 항목은 없습니다.
2. 평소의 `mem_fragmentation_bytes`(RSS − `used_memory`)를 봅니다. 큰 삭제가 반복되는 서비스라면 최고치 기준으로 봅니다.
3. 복제본이 뒤처질 때 늘 수 있는 양은 `client-output-buffer-limit replica` 한도(기본 hard 256MB)를 기준으로 어림합니다. 이 한도는 연결을 끊는 기준이지 메모리 상한이 아닙니다. 넘은 뒤에 끊고 버퍼를 정리하기까지 순간적으로 더 쓸 수 있으므로 여유를 둡니다.
4. 컨테이너 한도에서 이 셋을 빼고 남은 값보다 작게 `maxmemory`를 정합니다. 그 뒤 운영에서 컨테이너 메모리 지표의 최고치를 보고 여유가 실제로 남는지 확인하며, `used_memory_rss`와 함께 감시합니다.

관리형 서비스는 `maxmemory`를 서비스가 정하거나, 그 위에 예약 메모리 같은 별도 설정을 둡니다. 그 값과 의미는 서비스마다 다르므로 이 문서에서 다루지 않습니다. 확인할 것은 같습니다. 서비스가 보여 주는 "메모리 사용률"이 `used_memory` 기준인지 RSS 기준인지, fork에 쓸 여유를 누가 남겨 두는지입니다.

## 데이터와 요청이 늘면 무엇이 달라지는가

| 무엇이 늘면 | 어느 숫자가 커지는가 | 주의 |
| --- | --- | --- |
| 작은 키의 개수 | `used_memory`의 키당 고정 비용(객체 머리말, 표의 칸) | 데이터 자체 크기보다 훨씬 빨리 늡니다. 키당 메모리로 추정합니다 |
| 컬렉션 원소 수 | 내부 표현이 바뀌는 순간 키 하나의 메모리 | Hash처럼 한 번 바뀌면 작아져도 돌아오지 않는 자료형이 있습니다 |
| 크고 흩어진 삭제 | `mem_fragmentation_ratio`, `allocator_frag_ratio` | 다시 채울 데이터라면 재사용됩니다 |
| 쓰기량 | 저장 중 `current_cow_size`, 복제 버퍼 | `used_memory`에 안 보이는 몫이 커집니다 |
| 느린 구독자·큰 응답 | `mem_clients_normal` | `maxmemory` 안에서 키를 밀어냅니다 |
| `used_memory` 자체 | `fork` 시간, 저장 중 최악의 복사분 | 데이터가 커질수록 남겨야 할 여유도 커집니다 |

## 흔한 오해 모아 보기

| 오해 | 실제 |
| --- | --- |
| `used_memory`는 키와 값의 크기다 | 표, 클라이언트 버퍼, 복제 백로그, 스크립트까지 Redis가 할당한 거의 모든 것입니다 |
| 값 크기 × 키 수로 메모리를 추정할 수 있다 | 키마다 객체 머리말, 표의 칸, 크기 단계 올림이 붙습니다. 예시에서 312바이트가 약 376바이트 이상이 됩니다 |
| `maxmemory`를 정하면 Redis 프로세스가 그 이상 쓰지 않는다 | `used_memory`에서 일부 버퍼를 뺀 값의 축출 기준선입니다. RSS와 copy-on-write 복사분은 그 밖에 있습니다 |
| `mem_fragmentation_ratio`가 높으면 단편화가 심하다 | 과거 최고치, 할당기 밖 메모리, 작은 데이터셋이 모두 비율을 키웁니다. 바이트 수와 `allocator_frag_ratio`를 함께 봅니다 |
| 비율이 1보다 작으면 효율이 좋다 | 스왑의 신호일 수 있습니다. 운영체제의 스왑 사용량을 확인합니다 |
| 키를 지우면 RSS가 줄어든다 | 할당기가 빈칸으로 쥐고 있다가 재사용합니다 |
| RDB 저장 중 메모리 증가는 `used_memory`로 보인다 | copy-on-write 복사분은 `current_cow_size`로 봅니다 |
| `MEMORY USAGE`를 모두 더하면 `used_memory`가 된다 | 표의 칸, 버퍼, 빈칸이 빠지고, 큰 컬렉션은 추정값입니다 |

## 근거

**소스**(Redis 태그 [`8.2.10`](https://github.com/redis/redis/tree/8.2.10), 커밋 `86b20b1b9e5f03d86db72c3d59bec432e471fbc9`)

- [`src/server.c`](https://github.com/redis/redis/blob/8.2.10/src/server.c): `genRedisInfoString`의 Memory 절(`used_memory`는 `zmalloc_used_memory`, `used_memory_rss`·`allocator_*`는 `cron_malloc_stats`, `mem_not_counted_for_evict`는 `freeMemoryGetNotCountedMemory`, `mem_fragmentation_ratio` 위 주석의 "total RSS overhead, including fragmentation, but not just it", `total_system_memory`는 `server.system_memory_size`), Persistence 절의 `current_cow_size`·`rdb_last_cow_size`·`aof_last_cow_size`, Stats 절의 `client_output_buffer_limit_disconnections`·`evicted_clients`, Clients 절의 `client_recent_max_output_buffer`; `cronUpdateMemoryStats`(100밀리초마다 RSS와 `used_memory`를 같은 순간에 잼, 할당기 정보도 함께 갱신), `updatePeakMemory`(`used_memory`의 최고치와 시각); `initServer`(시작할 때 `rdb_last_cow_size`·`current_cow_size`의 바탕인 `stat_rdb_cow_bytes`·`stat_current_cow_bytes`를 0으로 둠. 재시작한 프로세스의 값은 이전 프로세스와 이어지지 않음)
- [`src/rdb.c`](https://github.com/redis/redis/blob/8.2.10/src/rdb.c)의 `rdbSaveBackground`("Background saving started by pid", 디스크로 하는 전체 동기화 포함)와 `rdbSaveToSlavesSockets`("Background RDB transfer started by pid"), [`src/aof.c`](https://github.com/redis/redis/blob/8.2.10/src/aof.c)의 `rewriteAppendOnlyFileBackground`("Background append only file rewriting started by pid"): 자식 프로세스를 만든 뒤 NOTICE 수준으로 로그에 남김. [`src/replication.c`](https://github.com/redis/redis/blob/8.2.10/src/replication.c)의 `startBgsaveForReplication`이 남기는 "Starting BGSAVE for SYNC with target"은 fork 전에 찍히는 시도 기록이라 자식이 실제로 만들어졌는지는 위 로그로 봄
- [`src/object.c`](https://github.com/redis/redis/blob/8.2.10/src/object.c): `getMemoryOverheadData`(overhead 항목의 합: 시작 시 메모리, 복제 백로그와 백로그를 넘는 복제 버퍼, 일반·Pub/Sub 클라이언트, AOF 버퍼, 스크립트 캐시와 엔진, DB마다 `kvstoreMemUsage`와 키 수 × `sizeof(robj)`; dataset은 `used_memory`에서 그 합을 뺀 값; `allocator_frag`는 작은 크기 단계의 빈칸 ÷ 할당량 + 1, `allocator_rss`·`rss_extra`·`total_frag`의 정의; `bytes_per_key`는 dataset ÷ 키 수), `getMemoryDoctorReport`(5MB 미만은 판단하지 않음, 150%, 1.4와 10MB, 1.1과 10MB, 200KB, 10MB, 스크립트 1,000개 조건과 문구), `kvobjComputeSize`(키 객체 조각에서 시작해 값 조각을 더함, 컬렉션은 표본 평균으로 추정, 키 공간 표의 칸은 더하지 않음), `OBJ_COMPUTE_SIZE_DEF_SAMPLES` 5, `memoryCommand`(`SAMPLES 0`은 전부, `PURGE`는 `jemalloc_purge`), `kvobjCreate`(객체 머리말, 만료 시각 8바이트, 키 길이 정보 1바이트와 키 sds를 한 조각에), `kvobjSet`(embstr 값은 합이 `CACHE_LINE_SIZE` 이하일 때만 키 객체에 함께 담음), `tryObjectEncodingEx`와 `OBJ_ENCODING_EMBSTR_SIZE_LIMIT` 44
- [`src/server.h`](https://github.com/redis/redis/blob/8.2.10/src/server.h): `kvobj` 배치 주석(객체 16바이트, 만료 시각 8바이트, 키 헤더 크기 1바이트, sds 키)
- [`src/config.h`](https://github.com/redis/redis/blob/8.2.10/src/config.h): `CACHE_LINE_SIZE`(Apple arm64 128, 그 밖 64)
- [`src/sds.c`](https://github.com/redis/redis/blob/8.2.10/src/sds.c): `sdsReqType`(31바이트 이하 type 5, 그다음 type 8, 65,535바이트 근처까지 type 16). 예시의 키 12바이트는 type 5(머리 1바이트), 값 300바이트는 type 16(머리 5바이트)
- [`src/networking.c`](https://github.com/redis/redis/blob/8.2.10/src/networking.c): `sdsZmallocSize`(sds 조각의 실제 할당 크기), `checkClientOutputBufferLimits`(hard·soft 한도를 `>=`로 비교, soft는 지속 시간이 설정 초를 넘을 때), `closeClientOnOutputBufferLimitReached`(한도에 닿은 연결을 비동기로 닫음), `getClientMemoryUsage`(버퍼, 연결 구조체, 명령 인자, 트랜잭션 상태를 셈), `evictClients`(`maxmemory-clients`를 넘으면 많이 쓰는 연결부터 끊음), `CLIENT LIST`의 `omem`·`tot-mem`
- [`src/server.c`](https://github.com/redis/redis/blob/8.2.10/src/server.c)의 `clientEvictionAllowed`: `maxmemory-clients` 대상은 일반·Pub/Sub 연결이고 `CLIENT NO-EVICT` 연결은 제외
- [`src/kvstore.c`](https://github.com/redis/redis/blob/8.2.10/src/kvstore.c)의 `kvstoreMemUsage`와 [`src/dict.c`](https://github.com/redis/redis/blob/8.2.10/src/dict.c)의 `dictEntryMemUsage`: overhead의 해시 표 몫은 표에 든 키 수 × 항목 크기 + 칸 수 × 8바이트로 추정(실제로 항목을 할당했는지와 무관)
- [`src/t_list.c`](https://github.com/redis/redis/blob/8.2.10/src/t_list.c): `listTypeTryConversionRaw`와 `LIST_CONV_SHRINKING`(List가 줄면 quicklist에서 listpack으로 되돌아갈 수 있음)
- [`src/t_hash.c`](https://github.com/redis/redis/blob/8.2.10/src/t_hash.c): `hashTypeDelete`(hashtable에서는 필드만 지우고 표현을 바꾸지 않음), `hashTypeConvert`(hashtable에서 다른 표현으로 바꾸는 경로는 "Not implemented")
- [`src/redis-cli.c`](https://github.com/redis/redis/blob/8.2.10/src/redis-cli.c): `findBigKeys`(자료형별 가장 큰 키와 키 수·합계·평균 요약, `--memkeys`면 바이트), `parseOptions`·`sendScan`(`--pattern`을 `SCAN … MATCH`로 넘겨 맞는 키만 잼)
- [`src/server.c`](https://github.com/redis/redis/blob/8.2.10/src/server.c)의 `dbDictType`·`dbExpiresDictType`(`no_value = 1`, 키 객체를 칸에 바로 둠)와 [`src/dict.c`](https://github.com/redis/redis/blob/8.2.10/src/dict.c)의 `dictInsertKeyAtLink`(빈 칸이면 항목 할당 없이 칸에 주소를 두고, 차 있으면 값 없는 항목을 할당), `_dictExpandIfNeeded`(키 수가 표 크기에 닿으면 2의 거듭제곱으로 확장)
- [`src/evict.c`](https://github.com/redis/redis/blob/8.2.10/src/evict.c): `getMaxmemoryState`(`zmalloc_used_memory`에서 `freeMemoryGetNotCountedMemory`를 빼서 `maxmemory`와 비교), `freeMemoryGetNotCountedMemory`(복제 버퍼 중 백로그 크기와 그 블록 관리 비용 추정치를 넘는 부분, AOF 버퍼)
- [`src/zmalloc.c`](https://github.com/redis/redis/blob/8.2.10/src/zmalloc.c): `zmalloc_get_rss`(Linux에서 `/proc/self/stat`의 RSS 페이지 수 × 페이지 크기), `zmalloc_get_memory_size`(`sysconf(_SC_PHYS_PAGES)` × 페이지 크기), `zmalloc_with_flags`·`zfree_with_flags`(이 경로의 할당도 `used_memory` 집계에 더하고 뺌), `zmalloc_get_private_dirty`(smaps의 `Private_Dirty`)
- [`src/script.c`](https://github.com/redis/redis/blob/8.2.10/src/script.c): `luaAlloc`(jemalloc이면 Lua 할당이 전용 arena로 `zrealloc_with_flags`·`zfree_with_flags`를 거침). 그래서 8.2.10에서 Lua 엔진 메모리는 `used_memory`에 들어가며, `getMemoryOverheadData`도 `script_vm`을 overhead에 더합니다. 공식 `INFO` 문서의 `used_memory_vm_eval` 설명("not part of used_memory")은 이 소스와 다르므로 소스를 따랐습니다
- [`src/childinfo.c`](https://github.com/redis/redis/blob/8.2.10/src/childinfo.c): `sendChildInfoGeneric`(자식이 자신의 `Private_Dirty`를 copy-on-write 크기로 보고하고 최고치를 기록), `updateChildInfo`
- [`src/defrag.c`](https://github.com/redis/redis/blob/8.2.10/src/defrag.c): `getAllocatorFragmentation`(작은 크기 단계의 빈칸 비율과 바이트), `computeDefragCycles`(하한 비율·무시 바이트 미만이면 시작하지 않고, 빈칸 비율에 따라 CPU 비율을 보간), `activeDefragTimeProc`를 `aeCreateTimeEvent`로 메인 이벤트 루프에 등록, 자식 프로세스가 있으면 시작하지 않고(`hasActiveChildProcess`) 진행 중이면 자식이 끝날 때까지 멈춤
- [`src/config.c`](https://github.com/redis/redis/blob/8.2.10/src/config.c): `activedefrag` 기본 no와 `isValidActiveDefrag`, `active-defrag-ignore-bytes` 100MB, `active-defrag-threshold-lower` 10, `active-defrag-threshold-upper` 100, `active-defrag-cycle-min` 1, `active-defrag-cycle-max` 25, `maxmemory-clients` 기본 0
- [`redis.conf`](https://github.com/redis/redis/blob/8.2.10/redis.conf): CLIENTS 절(`client-output-buffer-limit normal 0 0 0`, `replica 256mb 64mb 60`, `pubsub 32mb 8mb 60`, hard·soft 한도의 뜻, `maxmemory-clients`가 연결 메모리 합의 한도이며 가장 많이 쓰는 연결부터 끊음), ACTIVE DEFRAGMENTATION 절(기본 꺼짐, 함께 배포하는 jemalloc 필요, 단편화 문제가 없으면 켤 필요 없음)
- [`deps/jemalloc/doc/jemalloc.xml.in`](https://github.com/redis/redis/blob/8.2.10/deps/jemalloc/doc/jemalloc.xml.in)의 `stats.resident`: 할당기 관리 정보, 쓰는 조각의 페이지, 비었지만 돌려주지 않은 페이지를 포함하며, 아직 건드리지 않은 페이지가 섞일 수 있어 정확한 값이 아니라 최대값이라고 설명. `zmalloc_get_allocator_info`가 이 값을 `allocator_resident`로 읽음
- [`deps/jemalloc/src/sc.c`](https://github.com/redis/redis/blob/8.2.10/deps/jemalloc/src/sc.c)와 [`deps/Makefile`](https://github.com/redis/redis/blob/8.2.10/deps/Makefile)(`--with-lg-quantum=3`): 8바이트 간격의 작은 크기 단계(…, 32, 40, …, 256, 320, …). 39바이트는 40, 306바이트는 320 조각

**공식 문서**(redis/docs 커밋 `e53775dde0ee7e39ebe327e7ffff583caa9fbd65`. 특정 Redis 버전 전용 문서가 아니므로 소스와 다르면 소스를 따릅니다)

- [info](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/info.md): memory 절 필드의 뜻(`used_memory_rss`는 `top`·`ps`가 보여 주는 값, `mem_fragmentation_ratio`에 코드·라이브러리·스택이 섞임, `mem_fragmentation_bytes`가 수 MB면 1.5 이상의 비율도 문제의 신호가 아님, `allocator_frag_ratio`가 진짜 외부 단편화 지표, `allocator_rss_ratio`는 곧 돌려줄 페이지, `allocator_muzzy`, `mem_not_counted_for_evict`, `mem_clients_slaves`가 0일 수 있음), used가 RSS보다 크면 스왑, 해제한 메모리가 운영체제에 돌아가지 않을 수 있어 `used_memory_peak`로 확인
- [memory-optimization](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/operate/oss_and_stack/management/optimization/memory-optimization.md), "Memory allocation": `maxmemory`까지 할당하되 작은 추가 할당이 있을 수 있음, 5GB 중 2GB를 지워도 RSS는 5GB 근처, 최고치 기준으로 준비, 빈칸 재사용, 최고치가 크면 단편화 비율을 믿을 수 없음
- [memory-usage](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/memory-usage.md): 키와 값, 관리 비용을 포함한 바이트, 중첩 자료형은 표본 평균(기본 5, `SAMPLES 0`은 전부)
- [memory-stats](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/memory-stats.md): 각 항목의 뜻, `dataset.bytes`는 `total.allocated`에서 `overhead.total`을 뺀 값, `keys.bytes-per-key`
- [memory-doctor](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/memory-doctor.md), [memory-purge](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/memory-purge.md): 명령의 용도

## 실무 포인트

### 무엇을 미리 정할까

| 중요도 | 항목 | 이유와 선택 |
| --- | --- | --- |
| 필수 | 컨테이너·VM 한도와 `maxmemory` 사이에 여유 남기기 | `maxmemory`는 `used_memory` 기준이라 copy-on-write, 단편화, 일부 복제 버퍼가 그 밖에 있습니다. 여유가 없으면 OOM 에러가 아니라 프로세스 강제 종료로 끝납니다 |
| 필수 | 용량은 "값 크기 × 개수"가 아니라 대표 데이터로 잰 키당 메모리로 추정 | 작은 키는 고정 비용이 값보다 클 수 있습니다 |
| 필수 | 메모리 경보를 `used_memory`/`maxmemory`와 컨테이너 메모리 사용량/컨테이너 한도 두 축으로 걸기 | 앞의 것은 축출·OOM 에러를, 뒤의 것은 강제 종료를 예고합니다. 한쪽만 보면 다른 쪽 장애를 놓칩니다. 저장 중 copy-on-write 복사분은 부모의 `used_memory_rss`에 거의 드러나지 않으므로, RSS 경보만으로는 컨테이너 강제 종료를 대신 예고할 수 없습니다 |
| 알면 유리 | Pub/Sub 출력 버퍼 한도와 `maxmemory-clients` 검토 | 느린 구독자의 버퍼가 `maxmemory` 안에서 캐시 키를 밀어냅니다. 한도를 두면 대신 연결이 끊깁니다 |
| 알면 유리 | 영속성·복제를 켠 서버는 저장 중 `current_cow_size`를 운영 부하에서 한 번 재 두기 | 남겨야 할 여유의 가장 큰 항목이 이것인 경우가 많고, 환경과 쓰기 패턴 없이는 추정할 수 없습니다 |
| 참고 | `activedefrag` | 단편화가 실제 문제(컨테이너 한도 압박 등)일 때만 켭니다. 메인 스레드 CPU를 씁니다 |

### 개발자가 정할 수 있는 것과 관찰해야 하는 것

| 정할 수 있는 것 | 관찰해야 하는 것 |
| --- | --- |
| 키 이름 길이, 값 직렬화 형식, 작은 객체를 키마다 둘지 Hash로 묶을지 | `used_memory` 증가량 ÷ 키 수(키당 메모리) |
| `maxmemory`와 컨테이너 한도의 차이 | `used_memory_rss`, 컨테이너 메모리 지표, `mem_fragmentation_bytes` |
| 영속성 방식과 저장 시점 | `current_cow_size`, `rdb_last_cow_size` |
| 출력 버퍼 한도, `maxmemory-clients` | `mem_clients_normal`, `CLIENT LIST`의 `omem`, `client_output_buffer_limit_disconnections` |
| `repl-backlog-size` | `mem_replication_backlog`, `mem_clients_slaves` |
| 큰 삭제를 몰아서 할지 나눠서 할지 | `used_memory_peak`, `allocator_frag_ratio` |

### 증상과 확인 순서

**증상 1: "메모리 사용률 경보가 울렸다."**

1. 그 경보가 어느 숫자 기준인지 먼저 확인합니다. `used_memory`/`maxmemory`인지, RSS나 컨테이너 메모리인지에 따라 아래 순서가 갈립니다.
2. `used_memory` 기준이면 `used_memory_dataset`과 `used_memory_overhead` 중 무엇이 늘었는지 봅니다. dataset이 늘었으면 데이터 증가(키 수, 큰 키, 인코딩 전환)이고, overhead가 늘었으면 `mem_clients_normal`, `mem_clients_slaves`, `mem_aof_buffer`, 스크립트 중 무엇인지 봅니다.
3. RSS·컨테이너 기준이면 `mem_fragmentation_bytes`와 peak를 보고, 영속성이 켜져 있으면 지금 자식 프로세스가 도는지(`INFO persistence`의 `rdb_bgsave_in_progress`, `aof_rewrite_in_progress`)와 `current_cow_size`를 봅니다.

**증상 2: "키 수는 그대로인데 축출이 늘었다."**

1. 증상 1처럼 `used_memory_dataset`과 `used_memory_overhead` 중 무엇이 늘었는지 먼저 봅니다. 키 수가 같아도 같은 키의 값이나 컬렉션이 커지면 dataset이 늡니다.
2. overhead가 늘었다면 `mem_clients_normal`이 늘었는지 봅니다. 늘었다면 `CLIENT LIST`에서 `omem`이 큰 연결을 찾습니다. 구독 연결이면 느린 구독자, 일반 연결이면 큰 응답이나 응답을 늦게 읽는 클라이언트입니다.
3. 대응: 구독자를 고치거나 출력 버퍼 한도·`maxmemory-clients`로 연결을 끊게 합니다. 끊으면 그 연결의 메시지를 잃으므로, Pub/Sub이 최대 한 번 전달이라는 전제가 서비스에 맞는지 함께 봅니다.

**증상 3: "많이 지웠는데 RSS가 그대로다."**

1. `used_memory`가 줄었는지 봅니다. 줄었다면 Redis는 해제를 마쳤습니다.
2. 다시 채울 데이터라면 그대로 둡니다. 빈칸이 재사용됩니다.
3. 메모리를 돌려받아야 한다면 `allocator_rss_ratio`가 크면 `MEMORY PURGE`를, `allocator_frag_ratio`와 그 바이트가 크면 `activedefrag`를 검토합니다. 급하면 복제본을 주 서버로 전환하고 옛 주 서버를 다시 시작하는 방법도 있지만, 장애 조치의 데이터 손실 조건을 함께 봐야 합니다(복제).

**증상 4: "Redis가 `maxmemory` 아래였는데 컨테이너가 OOM으로 재시작됐다."**

재시작된 Redis는 새 프로세스라 `INFO`의 `rdb_last_cow_size`, `current_cow_size` 같은 값이 처음부터 다시 셉니다. 사고 직전의 값은 지금의 `INFO`가 아니라 미리 수집해 둔 지표 시계열과 사고 전 Redis 로그에서 봅니다. 그래서 `INFO memory`와 `INFO persistence`의 값을 평소에 시계열로 남겨 두어야 이 확인이 가능합니다.

1. 컨테이너가 메모리 한도 때문에 종료됐는지 컨테이너 플랫폼이 남긴 종료 사유로 확인합니다.
2. 종료 직전에 RDB 저장, AOF 다시 쓰기, 복제본 전체 동기화가 시작됐는지 Redis 로그로 보고, 수집해 둔 `current_cow_size`, `mem_fragmentation_bytes`, `mem_clients_slaves`, 컨테이너 메모리 사용량의 흐름을 봅니다.
3. 대응: `maxmemory`를 낮추거나 컨테이너 한도를 올려 위 항목의 여유를 만듭니다. `maxmemory`를 낮추면 캐시 적중률이 떨어지거나(축출 정책이 있을 때) 쓰기가 더 일찍 OOM 에러를 받습니다(`noeviction`일 때). 저장을 쓰기가 적은 시간으로 옮기는 것도 복사분을 줄입니다.

**증상 5: "`mem_fragmentation_ratio`가 1보다 작다."**

1. 스왑을 의심합니다. 운영체제의 스왑 사용량과 Redis 명령 지연을 봅니다.
2. 대응: 메모리를 늘리거나 데이터를 줄입니다. 단편화 정리와는 반대 방향의 문제입니다.

### Spring에서 확인할 것

- **직렬화 형식이 키당 메모리를 정합니다.** `GenericJackson2JsonRedisSerializer`처럼 타입 정보를 값에 함께 넣는 직렬화기는 값 바이트를 늘리고, 그만큼 크기 단계가 올라갈 수 있습니다. 직렬화 형식의 차이는 [직렬화](./serializer.md)에서 다룹니다. 용량을 추정할 때는 애플리케이션이 실제로 보내는 바이트로 잽니다.
- **캐시 키 접두사도 키마다 반복되는 바이트입니다.** Spring Data Redis 캐시는 기본으로 키 앞에 `캐시이름::`을 붙입니다([Spring `@Cacheable`의 함정](./spring-cacheable.md)). 키가 작고 많을수록 이 반복이 데이터 자체 크기에서 차지하는 비율이 커집니다.

## 말로 설명하기

### 30초 버전

Redis 메모리 숫자는 누가 본 크기인지가 다릅니다. `used_memory`는 Redis가 할당기에서 받은 조각의 합이라 데이터뿐 아니라 해시 표, 클라이언트 버퍼, 복제 백로그까지 들어가고, RSS는 운영체제가 본 실제 메모리라 할당기 안의 빈칸과 코드까지 들어갑니다. `maxmemory`는 RSS가 아니라 `used_memory`에서 일부 버퍼를 뺀 값으로 판단하므로, 컨테이너 한도와 `maxmemory` 사이에는 copy-on-write와 단편화를 위한 여유를 남겨야 합니다.

### 2분 버전

키 이름 12바이트, 값 300바이트인 키 100만 개는 데이터만 312MB지만 Redis에서는 그보다 큽니다. 키마다 객체 머리말과 만료 시각을 담은 40바이트 조각, 크기 단계로 올린 320바이트 값 조각, 그리고 키 공간과 만료 해시 표의 칸이 붙어 키당 약 376바이트 이상이 됩니다. 그래서 용량은 값 크기 × 개수가 아니라 실제로 넣어 본 키당 메모리로 추정합니다.

`INFO memory`의 숫자는 층이 다릅니다. `used_memory`는 Redis가 직접 더하고 빼는 할당 합이고, overhead와 dataset은 그것을 뺄셈으로 나눈 몫입니다. `allocator_*`는 할당기가 쥔 페이지, `used_memory_rss`는 운영체제가 본 프로세스 메모리입니다. 키를 지우면 `used_memory`는 바로 줄지만 RSS는 남을 수 있어 `mem_fragmentation_ratio`가 뜁니다. 이 비율은 과거 최고치, 할당기 밖 메모리까지 섞인 값이라, 바이트 수와 `allocator_frag_ratio`, peak를 함께 봐야 원인이 갈립니다. 1보다 작으면 스왑을 의심합니다.

`maxmemory`는 `used_memory`에서 백로그를 넘는 복제 버퍼와 AOF 버퍼를 뺀 값으로 판단합니다. 그래서 느린 Pub/Sub 구독자의 출력 버퍼는 `maxmemory` 안에서 키를 밀어내고, 반대로 fork 중 copy-on-write 복사분과 단편화는 `maxmemory` 밖에서 컨테이너 한도를 압박합니다. 앞의 것은 축출이나 OOM 에러로, 뒤의 것은 프로세스 강제 종료로 드러납니다. 그래서 경보는 두 축으로 걸고, 컨테이너 한도는 운영 부하에서 잰 copy-on-write와 단편화를 더해 잡습니다.

### 예상 질문과 꼬리 질문

**Q. `used_memory`와 `used_memory_rss`는 왜 다른가요?**
`used_memory`는 Redis가 할당기에서 받은 조각의 합이고, RSS는 운영체제가 본 프로세스의 실제 메모리입니다. RSS에는 할당기가 쥐고 있지만 Redis가 쓰지 않는 빈칸, 아직 운영체제에 돌려주지 않은 페이지, 할당기를 거치지 않는 코드와 라이브러리가 더해집니다. 반대로 스왑된 페이지는 RSS에서 빠집니다. (본문: "`INFO memory`의 숫자는 누가 본 크기인가")

- **꼬리: 그럼 둘 중 무엇이 "진짜" 사용량인가요?**
  질문에 따라 다릅니다. 축출과 OOM 에러를 예측하려면 `used_memory`(정확히는 일부 버퍼를 뺀 값)를, 컨테이너 강제 종료를 예측하려면 RSS와 컨테이너 메모리를 봅니다. 하나로 둘을 대신할 수 없습니다.
- **꼬리: 두 숫자를 같은 순간에 잰 건가요?**
  RSS와 할당기 숫자는 주기 작업에서 약 100밀리초마다 잽니다. `mem_fragmentation_ratio`는 그때 함께 잰 `used_memory`로 계산하지만, `INFO`의 `used_memory` 줄은 부르는 순간의 값이라 두 줄로 직접 나누면 비율이 약간 어긋날 수 있습니다.

**Q. `maxmemory`를 1GB로 정했는데 왜 컨테이너가 1GiB 한도로 죽나요?**
`maxmemory`는 `used_memory`에서 일부 버퍼를 뺀 값의 축출 기준선이지 프로세스 메모리 한도가 아니기 때문입니다. 할당기 안의 빈칸과 코드는 RSS에만 있고, fork한 자식이 사는 동안의 copy-on-write 복사분은 `used_memory`에도 부모의 RSS에도 잘 드러나지 않지만 컨테이너 한도에는 들어갑니다. 백로그를 넘는 복제 버퍼는 `used_memory`에 있지만 `maxmemory` 판단에서 빠집니다. (본문: "`maxmemory`는 RSS가 아니라 `used_memory`로 판단합니다", "컨테이너와 관리형 환경에서 `maxmemory`를 어떻게 잡는가")

- **꼬리: 그럼 `maxmemory`를 컨테이너의 몇 %로 잡으면 되나요?**
  고정 비율은 없습니다. 영속성과 복제를 끈 캐시라면 copy-on-write 몫이 없고, 쓰기가 많은 데이터 서버라면 최악의 경우 데이터 전체에 가까운 복사분이 필요합니다. 운영 부하에서 저장 중 `current_cow_size`와 평소 `mem_fragmentation_bytes`를 재고, 복제본 버퍼 한도를 더해 남은 값으로 정한 뒤, 컨테이너 메모리 지표의 실제 최고치로 확인합니다.
- **꼬리: Redis가 컨테이너 한도를 알아서 맞추지 않나요?**
  모릅니다. `total_system_memory`도 Linux에서 시스템 물리 메모리를 읽은 값이라 컨테이너 안에서는 호스트 메모리를 보여 줄 수 있습니다.
- **꼬리: 여유를 많이 남기면 대가는 무엇인가요?**
  같은 컨테이너에 담을 수 있는 데이터가 줄어 캐시 적중률이 떨어지거나 비용이 늡니다. 대신 강제 종료라는 더 큰 장애를 피합니다. 영속성을 쓰지 않는 캐시라면 복사분 몫을 줄일 수 있다는 점에서, 서버의 역할을 나누는 것이 여유를 줄이는 방법이 되기도 합니다.

**Q. `mem_fragmentation_ratio`가 2입니다. 단편화가 심한 건가요?**
그것만으로는 알 수 없습니다. 이 값은 RSS ÷ `used_memory`라 할당기 안의 빈칸뿐 아니라 과거 최고치 때 받은 페이지, 할당기 밖 메모리가 모두 섞입니다. 먼저 `mem_fragmentation_bytes`가 의미 있는 크기인지, `used_memory_peak`가 현재보다 훨씬 큰지 봅니다. 그다음 `allocator_frag_ratio`, `allocator_rss_ratio`, `rss_overhead_ratio` 중 무엇이 큰지로 층을 나눕니다. (본문: "비율이 1보다 클 때와 작을 때")

- **꼬리: 데이터를 많이 지운 직후라면요?**
  분모만 줄어 비율이 뛴 것일 가능성이 큽니다. 다시 채울 데이터라면 빈칸이 재사용되므로 대응하지 않아도 됩니다.
- **꼬리: 1보다 작으면요?**
  Redis가 할당한 것보다 RSS가 작다는 뜻이라 스왑을 의심하고 운영체제의 스왑 사용량을 확인합니다. 스왑이 맞다면 지연이 크게 늘 수 있어 단편화보다 급합니다.
- **꼬리: 데이터가 거의 없는 서버에서 비율이 3이면요?**
  시작 시 메모리와 코드가 분모에 비해 커서 그렇습니다. 공식 문서도 바이트 수가 수 MB면 높은 비율이 문제의 신호가 아니라고 적습니다.

**Q. 단편화를 줄이려면 어떻게 하나요? 각각의 대가는요?**
빈 페이지가 많으면(`allocator_rss_ratio`) `MEMORY PURGE`로 돌려줄 수 있지만, 쓰는 조각이 섞인 페이지의 빈칸은 못 줄입니다. 그 빈칸은 `activedefrag`가 조각을 새로 할당해 복사하고 옛 조각을 해제해 줄입니다. 이 작업은 메인 스레드가 타이머 작업으로 조금씩 하므로 그만큼 명령 처리 시간을 나눠 씁니다. 재시작은 다시 적재하며 그동안 쌓인 빈칸을 없애지만 가용성과 영속성 조건을 함께 따져야 합니다. (본문: "되찾는 방법과 그 대가")

- **꼬리: active defrag를 기본으로 켜 두면 안 되나요?**
  켜도 할당기 안의 빈칸이 100MB 미만이거나 10% 미만이면 시작하지 않으므로 평소 부담은 작습니다. 다만 시작하면 메인 스레드 CPU를 최대 25%까지 쓰도록 되어 있어, 지연에 민감한 서버에서는 단편화가 실제로 문제일 때 켜고 지연을 함께 보는 편이 안전합니다. `redis.conf`도 문제가 없으면 켤 필요가 없다고 적습니다.

**Q. `MEMORY USAGE`로 키마다 재서 더하면 전체 메모리가 나오나요?**
아닙니다. `MEMORY USAGE`는 키 객체와 값 조각만 세고 키 공간·만료 해시 표의 칸, 클라이언트 버퍼, 할당기의 빈칸은 세지 않습니다. 원소를 따로 담은 큰 컬렉션은 기본 5개 표본으로 추정합니다. 예시 키는 360으로 계산되지만 키당 실제 몫은 표의 칸까지 약 376바이트 이상입니다. (본문: "`MEMORY` 명령과 `redis-cli` 스캔으로 알 수 있는 것과 없는 것")

- **꼬리: 그럼 키 종류별 메모리는 어떻게 아나요?**
  `SCAN`으로 접두사별 표본을 떠서 `MEMORY USAGE`를 재고 개수를 곱하는 식으로 추정합니다. 표의 칸은 빠지므로 String처럼 표본 추정이 없는 키는 결과가 조금 작게 나옵니다. 더 정확한 키당 메모리는 대표 데이터를 넣어 `used_memory` 증가량으로 잽니다.
- **꼬리: `--bigkeys`가 아무것도 안 잡는데 메모리가 큽니다.**
  작은 키가 많거나 키가 아닌 메모리(버퍼, 백로그)일 수 있습니다. `used_memory_dataset`과 overhead를 먼저 나눠 봅니다.

**Q. 키 수는 그대로인데 축출이 시작됐습니다. 왜인가요?**
키 수가 같아도 값이 커졌을 수 있으므로 먼저 dataset과 overhead 중 무엇이 늘었는지 나눕니다. overhead가 늘었다면 데이터가 아닌 메모리가 늘어난 것입니다. 일반 클라이언트와 Pub/Sub 연결의 출력 버퍼는 `used_memory`에 들어가고 `maxmemory` 판단에서도 빠지지 않습니다. 느린 구독자나 큰 응답이 쌓이면 그만큼 키가 축출됩니다. `mem_clients_normal`과 `CLIENT LIST`의 `omem`으로 확인합니다. (본문: "장면 4: 느린 구독자의 출력 버퍼")

- **꼬리: 출력 버퍼 한도를 낮추면 해결되나요?**
  메모리는 지키지만 그 연결이 끊기고 버퍼에 있던 메시지를 잃습니다. Pub/Sub은 원래 끊긴 동안의 메시지를 보존하지 않으므로, 메시지를 잃어도 되는 용도인지가 먼저입니다. 일반·Pub/Sub 연결 전체의 상한은 `maxmemory-clients`로 둘 수 있고, 이때는 메모리를 가장 많이 쓰는 연결부터 끊깁니다.
- **꼬리: 복제본이 뒤처질 때 쌓이는 버퍼도 같은가요?**
  다릅니다. 백로그 크기를 넘는 복제 버퍼는 `maxmemory` 판단에서 빠지므로 키를 축출하게 만들지는 않지만, `used_memory`와 RSS를 키워 컨테이너 한도를 압박합니다.

**Q. RDB 저장 중에 메모리가 얼마나 늘었는지는 어떻게 아나요?**
`INFO persistence`의 `current_cow_size`(진행 중)와 `rdb_last_cow_size`(마지막 저장)로 봅니다. 자식이 보고한 자기 전용 페이지의 합이라 대부분이 부모와 더 이상 공유하지 않게 된 페이지입니다(자식의 새 할당도 조금 섞입니다). 복사분은 Redis가 할당한 것이 아니라 `used_memory`에 더해지지 않고, 두 프로세스의 RSS를 더하면 공유 페이지를 두 번 세므로 어느 쪽도 이 증가를 바로 보여 주지 않습니다. (본문: "장면 3: RDB 저장 중의 copy-on-write")

- **꼬리: 최악의 경우 얼마까지 늘 수 있나요?**
  자식이 사는 동안 부모가 모든 페이지에 한 번씩 쓰면 데이터 전체에 가까운 양이 복사됩니다. 실제 양은 쓰기량과 흩어진 정도, 저장 시간에 달려 있어 운영 부하에서 재야 합니다.

**Q. 작은 객체 수백만 개를 저장하려는데, 메모리를 줄이려면 무엇을 보나요?**
키마다 붙는 고정 비용(객체 머리말, 해시 표의 칸, 만료 표의 칸, 크기 단계 올림)이 값보다 클 수 있다는 점을 봅니다. 키 이름을 줄이거나, 관련 객체를 작은 Hash로 묶어 한 덩어리 표현을 쓰게 하는 것이 선택지입니다. 묶으면 객체마다 키 TTL을 따로 줄 수 없어 필드 단위 만료를 써야 하고, 한 키에 접근이 몰릴 수 있으므로, 접근 패턴과 함께 고릅니다([String과 Hash](./string-vs-hash.md)). (본문: "데이터 자체 크기와 Redis가 쓰는 메모리는 왜 다른가")

- **꼬리: `used_memory_dataset`을 키 수로 나누면 키당 메모리 아닌가요?**
  dataset은 뺄셈 결과이고, 객체 머리말과 해시 표의 추정 비용이 overhead로 가 있어 실제 키당 메모리보다 작게 나옵니다. 용량 추정에는 대표 데이터를 넣기 전후의 `used_memory` 차이를 키 수로 나눈 값을 씁니다.
