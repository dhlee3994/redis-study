# fork

> 필요한 주제: 7 Persistence, 8 Replication (실제 4주차). 레플리카의 full sync도 RDB를 만들기 때문에 fork를 씁니다.

## 1. 한 줄 요약

`fork()`는 지금 프로세스를 **통째로 복제**해서 자식 프로세스를 만듭니다. 한 번 호출하면 두 번 리턴합니다. 자식에게는 `0`, 부모에게는 자식의 PID가 돌아오고, 같은 코드가 `if`로 갈라집니다.

```c
if ((childpid = fork()) == 0) {
    /* 여기는 자식: RDB 파일 쓰고 exit */
} else {
    /* 여기는 부모: 하던 대로 명령 처리 계속 */
}
```

## 2. Redis는 왜 fork를 쓰나

메모리 전체를 디스크에 써야 하는 일이 있습니다. BGSAVE(RDB), AOF rewrite, 복제 full sync가 그렇습니다.

| 방법 | 문제 |
|---|---|
| 메인 스레드가 직접 쓰기 (`SAVE`) | 수 초~수 분 동안 모든 요청이 멈춤 |
| 스레드를 만들어서 쓰기 | 쓰는 동안 메인이 데이터를 계속 바꿈 → 일관된 스냅샷이 아님. 막으려면 락이 필요 |
| **fork** | 자식은 **fork한 순간의 메모리**를 그대로 가짐 → 락 없이 일관된 스냅샷. 부모는 계속 일함 |

이 "공짜 스냅샷"이 가능한 이유가 [11 copy-on-write](11_copy_on_write.md)입니다. fork는 메모리를 실제로 복사하지 않습니다.

## 3. fork 자체의 비용: 순간 멈춤

메모리는 복사하지 않지만, **"가상 주소 → 물리 페이지" 대응표(페이지 테이블)는 복사**합니다. 메모리가 클수록 표도 커서, fork 호출 동안 메인 스레드가 멈춥니다.

- `INFO stats`의 `latest_fork_usec`로 확인합니다.
- 실험값: RSS 약 100MB에서 **539µs**. 메모리가 수십 GB면 수십~수백 ms까지 늘 수 있고, 환경(가상화 종류 등)에 따라 차이가 큽니다.
- 그 순간 들어온 요청의 p99가 튑니다. RDB 저장 주기를 짧게 잡을수록 이 멈춤이 자주 생깁니다.

## 4. overcommit

fork 직후 자식은 이론상 부모만큼의 메모리를 쓸 **권리**를 가집니다. 리눅스가 "메모리 2배를 약속할 수 없다"고 판단하면 fork가 실패합니다. 그래서 Redis는 `vm.overcommit_memory = 1`을 권장하고, 아니면 시작할 때 경고를 남깁니다(`syscheck.c:117`).

## 5. Redis 소스에서 어디에 나오나

`server.c:7697` `redisFork`

| 줄 | 내용 |
|---|---|
| 7709 | `fork()` |
| 자식 쪽 | `in_fork_child` 설정, `setOOMScoreAdj`(메모리가 모자라면 OOM killer가 **자식을 먼저** 죽이도록), `updateDictResizePolicy`(자식은 dict resize 금지, [11](11_copy_on_write.md)), `dismissMemoryInChild` |
| 부모 쪽 | fork에 걸린 시간을 `stat_fork_time`에 기록 → `latest_fork_usec` |

- `rdb.c:2226` `rdbSaveBackground`: BGSAVE에서 `redisFork`를 부르는 곳
- `child_info_pipe`: 자식이 부모에게 진행 상황과 COW 크기를 보고하는 **파이프**입니다. [01](01_fd_모든것은파일.md)에서 본 fd 목록의 pipe 중 하나입니다.

## 6. 직접 확인

```bash
docker exec redis-os redis-cli DEBUG POPULATE 1000000
```

```bash
docker exec redis-os redis-cli BGSAVE
```

```bash
docker exec redis-os redis-cli INFO stats | grep latest_fork_usec
```

실제 결과: `latest_fork_usec:539` (100만 키, RSS 약 100MB)

주제 7에서 해 볼 것: 키를 500만, 1000만으로 늘려 가며 `latest_fork_usec`이 메모리 크기에 비례해 느는지 봅니다.

## 7. 여기서 멈춤

- **설명할 수 있으면 충분:** Redis가 스레드 대신 fork를 쓰는 이유, fork 순간 멈춤의 원인(페이지 테이블 복사), overcommit 경고의 의미.
- **몰라도 됨:** `exec` 계열, 좀비 프로세스와 `wait` 세부, `vfork`/`clone`.
