# `@Cacheable`이 캐시하지 않거나, 엉뚱한 값을 주거나, 너무 오래 둘 때

> **핵심 질문**: `@Cacheable`을 붙였는데 왜 어떤 호출은 캐시를 거치지 않고, 어떤 호출은 다른 메서드나 다른 사용자의 값을 받고, 어떤 값은 지워지지 않고 남는가? 캐시 키에는 무엇을 넣어야 하고, 키 가짓수가 끝없이 늘면 무엇이 문제인가? Redis가 실패하면 메서드는 DB로 넘어가는가? 이런 일을 운영 중에 무엇으로 알아채는가?
>
> **범위**: Spring Boot 자동 설정이 만든 `RedisCacheManager`와, 일반 값을 돌려주는 메서드(`CompletableFuture`나 리액티브 타입이 아닌)를 다룹니다. `@CacheEvict`가 커밋 전후 어느 시점에 지우는지와 `transactionAware()`는 [Cache-aside 정합성](./cache-aside-consistency.md)에서, `sync = true`의 잠금 동작과 null 캐시의 TTL 문제는 [캐시 스탬피드](./cache-stampede.md)에서, 값을 못 읽는 직렬화 문제는 [직렬화기 고르기](./serializer.md)에서 다룹니다. 이 문서는 그 내용을 다시 설명하지 않고 필요한 곳에서 연결합니다.

## 배경 지식

### 이 문서의 예시: 상품 서비스의 `product` 캐시

문서 끝까지 다음 장면 하나를 씁니다.

- 상품 API 서버 **4대**가 Redis **한 대**를 캐시로 씁니다. 설정 클래스에 `@EnableCaching`을 붙여 애너테이션 캐싱을 켰고, 캐시 매니저는 Spring Boot 자동 설정이 만들었습니다. 설정 파일에는 `spring.cache.redis.time-to-live=10m` 한 줄만 있습니다. 아래 표의 DB 조회 시간은 설명을 위한 가상 수치입니다.
- `ProductService`라는 Spring 빈에 다음 메서드들이 있습니다.

| 메서드 | 붙은 애너테이션 | 하는 일 | 미스 때 비용 |
| --- | --- | --- | --- |
| `getProduct(id: Long): ProductDetail?` | `@Cacheable("product")` | 상품 상세를 DB에서 읽음. 없는 상품이면 `null` | DB 조회 20ms |
| `getProductPage(id: Long)` | 없음 | 같은 클래스 안에서 `getProduct(id)`를 불러 화면용 응답을 만듦 | |
| `getProductSummary(id: Long): ProductSummary` | `@Cacheable("product")` | 목록 화면용 요약. 나중에 다른 개발자가 추가함 | DB 조회 5ms |
| `getRanking(category: String, size: Int)` | `@Cacheable("ranking")` | 카테고리 판매 순위 집계 | DB 집계 800ms |

- 컨트롤러는 상품 상세에 `productService.getProduct(42)`를, 상품 페이지에 `productService.getProductPage(42)`를 부릅니다.
- 같은 Redis를 사내 관리자 앱도 쓰고, 관리자 앱에도 `product`라는 이름의 캐시가 있습니다.
- 존재하지 않는 상품 번호 `999999`로 오는 조회가 있습니다.

이 예시에서 `getProduct(42)`가 캐시에 넣는 Redis 키는 `product::42`입니다. 왜 이런 모양이 되는지, 그리고 위 장면의 어디서 문제가 생기는지를 차례로 따라갑니다.

### 애너테이션은 스스로 아무 일도 하지 않습니다: 프록시

- **한 줄**: 프록시는 원래 객체 앞에 서서 메서드 호출을 먼저 받는 대리 객체입니다. `@Cacheable`의 동작(캐시 확인, 메서드 실행, 캐시에 넣기)은 이 대리 객체가 합니다.
- **비유**: 사장실 앞의 비서입니다. 바깥에서 오는 문의는 비서가 먼저 받아 서류함(캐시)에 답이 있으면 바로 돌려주고, 없을 때만 사장에게 넘긴 뒤 답을 서류함에 꽂아 둡니다. 하지만 사장이 자기 방 안에서 혼잣말로 자기에게 묻는 질문은 비서를 거치지 않습니다.
- **실제로는**: `@EnableCaching`으로 애너테이션 캐싱을 켜 두면, Spring은 애플리케이션이 시작할 때 `@Cacheable`이 붙은 빈마다 그 클래스를 상속한 하위 클래스를 실행 중에 만들어(CGLIB 프록시), 원래 객체 대신 이 프록시를 다른 빈에 주입합니다. Spring Boot는 기본으로 이렇게 클래스를 상속하는 방식을 씁니다. 프록시는 애너테이션이 붙은 메서드를 재정의해서, 캐시 처리를 한 뒤 원래 객체의 같은 메서드를 부릅니다. Kotlin의 `by`로 만드는 위임 객체나 데코레이터와 비슷하지만, 직접 쓰는 코드가 아니라 Spring이 만든다는 점, 그리고 상속으로 만들기 때문에 **재정의할 수 없는 메서드는 가로채지 못한다**는 점이 다릅니다.
- **이 주제에서는**: 컨트롤러가 주입받은 `productService`는 프록시이므로 `productService.getProduct(42)`는 캐시를 거칩니다. 반면 `getProductPage` 안에서 부르는 `getProduct(id)`는 `this.getProduct(id)`, 곧 원래 객체의 메서드를 직접 부르는 호출이라 프록시를 지나지 않습니다. 이 문서의 "캐시하지 않는다" 문제는 대부분 여기서 나옵니다.

### 캐시 키: 메서드 인자에서 만든 값을 Redis 키 문자열로 바꿉니다

- **한 줄**: 캐시 키는 "이 호출의 결과를 어디에 두고 찾을지"를 정하는 값입니다. 따로 정하지 않으면 Spring이 메서드 인자로 만듭니다.
- **비유**: 서류함의 칸 이름입니다. 칸 이름이 같으면 다른 질문의 답이라도 같은 칸에서 꺼냅니다.
- **실제로는**: 키는 두 단계로 만들어집니다.
  1. Spring이 인자로 키 객체를 만듭니다(기본 `SimpleKeyGenerator`). 인자가 없으면 빈 키, 인자가 하나이고 `null`도 배열도 아니면 **그 인자 자체**, 그 밖에는(인자가 둘 이상이거나, 하나뿐인 인자가 `null`이나 배열이면) 인자를 모두 담은 `SimpleKey`입니다. **메서드 이름과 클래스는 키에 들어가지 않습니다.**
  2. Spring Data Redis가 이 키 객체를 문자열로 바꾸고 앞에 `캐시이름::`을 붙입니다. 문자열이면 그대로, 숫자처럼 변환기가 있는 타입은 변환기로(`42L` → `"42"`), 그 밖에는 그 클래스가 직접 정의한 `toString()`으로 바꿉니다. `SimpleKey`의 `toString()`은 `SimpleKey [인자1, 인자2]` 모양입니다. 직접 정의한 `toString()`이 없는 객체(`Object`의 것만 물려받은 객체)는 바꾸지 못하고 예외가 납니다.
- **이 주제에서는**: Kotlin의 `HashMap`은 키를 `equals`·`hashCode`로 비교합니다. Spring의 메모리 캐시도 그렇습니다. 하지만 **Redis 캐시에서 두 호출이 같은 항목을 쓰는지는 최종 문자열이 같은지로 정해집니다.** `getProduct(42)`는 `product::42`, `getRanking("food", 10)`은 `ranking::SimpleKey [food, 10]`이 됩니다.

### 그 밖에 이 문서에서 쓰는 용어

- **SpEL**: `@Cacheable`의 `key`, `condition`, `unless`에 문자열로 적는 Spring 표현식입니다. `#id`는 인자 `id`, `#result`는 메서드의 반환값을 가리킵니다. 실행 중에 평가되므로 오타나 잘못된 참조는 컴파일 때가 아니라 호출 때 드러납니다.
- **직렬화**: 객체를 Redis에 넣을 수 있는 바이트로 바꾸는 일이고, 역직렬화는 그 반대입니다. Spring Boot 자동 설정의 Redis 캐시는 기본으로 Java 직렬화를 씁니다. 직렬화기를 고르는 기준은 [직렬화기 고르기](./serializer.md)에 있습니다.
- **TTL**: 키가 사라질 때까지 남은 시간입니다. 캐시에 쓸 때 설정한 기간으로 시작하며, Redis는 쓴 순간에 이 기간을 더한 만료 시각을 키에 기록해 둡니다. 만료와 메모리 한도에 의한 축출의 차이는 [TTL과 축출](./ttl-and-eviction.md)에 있습니다.
- **캐시 매니저**: 캐시 이름(`product`, `ranking`)을 받아 실제 캐시 객체를 내주는 빈입니다. 예시에서는 Spring Boot가 만든 `RedisCacheManager`이고, TTL·null 허용·키 접두사·직렬화기 같은 설정을 캐시 이름마다 들고 있습니다.

## 한 번의 호출에서 일어나는 일

기본 설정(`sync = false`)에서 프록시가 `productService.getProduct(42)`를 받으면 다음 순서로 처리합니다. 화살표는 실행 순서입니다.

```text
컨트롤러 ──▶ 프록시
              ① condition 평가 (메서드 실행 전, #result 없음)
                 └ false면 캐시를 전혀 쓰지 않고 메서드만 실행
              ② 키 만들기: 42 → "product::42"
              ③ Redis GET product::42
                 ├ 값이 있음(히트) → 역직렬화해서 돌려줌. 메서드는 실행하지 않음
                 └ 없음(미스) → ④ 원래 객체의 getProduct(42) 실행 (DB 20ms)
                               ⑤ unless 평가 (메서드 실행 후, #result 있음)
                                  └ true면 넣지 않음
                               ⑥ 직렬화 → Redis SET product::42 <값> (TTL 10분)
                               ⑦ 반환값을 컨트롤러에 돌려줌
```

③과 ⑥에서 난 예외는 모두 `CacheErrorHandler`라는 처리기를 거칩니다. 이 처리기가 예외를 다시 던지는지 삼키는지에 따라 Redis 장애 때의 모습이 정해지며, 뒤의 "Redis가 실패하면: 기본은 DB로 넘어가지 않고 실패합니다"에서 다룹니다. 이 그림에서는 트랜잭션과의 순서, `@CacheEvict`, 통계 수집을 뺐습니다. 앞의 둘은 [Cache-aside 정합성](./cache-aside-consistency.md)의 "Spring에서: `@CacheEvict`는 커밋 전에 지울 수도 있습니다"에, 통계는 [캐시 효과 재기](./cache-effectiveness.md)의 "캐시마다 세기: Spring의 Redis 캐시 통계"에 있습니다.

이 그림에서 보듯 `@Cacheable`은 호출마다 인자로 키 하나를 만들고, 반환값 전체를 값 하나로 읽고 씁니다. 하려는 일이 "같은 인자면 메서드 결과를 통째로 다시 쓴다"로 표현되면 `@Cacheable`이 맞습니다. 반대로 Redis 명령을 직접 골라야 하는 일은 이 그림에 들어가지 않으므로 `RedisTemplate`으로 씁니다. 상품마다 따로 저장한 키 100개를 명령 한 번으로 읽기, Hash(키 하나 아래 필드마다 값을 따로 두는 Redis 자료형)에 나눠 저장한 값에서 한 필드만 고치기, 조회 수 증가처럼 "읽고 더해 다시 쓰기" 사이에 다른 요청이 끼어들면 안 되어 Redis가 명령 하나로 처리해야 하는 일이 그렇습니다. 대가는 키 규칙, 미스 때 DB를 읽어 채우기, 언제 지울지, 실패했을 때 어떻게 할지를 직접 쓰고 지켜야 한다는 것입니다. 직렬화는 `RedisTemplate`이 해 주며, 따로 지정하지 않으면 Java 직렬화를 씁니다. 그 기본값을 그대로 둘지 바꿀지는 직접 판단합니다.

## 캐시하지 않는 경우: 호출이 프록시를 지나지 않습니다

`@Cacheable`이 붙어 있는데 매번 DB로 가는 메서드라면, 먼저 그 호출이 프록시를 지났는지, 프록시가 그 메서드를 가로채는지 의심합니다. 이 절의 경우 중 자기 호출, `private` 메서드, 시작 중의 호출, `condition`에 `#result`를 쓴 경우는 **에러도 경고도 없이 메서드가 그냥 실행됩니다.** 그래서 "캐시가 잘 안 맞는다"가 아니라 "이 경로는 캐시를 한 번도 안 쓴다"는 상태가 오래 숨어 있습니다.

### 같은 클래스 안에서 부르면 캐시를 거치지 않습니다

예시의 `getProductPage(42)`는 컨트롤러가 프록시로 부르지만, `getProductPage`에는 애너테이션이 없으므로 프록시는 원래 객체에 그대로 넘깁니다. 그 안에서 부르는 `getProduct(42)`는 원래 객체의 `this`를 통한 호출이라 프록시가 끼어들 자리가 없습니다. 그래서 상품 상세 API는 캐시를 쓰는데, 같은 상품의 상품 페이지 API는 매번 DB에 20ms를 씁니다. Spring 참조 문서도 프록시 방식에서는 바깥에서 프록시로 들어온 호출만 가로채며, 자기 호출(self-invocation)은 `@Cacheable`이 붙어 있어도 캐시하지 않는다고 적습니다.

대응은 두 가지입니다.

- **캐시하는 메서드를 다른 빈으로 옮깁니다(권장).** 예를 들어 `ProductReader` 빈에 `getProduct`를 두고 `ProductService`가 그것을 주입받아 부르면, 호출이 `ProductReader`의 프록시를 지납니다. 대가는 클래스가 하나 늘어난다는 것뿐이고, 동작을 코드 구조만 보고 알 수 있습니다.
- **AspectJ 방식으로 바꿉니다.** `@EnableCaching(mode = AdviceMode.ASPECTJ)`는 프록시 대신 클래스의 바이트코드를 고쳐 모든 호출을 가로챕니다. 자기 호출도, `private` 메서드도 캐시합니다. 대가는 빌드나 클래스 로딩 단계에 위빙(바이트코드를 고치는 작업) 설정이 따로 필요하다는 것입니다. 이 하나를 위해 바꾸기에는 비용이 큽니다.

### `private` 메서드, 열리지 않은 Kotlin 메서드

- **`private` 메서드는 가로채지 못합니다.** CGLIB 프록시는 메서드를 재정의해서 가로채는데, `private` 메서드는 재정의할 수 없으므로 붙인 `@Cacheable`이 에러 없이 무시됩니다. `protected` 메서드는 확인한 구현에서는 재정의할 수 있어 가로챕니다(`@EnableCaching`이 `public`이 아닌 메서드의 애너테이션도 읽도록 설정합니다). 다만 같은 버전의 Spring 참조 문서는 프록시 방식에서 `public` 메서드에만 붙이라고 적고 있어, 문서와 구현이 어긋납니다. 문서가 보장하는 범위인 `public`에만 붙이는 것이 안전합니다.
- **Kotlin은 클래스와 메서드가 기본으로 `final`입니다.** 상속해서 재정의할 수 없으므로 CGLIB 프록시가 가로채지 못합니다. 보통은 `kotlin-spring` 컴파일러 플러그인이 이 문제를 없앱니다. 이 플러그인은 `@Component`(그리고 `@Service`처럼 `@Component`가 붙은 애너테이션), `@Transactional`, `@Cacheable` 등이 **클래스에** 붙어 있으면 그 클래스와 멤버를 모두 `open`으로 컴파일합니다. 예시의 `ProductService`는 `@Service`이므로 열립니다.
  - 플러그인이 없거나 클래스에 해당 애너테이션이 없으면(예: `@Bean` 메서드로 등록한 일반 클래스의 메서드에만 `@Cacheable`을 붙인 경우), 클래스가 `final`이라 Spring이 프록시를 만들지 못해 **시작할 때 실패**합니다. 클래스는 `open`인데 메서드만 `final`이면 프록시는 만들어지지만 그 메서드는 가로채지 못합니다. 이때 Spring은 디버그 수준의 로그를 남기고(그 메서드가 인터페이스 메서드를 구현한 경우에는 경고 로그도 남깁니다), 호출은 원래 객체가 아닌 프록시 객체 자신에서 실행됩니다. 프록시는 원래 객체와 따로 만들어진 객체이고(Spring은 의존성을 주입해 초기화를 마친 원래 객체를 프록시로 감쌉니다), 기본 방식에서는 생성자도 거치지 않고 만들어집니다. 그래서 프록시 자신에서 실행된 메서드가 보는 필드는 비어 있어 `NullPointerException`이 날 수도 있습니다.

### 시작하는 동안의 호출과 `@PostConstruct` 예열

캐시 가로채기는 애플리케이션의 모든 싱글턴 빈이 만들어진 뒤에야 켜집니다. 그 전의 호출은 프록시를 지나더라도 캐시 처리 없이 메서드만 실행합니다. 그래서 `@PostConstruct`에서 인기 상품을 미리 불러 캐시를 채우려 하면 **DB만 읽고 캐시에는 아무것도 넣지 않습니다.** Spring 참조 문서도 초기화 코드에서 이 기능에 기대지 말라고 적습니다. 예열은 애플리케이션 시작이 끝난 뒤(예: `ApplicationReadyEvent`를 받은 뒤)에 합니다.

### `condition`에 `#result`를 쓰면 한 번도 캐시하지 않습니다

`condition`은 위 그림의 ①, **메서드를 실행하기 전**에 평가합니다. 그때는 반환값이 없으므로 `#result`는 `null`로 평가됩니다. 그래서 "null이 아닐 때만 캐시하자"는 뜻으로 `condition = "#result != null"`을 쓰면 조건이 항상 거짓이 되어, 캐시를 읽지도 쓰지도 않고 매번 메서드를 실행합니다. 반환값으로 거르려면 실행 **후**에 평가하는 `unless`를 씁니다(`unless = "#result == null"`). 둘은 이름이 비슷하지만 하는 일이 다릅니다.

| 속성 | 평가 시점 | `#result` | 결과가 참/거짓일 때 |
| --- | --- | --- | --- |
| `condition` | 메서드 실행 전 | 쓸 수 없음(`null`) | 거짓이면 캐시를 **읽지도 쓰지도 않음** |
| `unless` | 메서드 실행 후(미스일 때) | 반환값 | 참이면 **쓰지만 않음**. 읽기는 이미 했음 |

### 알아채는 방법

아래 단서는 후보를 좁혀 줄 뿐, 하나만으로 원인을 확정하지는 않습니다. `condition` 탈락, `unless`, 캐시 쓰기 실패, 만료나 삭제도 같은 모습을 만들기 때문입니다.

- **캐시 통계에서 그 캐시의 조회 수를 봅니다.** 같은 서버, 같은 시간대에서 상품 페이지 API 요청이 초당 수백 번인데 `product` 캐시의 `cache.gets`가 상품 상세 API의 요청 수만큼만 늘면, 상품 페이지 경로가 캐시를 거치지 않는다고 의심할 수 있습니다. 통계는 기본으로 꺼져 있고, 켜더라도 Actuator 지표에는 시작할 때 이미 있는 캐시만 묶입니다. 예시처럼 첫 사용 때 만들어지는 캐시는 `spring.cache.cache-names`로 미리 만들어야 지표가 생깁니다([캐시 효과 재기](./cache-effectiveness.md)).
- **로그 수준을 올려 봅니다.** `org.springframework.cache` 로거를 `TRACE`로 두면 가로챈 호출에서 계산한 키와 히트·미스 여부가 남습니다(`condition`이 거짓이면 남지 않습니다). 의심하는 API를 한 번 불렀는데 이 로그가 전혀 없으면 프록시를 지나지 않았을 가능성이 큽니다.
- **테스트로 확정합니다.** 실제 진입점에서 같은 인자로 두 번 불러, 두 번째 호출에서 DB 쿼리가 나가지 않는지 봅니다. 캐시를 비운 상태에서 시작하고 `condition`·`unless`에 걸리지 않는 인자를 쓰면 원인이 프록시로 좁혀집니다. 통합 테스트로 고정해 두면 리팩터링으로 자기 호출이 생기는 것을 잡을 수 있습니다.

## 엉뚱한 값을 주는 경우: 키가 메서드를 구분하지 않습니다

### 같은 캐시 이름을 쓰는 두 메서드는 같은 키를 씁니다

예시에서 나중에 추가된 `getProductSummary(id)`도 `@Cacheable("product")`입니다. 기본 키는 인자 하나인 `42` 자체이고 메서드 이름은 들어가지 않으므로, 두 메서드 모두 Redis 키 `product::42`를 씁니다.

```text
① getProduct(42)        미스 → ProductDetail 저장   product::42 = ProductDetail
② getProductSummary(42) 히트 → ProductDetail을 받음  (ProductSummary를 기대)
```

②의 호출자는 `ProductSummary`를 기대했는데 `ProductDetail`이 돌아오므로 형 변환 예외(`ClassCastException`)가 납니다. 어느 쪽이 먼저 캐시를 채웠는지에 따라 실패하는 API가 바뀌고, TTL이 지나면 저절로 사라졌다가 다시 나타나므로 재현이 어렵습니다. 두 메서드가 **같은 타입**을 돌려주되 내용이 다르면(예: 하나는 재고를 채우고 하나는 비워 둠) 예외도 없이 틀린 값이 나갑니다.

대응은 다음 순서로 고릅니다.

- **메서드마다 캐시 이름을 따로 둡니다(권장).** `product`와 `productSummary`로 나누면 키가 `product::42`와 `productSummary::42`로 갈리고, 뒤에서 볼 TTL도 캐시마다 따로 정할 수 있습니다.
- **키에 구분자를 넣습니다.** `key = "'summary:' + #id"`처럼 SpEL로 키를 정합니다. 캐시 이름을 바꿀 수 없을 때 씁니다. 같은 캐시를 지우는 `@CacheEvict`도 같은 키 식을 써야 하므로, 키를 바꾸는 곳이 여러 군데로 늘어납니다.
- **메서드 이름을 넣는 키 생성기를 씁니다.** 예시의 `getProduct`와 `getProductSummary`처럼 이름이 다른 메서드끼리의 충돌은 사라집니다. 다른 클래스의 같은 이름 메서드나 인자만 다른 오버로드, 뒤에서 볼 문자열 변환 충돌은 그대로 남습니다. 그리고 `@CacheEvict`가 붙은 메서드(예: `updateProduct`)는 이름이 달라 같은 키를 만들지 못합니다. 지우는 쪽에는 결국 키를 직접 적어야 합니다.

### Redis 캐시에서는 문자열이 같으면 같은 키입니다

배경 지식에서 본 것처럼 Redis 캐시는 키를 문자열로 바꾼 뒤 비교합니다. 그래서 Kotlin에서는 서로 다른 값인데 문자열이 같으면 충돌합니다.

- `product` 캐시에 `getProduct(42L)`와 상품 코드로 찾는 `getProductByCode("42")`가 함께 있으면, `Long` 42와 `String` "42"는 `equals`로는 다르지만 둘 다 `product::42`가 됩니다. 같은 캐시 이름을 메서드끼리 나눠 쓰지 말아야 하는 이유가 하나 더 생깁니다.
- 인자가 여럿이면 `SimpleKey [food, 10]`처럼 각 인자의 `toString()`을 이어 붙입니다. 인자 하나가 쉼표나 대괄호를 담은 문자열이면 다른 인자 조합과 같은 문자열이 될 수 있습니다. 예시의 `category`처럼 서비스가 정한 값이면 문제가 없지만, 사용자가 입력한 문자열을 그대로 여러 인자 중 하나로 쓴다면 키를 직접 정합니다.

### 키 객체의 `toString()`이 키가 됩니다

검색 조건을 Kotlin `data class ProductQuery(val categoryId: Long, val sort: String)`로 받아 `@Cacheable("search")`를 붙이면, 키는 `data class`가 만들어 주는 `toString()`, 곧 `search::ProductQuery(categoryId=7, sort=price)`입니다. 여기서 두 가지가 따라옵니다.

- **주 생성자에 프로퍼티를 하나 추가하면 모든 키가 바뀝니다.** `data class`가 만들어 주는 `toString()`에는 주 생성자의 프로퍼티가 모두 들어가기 때문입니다(클래스 본문에 선언한 프로퍼티는 빠집니다). `ProductQuery`의 주 생성자에 `page`를 더한 배포 직후에는 모든 검색이 미스가 되어 DB로 갑니다([캐시 스탬피드](./cache-stampede.md)의 상황). 옛 키는 아무도 읽지 않는 채 TTL까지 메모리에 남고, TTL이 없는 캐시라면 지울 때까지 남습니다.
- **일반 `class`는 키로 쓸 수 없습니다.** `toString()`을 직접 정의하지 않은 클래스는 Redis 캐시가 문자열로 바꾸지 못해 예외를 던집니다. 이 예외도 캐시 접근 중의 예외라 에러 처리기를 거치므로, 처리기를 바꿔 두었다면 경고 로그만 남고 **그 메서드는 한 번도 캐시되지 않습니다.**

키 객체를 넘기기보다 `key = "#query.categoryId + ':' + #query.sort"`처럼 캐시에 의미 있는 필드만 골라 키를 정하면, 필드를 추가해도 키가 바뀌지 않습니다. 대가는 키에 넣어야 할 필드를 빠뜨리면 다른 조건의 결과를 같은 키로 돌려준다는 것입니다. 필드를 추가할 때 키 식도 함께 검토해야 합니다.

### 결과를 바꾸는 값이 키에 없으면 다른 사용자의 응답이 나갑니다

키에 넣어야 하는 것은 "메서드 인자"가 아니라 **"이 결과를 정하는 값 전부"**입니다. 둘이 다른 경우가 더 위험합니다. 예시의 `getProduct(id)`가 회원가를 보여 주려고 로그인 사용자의 회원 등급을 인자가 아닌 보안 컨텍스트에서 읽는다고 합시다. 키는 여전히 `product::42`이므로 다음 일이 생깁니다.

```text
① VIP 회원이 getProduct(42)  미스 → VIP 회원가로 만든 ProductDetail 저장   product::42
② 비회원이 getProduct(42)    히트 → VIP 회원가를 받음 (10분 동안 모든 사용자가 같음)
```

앞 절의 메서드 사이 충돌은 반환 타입이 다르면 예외라도 나지만, 이 경우는 같은 메서드의 결과라 타입이 항상 같으므로 **예외 없이 틀린 값이 나갑니다.** 값이 권한에 따라 달라지면(예: 관리자에게만 원가를 채움) 틀린 값이 아니라 정보 노출 사고가 됩니다. 인자 밖에서 결과를 바꾸는 값은 보통 다음과 같습니다. 캐시를 붙이기 전에 메서드 본문과 그 메서드가 부르는 코드에서 이런 값을 읽는 곳이 있는지 확인합니다.

- 로그인 사용자, 회원 등급, 권한(보안 컨텍스트에서 읽음)
- 언어·통화·지역(요청 헤더나 `LocaleContextHolder`에서 읽음)
- 기능 플래그나 A/B 실험 그룹, 앱 버전에 따라 달라지는 응답 모양

대응은 다음 순서로 고릅니다.

- **모두에게 같은 부분만 캐시하고, 달라지는 부분은 캐시 밖에서 덧입힙니다(권장).** 예시라면 상품 상세는 `product::42` 하나로 캐시하고, 회원가와 찜 여부는 캐시에서 꺼낸 값에 요청마다 계산해 붙입니다. 키 수가 늘지 않고 한 번 만든 값을 모든 사용자가 다시 씁니다. 대가는 덧입히는 계산이 요청마다 실행된다는 것입니다. 그 계산이 DB를 읽는다면 그 부분은 캐시의 덕을 보지 못합니다.
- **결과를 정하는 값을 인자로 받아 키에 넣습니다.** 이때 그 결과를 정하는 **가장 거친 값**을 넣습니다. 회원가가 등급으로만 정해진다면 사용자 식별자가 아니라 등급(예: 4가지)을 넣습니다. 등급을 넣으면 키가 상품 수 × 4가지로 늘지만, 사용자 식별자를 넣으면 상품 수 × 사용자 수까지 늘 수 있고 같은 키가 다시 읽힐 일도 드뭅니다(뒤의 "키가 끝없이 늘어나는 경우: 다시 읽히는 조합만 캐시합니다"). 언어처럼 가짓수가 적은 값도 같은 이유로 키에 넣기 부담이 적습니다.
- **키 생성기나 SpEL에서 보안 컨텍스트를 직접 읽게 하는 방법은 권하지 않습니다.** 동작은 하지만 메서드 시그니처만 보고는 키에 무엇이 들어가는지 알 수 없고, 같은 키를 지워야 하는 `@CacheEvict` 쪽(예: 관리자가 가격을 바꾸는 메서드)은 그 사용자의 컨텍스트에서 실행되지 않으므로 모든 등급의 키를 지우도록 따로 맞춰야 합니다. 인자로 받으면 이 의존이 코드에 드러납니다.

키 식을 직접 쓸 때 결과를 바꾸는 필드를 빠뜨리는 것도 같은 결과를 냅니다(앞 절의 `ProductQuery`). 그래서 "이 결과를 바꾸는 값이 모두 키에 있는가"는 캐시를 처음 붙일 때와, 메서드가 새 값을 읽게 바뀔 때마다 확인합니다.

### SpEL에서 인자 이름이 보이지 않으면 키가 `null`이 됩니다

`key = "#id"`는 인자 이름을 실행 중에 알아야 동작합니다. Kotlin은 `kotlin-reflect`가 있으면 Spring이 Kotlin 메타데이터에서 이름을 읽으므로 보통 문제가 없습니다. Java 코드는 `-parameters` 옵션으로 컴파일해야 이름이 남습니다. 이름을 찾지 못하면 `#id`는 `null`이 되고, Spring은 "Null key returned"로 시작하는 `IllegalArgumentException`을 던집니다. 이름 대신 위치로 `#p0`(또는 `#a0`)라고 쓰면 이 문제를 피할 수 있습니다.

### 다른 앱과 같은 Redis를 쓰면 캐시 이름이 겹칩니다

예시의 관리자 앱도 같은 Redis에 `product`라는 캐시를 씁니다. 기본 키 접두사는 `캐시이름::`뿐이라 두 앱이 같은 `product::42`를 읽고 씁니다. 관리자 앱이 저장한 클래스를 상품 API가 갖고 있지 않으면 역직렬화에 실패하고([직렬화기 고르기](./serializer.md)의 "장애는 쓸 때가 아니라 읽을 때 드러납니다"), 그 클래스를 갖고 있더라도 메서드의 반환 타입과 다르면 형 변환 예외가 납니다. 같은 클래스라도 한쪽의 `@CacheEvict`가 다른 쪽 캐시를 지웁니다.

Spring Boot의 `spring.cache.redis.key-prefix`를 정하면 키가 `접두사 + 캐시이름 + ::`이 됩니다. 앱마다 다른 접두사(예: `shop:`, `admin:`)를 두면 `shop:product::42`와 `admin:product::42`로 갈립니다. `spring.cache.redis.use-key-prefix=false`는 접두사를 아예 없애 키가 `42`만 남으므로, 캐시가 여럿이거나 다른 키와 섞이는 Redis에서는 쓰지 않습니다. 개발·스테이징 환경이 Redis 하나를 나눠 쓰는 경우도 같은 문제라, 환경마다 Redis를 나누지 못한다면 접두사에 환경도 넣습니다(예: `shop:stg:`).

접두사는 읽는 키만이 아니라 **캐시를 통째로 비울 때 지우는 범위**도 정합니다. `@CacheEvict(allEntries = true)`는 키 하나가 아니라 캐시 전체를 비우는데, Redis 캐시는 이것을 "그 캐시의 키 모양에 맞는 키를 모두 찾아 지우기"로 처리합니다. 기본 설정에서는 `KEYS product::*`로 맞는 키를 모두 찾은 뒤 `DEL`로 지웁니다.

- **접두사가 앱마다 다르지 않으면 다른 앱의 캐시까지 지웁니다.** 관리자 앱의 "캐시 비우기"가 `product::*`를 지우면 상품 API의 `product::42`도 함께 사라집니다. 앱마다 접두사를 두면 패턴이 `admin:product::*`가 되어 자기 키만 지웁니다.
- **`use-key-prefix=false`면 패턴이 `*`가 되어 그 Redis 데이터베이스의 모든 키를 지웁니다.** 캐시 키가 아닌 세션이나 재고 같은 키도 포함됩니다. Spring Data Redis도 접두사를 끌 때는 전용 Redis를 쓰라고 안내합니다.
- **`KEYS`는 키가 많을수록 Redis 전체를 오래 막습니다.** `KEYS`는 패턴에 맞는 키만이 아니라 데이터베이스의 모든 키를 훑고, 그동안 Redis는 다른 명령을 처리하지 않습니다. 공식 문서도 운영 환경에서는 극히 조심해서 쓰라고 경고합니다. 캐시 키가 많은 Redis라면 캐시 매니저의 쓰기 객체를 `RedisCacheWriter.nonLockingRedisCacheWriter(connectionFactory, BatchStrategies.scan(1000))`처럼 `SCAN`으로 나눠 훑는 방식으로 바꿀 수 있습니다(빌더의 `cacheWriter(...)`). Spring Data Redis는 이 방식을 드라이버나 Redis 운영 방식에 따라 지원하지 않을 수 있다고 적고 있어, 쓰는 구성에서 동작하는지 확인합니다. 대가는 왕복이 여러 번으로 늘고, 비우는 도중 새로 들어온 키는 남을 수 있다는 것입니다. Redis가 명령을 하나씩 처리해서 생기는 이 문제와 `SCAN`의 대가는 [이벤트 루프](./event-loop.md)의 "무엇이 메인 스레드를 오래 잡는가"에 있습니다.

### 알아채는 방법

- **실제 키를 봅니다.** `redis-cli --scan --pattern 'product::*'`로 키 모양을 확인합니다. `SimpleKey [...]`나 `ProductQuery(...)`처럼 의도하지 않은 모양이 보이면 키 식을 다시 봅니다. 운영 Redis에서는 `--scan`을 쓰고 `KEYS`는 쓰지 않습니다.
- **`ClassCastException`이나 역직렬화 예외가 특정 시간대에만 나면** 같은 키를 쓰는 다른 메서드나 다른 앱을 의심합니다. `TRACE` 로그의 계산된 키를 두 경로에서 비교하면 확인됩니다.
- **다른 사용자의 값이 보인다는 문의가 오면** 그 키를 `redis-cli --scan`으로 찾아 키에 사용자·등급·언어 구분이 있는지 보고, 메서드가 인자 밖에서 그런 값을 읽는지 코드에서 찾습니다.

## 키가 끝없이 늘어나는 경우: 다시 읽히는 조합만 캐시합니다

앞 절은 키에 넣을 값이 **모자라서** 틀린 값이 나가는 문제였습니다. 반대로 키에 가짓수가 끝없는 값이 들어가면 값은 맞지만 캐시가 일을 하지 않습니다.

### 히트는 채운 키가 사라지기 전에 다시 조회되어야 생깁니다

히트는 조회한 키에 값이 살아 있을 때만 생깁니다. 비어 있는 키를 처음 조회하면 미스이고, 그 미스로 채운 값이 TTL로 사라지기 전에(그 사이 삭제나 축출이 없다면) **같은 키가 다시 조회되어야** 히트가 납니다. 그래서 TTL 동안 새로 만들어진 키가 몇 가지인지가 미스의 하한, 곧 히트의 상한을 정합니다. 새 키 가짓수가 조회 수에 가까우면, 거의 모든 조회가 그 키의 첫 조회라 미스입니다. 반대로 가능한 가짓수가 끝없더라도 실제 요청이 소수의 인기 검색어에 몰리면 히트는 날 수 있습니다. 문제가 되는 것은 가짓수가 크고 **실제 요청이 넓게 퍼지는** 인자이며, 어느 쪽인지는 서비스의 요청 분포를 재 봐야 압니다.

예시 서비스에서 이런 키가 생길 수 있는 인자는 셋입니다. 아래 숫자는 설명을 위한 가상 수치입니다. 10분 동안을 빈 캐시에서 시작해 보고, 저장 실패와 같은 키의 동시 미스는 없다고 가정합니다. 메모리는 값 크기 2KB만 곱한 값이며, 키 이름과 Redis의 관리용 구조는 뺐습니다.

| 캐시하려는 메서드 | 키에 들어가는 값 | 10분(TTL) 동안 | 다시 읽히는가 |
| --- | --- | --- | --- |
| `getProduct(id)` | 상품 번호(상품 1만 개) | 조회 60만 번, 키는 많아야 1만 개(약 20MB) | 인기 상품은 TTL 안에 수없이 다시 읽힘 |
| 상품 검색 `search(ProductQuery)`에 자유 검색어 `keyword` 추가 | 검색어 × 카테고리 × 정렬 × 페이지 | 검색 6만 번 중 서로 다른 조합 5만 가지 | 많아야 1만 번(6만 − 5만)만 히트. 적중률 상한 약 17% |
| 사용자별 상품 상세 `getProductForUser(userId, id)` | 사용자 × 상품 | 조회 60만 번이 거의 모두 다른 (사용자, 상품) 쌍 | 같은 사용자가 같은 상품을 10분 안에 다시 볼 때만 히트. 키 약 60만 개(약 1.2GB) |

같은 상품 상세 조회 60만 번이라도, 키를 상품 번호로 두면 새 키가 많아야 1만 개라 미스는 1만 번 근처에 머물 수 있는데, 사용자 식별자를 더하면 키가 60배로 늘고 히트는 거의 없습니다. 페이지 번호도 같은 방향으로 작용합니다. 앞쪽 페이지는 여러 사람이 다시 읽지만, 뒤쪽 페이지일수록 TTL 안에 다시 읽힐 일이 드뭅니다.

### 키가 늘면 메모리, 적중률, 다른 캐시가 함께 나빠집니다

- **메모리**: 모든 키에 같은 TTL이 붙고 저장 속도가 일정하다면, 살아 있는 키 수는 어림잡아 "초당 저장 수 × TTL"을 넘지 않습니다. 여기서 초당 저장 수는 지금 이 순간이 아니라 지난 TTL 동안의 평균입니다. 다시 읽히지 않는 키는 미스마다 새로 하나씩 저장되므로, 이 어림값이 거의 그대로 채워집니다. 위 표의 사용자별 상품 상세가 1.2GB를 쓰는 이유입니다. TTL이 없는 캐시라면 상한도 없습니다(뒤의 "너무 오래 두는 경우: TTL은 캐시 매니저 설정에서 옵니다").
- **적중률**: 같은 요청량이 더 많은 키로 나뉘므로 키 하나에 오는 조회가 줄고, 그 키가 TTL 안에 다시 읽힐 확률도 줄어듭니다. 키 하나에 오는 초당 조회와 TTL로 적중률을 어림하는 방법은 [캐시 효과 재기](./cache-effectiveness.md)의 "바꾸기 전에 예상을 계산해 둡니다"에 있습니다. 미스 한 번에는 DB 조회 앞뒤로 Redis 조회와 `SET` 왕복이 더해지므로, 히트를 거의 내지 못하는 캐시는 DB 부하를 줄이지 못한 채 이 일만 늘립니다. 그만큼 지연이 얼마나 늘지는 환경에 따라 다르므로 재서 확인합니다.
- **같은 Redis의 다른 키**: 메모리 한도(`maxmemory`)에 닿으면 축출 정책이 무엇을 할지 정합니다. 키를 지우는 정책이면 한 번 쓰이고 버려질 키를 넣으려고 인기 상품 키 같은 다른 키도 밀려날 수 있어, 멀쩡하던 `product` 캐시의 적중률까지 떨어집니다. Redis 기본 정책(`noeviction`)이면 지우지 않는 대신 쓰기가 OOM 에러를 받습니다. 그러면 위 그림의 ⑥ 쓰기가 실패하고, 기본 에러 처리기에서는 **DB를 이미 읽은 요청이 실패합니다.** 정책별 동작은 [TTL과 축출](./ttl-and-eviction.md)의 "지우지 못하면: 쓰기 명령이 OOM 에러를 받습니다"에 있습니다.

### 무엇을 캐시할지 고르는 기준

캐시를 붙이기 전에 세 가지를 봅니다.

1. **키 가짓수에 상한이 있는가.** 상품 번호(상품 수만큼), 카테고리 × 정렬처럼 서비스가 정한 값의 조합은 상한이 있습니다. 자유 검색어, 사용자 식별자, 깊은 페이지 번호, 시각이나 좌표처럼 연속적인 값은 사실상 상한이 없습니다.
2. **같은 키가 TTL 안에 다시 오는가.** 상한이 있어도 키마다 조회가 드물면 히트가 나지 않습니다. 캐시가 이미 있다면 그 캐시의 초당 히트 ÷ 초당 저장으로 저장 한 번이 몇 번 읽히는지 봅니다([캐시 효과 재기](./cache-effectiveness.md)의 "저장 한 번이 몇 번 읽히는가").
3. **미스 한 번이 얼마나 비싼가.** 예시의 `getRanking`(800ms)처럼 비싸다면 적중률이 낮아도 아끼는 DB 시간이 클 수 있고, `getProductSummary`(5ms)처럼 싸다면 캐시가 더하는 왕복과 메모리가 이득보다 클 수 있습니다.

상한이 없거나 다시 오지 않는 인자가 섞여 있다면, 메서드 전체를 캐시하는 대신 다음 중에서 고릅니다.

- **반복되는 범위만 캐시합니다.** `condition = "#query.keyword == null && #query.page < 3"`처럼 검색어가 없는 카테고리 목록의 앞쪽 페이지만 캐시하고 나머지는 매번 DB로 보냅니다. 대가는 빠진 요청이 캐시의 덕을 보지 못한다는 것입니다. 이것은 키 식에서 결과를 바꾸는 필드를 빼는 일과 다릅니다. 필드를 빼면 틀린 값이 나가고, 범위를 좁히면 값은 맞되 빠진 요청이 매번 DB로 갈 뿐입니다.
- **결과를 바꾸지 않는 차이를 키에서 없앱니다.** 검색어 앞뒤 공백처럼 조회 결과를 바꾸지 않는 차이는 메서드를 부르기 전에 인자를 정리해 두면 같은 키로 모입니다. 대소문자처럼 조회 방식에 따라 결과가 달라질 수 있는 차이는, 실제 조회가 그 둘을 같게 다룰 때만 합칩니다. 다르게 다루는데 합치면, 결과를 바꾸는 값을 키에서 빠뜨린 것과 같아 틀린 값이 나옵니다.
- **모두가 다시 쓰는 조각을 캐시하고 조합은 앱에서 합니다.** 사용자별 상품 상세 대신 상품 상세(`product::42`)를 캐시하고 사용자별 부분을 덧입히는 것(앞 절의 권장 대응)이 이것입니다. 순위처럼 `size`만 다른 키가 여럿이면 카테고리마다 가장 큰 목록 하나를 캐시하고 앱에서 잘라 주는 방법도 같은 생각이며, 그 계산은 [캐시 효과 재기](./cache-effectiveness.md)의 "바꾸기 전에 예상을 계산해 둡니다"에 있습니다.
- **캐시하지 않습니다.** 자유 검색어 검색처럼 조합이 끝없고 다시 오지 않는 조회는 캐시보다 DB 인덱스나 검색 엔진으로 미스 비용 자체를 줄이는 쪽이 맞을 수 있습니다.

### 알아채는 방법

- **캐시마다 초당 히트와 초당 저장을 비교합니다.** 이 비율은 저장 한 번이 평균 몇 번 읽혔는지를 보여 줄 뿐, 키 하나하나가 다시 읽혔는지는 알려 주지 않습니다. 그래도 히트가 저장보다 적다면 저장 한 번이 평균 한 번도 읽히지 않는다는 뜻이라, 상한 없는 인자가 키에 들어갔는지 의심할 근거가 됩니다. Spring의 캐시 통계를 켜야 캐시마다 셀 수 있습니다([캐시 효과 재기](./cache-effectiveness.md)).
- **`INFO keyspace`의 `keys`가 요청량을 따라 계속 늘면** 상한 없는 인자가 키에 들어갔는지 봅니다. 그 캐시의 키를 `redis-cli --scan --pattern 'search::*'`로 몇 개 뽑아 보면 어떤 인자가 가짓수를 키우는지 보입니다.
- **`INFO stats`의 `evicted_keys`가 늘기 시작하면** 메모리 한도에 닿아 축출이 일어나고 있다는 뜻입니다. 그 시점부터 다른 캐시의 적중률도 함께 떨어지는지 봅니다.

## 무엇을 캐시에 넣는가: null, `unless`, 넣기 실패

### null도 기본으로 캐시합니다

예시의 `getProduct(999999)`는 `null`을 돌려줍니다. Spring Data Redis의 기본 설정은 null을 허용하므로, null을 뜻하는 표식 값이 `product::999999`에 들어갑니다. 이것이 왜 좋기도 하고 문제이기도 한지(없는 번호 공격, 새 상품이 늦게 보임, 기본 TTL이 무기한일 때의 문제)는 [캐시 스탬피드](./cache-stampede.md)의 "Spring에서는 기본값이 \"없음도 캐시\"입니다"에 있습니다. 여기서는 null 캐시를 끌 때 생기는 일만 봅니다.

### null 캐시를 끄고 `unless`를 빠뜨리면, DB는 읽었는데 요청이 실패합니다

`spring.cache.redis.cache-null-values=false`로 null 캐시를 끄면, 메서드가 null을 돌려줄 때 Redis 캐시는 "null을 허용하지 않는다"는 `IllegalArgumentException`을 던집니다. 이 예외는 위 그림의 ⑥, 곧 **메서드가 이미 DB를 읽은 뒤**에 납니다.

- 기본 에러 처리기에서는 이 예외가 그대로 올라가 요청이 실패합니다. DB 조회는 성공했는데 사용자는 "없음" 응답 대신 오류를 받습니다(어떤 상태 코드가 되는지는 앱의 예외 처리가 정합니다).
- 에러 처리기를 바꿔 예외를 삼키게 했다면 경고 로그만 남고 null이 돌려집니다. 이 경우 `999999`는 **매번 DB로 갑니다.** 에러율에는 드러나지 않습니다.

그래서 null 캐시를 끌 때는 null을 돌려줄 수 있는 모든 `@Cacheable`에 `unless = "#result == null"`을 함께 붙입니다. 반환 타입이 `Optional`이면 `#result`는 `Optional`이 아니라 안의 값을 가리키므로 같은 식을 씁니다.

### `sync = true`에는 `unless`를 쓸 수 없습니다

`sync = true`인 메서드에 `unless`를 붙이면 처음 호출할 때 `IllegalStateException`이 납니다. 시작할 때가 아니라 호출할 때 나므로, 그 메서드를 부르는 테스트가 없으면 배포 후에 드러납니다. `sync = true`는 캐시 하나만 지정할 수 있고 같은 메서드에 다른 캐시 애너테이션을 함께 쓸 수 없다는 제약도 같은 방식으로 드러납니다.

## 너무 오래 두는 경우: TTL은 캐시 매니저 설정에서 옵니다

`@Cacheable`에는 TTL을 적는 속성이 없습니다. TTL은 캐시 매니저가 캐시 이름마다 들고 있는 설정에서 옵니다. 그래서 "이 캐시가 몇 분 동안 남는가"는 애너테이션이 아니라 설정을 봐야 압니다.

### 기본은 만료 없음, Spring Boot 속성은 모든 캐시에 하나

- Spring Data Redis의 기본 설정(`RedisCacheConfiguration.defaultCacheConfig()`)은 **만료가 없습니다.**
- Spring Boot의 `spring.cache.redis.time-to-live`를 정하면 그 값이 **모든 캐시의 기본 TTL**이 됩니다. 캐시 이름마다 TTL을 정하는 속성은 없습니다.

예시는 `time-to-live=10m` 한 줄이므로 `product`도 `ranking`도 10분입니다. 상품 상세에는 10분이 적당했더라도, 판매 순위가 30초 안에 새로 보여야 하는 요구라면 순위 캐시는 최대 10분 가까이 낡은 순위를 보여 줄 수 있습니다. 반대로 TTL을 30초로 낮추면, 자주 읽히는 상품 키는 30초마다 미스가 나 DB에 다시 갑니다. 한 값으로 둘 다 맞출 수 없습니다.

### 캐시마다 TTL을 정하는 방법과 빠지기 쉬운 함정

Spring Boot를 쓰면 `RedisCacheManagerBuilderCustomizer` 빈에서 빌더의 `withCacheConfiguration("ranking", ...)`으로 캐시 이름마다 설정을 줍니다. 이때 함정이 셋 있습니다.

1. **캐시별 설정을 무엇에서 출발해 만드느냐가 중요합니다.** `RedisCacheConfiguration.defaultCacheConfig().entryTtl(30초)`로 만들면, Spring Boot 속성으로 정한 키 접두사와 null 캐시 설정이 그 캐시에서만 빠집니다. 기본값에서 새로 출발한 설정이기 때문입니다. 빌더가 이미 들고 있는 기본 설정(`builder.cacheDefaults()`)에서 출발해 TTL만 바꾸면 나머지가 유지됩니다.
2. **`RedisCacheConfiguration` 빈을 직접 등록하면 `spring.cache.redis.*`의 TTL·접두사·null 속성이 무시됩니다.** 예를 들어 직렬화기를 JSON으로 바꾸려고 이 빈을 만들면, Spring Boot는 속성으로 기본 설정을 만드는 대신 그 빈을 그대로 기본 설정으로 씁니다(통계를 켜는 `enable-statistics`만은 따로 읽어 적용합니다). 설정 파일의 `time-to-live=10m`은 그대로 남아 있는데 **실제 캐시에는 TTL이 없어집니다.** 이 빈을 만들었다면 TTL, 접두사, null 설정을 빈 안에서 다시 정해야 합니다.
3. **설정에 없는 캐시 이름은 실행 중에 기본 설정으로 만들어집니다.** 캐시별 설정에 `ranking`만 있는데 누가 `@Cacheable("rankings")`라고 오타를 내거나 새 캐시 이름을 쓰면, 캐시 매니저는 에러 없이 기본 설정으로 새 캐시를 만듭니다. 그 캐시의 TTL은 기본 TTL(속성이 없으면 만료 없음)입니다. 빌더의 `disableCreateOnMissingCache()`를 부르면 미리 정하지 않은 이름에 대해 "Cannot find cache named"로 시작하는 예외가 나므로, 이런 실수를 첫 호출에서 잡을 수 있습니다. 대가는 캐시를 추가할 때마다 설정에도 이름을 등록해야 한다는 것입니다. 2번처럼 기본 설정을 직접 만들었다면 이 방법도 함께 봅니다.

TTL을 얼마로 정할지는 "이 값이 얼마나 낡아도 되는가"로 정합니다. 그 기준과, 지워도 늦게 끝난 조회가 낡은 값을 되써 넣을 수 있어 TTL이 마지막 안전망이 되는 이유는 [Cache-aside 정합성](./cache-aside-consistency.md)에 있습니다. 같은 시각에 넣은 키가 같은 시각에 만료되는 문제와 TTL에 무작위 값을 더하는 방법은 [캐시 스탬피드](./cache-stampede.md)에 있습니다.

### 알아채는 방법

- `redis-cli TTL product::42`가 `-1`이면 그 키에는 만료가 없습니다. 캐시마다 키 하나씩 확인하면 설정이 실제로 적용됐는지 알 수 있습니다. `-2`는 키가 없다는 뜻입니다.
- `INFO keyspace`의 `keys`와 `expires`를 비교합니다. 캐시 전용 Redis인데 `expires`가 `keys`보다 눈에 띄게 적으면 TTL 없이 들어가는 키가 있습니다.
- 메모리가 계속 늘기만 하면, 키 모양이 바뀐 배포(키 객체에 필드 추가, 캐시 이름 변경) 뒤로 옛 키가 TTL 없이 남아 있는지 봅니다.

## `sync = true`는 서버 사이에서 아무것도 보장하지 않습니다

`sync = true`는 Spring이 직접 잠그는 기능이 아니라, "없으면 메서드를 불러 넣기"를 한 번의 호출로 캐시 구현에 맡기는 표시입니다. 잠글지는 캐시 구현이 정합니다. Spring Boot 자동 설정의 Redis 캐시는 잠그지 않으므로, 예시에서 상품 42가 만료된 순간 들어온 요청은 같은 서버 안에서도 각자 DB에 갑니다. 잠그는 Redis 캐시 쓰기 방식은 서버 사이에서 한 번만 조회하게 하지만, 락이 상품 키가 아니라 `product`라는 캐시 이름 하나에 걸립니다. 그래서 상품 42를 다시 만드는 동안 같은 캐시의 상품 7을 읽는 요청까지 모든 서버에서 기다립니다. 자세한 내용은 [캐시 스탬피드](./cache-stampede.md)의 "Spring의 `@Cacheable(sync = true)`는 Redis 캐시에서 이 일을 하지 않습니다"에 있습니다.

이 문서의 주제와 관련해 더할 것은 두 가지입니다.

- **`transactionAware()`를 켜도 `sync = true`의 쓰기는 커밋 뒤로 미뤄지지 않습니다.** [Cache-aside 정합성](./cache-aside-consistency.md)의 "`transactionAware()`: put과 evict를 커밋 뒤로 미룹니다"에 있습니다.
- **에러 처리기가 예외를 삼키면, 메서드가 두 번 실행될 수 있습니다.** `sync = true`에서는 읽기·메서드 실행·쓰기가 캐시 구현의 호출 하나 안에서 일어납니다. 그 호출이 메서드 실행 **뒤**에 실패하면(예: `SET`이 타임아웃되거나, 반환값을 직렬화하지 못함) Spring은 그것을 캐시 읽기 실패로 받아 처리기에 넘기고, 처리기가 삼키면 메서드를 **한 번 더** 불러 그 결과를 돌려줍니다. Redis가 느려질 때 DB 조회가 두 배가 됩니다(소스를 읽고 이 문서가 끌어낸 결론이며, 재현하지 않았습니다). `sync = false`에서는 읽기와 쓰기가 따로 처리기를 거치므로 메서드는 한 번만 실행됩니다.

## Redis가 실패하면: 기본은 DB로 넘어가지 않고 실패합니다

### 모든 캐시 접근의 예외는 에러 처리기를 거칩니다

위 그림의 ③ 읽기, ⑥ 쓰기, 그리고 `@CacheEvict`의 삭제에서 난 예외는 모두 `CacheErrorHandler`에 넘어갑니다. 예외에는 Redis 연결 실패와 명령 타임아웃뿐 아니라, 이 문서에서 본 키 변환 실패, null 거부, 직렬화 실패도 포함됩니다.

- **기본 처리기(`SimpleCacheErrorHandler`)는 그대로 다시 던집니다.** 그래서 Redis가 응답하지 않으면 `getProduct(42)`는 DB를 읽지 않고 실패합니다. "캐시는 없어도 되는 것"이라는 기대와 다르게, 기본 설정에서는 캐시 장애가 상품 API 장애가 됩니다. 값을 못 읽는 경우의 같은 동작은 [직렬화기 고르기](./serializer.md)의 "`@Cacheable`도 기본 설정에서는 예외를 던집니다"에 있습니다.
- **처리기를 바꾸면 미스로 넘어갑니다.** `CachingConfigurer` 빈의 `errorHandler()`에서 Spring이 제공하는 `LoggingCacheErrorHandler`를 돌려주면, 읽기 예외는 경고 로그를 남기고 미스로 처리되어 메서드(DB 조회)가 실행됩니다. 이어지는 쓰기도 실패하면 다시 경고 로그만 남기고 반환값을 돌려줍니다. 기본 생성자는 스택 트레이스를 남기지 않습니다.

### 넘어가더라도 "빠르게" 넘어가지는 않습니다

미스로 넘어가는 것은 Redis 명령이 **실패로 끝난 뒤**입니다. Redis가 멈춰 응답이 없으면 명령은 명령 타임아웃까지 기다린 뒤에야 실패합니다. Spring Boot에서 `spring.data.redis.timeout`을 정하지 않으면 Lettuce의 기본 60초가 적용되므로, 처리기를 바꿔도 각 요청은 읽기에서 타임아웃까지 기다린 뒤 DB를 읽고, 쓰기에서 또 기다릴 수 있습니다. 예시의 상품 API는 사실상 멈춥니다. 연결이 끊긴 것을 클라이언트가 이미 알고 있으면 바로 실패할 수도 있는데, 어느 쪽이 되는지와 타임아웃을 정하는 기준은 [Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)에 있습니다. 캐시를 DB로 넘기는 구성이라면 명령 타임아웃을 API의 응답 기한보다 충분히 짧게 정하는 것이 같은 결정의 일부입니다.

그리고 모든 요청이 미스가 되면 캐시가 막던 조회가 전부 DB로 갑니다. 그 부하를 DB가 견디는지, 견디지 못하면 무엇을 먼저 포기할지는 [캐시 스탬피드](./cache-stampede.md)의 "마지막 방어선: DB에 동시에 들어가는 수를 제한합니다"에서 다룹니다. 타임아웃, 서킷 브레이커, DB로 우회할지 빨리 실패할지를 함께 정하는 일은 Redis 장애 시 우회에서 다룹니다.

### 삭제 실패를 삼키면 낡은 값이 남습니다

`LoggingCacheErrorHandler`는 읽기·쓰기·삭제를 가리지 않고 경고 로그만 남깁니다. 상품 가격을 바꾸는 메서드의 `@CacheEvict`가 Redis 타임아웃으로 실패해도 API는 성공으로 응답합니다. 타임아웃은 앱이 응답을 받지 못했다는 뜻일 뿐이라 Redis가 실제로는 지웠을 수도 있지만, 삭제가 실행되지 않았다면 DB의 가격은 바뀌었는데 캐시에는 옛 가격이 TTL까지 남습니다. 앱은 어느 쪽인지 알 수 없습니다. 읽기 실패를 삼키는 것은 "느려지는" 문제지만, 삭제 실패를 삼키는 것은 "틀린 값을 보여 주는" 문제입니다. 그 틈이 얼마나 오래 열리는지는 [Cache-aside 정합성](./cache-aside-consistency.md)의 "삭제가 실패하면 틈이 닫히지 않습니다"에 있습니다.

그래서 처리기를 직접 만들어 읽기·쓰기 실패는 미스로 넘기되, 삭제 실패는 지표로 세고 경보를 걸거나 다시 던지는 식으로 둘을 나눠 다루는 것이 좋습니다. 어느 쪽을 택하든 "캐시 에러 로그가 0인가"를 따로 지켜봐야 합니다. 처리기가 예외를 삼키면 에러율 지표에는 아무것도 드러나지 않기 때문입니다.

## 돌려받은 객체를 고쳐도 Redis 캐시는 바뀌지 않습니다

컨트롤러가 `getProduct(42)`로 받은 `ProductDetail`의 가격 필드를 회원 할인가로 바꿔 응답한다고 합시다. 이 수정이 캐시에 남아 다른 사용자에게도 할인가가 보일까요? Redis 캐시에서는 그렇지 않습니다.

- **미스일 때**: 프록시는 메서드가 돌려준 객체를 직렬화해 Redis에 넣은 **뒤에** 그 객체를 컨트롤러에 돌려줍니다(위 그림의 ⑥ 다음 ⑦). Redis에는 그 순간의 바이트가 들어 있으므로, 컨트롤러가 나중에 객체를 고쳐도 Redis의 값은 바뀌지 않습니다.
- **히트일 때**: Redis에서 읽은 바이트를 호출마다 새로 역직렬화하므로, 호출자마다 서로 다른 객체를 받습니다. 한 요청의 수정이 다른 요청에 보이지 않습니다.

이것은 Redis 캐시가 값을 바이트로 복사해 두기 때문이고, 모든 캐시가 그런 것은 아닙니다. Spring의 기본 메모리 캐시(`ConcurrentMapCacheManager`, 다른 캐시 라이브러리가 없을 때 Spring Boot가 쓰는 것)는 기본으로 **객체의 참조**를 저장합니다. 이 경우 컨트롤러가 할인가로 바꾼 객체가 곧 캐시에 든 객체이므로, 다음 사용자도 할인가를 받습니다. 그래서 다음 두 상황에서 Redis에서는 멀쩡하던 코드가 틀린 값을 냅니다.

- 테스트에서는 메모리 캐시를 쓰고 운영에서는 Redis를 쓰는 경우(또는 그 반대). 같은 코드가 환경에 따라 다르게 동작합니다.
- Redis 앞에 참조를 저장하는 로컬 캐시를 한 층 더 두는 경우.

캐시에서 받은 객체는 고치지 않는 것이 어느 캐시에서든 안전합니다. Kotlin에서는 반환 타입을 `val` 프로퍼티만 가진 `data class`로 두고, 바꿔야 하면 `copy(...)`로 새 객체를 만듭니다. 단 `val`은 참조를 바꾸지 못하게 할 뿐 참조된 객체의 변경은 막지 않고, `copy()`는 얕은 복사라 안의 `MutableList` 같은 객체는 원본과 공유합니다. 안쪽 프로퍼티까지 읽기 전용 타입(`List` 등)으로 두어야 이 방법이 통합니다. 대신 Redis 캐시에서는 히트마다 역직렬화 비용과 새 객체 생성이 따릅니다. 큰 객체를 자주 읽는 캐시라면 이 비용이 앱 서버 CPU에 드러날 수 있습니다.

## 데이터와 요청이 늘면 무엇이 달라지는가

- **캐시를 거치지 않는 경로의 비용은 요청 수에 비례해 DB로 갑니다.** 자기 호출로 캐시를 빠뜨린 상품 페이지 API는 요청이 늘수록 DB 쿼리가 그대로 늡니다. 평소에는 DB가 버티다가 트래픽이 몰리는 날에만 드러납니다.
- **키 충돌은 같은 키를 읽는 요청이 많을수록 자주 보입니다.** 인기 상품일수록 두 메서드가 같은 키를 번갈아 채울 기회가 많아, 오류가 특정 인기 상품에 몰려 보입니다.
- **TTL 없는 키는 데이터 종류가 늘수록 쌓입니다.** 없는 상품 번호, 검색 조건 조합, 키 모양이 바뀐 배포의 옛 키가 지워지지 않고 남습니다. 메모리 한도에 닿은 뒤 무엇이 지워지는지는 축출 정책이 정합니다([TTL과 축출](./ttl-and-eviction.md)).
- **키에 상한 없는 인자가 있으면 키 수가 요청량을 따라 늡니다.** 상품 번호로 만든 키는 상품 수에서 멈추지만, 검색어나 사용자 식별자가 들어간 키는 요청이 늘수록 함께 늘고 히트는 늘지 않습니다. 메모리 한도에 닿으면 같은 Redis의 다른 캐시까지 영향을 받습니다. 원리와 고르는 기준은 "키가 끝없이 늘어나는 경우: 다시 읽히는 조합만 캐시합니다"에 있습니다.
- **사용자 수가 늘면 사용자별 키의 비용이 그대로 늡니다.** 결과를 바꾸는 값을 키에 넣을 때 사용자 식별자 대신 등급처럼 거친 값을 고르면, 키 수의 상한이 상품 수 × 등급 수에 머물러 사용자 수와 상관없어집니다.
- **Redis가 느려지면 기다리는 요청이 요청 수에 비례해 쌓입니다.** 일반(동기) Spring MVC 앱에서는 요청 하나가 끝날 때까지 요청 스레드 하나를 차지하고, Redis 응답을 기다리는 동안에도 그 스레드는 다른 요청을 받지 못합니다. 처리기를 바꿔 DB로 넘기는 구성이라도, 명령 타임아웃 동안 기다리는 요청 수가 스레드 풀 크기에 닿으면 Redis와 상관없는 API까지 스레드를 기다립니다. 기다림이 자원을 붙잡는 원리는 [타임아웃](../cs/timeout.md)의 "기다림은 자원을 붙잡습니다"에 있습니다.

## 흔한 오해 모아 보기

| 오해 | 실제 | 본문 |
| --- | --- | --- |
| `@Cacheable`을 붙이면 그 메서드는 캐시된다 | 프록시를 지나는 바깥 호출만 캐시됩니다. 같은 클래스 안의 호출, `private` 메서드, 시작 중의 호출은 에러 없이 캐시를 건너뜁니다 | 캐시하지 않는 경우: 호출이 프록시를 지나지 않습니다 |
| 기본 키에는 메서드 이름이 들어간다 | 인자만으로 만듭니다. 같은 캐시 이름을 쓰는 메서드끼리 키가 겹칩니다 | 같은 캐시 이름을 쓰는 두 메서드는 같은 키를 씁니다 |
| 키는 `equals`로 비교된다 | Redis 캐시는 문자열로 바꾼 뒤의 키로 찾습니다. `42L`과 `"42"`는 같은 키입니다 | Redis 캐시에서는 문자열이 같으면 같은 키입니다 |
| 메서드 인자가 같으면 결과도 같다 | 메서드가 보안 컨텍스트나 요청 헤더에서 사용자·권한·언어를 읽으면 인자가 같아도 결과가 다르고, 키에 그 값이 없어 다른 사용자에게 나갑니다 | 결과를 바꾸는 값이 키에 없으면 다른 사용자의 응답이 나갑니다 |
| `allEntries = true`는 그 캐시의 키만 지운다 | 키 모양에 맞는 키를 `KEYS`로 찾아 지웁니다. 다른 앱이 같은 캐시 이름을 쓰면 그 앱의 키도, 접두사를 끄면 데이터베이스의 모든 키를 지웁니다 | 다른 앱과 같은 Redis를 쓰면 캐시 이름이 겹칩니다 |
| 캐시를 붙이면 그만큼 DB 부하가 준다 | 채운 키가 사라지기 전에 같은 키가 다시 와야 히트입니다. 검색어나 사용자 식별자가 들어간 키는 요청이 넓게 퍼지면 거의 다시 오지 않아 메모리만 씁니다 | 키가 끝없이 늘어나는 경우: 다시 읽히는 조합만 캐시합니다 |
| `condition = "#result != null"`로 null을 거른다 | `condition`은 실행 전에 평가해 `#result`가 없으므로 아예 캐시하지 않게 됩니다. `unless`를 씁니다 | `condition`에 `#result`를 쓰면 한 번도 캐시하지 않습니다 |
| `time-to-live`를 정했으니 모든 캐시에 TTL이 있다 | `RedisCacheConfiguration` 빈을 직접 등록하면 이 속성은 무시됩니다 | 캐시마다 TTL을 정하는 방법과 빠지기 쉬운 함정 |
| Redis가 죽으면 `@Cacheable`은 DB로 넘어간다 | 기본 처리기는 예외를 다시 던져 요청이 실패합니다. 넘어가게 해도 명령 타임아웃만큼 기다린 뒤입니다 | Redis가 실패하면: 기본은 DB로 넘어가지 않고 실패합니다 |
| 캐시에서 받은 객체를 고치면 캐시도 바뀐다 | Redis 캐시는 바이트를 복사해 두므로 바뀌지 않습니다. 참조를 저장하는 메모리 캐시에서는 바뀝니다 | 돌려받은 객체를 고쳐도 Redis 캐시는 바뀌지 않습니다 |

## 근거

**Spring Framework** 태그 [`v6.2.19`](https://github.com/spring-projects/spring-framework/tree/v6.2.19)(커밋 `6214eae8bd02c2ed7ab382bb8d16a9cc6de49522`)

- [`annotations.adoc`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/framework-docs/modules/ROOT/pages/integration/cache/annotations.adoc)
  - "Default Key Generation": 인자가 없으면 `SimpleKey.EMPTY`, 하나면 그 인자, 여럿이면 `SimpleKey`(단일 인자의 `null`·배열 예외는 `SimpleKeyGenerator.generateKey`에서 확인)
  - "Conditional Caching": `condition`이 거짓이면 캐시하지 않은 것처럼 매번 메서드 실행, `unless`는 메서드 실행 후 평가, `Optional`이면 `#result`는 안의 값
  - "Available Caching SpEL Evaluation Context": 인자 이름을 알 수 없으면(예: `-parameters` 없이 컴파일) `#a0`·`#p0`로 접근, `#result`는 `unless` 등에서만 사용 가능
  - "Method visibility and cache annotations": 프록시 방식에서는 `public` 메서드에만 붙이라고 안내(아래 `ProxyCachingConfiguration`의 실제 설정과 어긋남)
  - 프록시 방식 NOTE: 바깥에서 프록시로 들어온 호출만 가로채며 자기 호출은 캐시하지 않음, `aspectj` 모드 고려, 초기화 코드(`@PostConstruct`)에서 기대지 말 것
- [`SimpleKeyGenerator.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/cache/interceptor/SimpleKeyGenerator.java): `generate`·`generateKey`(단일 인자는 `null`도 배열도 아닐 때만 그대로 키, 인자만 사용하고 메서드는 키에 넣지 않음)
- [`SimpleKey.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/cache/interceptor/SimpleKey.java): `toString`(`SimpleKey [인자들]`)
- [`CacheAspectSupport.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/cache/interceptor/CacheAspectSupport.java)
  - `afterSingletonsInstantiated`·`execute(CacheOperationInvoker, Object, Method, Object[])`: 모든 싱글턴이 만들어진 뒤 `initialized`가 켜지기 전에는 캐시 처리 없이 메서드만 실행
  - `execute`·`findCachedValue`·`evaluate`: `condition` 평가 → 키 생성 → 조회 → 미스면 메서드 실행 → `put` 순서, 미스일 때 반환값을 넣은 뒤 같은 객체를 돌려줌
  - `generateKey`: 키가 `null`이면 "Null key returned ..." `IllegalArgumentException`
  - `CacheOperationContext.isConditionPassing`(처음 평가한 결과를 기억, `@Cacheable`은 결과 없이 평가)·`canPutToCache`(`unless`를 반환값으로 평가)
  - `CacheOperationContexts.determineSyncFlag`: `sync = true`와 `unless`, 여러 캐시, 다른 캐시 연산을 함께 쓰면 `IllegalStateException`. 이 객체는 호출마다 만들어짐
  - `executeSynchronized`: `doGet(cache, key, valueLoader)` 한 번으로 위임
- [`CacheOperationExpressionEvaluator.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/cache/interceptor/CacheOperationExpressionEvaluator.java): `createEvaluationContext`(`NO_RESULT`면 `result` 변수를 넣지 않음)
- [`MethodBasedEvaluationContext.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/context/expression/MethodBasedEvaluationContext.java): 인자를 `a0`·`p0`와, 이름을 알면 이름으로도 등록
- [`DefaultParameterNameDiscoverer.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-core/src/main/java/org/springframework/core/DefaultParameterNameDiscoverer.java): `kotlin-reflect`가 있으면 Kotlin 리플렉션으로, 그 밖에는 `-parameters`로 남은 이름을 읽음
- [`AbstractCacheInvoker.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/cache/interceptor/AbstractCacheInvoker.java): `doGet`·`doPut`·`doEvict`·`doClear`(예외를 `CacheErrorHandler`에 넘김, 처리되면 읽기는 미스로), `doGet(Cache, Object, Callable)`(캐시 호출이 `ValueRetrievalException`이 아닌 예외로 실패하면 처리기에 넘긴 뒤 `valueLoader`를 다시 호출)
- [`Cacheable.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/cache/annotation/Cacheable.java): `condition`, `unless`("evaluated after the method has been called"), `sync`(제약 세 가지, 제공자가 지원하지 않을 수 있는 힌트, `sync = true`는 읽기와 쓰기가 합쳐진 호출 하나)
- [`ProxyCachingConfiguration.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/cache/annotation/ProxyCachingConfiguration.java): `cacheOperationSource`가 `new AnnotationCacheOperationSource(false)`로 `public`이 아닌 메서드의 애너테이션도 읽음. [`AbstractFallbackCacheOperationSource.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/cache/interceptor/AbstractFallbackCacheOperationSource.java)의 `computeCacheOperations`(`publicMethodsOnly`가 참일 때만 `public`이 아닌 메서드를 제외)
- [`EnableCaching.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/cache/annotation/EnableCaching.java): `mode()` 기본 `AdviceMode.PROXY`
- [`CglibAopProxy.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-aop/src/main/java/org/springframework/aop/framework/CglibAopProxy.java): 하위 클래스를 만들지 못하면 "Common causes of this problem include using a final class" `AopConfigException`, `doValidateClass`(`final` 메서드는 프록시되지 않고 원래 객체로 가지 않는다는 debug 로그, 인터페이스 메서드를 구현한 `final` 메서드는 warn 로그)
- [`ObjenesisCglibAopProxy.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-aop/src/main/java/org/springframework/aop/framework/ObjenesisCglibAopProxy.java): `createProxyClassAndInstance`(기본은 Objenesis로 생성자를 호출하지 않고 프록시를 만들고, 실패하면 생성자로 만듦). [`proxying.adoc`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/framework-docs/modules/ROOT/pages/core/aop/proxying.adoc): CGLIB 프록시 인스턴스는 Objenesis로 만들어 생성자가 두 번 호출되지 않음(JVM이 생성자 우회를 허용하지 않으면 생성자로 만들어 두 번 호출될 수 있음)
- [`AbstractAutoProxyCreator.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-aop/src/main/java/org/springframework/aop/framework/autoproxy/AbstractAutoProxyCreator.java): `postProcessAfterInitialization`·`wrapIfNecessary`(초기화를 마친 원래 빈을 대상으로 두고 별개의 프록시를 만들어 감쌈)
- [`SimpleCacheErrorHandler.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/cache/interceptor/SimpleCacheErrorHandler.java): 모든 캐시 예외를 다시 던짐
- [`LoggingCacheErrorHandler.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/cache/interceptor/LoggingCacheErrorHandler.java): 읽기·쓰기·삭제·비우기 예외를 경고 로그로 남기고 던지지 않음, 기본 생성자는 스택 트레이스 없음
- [`CachingConfigurer.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/cache/annotation/CachingConfigurer.java): `errorHandler()`
- [`AbstractCacheResolver.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/cache/interceptor/AbstractCacheResolver.java): 캐시 매니저가 캐시를 내주지 않으면 "Cannot find cache named" `IllegalArgumentException`
- [`ConcurrentMapCacheManager.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/cache/concurrent/ConcurrentMapCacheManager.java): `storeByValue = false` 기본값(참조 저장)
- `@CacheEvict(allEntries = true)`가 캐시 전체 비우기가 되는 경로: `annotations.adoc`의 "The `@CacheEvict` Annotation"(`allEntries`는 캐시 전체 삭제), `CacheAspectSupport.performCacheEvicts`(`isCacheWide()`면 `doClear`), `AbstractCacheInvoker.doClear`(`beforeInvocation`이면 `invalidate()`, 아니면 `clear()`), [`Cache.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/cache/Cache.java)의 `invalidate` 기본 구현(`clear()` 호출)

**Spring Data Redis** 태그 [`3.5.13`](https://github.com/spring-projects/spring-data-redis/tree/3.5.13)(커밋 `42619b203148bae01235fe14f3cff3e171d93ebf`)

- [`RedisCache.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/RedisCache.java)
  - `createCacheKey`·`convertKey`·`hasToStringMethod`: 문자열은 그대로, 변환 서비스로 바꿀 수 있으면 변환, 아니면 `Object`가 아닌 클래스가 정의한 `toString()`, 둘 다 아니면 "Cannot convert cache key" `IllegalStateException`. 접두사를 쓰면 앞에 붙임
  - `put`·`processAndCheckValue`: null을 허용하지 않으면 "does not allow 'null' values" `IllegalArgumentException`, 값을 직렬화한 뒤 쓰기
  - `get(Object, Callable)`: 쓰기 객체의 "없으면 불러와 넣기" 호출 안에서 메서드 실행과 직렬화
  - `clear()`·`clear(String)`: `clear("*")`로 패턴 `*`에 `createCacheKey`를 적용해(접두사를 쓰면 `product::*`처럼 접두사 뒤에 `*`, 끄면 `*` 그대로) 쓰기 객체의 `clean`에 넘김. `RedisCache`는 `invalidate`를 재정의하지 않음
- [`RedisCacheWriter.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/RedisCacheWriter.java): `nonLockingRedisCacheWriter(connectionFactory)`는 `BatchStrategies.keys()`를 씀, 두 인자 버전으로 다른 `BatchStrategy` 지정. `RedisCacheManager.RedisCacheManagerBuilder.fromConnectionFactory`가 이 한 인자 버전으로 쓰기 객체를 만들고, 빌더의 `cacheWriter(...)`로 바꿀 수 있음
- [`BatchStrategies.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/BatchStrategies.java): `keys()`(`KEYS` 한 번과 `DEL`, "can block the Redis worker thread for a long time on large keyspaces"), `scan(int batchSize)`(`SCAN` 커서와 여러 번의 `DEL`, 드라이버와 운영 방식에 따라 지원하지 않을 수 있다는 Javadoc NOTE)
- [`DefaultRedisCacheWriter.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/DefaultRedisCacheWriter.java): `get(String, byte[], Supplier, Duration, boolean)`(조회 → 없으면 불러오기 → 쓰기를 한 작업 안에서 실행), `clean`(지정된 `BatchStrategy`의 `cleanCache`로 패턴에 맞는 키 삭제)
- [`RedisCacheConfiguration.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/RedisCacheConfiguration.java): `defaultCacheConfig`(만료 없음 `TtlFunction.persistent()`, null 허용, 접두사 사용, `CacheKeyPrefix.simple()`, Java 직렬화), `prefixCacheNameWith`, `disableKeyPrefix`(Javadoc: 접두사를 끄면 `Cache#clear()`가 의도하지 않은 키를 지울 수 있으니 전용 Redis를 쓰라는 NOTE), `entryTtl`
- [`CacheKeyPrefix.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/CacheKeyPrefix.java): `SEPARATOR = "::"`, `simple()`은 `캐시이름::`, `prefixed(prefix)`는 `prefix + 캐시이름 + ::`
- [`RedisCacheManager.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/RedisCacheManager.java): `getMissingCache`(실행 중 생성이 허용되면 기본 설정으로 새 캐시), `RedisCacheManagerBuilder`의 `cacheDefaults()`(현재 기본 설정 반환), `withCacheConfiguration`, `disableCreateOnMissingCache`, 실행 중 생성 기본 허용
- [`RedisTemplate.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/core/RedisTemplate.java): `afterPropertiesSet`(직렬화기를 지정하지 않으면 `JdkSerializationRedisSerializer`를 기본으로 씀)

**Spring Boot** 태그 [`v3.5.16`](https://github.com/spring-projects/spring-boot/tree/v3.5.16)(커밋 `0566f6933049aca6bc5ffc6d559fffade9cd2e0c`)

- [`RedisCacheConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/cache/RedisCacheConfiguration.java): `determineConfiguration`(`RedisCacheConfiguration` 빈이 있으면 그것을 쓰고 속성으로 만들지 않음), `createConfiguration`(`time-to-live`·`key-prefix`·`cache-null-values`·`use-key-prefix`를 기본 설정에 반영, 값 직렬화는 Java 직렬화), `cacheManager`(`enable-statistics`는 기본 설정과 별도로 빌더에 적용), 빌더에 `RedisCacheManagerBuilderCustomizer`를 적용한 뒤 생성
- [`CacheProperties.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/cache/CacheProperties.java): `Redis`의 속성은 `timeToLive`, `cacheNullValues`(기본 true), `keyPrefix`, `useKeyPrefix`(기본 true), `enableStatistics`뿐이며 캐시별 TTL 속성은 없음
- [`CacheMetricsRegistrarConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-actuator-autoconfigure/src/main/java/org/springframework/boot/actuate/autoconfigure/metrics/cache/CacheMetricsRegistrarConfiguration.java): `bindCacheManagerToRegistry`(등록 시점에 있는 캐시만 지표에 묶음)
- [`AopAutoConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/aop/AopAutoConfiguration.java): `spring.aop.proxy-target-class` 기본 true(클래스 상속 프록시)
- [`RedisAutoConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/data/redis/RedisAutoConfiguration.java): `redisTemplate`(직렬화기를 따로 지정하지 않은 `RedisTemplate`을 만듦)
- [`SimpleCacheConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/cache/SimpleCacheConfiguration.java): 다른 캐시가 없을 때 `ConcurrentMapCacheManager` 생성

**Redis 공식 문서**(redis/docs 커밋 `e53775dde0ee7e39ebe327e7ffff583caa9fbd65`)

- [TTL](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/ttl.md): 키가 없으면 `-2`, 키는 있는데 만료가 없으면 `-1`
- [INFO](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/info.md): `keyspace` 절의 `dbXXX: keys=XXX,expires=XXX,...`, `stats` 절의 `evicted_keys`(`maxmemory` 한도 때문에 축출된 키 수)
- [redis-cli](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/develop/tools/cli.md): "Get a list of keys"(`--scan`은 `SCAN`을 써서 서버를 막지 않으며 `KEYS *`는 막음, `--pattern`으로 거름)
- [KEYS](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/keys.md): 운영 환경에서 극히 조심해서 쓰라는 WARNING("It may ruin performance when it is executed against large databases")

**Redis 소스** 태그 [`8.2.10`](https://github.com/redis/redis/tree/8.2.10)

- [`redis.conf`](https://github.com/redis/redis/blob/8.2.10/redis.conf): `maxmemory-policy`의 기본값 `noeviction`

**Kotlin 문서**(JetBrains/kotlin-web-site 커밋 `3d59d2947125c1496f2847c40d7121b1cc3a9923`)

- [`kotlin-spring-plugin.md`](https://github.com/JetBrains/kotlin-web-site/blob/3d59d2947125c1496f2847c40d7121b1cc3a9923/docs/topics/compiler-plugins/kotlin-spring-plugin.md): 클래스에 `@Component`, `@Async`, `@Transactional`, `@Cacheable`, `@SpringBootTest`가 있으면 클래스와 멤버를 `open`으로, `@Component`를 메타 애너테이션으로 가진 애너테이션도 포함
- [`data-classes.md`](https://github.com/JetBrains/kotlin-web-site/blob/3d59d2947125c1496f2847c40d7121b1cc3a9923/docs/topics/data-classes.md): 자동 생성 `toString()` 등은 주 생성자의 프로퍼티만 사용("Properties declared in the class body"), `copy()`는 얕은 복사로 다른 객체의 참조를 공유("Copying")
- [`all-open-plugin.md`](https://github.com/JetBrains/kotlin-web-site/blob/3d59d2947125c1496f2847c40d7121b1cc3a9923/docs/topics/compiler-plugins/all-open-plugin.md): Kotlin의 클래스와 멤버는 기본 `final`

## 실무 포인트

### 중요도별 점검 항목

| 중요도 | 항목 | 이유 |
| --- | --- | --- |
| 필수 | 캐시 장애 때 요청을 실패시킬지, DB로 넘길지 정하고 에러 처리기와 명령 타임아웃을 함께 맞추기 | 기본은 실패이고, 처리기만 바꾸면 타임아웃(기본 60초)을 기다린 뒤 넘어가 사실상 멈춥니다. 둘을 따로 정하면 어느 쪽 목표도 이루지 못합니다 |
| 필수 | 캐시 이름마다 TTL을 정하고, `redis-cli TTL`로 실제 키에 붙었는지 확인하기 | 기본은 만료 없음이고, `RedisCacheConfiguration` 빈 하나로 TTL 속성이 조용히 무시될 수 있습니다 |
| 필수 | 캐시 이름을 메서드마다 따로 쓰기 | 기본 키에 메서드가 들어가지 않아, 이름을 나눠 쓰면 타입이 다른 값이 오가는 장애가 생깁니다 |
| 필수 | 캐시를 붙이기 전에 결과를 바꾸는 값(사용자, 권한, 언어, 실험 그룹)을 모두 찾아 키에 넣거나 캐시 밖으로 빼기 | 빠뜨리면 예외 없이 다른 사용자의 값이 나가고, 권한에 따라 다른 값이면 정보 노출이 됩니다 |
| 필수 | 공유 Redis라면 앱마다(필요하면 환경마다) `key-prefix` 정하기 | 같은 캐시 이름이 다른 앱의 값과 섞이고, `allEntries = true`가 다른 앱의 캐시까지 지웁니다 |
| 알면 유리 | 키에 들어가는 인자마다 가짓수의 상한을 확인하고, 상한이 없으면 범위 좁히기·조각 캐시·캐시 안 함 중에서 고르기 | 검색어나 사용자 식별자가 든 키는 요청이 넓게 퍼지면 메모리만 쓰고 히트를 거의 내지 못하며, 한도에 닿으면 다른 캐시의 키까지 밀어냅니다 |
| 필수 | 캐시가 실제로 동작하는지 테스트로 고정하기(같은 인자로 두 번 불러 DB 호출이 한 번인지) | 자기 호출과 `private` 메서드는 에러 없이 캐시를 건너뜁니다. 리팩터링이 언제든 만듭니다 |
| 알면 유리 | 삭제 실패와 읽기 실패를 처리기에서 나눠 다루기 | 삭제 실패를 삼키면 틀린 값이 TTL까지 남고 API는 성공합니다 |
| 알면 유리 | 캐시 키가 많은 Redis에서 `allEntries = true`를 쓴다면 쓰기 객체를 `SCAN` 방식으로 바꾸고, 쓰는 드라이버·운영 방식에서 동작하는지 확인하기 | 기본 `KEYS`는 데이터베이스의 모든 키를 훑는 동안 Redis를 막습니다 |
| 알면 유리 | `disableCreateOnMissingCache()`로 설정에 없는 캐시 이름을 막기 | 오타 난 캐시 이름이 기본 설정(기본 TTL이 없으면 만료 없음)의 새 캐시가 되는 것을 첫 호출에서 잡습니다 |
| 참고 | 키 객체 대신 필요한 필드만 키 식에 넣기 | `data class` 키 객체의 주 생성자에 프로퍼티를 추가하는 배포가 캐시 전체를 비우는 효과를 냅니다 |

### 개발자가 정할 수 있는 것과 관찰해야 하는 것

| 정할 수 있는 것 | 관찰해야 하는 것 |
| --- | --- |
| 캐시 이름, `key`·`condition`·`unless` 식 | `redis-cli --scan --pattern '캐시이름::*'`로 본 실제 키 모양 |
| 캐시별 TTL(`withCacheConfiguration`), 기본 TTL(`time-to-live`) | `redis-cli TTL <키>`, `INFO keyspace`의 `keys`와 `expires` |
| 에러 처리기(`CachingConfigurer.errorHandler()`), 명령 타임아웃(`spring.data.redis.timeout`) | 캐시 에러 로그 수, Redis 장애 시 API 지연과 DB 쿼리 수 |
| 캐시하는 메서드를 둘 빈의 경계 | 캐시별 `cache.gets`와 그 API 요청 수의 비율, `TRACE` 로그의 계산된 키 |
| 키에 넣을 값(등급처럼 거친 값인가, 사용자 식별자인가), 캐시할 범위(`condition`), 앱마다의 `key-prefix` | 캐시별 초당 히트와 초당 저장의 비율, `INFO keyspace`의 `keys` 추이, `INFO stats`의 `evicted_keys` |
| 반환 타입(불변 `data class`) | 히트 비율이 높은 캐시에서 앱 서버 CPU 중 역직렬화 비중 |

### 증상과 확인 순서

| 증상 | 먼저 볼 것 | 그다음 |
| --- | --- | --- |
| 특정 API만 매번 DB로 간다 | 그 경로가 `@Cacheable` 메서드를 다른 빈에서 부르는가(자기 호출) | `private`·`final` 여부, `condition` 식, `TRACE` 로그가 남는지 |
| 다른 사용자의 정보(회원가, 권한별 화면)가 보인다 | 결과를 바꾸는 값(사용자, 권한, 언어)이 메서드 인자와 키에 들어 있는가 | `TRACE` 로그의 계산된 키, `redis-cli --scan`으로 본 키에 사용자 구분이 있는지 |
| 캐시를 붙였는데 DB 부하는 그대로이고 Redis 메모리만 는다 | 그 캐시의 초당 히트가 초당 저장보다 적은가(저장 한 번이 평균 한 번도 읽히지 않음) | 키에 검색어·사용자 식별자·페이지 같은 상한 없는 인자가 있는지, `evicted_keys`가 늘며 다른 캐시의 적중률도 떨어지는지 |
| 관리 화면에서 캐시를 비운 뒤 다른 앱의 캐시나 세션이 사라졌다 | `allEntries = true`가 지운 패턴(`key-prefix`가 앱마다 다른가, `use-key-prefix=false`인가) | 비우는 동안 Redis 지연이 튀었는지(`KEYS` 실행) |
| 간헐적인 `ClassCastException` 또는 역직렬화 예외 | 같은 캐시 이름을 쓰는 다른 메서드·다른 앱이 있는가 | 두 경로의 계산된 키 비교, `key-prefix` 설정 |
| 값이 바뀌었는데 오래 낡은 값이 나온다 | `redis-cli TTL`이 `-1`인가 | `RedisCacheConfiguration` 빈 존재 여부, 캐시 이름 오타, 삭제 실패 로그 |
| 배포 직후 DB 부하가 뛰고 Redis 메모리가 는다 | 키 모양이 바뀌었는가(키 객체 필드 추가, 캐시 이름 변경) | 옛 키에 TTL이 있는지, 예열 시점 |
| Redis 장애 때 API 전체가 실패하거나 멈춘다 | 에러 처리기가 기본(다시 던짐)인가 | 명령 타임아웃 값, DB가 캐시 없이 받을 부하 |
| null 반환 API가 500을 낸다 | `cache-null-values=false`인데 `unless`가 없는가 | 에러 처리기가 삼키고 있다면 그 API의 DB 호출 수 |

## 말로 설명하기

### 30초 버전

`@Cacheable`은 애너테이션이 아니라 Spring이 끼워 넣은 프록시가 동작합니다. 그래서 같은 클래스 안의 호출이나 `private` 메서드는 에러 없이 캐시를 건너뜁니다. 기본 키는 인자만으로 만들고 메서드 이름이 없어서, 같은 캐시 이름을 쓰는 메서드끼리 값이 섞입니다. 키에는 결과를 정하는 값이 모두 들어가야 하는데, 사용자나 언어처럼 인자 밖에서 읽는 값이 빠지면 다른 사용자의 응답이 나가고, 반대로 검색어처럼 가짓수가 끝없고 요청이 넓게 퍼지는 값이 들어가면 메모리만 쓰고 히트는 거의 나지 않습니다. TTL은 애너테이션이 아니라 캐시 매니저 설정에서 오고 기본은 만료 없음입니다. 그리고 기본 에러 처리기는 Redis 예외를 그대로 던지므로, 캐시 장애가 곧 API 장애입니다. 그래서 캐시 이름을 메서드마다 나누고, 캐시별 TTL을 실제 키에서 확인하고, 장애 때 DB로 넘길지를 타임아웃과 함께 정합니다.

### 2분 버전

`@Cacheable`이 붙은 빈은 Spring이 상속으로 만든 프록시로 바뀌어 주입됩니다. 프록시는 호출을 받아 `condition`을 보고, 인자로 키를 만들어 Redis에서 `GET`하고, 없으면 원래 메서드를 실행한 뒤 `unless`를 보고 `SET`합니다. 이 구조에서 세 종류의 문제가 나옵니다.

첫째, 캐시하지 않는 문제입니다. 같은 클래스의 다른 메서드가 `this`로 부르면 프록시를 지나지 않습니다. `private` 메서드, 시작 중 `@PostConstruct`에서의 호출도 같습니다. Kotlin의 열리지 않은 클래스는 시작이 실패하고, 열리지 않은 메서드는 캐시를 건너뛰며 `NullPointerException`까지 낼 수 있습니다. `condition`은 실행 전에 평가하므로 `#result`로 거르면 아예 캐시되지 않습니다. 대부분 에러가 없으니 같은 인자로 두 번 불러 DB 호출 수를 세는 테스트로 고정합니다.

둘째, 엉뚱한 값입니다. 기본 키는 인자 하나면(`null`이나 배열이 아니면) 그 인자 자체이고 메서드 이름이 없으므로, 같은 캐시 이름을 쓰는 두 메서드가 `product::42`를 함께 씁니다. Redis 캐시는 키를 문자열로 바꿔 비교하므로 `42L`과 `"42"`도 같습니다. `data class` 키 객체는 `toString()`이 키가 되므로 주 생성자에 프로퍼티를 추가하면 키가 모두 바뀝니다. 공유 Redis라면 다른 앱의 같은 캐시 이름과도 섞이고, `allEntries = true`는 `KEYS`로 찾은 다른 앱의 키까지 지웁니다. 그래서 앱마다 `key-prefix`를 둡니다. 가장 조용한 사고는 메서드가 사용자 등급이나 언어를 인자가 아닌 보안 컨텍스트나 헤더에서 읽는 경우입니다. 키에 그 값이 없어 처음 채운 사용자의 응답이 TTL 동안 모두에게 나갑니다. 모두에게 같은 부분만 캐시하고 사용자별 부분을 밖에서 덧입히거나, 결과를 정하는 가장 거친 값(사용자 식별자가 아니라 등급)을 인자로 받아 키에 넣습니다.

키 가짓수도 설계 대상입니다. 히트는 채운 키가 TTL로 사라지기 전에 같은 키가 다시 와야 생기므로, 검색어나 사용자 식별자처럼 상한 없는 값이 키에 들어가고 요청이 넓게 퍼지면 키가 요청 수에 가깝게 늘고 히트는 거의 나지 않습니다. 메모리 한도에 닿으면 축출로 다른 캐시의 키를 밀어내거나, Redis 기본 정책(`noeviction`)에서는 쓰기가 OOM 에러를 받아 기본 에러 처리기 아래에서 요청이 실패합니다. 그래서 키 가짓수의 상한, TTL 안의 재조회, 미스 비용을 보고 반복되는 범위만 `condition`으로 캐시하거나, 모두가 다시 쓰는 조각을 캐시하거나, 캐시하지 않습니다.

셋째, 너무 오래 남는 문제입니다. TTL은 캐시 매니저 설정에서 오고 기본은 만료 없음입니다. Spring Boot 속성은 모든 캐시에 하나의 TTL만 주고, `RedisCacheConfiguration` 빈을 직접 만들면 그 TTL 속성마저 무시됩니다. 오타 난 캐시 이름은 기본 설정으로 새로 만들어집니다.

마지막으로 Redis 실패는 기본 처리기가 다시 던져 요청이 실패합니다. 처리기를 바꾸면 미스로 넘어가지만, 명령 타임아웃을 기다린 뒤이고 DB가 전체 부하를 받습니다. 삭제 실패까지 삼키면 낡은 값이 남으니 읽기와 삭제를 나눠 다룹니다.

### 예상 질문과 꼬리 질문

**Q. 같은 클래스 안에서 부르면 왜 캐시가 안 되나요?**
캐시 동작이 원래 객체가 아니라 그 앞에 선 프록시에 있기 때문입니다. 다른 빈이 주입받은 참조는 프록시라 호출이 가로채지지만, 메서드 안의 `this`는 원래 객체를 가리킵니다. Spring 참조 문서도 프록시 방식에서는 자기 호출을 캐시하지 않는다고 적습니다. (본문: "같은 클래스 안에서 부르면 캐시를 거치지 않습니다")

- **꼬리: 그러면 AspectJ 모드로 바꾸면 되지 않나요?**
  됩니다. 바이트코드를 고쳐 모든 호출을 가로채므로 자기 호출과 `private` 메서드도 캐시합니다. 하지만 컴파일이나 클래스 로딩 단계의 위빙 설정이 따로 필요해, 이 문제 하나를 위해서라면 캐시하는 메서드를 다른 빈으로 옮기는 편이 단순하고 코드만 보고도 동작을 알 수 있습니다.
- **꼬리: 테스트에서는 캐시가 됐는데 운영에서 안 된다면요?**
  테스트가 서비스 빈을 바깥에서 직접 불러 프록시를 지났고, 운영 경로는 같은 클래스의 다른 메서드를 거쳐 들어왔을 가능성이 큽니다. 테스트는 실제 진입점(컨트롤러나 그 API를 부르는 메서드)에서 같은 인자로 두 번 불러 DB 호출 수를 봐야 합니다.

**Q. Kotlin은 클래스가 `final`인데 프록시가 어떻게 만들어지나요?**
보통 `kotlin-spring` 플러그인이 `@Component` 계열이나 `@Cacheable`이 클래스에 붙은 경우 그 클래스와 멤버를 `open`으로 컴파일합니다. 플러그인이 없거나 클래스에 해당 애너테이션이 없으면 `final` 클래스라 프록시를 만들지 못해 시작이 실패합니다. 클래스만 열리고 메서드가 `final`이면 그 메서드는 가로채지 못합니다. (본문: "`private` 메서드, 열리지 않은 Kotlin 메서드")

- **꼬리: 메서드만 `final`이면 그냥 캐시 없이 실행되는 것 아닌가요?**
  그보다 나쁠 수 있습니다. 호출이 원래 객체로 넘어가지 않고 프록시 객체 자신에서 실행되는데, 프록시는 원래 객체와 별개의 객체이고 의존성은 원래 객체에만 주입되므로 필드가 비어 있어 `NullPointerException`이 날 수 있습니다. Spring은 이 상황을 debug 로그로 알리고, 경고 로그는 그 메서드가 인터페이스 메서드를 구현한 경우에만 남깁니다.

**Q. 기본 키 생성 방식의 문제는 무엇인가요?**
인자만으로 키를 만들고 메서드 이름을 넣지 않는다는 점입니다. 같은 캐시 이름을 쓰는 `getProduct(42)`와 `getProductSummary(42)`가 둘 다 `product::42`를 쓰므로, 먼저 채운 쪽의 값을 다른 쪽이 받아 형 변환 예외가 나거나, 타입이 같다면 조용히 틀린 값이 나갑니다. (본문: "같은 캐시 이름을 쓰는 두 메서드는 같은 키를 씁니다")

- **꼬리: 그러면 키 생성기를 바꿔 메서드 이름을 넣으면 해결되지 않나요?**
  예시의 두 메서드 사이 충돌은 사라지지만, 다른 클래스의 같은 이름 메서드나 오버로드는 여전히 겹칩니다. 그리고 `@CacheEvict`가 붙은 갱신 메서드는 이름이 달라 같은 키를 만들지 못합니다. 지우는 쪽에 결국 키를 직접 적어야 해서, 캐시 이름을 메서드마다 나누는 편이 단순합니다. 이름을 나누면 TTL도 따로 정할 수 있습니다.
- **꼬리: 키 비교는 `equals`로 하지 않나요?**
  메모리 캐시는 그렇지만 Redis 캐시는 키를 문자열로 바꾼 뒤 Redis 키로 찾습니다. `Long` 42와 `String` "42"는 `equals`로는 다르지만 같은 Redis 키입니다. (본문: "Redis 캐시에서는 문자열이 같으면 같은 키입니다")
- **꼬리: 키 객체로 `data class`를 쓰면 무엇이 문제인가요?**
  `toString()`이 키가 되므로 주 생성자에 프로퍼티를 하나 추가하는 배포가 모든 키를 바꿉니다. 그 순간 전부 미스가 되어 DB로 가고, 옛 키는 TTL까지(없으면 계속) 남습니다. 키에 넣을 필드를 SpEL로 골라 두면 피할 수 있지만, 그 필드 목록을 클래스와 함께 관리해야 합니다.

**Q. 캐시 키에는 무엇을 넣어야 하나요?**
메서드 인자가 아니라 그 결과를 정하는 값 전부입니다. 둘이 다른 경우가 위험합니다. 상품 상세가 회원가를 보여 주려고 회원 등급을 보안 컨텍스트에서 읽으면 키는 여전히 `product::42`라, 처음 채운 VIP 회원의 가격이 TTL 동안 모든 사용자에게 나갑니다. 타입이 같아 예외도 나지 않습니다. 그래서 캐시를 붙이기 전에 사용자, 권한, 언어, 실험 그룹처럼 인자 밖에서 결과를 바꾸는 값을 찾습니다. (본문: "결과를 바꾸는 값이 키에 없으면 다른 사용자의 응답이 나갑니다")

- **꼬리: 그러면 사용자 식별자를 키에 넣으면 되지 않나요?**
  값은 맞아지지만 키가 상품 수 × 사용자 수까지 늘고, 같은 사용자가 같은 상품을 TTL 안에 다시 볼 때만 히트가 나므로 캐시가 거의 일하지 않습니다. 먼저 모두에게 같은 부분만 캐시하고 사용자별 부분을 밖에서 덧입힐 수 있는지 봅니다. 키에 넣어야 한다면 결과를 정하는 가장 거친 값, 예를 들어 회원가가 등급으로만 정해지면 등급을 넣습니다. (본문: "키가 끝없이 늘어나는 경우: 다시 읽히는 조합만 캐시합니다")
- **꼬리: 키 생성기에서 보안 컨텍스트를 읽어 키에 넣으면 코드 변경이 적지 않나요?**
  적습니다. 대신 메서드 시그니처만 보고는 키에 무엇이 들어가는지 알 수 없고, 가격을 바꾸는 관리자 메서드의 `@CacheEvict`는 그 사용자 컨텍스트가 아니므로 모든 등급의 키를 지우도록 따로 맞춰야 합니다. 인자로 받으면 이 의존이 코드에 드러나 리뷰에서 보입니다.

**Q. 여러 서비스가 Redis 하나를 같이 쓰면 캐시 키에서 무엇을 정해야 하나요?**
앱마다 다른 `spring.cache.redis.key-prefix`입니다. 기본 키는 `캐시이름::키`뿐이라 두 앱이 `product`라는 이름을 쓰면 같은 키를 읽고 써서 역직렬화나 형 변환 예외가 나고, 한쪽의 삭제가 다른 쪽 캐시를 지웁니다. 접두사를 두면 `shop:product::42`와 `admin:product::42`로 갈립니다. 환경이 Redis를 나눠 쓴다면 환경도 접두사에 넣습니다. (본문: "다른 앱과 같은 Redis를 쓰면 캐시 이름이 겹칩니다")

- **꼬리: 접두사가 없으면 `allEntries = true`는 어떻게 되나요?**
  Redis 캐시는 캐시 전체 비우기를 "키 모양에 맞는 키를 `KEYS`로 찾아 `DEL`"로 처리합니다. 접두사가 앱마다 같으면 다른 앱의 `product::*`도 지우고, `use-key-prefix=false`면 패턴이 `*`라 세션 같은 캐시 아닌 키까지 데이터베이스의 모든 키를 지웁니다.
- **꼬리: 접두사를 잘 두면 `allEntries = true`는 안전한가요?**
  지우는 범위는 안전해지지만 비용은 남습니다. 기본 방식의 `KEYS`는 패턴과 상관없이 데이터베이스의 모든 키를 훑고, 그동안 Redis는 다른 명령을 처리하지 않습니다. 키가 많다면 쓰기 객체를 `SCAN` 방식(`BatchStrategies.scan`)으로 바꾸고, 쓰는 구성에서 지원되는지 확인합니다. 대신 왕복이 늘고 비우는 도중 들어온 키는 남을 수 있습니다.

**Q. 검색 결과처럼 조건이 다양한 조회도 `@Cacheable`로 캐시하면 되나요?**
조건 조합의 가짓수와 재조회를 먼저 봅니다. 히트는 채운 키가 TTL로 사라지기 전에 같은 키가 다시 와야 생기므로, 자유 검색어가 들어간 키처럼 대부분의 조합이 한 번만 오면 거의 모든 조회가 미스이고 키는 메모리만 씁니다. 예시의 가상 수치로 10분에 검색 6만 번 중 조합이 5만 가지면 빈 캐시에서 시작했다고 할 때 적중률은 많아야 약 17%입니다. 인기 검색어에 요청이 몰리는 서비스라면 다를 수 있으므로 실제 분포를 잽니다. 검색어 없는 카테고리 목록의 앞쪽 페이지처럼 반복되는 범위만 `condition`으로 캐시하고, 나머지는 DB 인덱스나 검색 엔진에 맡깁니다. (본문: "키가 끝없이 늘어나는 경우: 다시 읽히는 조합만 캐시합니다")

- **꼬리: 적중률이 낮아도 메모리가 넉넉하면 두어도 되지 않나요?**
  두 가지 비용이 남습니다. 미스마다 Redis 조회와 `SET` 왕복이 더해져 DB 부하는 줄지 않은 채 일만 늘고, 키가 요청량을 따라 늘다가 메모리 한도에 닿으면 축출 정책이 다른 캐시의 키까지 밀어내거나 기본 정책(`noeviction`)에서는 쓰기가 OOM 에러를 받습니다. 기본 에러 처리기라면 그 OOM 에러가 DB를 이미 읽은 요청을 실패시킵니다. 다만 미스 한 번이 아주 비싸다면 낮은 적중률로도 아끼는 DB 시간이 클 수 있으므로, 비율보다 줄어드는 DB 시간으로 판단합니다.
- **꼬리: 키 가짓수를 줄이려고 검색어를 소문자로 바꿔 키를 만들면 되나요?**
  실제 조회가 대소문자를 같게 다룰 때만 됩니다. 다르게 다루는데 키만 합치면 "Apple"의 결과가 "apple" 검색에 나가는 틀린 값이 됩니다. 앞뒤 공백처럼 결과를 바꾸지 않는 차이만, 메서드를 부르기 전에 인자에서 정리합니다.
- **꼬리: `condition`으로 범위를 좁히는 것과 키 식에서 필드를 빼는 것은 무엇이 다른가요?**
  필드를 빼면 다른 조건의 결과가 같은 키로 나가 틀린 값이 됩니다. 범위를 좁히면 값은 맞고, 빠진 요청이 매번 DB로 갈 뿐입니다.

**Q. `condition`과 `unless`는 어떻게 다른가요?**
`condition`은 메서드 실행 전에 평가해 거짓이면 캐시를 읽지도 쓰지도 않습니다. `unless`는 실행 후 반환값으로 평가해 참이면 쓰지만 않습니다. 그래서 반환값으로 거르려면 `unless`를 써야 하고, `condition`에 `#result`를 쓰면 `null`로 평가되어 캐시가 한 번도 동작하지 않을 수 있습니다. (본문: "`condition`에 `#result`를 쓰면 한 번도 캐시하지 않습니다")

- **꼬리: null 캐시를 끄면 `unless`가 꼭 필요한가요?**
  필요합니다. null을 허용하지 않는 Redis 캐시에 null을 넣으려 하면 메서드가 DB를 이미 읽은 뒤에 예외가 나고, 기본 처리기에서는 요청이 실패합니다. 처리기가 삼키게 되어 있으면 실패는 사라지지만 그 null은 매번 DB로 갑니다. (본문: "null 캐시를 끄고 `unless`를 빠뜨리면, DB는 읽었는데 요청이 실패합니다")

**Q. 캐시마다 TTL을 다르게 하려면 어떻게 하나요?**
Spring Boot 속성 `time-to-live`는 모든 캐시에 하나만 정합니다. `RedisCacheManagerBuilderCustomizer`에서 `withCacheConfiguration`으로 캐시 이름마다 설정을 줍니다. 이때 `builder.cacheDefaults()`에서 출발해 TTL만 바꿔야, 속성으로 정한 접두사와 null 설정이 유지됩니다. (본문: "캐시마다 TTL을 정하는 방법과 빠지기 쉬운 함정")

- **꼬리: 설정 파일에 `time-to-live`가 있는데 키에 TTL이 없다면요?**
  `RedisCacheConfiguration` 빈을 직접 등록했는지 봅니다. 그 빈이 있으면 Spring Boot는 TTL 같은 속성으로 기본 설정을 만들지 않고 그 빈을 기본 설정으로 씁니다. 그다음 캐시별 설정을 기본값에서 새로 만들지 않았는지 봅니다.
- **꼬리: 설정에 없는 캐시 이름을 막을 수는 없나요?**
  `disableCreateOnMissingCache()`를 부르면 미리 정하지 않은 이름에 첫 호출에서 예외가 납니다. 대신 캐시를 추가할 때마다 설정에도 등록해야 합니다.

**Q. `sync = true`를 붙이면 여러 서버에서 한 번만 DB를 읽나요?**
아닙니다. `sync`는 "없으면 불러와 넣기"를 캐시 구현에 한 번에 맡기는 표시이고, Spring Boot 자동 설정의 Redis 캐시는 잠그지 않으므로 같은 서버 안에서도 각자 DB를 읽습니다. 잠그는 쓰기 방식을 쓰면 서버 사이에서도 한 번만 읽지만 캐시 이름 전체에 락이 걸리는 대가가 있습니다. (본문: "`sync = true`는 서버 사이에서 아무것도 보장하지 않습니다", 상세는 캐시 스탬피드 문서)

- **꼬리: `sync = true`에서 Redis가 느려지면 무엇이 달라지나요?**
  읽기·실행·쓰기가 한 호출이라, 쓰기가 실패하면 Spring은 이를 읽기 실패로 받아 처리기에 넘깁니다. 처리기가 삼키게 되어 있으면 메서드를 한 번 더 실행하므로 DB 조회가 두 배가 될 수 있습니다. 소스로 추론한 동작이고 재현하지는 않았습니다.

**Q. Redis가 죽으면 `@Cacheable` 메서드는 DB를 읽나요?**
기본 설정에서는 아닙니다. 기본 에러 처리기가 예외를 그대로 던지므로 요청이 실패합니다. `CachingConfigurer`에서 `LoggingCacheErrorHandler` 같은 처리기로 바꾸면 미스로 처리되어 DB를 읽습니다. (본문: "모든 캐시 접근의 예외는 에러 처리기를 거칩니다")

- **꼬리: 처리기만 바꾸면 Redis 장애에도 서비스가 정상인가요?**
  두 가지가 남습니다. 미스로 넘어가는 것은 Redis 명령이 실패한 뒤이므로, Redis가 응답 없이 멈춘 경우 명령 타임아웃(설정하지 않으면 60초)을 기다립니다. 그리고 모든 요청이 DB로 가므로 DB가 그 부하를 견뎌야 합니다. 타임아웃과 DB 보호를 함께 정해야 넘기는 의미가 있습니다.
- **꼬리: 예외를 모두 삼키면 무엇이 위험한가요?**
  `@CacheEvict`의 삭제 실패도 삼켜집니다. 삭제가 실제로 실행되지 않았다면 DB는 바뀌었는데 캐시에 옛 값이 TTL까지 남고, API는 성공으로 응답해 에러율에도 드러나지 않습니다. 삭제 실패는 지표로 세고 경보를 걸거나 다시 던지도록 읽기 실패와 나눠 다룹니다.

**Q. 캐시에서 받은 객체를 고치면 캐시도 바뀌나요?**
Redis 캐시에서는 바뀌지 않습니다. 미스일 때는 직렬화해 넣은 뒤 객체를 돌려주고, 히트일 때는 호출마다 새로 역직렬화하기 때문입니다. 하지만 Spring의 기본 메모리 캐시는 참조를 저장하므로 고친 내용이 다른 호출에도 보입니다. 환경에 따라 동작이 달라지지 않게, 캐시에서 받은 객체는 고치지 않고 안쪽까지 읽기 전용인 `data class`의 `copy`로 새로 만듭니다. `copy`는 얕은 복사라 안의 가변 객체는 공유된다는 점에 주의합니다. (본문: "돌려받은 객체를 고쳐도 Redis 캐시는 바뀌지 않습니다")

- **꼬리: 그러면 Redis 캐시는 비용 없이 안전한가요?**
  히트마다 역직렬화와 객체 생성 비용을 앱 서버가 냅니다. 큰 객체를 자주 읽는 캐시에서는 이 비용이 CPU에 드러날 수 있습니다. 그리고 바이트에 클래스 형식이 묶이므로, 클래스를 바꾸는 배포에서 옛 값을 못 읽는 문제는 따로 남습니다(직렬화기 문서).
