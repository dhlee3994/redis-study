# Redis 공부에 필요한 OS 개념

Redis 소스를 읽다가 막히는 OS 개념만 모았습니다. **처음부터 다 읽지 마세요.** 아래 표에서 지금 하고 있는 주제의 줄만 읽으면 됩니다. 각 문서의 마지막 절 "여기서 멈춤"까지가 Redis 공부에 필요한 깊이입니다. 그보다 깊이 들어가지 마세요.

- 기준 소스: `redis/redis` unstable, 커밋 `b540ca4` (본 노트와 같음). 적어둔 줄 번호는 이 커밋에서 확인했습니다.
- 실험 결과: Docker `redis:8.0` 이미지(8.0.6, jemalloc-5.3.0)에서 직접 돌린 값입니다. Apple Silicon Mac + OrbStack 환경입니다.

## 언제 무엇을 읽나

| 언제 | 파일 | 한 줄 요약 |
|---|---|---|
| 주제 1 (복습용) | [01_fd_모든것은파일.md](01_fd_모든것은파일.md) | 커널이 주는 번호표. 소켓도 파이프도 전부 fd |
| 주제 1 (복습용) | [02_시스템콜_유저커널.md](02_시스템콜_유저커널.md) | 커널에 부탁하는 비싼 호출. Redis는 이걸 모아서 부른다 |
| 주제 1 (복습용) | [03_소켓.md](03_소켓.md) | 리슨 소켓 하나와 클라이언트마다 소켓 하나, 커널 버퍼 |
| 주제 1 (복습용) | [04_blocking_nonblocking.md](04_blocking_nonblocking.md) | 없으면 기다리기 vs 없으면 EAGAIN으로 바로 돌아오기 |
| 주제 1 (복습용) | [05_IO멀티플렉싱_epoll_kqueue.md](05_IO멀티플렉싱_epoll_kqueue.md) | "준비된 fd만 알려줘". 싱글 스레드가 빠른 이유 |
| **주제 3·4 (지금)** | [06_메모리할당자_jemalloc.md](06_메모리할당자_jemalloc.md) | malloc 뒤에 있는 할당자. 요청한 크기 ≠ 실제 크기 |
| **주제 3·4 (지금)** | [07_RSS와_used_memory.md](07_RSS와_used_memory.md) | Redis가 센 메모리 vs OS가 본 메모리 |
| **주제 3·4 (지금)** | [08_메모리단편화.md](08_메모리단편화.md) | 키를 절반 지워도 RSS가 그대로인 이유 |
| **주제 3·4 (지금)** | [09_백그라운드스레드_bio.md](09_백그라운드스레드_bio.md) | "싱글 스레드"의 정확한 뜻, UNLINK가 넘기는 곳 |
| 주제 7 (실제 4주차) | [10_fork.md](10_fork.md) | 프로세스를 통째로 복제해서 락 없이 스냅샷 만들기 |
| 주제 7 (실제 4주차) | [11_copy_on_write.md](11_copy_on_write.md) | 쓰는 페이지만 복사. 저장 중 쓰기가 많으면 메모리가 는다 |
| 주제 7 (실제 4주차) | [12_page_cache.md](12_page_cache.md) | write 성공 ≠ 디스크에 기록됨 |
| 주제 7 (실제 4주차) | [13_write와_fsync.md](13_write와_fsync.md) | appendfsync 3가지 = fsync를 누가, 언제 부르나 |
| 주제 8 (실제 4주차) | [14_TCP_기초.md](14_TCP_기초.md) | 바이트 스트림, RTT, 비동기 복제가 유실되는 지점 |

01~05는 [01_C문법_최소가이드.md](../01_C문법_최소가이드.md) 0절과 [02_이벤트루프_흐름지도.md](../02_이벤트루프_흐름지도.md)에서 이미 일부 다룬 내용을 OS 쪽에서 다시 본 것입니다. 1부를 읽다가 막혔던 부분이 있을 때만 보면 됩니다.

---

## 실험 환경 (모든 문서 공통)

Mac에서 Redis를 바로 띄우면 epoll이 아니라 **kqueue**가 쓰이고, `/proc`도 없어서 내부를 들여다보기 어렵습니다. 그래서 Docker를 씁니다. OrbStack은 안에서 리눅스 VM을 돌리므로 운영 환경과 같은 epoll이 쓰입니다.

### Redis 띄우기

`DEBUG POPULATE`(키를 대량으로 만드는 명령)를 쓰려면 debug 명령을 켜야 합니다. 7.0부터 기본값이 꺼짐입니다.

```bash
docker run -d --name redis-os redis:8.0 redis-server --enable-debug-command yes
```

### fd 목록 보기

root로 실행하면 `Permission denied`가 나옵니다. Redis 프로세스의 주인인 `redis` 사용자로 실행해야 보입니다.

```bash
docker exec -u redis redis-os ls -l /proc/1/fd
```

### 스레드 목록 보기

```bash
docker exec redis-os sh -c 'for t in /proc/1/task/*; do echo "$(basename $t) $(cat $t/comm)"; done'
```

### strace: Redis가 부르는 시스템 콜 엿보기

Redis 이미지에는 strace가 없어서, 옆에 alpine 컨테이너를 하나 붙여 Redis 프로세스(PID 1)를 추적합니다.

```bash
docker run --rm -it --pid=container:redis-os --cap-add SYS_PTRACE alpine sh -c "apk add -q strace && strace -f -p 1 -e trace=epoll_pwait,epoll_wait,accept4,read,write,fdatasync 2>&1 | grep -v redis-server | grep -v ') = 0$'"
```

- `--pid=container:redis-os`: Redis 컨테이너의 프로세스가 보이게 합니다.
- `-f`: 메인 스레드뿐 아니라 bio 스레드까지 추적합니다. `[pid 27]`처럼 스레드 번호가 붙습니다.
- `grep -v redis-server`: serverCron이 100ms마다 `/proc/self/stat`을 읽는 줄(RSS 측정, 07번 문서)을 걸러냅니다.
- `grep -v ') = 0$'`: 아무 일 없이 끝난 `epoll_pwait` 줄을 걸러냅니다. 부작용으로 `read(...) = 0`(연결 종료)도 같이 걸러지니, 그 줄을 볼 때는 이 grep을 빼세요.
- Apple Silicon(arm64) 리눅스에는 `epoll_wait` 시스템 콜이 없어서 **`epoll_pwait`** 로 보입니다. 하는 일은 같습니다.

strace를 띄운 상태에서 다른 터미널로 명령을 보냅니다.

```bash
docker exec redis-os redis-cli SET k v
```

### 정리

```bash
docker rm -f redis-os
```
