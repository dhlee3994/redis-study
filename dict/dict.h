/* ======================================================================
 * [해설판] dict.h — Redis 해시 테이블의 자료구조와 매크로
 * 원본: https://github.com/redis/redis/blob/unstable/src/dict.h (325줄)
 * 기준 커밋: b540ca4 (2026-09-30)
 *
 * ▶ 원본 전체입니다. 코드와 영문 주석은 그대로 두고 "▶"로 시작하는 한국어 주석만 추가했습니다.
 *   이 해설 블록 아래부터가 원본 1번 줄입니다.
 *
 * ▶ 이 파일에서 꼭 볼 것 (나머지는 훑어도 됩니다)
 *   L91   dictType          : 해시 함수, 키 비교, resizeAllowed 같은 콜백 묶음
 *   L159  DICTHT_SIZE/MASK  : 테이블 크기는 항상 2의 거듭제곱 → 나머지(%) 대신 AND(&)
 *   L162  struct dict       : 테이블 2개(ht_table[2]) + rehashidx   ★ 가장 중요 ★
 *   L212  DICT_HT_INITIAL_EXP : 첫 테이블 크기 = 버킷 4개
 *   L229~ 매크로            : dictSize, dictIsRehashing, dictPauseRehashing
 *   L246  dictResizeEnable  : fork 중 resize 정책 (ENABLE / AVOID / FORBID)
 * ====================================================================== */

/* Hash Tables Implementation.
 *
 * This file implements in-memory hash tables with insert/del/replace/find/
 * get-random-element operations. Hash tables will auto-resize if needed
 * tables of power of two in size are used, collisions are handled by
 * chaining. See the source code for more information... :)
 *
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Dict usage of pointer tagging
 * -----------------------------
 * In the "normal" case (no_value=0), a dict slot contains only a pointer to a 
 * dictEntry, and dictEntry holds untagged pointers to key and value. But when a 
 * dict is used as a set (no_value=1), we optimize by storing direct key pointers 
 * when possible, avoiding dictEntry allocation. This happens when A bucket contains 
 * only one key, or at the tail of a collision chain. Redis dicts uses pointer 
 * tagging, to identify direct key pointers from dictEntry pointers, i.e embedding 
 * metadata in the lowest three bits of pointers. This requires 8-byte alignment, 
 * which zmalloc() guarantees on both 32-bit and 64-bit systems (via jemalloc/tcmalloc, 
 * or standard malloc with explicit PREFIX_SIZE=8).
 * 
 * Besides of distinguishing direct key pointers from dictEntry pointers, we also 
 * need to distinguish between even and odd key pointers that being stored in the 
 * dict. Therefore, we use the following tagging scheme:
 * - dictEntry pointer: Points to a dictEntry structure (8-byte aligned). Left intact: 
 *   ENTRY_PTR_NORMAL=000
 * - Odd-address key (keys_are_odd=1): Direct pointer to a 
 *   key with odd address (e.g., all SDS strings), Left intact: 
 *   ENTRY_PTR_IS_ODD_KEY=XX1
 * - Even-address key  (keys_are_odd=0): Direct pointer to a key with 
 *   even address. Since 8-byte alignment yields bits = 000, same as dictEntry, 
 *   we tag it by setting bit 1 which results with: 
 *   ENTRY_PTR_IS_EVEN_KEY=010.
 */

/* ▶ 위 "pointer tagging" 설명 요약 (원본 L15~L38)
 *   메인 키 공간(server.c dbDictType)은 no_value=1인 dict입니다. 값이 kvobj 안에 키와 함께
 *   들어 있어서, dict는 "kvobj 포인터" 하나만 저장하면 됩니다.
 *   - 버킷에 키가 하나뿐이면 dictEntry를 만들지 않고 버킷 칸에 kvobj 포인터를 직접 넣습니다.
 *     체인의 마지막 칸도 마찬가지입니다.
 *     → 엔트리 하나(16바이트)를 아끼고, 조회 때 따라가야 할 포인터도 하나 줄어듭니다(캐시 미스 1번 감소).
 *   - 충돌로 체인이 생기면 {next, key}만 있는 작은 엔트리(dictEntryNoValue)를 씁니다.
 *   - 칸에 든 포인터가 "엔트리"인지 "키"인지는 주소의 하위 3비트로 구분합니다.
 *     zmalloc이 8바이트 정렬을 보장해서 하위 3비트는 원래 항상 0이므로, 그 자리를 표시용으로 씁니다.
 *   평균 O(1)이 되는 "이유"와는 별개로, O(1) 안의 상수(메모리 접근 횟수)를 줄이는 장치입니다. */

#ifndef __DICT_H
#define __DICT_H

#include "mt19937-64.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>

#define DICT_OK 0
#define DICT_ERR 1

/* Hash table parameters */
#define HASHTABLE_MIN_FILL        8      /* Minimal hash table fill 12.5%(100/8) */  /* ▶ 축소 기준: 키 수 ≤ 버킷 수/8 이면 줄임 (dict.c dictShrinkIfNeeded) */

/* stored-key vs. key
 * ------------------
 * If dictType.keyFromStoredKey is non-NULL, then dict distinguishes between the
 * lookup key and the actual stored-key object. In this case, "key" is used to 
 * locate entries, while "storedKey" is the actual element stored in the dict.
 * If dictType.keyFromStoredKey is NULL, the lookup "key" and the stored-key are the
 * same. This API is primarily relevant for no_value=1 dicts, where the key and value
 * might be packed together. When values are stored separately, this identity 
 * distinction does not arise. The marker __stored_key is used to indicate that 
 * the pointer refers to the stored-key rather than the lookup key.
 */
#define __stored_key

/* ▶ dictEntry의 실제 모양은 dict.c 안에만 있습니다(opaque). 밖에서는 dictGetKey() 같은 함수로만 접근합니다.
 *   Kotlin으로 치면 private class + public getter.
 *   dictEntryLink = "dictEntry*가 들어 있는 칸의 주소". 찾은 자리에 바로 끼워 넣거나 덮어쓸 때 씁니다. */
typedef struct dictEntry dictEntry; /* opaque */
typedef struct dict dict;
typedef dictEntry **dictEntryLink; /* See description of dictFindLink() */

/* ▶ dictCmpCache: 조회 한 번 동안 "찾는 키의 길이" 같은 값을 저장해 두는 임시 공간.
 *   체인의 엔트리 여러 개와 비교할 때 매번 다시 계산하지 않게 합니다 (server.c dictSdsCompareKV가 사용). */
/* Searching for a key in a dict may involve few comparisons.
 * If extracting the looked-up key is expensive (e.g., sdslen(), kvobjGetKey()),  
 * caching can be used to reduce those repetitive computations.  
 *  
 * This struct, passed to the comparison function as temporary caching, if 
 * needed by the function across comparison of a given lookup. 
 * for the looked-up key and resets before each new lookup. */
typedef struct dictCmpCache {
    int useCache;
    
    union {
        uint64_t u64;
        int64_t i64;
        int i;
        size_t sz;
        void *p;
    } data[2];
} dictCmpCache;

/* ▶ ===== dictType: dict의 "전략 객체" =====
 *   dict.c는 키가 무엇인지 모릅니다. 해시 계산, 키 비교, 메모리 해제를 전부 이 콜백에 맡깁니다.
 *   Kotlin이라면 interface DictType { fun hash(key): Long; fun same(a, b): Boolean; ... } 정도입니다.
 *   메인 키 공간은 server.c의 dbDictType을 씁니다 (dictSdsHash, dictSdsCompareKV, dictResizeAllowed).
 *   이번 주제와 관련된 필드:
 *     hashFunction      : 해시가 고르게 퍼진다는 것이 평균 O(1)의 전제
 *     keyCompare        : 체인을 따라가며 찾는 키와 같은지 비교
 *     resizeAllowed     : 확장할 때 큰 메모리를 한 번에 잡아도 되는지 (maxmemory 검사)
 *     rehashingStarted / rehashingCompleted : kvstore가 "rehash 중인 dict 목록"을 관리하려고 꽂는 훅
 *     force_full_rehash : 켜면 rehash를 한 번에 끝냄 (server.c commandTableDictType만 사용) */
typedef struct dictType {
    /* Callbacks */
    uint64_t (*hashFunction)(const void *key);  /* ▶ 키 → 64비트 해시. 메인 키 공간은 SipHash */
    void *(*keyDup)(dict *d, const void *key __stored_key);
    void *(*valDup)(dict *d, const void *obj);
    int (*keyCompare)(dictCmpCache *cache, const void *key1, const void *key2);  /* ▶ 1이면 같은 키 */
    void (*keyDestructor)(dict *d, void *key __stored_key);
    void (*valDestructor)(dict *d, void *obj);
    int (*resizeAllowed)(size_t moreMem, double usedRatio);  /* ▶ moreMem: 새 테이블 바이트 수, usedRatio: 현재 load factor */
    /* Invoked at the start of dict initialization/rehashing (old and new ht are already created) */
    void (*rehashingStarted)(dict *d);
    /* Invoked at the end of dict initialization/rehashing of all the entries from old to new ht. Both ht still exists
     * and are cleaned up after this callback.  */
    void (*rehashingCompleted)(dict *d);
    /* Invoked when the size of the dictionary changes.
     * The `delta` parameter can be positive (size increase) or negative (size decrease). */
    void (*bucketChanged)(dict *d, long long delta);
    /* Allow a dict to carry extra caller-defined metadata. The
     * extra memory is initialized to 0 when a dict is allocated. */
    size_t (*dictMetadataBytes)(dict *d);

    /* Data */
    void *userdata;

    /* Flags */
    /* The 'no_value' flag, if set, indicates that values are not used, i.e. the
     * dict is a set. When this flag is set, it's not possible to access the
     * value of a dictEntry and it's also impossible to use dictSetKey(). It 
     * enables an optimization to store a key directly without an allocating 
     * dictEntry in between, if it is the only key in the bucket. */
    unsigned int no_value:1;  /* ▶ 메인 키 공간은 1 (값이 kvobj 안에 있음) */
    /* This flag is required for `no_value` optimization since the optimization
     * reuses LSB bits as metadata */ 
    unsigned int keys_are_odd:1;

    /* Ensures that the entire hash table is rehashed at once if set. */
    unsigned int force_full_rehash:1;  /* ▶ 메인 키 공간은 0 → 점진적 rehash */
    
    /* Callback to extract key from stored-key object. When set, the dict can
     * store keys in one format (e.g., a structure) but look them up using a
     * different format, extracted from the stored-key. (e.g., sds or integer). 
     * Set to NULL if key and stored-key object are the same. Relevant only for
     * no_value=1 dicts. */
    const void *(*keyFromStoredKey)(const void *key __stored_key);  /* ▶ 메인 키 공간: kvobj → 그 안에 박힌 sds 키 (server.c kvGetKey) */

    /* Optional callback called when the dict is destroyed. */
    void (*onDictRelease)(dict *d);

    /* ▶ prefetch 훅: 여러 명령이 한꺼번에 파싱됐을 때(파이프라인이나 I/O 스레드) 조회를 모아서 메모리를 미리 불러오는 기능
     *   (memory_prefetch.c)용입니다. 이번 주제에서는 건너뛰어도 됩니다. */
    /* Optional prefetch hooks used by the memory_prefetch state machine.
     * Both default to NULL; when both are NULL the state machine just
     * prefetches the bucket + dictEntry chain and stops there.
     *
     * prefetchEntryKey: called after a dictEntry has been brought into
     *   cache. Returns an address to issue redis_prefetch_read on (so the
     *   key payload behind the entry is warm before keyCompare runs), or
     *   NULL if nothing extra is needed (e.g. the key is co-located with
     *   the entry).
     * prefetchEntryValue: called when the entry is the *presumed* match
     *   for the lookup key — either keyCompare returned equal, or the
     *   state machine took the "last entry in chain, not rehashing"
     *   shortcut and is betting on a hit without comparing. Callbacks
     *   must therefore not assume the key has been verified equal; the
     *   prefetch is advisory. Returns an address to prefetch for the
     *   value-side payload, or NULL. */
    void *(*prefetchEntryKey)(const dictEntry *de);
    void *(*prefetchEntryValue)(const dictEntry *de);
} dictType;

/* ▶ ===== 테이블 크기는 항상 2^exp =====
 *   ht_size_exp에는 지수만 저장합니다. exp=-1이면 테이블 없음(크기 0).
 *   MASK = 크기-1 = 하위 비트가 전부 1. 예) 크기 16 → mask 0b1111
 *   그래서 버킷 번호 = hash & mask. 나눗셈(hash % size)보다 훨씬 쌉니다.
 *   대신 해시의 "하위 비트"만 쓰므로, 해시 함수가 하위 비트까지 고르게 섞어 줘야 합니다. */
#define DICTHT_SIZE(exp) ((exp) == -1 ? 0 : (unsigned long)1<<(exp))
#define DICTHT_SIZE_MASK(exp) ((exp) == -1 ? 0 : (DICTHT_SIZE(exp))-1)

/* ▶ ===== struct dict   ★ 가장 중요 ★ =====
 *
 *   평소 (rehash 안 할 때)            rehash 중
 *   ht_table[0] → [버킷 배열]         ht_table[0] → [옛 배열]  ← 앞에서부터 비워지는 중
 *   ht_table[1] = NULL                ht_table[1] → [새 배열]  ← 새 키는 여기로만 들어감
 *   rehashidx   = -1                  rehashidx   = 다음에 옮길 옛 배열 칸 번호
 *
 *   - ht_used[i]  : 테이블 i의 키 수. 평균 체인 길이(load factor) = ht_used / 크기
 *   - rehashidx   : -1이면 rehash 안 함. 0 이상이면 "옛 배열의 0 ~ rehashidx-1 칸은 이미 비었음"
 *                   (그 뒤 칸도 조회 도중 먼저 옮겨졌을 수 있습니다. dict.c _dictBucketRehash)
 *   - pauserehash : 0보다 크면 rehash 일시 정지. safe iterator가 도는 동안 키가
 *                   다른 테이블로 옮겨지면 빠뜨리거나 두 번 보게 되므로 멈춥니다.
 *                   SCAN은 커서 한 번(dictScan 호출 하나)을 처리하는 동안만 멈춥니다.
 *                   호출과 호출 사이의 rehash는 역방향 비트 커서 설계가 견딥니다.
 *   - pauseAutoResize : 0보다 크면 삽입/삭제 때 자동 확장·축소를 안 함
 *   - metadata[]  : kvstore가 붙이는 추가 정보 (rehashing 목록의 노드 등)
 *
 *   테이블이 2개뿐이라 "rehash 중에 또 resize"는 할 수 없습니다(dict.c _dictResize의 assert). */
struct dict {
    dictType *type;

    dictEntry **ht_table[2];  /* ▶ [0]=현재 테이블, [1]=rehash 목적지 */
    unsigned long ht_used[2];  /* ▶ 각 테이블의 키 수 */

    long rehashidx; /* rehashing not in progress if rehashidx == -1 */

    /* Note: pauserehash is a full unsigned so iterator increments
     * don't perform RMW on the same storage unit as other bitfields. */
    unsigned pauserehash; /* If >0 rehashing is paused */

    /* Keep small vars at end for optimal (minimal) struct padding */
    signed char ht_size_exp[2]; /* exponent of size. (size = 1<<exp) */  /* ▶ 크기 = 1<<exp. -1이면 테이블 없음 */
    int16_t pauseAutoResize;  /* If >0 automatic resizing is disallowed (<0 indicates coding error) */
    void *metadata[];
};

/* ▶ safe iterator는 첫 dictNext에서 pauserehash++, 끝날 때 -- 합니다 (dict.c dictNext / dictResetIterator).
 *   KEYS처럼 키 공간 전체를 도는 명령이 실행되는 동안 그 dict의 rehash는 멈춥니다. */
/* If safe is set to 1 this is a safe iterator, that means, you can call
 * dictAdd, dictFind, and other functions against the dictionary even while
 * iterating. Otherwise it is a non safe iterator, and only dictNext()
 * should be called while iterating. */
typedef struct dictIterator {
    dict *d;
    long index;
    int table, safe;
    dictEntry *entry, *nextEntry;
    /* unsafe iterator fingerprint for misuse detection. */
    unsigned long long fingerprint;
} dictIterator;

typedef struct dictStats {
    int htidx;
    unsigned long buckets;
    unsigned long maxChainLen;
    unsigned long totalChainLen;
    unsigned long htSize;
    unsigned long htUsed;
    unsigned long *clvector;
} dictStats;

typedef void (dictScanFunction)(void *privdata, const dictEntry *de, dictEntry **plink);
typedef void *(dictDefragAllocFunction)(void *ptr);
typedef struct {
    dictDefragAllocFunction *defragAlloc; /* Used for entries etc. */
    dictDefragAllocFunction *defragKey;   /* Defrag-realloc keys (optional) */
    dictDefragAllocFunction *defragVal;   /* Defrag-realloc values (optional) */
} dictDefragFunctions;

/* This is the initial size of every hash table */
#define DICT_HT_INITIAL_EXP      2  /* ▶ 첫 테이블은 버킷 4개 */
#define DICT_HT_INITIAL_SIZE     (1<<(DICT_HT_INITIAL_EXP))

/* ------------------------------- Macros ------------------------------------*/
#define dictFreeVal(d, entry) do {                     \
    if ((d)->type->valDestructor)                      \
        (d)->type->valDestructor((d), dictGetVal(entry)); \
   } while(0)

#define dictFreeKey(d, entry) \
    if ((d)->type->keyDestructor) \
        (d)->type->keyDestructor((d), dictGetKey(entry))

#define dictMetadata(d) (&(d)->metadata)
#define dictMetadataSize(d) ((d)->type->dictMetadataBytes \
                             ? (d)->type->dictMetadataBytes(d) : 0)

/* ▶ dictSize = 두 테이블 키 수의 합. rehash 중에는 키가 두 테이블에 나뉘어 있습니다.
 *   dictPauseRehashing / Resume은 카운터라서 중첩할 수 있습니다 (iterator 여러 개). */
#define dictBuckets(d) (DICTHT_SIZE((d)->ht_size_exp[0])+DICTHT_SIZE((d)->ht_size_exp[1]))
#define dictSize(d) ((d)->ht_used[0]+(d)->ht_used[1])
#define dictIsEmpty(d) ((d)->ht_used[0] == 0 && (d)->ht_used[1] == 0)
#define dictIsRehashing(d) ((d)->rehashidx != -1)
#define dictPauseRehashing(d) ((d)->pauserehash++)
#define dictResumeRehashing(d) ((d)->pauserehash--)
#define dictIsRehashingPaused(d) ((d)->pauserehash > 0)
#define dictPauseAutoResize(d) ((d)->pauseAutoResize++)
#define dictResumeAutoResize(d) ((d)->pauseAutoResize--)

/* If our unsigned long type can store a 64 bit number, use a 64 bit PRNG. */
#if ULONG_MAX >= 0xffffffffffffffff
#define randomULong() ((unsigned long) genrand64_int64())
#else
#define randomULong() random()
#endif

/* ▶ ===== resize 정책 (전역 하나. server.c updateDictResizePolicy가 바꿈) =====
 *   ENABLE : 평소. load factor 1에서 확장, 1/8에서 축소
 *   AVOID  : fork한 자식(BGSAVE, AOF rewrite)이 살아 있을 때. load factor 4 이상(확장)이나
 *            1/32 이하(축소)일 만큼 심할 때만 resize. rehash로 메모리 페이지를 건드리면 copy-on-write로
 *            페이지가 복사돼서 메모리가 늘어나기 때문입니다.
 *   FORBID : fork된 자식 프로세스 "안"에서. 자식은 스냅샷을 디스크에 쓰기만 하므로 resize 금지 */
typedef enum {
    DICT_RESIZE_ENABLE,
    DICT_RESIZE_AVOID,
    DICT_RESIZE_FORBID,
} dictResizeEnable;

/* API */
dict *dictCreate(dictType *type);
void dictTypeAddMeta(dict **d, dictType *typeWithMeta);
int dictExpand(dict *d, unsigned long size);
int dictTryExpand(dict *d, unsigned long size);
int dictShrink(dict *d, unsigned long size);
int dictAdd(dict *d, void *key __stored_key, void *val);
dictEntry *dictAddRaw(dict *d, void *key __stored_key, dictEntry **existing);
dictEntry *dictAddOrFind(dict *d, void *key __stored_key);
int dictReplace(dict *d, void *key __stored_key, void *val);
int dictDelete(dict *d, const void *key);
dictEntry *dictUnlink(dict *d, const void *key);
void dictFreeUnlinkedEntry(dict *d, dictEntry *he);
dictEntryLink dictTwoPhaseUnlinkFind(dict *d, const void *key, int *table_index);
void dictTwoPhaseUnlinkFree(dict *d, dictEntryLink llink, int table_index);
void dictRelease(dict *d);
dictEntry * dictFind(dict *d, const void *key);  /* ▶ 조회 */
dictEntry *dictFindByHashAndPtr(dict *d, const void *oldptr, const uint64_t hash);
int dictShrinkIfNeeded(dict *d);  /* ▶ cron(kvstoreTryResizeDicts)이 직접 부름 */
int dictExpandIfNeeded(dict *d);  /* ▶ cron(kvstoreTryResizeDicts)이 직접 부름 */
void *dictGetKey(const dictEntry *de);
int dictEntryIsKey(const dictEntry *de);
int dictCompareKeys(dict *d, const void *key1, const void *key2);
size_t dictMemUsage(const dict *d);
size_t dictEntryMemUsage(int noValueDict);
dictIterator *dictGetIterator(dict *d);
dictIterator *dictGetSafeIterator(dict *d);
void dictInitIterator(dictIterator *iter, dict *d);
void dictInitSafeIterator(dictIterator *iter, dict *d);
void dictResetIterator(dictIterator *iter);
dictEntry *dictNext(dictIterator *iter);
dictEntry *dictGetNext(const dictEntry *de);
void dictReleaseIterator(dictIterator *iter);
dictEntry *dictGetRandomKey(dict *d);
dictEntry *dictGetFairRandomKey(dict *d);
unsigned int dictGetSomeKeys(dict *d, dictEntry **des, unsigned int count);
void dictGetStats(char *buf, size_t bufsize, dict *d, int full);
uint64_t dictGenHashFunction(const void *key, size_t len);
uint64_t dictGenCaseHashFunction(const unsigned char *buf, size_t len);
void dictEmpty(dict *d, void(callback)(dict*));
void dictSetResizeEnabled(dictResizeEnable enable);  /* ▶ server.c updateDictResizePolicy가 부름 */
int dictRehash(dict *d, int n);  /* ▶ 버킷 n개 옮기기 */
int dictRehashMicroseconds(dict *d, uint64_t us);  /* ▶ 정해진 시간 동안 옮기기 (cron용) */
void dictSetHashFunctionSeed(uint8_t *seed);
unsigned long dictScan(dict *d, unsigned long v, dictScanFunction *fn, void *privdata);
unsigned long dictScanDefrag(dict *d, unsigned long v, dictScanFunction *fn, dictDefragFunctions *defragfns, void *privdata);
uint64_t dictGetHash(dict *d, const void *key);
void dictRehashingInfo(dict *d, unsigned long long *from_size, unsigned long long *to_size);

size_t dictGetStatsMsg(char *buf, size_t bufsize, dictStats *stats, int full);
dictStats* dictGetStatsHt(dict *d, int htidx, int full);
void dictCombineStats(dictStats *from, dictStats *into);
void dictFreeStats(dictStats *stats);

dictEntryLink dictFindLink(dict *d, const void *key, dictEntryLink *bucket);  /* ▶ 메인 키 공간 조회가 실제로 쓰는 함수 */
void dictSetKeyAtLink(dict *d, void *key __stored_key, dictEntryLink *link, int newItem);

/* API relevant only when dict is used as a hash-map (no_value=0) */ 
void dictSetKey(dict *d, dictEntry* de, void *key __stored_key);
void dictSetVal(dict *d, dictEntry *de, void *val);
void *dictGetVal(const dictEntry *de);
void dictSetDoubleVal(dictEntry *de, double val);
double dictGetDoubleVal(const dictEntry *de);
double *dictGetDoubleValPtr(dictEntry *de);
void *dictFetchValue(dict *d, const void *key);
void dictSetUnsignedIntegerVal(dictEntry *de, uint64_t val);
uint64_t dictIncrUnsignedIntegerVal(dictEntry *de, uint64_t val);
uint64_t dictGetUnsignedIntegerVal(const dictEntry *de);

#ifdef REDIS_TEST
int dictTest(int argc, char *argv[], int flags);
#endif

#endif /* __DICT_H */
