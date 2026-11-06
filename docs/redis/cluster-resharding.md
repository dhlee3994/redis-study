# 클러스터 리샤딩: 노드를 더하고 슬롯을 옮기는 동안 애플리케이션은 무엇을 준비해야 하는가

> **핵심 질문**: 운영 중인 Redis Cluster에 노드를 더하고 슬롯을 옮길 때, 장애 없이 끝내려면 애플리케이션은 무엇을 미리 준비해야 하는가? 준비하지 않으면 어느 요청이 어떻게 실패하는가? 큰 키가 든 슬롯을 옮기는 동안에는 무엇이 멈추는가?
>
> **범위**: 주 서버(마스터)를 하나 더하고 슬롯을 그쪽으로 옮기는 작업을 다룹니다. 클라이언트의 토폴로지 갱신 설정, 옮기는 동안의 `ASK`·`TRYAGAIN`과 재시도, 큰 키가 든 슬롯의 이동 시간과 그동안 막히는 것, 작업 순서, 작업 중에 볼 지표를 다룹니다. 슬롯·해시 태그·리다이렉트의 기본 동작은 [Redis Cluster](./cluster.md)에, 클러스터를 도입할지의 판단은 [클러스터 도입 판단](./cluster-adoption.md)에 있습니다. 노드를 빼는 작업, 복제본을 더하는 작업, 장애 조치는 다루지 않습니다.

## 배경 지식

### 이 문서의 예시: 노드 D를 더해 슬롯 13307을 C에서 D로 옮깁니다

문서 끝까지 다음 장면 하나를 씁니다. [Redis Cluster](./cluster.md)의 예시를 그대로 이어 받습니다.

- 클러스터는 주 서버 3대 A(슬롯 0~5460), B(5461~10922), C(10923~16383)로 이루어져 있습니다.
- 상품 1의 재고 `stock:{item:1}`(String)과 구매자 목록 `buyers:{item:1}`(Set)은 해시 태그 `{item:1}`로 묶여 같은 슬롯 13307, 노드 C에 있습니다.
- 주문 API(Spring Boot, Lettuce)는 두 가지 요청을 보냅니다. 재고를 읽는 `GET stock:{item:1}`(키 하나)과, "이 사용자가 아직 사지 않았고 재고가 남았으면 재고를 1 줄이고 구매자에 넣는다"는 Lua 스크립트(키 둘)입니다. 스크립트는 Redis 서버가 한 번에 실행하는 요청이고, 다룰 키 두 개를 `EVAL`의 키 목록으로 함께 보냅니다. 노드는 이 키 목록만 보고 실행하기 전에 키가 자기에게 있는지 검사할 수 있습니다.
- 특가 행사를 앞두고 운영자가 빈 노드 D를 클러스터에 더하고, 슬롯 13307을 C에서 D로 옮깁니다.

```text
          주문 API (Lettuce, 슬롯 지도 사본)
            │ GET stock:{item:1}
            │ EVAL 주문스크립트 (stock:{item:1}, buyers:{item:1})
            ▼
   A 0~5460     B 5461~10922     C 10923~16383       D (새 노드, 슬롯 없음)
                                  슬롯 13307 ──────────▶ 슬롯 13307
                                  stock:{item:1}  MIGRATE
                                  buyers:{item:1}
```

위쪽 화살표는 명령이 가는 방향이고, C에서 D로 가는 화살표는 키가 옮겨지는 방향입니다. 복제본은 그림에서 뺐습니다. 이 문서의 작업은 주 서버 사이에서 슬롯을 옮기는 일이고, 복제본은 각자 자기 주 서버를 따라가기 때문입니다. 아래 실험의 노드 4개도 이 그림의 A~D에 그대로 대응하며, 실험에서는 복제본을 두지 않았습니다.

### 슬롯 지도와 리다이렉트

- **한 줄**: 클러스터는 키를 16,384개 슬롯 중 하나에 넣고 슬롯마다 맡을 주 서버를 정합니다. 클라이언트는 "슬롯 → 노드" 대응표(슬롯 지도)의 사본을 들고 직접 노드를 고르며, 틀린 노드에 보내면 노드가 리다이렉트 응답으로 바로잡아 줍니다.
- **이 주제에서는**: 리샤딩은 슬롯 지도의 한 줄(13307 → C)을 13307 → D로 바꾸는 일입니다. 바꾸는 동안과 바꾼 뒤에 클라이언트가 받는 응답은 세 가지입니다. `MOVED`는 "그 슬롯의 주인이 바뀌었다", `ASK`는 "옮기는 중이니 이번 한 번만 새 노드에 물어보라", `TRYAGAIN`은 "옮기는 중인 슬롯에서 요청한 키 중 일부가 이 노드에 없으니(다른 노드로 갔거나 아직 만들어지지 않았으니) 잠시 뒤 다시 보내라"입니다. 옮기는 중인 노드에 `ASK`를 따라 들어갈 때는 클라이언트가 먼저 `ASKING`이라는 명령을 보냅니다. `ASKING`은 "이 슬롯의 주인은 아니지만 다음 명령 하나는 받아 달라"는 일회성 허락입니다. 각 응답이 나오는 서버 쪽 조건은 [Redis Cluster](./cluster.md)의 "리샤딩 중에는 ASK와 TRYAGAIN이 생깁니다"에 있습니다.

### 토폴로지 갱신

- **한 줄**: 토폴로지 갱신은 클라이언트가 노드에 클러스터 구성을 다시 물어 자기 사본을 고치는 일입니다. 클라이언트가 받는 구성(Lettuce가 `CLUSTER NODES`로 받는 내용)에는 "슬롯 → 노드" 대응만이 아니라 노드 목록 전체가 들어 있으므로, 슬롯이 하나도 없는 D도 목록에 오를 수 있습니다. 이 문서에서 "앱이 D를 안다"는 D가 이 노드 목록에 있다는 뜻이고, "13307의 주인이 D다"와는 다릅니다. Lettuce는 이 갱신을 정해진 주기마다 하는 방식(주기 갱신)과, `MOVED`처럼 지도가 낡았다는 신호를 받았을 때 하는 방식(사건 기반 갱신, Lettuce 용어로 adaptive refresh)을 따로 켜게 합니다.
- **이 주제에서는**: 새 노드 D는 앱이 처음 지도를 받은 뒤에 생긴 노드입니다. 앱의 지도에 D가 없으면 무슨 일이 생기는지가 이 문서의 첫 번째 질문입니다. Lettuce 6.6.0과 Spring Boot 3.5의 기본값은 두 갱신이 모두 꺼짐입니다(설정 이름과 기본값은 [Redis Cluster](./cluster.md)의 "Lettuce는 지도를 언제 받고, 언제 고치는가").

### 메인 스레드: 노드 하나는 명령을 한 줄로 실행합니다

- **한 줄**: Redis 노드 하나는 메인 스레드 하나가 명령을 하나씩 끝까지 실행하므로, 오래 걸리는 명령 하나가 그 노드의 다른 모든 요청을 기다리게 합니다. 원리는 [Redis 이벤트 루프](./event-loop.md)에 있습니다.
- **이 주제에서는**: 키를 옮기는 `MIGRATE`도 보내는 노드의 메인 스레드에서 도는 명령 하나입니다. 큰 키를 옮기는 동안 C의 다른 슬롯 요청까지 기다리는 이유가 여기서 나옵니다("큰 키가 든 슬롯은 옮기는 동안 두 노드를 멈춥니다").

### 재시도와 "실행되지 않았다"는 보장

- **한 줄**: 재시도는 실패한 요청을 다시 보내는 일이고, 안전한지는 "처음 보낸 요청이 서버에서 실행되지 않았다고 확신할 수 있는가"로 정합니다. 원리는 [재시도와 서킷 브레이커](../cs/retry-and-circuit-breaker.md)에 있습니다.
- **이 주제에서는**: 리샤딩 중의 실패는 대부분 "그 전송은 실행 전에 거부되었다"가 확실한 실패입니다. 다만 그것은 마지막 전송에 대한 사실이고, 그 앞에 결과를 모르는 전송이 있었다면 요청 전체의 실행 여부까지 말해 주지는 않습니다("실행 전에 거부되므로 재시도해도 두 번 실행되지 않습니다"). 또 기다리는 동안 풀리는 실패와 기다려도 풀리지 않는 실패를 구분해야 재시도가 의미 있습니다("새 노드를 모르는 클라이언트는 옮긴 슬롯에서 계속 실패합니다", "옮기는 동안 여러 키 명령은 TRYAGAIN을 받습니다").

## 리샤딩은 네 단계이고, 단계마다 클라이언트가 받는 응답이 다릅니다

### 노드 추가와 슬롯 이동은 따로 일어납니다

노드를 더하는 일과 슬롯을 옮기는 일은 별개의 작업입니다. 공식 문서는 새 주 서버를 더하는 일을 "빈 노드를 더한 뒤 데이터를 옮기는 것"으로 설명합니다(공식 문서 scaling, "Add a new node").

1. **빈 노드 추가**: `redis-cli --cluster add-node <D> <기존 노드>`로 D를 클러스터에 소개합니다. 키는 옮기지 않습니다. 노드들은 이후 클러스터 버스로 D를 서로 알립니다. 이 시점의 D는 슬롯이 하나도 없는 주 서버이므로 데이터를 갖지 않습니다.
2. **이동 표시**: D에 "13307을 C에서 받는 중(IMPORTING)", C에 "13307을 D로 보내는 중(MIGRATING)"을 표시합니다.
3. **키 이동**: C에서 13307의 키 이름을 몇 개씩 꺼내(`CLUSTER GETKEYSINSLOT`) `MIGRATE`로 D에 보냅니다. `redis-cli --cluster reshard`는 한 번에 10개씩(`--cluster-pipeline`, 기본 10), `MIGRATE` 한 번의 타임아웃 60,000밀리초(`--cluster-timeout`, 기본 60000)로 보냅니다.
4. **주인 변경**: `CLUSTER SETSLOT 13307 NODE <D>`로 13307의 주인을 D로 바꿉니다. 표시를 붙이고 주인을 바꾸는 명령을 어느 노드에 어떤 순서로 보낼지는 `redis-cli`가 정해 둔 순서를 따르므로, 운영자가 따로 정할 것은 없습니다.

슬롯 여러 개를 옮기면 `redis-cli`는 슬롯 하나씩 2~4단계를 차례로 반복합니다. 슬롯 1,000개를 옮기는 작업은 이 반복이 1,000번 일어나는 것이고, 그동안 "옮기는 중인 슬롯"은 한 번에 하나입니다.

### 예시의 두 요청이 단계마다 받는 응답

슬롯 13307에 대해 앱의 두 요청이 단계마다 받는 응답은 다음과 같습니다. 앱의 지도에는 13307 → C가 적혀 있다고 둡니다.

| 단계 | C에 있는 키 | D에 있는 키 | `GET stock:{item:1}`(C로 보냄) | 주문 스크립트(C로 보냄) |
| --- | --- | --- | --- | --- |
| 1. D 추가 직후 | 둘 다 | 없음 | C가 실행 | C가 실행 |
| 2. 이동 표시 | 둘 다 | 없음 | C가 실행 | C가 실행 |
| 3a. `stock`만 옮겨짐 | `buyers` | `stock` | `ASK` → D | `TRYAGAIN` |
| 3b. 둘 다 옮겨짐 | 없음 | 둘 다 | `ASK` → D | `ASK` → D |
| 4. 주인이 D로 바뀜 | 없음 | 둘 다 | `MOVED` → D | `MOVED` → D |

이 표가 보여 주는 요점은 둘입니다.

- **새 노드 D의 주소는 3단계부터 클라이언트에 도착합니다.** `ASK`와 `MOVED`에는 D의 주소가 들어 있고, 클라이언트는 그 주소로 연결을 맺어야 합니다. 클라이언트가 D를 모르면 무엇이 일어나는지는 다음 절에서 봅니다.
- **여러 키를 다루는 요청은 3단계 동안 실패할 수 있습니다.** 두 키가 서로 다른 노드에 나뉘어 있거나, 요청한 키가 아직 없는 순간이 있기 때문입니다. 재고는 있고 구매자 키가 아직 없는 첫 구매처럼 새 키를 만드는 스크립트는 슬롯을 다 옮길 때까지 계속 거부될 수 있습니다([Redis Cluster](./cluster.md)의 "리샤딩 중에는 ASK와 TRYAGAIN이 생깁니다").

## 새 노드를 모르는 클라이언트는 옮긴 슬롯에서 계속 실패합니다

### Lettuce는 지도에 없는 노드로 연결하지 않습니다

Lettuce는 `MOVED`나 `ASK`를 받으면 응답에 적힌 주소로 같은 명령을 다시 보냅니다. 그런데 그 주소로 연결을 얻기 전에 "이 주소가 내 지도에 있는 노드인가"를 검사하고, 없으면 연결을 거부합니다. 이 검사는 `ClusterClientOptions`의 `validateClusterNodeMembership`이며 기본값은 켜짐입니다. 거부되면 명령은 "Connection to <주소> not allowed. This partition is not known in the cluster view."라는 메시지의 `UnknownPartitionException`으로 끝납니다.

`ASK`를 따라갈 때도 같은 연결 경로와 같은 검사를 거칩니다. 그래서 기본 설정의 앱은 슬롯의 주인이 D로 바뀐 뒤(4단계)만이 아니라, **키가 D로 옮겨지기 시작한 순간(3단계)부터** 그 키의 요청이 실패한다고 예상할 수 있습니다. 아래 실험으로 이 예상을 확인했습니다.

### 실험: 토폴로지 갱신 설정에 따라 무엇이 실패하는가

**목적**: 기본 설정의 Lettuce가 앱 시작 뒤에 더한 노드로 옮긴 슬롯에서 `UnknownPartitionException`으로 실패하는지 확인하고, 사건 기반 갱신과 주기 갱신이 그 실패를 얼마나 줄이는지 봅니다.

**환경**: 로컬 PC 한 대(Apple M1 Pro, macOS)의 Docker(OrbStack) 안에 Redis 8.2.10 공식 이미지 컨테이너 4개를 띄웠습니다. 3개로 클러스터를 만들고(복제본 없음), 1개는 처음에는 합류시키지 않았습니다. 클라이언트는 Spring 없이 Lettuce 6.6.0만 쓴 작은 Java 프로그램이고, 같은 Docker 네트워크의 다른 컨테이너에서 돌렸습니다. 모든 노드가 같은 PC 안에 있으므로 네트워크 지연은 실제 서버 사이와 다를 수 있습니다.

**조건**: 클라이언트는 시작할 때 노드 3개짜리 지도를 받고, 0.5초마다 예시의 `GET`과 주문 스크립트를 한 번씩 보내 결과를 기록합니다. 그동안 위 네 단계를 손으로 한 단계씩 실행하고(D 추가 → 이동 표시 → `stock` 이동 → `buyers` 이동 → 주인 변경), 주인 변경 뒤 30초 이상 관찰했습니다. 단계 사이를 수 초씩 일부러 벌려 각 상태를 여러 번 관찰했습니다. 실험용 노드의 `cluster-node-timeout`은 5,000밀리초로 줄였습니다(기본 15,000). 이 값은 Lettuce의 갱신 제한 시간과는 별개입니다. 다만 노드끼리 연락하고 상태를 수렴하는 주기에도 쓰이므로, 새 노드가 합류하는 데 걸린 시간에는 영향을 주었을 수 있습니다(비교 실험은 하지 않았습니다). 주기 갱신은 10초로 두었습니다.

**관찰**:

| 설정 | 3a단계(`stock`만 D) | 3b단계(둘 다 D) | 4단계(주인이 D) 이후 |
| --- | --- | --- | --- |
| 기본(갱신 모두 꺼짐) | `GET`은 모두 `UnknownPartitionException`, 스크립트는 `TRYAGAIN` | 둘 다 모두 `UnknownPartitionException` | 둘 다 관찰한 약 32초 동안 모두 `UnknownPartitionException`. 회복하지 않음 |
| 사건 기반 갱신만 | 첫 `GET` 1번만 `UnknownPartitionException`. 그 `ASK`가 갱신을 일으켜 약 0.5초 뒤부터 `GET` 성공. 스크립트는 `TRYAGAIN` | 둘 다 성공 | 둘 다 성공. 다만 갱신까지 약 15초 동안 명령 대부분이 `MOVED`를 거쳐 D로 감 |
| 주기 갱신만(10초) | D 추가 약 3초 뒤의 주기 갱신으로 D를 이미 앎. `GET` 성공, 스크립트는 `TRYAGAIN` | 둘 다 성공 | 둘 다 성공 |

기본 설정에서는 앱 시작 뒤 더한 노드로 옮긴 슬롯의 요청이 `UnknownPartitionException`으로 실패했고, 슬롯 이동이 끝난 뒤에도 회복하지 않았습니다. 실패는 주인 변경 전, 키가 옮겨지기 시작한 `ASK` 단계부터 시작되었고, 키 하나짜리 `GET`도 실패했습니다.

사건 기반 갱신에서 4단계 이후 `MOVED`가 약 15초 동안 계속된 이유는 다음 절의 30초 제한입니다. 3a단계 시작 때 한 번 갱신했으므로, 그로부터 30초가 지나기 전에는 `MOVED`가 새 갱신을 일으키지 못했습니다. 이 동안에도 D는 이미 지도에 있었으므로 요청은 성공했고, 요청마다 C를 한 번 거치는 왕복이 더해졌습니다.

모든 설정에서 3a단계의 주문 스크립트는 `TRYAGAIN`을 받았습니다. 토폴로지 갱신은 이 실패를 막지 못합니다. 지도가 정확해도 요청한 키 일부가 그 노드에 실제로 없기 때문입니다(다른 노드로 갔거나, 첫 구매처럼 아직 만들어지지 않았거나).

**한계**: 단계 사이의 시간을 손으로 벌렸으므로, 실패 횟수는 단계가 얼마나 길었는가를 보여 줄 뿐 실제 리샤딩의 실패율이 아닙니다. 실제 `redis-cli --cluster reshard`는 작은 키가 든 슬롯이라면 2~4단계를 훨씬 짧게 지납니다. 앱 인스턴스 하나, 요청 0.5초 간격이라는 조건에서 관찰했고, Spring Boot 설정을 거치지 않고 같은 Lettuce 옵션을 직접 주었습니다.

### 사건 기반 갱신만으로는 30초 동안 신호가 버려지는 빈틈이 남습니다

Lettuce의 사건 기반 갱신은 **한 번 실행하면 30초 동안 다음 신호를 버립니다**(`adaptiveRefreshTriggersTimeout`, 기본 30초). 신호가 몰릴 때 갱신 요청이 폭주하지 않게 하는 장치입니다. 문제는 D를 더하기 직전에 다른 이유로 갱신이 한 번 일어난 경우입니다. 그 갱신이 받은 지도에는 D가 없고, 30초가 지나기 전에 D로 슬롯을 옮기면 D로 가는 `ASK`와 `MOVED`, 지도에 없는 노드라는 신호는 모두 버려집니다.

이 빈틈을 같은 환경에서 확인했습니다. 슬롯 13307을 먼저 기존 노드 사이(C → B)로 옮겨 `MOVED`로 사건 기반 갱신을 한 번 일으키고, 약 1초 뒤 D를 더한 다음, D가 클러스터 상태를 정상으로 보고하자마자 13307을 B → D로 옮겼습니다.

| 설정 | 결과 |
| --- | --- |
| 사건 기반 갱신만 | 13307을 D로 옮긴 직후부터 약 23.5초 동안 `GET`과 스크립트가 모두 `UnknownPartitionException`. 첫 갱신 뒤 30초가 지난 시점의 신호로 갱신한 뒤에야 성공 |
| 사건 기반 갱신 + 주기 갱신(10초) | 같은 실패가 약 5.7초 동안. 다음 주기 갱신이 D를 포함한 지도를 받은 뒤 성공 |

30초는 갱신을 다시 받아들이기 시작하는 시점일 뿐, 그때 갱신이 저절로 일어나지는 않습니다. 30초가 지난 뒤 새 신호(이 실험에서는 다음 요청의 `MOVED`)가 와야 갱신이 시작되고, 갱신이 성공해야 지도가 바뀝니다. 주기 갱신은 30초 제한과 상관없이 정해진 주기마다 갱신을 시도합니다. 그래서 두 갱신을 함께 켜면, 갱신이 정상적으로 성공한다는 전제에서 빈틈이 대략 "주기 하나 + 지도를 받는 시간"으로 줄어듭니다. 갱신이 실패하거나 D가 아직 보이지 않는 노드에서 지도를 받으면 다음 주기로 밀리므로, 이 값은 보장된 상한이 아닙니다.

### 그래서 순서는 "노드 추가 → 앱이 새 지도를 받았는지 확인 → 슬롯 이동"입니다

위 결과에서 나오는 작업 순서는 다음과 같습니다.

1. **배포 전**: 앱에 사건 기반 갱신과 주기 갱신을 모두 켭니다. Spring Boot라면 `spring.data.redis.lettuce.cluster.refresh.adaptive=true`와 `spring.data.redis.lettuce.cluster.refresh.period`입니다. 이 설정은 `spring.data.redis.cluster.*`로 클러스터를 설정했을 때만 적용되므로, 연결 설정을 직접 빈으로 만들었다면 같은 옵션이 들어갔는지 확인합니다.
2. **노드 추가**: D를 더하고, D가 `CLUSTER INFO`에서 `cluster_state:ok`를 보고할 때까지 기다립니다. 실험에서는 D를 더한 직후 D가 잠시 `CLUSTERDOWN`으로 답해, 그때 시작한 `MIGRATE`가 "Target instance replied with error: CLUSTERDOWN"으로 실패했습니다. `redis-cli --cluster check`로 열린 슬롯(이동 표시가 남은 슬롯)이 없고 모든 슬롯이 덮였는지도 봅니다.
3. **앱 쪽 확인**: 모든 앱 인스턴스가 D를 포함한 지도를 받을 때까지 기다립니다. 진행 조건은 시간이 아니라 "각 인스턴스가 D를 포함한 지도를 적용했는가"입니다. 확인 수단이 없다면 주기 갱신의 주기에 여유를 더해 기다리는 것이 차선이지만, 갱신이 실패했을 수 있으므로 보장은 아닙니다. Lettuce는 지도가 바뀌면 이벤트 버스에 `ClusterTopologyChangedEvent`를 내므로, 앱이 이 이벤트를 로그로 남기게 해 두면 인스턴스마다 확인할 수 있습니다.
4. **슬롯 이동**: 그 뒤에 `redis-cli --cluster reshard`로 옮깁니다. `rebalance`를 쓴다면 슬롯이 없는 주 서버는 기본으로 대상에서 빠지므로 `--cluster-use-empty-masters`를 붙여야 D가 포함됩니다. 이 순서를 지키면 3단계의 `ASK`가 도착할 때 앱은 이미 D를 알고 있으므로, 새 노드를 몰라서 생기는 연결 거부가 생기지 않습니다. 이 순서가 없애는 것은 그 실패뿐입니다. 여러 키 요청의 `TRYAGAIN`과, 큰 키를 옮기는 동안의 명령 타임아웃("큰 키가 든 슬롯은 옮기는 동안 두 노드를 멈춥니다")은 남습니다.

이 순서가 사건 기반 갱신보다 중요한 이유가 있습니다. 사건 기반 갱신은 신호를 받은 뒤 갱신을 예약할 뿐 그 완료를 기다리지 않고, 신호를 준 요청은 그대로 D로 연결을 시도합니다. 그래서 갱신이 끝나기 전의 요청은 실패할 수 있습니다. 앞 실험에서는 갱신을 일으킨 첫 요청 하나가 실패했지만, 동시 요청이 많으면 몇 개가 실패할지는 정해져 있지 않습니다. 앱 인스턴스가 100대라면 인스턴스마다 이런 실패가 생길 수 있고, 30초 제한에 걸린 인스턴스는 수십 초 동안 실패할 수 있습니다. 노드를 먼저 알리고 옮기면 이 실패 자체가 생기지 않습니다.

**택하지 않은 대안: 멤버십 검사를 끄기.** `validateClusterNodeMembership`을 끄면 지도에 없는 주소로도 연결하므로, D를 모르는 앱도 `ASK`와 `MOVED`를 따라갈 수 있을 것입니다. 이 문서는 이 경로를 소스로만 봤고 실험하지 않았습니다. 택하지 않은 이유는 두 가지입니다. 지도는 여전히 낡아 있으므로 요청마다 C를 거치는 왕복이 계속 붙고, 리다이렉트 응답에 적힌 주소라면 어디든 연결하게 되어 지도라는 안전장치를 버리기 때문입니다. Spring Boot 속성으로는 바꿀 수 없어 클라이언트 옵션을 직접 만들어야 한다는 점도 있습니다.

## 옮기는 동안 여러 키 명령은 TRYAGAIN을 받습니다

### 실행 전에 거부되므로 재시도해도 두 번 실행되지 않습니다

`TRYAGAIN`은 노드가 명령을 실행하기 전에 키의 위치만 보고 거부한 응답입니다. 그 전송은 실행되지 않았으므로, 같은 명령을 다시 보내도 그 전송 때문에 두 번 반영되지는 않습니다. 멱등하다는 것은 같은 요청을 여러 번 실행해도 결과가 한 번 실행한 것과 같다는 뜻입니다. 재고 차감은 실행할 때마다 재고가 하나씩 줄므로 멱등하지 않습니다. 그래도 `TRYAGAIN`만 받은 요청이라면 다시 보내도 됩니다.

단서가 하나 있습니다. Lettuce는 기본으로 연결이 끊겼다 다시 맺어지면 응답을 받지 못한 명령을 다시 보냅니다. 그래서 "처음 전송이 실행됨 → 응답이 유실됨 → Lettuce가 다시 보냄 → 그사이 시작된 리샤딩으로 `TRYAGAIN`"이라는 순서도 가능합니다. 이때 마지막 `TRYAGAIN`은 처음 전송이 실행되지 않았다는 뜻이 아닙니다. 이 순서는 소스 경로로 판단한 것이며 재현하지 않았습니다. 멱등하지 않은 요청을 재시도할 때 "실행되지 않았다"고 믿을 수 있는 것은, 그 요청에 결과를 모르는 앞선 전송(타임아웃, 연결 끊김 뒤 재전송)이 없었을 때뿐입니다. 자동 재전송의 동작은 [Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)의 "연결이 끊기면 Lettuce가 응답 못 받은 명령을 다시 보냅니다"에 있습니다.

Lettuce는 `MOVED`와 `ASK`만 따라가고 `TRYAGAIN`은 다시 보내지 않으므로, 앱에는 예외로 올라옵니다. 실험에서 Lettuce의 예외는 메시지가 "TRYAGAIN Multiple keys request during rehashing of slot"인 `RedisCommandExecutionException`이었습니다. Spring Data Redis 3.5.13은 이 예외를 `RedisSystemException`("Error in execution")으로 감싸므로, 재시도 여부는 원인 예외의 메시지가 `TRYAGAIN`으로 시작하는지로 가립니다. 원인 예외로 오류를 가르는 방법은 [Redis 오류별 재시도 정책](./retry-policy.md)의 "Spring에서 오류를 구분하는 법"에 있습니다.

`UnknownPartitionException`도 그 전송에 대해서는 실행 전의 실패입니다. 명령은 C에서 `ASK`나 `MOVED`로 거부되었고, D로는 연결 자체를 맺지 않았습니다. 앞의 단서는 여기에도 똑같이 적용됩니다. Spring Data Redis는 이 예외를 `RedisSystemException`("Redis exception")으로 바꿉니다. 다만 이 실패는 기다린다고 풀리지 않을 수 있습니다. 갱신이 꺼져 있으면 영원히, 30초 제한에 걸렸으면 수십 초 동안 같은 결과입니다. 그래서 이 예외의 재시도는 갱신을 켠 앱에서 짧게 한두 번까지만 의미가 있습니다. 실험에서 사건 기반 갱신을 켠 앱은 실패한 다음 요청(약 0.5초 뒤)부터 성공했습니다.

### 재시도 예산은 슬롯 하나를 옮기는 시간으로 정합니다

`TRYAGAIN`이 이어지는 시간은 요청한 키 일부가 요청을 받은 노드에 없는 시간입니다. 두 경우가 있습니다.

- **두 키가 모두 있을 때**: 두 키가 서로 다른 `MIGRATE` 묶음에 들어가면 두 묶음 사이에 `TRYAGAIN`이 납니다. `redis-cli`는 `CLUSTER GETKEYSINSLOT`이 돌려준 순서대로 10개씩 묶으므로, 같은 태그의 두 키가 같은 묶음에 들어간다는 보장은 없습니다. 실험의 3a단계는 이 구간을 손으로 늘린 것입니다.
- **새 키를 만드는 요청일 때**: 첫 구매처럼 구매자 키가 아직 없으면, 재고 키가 C에 남아 있는 동안은 C가, 재고 키가 D로 간 뒤에는 D가 거부합니다. 그래서 슬롯 하나를 다 옮길 때까지 실패할 수 있습니다([Redis Cluster](./cluster.md)의 "리샤딩 중에는 ASK와 TRYAGAIN이 생깁니다").

슬롯 하나를 옮기는 시간은 그 슬롯의 키 수와 키 크기에 달려 있습니다. 작은 키 몇십 개라면 `MIGRATE` 몇 번이면 끝나므로, 몇십~몇백 밀리초 간격으로 몇 번 재시도하면 대개 넘깁니다(이 범위는 이 문서의 추정이며, 실제 시간은 작업 중에 재 봐야 합니다). 반면 큰 키가 든 슬롯은 `MIGRATE` 한 번이 길어지고, 그 시간 동안은 재시도도 기다리기만 합니다. 재시도 횟수와 간격은 "슬롯 하나를 옮기는 시간"과 "요청의 전체 기한" 중 짧은 쪽에 맞추고, 기한을 넘기면 실패로 돌려줍니다. 재시도가 부하를 곱하는 문제와 기한 안에서 재시도하는 원칙은 [재시도와 서킷 브레이커](../cs/retry-and-circuit-breaker.md)에 있습니다.

## 큰 키가 든 슬롯은 옮기는 동안 두 노드를 멈춥니다

### MIGRATE 한 번은 직렬화·전송·복원·삭제를 모두 기다립니다

`MIGRATE`는 보내는 노드 C의 메인 스레드에서 끝까지 기다리는 명령입니다. 한 번의 실행 안에서 다음 일이 차례로 일어납니다.

1. C가 키 전체를 `DUMP` 형식의 바이트로 직렬화해 메모리에 만듭니다. 키 안을 나눠 보내지 않습니다.
2. C가 그 바이트를 D에 `RESTORE-ASKING` 명령으로 보냅니다.
3. D가 받은 바이트로 키를 되살립니다. 이 동안 D의 메인 스레드가 이 일을 합니다.
4. C가 D의 성공 응답을 받은 뒤 자기 쪽 키를 지웁니다. 이 삭제는 `lazyfree-lazy-server-del`(기본 `no`)에 따라 메인 스레드에서 바로 합니다.

`KEYS` 옵션으로 여러 키를 한 번에 보내면 1단계에서 묶음의 키를 모두 직렬화해 한 번에 보내고, 응답도 키마다 차례로 읽습니다. C는 묶음 전체의 1~4가 끝날 때까지 다른 명령을 처리하지 않습니다. 공식 문서도 `MIGRATE`가 키를 옮기는 동안 두 인스턴스를 막는다고 적습니다(공식 문서 migrate). 막히는 것은 슬롯 13307의 요청만이 아닙니다. **C가 맡은 다른 슬롯 5,000여 개의 요청도 함께** 기다립니다. D는 3단계 동안 막힙니다. 빅키가 왜 키 하나를 다루는 한 번에 비싼지는 [Hot Key와 Big Key](./hot-key-big-key.md)에 있습니다.

### 실측: 필드 10만 개와 100만 개의 Hash 하나를 옮기는 시간

같은 로컬 환경에서 슬롯 13307에 든 Hash 키 하나(`bighash:{item:1}`, 필드 이름 `field:<번호>`, 값 `value-<번호>`)를 C에서 D로 `MIGRATE` 한 번으로 옮기고, 두 노드의 `SLOWLOG`와, 별도 컨테이너에서 10밀리초마다 보낸 `PING`의 최대 응답 시간을 쟀습니다.

| Hash 필드 수 | `MEMORY USAGE`(C에서, 바이트) | C의 `MIGRATE` 실행 시간 | D의 `RESTORE-ASKING` 실행 시간 | 그동안 C에 보낸 `PING` 최대 | D에 보낸 `PING` 최대 |
| --- | --- | --- | --- | --- | --- |
| 100,000 | 7,172,960 | 27ms | 12ms | 21ms | 12ms |
| 1,000,000 | 64,388,704 | 718ms | 308ms | 713ms | 300ms |

실행 시간은 각 노드 `SLOWLOG`의 마이크로초 값을 밀리초로 반올림한 것이고, 각 크기에서 한 번씩만 쟀습니다.

해석은 다음과 같습니다.

- **C에 보낸 `PING`이 `MIGRATE` 실행 시간만큼 기다렸습니다.** 필드 100만 개에서 `MIGRATE`가 718ms였고, 같은 시간대의 `PING`은 최대 713ms를 기다렸습니다. `PING`은 슬롯과 관계없는 명령이므로, 그 시간에 C에 도착한 다른 슬롯의 요청도 실행이 밀립니다. 각 요청이 실제로 기다린 시간은 도착한 시점에 따라 다르며, 이 실험은 `PING`만 쟀습니다.
- **D도 막혔지만 C보다 짧았습니다.** D가 막히는 것은 되살리는 동안뿐이기 때문입니다.
- **키가 클수록 정지가 길어졌지만, 배율은 일반화하지 않습니다.** 한 번씩 잰 값이기 때문입니다. 이 결과가 말하는 것은 "키 하나의 크기가 그 노드 전체의 일시 정지 시간이 된다"는 점이고, 절대값은 CPU, 메모리 할당기, 네트워크, 자료형에 따라 달라집니다. 실제 서버 사이에서는 2단계의 전송에 네트워크 시간이 더해집니다.

앱이 이 정지를 어떻게 보는지는 명령 타임아웃에 달려 있습니다. 명령 타임아웃이 500ms인 앱이라면, 필드 100만 개짜리 키를 옮기는 동안 C로 간 명령은 슬롯과 관계없이 타임아웃으로 끝날 수 있습니다. Lettuce는 노드마다 연결을 나눠 쓰므로, 앞선 명령이 응답을 기다리면 그 연결의 뒤 명령도 함께 기다립니다([Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)의 "공유 연결의 대가: 앞 응답이 늦으면 뒤 응답도 늦습니다").

### MIGRATE 타임아웃이 짧으면 키가 두 노드에 모두 남을 수 있습니다

`MIGRATE`의 타임아웃은 전체 시간의 상한이 아니라 "전송 중에 아무것도 오가지 않는 시간"의 상한입니다(공식 문서 migrate). C는 D의 `RESTORE` 응답을 이 시간까지 기다립니다. D가 큰 키를 되살리는 시간이 타임아웃보다 길면, C는 기다림을 끝내고 `IOERR`을 돌려주며 자기 쪽 키를 지우지 않습니다. C의 정지는 그만큼 짧아질 수 있지만, 직렬화하는 시간은 이 타임아웃과 관계없고, D는 받은 키를 끝까지 되살립니다. 그래서 `IOERR` 뒤에 키는 C에만 있을 수도, 두 노드에 모두 있을 수도 있습니다(공식 문서 migrate).

같은 환경에서 필드 100만 개 키를 타임아웃 100ms로 옮겨 확인했습니다.

| 순서 | 결과 |
| --- | --- |
| `MIGRATE … 0 100` | `IOERR error or timeout reading to target instance` |
| 직후 C의 `EXISTS` | 1(키가 남아 있음) |
| 직후 D의 `ASKING` + `HLEN` | 1,000,000(D에도 완전한 키가 있음) |
| 타임아웃 60,000ms로 다시 `MIGRATE` | `ERR Target instance replied with error: BUSYKEY Target key name already exists.`(C가 D의 `RESTORE-ASKING` 오류를 감싸 돌려줌) |

키가 양쪽에 있는 동안 C는 아직 그 키를 갖고 있으므로 계속 C가 요청을 처리합니다. 문제는 그다음입니다. 같은 `MIGRATE`를 다시 보내면 D에 키가 있다며 거부되고, 그 사이 C에서 키가 바뀌었다면 두 사본이 다릅니다. `redis-cli --cluster reshard`는 `BUSYKEY`를 받으면, `--cluster-replace`를 붙이지 않았을 때 두 노드의 값을 `DEBUG DIGEST-VALUE`로 비교합니다. 같으면 덮어써서 이어 가고, 다르면 키 목록을 출력하고 "직접 고치거나 `--cluster-replace`를 붙여 다시 실행하라"고 안내한 뒤 실패로 끝납니다. 그런데 `DEBUG` 명령은 `enable-debug-command`(기본 `no`)가 막고 있어, 기본 설정에서는 값이 같아도 비교 자체가 실패해 작업이 멈출 수 있습니다(소스로 확인했고 실행하지는 않았습니다). `--cluster-replace`를 붙이면 비교 없이 덮어씁니다. 어느 쪽이든 작업이 중간에 멈추고 사람이 판단해야 하므로, 큰 키가 있는 클러스터에서 `--cluster-timeout`을 기본값보다 줄이지 않습니다. 줄여도 직렬화와 D의 복원 시간은 줄지 않습니다.

### 큰 키는 옮기기 전에 찾아 나눕니다

큰 키 하나의 직렬화·복원 시간은 리샤딩 설정으로 줄일 수 없습니다. 키 하나는 늘 통째로 옮겨지기 때문입니다. `--cluster-pipeline`은 한 번의 `MIGRATE`에 묶는 키 개수라서 묶음 전체의 정지 시간에는 영향을 주지만, 큰 키 하나가 든 묶음은 그 키의 시간 아래로 줄어들지 않습니다. 대응은 작업 전에 합니다.

- **찾기**: 옮길 슬롯의 노드에서 `redis-cli --bigkeys`나 `--memkeys`로 큰 키를 찾습니다([Hot Key와 Big Key](./hot-key-big-key.md)의 "빅키: 키를 훑으며 크기를 잽니다"). 슬롯 하나의 키 수는 `CLUSTER COUNTKEYSINSLOT`으로 봅니다.
- **나누기**: 큰 키를 여러 키로 나누면 키 하나의 직렬화·복원이 짧아집니다. 다만 나눈 키가 같은 슬롯에 남아 같은 `MIGRATE` 묶음에 들어가면 한 번의 정지 시간은 비슷할 수 있습니다. 나눈 키가 다른 슬롯이라면 이동이 여러 슬롯, 여러 번에 흩어집니다. 나누면 키 하나에 대한 원자적 연산을 잃을 수 있다는 대가는 [Hot Key와 Big Key](./hot-key-big-key.md)의 "빅키는 나누거나, 덜 읽습니다"에 있습니다.
- **시간과 범위 정하기**: 나눌 수 없다면 그 슬롯을 따로, 요청이 적은 시간에 옮기고, 그 시간 동안 C로 가는 요청의 타임아웃이 늘 수 있음을 미리 알립니다.

## 작업 중에 무엇을 보는가

### 작업 전

| 볼 것 | 어떻게 | 기준 |
| --- | --- | --- |
| 앱의 갱신 설정 | 배포 설정의 `refresh.adaptive`, `refresh.period` | 둘 다 켜져 있어야 함 |
| 새 노드의 상태 | D에서 `CLUSTER INFO`의 `cluster_state` | `ok` |
| 열린 슬롯 | `redis-cli --cluster check <노드>` | "All 16384 slots covered", 이동 표시가 남은 슬롯 없음 |
| 앱이 새 노드를 아는가 | 앱 로그의 `ClusterTopologyChangedEvent`. 없으면 주기에 여유를 더해 기다림(보장은 아님) | 모든 인스턴스가 D를 포함한 지도를 적용함 |
| 앱에서 새 노드에 닿는가 | `CLUSTER NODES`에 적힌 D의 주소·포트로 앱이 도는 곳(컨테이너, 다른 네트워크)에서 접속해 봄 | 접속됨. `ASK`·`MOVED`와 지도에는 노드가 알리는 주소가 실리므로, 컨테이너·NAT 환경에서는 D의 `cluster-announce-ip`·`cluster-announce-port`가 앱에서 닿는 주소여야 함 |
| 옮길 슬롯의 큰 키 | `--bigkeys`·`--memkeys`, `CLUSTER COUNTKEYSINSLOT` | 키 하나의 이동 시간이 앱의 명령 타임아웃보다 충분히 짧은가 |

### 작업 중

- **서버의 오류 응답 수**: 각 노드의 `INFO errorstats`는 돌려준 오류 응답을 접두어별로 셉니다. 실험에서 `errorstat_ASK`, `errorstat_MOVED`, `errorstat_TRYAGAIN`, `errorstat_CLUSTERDOWN`, `errorstat_IOERR`가 각각 늘어나는 것을 확인했습니다. `ASK`와 `TRYAGAIN`은 옮기는 동안 정상적으로 생기는 응답이므로 0이 아니라는 사실보다, 이동을 마친 뒤에도 `MOVED`가 계속 늘어나는지를 봅니다. 계속 늘면 지도가 낡은 앱이 남아 있는지 조사할 신호입니다. 서버 통계만으로는 어느 클라이언트가 왜 틀린 노드로 보냈는지 알 수 없으므로, 앱 쪽 `MOVED` 이벤트와 토폴로지로 확인합니다.
- **옮기는 중인 슬롯**: `CLUSTER NODES`의 출력에서 보내는 노드는 `[13307->-<D의 ID>]`, 받는 노드는 `[13307-<-<C의 ID>]`를 슬롯 범위 뒤에 붙입니다. 작업이 중간에 멈추면 이 표시가 남으므로, `redis-cli --cluster check`가 "open slots"로 알려 줍니다.
- **두 노드의 정지 시간**: C와 D의 `SLOWLOG`에 `MIGRATE`와 `RESTORE-ASKING`이 몇 밀리초로 남는지 봅니다. 앱 쪽에서는 C와 D로 가는 명령의 지연 백분위와 타임아웃 수를 봅니다.
- **앱의 예외**: 원인 예외별로 셉니다. `UnknownPartitionException`이 하나라도 나오면 그 인스턴스는 D를 모르는 것이므로 작업을 멈추고 확인합니다. `TRYAGAIN`은 재시도로 흡수되는지, 재시도 예산을 넘겨 실패로 올라오는지를 봅니다. Lettuce는 리다이렉트마다 이벤트 버스에 `MovedRedirectionEvent`, `AskRedirectionEvent`를 냅니다.

### 작업 후

`redis-cli --cluster check`로 모든 슬롯이 덮였고 열린 슬롯이 없는지 봅니다. 앱의 `MOVED` 이벤트와 서버의 `errorstat_MOVED` 증가가 멈췄는지 봅니다. 노드별 키 수와 메모리, `CLUSTER SLOT-STATS`의 슬롯별 키 수로 의도한 만큼 옮겨졌는지 확인합니다.

## 데이터와 요청이 늘면 무엇이 달라지는가

- **키가 커지면 노드 전체의 정지가 길어집니다.** 실측에서 필드 100만 개의 Hash 하나가 C를 약 0.7초 멈췄습니다. 키가 커질수록 이 시간이 늘고, 그 노드가 맡은 모든 슬롯의 요청이 함께 기다립니다.
- **슬롯 하나의 키가 많아지면 `TRYAGAIN` 구간이 길어집니다.** 키를 10개씩 옮기므로, 슬롯 하나의 키가 1만 개면 `MIGRATE`가 약 1,000번 필요합니다. 그만큼 새 키를 만드는 여러 키 요청이 오래 실패합니다.
- **앱 인스턴스가 많아지면 "모든 인스턴스가 새 지도를 받았는가"가 어려워집니다.** 사건 기반 갱신에만 기대면 인스턴스마다 갱신 전의 실패가 생길 수 있습니다. 주기 갱신을 짧게 하면 갱신 요청이 "인스턴스 수 × 묻는 노드 수"만큼 늘어나므로 주기를 무작정 줄일 수 없습니다([Redis Cluster](./cluster.md)의 "Lettuce는 지도를 언제 받고, 언제 고치는가"). 작업 순서에서 새 지도가 적용되기를 기다리는 편이 갱신 주기를 줄이는 것보다 쌉니다.
- **옮길 슬롯이 많아지면 작업 전체가 길어집니다.** 슬롯은 하나씩 옮겨지므로, 각 슬롯의 `TRYAGAIN` 구간과 큰 키 정지가 작업 시간 동안 흩어져 계속 나타납니다.

## 흔한 오해 모아 보기

| 오해 | 실제 | 본문 |
| --- | --- | --- |
| 노드를 더하면 데이터가 저절로 나뉜다 | 빈 노드가 더해질 뿐입니다. 슬롯은 따로 옮겨야 합니다 | 노드 추가와 슬롯 이동은 따로 일어납니다 |
| 리샤딩 중 실패는 여러 키 명령만 겪는다 | 앱이 새 노드를 모르면 키 하나짜리 `GET`도 `ASK` 단계부터 실패합니다 | 실험: 토폴로지 갱신 설정에 따라 무엇이 실패하는가 |
| 사건 기반 갱신만 켜면 충분하다 | 직전에 갱신한 앱은 30초 동안 신호를 버리므로, 그동안과 그 뒤 다음 신호가 올 때까지 새 노드를 모를 수 있습니다 | 사건 기반 갱신만으로는 30초 동안 신호가 버려지는 빈틈이 남습니다 |
| 토폴로지 갱신을 켜면 `TRYAGAIN`도 사라진다 | `TRYAGAIN`은 요청한 키 일부가 그 노드에 실제로 없어서(다른 노드로 갔거나 아직 없어서) 생기므로 지도와 무관합니다 | 실험: 토폴로지 갱신 설정에 따라 무엇이 실패하는가 |
| 슬롯을 옮기는 동안 막히는 것은 그 슬롯뿐이다 | `MIGRATE`는 보내는 노드의 메인 스레드를 잡으므로 그 노드의 모든 슬롯이 기다립니다 | MIGRATE 한 번은 직렬화·전송·복원·삭제를 모두 기다립니다 |
| `MIGRATE` 타임아웃을 줄이면 오래 막히지 않는다 | C가 응답을 기다리는 시간만 끊을 뿐 직렬화와 D의 복원은 그대로이고, 타임아웃이 나면 키가 두 노드에 모두 남아 작업이 멈출 수 있습니다 | MIGRATE 타임아웃이 짧으면 키가 두 노드에 모두 남을 수 있습니다 |

## 근거

**Redis 소스**(태그 [`8.2.10`](https://github.com/redis/redis/tree/8.2.10), 커밋 `86b20b1b9e5f03d86db72c3d59bec432e471fbc9`)

- [`src/cluster.c`](https://github.com/redis/redis/blob/8.2.10/src/cluster.c):
  - `migrateCommand`: 키를 찾아 `createDumpPayload`로 직렬화한 뒤 `RESTORE-ASKING`(클러스터 모드)으로 64KB씩 동기 전송, 응답을 동기로 읽음, 성공한 키만 `dbDelete`로 지움, 타임아웃이 0 이하면 1000ms, 소켓 오류·타임아웃이면 `IOERR`, 대상의 오류 응답은 "Target instance replied with error: …"
  - `clusterRedirectClient`: `TRYAGAIN`·`CLUSTERDOWN`·`MOVED`·`ASK` 응답을 일반 오류 응답으로 보냄(오류 통계 제외 표시 없음), `MOVED`·`ASK`에는 대상 노드의 `clusterNodePreferredEndpoint`(기본 `cluster-preferred-endpoint-type ip`이면 노드의 IP)를 실음
- [`src/networking.c`](https://github.com/redis/redis/blob/8.2.10/src/networking.c): `afterErrorReply`(오류 통계 제외 표시가 없으면 오류 접두어별로 `errorstats`에 셈)
- [`src/db.c`](https://github.com/redis/redis/blob/8.2.10/src/db.c): `dbDelete`(삭제를 `lazyfree-lazy-server-del` 설정에 따라 함)
- [`src/slowlog.c`](https://github.com/redis/redis/blob/8.2.10/src/slowlog.c): `slowlogCreateEntry`(실행 시간을 마이크로초로 기록)
- [`src/cluster_slot_stats.c`](https://github.com/redis/redis/blob/8.2.10/src/cluster_slot_stats.c): `addReplySlotStat`(슬롯별 `key-count`)
- [`src/config.c`](https://github.com/redis/redis/blob/8.2.10/src/config.c): `lazyfree-lazy-server-del` 기본 no, `cluster-require-full-coverage` 기본 yes, `cluster-node-timeout` 기본 15000, `enable-debug-command` 기본 no, `cluster-preferred-endpoint-type` 기본 ip
- [`redis.conf`](https://github.com/redis/redis/blob/8.2.10/redis.conf): "CLUSTER DOCKER/NAT support"(NAT·포트 전달·컨테이너에서는 노드 주소 발견이 실패할 수 있어, 노드가 자기 공개 주소를 `cluster-announce-ip`·`cluster-announce-port` 등으로 알리게 함)
- [`src/cluster_legacy.c`](https://github.com/redis/redis/blob/8.2.10/src/cluster_legacy.c): `clusterUpdateState`(`cluster-require-full-coverage`이면 맡은 노드가 없는 슬롯이 하나라도 있을 때 실패 상태), `clusterCommandSpecial`의 `CLUSTER SETSLOT … NODE` 처리(자기가 맡던 슬롯의 키가 남아 있으면 "Can't assign hashslot … while I still hold keys for this hash slot."), `clusterGenNodeDescription`(MIGRATING은 `[슬롯->-ID]`, IMPORTING은 `[슬롯-<-ID]`)
- [`src/redis-cli.c`](https://github.com/redis/redis/blob/8.2.10/src/redis-cli.c): `CLUSTER_MANAGER_MIGRATE_TIMEOUT 60000`, `CLUSTER_MANAGER_MIGRATE_PIPELINE 10`, `--cluster-timeout`·`--cluster-pipeline`·`--cluster-replace` 옵션, `clusterManagerMoveSlot`(받는 쪽 IMPORTING → 보내는 쪽 MIGRATING → 키 이동 → 받는 쪽, 보내는 쪽, 나머지 주 서버 순으로 `SETSLOT NODE`), `clusterManagerMigrateKeysInSlot`(`CLUSTER GETKEYSINSLOT`으로 묶음 크기만큼 꺼내 `MIGRATE`, `BUSYKEY`면 `--cluster-replace`가 없을 때 두 노드의 값을 비교해 같으면 `REPLACE`로 다시 보내고, 다르거나 비교에 실패하면 안내를 출력하고 실패로 끝남), `clusterManagerCompareKeysValues`(`DEBUG DIGEST-VALUE`로 비교), `clusterManagerCommandRebalance`(`--cluster-use-empty-masters`가 없으면 슬롯 없는 주 서버를 제외), `clusterManagerCommandAddNode`

**Redis 공식 문서**(저장소 [`redis/docs`](https://github.com/redis/docs/tree/e53775dde0ee7e39ebe327e7ffff583caa9fbd65), 커밋 `e53775dde0ee7e39ebe327e7ffff583caa9fbd65`)

- [scaling](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/operate/oss_and_stack/management/scaling.md): "Add a new node"(빈 노드를 더한 뒤 데이터를 옮김, `add-node`는 상태를 점검하고 `CLUSTER MEET`을 보냄, 슬롯이 없는 주 서버), "Reshard the cluster"(`redis-cli --cluster reshard`와 비대화식 옵션)
- [cluster-setslot](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/cluster-setslot.md): "Redis Cluster live resharding explained"(받는 쪽 IMPORTING을 먼저, 주인 변경은 받는 쪽에 먼저 보내는 이유)
- [migrate](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/migrate.md): 키를 옮기는 동안 두 인스턴스를 막음, `timeout`은 전송 중 최대 유휴 시간, `IOERR`이면 키가 양쪽에 있을 수 있음

**Lettuce**(태그 [`6.6.0.RELEASE`](https://github.com/redis/lettuce/tree/6.6.0.RELEASE))

- [`ClusterDistributionChannelWriter.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/ClusterDistributionChannelWriter.java): `doWrite`(MOVED와 ASK 모두 응답의 주소로 `getConnectionAsync(…, host, port)`를 거쳐 다시 보냄, `onMovedRedirection`·`onAskRedirection` 알림, `MovedRedirectionEvent`·`AskRedirectionEvent` 발행)
- [`PooledClusterConnectionProvider.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/PooledClusterConnectionProvider.java): `beforeGetConnection`(지도에 없는 주소면 `onUnknownNode`를 알리고, 멤버십 검사가 켜져 있으면 `UnknownPartitionException` "Connection to … not allowed. This partition is not known in the cluster view.")
- [`ClusterClientOptions.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/ClusterClientOptions.java): `DEFAULT_VALIDATE_CLUSTER_MEMBERSHIP = true`, `validateClusterNodeMembership` 빌더 옵션
- [`ClusterTopologyRefreshOptions.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/ClusterTopologyRefreshOptions.java): `DEFAULT_ADAPTIVE_REFRESH_TIMEOUT = 30`(초), `adaptiveRefreshTriggersTimeout`, `enablePeriodicRefresh`, `DEFAULT_REFRESH_PERIOD = 60`(주기만 켜고 값을 주지 않을 때)
- [`ClusterTopologyRefreshScheduler.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/ClusterTopologyRefreshScheduler.java): `indicateTopologyRefreshSignal`·`acquireTimeout`(사건 기반 갱신은 제한 시간이 지나지 않았으면 신호를 버림), `run`(주기 갱신은 제한 시간과 무관하게 갱신 작업을 제출)
- [`ClusterCommand.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/ClusterCommand.java): `complete`(MOVED·ASK만 다시 보냄. TRYAGAIN은 다시 보내지 않음)
- [`RedisClusterClient.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/RedisClusterClient.java): 지도가 바뀌었을 때 `ClusterTopologyChangedEvent` 발행
- [`PartitionException.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/PartitionException.java): `RedisException`을 상속(`UnknownPartitionException`의 상위)
- [`topology/DefaultClusterTopologyRefresh.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/cluster/topology/DefaultClusterTopologyRefresh.java): `loadViews`(묻는 노드마다 `requestTopology`(`CLUSTER NODES`)와 `requestInfo`(`INFO`))
- [`ClientOptions.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/ClientOptions.java): `DEFAULT_AUTO_RECONNECT = true`, [`protocol/DefaultEndpoint.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/protocol/DefaultEndpoint.java): `notifyDrainQueuedCommands`(끊긴 연결의 응답 못 받은 명령을 다시 보낼 대기열로 옮김)

**Spring Data Redis**(태그 [`3.5.13`](https://github.com/spring-projects/spring-data-redis/tree/3.5.13), 커밋 `42619b203148bae01235fe14f3cff3e171d93ebf`)

- [`LettuceExceptionConverter.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceExceptionConverter.java): `RedisCommandExecutionException` → `RedisSystemException`("Error in execution"), 그 밖의 `RedisException` → `RedisSystemException`("Redis exception")

**Spring Boot**(태그 [`v3.5.16`](https://github.com/spring-projects/spring-boot/tree/v3.5.16), 커밋 `0566f6933049aca6bc5ffc6d559fffade9cd2e0c`)

- [`RedisProperties.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/data/redis/RedisProperties.java): `Lettuce.Cluster.Refresh`(`period` 기본 없음, `adaptive` 기본 false, `dynamicRefreshSources` 기본 true)
- [`LettuceConnectionConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/data/redis/LettuceConnectionConfiguration.java): `initializeClientOptionsBuilder`(클러스터 속성이 있으면 `ClusterClientOptions.Builder`를 만들고 `period`가 있으면 `enablePeriodicRefresh`, `adaptive`면 `enableAllAdaptiveRefreshTriggers`. 사건 기반 갱신의 제한 시간과 멤버십 검사를 바꾸는 속성은 없음), `createClientOptions`(그 빌더를 `LettuceClientOptionsBuilderCustomizer`에 넘김)

**이 문서의 실험**(로컬 Docker, Redis 8.2.10 공식 이미지, Lettuce 6.6.0.RELEASE를 쓴 Java 프로그램, Java 21)

- 토폴로지 갱신 설정별 결과, 30초 빈틈, 새 노드의 합류 직후 `CLUSTERDOWN`, 큰 키 `MIGRATE`의 실행 시간과 `PING` 지연, 짧은 타임아웃의 `IOERR`·양쪽 잔존·`BUSYKEY`, `INFO errorstats`의 접두어별 증가, `CLUSTER NODES`의 이동 표시는 모두 이 실험에서 관찰했습니다. 실행 환경과 조건은 본문 "실험: 토폴로지 갱신 설정에 따라 무엇이 실패하는가"와 "실측: 필드 10만 개와 100만 개의 Hash 하나를 옮기는 시간"에 있습니다. 실험 코드는 저장소에 두지 않았습니다.

**이 문서가 추론한 것**

- 합류 직후의 새 노드가 `CLUSTERDOWN`으로 답한 원인(슬롯 지도를 다 받기 전에는 맡은 노드가 없는 슬롯이 있다고 봄)은 관찰과 `clusterUpdateState`에서 끌어낸 판단입니다. 같은 함수에는 과반 도달 여부, 상태 복귀 지연 같은 다른 분기도 있어 원인을 확정하지는 못했습니다.
- `TRYAGAIN` 앞에 Lettuce의 자동 재전송이 끼면 처음 전송이 실행되었을 수 있다는 설명, 기본 설정의 `redis-cli`가 `DEBUG` 명령이 막혀 값 비교에 실패할 수 있다는 설명은 소스로 판단한 것이며 실행하지 않았습니다.
- 새 노드가 알리는 주소에 앱이 닿지 못하면 갱신을 켜도 연결 실패로 끝난다는 설명은 위 소스와 설정 설명에서 끌어낸 판단이며, 이 실험은 모든 노드가 같은 Docker 네트워크에 있어 이 경우를 재현하지 않았습니다.
- 멤버십 검사를 끄면 새 노드를 모르는 앱도 리다이렉트를 따라갈 수 있다는 설명, 작은 키 슬롯의 `TRYAGAIN` 재시도 간격 범위는 실험하지 않은 추론입니다.

## 실무 포인트

### 작업 전에 앱이 갖춰야 할 것

| 중요도 | 항목 | 이유와 선택 |
| --- | --- | --- |
| 필수 | 사건 기반 갱신과 주기 갱신 모두 켜기 | 꺼 두면 새 노드로 옮긴 슬롯의 요청이 `ASK` 단계부터 끝까지 실패합니다. 사건 기반만 켜면 갱신이 끝나기 전의 요청이 실패할 수 있고 30초 빈틈이 남습니다. 주기는 앱 인스턴스 수와 노드 수로 생기는 갱신 부하를 보고 정합니다 |
| 필수 | 작업 순서: 노드 추가 → 앱이 새 지도를 받을 때까지 대기 → 슬롯 이동 | 앱이 새 노드를 먼저 알면 새 노드를 몰라서 생기는 연결 거부가 생기지 않습니다. 진행 조건은 각 인스턴스가 새 지도를 적용했는지이고, 확인할 수 없으면 주기에 여유를 더해 기다립니다 |
| 필수 | 여러 키 스크립트의 `TRYAGAIN` 재시도 | 그 전송은 실행 전 거부라, 결과를 모르는 앞선 전송이 없었다면 멱등하지 않은 스크립트도 다시 보낼 수 있습니다. 횟수와 간격은 슬롯 하나를 옮기는 시간과 요청 기한 중 짧은 쪽에 맞춥니다 |
| 알면 유리 | 원인 예외로 오류 가르기 | `TRYAGAIN`과 `UnknownPartitionException`은 Spring에서 둘 다 `RedisSystemException`입니다. 원인 예외의 종류와 메시지 접두어로 가릅니다 |
| 알면 유리 | 옮길 슬롯의 큰 키 찾기 | 키 하나의 `MIGRATE` 시간이 그 노드 전체의 정지 시간입니다. 앱의 명령 타임아웃과 비교합니다 |
| 참고 | `ClusterTopologyChangedEvent` 기록 | 인스턴스마다 새 지도를 받았는지 확인할 수단입니다 |

### 개발자가 정할 수 있는 것과 관찰해야 하는 것

| 정할 수 있는 것 | 관찰해야 하는 것 |
| --- | --- |
| `refresh.adaptive`, `refresh.period` | 작업 중 앱의 `UnknownPartitionException` 수(0이어야 함) |
| `TRYAGAIN` 재시도 횟수·간격·기한 | 재시도를 넘겨 실패로 올라온 `TRYAGAIN` 수 |
| 큰 키를 나눌지, 언제 옮길지 | C·D의 `SLOWLOG`에 남은 `MIGRATE`·`RESTORE-ASKING` 시간, 그 시간대의 앱 지연 |
| 노드 추가와 슬롯 이동 사이의 대기 시간 | 각 노드 `INFO errorstats`의 `MOVED`·`ASK`·`TRYAGAIN`·`CLUSTERDOWN` 증가 |
| `--cluster-timeout`(줄이지 않음) | `redis-cli --cluster check`의 열린 슬롯, `IOERR`·`BUSYKEY` 출력 |

### 증상과 확인 순서

**증상 1: 노드를 더하고 슬롯을 옮기기 시작하자, 일부 상품의 조회와 주문이 모두 실패합니다. 예외 메시지는 "not known in the cluster view"입니다.**

1. 실패한 키의 슬롯을 `CLUSTER KEYSLOT`으로 보고, 그 슬롯이 새 노드로 옮겨지는 중이거나 옮겨졌는지 `CLUSTER NODES`에서 봅니다.
2. 앱의 토폴로지 갱신 설정을 확인합니다. 꺼져 있다면 앱은 새 노드를 영영 모릅니다. 예외가 "not known in the cluster view"가 아니라 새 노드로의 연결 실패라면, 앱은 새 노드를 이미 지도에 올린 것입니다. 이때 원인 예외가 연결 거부나 연결 타임아웃처럼 그 주소에 닿지 못했다는 내용이면 "작업 전" 표의 주소 확인으로 가고, 연결은 되었지만 인증 같은 연결 직후의 준비에서 실패한 것이면 새 노드의 계정·비밀번호 설정을 봅니다(이 경우들의 예외 모양은 실험하지 않았습니다).
3. 당장은 앱을 순차로 재시작해 지도를 다시 받게 하고, 갱신 설정을 켜서 배포합니다. **부작용**: 재시작하는 동안 인스턴스가 줄고, 다시 맺는 연결이 몰립니다. 이동을 잠시 멈추는 것은 이미 `ASK`나 `MOVED`로 새 노드를 가리키는 슬롯에는 도움이 되지 않습니다.

**증상 2: 갱신을 켰는데도 노드 추가 직후 슬롯을 옮기자 수십 초 동안 같은 예외가 납니다.**

1. 직전 30초 안에 다른 리다이렉트로 사건 기반 갱신이 일어났는지 앱 로그로 봅니다.
2. 주기 갱신이 켜져 있는지 봅니다. 사건 기반 갱신만 켜져 있다면 30초 제한에 걸린 것입니다.
3. 주기 갱신을 켜고, 다음 작업부터는 노드 추가 후 모든 인스턴스가 새 지도를 적용한 것을 확인한 뒤 옮깁니다.

**증상 3: 슬롯 이동 중에 그 노드의 모든 API가 잠깐씩 타임아웃을 냅니다.**

1. 보내는 노드의 `SLOWLOG`에서 `MIGRATE`의 실행 시간을 봅니다. 받는 노드에서는 `RESTORE-ASKING`을 봅니다.
2. 오래 걸린 키를 찾아 크기를 봅니다.
3. 큰 키를 나누거나, 그 슬롯만 따로 요청이 적은 시간에 옮깁니다. `--cluster-timeout`을 줄여서 해결하려 하지 않습니다. **부작용**: 줄여도 직렬화와 받는 쪽의 복원 시간은 그대로이고, `IOERR`로 키가 양쪽에 남으면 작업이 멈출 수 있습니다.

### Spring에서 확인할 것

- `spring.data.redis.lettuce.cluster.refresh.*`는 `spring.data.redis.cluster.*`가 있을 때만 적용됩니다. `RedisClusterConfiguration`과 `LettuceConnectionFactory`를 직접 만든다면 `ClusterClientOptions`에 같은 갱신 옵션을 넣었는지 확인합니다.
- 사건 기반 갱신의 30초 제한과 멤버십 검사는 Spring Boot 속성으로 바꿀 수 없습니다. 바꿔야 한다면 `LettuceClientOptionsBuilderCustomizer`로 클라이언트 옵션 빌더를 직접 고쳐야 합니다. 클러스터 설정이면 이 빌더는 `ClusterClientOptions.Builder`입니다. 이 문서는 두 값을 기본값으로 두고 작업 순서로 빈틈을 막는 쪽을 택했습니다.
- 재시도를 `@Retryable` 같은 도구로 붙인다면, 재시도 조건을 예외 클래스가 아니라 원인 예외의 메시지 접두어(`TRYAGAIN`)로 정해야 합니다. `RedisSystemException` 전체를 재시도하면 `MISCONF`처럼 기다려도 풀리지 않는 오류까지 다시 보냅니다([Redis 오류별 재시도 정책](./retry-policy.md)).

## 말로 설명하기

### 30초 버전

노드를 더하는 것과 슬롯을 옮기는 것은 별개의 작업입니다. 슬롯을 새 노드로 옮기기 시작하면 클라이언트는 `ASK`와 `MOVED`로 새 노드의 주소를 받는데, Lettuce는 자기 지도에 없는 노드로는 연결하지 않습니다. 기본 설정은 지도를 갱신하지 않으므로, 옮긴 슬롯의 요청이 끝까지 실패합니다. 그래서 사건 기반 갱신과 주기 갱신을 모두 켜고, 노드를 더한 뒤 앱이 새 지도를 받을 때까지 기다렸다가 옮깁니다. 옮기는 동안의 여러 키 스크립트는 `TRYAGAIN`을 받으므로 짧게 재시도하고, 큰 키는 `MIGRATE` 한 번에 노드 전체를 멈추므로 미리 찾아 나눕니다.

### 2분 버전

리샤딩은 빈 노드 D를 더하고, 슬롯마다 "받는 중·보내는 중" 표시를 한 뒤, 키를 `MIGRATE`로 몇 개씩 옮기고, 마지막에 슬롯의 주인을 바꾸는 순서로 진행됩니다. 키가 옮겨지기 시작하면 옛 노드 C는 없는 키에 대해 `ASK`로 D를 가리키고, 주인이 바뀌면 `MOVED`로 D를 가리킵니다.

Lettuce는 두 응답 모두 D로 따라가려 하지만, 그 전에 D가 자기가 아는 노드 목록에 있는지 검사하고 없으면 `UnknownPartitionException`으로 끝냅니다. 로컬 실험에서 기본 설정의 앱은 키가 옮겨지기 시작한 순간부터 키 하나짜리 `GET`까지 실패했고, 이동이 끝난 뒤에도 회복하지 않았습니다. 사건 기반 갱신을 켜면 첫 요청 하나가 실패한 뒤 갱신으로 회복했고, 주기 갱신을 켜면 슬롯을 옮기기 전에 이미 D를 알아 실패하지 않았습니다. 사건 기반 갱신은 한 번 돌면 30초 동안 신호를 버리므로, 실험에서 직전에 갱신한 앱은 D를 23초 넘게 몰랐습니다. 그래서 두 갱신을 함께 켜고, 노드를 더한 뒤 모든 앱이 새 지도를 적용한 것을 확인하고 슬롯을 옮깁니다.

옮기는 동안 요청한 키 일부가 그 노드에 없으면(D로 갔거나 아직 없으면) 여러 키 스크립트는 `TRYAGAIN`을 받습니다. 그 전송은 실행 전 거부라 다시 보내도 그 때문에 두 번 반영되지는 않지만, Lettuce가 다시 보내지 않으므로 앱이 재시도를 맡습니다. Spring에서는 `RedisSystemException`으로 오므로 원인 예외의 메시지로 가립니다.

`MIGRATE`는 키 하나를 통째로 직렬화해 보내고 받는 쪽이 되살릴 때까지 기다리는 명령이라, 그동안 보내는 노드의 모든 슬롯이 멈춥니다. 로컬에서 필드 100만 개 Hash 하나가 C를 약 0.7초, D를 약 0.3초 멈췄습니다. 타임아웃을 줄여도 직렬화와 받는 쪽 복원은 줄지 않고 키가 양쪽에 남아 작업이 멈출 수 있으므로, 큰 키는 작업 전에 찾아 나눕니다.

### 예상 질문과 꼬리 질문

**Q. 노드를 더하면 부하가 바로 나뉘나요?**
아닙니다. `add-node`는 슬롯이 없는 빈 주 서버를 클러스터에 소개할 뿐입니다. 부하는 슬롯을 그 노드로 옮겨야 나뉩니다. 이 두 작업이 분리되어 있다는 점이 오히려 쓸모 있습니다. 노드를 먼저 더해 두고, 앱이 새 노드를 알게 된 뒤에 슬롯을 옮길 수 있기 때문입니다. (본문: "노드 추가와 슬롯 이동은 따로 일어납니다")

**Q. 갱신을 안 켜면 정확히 무엇이 실패하나요? `MOVED`를 따라가면 되지 않나요?**
따라가려면 새 노드로 연결해야 하는데, Lettuce는 지도에 없는 노드로의 연결을 기본으로 거부합니다. 그래서 새 노드로 옮긴 슬롯의 요청은 `UnknownPartitionException`으로 끝납니다. 로컬 실험에서는 키가 옮겨지기 시작한 `ASK` 단계부터 키 하나짜리 `GET`도 실패했고, 슬롯 이동이 끝나고 30초 넘게 지나도 회복하지 않았습니다. 기존 노드 사이에서 옮겼다면 연결 거부는 없고 요청마다 왕복이 하나 더 들 뿐입니다. (본문: "실험: 토폴로지 갱신 설정에 따라 무엇이 실패하는가")

- **꼬리: 그 검사를 끄면 되지 않나요?**
  `validateClusterNodeMembership`을 끄면 연결은 될 것으로 보지만, 이 문서는 소스로만 봤고 실험하지 않았습니다. 지도는 여전히 낡아 요청마다 옛 노드를 거치고, 리다이렉트에 적힌 주소라면 어디든 연결하게 되어 안전장치를 버립니다. 갱신을 켜고 작업 순서를 지키는 쪽이 원인을 고치는 방법입니다.

**Q. 사건 기반 갱신만 켜면 안 되나요? `MOVED`가 오면 바로 갱신하잖아요.**
두 가지가 남습니다. 첫째, 사건 기반 갱신은 갱신을 예약할 뿐 끝나기를 기다리지 않으므로 갱신 전의 요청은 실패할 수 있습니다. 실험에서는 인스턴스 하나에서 첫 요청 하나가 실패했고, 동시 요청이 많으면 실패 수는 정해져 있지 않습니다. 둘째, 한 번 갱신하면 30초 동안 다음 신호를 버립니다. 노드를 더하기 직전에 다른 이유로 갱신한 앱은 새 노드를 그 30초가 끝나고 다음 신호가 올 때까지 모릅니다. 로컬 실험에서 이 빈틈은 약 23.5초의 연속 실패로 나타났고, 10초 주기 갱신을 함께 켜자 약 5.7초로 줄었습니다. (본문: "사건 기반 갱신만으로는 30초 동안 신호가 버려지는 빈틈이 남습니다")

- **꼬리: 그럼 주기를 아주 짧게 하면 되지 않나요?**
  갱신 한 번에 묻는 노드마다 `CLUSTER NODES`와 `INFO`를 보내므로, 갱신 요청은 "인스턴스 수 × 노드 수 ÷ 주기"로 늘어납니다. 노드를 더하는 일은 드물므로, 주기를 줄이기보다 노드를 더한 뒤 앱이 새 지도를 적용한 것을 확인하고 옮기는 편이 쌉니다.
- **꼬리: 앱이 새 노드를 알았는지는 어떻게 확인하나요?**
  Lettuce는 지도가 바뀌면 `ClusterTopologyChangedEvent`를 냅니다. 이 이벤트를 로그로 남기면 인스턴스마다 확인할 수 있고, 그런 장치가 없다면 주기 갱신의 주기에 여유를 더해 기다립니다.

**Q. 리샤딩 중에 `TRYAGAIN`이 나면 재시도해도 안전한가요? 재고 차감인데요.**
그 전송에 대해서는 안전합니다. `TRYAGAIN`은 노드가 키의 위치만 보고 실행 전에 거부한 응답이라 그 전송으로는 재고가 줄지 않았습니다. 다만 그 요청에 앞서 타임아웃이나 연결 끊김 뒤 Lettuce의 자동 재전송이 있었다면, 처음 전송이 이미 실행되었을 수 있으므로 `TRYAGAIN`만으로 "한 번도 실행되지 않았다"고 단정하지 않습니다. 다만 Lettuce는 이 응답을 다시 보내지 않으므로 앱이 재시도해야 하고, Spring에서는 `RedisSystemException`으로 오므로 원인 예외의 메시지가 `TRYAGAIN`으로 시작하는지로 가립니다. (본문: "실행 전에 거부되므로 재시도해도 두 번 실행되지 않습니다")

- **꼬리: 몇 번, 얼마나 기다리며 재시도하나요?**
  `TRYAGAIN`은 요청한 키 일부가 요청을 받은 노드에 없는 동안 이어지고, 새 키를 만드는 스크립트라면 슬롯 하나를 다 옮길 때까지 이어질 수 있습니다. 그래서 횟수와 간격은 "슬롯 하나를 옮기는 시간"과 "요청의 전체 기한" 중 짧은 쪽에 맞추고, 넘기면 실패로 돌려줍니다. 큰 키가 든 슬롯이라면 재시도로 버틸 수 있는 시간이 아닐 수 있습니다.
- **꼬리: 토폴로지 갱신을 켜면 `TRYAGAIN`도 줄어드나요?**
  아닙니다. 지도가 정확해도 요청한 키 일부가 그 노드에 실제로 없기 때문에 생깁니다. 로컬 실험에서도 세 설정 모두 같은 구간에서 `TRYAGAIN`을 받았습니다.

**Q. 큰 키가 든 슬롯을 옮기면 무엇이 멈추나요?**
보내는 노드의 모든 요청입니다. `MIGRATE`는 키를 통째로 직렬화하고 보내고 받는 쪽이 되살린 응답을 받은 뒤 지울 때까지 메인 스레드에서 기다리므로, 그 노드가 맡은 다른 슬롯의 요청도 함께 기다립니다. 받는 노드는 되살리는 동안 멈춥니다. 로컬에서 필드 100만 개 Hash 하나가 보내는 쪽을 약 0.7초, 받는 쪽을 약 0.3초 멈췄고, 같은 시간대의 `PING`이 그만큼 기다렸습니다. 절대값은 환경에 따라 다릅니다. (본문: "큰 키가 든 슬롯은 옮기는 동안 두 노드를 멈춥니다")

- **꼬리: `MIGRATE` 타임아웃을 줄이면 덜 멈추지 않나요?**
  보내는 쪽이 응답을 기다리는 시간은 짧아질 수 있지만, 직렬화 시간은 그대로이고 받는 쪽의 복원은 멈추지 않습니다. 로컬에서 100ms로 줄이자 `IOERR`가 났고, 키는 두 노드에 모두 남았으며, 다시 옮기려 하자 `BUSYKEY`로 거부되었습니다. `redis-cli`는 이때 두 값을 비교하려 하는데, 값이 다르거나 비교에 쓰는 `DEBUG` 명령이 막혀 있으면 실패로 끝나고 사람이 다시 실행해야 합니다.
- **꼬리: `--cluster-pipeline`을 줄이면요?**
  묶음 하나의 정지 시간은 줄 수 있지만, 키 하나는 늘 통째로 옮겨지므로 큰 키 하나가 든 묶음은 그 키의 시간 아래로 줄지 않습니다. 큰 키는 작업 전에 찾아 나누거나 요청이 적은 시간에 그 슬롯만 따로 옮깁니다.

**Q. 작업 중에 무엇을 보면 잘 되고 있는지 알 수 있나요?**
앱 쪽은 `UnknownPartitionException`이 0인지와, 재시도를 넘겨 올라온 `TRYAGAIN` 수를 봅니다. 서버 쪽은 각 노드의 `INFO errorstats`에서 `MOVED`·`ASK`·`TRYAGAIN`·`CLUSTERDOWN` 증가를, 보내고 받는 노드의 `SLOWLOG`에서 `MIGRATE`·`RESTORE-ASKING`의 실행 시간을 봅니다. `ASK`와 `TRYAGAIN`은 옮기는 동안 정상적으로 나오므로, 끝난 뒤에도 `MOVED`가 계속 늘어나면 지도가 낡은 앱이 남았는지 앱 쪽 이벤트로 조사합니다. 작업 뒤에는 `redis-cli --cluster check`로 열린 슬롯이 없는지 봅니다. (본문: "작업 중에 무엇을 보는가")

- **꼬리: 새 노드를 더하자마자 옮기면 안 되나요?**
  앱 쪽 이유 말고도 서버 쪽 이유가 있습니다. 로컬 실험에서 갓 합류한 노드는 잠시 `CLUSTERDOWN`으로 답했고, 그때 시작한 `MIGRATE`는 실패했습니다. 새 노드의 `cluster_state:ok`와 `redis-cli --cluster check`를 먼저 확인합니다.
