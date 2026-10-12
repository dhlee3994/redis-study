/* ======================================================================
 * [해설판] ae.h — 이벤트루프의 "설계도" (구조체, 상수, 함수 목록)
 * 원본: https://github.com/redis/redis/blob/unstable/src/ae.h
 * 기준 커밋: b540ca4 (2026-09-30). 코드는 원본 그대로이고, 원본의 영문 주석은
 * 한국어로 옮겼습니다(저작권·라이선스 문구만 원문 유지). "▶"로 시작하는 주석은
 * 추가한 해설입니다.
 *
 * ▶ 이 파일에서 볼 것
 *   1) 상수: 어떤 이벤트에 관심 있는지(READABLE/WRITABLE)를 비트로 표현
 *   2) 콜백 타입 4종: ae가 "언제" 부를지만 알고 "무엇을" 할지는 모르는 이유
 *   3) 구조체 4종: aeFileEvent, aeTimeEvent, aeFiredEvent, aeEventLoop
 * ====================================================================== */

/* 간단한 이벤트 기반 프로그래밍 라이브러리. 원래 Jim(Tcl 인터프리터)의
 * 이벤트루프용으로 작성했지만, 나중에 재사용하기 쉽도록 라이브러리 형태로
 * 옮겼다.
 *
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

/* ▶ include guard: 이 헤더가 여러 번 include돼도 한 번만 들어가게 하는 관용구.
 *   "__AE_H__가 아직 정의 안 됐으면 정의하고 아래 내용을 넣어라." 무시해도 됩니다. */
#ifndef __AE_H__
#define __AE_H__

/* ▶ monotime 타입(= uint64_t, 마이크로초)과 getMonotonicUs()를 가져옴.
 *   monotonic clock = "되돌아가지 않는 시계". 벽시계(System.currentTimeMillis)는
 *   NTP 보정 등으로 뒤로 갈 수 있어서 타이머에 쓰면 위험합니다.
 *   Kotlin 대응: System.nanoTime() */
#include "monotonic.h"

/* ▶ 함수 반환값. Kotlin이면 예외나 Result를 쓰겠지만 C는 정수로 성공/실패를 알립니다. */
#define AE_OK 0
#define AE_ERR -1

/* ▶ [파일 이벤트 마스크] fd 하나에 대해 "무엇을 기다리는가"를 비트로 표현.
 *   READABLE|WRITABLE = 3 이면 "읽기도, 쓰기도 알려줘".
 *   Kotlin 감각: EnumSet<Interest> */
#define AE_NONE 0       /* 등록된 이벤트 없음. */
#define AE_READABLE 1   /* fd가 읽기 가능해지면 발생. */
#define AE_WRITABLE 2   /* fd가 쓰기 가능해지면 발생. */
#define AE_BARRIER 4    /* WRITABLE과 함께 쓴다. 같은 루프 반복에서 READABLE
                           이벤트가 이미 발생했으면 이 이벤트는 절대 발생시키지
                           않는다. 응답을 보내기 전에 디스크에 기록하고, 그 기록을
                           모아서 한 번에 하고 싶을 때 유용하다. */
/* ▶ AE_BARRIER: "같은 바퀴에서 읽기 다음에 쓰기를 하지 마라".
 *   읽고(명령 받고) → 잠들기 전 beforeSleep에서 디스크 fsync → 다음 바퀴에 쓰기(응답).
 *   참고: 현재 Redis 본체는 이 비트를 직접 쓰지 않고, connection 계층의
 *   CONN_FLAG_WRITE_BARRIER로 같은 일을 합니다(socket.c). 개념만 알아두세요. */

/* ▶ [aeProcessEvents의 flags] 위의 마스크와는 "다른 용도"의 비트들입니다. 헷갈리지 마세요.
 *   위: fd에 대한 관심사 / 아래: 이번 루프 한 바퀴를 어떻게 돌지에 대한 옵션.
 *   (1<<0)=1, (1<<1)=2, (1<<2)=4, (1<<3)=8, (1<<4)=16 */
#define AE_FILE_EVENTS (1<<0)      /* ▶ 파일 이벤트 처리해라 */
#define AE_TIME_EVENTS (1<<1)      /* ▶ 타이머 처리해라 */
#define AE_ALL_EVENTS (AE_FILE_EVENTS|AE_TIME_EVENTS)
#define AE_DONT_WAIT (1<<2)        /* ▶ epoll에서 잠들지 말고 바로 돌아와라 */
#define AE_CALL_BEFORE_SLEEP (1<<3) /* ▶ 잠들기 전에 beforesleep 콜백 불러라 */
#define AE_CALL_AFTER_SLEEP (1<<4)  /* ▶ 깨어난 후 aftersleep 콜백 불러라 */

/* ▶ AE_NOMORE: 타이머 콜백이 이 값을 반환하면 "다시 실행하지 마(일회성)".
 *   다른 값 N을 반환하면 "N ms 뒤에 또 불러줘". serverCron은 1000/hz를 반환합니다. */
#define AE_NOMORE -1
/* ▶ 타이머를 지울 때 id를 이 값으로 덮어써서 "삭제 예정" 표시만 합니다. (ae.c 참고) */
#define AE_DELETED_EVENT_ID -1

/* 매크로 */
/* ▶ "이 인자 안 씀" 표시용. 컴파일러 경고를 없애는 용도. Kotlin의 `_` 파라미터와 비슷. */
#define AE_NOTUSED(V) ((void) V)

/* ▶ 전방 선언: "aeEventLoop라는 구조체가 있다"고 미리 알려둠.
 *   바로 아래 콜백 타입이 aeEventLoop를 쓰는데, 정의는 더 아래에 있기 때문. */
struct aeEventLoop;

/* 타입과 자료구조 */
/* ▶ ===== 콜백(함수 포인터) 타입 4종 =====
 *   Kotlin typealias로 옮기면:
 *
 *   typealias AeFileProc = (el: AeEventLoop, fd: Int, clientData: Any?, mask: Int) -> Unit
 *       fd가 읽기/쓰기 가능해졌을 때 불림. mask = 실제 발생한 이벤트.
 *
 *   typealias AeTimeProc = (el: AeEventLoop, id: Long, clientData: Any?) -> Int
 *       타이머 시각이 됐을 때 불림. 반환값 = 다음 실행까지 ms (또는 AE_NOMORE).
 *
 *   typealias AeEventFinalizerProc = (el: AeEventLoop, clientData: Any?) -> Unit
 *       타이머가 최종 삭제될 때 불림. clientData 정리용 (Kotlin의 Closeable.close 느낌).
 *
 *   typealias AeBeforeSleepProc = (el: AeEventLoop) -> Unit
 *       epoll에서 잠들기 직전/직후에 불림. Redis의 beforeSleep/afterSleep.
 *
 *   clientData(void*) = Any?. ae는 내용을 모르고 그대로 전달만 합니다. */
typedef void aeFileProc(struct aeEventLoop *eventLoop, int fd, void *clientData, int mask);
typedef int aeTimeProc(struct aeEventLoop *eventLoop, long long id, void *clientData);
typedef void aeEventFinalizerProc(struct aeEventLoop *eventLoop, void *clientData);
typedef void aeBeforeSleepProc(struct aeEventLoop *eventLoop);

/* 파일 이벤트 구조체 */
/* ▶ ===== 파일 이벤트 = fd 하나에 대한 "등록 정보" =====
 *   events[fd] 배열의 한 칸입니다. fd 자체는 저장하지 않습니다(배열 인덱스가 곧 fd).
 *
 *   class AeFileEvent(
 *       var mask: Int,                 // 관심 있는 이벤트 (READABLE/WRITABLE/BARRIER)
 *       var rfileProc: AeFileProc?,    // 읽기 가능할 때 부를 함수
 *       var wfileProc: AeFileProc?,    // 쓰기 가능할 때 부를 함수
 *       var clientData: Any?           // 콜백에 넘길 데이터 (Redis에선 주로 connection*)
 *   )
 *
 *   읽기/쓰기 콜백이 따로 있지만 clientData는 하나뿐입니다. 즉 fd 하나는 "한 주인"의 것. */
typedef struct aeFileEvent {
    int mask; /* AE_(READABLE|WRITABLE|BARRIER) 중 하나 */
    aeFileProc *rfileProc;
    aeFileProc *wfileProc;
    void *clientData;
} aeFileEvent;

/* 시간 이벤트 구조체 */
/* ▶ ===== 시간 이벤트 = 타이머 하나 =====
 *   양방향 연결 리스트의 노드입니다(prev/next).
 *
 *   class AeTimeEvent(
 *       var id: Long,          // 식별자. 삭제 예정이면 AE_DELETED_EVENT_ID(-1)로 바뀜
 *       var `when`: Long,      // 실행할 시각 (monotonic, 마이크로초)
 *       val timeProc: AeTimeProc,
 *       val finalizerProc: AeEventFinalizerProc?,
 *       val clientData: Any?,
 *       var prev: AeTimeEvent?, var next: AeTimeEvent?,
 *       var refcount: Int      // 지금 실행 중이면 >0. 실행 중엔 해제 금지
 *   ) */
typedef struct aeTimeEvent {
    long long id; /* 시간 이벤트 식별자. */
    monotime when;
    aeTimeProc *timeProc;
    aeEventFinalizerProc *finalizerProc;
    void *clientData;
    struct aeTimeEvent *prev;
    struct aeTimeEvent *next;
    int refcount; /* 시간 이벤트 콜백이 재귀적으로 호출될 때
  		   * 타이머가 해제되지 않게 막는 참조 카운트. */
} aeTimeEvent;

/* 발생한 이벤트 */
/* ▶ ===== 발생한 이벤트 = epoll이 돌려준 "결과" 한 건 =====
 *   "fd 7번에서 READABLE이 발생했다". aeApiPoll이 fired[] 배열에 채워 넣습니다.
 *   aeFileEvent(등록부)와 헷갈리지 마세요. 이건 결과지입니다.
 *   Kotlin: data class AeFiredEvent(val fd: Int, val mask: Int) */
typedef struct aeFiredEvent {
    int fd;
    int mask;
} aeFiredEvent;

/* 이벤트 기반 프로그램의 상태 */
/* ▶ ===== 이벤트루프 본체 =====
 *   Redis 메인 스레드에 하나(server.el), I/O 스레드마다 하나씩(8.0~) 있습니다.
 *
 *   maxfd        : 현재 등록된 가장 큰 fd. 아무것도 없으면 -1.
 *                  select 백엔드가 0..maxfd를 훑을 때 씀.
 *   setsize      : 추적 가능한 fd의 상한. Redis는 maxclients + 128로 만듦.
 *   timeEventNextId : 다음 타이머에 줄 id (0, 1, 2, ...)
 *   nevents      : events/fired 배열의 "현재" 크기. 필요하면 setsize까지 커짐.
 *   events       : 등록부 배열. events[fd]
 *   fired        : 이번 바퀴 결과 배열
 *   timeEventHead: 타이머 리스트의 머리
 *   stop         : 1이 되면 aeMain 루프 종료
 *   apidata      : epoll/kqueue/select별 내부 상태. ae.c는 내용을 모름(void*)
 *                  Kotlin이면 private val api: PollApi
 *   beforesleep / aftersleep : 잠들기 전/후 훅
 *   flags        : 루프 자체의 지속 옵션. 현재는 AE_DONT_WAIT만 씀 (aeSetDontWait)
 *   privdata     : 사용자(Redis)가 아무거나 달아둘 수 있는 칸 3개 */
typedef struct aeEventLoop {
    int maxfd;   /* 현재 등록된 가장 큰 fd 번호 */
    int setsize; /* 추적할 수 있는 fd의 최대 개수 */
    long long timeEventNextId;
    int nevents; /* 등록 이벤트 배열의 크기 */
    aeFileEvent *events; /* 등록된 이벤트 */
    aeFiredEvent *fired; /* 발생한 이벤트 */
    aeTimeEvent *timeEventHead;
    int stop;
    void *apidata; /* 폴링 API(epoll 등)별 데이터를 담는 곳 */
    aeBeforeSleepProc *beforesleep;
    aeBeforeSleepProc *aftersleep;
    int flags;
    void *privdata[3];
} aeEventLoop;

/* 함수 원형 */
/* ▶ 공개 함수 목록 (Kotlin이면 class AeEventLoop의 public 메서드들).
 *   Redis 다른 파일이 실제로 쓰는 것 위주로 표시했습니다.
 *
 *   ★ aeCreateEventLoop  : 루프 생성                      (server.c initServer)
 *   ★ aeCreateFileEvent  : fd 감시 등록                   (socket.c, server.c)
 *   ★ aeDeleteFileEvent  : fd 감시 해제                   (socket.c)
 *   ★ aeCreateTimeEvent  : 타이머 등록                    (server.c: serverCron)
 *   ★ aeProcessEvents    : 루프 한 바퀴                   (aeMain, 로딩 중 processEventsWhileBlocked)
 *   ★ aeMain             : 무한 루프                      (server.c main 맨 끝)
 *   ★ aeSetBeforeSleepProc / aeSetAfterSleepProc          (server.c initServer)
 *   ★ aeSetDontWait      : 다음 바퀴에 잠들지 말 것       (server.c beforeSleep 끝)
 *     aeWait             : fd 하나를 poll()로 동기 대기   (socket.c 동기 연결 등) */
aeEventLoop *aeCreateEventLoop(int setsize);
void aeDeleteEventLoop(aeEventLoop *eventLoop);
void aeStop(aeEventLoop *eventLoop);
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
        aeFileProc *proc, void *clientData);
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask);
int aeGetFileEvents(aeEventLoop *eventLoop, int fd);
void *aeGetFileClientData(aeEventLoop *eventLoop, int fd);
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
        aeTimeProc *proc, void *clientData,
        aeEventFinalizerProc *finalizerProc);
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id);
int aeProcessEvents(aeEventLoop *eventLoop, int flags);
int aeWait(int fd, int mask, long long milliseconds);
void aeMain(aeEventLoop *eventLoop);
char *aeGetApiName(void);
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep);
void aeSetAfterSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *aftersleep);
int aeGetSetSize(aeEventLoop *eventLoop);
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize);
void aeSetDontWait(aeEventLoop *eventLoop, int noWait);

#endif
