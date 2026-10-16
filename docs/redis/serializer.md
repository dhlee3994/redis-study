# 배포했더니 Redis에 있던 값을 못 읽는다: 직렬화기 고르기

> **핵심 질문**: 배포 후 Redis에 이미 있던 값을 못 읽는 장애는 왜 생기고, Spring `RedisTemplate`의 직렬화기를 어떻게 골라야 막을 수 있는가?
>
> **범위**: Spring Data Redis가 제공하는 직렬화기 네 가지(`JdkSerializationRedisSerializer`, `GenericJackson2JsonRedisSerializer`, `Jackson2JsonRedisSerializer`, `StringRedisSerializer`)를 다룹니다. `RedisTemplate`을 중심으로 쓰고, 같은 직렬화기를 쓰는 Spring Cache(`@Cacheable`)와 Spring Session에서 장애가 어떻게 다르게 나타나는지 함께 봅니다. Protobuf, Kryo 같은 다른 형식과 Redis 서버 쪽 자료형 선택은 다루지 않습니다. 자료형과 Hash 내부 표현은 [객체 하나를 String에 담을까, Hash에 담을까](./string-vs-hash.md)에 있습니다.

## 배경 지식

### 이 문서의 예시: 상품 캐시 `product:42`

문서 끝까지 다음 장면 하나를 씁니다.

- 상품 상세 API는 DB에서 읽은 상품을 `ProductDto`로 만들어 Redis 키 `product:42`에 10분 TTL로 캐시합니다. 다음 요청은 DB 대신 이 값을 읽습니다.
- 처음 버전(v1)의 `ProductDto`는 필드 세 개입니다.

| 필드 | 값 |
| --- | --- |
| `id` | `42` |
| `name` | `무선 키보드` |
| `price` | `39000` |

- 다음 배포(v2)에서 할인율 필드 `discountRate`(값 `10`)를 더합니다. 다른 경우로, 클래스를 `com.example.product` 패키지에서 `com.example.catalog` 패키지로 옮기는 리팩터링도 봅니다.
- 서버는 여러 대이고 한 대씩 교체하며 배포합니다. 그래서 잠시 v1 서버와 v2 서버가 같은 `product:42`를 함께 읽고 씁니다.

이 문서에 나오는 바이트 수와 예외 메시지는 이 예시를 Java 클래스로 만들어 Spring Data Redis 3.5.13 직렬화기로 직접 직렬화·역직렬화해 본 결과입니다(근거 절의 "실측"). Redis 서버를 거치지 않은 실험이지만, Redis는 받은 바이트를 그대로 저장하고 돌려주므로 결과는 같습니다.

### 직렬화는 값과 함께 "어떻게 읽을지"를 적습니다

- **한 줄**: 직렬화는 메모리 안의 객체를 저장하거나 보낼 수 있는 바이트열로 바꾸는 일이고, 역직렬화는 바이트열에서 객체를 다시 만드는 일입니다.
- **비유**: 가구를 분해해 상자에 담으면서 조립 설명서를 함께 넣는 일입니다. 설명서에 "모델명 KB-42, 설계도 3판"이라고 적어 두면, 받는 사람이 같은 모델·같은 판의 도면을 갖고 있어야 조립할 수 있습니다.
- **실제로는**: 직렬화 형식마다 바이트에 적는 것이 다릅니다. Java 직렬화는 값과 함께 클래스 이름과 클래스 버전 번호를 적습니다. JSON은 필드 이름과 값을 적고, 설정에 따라 클래스 이름을 함께 적기도 합니다. 비유와 달리 "설명서"는 상자 안 바이트의 일부일 뿐이라, Redis는 그 안에 무엇이 적혔는지 모르고 바이트열로만 저장합니다. 이 내용은 [객체 하나를 String에 담을까, Hash에 담을까](./string-vs-hash.md)의 "세 층을 구분합니다: 자료형, 내부 표현, 직렬화"와 같습니다.
- **이 주제에서는**: 바이트에 클래스 이름이나 버전 번호를 적는 형식은, 읽는 쪽 코드의 클래스가 그 이름·번호와 맞아야만 읽힙니다. 장애는 여기서 시작합니다.

### 쓴 쪽과 읽는 쪽의 코드 버전이 다를 수 있습니다

- **한 줄**: Redis에 값을 쓴 애플리케이션과 그 값을 읽는 애플리케이션이 같은 코드라는 보장은 없습니다. 새 코드가 옛 코드가 쓴 값을 읽을 수 있는 성질을 **하위 호환**, 옛 코드가 새 코드가 쓴 값을 읽을 수 있는 성질을 **상위 호환**이라고 부릅니다.
- **비유**: 새 도면을 받은 공장이 옛 상자를 조립할 수 있는가(하위 호환), 옛 도면만 가진 공장이 새 상자를 조립할 수 있는가(상위 호환)의 두 질문입니다.
- **실제로는**: JPA 엔티티는 DB 컬럼과 매핑되고 스키마 변경을 마이그레이션 도구로 관리하지만, Redis에 직렬화해 넣은 객체에는 그런 관리 장치가 없습니다. 클래스를 바꾸는 순간 Redis에는 옛 형식의 값이 남아 있고, 그 값은 TTL이 끝나거나 누가 덮어쓸 때까지 그대로입니다.
- **이 주제에서는**: v2를 배포한 직후 v2 서버는 v1이 쓴 `product:42`를 읽어야 하고(하위 호환), 배포가 끝나기 전까지 아직 남은 v1 서버는 v2가 쓴 값을 읽어야 합니다(상위 호환). 둘 중 하나만 깨져도 장애입니다.

### 롤링 배포

- **한 줄**: 서버 여러 대를 한꺼번에 내리지 않고 한 대씩(또는 몇 대씩) 새 버전으로 교체하는 배포 방식입니다.
- **비유**: 공장 라인 다섯 개 중 하나씩 새 도면으로 바꾸는 동안, 옛 도면 라인과 새 도면 라인이 같은 창고(Redis)의 상자를 함께 꺼내 씁니다.
- **실제로는**: 무중단 배포에서 흔히 쓰는 방식입니다. 배포 중에는 로드 밸런서가 같은 사용자의 요청도 옛 서버와 새 서버에 번갈아 보낼 수 있습니다. 배포가 실패해 되돌리면(롤백) 이번에는 새 버전이 쓴 값을 옛 버전이 읽습니다.
- **이 주제에서는**: 롤링 배포에서는 하위 호환과 상위 호환이 **둘 다** 필요합니다. 한 번에 전부 바꾸는 배포라도, 롤백할 가능성이 있다면 상위 호환이 필요합니다.

### `RedisTemplate`은 네 칸에 직렬화기를 따로 둡니다

`RedisTemplate`은 Kotlin 객체와 Redis가 받는 바이트열 사이를 바꾸는 직렬화기를 네 곳에 따로 둡니다.

| 칸 | 무엇을 바꾸는가 | 예시에서 |
| --- | --- | --- |
| 키 직렬화기 | 키 이름 | `product:42` |
| 값 직렬화기 | String 값, List·Set 원소 등 | `ProductDto` 객체 |
| Hash 키 직렬화기 | Hash의 필드 이름(Redis 키 하나 아래에 있는 이름) | (Hash를 쓸 때) 필드 이름 |
| Hash 값 직렬화기 | Hash의 필드 값 | (Hash를 쓸 때) 필드 값 |

- **아무것도 정하지 않으면 네 칸 모두 Java 직렬화(`JdkSerializationRedisSerializer`)입니다.** 값뿐 아니라 키 이름까지 Java 직렬화합니다.
- `StringRedisTemplate`은 네 칸 모두 `StringRedisSerializer`(UTF-8 문자열)로 정해 둔 `RedisTemplate`입니다.
- Spring Boot는 두 빈을 자동으로 만듭니다. 이름이 `redisTemplate`인 `RedisTemplate<Object, Object>`는 직렬화기를 정하지 않은 채 만들어 네 칸 모두 Java 직렬화이고, `stringRedisTemplate`은 네 칸 모두 문자열입니다.
- Spring Cache의 `RedisCache`는 기본 설정에서 키는 문자열, 값은 Java 직렬화입니다. Spring Session은 키와 필드 이름은 문자열, 필드 값은 Java 직렬화입니다.

그래서 별도 설정 없이 Spring으로 Redis를 쓰면 대부분 어딘가에서 Java 직렬화를 쓰고 있습니다.

## 장애는 쓸 때가 아니라 읽을 때 드러납니다

v2를 배포한 직후 상품 상세 API에서 500 에러가 나기 시작하고, 로그에는 이런 예외가 찍힙니다.

```text
SerializationException: Cannot deserialize
  cause java.io.InvalidClassException: com.example.product.ProductDto; local class incompatible:
        stream classdesc serialVersionUID = 7168621133314933517, local class serialVersionUID = 7030662477259350875
```

이 장애에는 세 가지 특징이 있습니다.

1. **쓰기는 성공합니다.** 클래스가 바뀌었다는 사실은 쓰기에 영향을 주지 않습니다. 쓰는 쪽은 지금 코드의 클래스로 직렬화하면 되기 때문입니다. 실패는 다른 버전의 코드가 그 바이트를 읽을 때 나옵니다. 그래서 같은 버전 안에서 쓰고 읽는 테스트는 통과합니다. (쓰기부터 실패하는 경우도 따로 있습니다. `Serializable`을 구현하지 않은 객체를 Java 직렬화하거나, 필요한 모듈이 없는 타입을 JSON으로 쓰는 경우입니다. 이런 실패는 같은 버전 안에서 한 번 써 보기만 해도 드러나므로 이 문서의 장애와는 다릅니다.)
2. **배포 직후에 몰립니다.** Redis에 남아 있는 옛 값이 모두 실패 대상입니다. 캐시라면 TTL이 끝나 옛 값이 사라질 때까지, 세션이라면 세션이 만료될 때까지 이어질 수 있습니다.
3. **Redis는 정상입니다.** Redis는 저장한 바이트를 그대로 돌려줬습니다. Redis 지표를 봐도 원인이 보이지 않고, 애플리케이션 로그의 `SerializationException`과 그 원인 예외(cause)를 봐야 합니다.

이 실패는 직렬화기가 값에 **무엇을 적었는가**와 클래스를 **어떻게 바꿨는가**의 조합으로 정해집니다. 아래에서 차례로 봅니다.

## 네 직렬화기가 값에 적는 것

v1의 `product:42` 값을 네 직렬화기로 만들면 다음과 같습니다. 바이트 수는 직렬화 결과 바이트열의 길이입니다. Redis가 이 값을 담느라 쓰는 메모리와는 다릅니다.

| 직렬화기 | Redis에 들어가는 값 | 길이(바이트) | 값에 적힌 타입 정보 |
| --- | --- | --- | --- |
| `JdkSerializationRedisSerializer` | `\xac\xed\x00\x05sr\x00\x1ecom.example.product.ProductDto...`(바이너리) | 227 | 클래스 이름, `serialVersionUID`, 필드 이름과 타입 |
| `GenericJackson2JsonRedisSerializer` | `{"@class":"com.example.product.ProductDto","id":42,"name":"무선 키보드","price":39000}` | 91 | `@class`에 클래스 전체 이름 |
| `Jackson2JsonRedisSerializer`(`ProductDto` 전용) | `{"id":42,"name":"무선 키보드","price":39000}` | 49 | 없음 |
| `StringRedisSerializer` | 애플리케이션이 만든 문자열 그대로 | 문자열을 UTF-8로 바꾼 바이트 수(글자 수와 다름) | 없음 |

`무선 키보드`는 UTF-8로 16바이트입니다(한글 한 글자 3바이트 × 5, 공백 1). 표의 `\xac`처럼 `\x` 뒤에 16진수 두 자리를 쓴 것은 글자 네 개가 아니라 **바이트 하나**를 사람이 볼 수 있게 적은 표기입니다. `redis-cli`도 출력할 수 없는 바이트를 이렇게 보여 줍니다.

### Java 직렬화: 클래스 이름과 버전 번호를 적습니다

- 값 맨 앞의 `\xac\xed`는 Java 직렬화 형식임을 나타내는 고정된 두 바이트입니다. Redis에서 값이 이 두 바이트로 시작하면 Java 직렬화 값입니다.
- 그 뒤에 클래스 전체 이름 `com.example.product.ProductDto`, 클래스의 버전 번호 `serialVersionUID`, 필드 이름과 타입이 적힙니다. 읽을 때 Java는 이 이름으로 클래스를 찾고, 번호가 지금 클래스의 번호와 같은지 확인합니다.
- **`serialVersionUID`**는 클래스가 직렬화 형식의 몇 번째 판인지 나타내는 64비트 숫자입니다. 클래스에 직접 선언하지 않으면 Java가 클래스 이름, 구현한 인터페이스, 필드, 메서드 등으로 계산한 해시를 씁니다. 그래서 **선언하지 않은 클래스는 이 예시처럼 필드 하나만 더해도 번호가 바뀝니다**(명세상 `private static`, `private transient` 필드는 계산에서 빠지지만, 직렬화되는 보통 필드는 들어갑니다). 앞의 예외에서 7168621133314933517이 v1, 7030662477259350875가 v2의 계산값입니다. JDK의 `Serializable` 문서도 이 계산이 클래스 세부와 컴파일러에 민감하다며 직접 선언하기를 강하게 권합니다.
- 키에도 같은 일이 일어납니다. 기본 `redisTemplate`으로 `product:42`를 쓰면 Redis에 실제로 저장되는 키 이름은 `\xac\xed\x00\x05t\x00\nproduct:42`(17바이트)입니다. 문자열로 쓰면 10바이트입니다.

### `GenericJackson2JsonRedisSerializer`: JSON에 클래스 이름을 적습니다

- 이 직렬화기는 어떤 객체든 받아 JSON으로 만들고, 읽을 때 어떤 클래스로 만들지 알기 위해 `@class` 속성에 클래스 전체 이름을 함께 적습니다. 그래서 `RedisTemplate<String, Any>` 하나로 여러 타입을 담을 수 있습니다.
- 대신 Java 직렬화처럼 **클래스 이름이 값 안에 들어갑니다.** 버전 번호는 적지 않으므로 필드를 더하는 변경에는 Java 직렬화보다 덜 민감하지만, 클래스 이름을 바꾸는 변경에는 똑같이 깨집니다.
- 기본 생성자(`GenericJackson2JsonRedisSerializer()`)는 Spring Boot의 `ObjectMapper` 빈이 아니라 자기만의 `ObjectMapper`를 새로 만듭니다. 이 `ObjectMapper`에는 Kotlin 모듈이나 Java 시간 타입 모듈이 등록되지 않습니다.
- `Long` 같은 기본 타입의 감싼 클래스(wrapper)나 열거형 등 일부 타입에는 `@class`를 붙이지 않습니다.

### `Jackson2JsonRedisSerializer`: 읽을 타입을 코드가 정합니다

- 만들 때 `Jackson2JsonRedisSerializer(ProductDto::class.java)`처럼 타입 하나를 받습니다. 값에는 필드만 적고, 읽을 때는 받은 타입으로 만듭니다. **타입 정보가 값이 아니라 코드에 있습니다.**
- 그래서 클래스 이름이나 패키지가 바뀌어도, 필드 이름과 타입이 같으면 읽힙니다.
- 대가는 타입마다 `RedisTemplate`(또는 직렬화기)을 따로 둬야 한다는 점입니다. `ProductDto` 캐시와 `CouponDto` 캐시는 서로 다른 템플릿을 씁니다.

### `StringRedisSerializer`: 문자열만 다룹니다

- 문자열을 UTF-8 바이트로 바꿀 뿐입니다. 객체를 담으려면 애플리케이션이 먼저 JSON 문자열로 만들어 넘깁니다. `StringRedisTemplate`과 애플리케이션의 `ObjectMapper`를 함께 쓰는 방식이 이것입니다. 효과는 `Jackson2JsonRedisSerializer`와 같고, 직렬화를 Spring Data Redis 밖의 코드가 한다는 점만 다릅니다.
- **키에는 이 직렬화기를 씁니다.** 키 이름은 사람이 `redis-cli`로 찾고, 패턴으로 훑고, 다른 서비스와 공유하는 이름이기 때문입니다.

## 무엇을 바꾸면 못 읽는가

v1이 쓴 값을 바뀐 클래스로 읽어 본 결과입니다. "성공"은 예외 없이 객체가 만들어졌다는 뜻입니다.

| 바꾼 것 | Java 직렬화(`serialVersionUID` 선언 안 함) | Java 직렬화(`serialVersionUID` 처음부터 선언) | `GenericJackson2JsonRedisSerializer` | `Jackson2JsonRedisSerializer` |
| --- | --- | --- | --- | --- |
| 필드 추가, 새 코드가 옛 값 읽기 | 실패: `InvalidClassException`(번호 불일치) | 성공(`discountRate`는 `null`. 실험의 필드 타입이 `Integer`라서이며, 값이 없는 필드는 타입의 기본값으로 채워짐) | 성공 | 성공 |
| 필드 추가, 옛 코드가 새 값 읽기 | 실패: `InvalidClassException` | 성공(`discountRate`는 버려짐) | 실패: `UnrecognizedPropertyException` | 실패: `UnrecognizedPropertyException` |
| 패키지 이동 | 실패: `ClassNotFoundException` | (같은 이유로 실패) | 실패: 클래스를 찾을 수 없음 | 성공 |

"`serialVersionUID` 처음부터 선언" 열의 패키지 이동은 직접 재지 않았습니다. Java 직렬화는 값에 적힌 클래스 이름으로 클래스를 찾으므로, 번호 선언과 상관없이 왼쪽 열과 같은 이유로 실패합니다.

### Java 직렬화: 번호를 처음부터 선언해야 합니다

- 번호를 선언하지 않으면 이 예시처럼 필드 하나를 더해도 양쪽 방향 모두 깨집니다. 롤링 배포에서 가장 나쁜 경우입니다.
- 번호를 고정해 두면 Java 직렬화 명세가 "호환되는 변경"으로 정한 필드 추가는 양쪽 방향 모두 읽힙니다. 반면 기본형(`int`, `long` 등) 필드의 타입 변경, 클래스 계층 이동 같은 "호환되지 않는 변경"은 번호를 고정해도 깨집니다.
- **이미 저장된 값이 있는 클래스에 나중에 번호를 선언하면 그 순간 깨집니다.** v1 클래스에 `serialVersionUID = 1L`만 더하고 필드는 그대로 둔 채 v1의 값을 읽었더니 `InvalidClassException`이 났습니다. 저장된 값에는 계산된 번호(7168621133314933517)가 적혀 있기 때문입니다. 나중에 선언하려면 JDK의 `serialver` 도구로 **옛 클래스의 계산값**을 구해 그 값을 선언해야 합니다.
- Kotlin 클래스도 컴파일되면 JVM 클래스이므로, `Serializable`을 구현하면 같은 규칙을 따릅니다. 이 문서의 실험은 Java 클래스로 했습니다.

### JSON: 모르는 필드와 클래스 이름이 문제입니다

- Jackson은 기본 설정에서 **모르는 필드를 만나면 실패합니다**(`FAIL_ON_UNKNOWN_PROPERTIES`가 기본으로 켜져 있음). 그래서 필드를 더하면 새 코드는 옛 값을 읽지만, 옛 코드는 새 값의 `discountRate`를 몰라 실패합니다. 이 설정을 끈 `ObjectMapper`로 만든 `Jackson2JsonRedisSerializer`는 옛 코드가 새 값을 읽어 `discountRate`를 버렸습니다. 클래스에 `@JsonIgnoreProperties(ignoreUnknown = true)`를 붙여도 같은 효과입니다.
- 이 설정은 값에 **있는** 낯선 필드를 버리는 것이지, 값에 **없는** 필드를 채워 주지 않습니다. 실험의 Java 클래스는 빠진 필드를 `null`로 두었지만, Kotlin에서 기본값 없는 non-null 생성자 매개변수로 더하면 결과가 타입에 따라 갈립니다(Kotlin 모듈의 동작으로, 이 문서에서 재지 않음). `discountRate: String`처럼 참조 타입이면 옛 값에 그 필드가 없어 새 코드가 읽지 못합니다. `discountRate: Int`처럼 JVM 기본형이 되는 타입이면 Jackson 기본 설정에서는 실패하지 않고 `0`이 들어가, 할인이 없는 상품처럼 조용히 틀린 값이 됩니다. 새 필드에는 기본값을 주거나 nullable로 선언합니다.
- Spring Boot가 만드는 `ObjectMapper` 빈은 `spring.jackson.*` 설정으로 바꾸지 않았다면 이 설정을 끈 채로 만들어집니다(Spring의 `Jackson2ObjectMapperBuilder` 기본값). 반면 직렬화기가 스스로 만든 `ObjectMapper`는 켜져 있습니다. **같은 Jackson이라도 어떤 `ObjectMapper`를 쓰는지에 따라 상위 호환이 갈립니다.**
- `GenericJackson2JsonRedisSerializer`는 값에 적힌 `@class`로 클래스를 찾으므로, 패키지를 옮기면 옛 값의 `com.example.product.ProductDto`를 찾지 못해 실패합니다. `Jackson2JsonRedisSerializer`는 이름을 값에서 읽지 않으므로 같은 필드를 가진 `com.example.catalog.ProductDto`로 읽었습니다.
- 필드 이름을 바꾸는 것은 JSON 형식 모두에 "필드 하나를 지우고 하나를 더한 것"입니다. 옛 이름은 모르는 필드가 되고, 새 이름은 값에 없어 비어 있게 됩니다.

### 쓰기는 되는데 읽기만 안 되는 클래스

필드를 생성자로만 받고 기본 생성자(인자 없는 생성자)가 없는 클래스를 직렬화기가 만든 `ObjectMapper`로 다루면, 쓰기는 성공하고 읽기만 `InvalidDefinitionException: Cannot construct instance ... (no Creators, like default constructor, exist)`로 실패했습니다. Jackson이 객체를 만들 방법을 모르기 때문입니다.

이 실험은 Java 클래스로 했습니다. Kotlin `data class`는 모든 매개변수에 기본값이 있지 않은 한 기본 생성자가 없으므로 같은 상황이 될 것으로 추론합니다(직접 재지 않음). Kotlin 클래스의 생성자 정보를 Jackson이 자동으로 읽게 하려면 Kotlin 모듈(`jackson-module-kotlin`)이 등록된 `ObjectMapper`를 씁니다. 모듈 없이 `@JsonCreator` 같은 표시로 생성자를 직접 알려 주는 방법도 있지만, 클래스마다 붙여야 합니다. Spring Boot의 `ObjectMapper` 빈은 이 모듈이 클래스패스에 있으면 자동으로 등록하지만, `GenericJackson2JsonRedisSerializer()`가 스스로 만든 `ObjectMapper`에는 없습니다. 이 함정도 "쓰기는 성공한다"는 특징 때문에 같은 버전 안의 저장 테스트로는 드러나지 않습니다. 쓴 값을 다시 읽는 테스트를 둬야 합니다.

## 롤링 배포에서는 두 방향을 모두 읽어야 합니다

앞의 표를 배포 순서에 놓으면 이렇습니다. 예시는 v1 서버 세 대를 한 대씩 v2로 바꾸는 상황입니다.

```text
시간 →

v1 서버 A ─────────┤교체├─ v2 ───────────────────────────────
v1 서버 B ──────────────────────┤교체├─ v2 ──────────────────
v1 서버 C ───────────────────────────────────┤교체├─ v2 ─────

             ① A가 v2가 된 순간부터
                v2(A)는 v1이 쓴 product:42를 읽음     → 하위 호환 필요
                v1(B, C)은 v2(A)가 덮어쓴 product:42를 읽음 → 상위 호환 필요
                                              ② C까지 바뀌면 v1은 없어지지만,
                                                 Redis에는 v1이 쓴 값이 TTL만큼 남음
```

화살표 방향은 시간이고, `┤교체├`는 그 서버가 내려갔다 새 버전으로 올라오는 구간입니다. 캐시 키 하나를 서버 여러 대가 함께 덮어쓰므로, ① 구간에서 `product:42`는 v1 형식과 v2 형식 사이를 오갈 수 있습니다.

| 직렬화 방식 | ① 구간(두 버전 공존) | ② 이후(v1 형식 값이 남음) |
| --- | --- | --- |
| Java 직렬화, 번호 선언 안 함 | 서로의 값을 못 읽음 | v2가 옛 값을 못 읽음 |
| Java 직렬화, 번호 고정 | 필드 추가는 읽힘 | 필드 추가는 읽힘 |
| JSON, 모르는 필드에서 실패 | v1이 v2의 값을 못 읽음 | 읽힘 |
| JSON, 모르는 필드 무시 | 필드 추가는 읽힘(새 필드에 기본값이 있거나 nullable일 때) | 읽힘(같은 조건) |

그래서 필드를 **지우거나 이름을 바꾸는** 변경은 어떤 형식이든 한 번의 배포로 끝내지 않습니다. 먼저 "바뀐 뒤의 값도 읽을 줄 아는" 버전을 배포하고, 그 버전이 모든 서버에 퍼진 뒤에 "바뀐 형식으로 쓰는" 버전을 배포합니다. 읽기를 먼저 넓히고 쓰기를 나중에 바꾸는 두 단계 배포입니다. 롤백할 때도 첫 단계 버전까지만 되돌리면 새 형식의 값을 읽을 수 있습니다.

첫 단계에서 할 일은 변경 종류마다 다릅니다.

- **이름 변경**: 모르는 필드 무시만 켜면 옛 이름의 값은 버려지고, 새 이름의 필드는 값이 없는 채로 읽힙니다. 대부분은 예외 없이 `null`이나 기본값, Kotlin에서 JVM 기본형이 되는 타입이면 `0`이 들어가 조용히 틀린 값이 됩니다. 읽기가 실패하는 것은 그 필드를 필수로 표시했거나(`@JsonProperty(required = true)`), Kotlin에서 기본값 없는 non-null 참조 타입일 때입니다("JSON: 모르는 필드와 클래스 이름이 문제입니다"에서 본 동작). 그래서 두 이름을 같은 속성으로 읽게 하되, 단계마다 쓰는 이름을 구분합니다. Jackson이 JSON에 쓰는 기본 이름은 프로퍼티 이름이고, `@JsonProperty("이름")`으로 따로 정할 수 있습니다. `@JsonAlias`는 다른 이름도 같은 속성으로 읽게 하는 표시로, 읽을 때만 쓰이고 쓸 때는 기본 이름을 씁니다. 첫 단계에서는 코드의 프로퍼티 이름을 이미 바꿨더라도 `@JsonProperty`로 기본 이름을 옛 이름으로 두고 새 이름을 `@JsonAlias`로 더해, 옛 이름으로 쓰면서 두 이름을 모두 읽습니다. 모든 서버가 첫 단계 버전이 된 뒤 둘째 단계에서 기본 이름을 새 이름으로 바꾸고 옛 이름을 `@JsonAlias`로 남깁니다. 이 별칭은 옛 이름으로 쓰인 값이 TTL로 모두 사라지고 첫 단계 버전으로 롤백할 일이 없어진 뒤에 지웁니다.
- **필드 삭제**: 옛 코드가 그 필드가 없는 값을 읽게 됩니다. 첫 단계에서 그 필드를 nullable이나 기본값이 있게 바꾸고 그 값에 기대는 코드를 걷어 낸 뒤, 다음 배포에서 필드를 지웁니다. Java 직렬화 명세도 필드 삭제를 "호환되지 않는 변경"으로 분류합니다. 예외 없이 읽히더라도 옛 클래스는 그 필드를 기본값으로 채우기 때문입니다.

어느 쪽이든 확인할 것은 "예외가 나지 않는가"가 아니라 "읽은 값이 맞는가"입니다.

## 못 읽었을 때 서비스에 나타나는 모습: 캐시와 세션이 다릅니다

같은 역직렬화 실패라도 누가 그 값을 읽느냐에 따라 사용자가 보는 결과가 다릅니다.

### `RedisTemplate`으로 직접 읽으면 예외가 그대로 올라옵니다

`opsForValue().get("product:42")`는 값을 못 읽으면 `null`이 아니라 `SerializationException`을 던집니다. 코드가 "캐시에 없으면 DB에서 읽는다"를 `null` 검사로만 짰다면, 이 예외는 캐시 미스가 아니라 요청 실패가 됩니다. 캐시라면 예외를 잡아 미스로 처리하고, 그 키를 지우거나 새 값으로 덮어써 같은 실패가 되풀이되지 않게 합니다.

### `@Cacheable`도 기본 설정에서는 예외를 던집니다

Spring Cache는 캐시에서 읽다 난 예외를 `CacheErrorHandler`에 넘깁니다. 기본 구현(`SimpleCacheErrorHandler`)은 그 예외를 그대로 다시 던집니다. 그래서 `@Cacheable` 메서드는 DB를 대신 읽지 않고 실패합니다. "캐시는 없어도 되는 것"이라고 생각하기 쉽지만, 기본 설정에서는 캐시의 옛 값 하나가 API 전체를 실패시킵니다.

에러 처리기를 `LoggingCacheErrorHandler`나 직접 만든 구현으로 바꾸면, 예외를 로그로 남기고 캐시 미스로 처리해 메서드 본문(DB 조회)을 실행합니다. 대가는 장애가 조용해진다는 점입니다. 배포 직후 DB 부하가 늘어도 에러율에는 드러나지 않으므로, 캐시 에러 로그나 지표를 따로 봐야 합니다. 에러 처리기를 바꿨을 때 DB가 받는 부하와 그 밖의 함정은 [Spring `@Cacheable`의 함정](./spring-cacheable.md)에서 다룹니다.

### Spring Session은 그 세션을 읽는 요청이 모두 실패합니다

Spring Session은 요청이 세션을 처음 찾을 때 세션 Hash를 `HGETALL`로 통째로 읽고, **모든 필드 값을 그 자리에서 역직렬화합니다.** 세션 속성 하나라도 못 읽으면 세션 전체를 만들지 못하고, 이 경로에는 예외를 잡아 새 세션으로 바꾸는 처리가 없습니다. 그 요청이 쓰지 않는 속성이 원인이어도 마찬가지입니다.

그래서 캐시와 달리 "없는 것으로 치고 넘어가기"가 기본으로 일어나지 않습니다. 소스를 따라가면, 그 세션 쿠키로 오는 요청 중 세션을 읽는 요청(로그인 확인처럼 대부분의 요청이 해당)은 세션 키가 만료될 때까지 계속 실패합니다. 만료된 뒤에는 세션이 없는 것으로 보이므로 기존 로그인 정보가 사라져 사용자는 로그아웃된 상태가 됩니다. 새 세션은 애플리케이션이 세션을 만들려 할 때 생깁니다(소스로 추론한 동작이며 실행해 보지 않음). 세션의 기본 만료는 마지막 접근 후 30분입니다.

세션에는 내 클래스만 들어가지 않습니다. Spring Security는 로그인 정보(`SecurityContext`)를 세션 속성으로 저장하는데, Spring Security 클래스들의 `serialVersionUID`는 라이브러리 안의 상수 하나로 정해집니다(6.5.11에서 `620`). 이 번호는 내 코드가 아니라 **라이브러리 버전을 올릴 때** 바뀔 수 있습니다. Java 직렬화로 세션을 저장한다면, Spring Security를 올리는 배포 전에 이 상수가 바뀌는지 확인해야 합니다.

| 읽는 쪽 | 기본 설정에서 못 읽으면 | 사용자 영향 |
| --- | --- | --- |
| `RedisTemplate` 직접 사용 | `SerializationException` | 코드가 잡지 않으면 요청 실패 |
| `@Cacheable`(`RedisCache`) | 에러 처리기가 다시 던짐 | 요청 실패. 미스로 바꾸려면 에러 처리기 교체 |
| Spring Session | 세션 조회 자체가 실패 | 그 세션을 읽는 요청이 실패, 만료 후 로그인 정보 소멸 |

## `redis-cli`로 값을 읽을 수 있는가

장애를 조사할 때는 `redis-cli`로 값을 직접 봅니다. 이때 직렬화 형식이 조사 속도를 가릅니다.

- **Java 직렬화 키는 이름으로 찾을 수 없습니다.** 기본 `redisTemplate`으로 쓴 값은 `GET product:42`가 `(nil)`을 돌려줍니다. 실제 키 이름 앞에 `\xac\xed\x00\x05t\x00\n`이 붙어 있기 때문입니다. 같은 이유로 `stringRedisTemplate`으로 쓰고 `redisTemplate`으로 읽으면(또는 그 반대), 값이 있는데도 매번 캐시 미스가 납니다. 두 템플릿은 같은 이름을 서로 다른 키로 바꿉니다.
- **Java 직렬화 값은 사람이 읽기 어렵습니다.** 클래스 이름은 보이지만 숫자와 필드 값은 바이너리로 섞여 있습니다.
- **JSON 값은 읽힙니다.** 다만 `redis-cli`는 터미널에 출력할 때 출력할 수 없는 바이트를 `\xNN`으로 바꿔 보여 주므로, 한글은 `\xeb\xac\xb4...`처럼 보입니다. `redis-cli --raw`로 실행하면 바이트를 그대로 출력해 한글이 보입니다.

JSON이면 운영 중에 값을 읽고, 필요하면 손으로 고쳐 넣을 수도 있습니다. Java 직렬화 값은 형식 명세가 공개되어 있어 바이트 구조를 분석할 수는 있지만, 원래 객체로 되돌리려면 그 클래스를 가진 JVM 코드가 필요합니다.

## 값의 길이는 Hash 내부 표현까지 바꿉니다

직렬화 결과의 길이는 네트워크로 오가는 바이트와 Redis 메모리뿐 아니라, Hash에 담을 때 Redis가 고르는 내부 표현까지 바꿉니다. Redis 8.2는 Hash의 필드 이름이나 값 중 하나라도 `hash-max-listpack-value`(기본 64바이트)를 넘으면, 한 덩어리로 담는 `listpack` 대신 필드마다 메모리 조각을 따로 받는 `hashtable`로 바꿉니다. 이 기준과 대가는 [객체 하나를 String에 담을까, Hash에 담을까](./string-vs-hash.md)에 있습니다.

| 값 | Java 직렬화 | `GenericJackson2JsonRedisSerializer` | `Jackson2JsonRedisSerializer` |
| --- | --- | --- | --- |
| `Long` 하나(`39000`) | 82바이트 | 5바이트 | 5바이트 |
| `ProductDto`(v1) | 227바이트 | 91바이트 | 49바이트 |

`ProductDto`를 String 하나에 담는 이 문서의 예시에서는 내부 표현이 문제가 되지 않습니다. 문제는 객체를 Hash 필드로 나눠 담거나, Spring Session처럼 Hash의 필드 값에 직렬화한 값을 넣을 때입니다. Java 직렬화는 `Long` 하나로도 64바이트를 넘습니다. `GenericJackson2JsonRedisSerializer`도 객체 값에는 `@class`와 클래스 전체 이름이 붙어, 이 예시에서만 42바이트가 늘었습니다.

## 값에 클래스 이름을 적는 형식은 보안 경계가 됩니다

값에 클래스 이름을 적고, 읽을 때 그 이름의 클래스를 만드는 형식은 **Redis에 값을 쓸 수 있는 누군가가 애플리케이션이 만들 클래스를 고를 수 있다**는 뜻이 됩니다. 클래스패스에 있는 어떤 클래스는 만들어지는 과정에서 위험한 동작(파일 접근, 원격 코드 실행 등)을 일으킬 수 있고, 이를 이용한 공격을 역직렬화 공격이라고 부릅니다.

- **Java 직렬화**: Spring Data Redis 공식 문서("Serializers" 절)는 `RedisCache`와 `RedisTemplate`의 기본값이 Java 직렬화라는 점을 경고하며, 조작된 입력이 역직렬화 중에 원치 않는 코드를 실행할 수 있으므로 신뢰할 수 없는 환경에서 쓰지 말고 JSON 같은 다른 형식을 권합니다. JVM의 직렬화 필터(JEP 290)로 읽을 수 있는 클래스를 제한하는 방법도 안내합니다. Spring Data Redis의 Java 직렬화기는 이 필터를 스스로 걸지 않습니다.
- **`GenericJackson2JsonRedisSerializer`**: `@class`에 적힌 클래스를 만들도록 Jackson의 기본 타이핑을 켭니다. 이때 어떤 클래스를 허용할지 검사하는 검증기는 `ObjectMapper`의 기본값을 따르는데, Jackson 2.x의 기본값은 모든 하위 타입을 허용합니다(`LaissezFaireSubTypeValidator`). Jackson은 알려진 위험 클래스 목록을 따로 막지만, 목록에 없는 클래스까지 막지는 않습니다. Jackson 문서도 기본 타이핑을 신뢰할 수 없는 내용에 쓰면 보안 위험이 될 수 있다고 적습니다.
- **`Jackson2JsonRedisSerializer`와 `StringRedisSerializer`**: 만들 클래스를 코드가 정하므로 값으로 클래스를 고를 수 없습니다. 단, 대상 클래스 안에 클래스 이름으로 타입을 적는 설정(`@JsonTypeInfo(use = JsonTypeInfo.Id.CLASS)`)을 직접 붙였다면 그 자리에는 같은 문제가 다시 생깁니다.

전제는 "누가 Redis에 쓸 수 있는가"입니다. Redis가 내 서비스만 쓰고 인증과 네트워크 접근 제어가 되어 있다면 위험은 낮아집니다. 반대로 여러 서비스가 한 Redis를 공유하거나, Redis 접근 권한이 넓거나, 사용자 입력이 그대로 Redis 값이 되는 경로가 있다면, 그 경계 하나가 뚫렸을 때 Redis 장애가 애플리케이션 서버의 코드 실행으로 번질 수 있습니다.

## 어떻게 고르는가

| 쓰임 | 고를 것 | 이유 | 대가 |
| --- | --- | --- | --- |
| 키 이름(모든 경우) | `StringRedisSerializer` | `redis-cli`로 찾고, 패턴으로 훑고, 템플릿끼리 같은 키를 가리킴 | 없음 |
| 타입이 정해진 캐시 값(예: `ProductDto`) | `Jackson2JsonRedisSerializer` + 애플리케이션의 `ObjectMapper`(또는 `StringRedisTemplate` + `ObjectMapper`) | 값에 클래스 이름이 없어 패키지 이동·이름 변경에 강하고, 모르는 필드 무시로 상위 호환을 얻음. 값으로 클래스를 고를 수 없음 | 타입마다 템플릿이나 직렬화기를 따로 둠 |
| 한 템플릿에 여러 타입을 섞어야 함 | `GenericJackson2JsonRedisSerializer` | 템플릿 하나로 다룸 | 클래스 이름이 값에 들어가 리팩터링에 깨지고, 허용 클래스를 검증기로 제한해야 함. 값이 길어짐 |
| 숫자·짧은 문자열(카운터, 플래그) | `StringRedisSerializer` | `INCR`·`HINCRBY` 같은 서버 연산이 값을 숫자로 읽을 수 있음 | 객체는 담지 못함 |
| Java 직렬화 | 새로 고르지 않음 | — | 이미 쓰고 있다면 `serialVersionUID`를 고정하고 옮길 계획을 세움 |

`Jackson2JsonRedisSerializer`에 넘길 `ObjectMapper`는 Spring Boot의 `ObjectMapper` 빈을 `copy()`한 것을 권합니다. 이 빈은 모르는 필드를 무시하고 Kotlin 모듈을 등록해 두었으므로, 앞에서 본 상위 호환과 생성자 문제가 함께 풀립니다. 복사하는 이유는 공유 객체를 고치지 않기 위해서입니다. 특히 `GenericJackson2JsonRedisSerializer.builder().objectMapper(...)`에 빈을 그대로 넘기면, 빌더는 그 `ObjectMapper`에 직접 모듈(`NullValue` 직렬화기)을 등록하고, `defaultTyping(true)`까지 주면 기본 타이핑도 직접 켭니다. 그러면 HTTP 응답 JSON에도 `@class`가 붙을 수 있습니다.

반대로 생성자 `GenericJackson2JsonRedisSerializer(objectMapper)`로 매퍼를 넘기면 직렬화기는 기본 타이핑을 켜지 않습니다. 넘긴 매퍼에 타이핑 설정이 없으면 값에 `@class`가 적히지 않고, 읽을 때는 `ProductDto`가 아니라 `LinkedHashMap`이 돌아와 호출부의 형 변환에서 `ClassCastException`이 납니다(소스로 추론한 동작이며 실행해 보지 않음). 이것도 쓰기는 성공하는 실패이므로, 여러 타입을 쓰고 다시 읽어 원래 클래스로 돌아오는지 확인하는 테스트를 둡니다.

## 이미 저장된 값을 새 형식으로 옮기는 방법

직렬화기를 바꾸는 배포 자체가 "옛 형식의 값을 새 코드가 읽는" 상황입니다. 형식이 다르면 서로의 값을 전혀 읽지 못합니다. 실험에서 Java 직렬화기는 JSON 값을 `StreamCorruptedException: invalid stream header`로, Jackson은 Java 직렬화 값을 `JsonParseException`으로 거부했습니다. JSON끼리도 `GenericJackson2JsonRedisSerializer`는 `@class`가 없는 값을, 기본 설정의 `Jackson2JsonRedisSerializer`는 `@class`가 있는 값을 읽지 못했습니다.

데이터의 성격에 따라 방법이 다릅니다.

**캐시: 키 이름을 바꿔 옛 값을 버립니다.** `product:42`를 `product:v2:42`처럼 형식 버전이 들어간 키로 바꾸면, 새 코드는 옛 값을 아예 보지 않고 캐시 미스로 DB에서 읽습니다. 옛 키는 TTL로 사라집니다. 대가는 두 가지입니다. 배포 직후 캐시가 비어 DB 부하가 한꺼번에 몰리고, TTL이 끝날 때까지 옛 키와 새 키가 함께 메모리를 씁니다. 키를 바꾸지 못한다면 두 단계로 배포합니다. 먼저 캐시 에러 처리기를 미스로 처리하게 바꾼 버전을 모든 서버에 배포하고, 그다음에 쓰기 형식을 바꿉니다. 형식과 처리기를 한 번에 바꾸면, 아직 남은 옛 서버는 새 형식 값을 읽다 기본 처리기로 예외를 던집니다. 롤백도 첫 단계 버전까지만 안전합니다.

**세션처럼 버리면 안 되는 값: 두 형식을 모두 읽는 직렬화기를 거쳐 갑니다.**

1. 읽기만 두 형식을 지원하는 직렬화기를 배포합니다. 값이 `\xac\xed`로 시작하면 Java 직렬화로, 아니면 JSON으로 읽습니다. 쓰기는 아직 Java 직렬화입니다. 이 단계에서는 옛 서버와 새 서버가 같은 형식으로 쓰므로 롤링 배포가 안전합니다.
2. 모든 서버가 1단계 버전이 된 뒤, 쓰기를 JSON으로 바꾼 버전을 배포합니다. 이제 어느 서버든 두 형식을 다 읽으므로 공존 구간에도 안전합니다.
3. Java 직렬화 읽기 경로는 시간이 지났다고 지우지 않습니다. 기본 저장 모드는 이번 요청에서 `setAttribute`로 다시 넣은 속성과 접근 시각 같은 일부 메타데이터만 Hash 필드에 다시 씁니다. 객체 내용이 바뀌었는지 비교하지 않으므로, 계속 쓰이는 세션이라도 다시 넣지 않은 속성과 `creationTime`은 Java 직렬화 값으로 남을 수 있습니다. 옛 형식 쓰기가 멈춘 뒤 `SCAN`으로 세션 키를 커서가 끝날 때까지 전부 훑어, `\xac\xed`로 시작하는 필드 값이 하나도 없음을 확인한 뒤에 지웁니다. 일부만 훑은 표본에서 없었다는 것은 나머지에도 없다는 증거가 되지 않습니다. 확인할 수 없다면 두 형식 읽기를 계속 둡니다.

Spring Session이라면 이 직렬화기를 `springSessionDefaultRedisSerializer`라는 이름의 빈으로 두면 필드 값의 직렬화에 쓰입니다. 세션 속성에 들어간 객체를 JSON으로 읽을 수 있어야 하므로, Spring Security의 로그인 정보처럼 내 클래스가 아닌 객체를 JSON으로 다룰 준비가 함께 필요합니다.

## 흔한 오해 모아 보기

| 오해 | 실제 | 바로잡은 곳 |
| --- | --- | --- |
| 직렬화 문제는 저장할 때 드러난다 | 클래스가 바뀌어도 쓰기는 성공하고, 다른 버전이나 다른 설정의 코드가 읽을 때 실패합니다 | 장애는 쓸 때가 아니라 읽을 때 드러납니다 |
| JSON으로 바꾸면 클래스 변경에 안전하다 | `@class`를 적는 직렬화기는 패키지 이동에 깨지고, 기본 Jackson은 모르는 필드에서 실패합니다 | 무엇을 바꾸면 못 읽는가 |
| `serialVersionUID`는 지금 붙이면 된다 | 이미 저장된 값에는 계산된 번호가 적혀 있어, 다른 번호를 선언하는 순간 못 읽습니다 | Java 직렬화: 번호를 처음부터 선언해야 합니다 |
| 캐시는 못 읽으면 미스로 처리된다 | `RedisTemplate`과 기본 `@Cacheable`은 예외를 던집니다 | 못 읽었을 때 서비스에 나타나는 모습: 캐시와 세션이 다릅니다 |
| `GET product:42`가 `nil`이면 캐시에 없다 | 기본 `redisTemplate`은 키 이름도 Java 직렬화해, 다른 이름으로 저장되어 있을 수 있습니다 | `redis-cli`로 값을 읽을 수 있는가 |
| Spring의 Jackson 설정이 Redis 직렬화기에도 적용된다 | `GenericJackson2JsonRedisSerializer()`는 자기 `ObjectMapper`를 새로 만들어, Boot의 설정과 Kotlin 모듈을 쓰지 않습니다 | `GenericJackson2JsonRedisSerializer`: JSON에 클래스 이름을 적습니다 |

## 근거

**Spring Data Redis** 태그 [`3.5.13`](https://github.com/spring-projects/spring-data-redis/tree/3.5.13)(태그 객체 `dc9e6fc791640615dfdecaec43477e0f4de3d147`). 이 버전의 빌드가 선언한 Jackson은 2.19.4이고([spring-data-build `3.5.13`의 `parent/pom.xml`](https://github.com/spring-projects/spring-data-build/blob/3.5.13/parent/pom.xml) `jackson` 속성), 이 버전을 관리하는 Spring Boot 3.5.16은 Jackson 2.21.4를 씁니다. 아래 Jackson 주장은 두 버전의 소스에서 같음을 확인했습니다.

- [`RedisTemplate.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/core/RedisTemplate.java): `afterPropertiesSet`에서 기본 직렬화기가 없으면 `JdkSerializationRedisSerializer`를 만들고, 키·값·Hash 키·Hash 값 직렬화기 중 정하지 않은 칸을 모두 그것으로 채움
- [`StringRedisTemplate.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/core/StringRedisTemplate.java): 네 칸 모두 `RedisSerializer.string()`
- [`JdkSerializationRedisSerializer.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/serializer/JdkSerializationRedisSerializer.java): 역직렬화 실패를 `SerializationException("Cannot deserialize")`로 감싸 던짐
- [`GenericJackson2JsonRedisSerializer.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/serializer/GenericJackson2JsonRedisSerializer.java): 기본 생성자는 `new ObjectMapper()`를 만들고 `NullValue` 직렬화기만 등록; `createDefaultTypeResolverBuilder`가 `JsonTypeInfo.Id.CLASS`·`As.PROPERTY`로 기본 타이핑을 켜고 속성 이름 기본값은 `@class`; `TypeResolverBuilder.forEverything`은 `mapper.getPolymorphicTypeValidator()`를 씀; `useForType`은 열거형, 기본 타입과 감싼 클래스, `java`로 시작하는 패키지의 `final` 클래스(Kotlin 타입 제외)를 타입 정보 대상에서 뺌; `ObjectMapper`를 받는 생성자는 기본 타이핑을 켜지 않음; `deserialize(byte[])`는 `Object.class`로 읽고, `resolveType`은 기본 타이핑이 꺼져 있으면 값의 `@class`를 보지 않고 넘겨받은 타입(`Object`)으로 읽음; `GenericJackson2JsonRedisSerializerBuilder.build`는 넘겨받은 `ObjectMapper`에 `registerModule`을 직접 호출하고, 넘겨받은 경우에는 `defaultTyping(true)`일 때만 `setDefaultTyping`을 호출
- [`Jackson2JsonRedisSerializer.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/serializer/Jackson2JsonRedisSerializer.java): 생성자로 받은 타입으로 읽음
- [`RedisCacheConfiguration.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/cache/RedisCacheConfiguration.java): `defaultCacheConfig`는 키 `RedisSerializer.string()`, 값 `RedisSerializer.java(classLoader)`
- [`DefaultHashOperations.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/core/DefaultHashOperations.java)의 `entries`와 [`AbstractOperations.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/core/AbstractOperations.java)의 `deserializeHashMap`: `HGETALL` 결과의 모든 필드를 반복문 안에서 바로 역직렬화
- 공식 문서 [`template.adoc`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/antora/modules/ROOT/pages/redis/template.adoc) "Serializers" 절: `RedisCache`와 `RedisTemplate`의 기본값이 Java 직렬화라는 경고, 신뢰할 수 없는 환경에서 쓰지 말고 JSON 등을 권함, JVM 직렬화 필터(JEP 290) 안내. [`redis-cache.adoc`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/antora/modules/ROOT/pages/redis/redis-cache.adoc): `RedisCache` 기본 키 직렬화기 `StringRedisSerializer`, 값 직렬화기 `JdkSerializationRedisSerializer`

**Spring Framework** 태그 [`v6.2.19`](https://github.com/spring-projects/spring-framework/tree/v6.2.19)(Spring Data Redis 3.5.13 빌드와 Spring Boot 3.5.16이 쓰는 버전)

- [`DefaultDeserializer.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-core/src/main/java/org/springframework/core/serializer/DefaultDeserializer.java), [`ConfigurableObjectInputStream.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-core/src/main/java/org/springframework/core/ConfigurableObjectInputStream.java): `ObjectInputStream`의 `resolveClass`·`resolveProxyClass`를 클래스 로더용으로 바꿀 뿐, 직렬화 필터는 설정하지 않음
- [`AbstractCacheInvoker.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/cache/interceptor/AbstractCacheInvoker.java) `doGet`: 캐시 읽기 예외를 `handleCacheGetError`에 넘기고, 처리기가 던지지 않으면 미스로 처리
- [`CacheAspectSupport.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/cache/interceptor/CacheAspectSupport.java): 에러 처리기 기본값 `SimpleCacheErrorHandler`. [`SimpleCacheErrorHandler.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/cache/interceptor/SimpleCacheErrorHandler.java): `handleCacheGetError`가 예외를 다시 던짐. [`LoggingCacheErrorHandler.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-context/src/main/java/org/springframework/cache/interceptor/LoggingCacheErrorHandler.java): 로그만 남김
- [`Jackson2ObjectMapperBuilder.java`](https://github.com/spring-projects/spring-framework/blob/v6.2.19/spring-web/src/main/java/org/springframework/http/converter/json/Jackson2ObjectMapperBuilder.java): `FAIL_ON_UNKNOWN_PROPERTIES`를 따로 정하지 않으면 끔, `registerWellKnownModulesIfAvailable`에서 `jackson-module-kotlin`이 있으면 `KotlinModule` 등록

**Spring Boot** 태그 [`v3.5.16`](https://github.com/spring-projects/spring-boot/tree/v3.5.16)(커밋 `0566f6933049aca6bc5ffc6d559fffade9cd2e0c`). [`spring-boot-dependencies/build.gradle`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-dependencies/build.gradle)에서 Spring Data Bom 2025.0.13(Spring Data Redis 3.5.13 포함), Spring Session 3.5.7, Spring Security 6.5.11. [`gradle.properties`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/gradle.properties)에서 Jackson 2.21.4, Spring Framework 6.2.19

- [`RedisAutoConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/data/redis/RedisAutoConfiguration.java): `redisTemplate`은 직렬화기를 정하지 않은 `RedisTemplate<Object, Object>`, `stringRedisTemplate`은 `StringRedisTemplate`
- [`JacksonAutoConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/jackson/JacksonAutoConfiguration.java): `ObjectMapper` 빈을 `Jackson2ObjectMapperBuilder`로 만듦

**Jackson databind** 태그 [`jackson-databind-2.21.4`](https://github.com/FasterXML/jackson-databind/tree/jackson-databind-2.21.4)와 [`jackson-databind-2.19.4`](https://github.com/FasterXML/jackson-databind/tree/jackson-databind-2.19.4)

- [`DeserializationFeature.java`](https://github.com/FasterXML/jackson-databind/blob/jackson-databind-2.21.4/src/main/java/com/fasterxml/jackson/databind/DeserializationFeature.java): `FAIL_ON_UNKNOWN_PROPERTIES(true)`
- [`ObjectMapper.java`](https://github.com/FasterXML/jackson-databind/blob/jackson-databind-2.21.4/src/main/java/com/fasterxml/jackson/databind/ObjectMapper.java): `DEFAULT_BASE`의 하위 타입 검증기가 `LaissezFaireSubTypeValidator.instance`; 클래스 주석과 기본 타이핑 관련 주석이 신뢰할 수 없는 내용에 쓰면 보안 위험이 될 수 있다고 적음
- [`UntypedObjectDeserializer.java`](https://github.com/FasterXML/jackson-databind/blob/jackson-databind-2.21.4/src/main/java/com/fasterxml/jackson/databind/deser/std/UntypedObjectDeserializer.java): `Object` 타입으로 읽는 JSON 객체를 `LinkedHashMap`으로 만듦
- [`PropertyValueBuffer.java`](https://github.com/FasterXML/jackson-databind/blob/jackson-databind-2.21.4/src/main/java/com/fasterxml/jackson/databind/deser/impl/PropertyValueBuffer.java) `_findMissing`: 값에 없는 생성자 속성이 필수(`isRequired`)이면 읽기를 실패시킴
- [`JsonAlias.java`](https://github.com/FasterXML/jackson-annotations/blob/jackson-annotations-2.21/src/main/java/com/fasterxml/jackson/annotation/JsonAlias.java)(jackson-annotations 태그 `jackson-annotations-2.21`, databind 2.21.4가 쓰는 계열): 역직렬화에서 다른 이름도 같은 속성으로 받고, 직렬화에는 영향이 없어 기본 이름으로 씀
- [`SubTypeValidator.java`](https://github.com/FasterXML/jackson-databind/blob/jackson-databind-2.21.4/src/main/java/com/fasterxml/jackson/databind/jsontype/impl/SubTypeValidator.java): 알려진 위험 클래스 이름 목록(`DEFAULT_NO_DESER_CLASS_NAMES`)과 일부 접두어 규칙으로 역직렬화를 막음. [`BeanDeserializerFactory.java`](https://github.com/FasterXML/jackson-databind/blob/jackson-databind-2.21.4/src/main/java/com/fasterxml/jackson/databind/deser/BeanDeserializerFactory.java)의 `_validateSubType`이 이를 호출

**Spring Session** 태그 [`3.5.7`](https://github.com/spring-projects/spring-session/tree/3.5.7)(커밋 `bf21f5a75e430026a64f1e399b5bf63ea8cbd3e6`)

- [`RedisSessionRepository.java`](https://github.com/spring-projects/spring-session/blob/3.5.7/spring-session-data-redis/src/main/java/org/springframework/session/data/redis/RedisSessionRepository.java) `findById`: `opsForHash().entries()`로 읽고 예외를 잡지 않음, 읽은 Hash가 비어 있으면(키가 없으면) `null`을 돌려줌; `getSessionKey`: 키 이름은 `DEFAULT_KEY_NAMESPACE`(`spring:session`) 뒤에 `:sessions:<세션 ID>`; `saveMode` 기본값 `SaveMode.ON_SET_ATTRIBUTE`; 내부 `RedisSession` 생성자는 새 세션일 때만 `creationTime` 등을 변경분에 넣고, `SaveMode.ALWAYS`가 아니면 기존 속성을 변경분에 넣지 않음; `setAttribute`가 그 속성을 변경분에 넣고 `saveDelta`가 변경분만 `putAll`로 씀
- [`SessionRepositoryFilter.java`](https://github.com/spring-projects/spring-session/blob/3.5.7/spring-session-core/src/main/java/org/springframework/session/web/http/SessionRepositoryFilter.java): `getRequestedSession`이 `findById`를 부르고 예외를 잡지 않음, `doFilterInternal`의 `finally`에서 `commitSession`
- [`MapSession.java`](https://github.com/spring-projects/spring-session/blob/3.5.7/spring-session-core/src/main/java/org/springframework/session/MapSession.java): `DEFAULT_MAX_INACTIVE_INTERVAL_SECONDS = 1800`
- [`AbstractRedisHttpSessionConfiguration.java`](https://github.com/spring-projects/spring-session/blob/3.5.7/spring-session-data-redis/src/main/java/org/springframework/session/data/redis/config/annotation/web/http/AbstractRedisHttpSessionConfiguration.java) `createRedisTemplate`: 키와 Hash 키는 문자열 직렬화, `springSessionDefaultRedisSerializer` 빈이 있으면 그것을 기본 직렬화기로

**Spring Security** 태그 [`6.5.11`](https://github.com/spring-projects/spring-security/tree/6.5.11)

- [`SpringSecurityCoreVersion.java`](https://github.com/spring-projects/spring-security/blob/6.5.11/core/src/main/java/org/springframework/security/core/SpringSecurityCoreVersion.java): `SERIAL_VERSION_UID = 620L`. [`SecurityContextImpl.java`](https://github.com/spring-projects/spring-security/blob/6.5.11/core/src/main/java/org/springframework/security/core/context/SecurityContextImpl.java), [`UsernamePasswordAuthenticationToken.java`](https://github.com/spring-projects/spring-security/blob/6.5.11/core/src/main/java/org/springframework/security/authentication/UsernamePasswordAuthenticationToken.java): `serialVersionUID`를 이 상수로 선언

**Redis** 태그 [`8.2.10`](https://github.com/redis/redis/tree/8.2.10)(커밋 `86b20b1b9e5f03d86db72c3d59bec432e471fbc9`)

- [`src/redis-cli.c`](https://github.com/redis/redis/blob/8.2.10/src/redis-cli.c) `cliFormatReplyTTY`: 터미널 출력에서 문자열 응답을 `sdscatrepr`로 출력; `--raw` 옵션. [`src/sds.c`](https://github.com/redis/redis/blob/8.2.10/src/sds.c) `sdscatrepr`: `isprint`가 아닌 바이트를 `\x%02x`로 출력
- `hash-max-listpack-value` 기본값과 변환 조건의 근거는 [객체 하나를 String에 담을까, Hash에 담을까](./string-vs-hash.md)의 근거 절

**Java 직렬화 명세와 API 문서**(Java SE 25)

- [Java Object Serialization Specification 4장](https://docs.oracle.com/en/java/javase/25/docs/specs/serialization/class.html#stream-unique-identifiers) "Stream Unique Identifiers": `serialVersionUID`를 선언하지 않으면 클래스 이름, 인터페이스, 메서드, 필드 등의 해시를 씀
- [같은 명세 5장](https://docs.oracle.com/en/java/javase/25/docs/specs/serialization/version.html#compatible-changes) "Compatible Changes"(필드 추가 등), ["Incompatible Changes"](https://docs.oracle.com/en/java/javase/25/docs/specs/serialization/version.html#incompatible-changes)(필드 삭제: 옛 클래스는 그 필드를 기본값으로 읽음, 기본형 필드의 선언 타입 변경, 클래스 계층 이동 등)
- [같은 명세 6장](https://docs.oracle.com/en/java/javase/25/docs/specs/serialization/protocol.html) "Terminal Symbols and Constants": `STREAM_MAGIC = (short)0xaced`
- [`java.io.Serializable`](https://docs.oracle.com/en/java/javase/25/docs/api/java.base/java/io/Serializable.html): 기본 계산이 클래스 세부와 컴파일러에 민감하므로 `serialVersionUID`를 직접 선언하기를 강하게 권함

**실측**(이 문서를 쓰며 한 번 실행한 결과. Redis 서버를 거치지 않고 직렬화기만 호출했습니다)

- 환경: macOS, JDK 25.0.2(Temurin), Spring Data Redis 3.5.13, Spring Framework 6.2.19, Jackson 2.21.4. `ProductDto`는 기본 생성자와 getter·setter가 있는 Java 클래스(`Serializable` 구현)로 만들고, 버전마다 따로 컴파일해 한 JVM에서 쓴 바이트를 다른 클래스를 가진 JVM에서 읽었습니다.
- 길이: v1 `ProductDto`는 Java 직렬화 227바이트, `GenericJackson2JsonRedisSerializer` 91바이트, `Jackson2JsonRedisSerializer` 49바이트. v2(필드 추가)는 각각 314, 109, 67바이트. `Long` 39000은 Java 직렬화 82바이트, `GenericJackson2JsonRedisSerializer` 5바이트. 문자열 키 `product:42`는 Java 직렬화 17바이트, 문자열 10바이트. Java 직렬화 값의 첫 두 바이트는 `ac ed`
- "무엇을 바꾸면 못 읽는가" 표의 성공·실패와 예외 이름, 나중에 `serialVersionUID = 1L`을 선언한 클래스가 옛 값을 못 읽은 결과, `serialver`가 출력한 v1 계산값 7168621133314933517
- 모르는 필드를 무시하도록 설정한 `ObjectMapper`의 `Jackson2JsonRedisSerializer`로 옛 클래스가 새 값을 읽은 결과(성공)
- 기본 생성자가 없는 클래스: 세 직렬화기 모두 쓰기 성공, 읽기는 Java 직렬화만 성공하고 두 JSON 직렬화기는 `InvalidDefinitionException`
- 형식이 섞인 경우: Java 직렬화기가 JSON을 읽으면 `StreamCorruptedException: invalid stream header`, `GenericJackson2JsonRedisSerializer`가 Java 직렬화 값을 읽으면 `JsonParseException`, `@class` 없는 값을 읽으면 `InvalidTypeIdException`(missing type id property '@class'), 기본 `Jackson2JsonRedisSerializer`가 `@class` 있는 값을 읽으면 `UnrecognizedPropertyException`

## 실무 포인트

### 고르기 전에 답할 질문

| 중요도 | 질문 | 왜 |
| --- | --- | --- |
| 필수 | 지금 어떤 직렬화기를 쓰고 있는가? 키도 포함해서 | Spring의 기본값은 대부분 Java 직렬화이고, 기본 `redisTemplate`은 키까지 Java 직렬화합니다. 설정하지 않으면 모르는 사이에 이 기본값을 씁니다 |
| 필수 | 값을 못 읽으면 요청이 실패하는가, 미스로 넘어가는가? | `RedisTemplate`과 기본 `@Cacheable`은 실패합니다. 캐시라면 미스로 처리하는 경로를 둡니다 |
| 필수 | 값에 클래스 이름이 들어가는가? | 들어가면 패키지 이동·이름 변경이 저장된 값을 깨뜨리고, Redis에 쓸 수 있는 쪽이 만들 클래스를 고를 수 있게 됩니다 |
| 필수 | 옛 코드가 새 값을 읽을 수 있는가? | 롤링 배포와 롤백에서 필요합니다. JSON이라면 모르는 필드를 무시하는 설정인지 확인합니다 |
| 알면 유리 | 세션에 무엇을 넣는가? | 세션 속성 하나를 못 읽어도 그 세션을 읽는 요청이 모두 실패합니다. 라이브러리 클래스(Spring Security 등)도 들어갑니다 |
| 알면 유리 | 값을 Hash 필드에 넣는가? | 직렬화 결과가 64바이트를 넘으면 Hash가 `hashtable`이 되어 키당 메모리가 늘어납니다 |
| 참고 | `GenericJackson2JsonRedisSerializer`를 꼭 써야 하는가? | 써야 한다면 허용 클래스를 제한하는 검증기를 넘긴 `ObjectMapper`로 만듭니다 |

### 개발자가 정할 수 있는 것과 관찰해야 하는 것

| 정할 수 있는 것 | 관찰해야 하는 것 |
| --- | --- |
| 템플릿 네 칸의 직렬화기, `RedisCacheConfiguration`의 값 직렬화기, `springSessionDefaultRedisSerializer` | 애플리케이션 로그의 `SerializationException`과 원인 예외(cause): `InvalidClassException`, `ClassNotFoundException`, `UnrecognizedPropertyException` 등 |
| 직렬화기에 넘길 `ObjectMapper`(모르는 필드 무시, Kotlin 모듈) | 배포 직후 캐시 에러 로그 수와 DB 조회 수 |
| `CacheErrorHandler` 구현 | `redis-cli GET <키>`(터미널 출력)로 본 값이 `\xac\xed`로 시작하는가, `redis-cli --raw GET <키>`로 본 JSON에 `@class`가 있는가 |
| 형식 버전이 들어간 키 이름(`product:v2:42`) | `SCAN`으로 본 키 이름에 `\xac\xed`가 붙어 있는지 |
| 클래스의 `serialVersionUID` 선언 | 저장된 값을 다른 버전 코드로 읽는 테스트의 결과 |

### 증상과 확인 순서

**증상: "배포 직후 특정 API만 500이 나고, 시간이 지나면 저절로 줄어든다."**

1. 로그에서 `SerializationException`의 원인 예외를 봅니다. `InvalidClassException`의 `serialVersionUID` 불일치면 Java 직렬화 클래스가 바뀐 것이고, `ClassNotFoundException`이나 "Cannot locate class"면 클래스 이름이 바뀐 것, `UnrecognizedPropertyException`이면 옛 코드가 새 필드를 만난 것입니다.
2. "저절로 줄어든다"는 옛 값이 TTL로 사라지고 있다는 신호일 수 있습니다. 실패한 키의 `TTL`(남은 수명)과 값 형식은 이 가설을 좁히는 단서일 뿐 원인을 확정하지 않습니다. 같은 Java 직렬화 형식이면 클래스 버전이 달라도 앞부분이 똑같기 때문입니다. 실패 건수가 옛 키의 만료 시각에 맞춰 줄어드는지 함께 봅니다. 캐시라면 기다리는 동안 해당 키 패턴을 지우거나, 에러 처리기를 미스로 바꾸는 배포를 검토합니다. 키를 한꺼번에 지우면 DB에 부하가 몰리므로, 지울 키 수와 DB 여유를 먼저 봅니다.
3. 재발을 막으려면 해당 클래스의 변경을 두 단계 배포로 나누거나, 클래스 이름을 적지 않는 직렬화기로 옮깁니다.

**증상: "배포 후 일부 사용자가 계속 에러를 보다가, 얼마 뒤 로그아웃된다."**

1. 세션 저장소가 Redis인지, 세션 필드 값이 Java 직렬화인지(`\xac\xed`로 시작하는지) 확인합니다.
2. 이번 배포에서 세션에 넣는 클래스나 Spring Security 버전이 바뀌었는지 봅니다.
3. 옛 버전이 새 버전이 쓴 값을 읽을 수 있다면 롤백이 빠른 선택지입니다. 읽을 수 없다면 롤백도 같은 장애를 반대 방향으로 만듭니다.
4. 롤백도 막혔다면 세션에 다시 로그인해서는 되살릴 수 없는 상태가 있는지로 고릅니다. 없다면 실패하는 세션 키(`spring:session:sessions:<세션 ID>`)를 지웁니다. 지운 뒤에 오는 요청부터 세션이 없는 것으로 보이므로 그 세션의 실패는 멈추고, 대가는 그 사용자들이 다시 로그인해야 한다는 점입니다. 있다면 두 형식(또는 두 클래스 버전)을 모두 읽는 수정본을 배포하고, 그동안 장애가 이어지는 것을 감수합니다.
5. 이후 `serialVersionUID` 고정, 세션에 넣는 객체 줄이기, 두 형식을 읽는 직렬화기를 거친 JSON 전환 중에서 고릅니다.

**증상: "값은 분명히 넣었는데 캐시가 항상 미스다."**

1. 쓰는 코드와 읽는 코드가 같은 템플릿(같은 키 직렬화기)을 쓰는지 봅니다. `redisTemplate`과 `stringRedisTemplate`이 섞여 있으면 같은 이름이 서로 다른 키가 됩니다.
2. `redis-cli`에서 `SCAN 0 MATCH *product:42*`로 찾아, 키 이름 앞에 `\xac\xed`가 붙은 키가 있는지 확인합니다.

## 말로 설명하기

### 30초 버전

배포 후 Redis 값을 못 읽는 장애는 직렬화기가 값에 클래스 이름이나 클래스 버전 번호를 적기 때문에 생깁니다. Spring의 기본값인 Java 직렬화는 둘 다 적어서, 번호를 선언하지 않은 클래스는 필드 하나만 더해도 옛 값을 못 읽습니다. 클래스가 바뀌어도 쓰기는 성공하고 다른 버전이 읽을 때만 터져서 테스트로 잘 안 잡힙니다. 그래서 키는 문자열로, 값은 타입을 코드가 정하는 JSON으로 쓰고, 모르는 필드를 무시하게 설정하고 새 필드에는 기본값을 줘서 롤링 배포 중 옛 서버와 새 서버가 서로의 값을 읽을 수 있게 합니다.

### 2분 버전

Redis는 받은 바이트를 그대로 저장할 뿐이고, 그 바이트를 객체로 되돌리는 일은 애플리케이션의 직렬화기가 합니다. 문제는 직렬화기가 값에 무엇을 적느냐입니다. Spring `RedisTemplate`은 아무것도 정하지 않으면 키·값·Hash 키·Hash 값 네 칸 모두 Java 직렬화를 씁니다. Java 직렬화는 클래스 이름과 `serialVersionUID`를 적는데, 번호를 선언하지 않으면 클래스 구조로 계산하므로 필드 하나만 더해도 바뀝니다. 그러면 새 버전은 옛 값을, 옛 버전은 새 값을 못 읽습니다. 롤링 배포에서는 두 버전이 같은 키를 함께 읽고 쓰므로 양쪽 방향이 다 필요합니다.

JSON으로 바꿔도 끝이 아닙니다. `GenericJackson2JsonRedisSerializer`는 `@class`에 클래스 이름을 적어 패키지를 옮기면 깨지고, Jackson 기본값은 모르는 필드에서 실패해 옛 서버가 새 필드를 만나면 깨집니다. 그래서 타입을 코드가 정하는 `Jackson2JsonRedisSerializer`에, 모르는 필드를 무시하고 Kotlin 모듈이 있는 `ObjectMapper`를 넘깁니다. 값에 클래스 이름이 없으니 Redis에 쓸 수 있는 누군가가 만들 클래스를 고르는 위험도 사라집니다.

못 읽었을 때의 영향도 다릅니다. `RedisTemplate`과 기본 `@Cacheable`은 예외를 던지므로 캐시인데도 요청이 실패합니다. 에러 처리기를 바꿔 미스로 처리합니다. Spring Session은 세션 속성 하나만 못 읽어도 그 세션을 읽는 요청이 모두 실패합니다. 이미 저장된 값을 옮길 때는 캐시라면 키 이름에 형식 버전을 넣어 옛 값을 버리고, 세션이라면 두 형식을 읽는 직렬화기를 먼저 배포한 뒤 쓰기를 바꿉니다.

### 예상 질문과 꼬리 질문

**Q. 배포 후 캐시 값을 못 읽는 장애는 왜 생기나요?**
직렬화기가 값에 클래스 이름이나 버전 번호를 적고, 읽을 때 그것이 지금 코드의 클래스와 맞아야 하기 때문입니다. Java 직렬화는 `serialVersionUID`를 적는데, 선언하지 않으면 클래스 구조로 계산되어, 직렬화되는 필드 하나만 더해도 바뀝니다. 그러면 Redis에 남은 옛 값은 TTL이 끝날 때까지 못 읽는 값이 됩니다. (본문: "네 직렬화기가 값에 적는 것", "무엇을 바꾸면 못 읽는가")

- **꼬리: 테스트에서는 왜 안 잡혔나요?**
  클래스가 바뀌어도 직렬화는 지금 클래스로 하므로 성공하고, 실패는 다른 버전의 코드가 그 바이트를 읽을 때 납니다. 같은 버전 안에서 쓰고 읽는 테스트는 통과합니다. 기본 생성자가 없는 클래스를 JSON 직렬화기로 다룰 때처럼 같은 버전 안에서도 읽기만 실패하는 경우가 있으므로, 최소한 쓴 값을 다시 읽는 테스트는 둬야 합니다.
- **꼬리: 그럼 `serialVersionUID`를 지금 붙이면 되나요?**
  이미 저장된 값에는 계산된 번호가 적혀 있어서, 다른 번호를 선언하는 순간 그 값들을 못 읽습니다. 나중에 붙이려면 `serialver`로 옛 클래스의 계산값을 구해 그 값을 선언해야 합니다. 번호를 고정해도 기본형 필드의 타입 변경이나 클래스 이름 변경은 막지 못합니다.

**Q. JSON 직렬화기로 바꾸면 해결되나요?**
어떤 JSON 직렬화기냐에 따라 다릅니다. `GenericJackson2JsonRedisSerializer`는 `@class`에 클래스 전체 이름을 적어서 패키지를 옮기면 옛 값을 못 읽습니다. 또 Jackson 기본값은 모르는 필드에서 실패하므로, 필드를 더한 새 버전의 값을 옛 버전이 읽으면 실패합니다. 타입을 코드가 정하는 `Jackson2JsonRedisSerializer`에 모르는 필드를 무시하는 `ObjectMapper`를 넘기고, 새 필드에 기본값을 주거나 nullable로 선언해야 필드 추가와 패키지 이동을 함께 견딥니다. 모르는 필드 무시는 옛 값에 없는 필드를 채워 주지 않기 때문입니다. (본문: "JSON: 모르는 필드와 클래스 이름이 문제입니다")

- **꼬리: 필드를 지우거나 이름을 바꾸는 것은요?**
  어떤 형식이든 한 번의 배포로는 안전하지 않습니다. 이름 변경은 옛 이름을 지우고 새 이름을 더한 것과 같아서, 한쪽은 모르는 필드가 되고 다른 쪽은 값이 비게 됩니다. 모르는 필드 무시는 옛 이름의 값을 버릴 뿐이라, 새 필드는 대부분 `null`이나 기본값으로 조용히 틀린 값이 되고, 필수로 표시했거나 Kotlin의 기본값 없는 non-null 참조 타입이면 읽기가 실패합니다. 그래서 바뀐 뒤의 값도 읽을 줄 아는 버전을 먼저 모든 서버에 퍼뜨립니다. 이름 변경이면 `@JsonProperty`로 옛 이름을 기본 이름으로 두어 계속 옛 이름으로 쓰면서 새 이름을 `@JsonAlias`로 더해 두 이름을 모두 읽게 하고, 삭제면 그 필드를 nullable이나 기본값이 있게 바꿔 둡니다. 그다음에 바뀐 형식으로 쓰는 버전을 배포합니다. 이름 변경이라면 이때 기본 이름을 새 이름으로 바꾸고 옛 이름을 `@JsonAlias`로 남겼다가, 옛 이름의 값이 TTL로 사라지고 롤백할 일이 없어진 뒤에 지웁니다.
- **꼬리: 타입마다 템플릿을 따로 두는 게 번거롭지 않나요?**
  번거로운 것이 대가입니다. 대신 값에 클래스 이름이 없어 리팩터링에 강하고, 값으로 만들 클래스를 고를 수 없습니다. 여러 타입을 한 템플릿에 꼭 섞어야 할 때만 `GenericJackson2JsonRedisSerializer`를 쓰고, 그때는 허용 클래스를 검증기로 제한합니다.

**Q. 롤링 배포에서는 무엇이 추가로 필요한가요?**
새 코드가 옛 값을 읽는 것(하위 호환)뿐 아니라 옛 코드가 새 값을 읽는 것(상위 호환)도 필요합니다. 배포 중에는 두 버전이 같은 키를 함께 덮어쓰고, 롤백하면 옛 버전이 새 버전의 값을 읽기 때문입니다. Java 직렬화에 번호를 선언하지 않았다면 두 방향 모두 깨지고, 기본 Jackson은 상위 호환이 깨집니다. (본문: "롤링 배포에서는 두 방향을 모두 읽어야 합니다")

- **꼬리: 한 번에 전부 교체하는 배포라면 상위 호환은 필요 없지 않나요?**
  롤백하지 않는다면 그렇습니다. 하지만 배포가 실패해 되돌리면 옛 버전이 새 버전이 쓴 값을 읽게 되므로, 롤백 가능성을 남겨 두려면 상위 호환이 필요합니다.

**Q. 캐시는 못 읽어도 DB에서 다시 읽으면 되지 않나요?**
기본 설정에서는 그렇게 되지 않습니다. `RedisTemplate`은 `null`이 아니라 `SerializationException`을 던지고, `@Cacheable`의 기본 에러 처리기는 그 예외를 다시 던집니다. 그래서 캐시의 옛 값 하나가 API 실패가 됩니다. 에러 처리기를 바꾸거나 직접 예외를 잡아야 미스로 처리됩니다. (본문: "못 읽었을 때 서비스에 나타나는 모습: 캐시와 세션이 다릅니다")

- **꼬리: 미스로 처리하면 문제가 없나요?**
  장애가 조용해집니다. 옛 값을 읽을 때마다 DB를 읽으므로 배포 직후 DB 부하가 늘지만 에러율에는 드러나지 않습니다. 캐시 에러 로그나 지표를 따로 보고, 실패한 키는 새 값으로 덮어써 같은 실패가 되풀이되지 않게 합니다.

**Q. 세션은 캐시와 무엇이 다른가요?**
Spring Session은 세션을 찾을 때 모든 필드 값을 그 자리에서 역직렬화하고, 실패를 잡아 새 세션으로 바꾸는 처리가 없습니다. 그래서 속성 하나만 못 읽어도 그 세션을 읽는 요청은 모두 실패합니다. 소스를 따라가면 세션 키가 만료된 뒤에야 세션이 없는 것으로 보여, 사용자는 에러를 보다가 로그아웃된 상태가 됩니다. (본문: "Spring Session은 그 세션을 읽는 요청이 모두 실패합니다")

- **꼬리: 내 클래스를 안 바꿨는데도 생길 수 있나요?**
  생길 수 있습니다. 세션에는 Spring Security의 로그인 정보도 Java 직렬화로 들어가고, 그 클래스들의 `serialVersionUID`는 라이브러리 안의 상수로 정해집니다. 라이브러리를 올릴 때 이 상수가 바뀌면 내 코드 변경 없이도 같은 장애가 납니다. 업그레이드 전에 확인해야 합니다.
- **꼬리: 세션 직렬화기를 JSON으로 바꾸면 되나요?**
  바꾸는 배포 자체가 옛 형식의 값을 새 코드가 읽는 상황이라, 그냥 바꾸면 모든 세션을 못 읽습니다. 두 형식을 모두 읽는 직렬화기를 먼저 배포하고, 모든 서버에 퍼진 뒤 쓰기를 JSON으로 바꿉니다. Spring Security 객체처럼 내 클래스가 아닌 객체를 JSON으로 다룰 준비도 필요합니다.

**Q. `GenericJackson2JsonRedisSerializer`가 보안 위험이라는 말은 맞나요?**
조건이 붙습니다. 이 직렬화기는 값의 `@class`에 적힌 클래스를 만들고, 기본 검증기는 모든 하위 타입을 허용합니다. Jackson이 알려진 위험 클래스 목록은 막지만 목록 밖은 막지 않습니다. 그래서 Redis에 값을 쓸 수 있는 누군가가 만들 클래스를 고를 수 있게 됩니다. Java 직렬화도 같은 종류의 위험이 있고, Spring Data Redis 문서가 직접 경고합니다. 실제 위험의 크기는 누가 Redis에 쓸 수 있는가에 달려 있습니다. (본문: "값에 클래스 이름을 적는 형식은 보안 경계가 됩니다")

- **꼬리: Redis에 쓸 수 있으면 어차피 데이터를 망칠 수 있는데, 차이가 있나요?**
  있습니다. 값을 망치면 잘못된 데이터가 서비스에 퍼지지만, 역직렬화 공격은 그에 더해 Redis 접근 권한을 애플리케이션 서버의 코드 실행으로 넓힐 수 있습니다. 여러 서비스가 Redis를 공유하거나 Redis 접근 제어가 약할수록 이 차이가 커집니다.

**Q. Boot의 `ObjectMapper` 빈을 직렬화기에 그대로 넘기면 안 되나요?**
`Jackson2JsonRedisSerializer`라면 설정을 바꾸지 않으므로 문제가 적지만, 공유 객체이니 `copy()`해서 넘기는 편이 안전합니다. `GenericJackson2JsonRedisSerializer`의 빌더는 넘겨받은 `ObjectMapper`에 모듈을 직접 등록하고, `defaultTyping(true)`를 주면 기본 타이핑도 직접 켭니다. 빈을 그대로 넘긴 채 타이핑을 켜면 HTTP 응답 JSON에도 `@class`가 붙을 수 있습니다. (본문: "어떻게 고르는가")

- **꼬리: 왜 굳이 Boot의 `ObjectMapper`에서 출발하나요?**
  Boot가 만든 `ObjectMapper`는 모르는 필드를 무시하고, 클래스패스에 있으면 Kotlin 모듈을 등록합니다. 직렬화기가 스스로 만든 `ObjectMapper`에는 둘 다 없어서, 상위 호환이 깨지고 Kotlin `data class`를 읽지 못할 수 있습니다.

**Q. `redis-cli`로 `GET product:42`를 했더니 `nil`인데 앱은 캐시를 읽고 있습니다. 왜 그런가요?**
기본 `redisTemplate`은 키 이름도 Java 직렬화하므로, 실제 키는 `product:42` 앞에 `\xac\xed\x00\x05t\x00\n`이 붙은 다른 이름입니다. `SCAN`으로 패턴을 찾아 확인할 수 있습니다. 같은 이유로 `redisTemplate`으로 쓰고 `stringRedisTemplate`으로 읽으면 매번 미스입니다. 키는 언제나 문자열 직렬화기를 씁니다. (본문: "`redis-cli`로 값을 읽을 수 있는가")
