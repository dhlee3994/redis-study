# Redis 세션 저장소: 서버 여러 대가 로그인 상태를 함께 쓰면 무엇을 얻고 무엇에 기대게 되는가

> **핵심 질문**: 앱 서버 여러 대가 로그인 세션을 Redis에 함께 두면 만료·로그아웃·동시 로그인 제한은 어떻게 동작하는가? 요청마다 Redis에 무엇을 몇 번 보내며, Redis가 느리거나 죽거나 키를 잃으면 사용자에게 무엇이 일어나는가?
>
> **범위**: Spring MVC(서블릿) 앱이 Spring Security로 로그인하고, Spring Session Data Redis로 세션을 Redis에 두는 경우를 다룹니다. WebFlux용 리액티브 저장소, JDBC 세션 저장소, 자동 로그인(remember-me)은 다루지 않습니다. 세션 값을 바이트로 바꾸는 직렬화와, 세션을 String이 아니라 Hash에 담는 이유는 다른 문서에 있어 한 줄로만 짚습니다.
>
> **출처와 설계의 구분**: Spring Session·Spring Security·Spring Boot의 동작과 기본값, Redis 명령의 성질은 "근거"에 적은 소스와 공식 문서로 확인했습니다. 소스를 따라가 추론했지만 실행해 보지 않은 동작에는 그 자리에 "소스로 추론"이라고 적었습니다. 장애 시 대응 방식 고르기와 JWT 비교는 이 문서의 판단입니다. 재현해 보지는 않았습니다.

## 배경 지식

### 이 문서의 예시: 앱 서버 두 대, 회원 neo, 노트북과 휴대폰

문서 끝까지 다음 장면 하나를 씁니다.

- 쇼핑몰 API는 앱 서버 **A, B 두 대**입니다. 앞의 로드 밸런서가 요청을 두 서버에 번갈아 보냅니다.
- 세션은 Redis 한 대에 둡니다. 이 Redis에는 복제본이 하나 있고, 주 서버가 죽으면 복제본이 주 서버로 승격됩니다.
- 세션 만료는 기본값인 **마지막 접근 후 30분**입니다.
- 서비스 규칙: **한 회원은 한 곳에서만 로그인**할 수 있습니다. 새 곳에서 로그인하면 이전 곳은 로그아웃됩니다.
- 회원 neo의 하루는 다음과 같습니다. 세션 ID는 실제로는 36자 UUID지만 읽기 쉽게 `aaa`, `bbb`, `ccc`로 줄입니다.

| 시각 | 기기 | 일어난 일 | 세션 ID |
| --- | --- | --- | --- |
| 09:00:00 | 노트북 | 로그인이 필요한 화면을 열었다가 로그인 화면으로 넘어갑니다. 로그인 전인데 세션이 생깁니다 | `aaa` |
| 09:00:30 | 노트북 | 로그인합니다. 세션 ID가 바뀝니다 | `aaa` → `bbb` |
| 09:05 | 노트북 | 상품을 봅니다. 요청이 A, B로 번갈아 갑니다 | `bbb` |
| 09:10 | 휴대폰 | 로그인합니다. 규칙 때문에 노트북 세션이 끝나야 합니다 | `ccc` |
| 09:11 | 노트북 | 다음 요청에서 "다른 곳에서 로그인했습니다"를 봅니다 | `bbb` 끝 |
| 09:15 | 휴대폰 | 마지막 요청 뒤 아무것도 하지 않습니다 | `ccc` |
| 09:45 | — | 마지막 접근 30분 뒤라 휴대폰 세션이 만료됩니다 | `ccc` 끝 |

09:00:00의 세션은 로그인과 무관하게, 서버가 세션에 무언가를 저장할 때 생깁니다. 이 예시에서는 Spring Security가 "로그인 뒤 돌아갈 원래 요청"을 세션에 기억해 둡니다. 화면을 연다고 항상 세션이 생기는 것은 아닙니다.

장애는 "Redis가 느리거나 멈추면 사용자에게 무엇이 일어나는가"에서 이 하루의 09:20에 Redis 주 서버가 멈춘다고 가정해 따로 봅니다.

```text
 browser (cookie SESSION=<id>)
      |
      v
 load balancer --(round robin)--> app A ---+
                              \--> app B ---+--> Redis primary --(async copy)--> replica
                                                 spring:session:sessions:bbb
```

화살표는 요청이 가는 방향입니다. 앱 A와 B는 서로 세션을 주고받지 않고, 둘 다 같은 Redis 키를 읽고 씁니다.

### HTTP 세션: 서버가 기억하고, 브라우저는 번호표만 듭니다

- **한 줄**: HTTP 세션은 같은 사용자의 요청 사이에 이어 가야 할 정보(로그인한 사람이 누구인가 등)를 서버 쪽에 저장해 두고, 브라우저에는 그 저장소를 찾는 **세션 ID**만 쿠키로 들려 보내는 방식입니다.
- **비유**: 외투 보관소입니다. 외투(로그인 정보)는 보관소에 두고 손님은 번호표(세션 ID)만 듭니다. 다시 올 때마다 번호표를 내밀면 직원이 외투를 찾아 줍니다.
- **실제로는**: HTTP 요청은 원래 서로 독립적이라, 서버는 이전 요청을 누가 보냈는지 모릅니다. 그래서 첫 응답에 `Set-Cookie`로 세션 ID를 주고, 브라우저는 이후 같은 사이트로 보내는 요청마다 그 쿠키를 붙입니다. 서블릿 컨테이너(Tomcat)는 기본적으로 세션을 **자기 프로세스 메모리 안의 맵**에 둡니다. Kotlin으로 치면 `Map<String, HttpSession>`이 Tomcat 안에 있고, 키가 세션 ID인 셈입니다. 비유와 다른 점은 번호표를 누구나 베낄 수 있다는 것입니다. 남의 세션 ID를 손에 넣으면 그 사람으로 행세할 수 있으므로, 세션 ID는 비밀번호처럼 다뤄야 합니다.
- **이 주제에서는**: 맵이 앱 서버 A의 메모리에 있으면, 번호표를 들고 B로 간 요청은 외투를 찾지 못합니다. 이 문서는 그 보관소를 Redis로 옮겼을 때의 동작과 대가를 다룹니다.

### 서블릿 필터: 모든 요청이 컨트롤러 앞에서 지나는 관문

- **한 줄**: 필터는 요청이 컨트롤러에 닿기 전과 컨트롤러가 일을 마친 뒤(응답을 마무리할 때)에 끼어들어 일하는 코드입니다. Spring Security도 필터 묶음으로 동작합니다.
- **이 주제에서는**: Spring Session은 Spring Security보다 앞에 놓이는 필터 하나로 동작합니다. 그래서 컨트롤러와 Spring Security는 여전히 `HttpSession`을 쓰는데, 그 뒤에서 세션이 Redis로 오갑니다.

### 이 문서가 기대는 원리들

- **왕복 시간**: 요청을 보내고 응답을 받기까지 일을 하지 않고 기다리는 시간입니다. 하나씩 기다려 보내면 걸리는 시간은 대략 "명령 수 × 왕복 시간"입니다. 이 문서에서는 세션 처리가 요청마다 Redis 명령을 몇 개 보내는지가 그대로 지연이 됩니다. 원리는 [왕복과 묶어 보내기](../cs/round-trip-and-batching.md)에 있습니다.
- **경쟁 상태**: 여러 실행 흐름이 같은 데이터를 "읽고 판단하고 쓰는" 사이에 서로 끼어들어 결과가 틀어지는 것입니다. 이 문서에서는 두 서버에서 동시에 로그인할 때 동시 로그인 제한이 뚫리는 이유가 됩니다. 원리는 [경쟁 상태와 원자성](../cs/race-condition-and-atomicity.md)에 있습니다.
- **타임아웃**: 응답을 언제까지 기다릴지의 상한입니다. 이 문서에서는 Redis가 느릴 때 앱의 요청 스레드가 얼마나 오래 붙잡히는지를 정합니다. 원리는 [타임아웃](../cs/timeout.md)에 있습니다.

### Redis 쪽에서 쓰는 개념

- **TTL**: 키가 사라질 때까지 남은 시간입니다. Redis는 TTL을 걸 때 만료 시각을 키에 기록해 두고, 그 시각이 지나면 키를 지웁니다. 메모리가 모자랄 때 정책에 따라 키를 지우는 **축출(eviction)**과는 다른 길입니다. 둘 다 [TTL과 축출](./ttl-and-eviction.md)에 있습니다.
- **Hash**: 키 하나 아래에 "필드 이름 → 값" 쌍을 여러 개 담는 자료형입니다. Kotlin의 `Map<String, ByteArray>` 하나가 키 하나에 붙어 있다고 보면 됩니다. Spring Session이 세션을 왜 String 하나가 아니라 Hash에 담는지(바뀐 속성만 써서 동시 요청이 서로의 변경을 덮어쓰지 않게)는 [객체 하나를 String에 담을까, Hash에 담을까](./string-vs-hash.md)에 있습니다.
- **직렬화**: 세션 속성 객체를 바이트로 바꾸는 일입니다. 기본은 Java 직렬화이고, 배포로 클래스가 바뀌면 이미 저장된 세션을 못 읽어 그 세션의 요청이 모두 실패할 수 있습니다. 증상과 선택은 [직렬화기](./serializer.md)에 있습니다.

## 앱 서버 메모리의 세션으로는 왜 부족한가

세션이 Tomcat 메모리에 있으면 예시는 09:05에 깨집니다. 노트북이 A에서 로그인했는데 다음 요청이 B로 가면, B의 맵에는 `bbb`가 없어 로그인하지 않은 사용자로 보입니다.

흔한 첫 대응은 **고정 세션(sticky session)**입니다. 로드 밸런서가 쿠키나 접속 주소를 보고 같은 사용자를 늘 같은 서버로 보냅니다. 코드는 바꾸지 않아도 되지만 대가가 있습니다.

- **서버가 사라지면 그 서버의 사용자가 모두 로그아웃됩니다.** 배포로 A를 재시작하거나, 트래픽이 줄어 A를 내리거나, A가 죽으면 A의 메모리에 있던 세션이 함께 사라집니다. 배포할 때마다 사용자 일부가 로그아웃되는 셈입니다.
- **부하가 고르게 나뉘지 않습니다.** 오래 머무는 사용자가 한 서버에 몰려 있으면, 새 서버를 추가해도 기존 사용자는 옮겨 가지 않습니다.
- **서버를 넘는 기능을 만들 수 없습니다.** "neo의 다른 기기 세션을 끝낸다"는 일은 그 세션이 어느 서버 메모리에 있는지 모르면 할 수 없습니다.

서버끼리 세션을 복제해 나눠 갖는 방법도 있지만 이 문서는 다루지 않습니다.

세션을 Redis에 두면 이 세 문제가 풀립니다. A와 B가 같은 키를 읽으므로 어느 서버로 가도 같은 세션이고, 앱 서버를 재시작해도 세션은 남습니다. 대가는 두 가지입니다. **요청마다 Redis 왕복이 붙고, 로그인 상태가 Redis의 가용성과 데이터 보존에 기대게 됩니다.** 이 문서의 나머지는 이 두 대가가 구체적으로 무엇인지를 따라갑니다. 로그인 상태를 서버에 두지 않는 JWT는 "세션 대신 JWT를 쓰면 무엇을 얻고 잃는가"에서 비교합니다.

## Spring Session은 요청마다 Redis에 무엇을 보내는가

### 필터가 HttpSession을 Redis 세션으로 바꿔 끼웁니다

Spring Boot에서 Spring Session Data Redis를 의존성에 넣으면, 필터 하나가 요청 객체를 감싸 `request.getSession()`이 Tomcat 세션 대신 Redis 세션을 돌려주게 만듭니다. 그래서 컨트롤러와 Spring Security 코드는 바꾸지 않습니다.

- **쿠키**: 이름은 기본 `SESSION`이고, 값은 세션 ID를 Base64로 바꾼 문자열입니다. Tomcat의 `JSESSIONID`가 아닙니다. 쿠키 이름·`Secure`·`SameSite` 같은 속성은 `server.servlet.session.cookie.*` 속성으로 정합니다. 기본으로 `HttpOnly`(스크립트에서 읽지 못함)이고 `SameSite`는 `Lax`입니다.
- **세션 ID**: 무작위 UUID입니다. 추측하기 어려운 값이어야 남이 번호표를 지어내지 못합니다.

### 세션을 처음 찾을 때 Hash 하나를 통째로 읽습니다

필터는 요청이 들어오자마자 Redis에 가지 않습니다. 요청 처리 중 누군가 세션을 처음 찾을 때 쿠키의 세션 ID로 키 하나를 `HGETALL`로 통째로 읽고, 그 요청이 끝날 때까지 메모리에 들고 있습니다. 같은 요청 안에서 다시 찾으면 Redis에 가지 않습니다.

로그인 정보를 확인해야 하는 요청이라면 Spring Security가 세션에서 로그인 정보(`SecurityContext`)를 찾으므로 Redis를 읽습니다. 반대로 세션을 전혀 찾지 않는 요청은 Redis에 가지 않습니다. 어떤 요청이 세션을 찾는지는 보안 설정과 코드에 달려 있으므로, 실제 몇 번 읽는지는 Redis의 `INFO commandstats`에서 `hgetall` 호출 수를 요청 수와 비교해 확인합니다.

읽은 Hash가 비어 있으면(키가 없으면) 세션이 없는 것으로 봅니다. 읽은 세션이 앱 서버 시계로 이미 만료됐으면 키를 지우고 역시 없는 것으로 봅니다.

### 요청이 끝날 때 바뀐 필드만 쓰고, 만료 시각을 다시 겁니다

세션을 찾은 요청은 그 순간 세션의 **마지막 접근 시각(`lastAccessedTime`)**을 지금으로 바꿉니다. 그래서 세션을 찾은 요청은 무언가를 바꾸지 않았어도 쓸 것이 생깁니다. 응답을 보낼 때 필터는 세션을 저장하며 기본 저장소(`RedisSessionRepository`)는 다음 순서로 명령을 보냅니다.

| 순서 | 명령 | 하는 일 |
| --- | --- | --- |
| 1 | `HGETALL spring:session:sessions:bbb` | 세션을 처음 찾을 때 통째로 읽기 |
| 2 | `EXISTS spring:session:sessions:bbb` | 저장 직전에 키가 아직 있는지 확인. 없으면 "다른 곳에서 지운 세션"으로 보고 예외 |
| 3 | `HMSET spring:session:sessions:bbb lastAccessedTime ...` | 이번 요청에서 바뀐 필드만 쓰기 |
| 4 | `PEXPIREAT spring:session:sessions:bbb <시각>` | 만료 시각을 "마지막 접근 + 30분"으로 다시 걸기 |

**로그인한 사용자의 요청 하나에 Redis 명령 네 개, 왕복 네 번**이 붙습니다. 이 명령들은 하나씩 응답을 기다려 보내며 묶어 보내지 않습니다. 2번 확인은 로그아웃과 경합할 때 지운 세션을 되살리지 않으려는 장치입니다("로그아웃: 키를 지우면 모든 서버에서 즉시 끝납니다"). 새로 만든 세션의 첫 저장은 2번 없이 모든 필드를 씁니다.

이 네 개는 **응답을 한 번에 마치는 보통 경로**의 수입니다. 응답 본문을 중간에 내보내거나(`flushBuffer`) 리다이렉트로 응답이 먼저 확정되면 필터가 그 순간 한 번 저장하고, 요청 끝에 한 번 더 저장합니다. 두 번째 저장은 바뀐 필드가 없어 쓰지는 않지만 세션을 다시 읽고 확인하므로 `HGETALL`·`EXISTS`가 하나씩 더 붙습니다(소스로 추론). 아래의 명령 수는 모두 보통 경로 기준입니다.

무엇을 "바뀐 필드"로 모을지는 **저장 모드**(`spring.session.redis.save-mode`, 기본 `on-set-attribute`)가 정합니다. 기본은 이번 요청에서 `setAttribute`로 넣은 속성과 접근 시각만 모읍니다. 세션에서 꺼낸 객체의 안쪽만 고치고 다시 `setAttribute`하지 않으면 저장되지 않는다는 함정과, 다른 모드에서 생기는 갱신 손실은 [객체 하나를 String에 담을까, Hash에 담을까](./string-vs-hash.md)에 있습니다.

언제 쓸지는 **플러시 모드**(`spring.session.redis.flush-mode`)가 정합니다. 기본 `on-save`는 위처럼 응답을 마칠 때 한 번 씁니다. `immediate`는 `setAttribute`나 접근 시각 변경이 일어날 때마다 그 자리에서 씁니다. 같은 사용자의 다른 요청이 응답 전에도 바뀐 값을 봐야 할 때 쓰지만, 속성을 바꿀 때마다 쓰기 명령과 왕복이 늘어납니다.

### 키 하나의 모양

예시에서 09:05의 노트북 세션은 다음과 같습니다(기본 저장소, 기본 이름공간 `spring:session`).

```text
key: spring:session:sessions:bbb      (Hash, TTL = lastAccessedTime + 30 min)
  creationTime                              -> 09:00:00 (epoch ms)
  lastAccessedTime                          -> 09:05:12 (epoch ms)
  maxInactiveInterval                       -> 1800 (seconds)
  sessionAttr:SPRING_SECURITY_CONTEXT       -> serialized login info of neo
  sessionAttr:<other attributes>            -> ...
```

필드 값은 실제로는 직렬화된 바이트이며, 그림에는 뜻만 적었습니다. 키 이름의 `spring:session`은 `spring.session.redis.namespace`로 바꿀 수 있습니다. 같은 Redis를 쓰는 서비스가 여럿이면 서비스마다 다르게 두어야 서로의 세션 키가 섞이지 않습니다.

**기본 저장소는 이 키 하나가 전부입니다.** 회원 ID로 세션을 찾는 색인도, 만료 알림도 없습니다. 이 단순함이 "동시 로그인 제한: 사용자별 색인이 필요하고, 그 색인은 공짜가 아닙니다"에서 대가로 돌아옵니다.

## 만료: 마지막 접근부터 30분은 Redis의 TTL이 지킵니다

세션 만료는 `spring.session.timeout`으로 정하고, 이 값이 없으면 `server.servlet.session.timeout`(기본 30분)을 씁니다.

저장할 때마다 키에 **절대 시각**으로 만료를 겁니다. 값은 "마지막 접근 시각 + 30분"입니다. 예시의 휴대폰 세션 `ccc`는 09:15에 마지막으로 저장되었으므로 만료 시각이 09:45로 걸리고, 그 뒤로 요청이 없으면 Redis가 09:45 이후 키를 지웁니다. 요청이 올 때마다 만료 시각이 뒤로 밀리므로 "활동하는 동안은 로그인 유지"가 됩니다. 앱이 따로 만료 작업을 돌리지 않습니다.

여기서 세 가지를 짚습니다.

- **세션을 찾지 않는 요청은 만료를 늘리지 않습니다.** 접근 시각은 세션을 찾을 때만 바뀌기 때문입니다. 예를 들어 화면이 로그인 확인이 필요 없는 API만 계속 부르면, 사용자는 화면을 쓰고 있는데 30분 뒤 로그아웃될 수 있습니다.
- **만료 시각은 앱 서버의 시계로 계산합니다.** "마지막 접근 시각"은 그 요청을 처리한 앱 서버의 현재 시각이고, Redis에는 그 값으로 계산한 절대 시각을 겁니다. A와 B의 시계가 1분 어긋나 있으면 어느 서버가 마지막으로 저장했는지에 따라 만료가 1분 달라집니다(소스로 추론). 서버 시계를 동기화해 두어야 하는 이유입니다.
- **"활동과 무관하게 12시간 뒤 다시 로그인" 같은 절대 만료는 없습니다.** 이 버전의 Spring Session에서 그런 설정을 찾지 못했습니다. 필요하면 세션에 저장된 생성 시각(`creationTime`)을 앱이 검사해 직접 끝내야 합니다.

기본 저장소는 **만료되는 순간 앱에 알려 주지 않습니다.** Redis가 키를 조용히 지울 뿐입니다. "세션이 끝나면 감사 로그를 남긴다", "세션이 끝나면 웹소켓 연결을 닫는다"가 필요하면 다음 절의 색인 저장소가 필요합니다.

## 로그아웃: 키를 지우면 모든 서버에서 즉시 끝납니다

Spring Security의 로그아웃은 기본으로 세션을 무효화하고, Spring Session은 이를 `DEL spring:session:sessions:bbb`로 처리합니다. 응답에는 쿠키를 지우라는 `Set-Cookie`가 나갑니다.

A에서 로그아웃해도 B가 다음 요청에서 같은 키를 읽으므로, **그 뒤로 세션을 찾는 요청은 어느 서버에서든 그 로그인 세션을 얻지 못합니다.** 이것이 세션을 서버 쪽에 두는 방식의 가장 큰 장점입니다. 관리자가 특정 세션을 강제로 끝내는 것도 키를 지우면 됩니다. 다만 기본 저장소는 세션 ID를 알아야 지울 수 있고, "neo의 모든 세션"을 찾는 방법은 다음 절의 색인이 있어야 생깁니다.

로그아웃과 다른 요청이 겹치는 경우를 보겠습니다. 노트북이 A에서 로그아웃하는 사이 B에서 같은 세션의 다른 요청이 처리 중이었다면, B는 이미 읽어 둔 로그인 정보로 계속 처리되고, 세션을 저장할 때 접근 시각을 쓰려 합니다. 이때 앞의 "`EXISTS` 확인"이 키가 없음을 보고 예외를 던지므로 B가 세션을 되살리지 않습니다. 곧 로그아웃은 이미 처리 중인 요청을 멈추지 않으며, 그 요청이 한 업무는 반영된 채 저장에서 예외가 날 수 있습니다(소스로 추론). 다만 확인과 쓰기가 명령 두 개라 그 사이에 지워지면 막지 못합니다. 그 경우 `lastAccessedTime` 같은 일부 필드만 담긴 키가 생기고 만료 시각까지 남습니다. 이 키는 필수 필드가 없어 읽으면 예외가 나지만, 로그아웃 응답이 쿠키를 지웠으므로 보통은 아무도 읽지 않습니다(소스로 추론).

## 로그인 순간 세션 ID를 바꾸는 이유와 대가

예시의 09:00:30에 세션 ID가 `aaa`에서 `bbb`로 바뀝니다. 이유는 **세션 고정(session fixation) 공격**입니다.

- **한 줄**: 공격자가 미리 알고 있는 세션 ID를 피해자 브라우저에 심어 두고, 피해자가 그 ID로 로그인하면 같은 ID로 피해자 행세를 하는 공격입니다.
- **비유**: 보관소 번호표를 미리 복사해 두고 손님 손에 쥐여 줍니다. 손님이 그 번호표로 귀중품을 맡기면, 복사본으로 찾아갈 수 있습니다.
- **실제로는**: 로그인 전 세션 ID가 로그인 뒤에도 그대로면 공격이 성립합니다. 그래서 Spring Security는 기본으로 로그인에 성공하면 세션 ID를 바꿉니다. 세션의 내용은 유지하고 번호만 새로 뽑습니다.
- **이 주제에서는**: Spring Session은 새 UUID를 만들고, 요청을 마칠 때 `RENAME spring:session:sessions:aaa spring:session:sessions:bbb`로 키 이름을 바꾼 뒤, 응답 쿠키에 새 ID를 넣습니다.

대가가 하나 있습니다. 로그인 요청이 처리되는 동안 브라우저가 옛 ID `aaa`로 다른 요청을 함께 보냈다면(화면이 여러 API를 동시에 부르는 경우), 그 요청은 저장할 때 `aaa` 키가 없음을 보고 "Session was invalidated" 예외로 실패합니다(소스로 추론, 재현하지 않음). 로그인 직후 간헐적으로 실패하는 요청이 있다면 이 경로를 의심할 수 있습니다.

## 동시 로그인 제한: 사용자별 색인이 필요하고, 그 색인은 공짜가 아닙니다

### 기본 설정으로는 서버 두 대에서 제한이 동작하지 않습니다

Spring Security의 동시 로그인 제한(`maximumSessions`)은 로그인할 때 "이 회원의 세션이 지금 몇 개인가"를 세서, 정한 수를 넘으면 가장 오래 쓰지 않은 세션을 끝내거나(기본) 새 로그인을 거부합니다(`maxSessionsPreventsLogin(true)`).

세는 일은 **세션 레지스트리**가 맡습니다. 빈으로 따로 등록하지 않으면 Spring Security는 **앱 서버 메모리 안의 레지스트리**(`SessionRegistryImpl`)를 씁니다. 그러면 A는 A에서 로그인한 세션만, B는 B에서 로그인한 세션만 셉니다. 예시에서 노트북이 A에서, 휴대폰이 B에서 로그인하면 각 서버가 세는 neo의 세션은 하나씩이라 제한이 걸리지 않습니다. **세션을 Redis에 옮겨도 레지스트리를 바꾸지 않으면 제한은 서버마다 따로 동작합니다.**

서버 전체에서 세려면 Spring Session이 주는 레지스트리(`SpringSessionBackedSessionRegistry`)를 써야 하는데, 이 레지스트리는 "회원 이름으로 세션 찾기"를 할 수 있는 저장소를 요구합니다. 기본 저장소는 키가 세션 ID뿐이라 할 수 없습니다. 모든 세션 키를 훑어야 하기 때문입니다. 그래서 **색인 저장소**(`RedisIndexedSessionRepository`, `spring.session.redis.repository-type=indexed`)로 바꿔야 합니다.

### 색인 저장소는 키를 네 종류 둡니다

| 키 | 자료형 | 담는 것 | 만료 |
| --- | --- | --- | --- |
| `spring:session:sessions:bbb` | Hash | 세션 내용(기본 저장소와 같음) | 저장할 때마다 그 시점부터 30분 + **5분** |
| `spring:session:sessions:expires:bbb` | String(빈 값) | 만료를 알리기 위한 그림자 키 | 저장할 때마다 그 시점부터 30분 |
| `spring:session:expirations:<분 단위 시각>` | Set | 그 분에 만료될 그림자 키 이름들 | 대개 35분. 매분 정리 작업이 지난 분의 Set을 지움 |
| `spring:session:index:org.springframework.session.FindByIndexNameSessionRepository.PRINCIPAL_NAME_INDEX_NAME:neo` | Set | neo의 세션 ID들 | **없음** |

마지막 줄이 회원별 색인입니다. 로그인 정보가 세션에 저장될 때 그 세션 ID를 회원 이름의 Set에 넣습니다. "neo의 세션 목록"은 이 Set을 `SMEMBERS`로 읽고, 들어 있는 ID마다 세션 Hash를 `HGETALL`로 읽어 아직 살아 있는 것만 고릅니다.

기본 저장소와 달리 만료를 "마지막 접근 + 30분" 절대 시각이 아니라 **저장한 시점부터의 상대 시간**으로 겁니다. 요청 처리가 길었다면 Redis가 지우는 시각이 앱이 판정하는 만료 시각보다 그만큼 늦습니다. 나머지 두 종류는 만료를 앱에 알리기 위한 장치이며, 아래 "색인이 기대는 만료 알림과 그 약점"에서 설명합니다. 세션 Hash 자체에 5분을 더 주는 이유도 거기에 있습니다.

### 휴대폰이 로그인할 때 노트북 세션은 어떻게 끝나는가

09:10에 휴대폰이 B에서 로그인합니다. 최대 1개로 정했다고 하겠습니다.

1. B가 neo의 세션을 셉니다. 색인 Set에서 `bbb`를 찾고, `bbb`의 Hash를 읽어 하나로 셉니다. 1개 ≥ 최대 1개이므로 넘칩니다.
2. 기본 동작은 가장 오래 쓰지 않은 세션, 곧 `bbb`를 끝내는 것입니다. 그런데 **이때 `bbb`를 지우지 않습니다.** `bbb`의 Hash에 "만료됨" 표시 속성을 하나 써 넣을 뿐입니다.
3. 휴대폰의 새 세션 ID를 만들고(앞의 세션 고정 방어), 로그인 정보를 저장하면서 색인 Set에 `ccc`를 넣습니다.
4. 09:11에 노트북이 A로 요청합니다. 동시 로그인 제한을 켜면 붙는 필터가 **요청마다** 레지스트리에 이 세션의 정보를 묻고, 레지스트리는 저장소에서 `bbb`를 다시 읽어 "만료됨" 표시를 봅니다. 그제야 로그아웃 처리(세션 삭제)를 하고 "다른 곳에서 로그인했습니다" 응답을 보냅니다.

곧, 노트북은 **다음 요청을 보낼 때** 끝난 것을 알게 됩니다. 노트북이 요청을 보내지 않으면 표시만 된 채 만료 시각까지 남습니다.

`maxSessionsPreventsLogin(true)`로 새 로그인을 거부하게 하면 다른 문제가 생깁니다. 노트북에서 로그아웃하지 않고 브라우저만 닫은 neo는 그 세션이 30분 뒤 만료될 때까지 휴대폰으로 로그인할 수 없습니다. 고객센터 문의로 이어지기 쉬운 선택이므로, 거부보다 "이전 기기 끝내기"가 보통 덜 위험합니다.

### 두 곳에서 동시에 로그인하면 제한을 넘을 수 있습니다

로그인 시 "세고 → 판단하고 → 새 세션을 등록하는" 일은 Redis 명령 여러 개로 나뉘어 있고, 묶어 원자적으로 처리하지 않습니다. 노트북이 A에서, 휴대폰이 B에서 거의 같은 순간 로그인하면 두 서버 모두 "neo의 세션 0개"를 읽고 둘 다 통과할 수 있습니다. 그러면 최대 1개인데 2개가 남습니다(소스로 추론, 재현하지 않음). 읽은 값이 쓰는 순간 이미 낡는 전형적인 경쟁 상태입니다.

동시 로그인 제한을 과금이나 계정 공유 차단처럼 **정확해야 하는 규칙**으로 쓴다면 이 틈을 메워야 합니다. 회원 단위로 로그인 처리를 직렬화하는 락([Redis 분산 락](./distributed-lock.md))이 한 방법인데, 락은 세기부터 새 세션이 Redis에 저장되어 색인에 들어갈 때까지를 덮어야 합니다. Spring Session의 레지스트리는 등록 단계에서 아무것도 하지 않고, 색인은 요청 끝의 세션 저장 때 갱신되기 때문입니다. 다만 이 락도 보유자가 기한보다 오래 멈춘 사이 락 키가 만료되어 다른 로그인이 락을 얻거나, 장애 조치로 락 키를 잃으면 두 로그인이 겹칠 수 있으므로, 그 경우까지 막아야 하는지는 분산 락 문서의 정확성용 락 기준으로 판단합니다. 로그인 뒤 넘친 세션을 찾아 끝내는 사후 점검은 넘친 상태가 이어지는 시간을 줄일 뿐 동시에 둘이 유효한 구간을 막지는 못합니다. "대부분 막으면 된다"는 수준이면 이 틈을 받아들이는 것도 선택입니다.

### 요청마다 드는 비용이 늘어납니다

색인 저장소는 저장할 때마다 Hash 외의 키도 갱신합니다. 로그인 상태가 바뀌지 않는 평범한 요청 하나의 저장에 다음이 붙습니다(소스로 센 명령, 경우에 따라 하나 더하거나 뺌).

| 구성 | 요청 하나의 Redis 명령(왕복) |
| --- | --- |
| 기본 저장소 | `HGETALL`, `EXISTS`, `HMSET`, `PEXPIREAT` — 4개 |
| 색인 저장소 | `HGETALL`, `HMSET`, `APPEND`·`PEXPIRE`(그림자 키), `PEXPIRE`(Hash), 분 단위 Set의 `PEXPIRE`·`SADD`, 분이 바뀌었으면 옛 Set의 `SREM` — 7~8개 |
| 색인 저장소 + 동시 로그인 제한 | 위에 레지스트리 확인용 `HGETALL` 하나 — 8~9개 |

같은 요청에서 세션 Hash를 두 번 통째로 읽는다는 점도 눈여겨볼 만합니다. 세션 속성이 크면 그만큼 두 번 받고 두 번 역직렬화합니다.

## 색인이 기대는 만료 알림과 그 약점

### 키스페이스 알림: 키가 지워지면 Redis가 방송합니다

- **한 줄**: 키스페이스 알림(keyspace notifications)은 키가 만료되거나 지워질 때 Redis가 그 사실을 Pub/Sub 채널에 메시지로 보내는 기능입니다.
- **비유**: 보관소 직원이 외투를 정리할 때마다 확성기로 번호를 외칩니다. 듣고 있던 사람만 압니다.
- **실제로는**: Pub/Sub은 채널을 구독하고 있는 연결에만 메시지를 보내고, 저장해 두지 않습니다. 공식 문서도 "fire and forget"이라 적고, 끊겨 있던 동안의 메시지는 사라진다고 합니다. 이 기능은 기본으로 꺼져 있고(`notify-keyspace-events ""`), 켜려면 서버 설정을 바꿔야 합니다.
- **이 주제에서는**: 색인 저장소는 이 알림으로 "세션이 끝났다"를 알아내 회원별 색인에서 그 세션을 빼고, `SessionExpiredEvent`·`SessionDeletedEvent`를 앱 안에 발행합니다.

### 왜 그림자 키와 분 단위 Set이 필요한가

세션 Hash 자체가 만료되어 지워지면, 알림을 받은 앱은 이미 사라진 세션의 내용(누구의 세션이었는지)을 읽을 수 없습니다. 그래서 만료를 **빈 그림자 키**에 걸고, 진짜 Hash는 5분 더 살려 둡니다. 그림자 키가 만료되었다는 알림을 받으면 아직 남아 있는 Hash를 읽어 회원 이름을 알아내고, 색인에서 뺍니다.

또 하나의 문제는 **만료 알림이 늦게 올 수 있다**는 것입니다. 공식 문서에 따르면 `expired` 알림은 TTL이 0이 되는 순간이 아니라 Redis가 실제로 키를 지울 때 나갑니다. Redis는 만료된 키를 누군가 접근할 때 지우거나, 백그라운드에서 조금씩 찾아 지우므로, 아무도 접근하지 않는 키는 한참 뒤에 지워질 수 있습니다([TTL과 축출](./ttl-and-eviction.md)). 그래서 색인 저장소는 만료될 그림자 키를 분 단위 Set에 모아 두고, **매분** 정리 작업이 지난 분의 Set을 읽어 각 키에 접근합니다. 접근하면 Redis가 만료된 키를 그 자리에서 지우고 알림을 냅니다.

### 이 장치들의 약점

- **서버 설정을 바꿀 권한이 필요합니다.** 색인 저장소는 시작할 때 `CONFIG SET notify-keyspace-events`로 필요한 알림을 켭니다. 관리형 Redis처럼 `CONFIG`가 막힌 환경에서는 시작이 실패합니다. 이때는 `spring.session.redis.configure-action=none`으로 자동 설정을 끄고, 운영 쪽에서 서버에 알림을 켜 둬야 합니다. 끄기만 하고 서버 설정을 빼먹으면 시작은 되지만 만료 알림이 오지 않습니다. 세션은 TTL로 만료되지만, 회원 색인 정리와 만료 이벤트 발행이 멈춥니다.
- **앱 서버 대수만큼 같은 일을 합니다.** 알림 구독과 매분 정리 작업은 앱 서버마다 돕니다. 예시에서 `ccc`가 만료되면 A와 B가 모두 알림을 받고, 둘 다 Hash를 읽고 색인에서 빼고 `SessionExpiredEvent`를 발행합니다(설정 소스로 추론). 이 이벤트에 감사 로그를 남기면 서버 대수만큼 중복됩니다. 이벤트 처리는 중복을 견디게 만들어야 합니다.
- **알림을 놓치면 색인에 죽은 ID가 남습니다.** 모든 앱 서버가 재시작 중이었거나 구독 연결이 끊겨 있던 동안의 알림은 사라집니다. 회원별 색인 Set에는 만료가 없으므로, 그 사이 끝난 세션 ID가 Set에 남습니다. 세션 찾기는 Hash가 없는 ID를 건너뛰므로 결과는 맞지만, Set은 커지고 로그인할 때마다 죽은 ID마다 `HGETALL`을 한 번씩 더 보냅니다(소스로 추론).
- **Redis Cluster에서는 알림 일부를 놓칩니다.** Redis Cluster는 키를 여러 주 서버에 나눠 담는 구성입니다. 같은 데이터를 복사해 두는 복제본과 달리 주 서버마다 다른 키를 갖습니다. 키스페이스 알림은 그 키를 가진 노드에서만 나가며 다른 노드로 전달되지 않습니다. Spring Session 공식 문서는 Cluster에서 색인 저장소가 무작위 노드 하나의 알림만 구독하므로 일부 색인이 정리되지 않을 수 있다고 경고합니다.

곧 색인 저장소는 동시 로그인 제한과 만료 이벤트를 얻는 대신, **명령 수가 두 배 가까이 늘고, Pub/Sub 알림이라는 손실 가능한 경로에 정리를 맡깁니다.** 이 기능들이 필요 없다면 기본 저장소가 낫습니다.

## Redis가 느리거나 멈추면 사용자에게 무엇이 일어나는가

예시의 09:20에 Redis 주 서버가 멈췄다고 하겠습니다.

### 느릴 때: 로그인한 모든 요청이 함께 느려집니다

로그인한 사용자의 요청은 모두 세션을 읽고 쓰므로, Redis 왕복이 1ms에서 50ms로 늘면 요청마다 그 몇 배가 붙습니다(기본 저장소면 왕복 4번). 요청 스레드는 그동안 붙잡혀 있습니다. Spring Data Redis 기본 설정에서 Lettuce의 명령 타임아웃은 60초라, Redis가 응답하지 않는 채로 멈추면 스레드가 명령마다 최대 60초씩 기다리다 앱의 요청 스레드가 바닥날 수 있습니다. 그러면 세션을 쓰지 않는 요청까지 처리할 스레드가 없어 함께 멈춥니다. 연결 자체가 거부되는 경우(프로세스가 내려가 포트가 닫힘)는 이보다 빨리 실패합니다. 명령 타임아웃을 서비스 기한에 맞춰 짧게 정하는 기준은 [Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)에 있습니다.

### 멈췄을 때: 로그아웃이 아니라 요청 실패입니다

세션을 읽는 `HGETALL`이 연결 실패나 타임아웃으로 예외가 나면, Spring Session의 필터와 Spring Security의 세션 읽기 경로 모두 이 예외를 잡지 않습니다. 그래서 로그인한 사용자의 요청은 **로그인하지 않은 사용자로 처리되는 것이 아니라 예외로 끝납니다**(오류 처리 설정에 따라 보통 500 응답). 로그인도 세션을 쓰므로 실패합니다. 세션을 찾지 않는 요청은 영향을 받지 않습니다.

실패가 언제 나느냐에 따라 사용자가 본 결과와 실제 업무 결과가 달라집니다. 로그인 확인처럼 컨트롤러 앞에서 세션을 읽다가 실패하면 업무 처리 전이라 아무것도 반영되지 않습니다. 반면 Redis가 요청 처리 도중에 멈추면, 업무 처리(예: 주문 DB 반영)는 끝났는데 그 뒤의 세션 저장이 실패할 수 있습니다. 필터는 이 저장 예외도 잡지 않습니다. 응답 확정은 상태 코드와 헤더가 클라이언트로 나가 더는 바꿀 수 없게 되는 시점입니다. 이 저장은 응답이 확정되기 직전에 일어나므로 보통은 응답이 오류가 됩니다. 하지만 응답이 이미 확정된 뒤의 두 번째 저장(앞의 "요청이 끝날 때 바뀐 필드만 쓰고, 만료 시각을 다시 겁니다")에서 실패하면, 이미 나간 상태 코드(예: 200)는 바뀌지 않지만 본문과 연결이 정상으로 끝난다는 보장도 없습니다. 클라이언트는 성공 코드를 받거나, 응답이 중간에 끊긴 것으로 볼 수 있습니다(소스로 추론, 재현하지 않음). 그래서 오류 응답만 보고 업무가 실패했다고 단정하고 다시 보내면 같은 처리가 두 번 남을 수 있습니다. 다시 보내기 전에 업무 결과를 확인하거나, 다시 와도 한 번만 처리되게 만드는 방법은 [Redis 멱등 키](./idempotency-key.md)에 있습니다.

예시에서 09:20 이후 휴대폰이 보내는 요청은 오류 화면을 받고, 다시 로그인하려 해도 실패합니다. Redis가 돌아오면 `ccc` 키가 그대로 있으므로(키를 잃지 않았다면) 로그인 상태로 이어집니다.

이 버전의 Spring Session에는 Redis 장애 때 대체 동작을 고르는 설정이 없습니다. 고를 수 있는 길과 대가는 다음과 같습니다(이 문서의 판단).

| 선택 | 사용자가 겪는 것 | 대가 |
| --- | --- | --- |
| 실패를 그대로 둔다(기본) | 로그인이 필요한 화면이 오류. 공개 화면은 정상 | 가장 단순하고 상태가 엇갈리지 않음. 명령 타임아웃을 짧게, 회로 차단기(실패가 쌓이면 한동안 Redis를 부르지 않고 바로 실패시키는 장치)로 빨리 실패하게 만들어야 스레드를 지킴 |
| 세션 저장소 예외를 잡아 "세션 없음"으로 바꾼다 | 모두 로그아웃된 것처럼 보이고 로그인 화면으로 감 | 사용자가 로그인을 다시 시도하지만 로그인도 실패해 요청이 몰림. Redis가 돌아오면 옛 세션이 그대로 살아 있어 "로그아웃된 줄 알았는데" 혼란 |
| 장애 동안 앱 메모리 세션으로 바꾼다 | 서버마다 다른 로그인 상태 | 고정 세션 없이는 A에서 로그인하고 B에서 로그아웃 상태. 복구 후 두 세션이 엇갈림. 권하지 않음 |

로그인 상태를 Redis에 두기로 했다면, Redis는 캐시가 아니라 **로그인 기능의 필수 의존성**입니다. 그래서 장애 조치가 있는 구성(복제본과 Sentinel 등)과 빠른 실패 설정을 함께 갖추는 것이 첫 대응입니다. 실패 시 재시도와 회로 차단기의 원리는 [재시도와 서킷 브레이커](../cs/retry-and-circuit-breaker.md)에 있습니다.

### 재시작·장애 조치·축출로 키를 잃으면: 오류가 아니라 강제 로그아웃입니다

키가 사라진 경우는 앞과 증상이 다릅니다. `HGETALL`이 빈 결과를 돌려주므로 세션이 없는 것으로 보이고, 사용자는 **오류 없이 로그아웃됩니다.**

- **영속성 없이 재시작하면 모두 로그아웃됩니다.** 디스크에 남기지 않고 예전에 남겨 둔 RDB 파일도 없는 Redis가 다시 시작하면 비어 있으므로, 그 순간 로그인한 모든 사용자가 로그아웃됩니다. RDB만 쓰면 마지막 저장 이후의 변경을 잃습니다. 그 사이 로그인한 사용자는 로그아웃되고, 그 사이 로그아웃한 세션은 되살아납니다. 어떤 설정이 무엇을 남기는지는 [Redis 영속성](./persistence.md)에 있습니다.
- **장애 조치로 최근 쓰기를 잃습니다.** 복제는 비동기라, 주 서버가 응답한 쓰기가 복제본에 닿기 전에 주 서버가 죽으면 승격된 복제본에는 그 쓰기가 없습니다([Redis 장애 조치와 클라이언트](./failover-client.md)). 세션에서는 다음처럼 나타납니다(복제 성질로 추론).
  - 방금 로그인한 사용자: 로그인이 저장한 세션이나 ID 변경(`RENAME`)이 없으면, 새 쿠키가 가리키는 키가 없어 로그아웃됩니다.
  - 방금 로그아웃한 사용자: `DEL`을 잃으면 **로그아웃한 세션이 새 주 서버에서 되살아납니다.** 쿠키는 지워졌으므로 보통은 쓰이지 않지만, 그 세션 ID를 탈취한 사람이 있었다면 쓸 수 있고, 만료는 마지막 접근 기준이라 탈취자가 계속 요청하는 동안은 끝나지 않습니다. 공용 PC 로그아웃처럼 보안이 걸린 경로라면 염두에 둘 일입니다.
  - 동시 로그인 제한의 "만료됨" 표시를 잃으면 노트북 세션이 계속 살아 있습니다.
- **축출로 지워지면 강제 로그아웃입니다.** 메모리 한도(`maxmemory`)에 닿았을 때 무엇을 지우는지는 `maxmemory-policy`가 정합니다. 세션 키는 늘 TTL이 있으므로 `volatile-*` 정책에서도 축출 후보이고, `allkeys-*`면 물론 후보입니다. 캐시와 같은 Redis에 두면 캐시가 늘어 세션이 밀려나고, 이는 사용자에게 갑작스러운 로그아웃으로 보입니다.
- **축출하지 않으면 세션을 저장해야 하는 요청이 모두 실패합니다.** 기본 정책 `noeviction`에서 메모리 사용량이 한도를 넘으면 메모리를 늘릴 수 있는 쓰기 명령으로 표시된 명령(`HMSET`, `APPEND`, `SADD` 등)이 OOM 에러를 받습니다. OOM 에러는 메모리 한도 때문에 Redis가 그 명령을 거부했다는 응답이며, Redis 프로세스는 살아 있고 읽기 명령은 계속 처리합니다. 이번 쓰기가 같은 크기의 값을 덮어쓰는 갱신이어도 거부됩니다. `HGETALL`과 `EXISTS`는 되지만, 세션을 새로 만들거나 기존 세션을 찾아 이어 쓰는 요청은 저장할 때 보내는 `HMSET`이 실패해 예외로 끝납니다. 세션이 없고 새로 만들지도 않는 요청과, 세션을 지우기만 하는 로그아웃(`DEL`은 거부되지 않음)은 `HMSET`을 보내지 않습니다. 기본 플러시 모드에서는 이 저장이 컨트롤러가 일을 마친 뒤에 일어나므로, 앞의 "멈췄을 때: 로그아웃이 아니라 요청 실패입니다"에서 본 것처럼 오류 응답이어도 업무 결과는 반영되어 있을 수 있습니다. 읽기는 되는데 요청은 실패하는 모양이라 원인을 찾기 어렵습니다.

그래서 세션은 **캐시와 다른 Redis(또는 메모리 여유를 따로 지키는 구성)에 두는 것**이 안전합니다. 세션 전용이라면 `noeviction`에 메모리 사용률 경보를 걸어 한도 전에 늘리고, 캐시와 함께 둘 수밖에 없다면 축출이 곧 로그아웃이라는 것을 받아들이는 결정을 명시적으로 합니다. 정책별 동작은 [TTL과 축출](./ttl-and-eviction.md)에 있습니다.

## 세션 대신 JWT를 쓰면 무엇을 얻고 잃는가

이 절은 특정 라이브러리 동작이 아니라 방식의 비교이며 이 문서의 판단입니다.

- **한 줄**: JWT(JSON Web Token, RFC 7519)는 "누구인가" 같은 정보를 담은 문자열 형식입니다. 이 절에서 비교하는 것은 흔히 쓰는 방식, 곧 서버가 서명한 JWT에 만료 시각(`exp`)을 넣어 주고, 요청마다 서명과 만료만 검증하며 저장소는 조회하지 않는 설계입니다.
- **비유**: 보관소 번호표 대신 위조 방지 도장이 찍힌 출입증을 줍니다. 경비는 도장만 보고 들여보내므로 보관소에 물어보지 않습니다.
- **실제로는**: 서명은 서버만 가진 키로 만든 값이라, 내용을 바꾸면 검증에 실패합니다. 서명만 한 JWT의 내용은 암호화가 아니라 인코딩일 뿐이라 누구나 읽을 수 있고(내용을 암호화하는 형식도 따로 있습니다), 이 설계에서는 한 번 발급하면 만료 시각까지 유효합니다. 비유와 다른 점은 **출입증을 회수할 방법이 없다**는 것입니다.
- **이 주제에서는**: 세션 저장소가 주는 기능과 비용이 정확히 뒤집힙니다.

| 기준 | Redis 세션 | JWT만 |
| --- | --- | --- |
| 요청마다 Redis | 왕복 4번 이상 | 없음 |
| Redis 장애 시 로그인 확인 | 실패 | 영향 없음 |
| 즉시 로그아웃·강제 로그아웃 | 키 삭제로 즉시 | 만료 시각까지 불가. 막으려면 폐기 목록을 요청마다 조회해야 함 |
| 동시 로그인 제한 | 색인 저장소로 가능 | 서버 쪽 상태 없이 불가 |
| 권한 변경 반영 | 세션 내용을 바꾸면 다음 요청부터 | 토큰을 다시 발급받을 때까지 옛 권한 |
| 매 요청의 크기 | 쿠키에 ID만 | 토큰 전체(담은 정보와 서명만큼 ID보다 김) |

JWT의 약점을 메우는 흔한 방법은 **수명이 짧은 액세스 토큰**(예: 15분)과 **서버에 저장하는 갱신 토큰**을 함께 쓰는 것입니다. 즉시 로그아웃은 갱신 토큰을 지워 "최대 15분 뒤 끝남"으로 바꾸고, 갱신 토큰 저장소는 다시 Redis나 DB가 됩니다. 곧 JWT도 상태를 완전히 없애지는 못하고, **요청마다 조회하던 상태를 15분마다 조회하는 상태로 줄이는 것**입니다.

예시의 쇼핑몰처럼 브라우저 중심이고 "다른 곳에서 로그인하면 이전 기기 로그아웃", "로그아웃 즉시 끝남"이 요구사항이면 세션이 맞습니다. 여러 서비스가 같은 사용자를 확인해야 하고 Redis 한 곳에 모든 요청이 기대는 것이 부담이면 JWT 쪽으로 기웁니다.

## 데이터와 요청이 늘면 무엇이 달라지는가

- **요청이 늘면 Redis 명령은 요청 수 × 요청당 명령 수로 늡니다.** 초당 로그인 요청 5,000건에 기본 저장소면 초당 2만 개, 색인 저장소에 동시 로그인 제한까지면 초당 4만 개 남짓입니다(가상 예시). 명령 하나하나는 가볍지만 Redis 메인 스레드 하나가 모두 처리합니다.
- **세션 수는 로그인 사용자 수가 아니라 "최근 30분 안에 세션을 찾거나 만든, 로그아웃하지 않은 방문 수"입니다.** 로그인하지 않은 방문에서도 세션을 만드는 코드가 있으면(로그인 전 `aaa`처럼) 크롤러와 봇의 방문마다 키가 생기고 30분씩 남습니다. 세션 키 수(`INFO keyspace`의 키 수, 이름공간별 표본 조사)가 로그인 사용자 수보다 훨씬 많다면 어디서 세션을 만드는지 찾아야 합니다.
- **세션 속성이 커지면 매 요청이 그 크기를 옮깁니다.** `HGETALL`은 모든 속성을 읽으므로, 장바구니나 큰 목록을 세션에 넣으면 그 요청이 쓰지 않는 속성까지 매번 받고 역직렬화합니다. 동시 로그인 제한을 켜면 두 번입니다. 세션에는 로그인 확인에 필요한 작은 값만 두고 나머지는 별도 키나 DB에 둡니다.
- **Cluster로 나누면 기본 저장소의 읽기와 저장은 그대로 동작합니다.** 읽기와 저장의 명령은 키 하나에만 닿기 때문입니다. 예외는 로그인 때의 세션 ID 변경으로, 옛 키와 새 키의 슬롯(클러스터가 키 이름으로 정하는 칸. 노드 하나가 여러 슬롯을 맡습니다. [Redis Cluster](./cluster.md))이 다르면(세션 ID가 무작위라 대개 다릅니다) 같은 노드에 있더라도 Spring Data Redis가 `RENAME` 대신 옛 키를 읽어 새 키에 복원하고 옛 키를 지우는 세 단계로 처리합니다. 원자적이지 않으므로 그 사이 실패하면 옛 키와 새 키가 함께 남거나 어긋날 수 있습니다(소스로 추론). 색인 저장소는 앞의 "Redis Cluster에서는 알림 일부를 놓칩니다" 문제가 있습니다.

## 흔한 오해 모아 보기

| 오해 | 실제 |
| --- | --- |
| 세션을 Redis에 두면 동시 로그인 제한이 서버 전체에서 동작한다 | 세션 레지스트리를 따로 등록하지 않으면 서버마다 메모리에서 셉니다. 색인 저장소와 Spring Session의 레지스트리가 필요합니다 |
| Redis가 죽으면 사용자는 로그아웃된다 | 연결이 안 되면 요청이 예외로 끝납니다. 로그아웃처럼 보이는 것은 키를 잃었을 때(재시작, 장애 조치, 축출)입니다 |
| 세션 만료는 앱이 주기적으로 지운다 | 기본 저장소는 저장할 때마다 Redis 키에 만료 시각을 걸고 Redis가 지웁니다 |
| 요청이 오면 무조건 세션 만료가 늘어난다 | 세션을 찾은 요청만 늘립니다 |
| 동시 로그인 제한으로 밀려난 세션은 바로 지워진다 | "만료됨" 표시만 하고, 그 기기가 다음 요청을 보낼 때 로그아웃 처리됩니다 |
| 만료 이벤트는 정확히 만료 시각에 한 번 온다 | Redis가 키를 실제로 지울 때 나가며, 앱 서버마다 받고, 끊겨 있으면 사라집니다 |
| JWT로 바꾸면 Redis가 필요 없다 | 즉시 로그아웃이나 갱신 토큰을 쓰면 서버 쪽 저장소가 다시 필요합니다 |

## 근거

기준: Spring Boot [`v3.5.16`](https://github.com/spring-projects/spring-boot/tree/v3.5.16)이 관리하는 Spring Session [`3.5.7`](https://github.com/spring-projects/spring-session/tree/3.5.7)(커밋 `bf21f5a75e430026a64f1e399b5bf63ea8cbd3e6`), Spring Security [`6.5.11`](https://github.com/spring-projects/spring-security/tree/6.5.11)(커밋 `73b077790fcb04ac3712033d3e939daf42264545`), Spring Data Redis 3.5.13, Redis [`8.2.10`](https://github.com/redis/redis/tree/8.2.10) 소스, 공식 문서 [redis/docs `e53775dd`](https://github.com/redis/docs/tree/e53775dde0ee7e39ebe327e7ffff583caa9fbd65).

**Spring Session 3.5.7**

- [`RedisSessionRepository.java`](https://github.com/spring-projects/spring-session/blob/3.5.7/spring-session-data-redis/src/main/java/org/springframework/session/data/redis/RedisSessionRepository.java)
  - `findById`: `opsForHash().entries()`(`HGETALL`)로 읽고, 비어 있으면 `null`, `isExpired()`면 `deleteById` 후 `null`("읽은 Hash가 비어 있으면(키가 없으면) 세션이 없는 것으로", "앱 서버 시계로 이미 만료됐으면 키를 지우고")
  - `save`: 새 세션이 아니면 `hasKey`(`EXISTS`)로 확인하고 없으면 `IllegalStateException("Session was invalidated")`("저장 직전에 키가 아직 있는지 확인", "그 요청은 저장할 때 `aaa` 키가 없음을 보고")
  - `RedisSession.saveDelta`: 변경분만 `opsForHash().putAll`, 이어서 `expireAt(lastAccessedTime + maxInactiveInterval)`("바뀐 필드만 쓰고, 만료 시각을 다시 겁니다", "저장할 때마다 키에")
  - `RedisSession.saveChangeSessionId`: ID가 바뀌었고 새 세션이 아니면 `rename`("키 이름을 바꾼 뒤")
  - `RedisSession` 생성자: 새 세션이면 `creationTime`·`maxInactiveInterval`·`lastAccessedTime`과 모든 속성을 변경분에 넣음("새로 만든 세션의 첫 저장은 2번 없이 모든 필드를 씁니다"). `setLastAccessedTime`이 변경분에 넣음. `flushIfRequired`: `FlushMode.IMMEDIATE`면 바로 `save`
  - `deleteById`: `delete`(`DEL`). 기본값 `saveMode = ON_SET_ATTRIBUTE`, `flushMode = ON_SAVE`, `getSessionKey` = 이름공간 + `sessions:` + ID. `FindByIndexNameSessionRepository`를 구현하지 않음("기본 저장소는 키가 세션 ID뿐이라 할 수 없습니다")
- [`RedisIndexedSessionRepository.java`](https://github.com/spring-projects/spring-session/blob/3.5.7/spring-session-data-redis/src/main/java/org/springframework/session/data/redis/RedisIndexedSessionRepository.java)
  - `getPrincipalKey`, `getExpirationsKey`, `configureSessionChannels`(`sessions:expires:` 접두어): 키 이름("색인 저장소는 키를 네 종류 둡니다")
  - `RedisSession.saveDelta`: `putAll`, 로그인 정보가 바뀌면 회원 Set에서 `remove`/`add`, `createShadowKey`(`append("")` + `expire(세션 만료)`), Hash에 `expire(세션 만료 + 5분)`. 둘 다 저장 시점부터의 상대 시간("저장한 시점부터의 상대 시간"), `expirationStore.save`. 회원 Set에는 만료를 걸지 않음("회원별 색인 Set에는 만료가 없으므로", "요청 하나의 Redis 명령")
  - `MinuteBasedRedisSessionExpirationStore`: 분 단위로 올림한 시각의 Set에 먼저 `expire`, 그다음 `expires:<id>`를 `add`(Set이 없으면 만료 없이 생기고, 같은 분에 다음 등록이 와야 만료가 걸림), 분이 바뀌었으면 옛 Set에서 `remove`. `cleanupExpiredSessions`가 지난 분의 Set을 `members`로 읽고 지운 뒤 각 키에 `hasKey`로 접근("정리 작업이 지난 분의 Set을 읽어 각 키에 접근합니다")
  - `DEFAULT_CLEANUP_CRON = "0 * * * * *"`, `afterPropertiesSet`에서 앱마다 스케줄러로 정리 작업 실행
  - `onMessage`: `del`/`expired` 알림의 `sessions:expires:` 키를 받아 Hash를 읽고(`getSession(id, true)`), `cleanupPrincipalIndex`, `SessionDeletedEvent`/`SessionExpiredEvent` 발행("그림자 키가 만료되었다는 알림을 받으면")
  - `findByIndexNameAndIndexValue`: 회원 Set을 `members`로 읽고 ID마다 `findById`, 없는 세션은 건너뜀("Hash가 없는 ID를 건너뛰므로", "죽은 ID마다 `HGETALL`")
  - 클래스 주석 "Expiration": 만료 알림이 보장되지 않아 분 단위로 추적하고, 세션 Hash는 5분 뒤 만료해 만료 처리 때 값을 읽을 수 있게 함("세션 Hash 자체에 5분을 더 주는 이유")
- [`RedisIndexedHttpSessionConfiguration.java`](https://github.com/spring-projects/spring-session/blob/3.5.7/spring-session-data-redis/src/main/java/org/springframework/session/data/redis/config/annotation/web/http/RedisIndexedHttpSessionConfiguration.java): `springSessionRedisMessageListenerContainer`가 삭제·만료 채널과 생성 패턴을 저장소에 연결(앱마다 구독, "앱 서버마다 돕니다"), `EnableRedisKeyspaceNotificationsInitializer`가 시작 시 `ConfigureRedisAction`을 실행하고 `NO_OP`이면 건너뜀
- [`ConfigureNotifyKeyspaceEventsAction.java`](https://github.com/spring-projects/spring-session/blob/3.5.7/spring-session-data-redis/src/main/java/org/springframework/session/data/redis/config/ConfigureNotifyKeyspaceEventsAction.java): `CONFIG GET notify-keyspace-events` 후 `E`, `g`, `x`가 없으면 더해 `CONFIG SET`. 읽기 실패 시 `IllegalStateException`("시작할 때 `CONFIG SET notify-keyspace-events`로", "`CONFIG`가 막힌 환경에서는 시작이 실패합니다")
- [`RedisSessionMapper.java`](https://github.com/spring-projects/spring-session/blob/3.5.7/spring-session-data-redis/src/main/java/org/springframework/session/data/redis/RedisSessionMapper.java): `creationTime`·`lastAccessedTime`·`maxInactiveInterval`이 없으면 `IllegalStateException`("필수 필드가 없어 읽으면 예외")
- [`SessionRepositoryFilter.java`](https://github.com/spring-projects/spring-session/blob/3.5.7/spring-session-core/src/main/java/org/springframework/session/web/http/SessionRepositoryFilter.java): `getRequestedSession`이 처음 필요할 때 `findById`를 부르고 결과를 요청 동안 보관, `getSession`이 찾은 세션에 `setLastAccessedTime(Instant.now())`, `doFilterInternal`의 `finally`에서 `commitSession` → `save`. `findById`와 `save`의 예외를 잡지 않음("필터는 이 저장 예외도 잡지 않습니다", "한 업무는 반영된 채 저장에서 예외가 날 수 있습니다"). [`OnCommittedResponseWrapper.java`](https://github.com/spring-projects/spring-session/blob/3.5.7/spring-session-core/src/main/java/org/springframework/session/web/http/OnCommittedResponseWrapper.java): `flushBuffer`·`sendRedirect` 등이 실제 확정 전에 `doOnResponseCommitted`를 먼저 부름("응답이 확정되기 직전에 일어나므로"). `SessionRepositoryResponseWrapper.onResponseCommitted`도 `commitSession`을 부르고, `commitSession`이 요청 세션 캐시를 비우므로 두 번째 커밋은 `findById`부터 다시 함("필터가 그 순간 한 번 저장하고, 요청 끝에 한 번 더 저장합니다"), `HttpSessionWrapper.invalidate`가 `deleteById`, 세션이 없고 무효화됐으면 쿠키 만료("세션을 처음 찾을 때", "세션을 찾은 요청만", "예외를 잡지 않습니다")
- `SessionRepositoryFilter.DEFAULT_ORDER = Integer.MIN_VALUE + 50`: Spring Boot의 Spring Security 필터 기본 순서(`OrderedFilter.REQUEST_WRAPPER_FILTER_MAX_ORDER - 100`, 곧 -100)보다 앞("Spring Security보다 앞에 놓이는 필터"). Spring Boot 쪽 순서는 [`SecurityProperties.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/security/SecurityProperties.java)의 `DEFAULT_FILTER_ORDER`
- [`DefaultCookieSerializer.java`](https://github.com/spring-projects/spring-session/blob/3.5.7/spring-session-core/src/main/java/org/springframework/session/web/http/DefaultCookieSerializer.java): `cookieName = "SESSION"`, `useBase64Encoding = true`, `useHttpOnlyCookie = true`, `sameSite = "Lax"`
- [`UuidSessionIdGenerator.java`](https://github.com/spring-projects/spring-session/blob/3.5.7/spring-session-core/src/main/java/org/springframework/session/UuidSessionIdGenerator.java): `UUID.randomUUID()`
- [`MapSession.java`](https://github.com/spring-projects/spring-session/blob/3.5.7/spring-session-core/src/main/java/org/springframework/session/MapSession.java): `DEFAULT_MAX_INACTIVE_INTERVAL_SECONDS = 1800`, `isExpired`는 `Instant.now()`(앱 시계)로 판단("앱 서버의 시계로 계산"). 절대 만료 관련 설정은 Spring Session 3.5.7의 core·data-redis 주 소스에서 찾지 못함
- [`SpringSessionBackedSessionRegistry.java`](https://github.com/spring-projects/spring-session/blob/3.5.7/spring-session-core/src/main/java/org/springframework/session/security/SpringSessionBackedSessionRegistry.java): 생성자가 `FindByIndexNameSessionRepository`를 요구, `getAllSessions`는 `findByPrincipalName`, `getSessionInformation`은 `findById`, `registerNewSession`은 아무것도 하지 않음
- [`SpringSessionBackedSessionInformation.java`](https://github.com/spring-projects/spring-session/blob/3.5.7/spring-session-core/src/main/java/org/springframework/session/security/SpringSessionBackedSessionInformation.java): `expireNow`가 세션을 찾아 `EXPIRED_ATTR` 속성을 `true`로 쓰고 저장. 지우지 않음("이때 `bbb`를 지우지 않습니다")
- [`PrincipalNameIndexResolver.java`](https://github.com/spring-projects/spring-session/blob/3.5.7/spring-session-core/src/main/java/org/springframework/session/PrincipalNameIndexResolver.java): 회원 이름을 `SPRING_SECURITY_CONTEXT`의 `authentication.name`에서 얻음
- 공식 문서 [`configuration/redis.adoc`](https://github.com/spring-projects/spring-session/blob/3.5.7/spring-session-docs/modules/ROOT/pages/configuration/redis.adoc): "Choosing Between `RedisSessionRepository` and `RedisIndexedSessionRepository`"(Spring Boot의 기본은 `RedisSessionRepository`, Cluster에서 색인 저장소는 무작위 노드 하나의 알림만 구독한다는 CAUTION), "Listening to Session Events"·"Finding All Sessions of a Specific User"(색인 저장소가 필요)
- 공식 문서 [`spring-security.adoc`](https://github.com/spring-projects/spring-session/blob/3.5.7/spring-session-docs/modules/ROOT/pages/spring-security.adoc): "Spring Security Concurrent Session Control"(Spring Session의 `SessionRegistry`는 클러스터 환경에서도 동작, `FindByIndexNameSessionRepository` 필요)
- 공식 문서 [`api.adoc`](https://github.com/spring-projects/spring-session/blob/3.5.7/spring-session-docs/modules/ROOT/pages/api.adoc): "Using `RedisSessionRepository`"(세션 이벤트를 발행하지 않음), "`SessionDeletedEvent` and `SessionExpiredEvent`"(키스페이스 알림 `Egx` 필요, 보안 환경에서 `CONFIG`가 막히면 `ConfigureRedisAction.NO_OP`)

**Spring Boot 3.5.16**

- [`RedisSessionProperties.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/session/RedisSessionProperties.java): `spring.session.redis.*`의 `namespace`(기본 `spring:session`), `flush-mode`(`ON_SAVE`), `save-mode`(`ON_SET_ATTRIBUTE`), `configure-action`(`NOTIFY_KEYSPACE_EVENTS`, `NONE`), `repository-type`(`DEFAULT`, `INDEXED`)
- [`RedisSessionConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/session/RedisSessionConfiguration.java): `repository-type`이 없거나 `default`면 `RedisHttpSessionConfiguration`(기본 저장소), `indexed`면 `RedisIndexedHttpSessionConfiguration`. `configure-action`이 `NONE`이면 `ConfigureRedisAction.NO_OP`. 만료는 `SessionProperties.determineTimeout`에 `server.servlet.session.timeout`을 대체값으로 넘김
- [`SessionProperties.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/session/SessionProperties.java): `spring.session.timeout`, 없으면 대체값 사용. [`Session.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot/src/main/java/org/springframework/boot/web/servlet/server/Session.java): `timeout = Duration.ofMinutes(30)`
- [`SessionAutoConfiguration.java`](https://github.com/spring-projects/spring-boot/blob/v3.5.16/spring-boot-project/spring-boot-autoconfigure/src/main/java/org/springframework/boot/autoconfigure/session/SessionAutoConfiguration.java): `DefaultCookieSerializer`에 `server.servlet.session.cookie.*`(이름, 도메인, 경로, `HttpOnly`, `Secure`, `SameSite` 등)를 적용

**Spring Security 6.5.11**

- [`SessionManagementConfigurer.java`](https://github.com/spring-projects/spring-security/blob/6.5.11/config/src/main/java/org/springframework/security/config/annotation/web/configurers/SessionManagementConfigurer.java): 기본 세션 고정 방어는 `ChangeSessionIdAuthenticationStrategy`. 동시 로그인 제한을 켜면 `ConcurrentSessionControlAuthenticationStrategy` → 세션 고정 방어 → `RegisterSessionAuthenticationStrategy` 순서, `ConcurrentSessionFilter` 추가. `getSessionRegistry`는 `SessionRegistry` 빈이 없으면 `SessionRegistryImpl`을 만듦
- [`SessionRegistryImpl.java`](https://github.com/spring-projects/spring-security/blob/6.5.11/core/src/main/java/org/springframework/security/core/session/SessionRegistryImpl.java): `ConcurrentHashMap`에 보관(앱 메모리)
- [`ConcurrentSessionControlAuthenticationStrategy.java`](https://github.com/spring-projects/spring-security/blob/6.5.11/web/src/main/java/org/springframework/security/web/authentication/session/ConcurrentSessionControlAuthenticationStrategy.java): `getAllSessions`로 세고 허용 수 이상이면, `exceptionIfMaximumExceeded`면 예외, 아니면 `lastRequest` 순으로 정렬해 오래된 것부터 `expireNow`. 세기와 등록을 묶는 락이 없음("두 곳에서 동시에 로그인하면")
- [`ConcurrentSessionFilter.java`](https://github.com/spring-projects/spring-security/blob/6.5.11/web/src/main/java/org/springframework/security/web/session/ConcurrentSessionFilter.java): 세션이 있으면 요청마다 `getSessionInformation`, 만료됐으면 로그아웃 처리 후 만료 전략 실행("레지스트리에 이 세션의 정보를 묻고")
- [`ChangeSessionIdAuthenticationStrategy.java`](https://github.com/spring-projects/spring-security/blob/6.5.11/web/src/main/java/org/springframework/security/web/authentication/session/ChangeSessionIdAuthenticationStrategy.java): `request.changeSessionId()`
- [`SecurityContextLogoutHandler.java`](https://github.com/spring-projects/spring-security/blob/6.5.11/web/src/main/java/org/springframework/security/web/authentication/logout/SecurityContextLogoutHandler.java): `invalidateHttpSession` 기본 `true`
- [`HttpSessionRequestCache.java`](https://github.com/spring-projects/spring-security/blob/6.5.11/web/src/main/java/org/springframework/security/web/savedrequest/HttpSessionRequestCache.java) `saveRequest`: 로그인 전 요청을 `request.getSession()`으로 세션에 저장("로그인 뒤 돌아갈 원래 요청")
- [`HttpSessionSecurityContextRepository.java`](https://github.com/spring-projects/spring-security/blob/6.5.11/web/src/main/java/org/springframework/security/web/context/HttpSessionSecurityContextRepository.java): `SPRING_SECURITY_CONTEXT` 속성에 로그인 정보, `request.getSession(false)`로 읽음

**Spring Data Redis 3.5.13**

- [`RedisTemplate.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/core/RedisTemplate.java): `expireAt`이 `pExpireAt`(`PEXPIREAT`), `expire(key, timeout, unit)`이 `pExpire`(`PEXPIRE`)
- [`LettuceClusterKeyCommands.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/connection/lettuce/LettuceClusterKeyCommands.java) `rename`: 두 키의 슬롯이 다르면 `dump` → `restore` → `del`("`RENAME` 대신 옛 키를 읽어 새 키에 복원하고 옛 키를 지우는 세 단계")
- Lettuce [`RedisURI.java`](https://github.com/redis/lettuce/blob/6.6.0.RELEASE/src/main/java/io/lettuce/core/RedisURI.java)(6.6.0.RELEASE): `DEFAULT_TIMEOUT = 60`(초). `spring.data.redis.timeout`을 정하지 않았을 때 이 값이 명령 타임아웃이 되는 경로는 [Spring 앱의 Redis 연결과 명령 타임아웃](./client-connection-timeout.md)의 근거에 있음("Lettuce의 명령 타임아웃은 60초")
- [`DefaultHashOperations.java`](https://github.com/spring-projects/spring-data-redis/blob/3.5.13/src/main/java/org/springframework/data/redis/core/DefaultHashOperations.java): `putAll`이 `hMSet`(`HMSET`)

**Redis 8.2.10 소스와 공식 문서**

- [`src/config.c`](https://github.com/redis/redis/blob/8.2.10/src/config.c): `maxmemory-policy` 기본 `noeviction`. [`redis.conf`](https://github.com/redis/redis/blob/8.2.10/redis.conf): `notify-keyspace-events ""`(기본 꺼짐)
- [`src/commands/hmset.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/hmset.json), [`append.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/append.json), [`sadd.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/sadd.json): `DENYOOM` 플래그. [`hgetall.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/hgetall.json), [`exists.json`](https://github.com/redis/redis/blob/8.2.10/src/commands/exists.json): 읽기 전용, `DENYOOM` 없음("`HGETALL`과 `EXISTS`는 되지만 ... `HMSET`이 실패")
- [`src/server.c`](https://github.com/redis/redis/blob/8.2.10/src/server.c) `processCommand`: `maxmemory`가 있고 `performEvictions()`가 `EVICT_FAIL`이면 `DENYOOM` 명령을 그 실행이 메모리를 실제로 늘리는지와 무관하게 OOM 에러로 거부("같은 크기의 값을 덮어쓰는 갱신이어도 거부됩니다")
- [keyspace-notifications](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/develop/pubsub/keyspace-notifications.md): Pub/Sub은 fire and forget, "Timing of expired events"(키를 실제로 지울 때 알림), "Events in a cluster"(노드별로만 나감)
- [eviction](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/develop/reference/eviction/index.md): 정책별 후보 범위
- [hgetall](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/hgetall.md): O(N), N은 Hash 크기. [rename](https://github.com/redis/docs/blob/e53775dde0ee7e39ebe327e7ffff583caa9fbd65/content/commands/rename.md): 키 이름 바꾸기

**설계 비교의 출처**

- [RFC 7519](https://www.rfc-editor.org/rfc/rfc7519): JWT의 정의와 `exp` 클레임. JWT 절의 나머지 비교는 이 문서의 판단입니다

## 실무 포인트

### 먼저 정할 것

| 중요도 | 질문 | 판단 |
| --- | --- | --- |
| 필수 | 동시 로그인 제한이나 "회원의 모든 세션 끝내기"가 필요한가? | 필요하면 `repository-type=indexed`와 Spring Session의 세션 레지스트리 빈. 필요 없으면 기본 저장소가 명령 수가 절반이고 알림 설정도 필요 없습니다 |
| 필수 | 세션을 어느 Redis에 둘 것인가? | 캐시와 같은 Redis에 두면 축출이 곧 강제 로그아웃이고, `noeviction`이면 메모리가 차는 순간 세션을 만들거나 이어 쓰는 요청이 모두 실패합니다. 가능하면 세션 전용으로 분리합니다 |
| 필수 | Redis가 멈추면 로그인 화면을 어떻게 보일 것인가? | 기본은 요청 실패입니다. 명령 타임아웃과 회로 차단기로 빨리 실패하게 하고, 공개 화면이 세션을 찾지 않게 해 피해 범위를 줄입니다 |
| 알면 유리 | 재시작·장애 조치 때 로그아웃을 얼마나 견디는가? | 전체 로그아웃이 안 되면 영속성을 켜고, 장애 조치 직전 로그인·로그아웃 몇 건을 잃을 수 있다는 것을 받아들일지 정합니다 |
| 알면 유리 | 동시 로그인 제한이 정확해야 하는가? | 과금·계정 공유 차단이면 동시 로그인 경쟁을 막는 별도 장치가 필요합니다 |
| 참고 | JWT로 바꿀 이유가 있는가? | 즉시 로그아웃·동시 로그인 제한이 요구사항이면 세션이 낫습니다 |

### 개발자가 정할 수 있는 것과 관찰해야 하는 것

- **정할 수 있는 것**: 저장소 종류(`spring.session.redis.repository-type`), 만료(`spring.session.timeout`), 이름공간(`spring.session.redis.namespace`), 저장·플러시 모드, 알림 자동 설정(`spring.session.redis.configure-action`), 쿠키 속성(`server.servlet.session.cookie.*`), 세션 레지스트리 빈과 `maximumSessions`, 명령 타임아웃(`spring.data.redis.timeout`), 세션에 넣는 속성의 크기.
- **관찰할 것**:
  - 요청당 Redis 명령 수: 같은 구간 동안 `INFO commandstats`의 `hgetall`·`exists`·`hmset`·`pexpireat` 호출 수가 늘어난 양 ÷ 앱 요청 수. 값은 누적치라 두 시점의 차이를 쓰고, 세션 말고 다른 용도로 같은 명령을 쓰면 섞입니다. 기본 저장소에서 로그인 요청당 4개 근처가 기대값입니다.
  - 세션 키 수와 크기: 이름공간 표본을 `SCAN`으로 세고, 큰 세션은 `MEMORY USAGE`로 봅니다. 로그인 사용자 수보다 훨씬 많으면 비로그인 세션 생성을 의심합니다.
  - Redis의 `evicted_keys`(0이 아니면 세션이 밀려나고 있을 수 있음), `used_memory`와 `maxmemory`의 비율.
  - 색인 저장소라면 회원 색인 Set의 크기와, 그 안에서 Hash가 이미 없는 ID의 비율. 동시 로그인 제한으로 밀려난 세션도 만료까지 남으므로 크기만으로는 판단하지 않고, Hash 없는 ID가 계속 쌓이면 알림을 놓치고 있다고 봅니다.
  - 앱 쪽 Redis 명령 지연과 타임아웃 예외 수, 로그인 직후 "Session was invalidated" 예외 수.

### 증상과 확인 순서

| 증상 | 먼저 볼 것 | 원인 후보 | 대응과 부작용 |
| --- | --- | --- | --- |
| 서버 두 대에서 동시 로그인 제한이 안 걸림 | 세션 레지스트리 빈 등록 여부, `repository-type` | 기본 메모리 레지스트리 사용 | 색인 저장소 + Spring Session 레지스트리. 요청당 명령이 두 배 가까이로 |
| 로그인한 사람만 500 오류, 공개 화면은 정상 | Redis 연결 오류·타임아웃 로그, OOM 에러, 오류 응답을 받은 요청의 업무 결과가 실제로 반영됐는지 | Redis 장애, 또는 `noeviction`에서 메모리 한도 | 장애 조치·메모리 증설. 원인이 OOM이면 축출을 켜는 것은 강제 로그아웃으로 바꾸는 선택 |
| 사용자들이 일제히 로그아웃됨 | Redis 재시작 시각, 장애 조치 이력, `evicted_keys` | 영속성 없는 재시작, 축출 | 영속성 켜기, 세션 Redis 분리. 영속성은 디스크 쓰기 비용을 더함 |
| 활동 중인데 30분 뒤 로그아웃됨 | 그 화면이 부르는 API가 세션을 찾는지 | 세션을 찾지 않는 요청만 이어짐 | 주기적으로 세션을 찾는 요청을 두거나 만료를 늘림. 만료를 늘리면 세션 수가 늘어남 |
| 로그인 직후 간헐적 요청 실패 | "Session was invalidated" 예외 | 로그인 중 옛 세션 ID로 보낸 동시 요청 | 로그인 응답을 받은 뒤 다른 요청을 보내게 화면 순서 조정 |
| 색인 저장소에서 시작 실패 | `CONFIG` 권한 오류 | 관리형 Redis가 `CONFIG`를 막음 | `configure-action=none` + 서버에 `notify-keyspace-events` 설정. 서버 설정을 빼먹으면 색인 정리와 만료 이벤트가 조용히 멈춤(세션 만료 자체는 동작) |
| 세션 만료 감사 로그가 두 번씩 찍힘 | 앱 서버 대수 | 앱마다 알림을 받아 이벤트 발행 | 이벤트 처리를 중복에 견디게(세션 ID로 한 번만 기록) |

### Spring에서 확인할 것

- 세션에서 꺼낸 객체의 안쪽만 고치고 `setAttribute`를 다시 부르지 않으면 저장되지 않습니다. 장바구니 같은 가변 객체를 세션에 넣었다면 특히 확인합니다.
- Spring Security나 세션에 넣는 클래스를 바꾸는 배포는 이미 저장된 세션을 못 읽게 만들 수 있습니다. 배포 전에 [직렬화기](./serializer.md)의 점검을 따릅니다.
- `spring.data.redis.timeout`을 명시하지 않으면 Redis가 멈췄을 때 요청 스레드가 명령마다 60초를 기다립니다.

## 말로 설명하기

### 30초 버전

앱 서버가 여러 대면 Tomcat 메모리 세션은 다른 서버에서 보이지 않고, 고정 세션은 배포 때마다 로그아웃을 만듭니다. 그래서 Spring Session으로 세션을 Redis Hash 하나에 두고, 모든 서버가 같은 키를 읽습니다. 만료는 저장할 때마다 Redis 키에 "마지막 접근 + 30분"을 걸어 Redis가 지키고, 로그아웃은 키 삭제라 그 뒤로 어느 서버에서든 그 세션을 얻지 못합니다. 대가는 로그인한 요청마다 Redis 왕복 4번이 붙고, Redis가 멈추면 로그인한 요청이 실패하며, 키를 잃으면 사용자가 로그아웃된다는 것입니다. 동시 로그인 제한은 색인 저장소가 필요하고 명령이 두 배 가까이 늘어납니다.

### 2분 버전

Spring Session은 필터로 `HttpSession`을 바꿔 끼웁니다. 요청 중 세션을 처음 찾을 때 `HGETALL`로 통째로 읽고, 응답을 마칠 때 키가 아직 있는지 `EXISTS`로 확인한 뒤 바뀐 필드만 `HMSET`으로 쓰고 `PEXPIREAT`로 만료를 "마지막 접근 + 30분"에 다시 겁니다. 세션을 찾은 요청만 만료를 늘리고, 시각은 앱 서버 시계로 계산합니다. 로그인할 때는 세션 고정 공격을 막으려고 ID를 바꾸며, Redis에서는 `RENAME`입니다.

동시 로그인 제한은 "이 회원의 세션 목록"이 있어야 하는데, 기본 저장소는 키가 세션 ID뿐이라 못 하고 Spring Security의 기본 레지스트리는 서버 메모리에서 셉니다. 그래서 색인 저장소로 바꿔 회원별 Set을 두고, Spring Session의 레지스트리를 등록합니다. 밀려난 세션은 지우지 않고 "만료됨" 표시만 하며, 그 기기의 다음 요청에서 끝납니다. 색인 정리는 Redis 키스페이스 알림에 기대는데, 알림은 Pub/Sub이라 끊기면 잃고, 앱 서버마다 받으며, Cluster에서는 일부를 놓칩니다. 세기와 등록이 원자적이지 않아 두 곳 동시 로그인은 제한을 넘을 수 있습니다.

장애는 두 모양입니다. Redis에 닿지 않으면 로그인한 요청이 예외로 실패하고, 키를 잃으면(영속성 없는 재시작, 비동기 복제의 장애 조치, 축출) 오류 없이 로그아웃됩니다. 장애 조치로 `DEL`을 잃으면 로그아웃한 세션이 되살아날 수도 있습니다. 그래서 세션은 캐시와 분리한 Redis에 두고, 명령 타임아웃을 짧게 잡습니다. JWT는 요청마다 조회가 없어 Redis 장애에 강하지만, 즉시 로그아웃과 동시 로그인 제한을 잃고 이를 메우려면 다시 서버 쪽 저장소가 필요합니다.

### 예상 질문과 꼬리 질문

**Q. 고정 세션으로 충분하지 않나요? 코드도 안 바뀌는데요.**
요청을 같은 서버로 보내는 것까지는 됩니다. 하지만 세션이 그 서버 메모리에만 있어 배포나 축소로 서버가 사라지면 그 서버의 사용자가 모두 로그아웃되고, 다른 서버에 있는 같은 회원의 세션을 끝낼 수도 없습니다. 배포가 잦고 동시 로그인 제한이 필요하면 공유 저장소가 필요합니다. (본문: "앱 서버 메모리의 세션으로는 왜 부족한가")

- **꼬리: 그럼 Redis 세션은 대가가 없나요?**
  로그인한 요청마다 Redis 왕복이 붙고, Redis가 로그인 기능의 필수 의존성이 됩니다. Redis가 멈추면 로그인한 요청이 실패합니다. 고정 세션이 서버 하나의 장애를 그 서버 사용자의 로그아웃으로 국한했다면, Redis 세션은 Redis 장애를 전체 로그인 실패로 키웁니다.

**Q. 요청 하나에 Redis에 몇 번 가나요?**
응답을 한 번에 마치는 보통 경로에서, 기본 저장소에서 세션을 찾는 요청이면 `HGETALL`, `EXISTS`, `HMSET`, `PEXPIREAT` 네 번입니다. 응답이 중간에 확정되면 저장이 두 번 일어나 읽기와 확인이 하나씩 더 붙습니다. 하나씩 응답을 기다려 보내므로 왕복 시간 × 4가 붙습니다. 색인 저장소면 그림자 키와 분 단위 Set 갱신이 붙어 7~8번, 동시 로그인 제한까지 켜면 세션을 한 번 더 읽어 8~9번입니다. 세션을 찾지 않는 요청은 0번입니다. (본문: "요청이 끝날 때 바뀐 필드만 쓰고, 만료 시각을 다시 겁니다", "요청마다 드는 비용이 늘어납니다")

- **꼬리: 아무것도 안 바꾼 요청인데 왜 쓰나요?**
  세션을 찾는 순간 마지막 접근 시각이 바뀌기 때문입니다. 그 시각을 저장하고 만료를 다시 걸어야 "활동하는 동안 로그인 유지"가 됩니다. 쓰지 않으면 활동 중인데도 처음 로그인 30분 뒤 끊깁니다.
- **꼬리: `EXISTS`는 왜 필요한가요? 빼면 한 번 줄잖아요.**
  다른 서버에서 로그아웃으로 지운 세션을, 동시에 처리 중이던 요청이 다시 써서 되살리지 않게 하려는 확인입니다. 다만 확인과 쓰기가 명령 두 개라 그 사이에 지워지면 막지 못합니다. 그때는 필수 필드가 빠진 키가 만료까지 남지만, 쿠키가 지워졌으므로 보통 다시 읽히지 않습니다.

**Q. 세션 만료는 누가 처리하나요?**
기본 저장소는 저장할 때마다 Redis 키에 "마지막 접근 + 30분" 절대 시각을 걸고, Redis가 지웁니다. 앱은 별도 작업을 돌리지 않습니다. 읽을 때 앱 시계로 한 번 더 만료를 확인합니다. (본문: "만료: 마지막 접근부터 30분은 Redis의 TTL이 지킵니다")

- **꼬리: 사용자가 화면을 쓰고 있는데 로그아웃됐다는 신고가 왔습니다.**
  세션을 찾지 않는 요청만 이어졌는지 먼저 봅니다. 만료는 세션을 찾은 요청만 늘립니다. 그다음 Redis의 `evicted_keys`로 축출을, 재시작이나 장애 조치 이력으로 키 손실을 확인합니다.
- **꼬리: 12시간 지나면 무조건 다시 로그인하게 할 수 있나요?**
  이 버전의 Spring Session에는 그런 설정이 없습니다. 세션의 생성 시각을 앱이 검사해 직접 끝내야 합니다.

**Q. 동시 로그인 제한을 켰는데 서버 두 대에서 안 걸립니다.**
세션 레지스트리를 빈으로 등록하지 않으면 Spring Security가 서버 메모리 안의 레지스트리를 만들어 서버마다 따로 셉니다. Spring Session의 레지스트리를 등록해야 하고, 그 레지스트리는 회원 이름으로 세션을 찾는 저장소가 필요하므로 색인 저장소로 바꿔야 합니다. (본문: "기본 설정으로는 서버 두 대에서 제한이 동작하지 않습니다")

- **꼬리: 색인 저장소로 바꾸면 무엇을 치르나요?**
  요청당 명령이 두 배 가까이로 늘고, 동시 로그인 제한 필터가 요청마다 세션을 한 번 더 읽습니다. 만료 정리를 키스페이스 알림에 맡기므로 `CONFIG` 권한이나 서버 설정이 필요하고, 알림을 놓치면 회원 색인에 죽은 ID가 쌓입니다.
- **꼬리: 정확히 1개로 막히나요?**
  아닙니다. 세고 등록하는 일이 원자적이지 않아, 두 서버에서 거의 동시에 로그인하면 둘 다 0개를 보고 통과할 수 있습니다. 정확해야 하면 회원 단위 락 같은 별도 장치가 필요합니다. (본문: "두 곳에서 동시에 로그인하면 제한을 넘을 수 있습니다")
- **꼬리: 밀려난 노트북 세션은 언제 끝나나요?**
  즉시 지워지지 않고 "만료됨" 표시만 됩니다. 노트북이 다음 요청을 보낼 때 필터가 표시를 보고 로그아웃 처리합니다. 노트북이 요청을 보내지 않으면 만료 시각까지 키가 남습니다.
- **꼬리: 새 로그인을 거부하는 쪽이 더 안전하지 않나요?**
  로그아웃하지 않고 브라우저를 닫은 사용자가 만료 시각까지 다른 기기로 로그인할 수 없게 됩니다. 계정 공유를 막는 효과보다 정상 사용자의 불편이 크기 쉬워, 보통은 이전 세션을 끝내는 쪽이 낫습니다.

**Q. 만료 이벤트로 감사 로그를 남기려고 합니다.**
기본 저장소는 이벤트를 내지 않으므로 색인 저장소가 필요합니다. 이벤트는 Redis가 키를 실제로 지울 때 나가므로 정확한 만료 시각에 오지 않을 수 있고, 앱 서버마다 받아 서버 대수만큼 발행되며, 모든 서버의 구독이 끊겨 있던 동안의 알림은 사라집니다. 중복을 견디게 만들고, 빠짐없이 남아야 하는 기록이면 이 경로에만 맡기지 않습니다. (본문: "이 장치들의 약점")

- **꼬리: 왜 세션 Hash를 5분 더 남기나요?**
  알림을 받았을 때 그 세션이 누구 것이었는지 읽어야 색인에서 뺄 수 있기 때문입니다. 만료는 빈 그림자 키에 걸고, 내용은 5분 더 둡니다.

**Q. 로그인할 때 세션 ID는 왜 바뀌나요?**
세션 고정 공격을 막기 위해서입니다. 공격자가 심어 둔 로그인 전 ID가 로그인 뒤에도 유효하면 공격자가 그 ID로 행세할 수 있습니다. Spring Security가 기본으로 ID를 바꾸고, Redis에서는 `RENAME`과 새 쿠키로 나타납니다. (본문: "로그인 순간 세션 ID를 바꾸는 이유와 대가")

- **꼬리: 그 때문에 생기는 문제는요?**
  로그인 요청 처리 중 옛 ID로 함께 보낸 요청은 저장할 때 옛 키가 없어 "Session was invalidated"로 실패할 수 있습니다. 소스로 추론한 경로라 재현으로 확인할 일입니다.

**Q. Redis가 죽으면 사용자는 로그아웃되나요?**
연결이 안 되는 경우는 로그아웃이 아니라 요청 실패입니다. 세션 읽기 예외를 Spring Session도 Spring Security도 잡지 않아 로그인한 요청이 오류로 끝나고, 로그인도 실패합니다. 세션을 찾지 않는 공개 화면은 정상입니다. 세션 저장에서 실패하면 업무 처리는 이미 끝났을 수 있고, 응답이 이미 확정됐다면 사용자는 성공 코드를 받거나 끊긴 응답을 받기도 하므로, 오류 응답만 보고 다시 보내지 않고 업무 결과를 먼저 확인합니다. 로그아웃처럼 보이는 것은 키를 잃었을 때입니다. (본문: "멈췄을 때: 로그아웃이 아니라 요청 실패입니다")

- **꼬리: 예외를 잡아 비로그인으로 처리하면 낫지 않나요?**
  사용자는 로그인 화면으로 가지만 로그인도 Redis가 필요해 실패하고, 재시도가 몰립니다. Redis가 돌아오면 옛 세션이 그대로 살아 있어 상태가 혼란스럽습니다. 명령 타임아웃을 짧게 잡고 회로 차단기로 빨리 실패하게 해 스레드를 지키는 쪽이 단순합니다.
- **꼬리: 타임아웃을 안 정하면요?**
  Lettuce 기본 60초 동안 명령마다 스레드가 붙잡혀 요청 스레드가 바닥나고, 세션과 무관한 요청까지 멈출 수 있습니다.

**Q. 세션을 캐시와 같은 Redis에 두면 안 되나요?**
축출 정책이 있으면 세션도 후보입니다. 세션 키는 늘 TTL이 있어 `volatile-*`에서도 후보이고, 축출은 사용자에게 갑작스러운 로그아웃입니다. 축출을 끈 `noeviction`이면 메모리가 차는 순간 `HMSET`이 같은 크기의 갱신이어도 OOM으로 거부되어 세션을 만들거나 이어 쓰는 요청이 모두 실패합니다. 기본 플러시 모드에서는 이 실패가 업무 처리 뒤의 세션 저장에서 나므로 업무 결과는 반영되어 있을 수 있습니다. 그래서 세션은 분리하는 것이 안전합니다. (본문: "재시작·장애 조치·축출로 키를 잃으면: 오류가 아니라 강제 로그아웃입니다")

- **꼬리: 분리할 수 없다면요?**
  "축출 = 로그아웃"을 받아들이는 결정을 명시하고, `evicted_keys`와 메모리 사용률에 경보를 겁니다. 세션 속성을 작게 유지해 세션이 차지하는 몫을 줄입니다.

**Q. 장애 조치가 있으면 세션은 안전한가요?**
복제가 비동기라 주 서버가 응답한 마지막 쓰기를 잃을 수 있습니다. 방금 로그인한 사용자는 로그아웃되고, 방금 로그아웃한 세션은 `DEL`을 잃으면 되살아나며, 동시 로그인 제한의 "만료됨" 표시를 잃으면 밀려난 세션이 다시 살아납니다. 복제 성질로 추론한 결과입니다. (본문: "재시작·장애 조치·축출로 키를 잃으면: 오류가 아니라 강제 로그아웃입니다")

- **꼬리: 로그아웃한 세션이 되살아나는 게 실제로 위험한가요?**
  브라우저 쿠키는 지워졌으므로 그 사용자 자신은 다시 쓰지 않습니다. 위험은 그 세션 ID를 이미 탈취한 사람이 있을 때이며, 탈취자가 계속 요청하면 만료가 계속 밀려 끝나지 않습니다. 보안 요구가 높으면 로그아웃 시 별도 폐기 기록을 두는 것을 검토합니다.

**Q. JWT로 바꾸면 이 문제가 다 사라지나요?**
요청마다 Redis를 보지 않으므로 Redis 장애와 왕복 비용은 사라집니다. 대신 발급한 토큰을 만료 전에 회수할 수 없어 즉시 로그아웃, 강제 로그아웃, 동시 로그인 제한, 권한 변경 즉시 반영을 잃습니다. 이를 메우려고 짧은 액세스 토큰과 서버에 저장하는 갱신 토큰을 쓰면, 상태가 다시 Redis나 DB로 돌아옵니다. 조회 빈도를 요청마다에서 갱신 주기마다로 줄이는 선택입니다. (본문: "세션 대신 JWT를 쓰면 무엇을 얻고 잃는가")

- **꼬리: 폐기 목록을 Redis에 두면요?**
  요청마다 폐기 목록을 조회하게 되어 Redis 세션과 같은 의존성이 돌아옵니다. 차이는 조회 결과가 "있으면 거부"라는 점뿐이라, Redis 장애 때 통과시킬지 막을지를 다시 정해야 합니다.

**Q. 세션 데이터가 커지면 무엇이 문제인가요?**
`HGETALL`이 모든 속성을 읽으므로, 그 요청이 쓰지 않는 큰 속성도 매번 옮기고 역직렬화합니다. 동시 로그인 제한을 켜면 두 번입니다. 세션에는 로그인 확인에 필요한 작은 값만 두고, 장바구니 같은 큰 데이터는 별도 키나 DB에 둡니다. (본문: "데이터와 요청이 늘면 무엇이 달라지는가")

- **꼬리: 세션 키 수가 로그인 사용자 수보다 열 배 많습니다.**
  로그인하지 않은 방문에서 세션을 만드는 코드가 있는지 봅니다. 세션 하나가 만들어지면 30분 남으므로, 봇 방문마다 키가 생깁니다. 어디서 `getSession()`이 불리는지 찾아 세션 없이 처리하게 바꿉니다.
