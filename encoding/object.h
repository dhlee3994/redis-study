/* ======================================================================
 * [해설판] object.h — Redis 객체(robj)와 인코딩 상수
 * 원본: https://github.com/redis/redis/blob/unstable/src/object.h (219줄)
 * 기준 커밋: b540ca4 (2026-09-30)
 *
 * ▶ 원본 전체입니다. 코드와 영문 주석은 그대로 두고 "▶"로 시작하는 한국어 주석만 추가했습니다.
 *   이 해설 블록 아래부터가 원본 1번 줄입니다.
 *
 * ▶ 이 파일이 답하는 질문: "같은 자료형의 내부 표현은 왜 달라지는가?"
 *   Redis 값은 전부 robj 하나로 감쌉니다. robj에는 "논리적 타입(type)"과 "물리적 표현(encoding)"이
 *   따로 있습니다. 사용자는 type만 보고(HSET, LPUSH ...), 내부에서는 크기와 모양에 따라 encoding을
 *   골라 씁니다. 같은 Hash라도 작으면 listpack(연속 메모리), 크면 hashtable(dict)입니다.
 *
 * ▶ 타입별로 쓰는 인코딩 (OBJECT ENCODING key 로 확인)
 *   타입      작을 때                       클 때               바뀌는 기준 (기본값)
 *   String    int / embstr                  raw                 정수인가 → 44바이트 이하인가 → 키스페이스에서는 키+값이
 *                                                               CACHE_LINE_SIZE−23 이하인가 (리눅스·x86은 64 → 41바이트,
 *                                                               Apple Silicon macOS 직접 빌드는 128 → 105바이트)
 *   List      listpack                      quicklist           listpack 하나가 8KB 초과 (list-max-listpack-size -2)
 *   Hash      listpack                      hashtable           필드 512개 초과, 또는 필드/값 64바이트 초과
 *   Set       intset / listpack             hashtable           정수만 512개, 그 외 128개, 또는 64바이트 초과
 *   ZSet      listpack                      skiplist(+dict)     128개 초과, 또는 64바이트 초과 (4주차)
 *
 * ▶ 이 파일에서 꼭 볼 것
 *   L1~L67  머리 주석     : robj와 kvobj(키를 값 객체 안에 같이 담는 형태)의 메모리 그림   ★
 *   L76     OBJ_ENCODING_*: 인코딩 번호. 3, 4, 5는 옛 인코딩이라 더는 쓰지 않음
 *   L98     refcount 23비트와 OBJ_SHARED_REFCOUNT (공유 객체)
 *   L103    struct redisObject : 16바이트 헤더                                         ★
 *   L125    kvBits        : kvobj 뒤에 붙는 1바이트
 *
 * ▶ Redis 8.0과 다른 점
 *   8.0까지는 키(sds)와 값(robj)을 따로 할당했습니다. 8.2부터 키를 값 객체 뒤에 붙여서 한 번에
 *   할당하는 kvobj가 들어왔고, 8.6부터 이 정의가 server.h에서 object.h로 분리됐습니다.
 *   그래서 "embstr은 44바이트까지"라는 오래된 규칙이 키스페이스 값에서는 키 길이에 따라 달라집니다
 *   (object_발췌.c [2-4] kvobjSet).
 * ====================================================================== */

/*
 * Redis objects overview
 *
 * robj (struct redisObject) is the fundamental in-memory container that can hold
 * values of different logical types (strings, lists, sets, hashes, sorted sets,
 * streams, modules, ...). It contains:
 *   - type: one of OBJ_STRING, OBJ_LIST, OBJ_SET, OBJ_ZSET, OBJ_HASH, OBJ_STREAM,
 *           OBJ_MODULE, ...
 *   - encoding: an implementation detail of how the value is represented in
 *           memory for the given type (see OBJ_ENCODING_* below). For example,
 *           strings may be RAW/EMBSTR/INT, sets may be INTSET or HT, etc.
 *   - lru: either LRU information (relative to the global LRU clock) or LFU data
 *           when LFU is enabled.
 *   - refcount: reference counting for object lifetime management.
 *   - ptr: a pointer to the underlying value payload (SDS, dict, quicklist, ...).
 *
 * Object encodings
 * -----------------
 * Some kinds of objects (strings, hashes, sets, zsets, lists) have multiple
 * possible in-memory encodings optimized for memory footprint and speed. The
 * 'encoding' field indicates which representation is currently used (see
 * OBJ_ENCODING_* defines below).
 *
 * kvobj (key-value object)
 * ------------------------
 * kvobj is a specific use of robj that additionally embeds the key (and optional
 * metadata) alongside the value. It is identified by the iskvobj flag in robj.
 * The distinction is mostly declarative for clarity and may be enforced later with
 * explicit casting in places. Conceptually, robj and kvobj are in a relation 
 * similar to that of a parent-child class hierarchy.
 * 
 * When iskvobj is set, it also contains:
 *   - metabits: bitmap of additional metadata attached to the object.
 *   - lru: LRU time (relative to global lru_clock) or LFU data (see robj above).
 *   - kvbits: one extra byte that follows the robj (see struct kvBits below).
 *   - embedded key: the key string is stored inline after the struct.
 *   - embedded value: for small strings, the value is stored inline after the key.
 *
 * Example layout with key and embedded value "myvalue":
 *    +--------------+----------+--------------------+----------------------+
 *    | serverObject | kvbits   | sdshdr5 "mykey" \0 | sdshdr8 "myvalue" \0 |
 *    | 16 bytes     | 1 byte   | 1      +   5   + 1 | 3    +      7    + 1 |
 *    +--------------+----------+--------------------+----------------------+
 * 
 * kvobj with metadata (+expiration)
 * ---------------------------------
 * Up to 8 metadata classes are supported, each storing one 8-byte field.
 * Class 0 is reserved for expiration time. Metadata blocks are placed before
 * the kvobj itself, in reverse class order.
 * 
 * Example of a key with expiration time (metabits=0b00000001):
 *     +--------------+--------------+----------+--------------------+
 *     | Expiry Time  | serverObject | kvbits   | sdshdr5 "mykey" \0 |
 *     | 8 byte       | 16 bytes     | 1 byte   | 1      +   5   + 1 |
 *     +--------------+--------------+----------+--------------------+
 *                    ^
 *                    +---- kvobjCreate() returns pointer here
 * 
 * Example with metadata of class1 and class3 attached (metabits=0b00001010):
 * +--------------+--------------+--------------+----------+--------------------+
 * | meta (class3)| meta (class1)| serverObject | kvbits   | sdshdr5 "mykey" \0 |
 * | 8 byte       | 8 byte       | 16 bytes     | 1 byte   | 1      +   5   + 1 |
 * +--------------+--------------+--------------+----------+--------------------+
 *                               ^
 *                               +---- kvobjCreate() returns pointer here
 * 
 */
/* ▶ 위 머리 주석 요약 (원본 L1~L66)
 *   robj  = type(4비트) + encoding(4비트) + refcount + lru + ptr. 모든 값의 공통 껍데기.
 *   kvobj = robj를 키스페이스에 넣을 때의 모양. robj 바로 뒤에 kvBits 1바이트와 키 문자열을
 *           붙여서 할당 한 번으로 끝냅니다. 작은 문자열 값은 키 뒤에 값까지 붙입니다(embstr).
 *           만료 시간 같은 메타데이터는 robj "앞쪽"에 8바이트씩 붙고, 포인터는 robj를 가리킵니다.
 *   왜 붙이나: 할당 횟수와 포인터 따라가기(캐시 미스)를 줄이기 위해서입니다.
 *           값이 int/embstr이고 버킷 충돌이 없으면 GET 한 번이 "dict 버킷 → kvobj" 두 번의
 *           메모리 접근으로 끝납니다(충돌이면 엔트리, raw 값이면 값 sds를 한 번 더 따라감).
 *           예전처럼 키 sds, robj, 값 sds가 따로 있으면 접근이 더 늘어납니다. */
#ifndef __OBJECT_H
#define __OBJECT_H

/* forward declarations */
struct client;
struct RedisModuleType;

/* ▶ 인코딩 번호는 4비트(0~15)에 들어갑니다. 15번까지 다 썼습니다.
 *   "작은 표현"과 "큰 표현"의 짝:
 *     INT/EMBSTR  ↔ RAW         (String)
 *     LISTPACK    ↔ QUICKLIST   (List)
 *     LISTPACK    ↔ HT          (Hash)
 *     INTSET/LISTPACK ↔ HT      (Set)
 *     LISTPACK    ↔ SKIPLIST    (ZSet) */
/* Object encodings (see header comment below for details). */
#define OBJ_ENCODING_RAW 0     /* Raw representation */  /* ▶ sds 포인터. 길이 제한 없음, 값 문자열을 따로 할당 */
#define OBJ_ENCODING_INT 1     /* Encoded as integer */  /* ▶ ptr 자리에 정수 값을 그대로 넣음 (할당 없음) */
#define OBJ_ENCODING_HT 2      /* Encoded as hash table */  /* ▶ dict. Hash, Set이 클 때 */
#define OBJ_ENCODING_ZIPMAP 3  /* No longer used: old hash encoding. */  /* ▶ 아래 3개는 더는 쓰지 않음. 번호만 비워 둠(재사용 안 함). 옛 RDB 형식은 rdb.h의 RDB_TYPE_*가 처리 */
#define OBJ_ENCODING_LINKEDLIST 4 /* No longer used: old list encoding. */
#define OBJ_ENCODING_ZIPLIST 5 /* No longer used: old list/hash/zset encoding. */
#define OBJ_ENCODING_INTSET 6  /* Encoded as intset */  /* ▶ 정렬된 정수 배열. Set 전용 */
#define OBJ_ENCODING_SKIPLIST 7  /* Encoded as skiplist */  /* ▶ ZSet이 클 때. 실제로는 dict + skiplist 두 개를 같이 씀 (4주차) */
#define OBJ_ENCODING_EMBSTR 8  /* Embedded sds string encoding */  /* ▶ robj(또는 kvobj)와 값 sds를 한 덩어리로 할당. 수정 불가 */
#define OBJ_ENCODING_QUICKLIST 9 /* Encoded as linked list of listpacks */  /* ▶ listpack 노드들의 이중 연결 리스트. List가 클 때 */
#define OBJ_ENCODING_STREAM 10 /* Encoded as a radix tree of listpacks */
#define OBJ_ENCODING_LISTPACK 11 /* Encoded as a listpack */  /* ▶ 연속 메모리 하나. List/Hash/Set/ZSet이 작을 때 공통으로 씀 */
#define OBJ_ENCODING_LISTPACK_EX 12 /* Encoded as listpack, extended with metadata */  /* ▶ Hash 필드 TTL(HEXPIRE)용. 이번 범위에서는 생략 */
#define OBJ_ENCODING_SLICED_ARRAY 13 /* Encoded as sliced array */
#define OBJ_ENCODING_TMPL_LP 14 /* Hash with shared template, values in listpack */  /* ▶ 8.10부터 들어온 Hash 템플릿. 자동 변환은 기본 꺼짐 */
#define OBJ_ENCODING_TMPL_ARRAY 15 /* Hash with shared template, values in sds array */

#define LRU_BITS 24
#define LRU_CLOCK_MAX ((1<<LRU_BITS)-1) /* Max value of obj->lru */
#define LRU_CLOCK_RESOLUTION 1000 /* LRU clock resolution in ms */

#define OBJ_NUM_KVMETA_BITS 8
#define OBJ_REFCOUNT_BITS 23  /* ▶ refcount는 23비트. 최대값 근처 2개는 특수값으로 예약 */
#define OBJ_SHARED_REFCOUNT ((1 << OBJ_REFCOUNT_BITS) - 1) /* Global object never destroyed. */  /* ▶ 공유 객체 (shared.integers 등). incr/decr해도 변하지 않고 해제되지 않음 */
#define OBJ_STATIC_REFCOUNT ((1 << OBJ_REFCOUNT_BITS) - 2) /* Object allocated in the stack. */
#define OBJ_FIRST_SPECIAL_REFCOUNT OBJ_STATIC_REFCOUNT

/* ▶ ★ robj 헤더는 16바이트입니다.
 *     첫 4바이트: type 4 + encoding 4 + refcount 23 + iskvobj 1 = 32비트
 *     다음 4바이트: metabits 8 + lru 24 = 32비트
 *     마지막 8바이트: ptr
 *   type과 encoding이 4비트씩이라 OBJECT ENCODING은 이 4비트를 읽어 이름으로 바꾸는 것뿐입니다
 *   (object.c strEncoding). */
struct redisObject {
    unsigned type:4;  /* ▶ OBJ_STRING, OBJ_LIST ... 사용자가 보는 타입 */
    unsigned encoding:4;  /* ▶ OBJ_ENCODING_* 실제 메모리 표현 */
    unsigned refcount : OBJ_REFCOUNT_BITS;
    unsigned iskvobj : 1;   /* 1 if this struct serves as a kvobj base */  /* ▶ 1이면 뒤에 kvBits와 키가 붙어 있음 */
    
    /* metabits and lru are Relevant only when iskvobj is set: */     
    unsigned metabits :8;  /* Bitmap of metadata (+expiry) attached to this kvobj */  /* ▶ 만료 시간 등 앞쪽에 붙은 8바이트 메타데이터가 무엇인지 비트로 표시 */
    unsigned lru:LRU_BITS; /* LRU time (relative to global lru_clock) or
                            * LFU data (least significant 8 bits frequency
                            * and most significant 16 bits access time). */
    void *ptr;  /* ▶ 실제 데이터. encoding에 따라 sds / dict / listpack / quicklist ... 를 가리킴 (INT면 정수 자체) */
};

/* robj - General purpose redis object */
typedef struct redisObject robj;  /* ▶ 같은 struct를 두 이름으로 씁니다. 키스페이스에 들어간 값은 kvobj */

/* kvobj: see header comment above for definition and memory layout. */
typedef struct redisObject kvobj;

/* Whenever an robj serves as a kvobj base (iskvobj=1), a single byte of extra
 * bits is allocated right after it, before the embedded key. */
/* ▶ kvobj에서만 robj 바로 뒤에 붙는 1바이트입니다. 키 sds 헤더 종류를 여기 적어 두어서,
 *   kvobjGetKey가 계산 없이 키 위치를 바로 찾습니다. */
typedef struct __attribute__ ((__packed__)) kvBits {
    /* SDS header type of the embedded key: SDS_TYPE_5/8/16 or 32 (values 0..3). 
     * Keys are never longer than 4GB, so SDS_TYPE_64 is not needed. */
    unsigned key_sds_type : 2;
    unsigned no_evict : 1;    /* Protected from maxmemory eviction. */
    unsigned unused : 5;       /* Free bits. Available for future use. */
} kvBits;

/* Returns the kvBits that follow the robj. Valid only if kv->iskvobj is set.
 * Note that a plain robj has no such byte allocated after it. */
static inline kvBits *kvobjBits(const kvobj *kv) {
    return (kvBits *) (void *) (kv + 1);
}

kvobj *kvobjCreate(int type, const sds key, void *ptr, uint32_t keyMetaBits);  /* ▶ 키를 품은 객체 생성 (object.c L64) */
kvobj *kvobjSet(sds key, robj *val, uint32_t keyMetaBits);  /* ▶ ★ 값 robj를 키스페이스용 kvobj로 바꿈. embstr 여부가 여기서 다시 결정됨 (object.c L290) */
kvobj *kvobjSetExpire(kvobj *kv, long long expire);
sds kvobjGetKey(const kvobj *kv);
long long kvobjGetExpire(const kvobj *val);
uint64_t *kvobjMetaRef(kvobj *kv, int metaId);

/* Redis object implementation */
void decrRefCount(robj *o);
void incrRefCount(robj *o);
robj *makeObjectShared(robj *o);
void freeStringObject(robj *o);
void freeListObject(robj *o);
void freeSetObject(robj *o);
void freeZsetObject(robj *o);
void freeHashObject(robj *o);
void dismissObject(robj *o, size_t dump_size);
robj *createObject(int type, void *ptr);
void initObjectLRUOrLFU(robj *o);
robj *createStringObject(const char *ptr, size_t len);  /* ▶ 길이 44 이하면 embstr, 아니면 raw (object.c L347) */
robj *createRawStringObject(const char *ptr, size_t len);
robj *tryCreateRawStringObject(const char *ptr, size_t len);
robj *tryCreateStringObject(const char *ptr, size_t len);
robj *dupStringObject(const robj *o);
int isSdsRepresentableAsLongLong(sds s, long long *llval);
int isObjectRepresentableAsLongLong(robj *o, long long *llongval);
robj *tryObjectEncoding(robj *o);  /* ▶ ★ 문자열을 int/embstr로 줄일 수 있으면 줄임 (object.c L1008) */
robj *tryObjectEncodingEx(robj *o, int try_trim);
size_t getObjectLength(robj *o);
robj *getDecodedObject(robj *o);
size_t stringObjectLen(robj *o);
size_t stringObjectAllocSize(const robj *o);
robj *createStringObjectFromLongLong(long long value);
robj *createStringObjectFromLongLongForValue(long long value);
robj *createStringObjectFromLongLongWithSds(long long value);
robj *createStringObjectFromLongDouble(long double value, int humanfriendly);
robj *createQuicklistObject(int fill, int compress);
robj *createListListpackObject(void);
robj *createSetObject(void);
robj *createIntsetObject(void);
robj *createSetListpackObject(void);
robj *createHashObject(void);
robj *createZsetObject(void);
robj *createZsetListpackObject(void);
robj *createStreamObject(void);
robj *createGCRAObject(long long value);
robj *createModuleObject(struct RedisModuleType *mt, void *value);
robj *createArrayObject(void);
int getLongFromObjectOrReply(struct client *c, robj *o, long *target, const char *msg);
int getPositiveLongFromObjectOrReply(struct client *c, robj *o, long *target, const char *msg);
int getRangeLongFromObjectOrReply(struct client *c, robj *o, long min, long max, long *target, const char *msg);
int checkType(struct client *c, robj *o, int type);
int getLongLongFromObjectOrReply(struct client *c, robj *o, long long *target, const char *msg);
int getDoubleFromObjectOrReply(struct client *c, robj *o, double *target, const char *msg);
int getDoubleFromObject(const robj *o, double *target);
int getLongLongFromObject(robj *o, long long *target);
int getLongLongFromGCRAObject(robj *o, long long *target);
int getLongDoubleFromObject(robj *o, long double *target);
int getLongDoubleFromObjectOrReply(struct client *c, robj *o, long double *target, const char *msg);
int getIntFromObjectOrReply(struct client *c, robj *o, int *target, const char *msg);
char *strEncoding(int encoding);  /* ▶ OBJECT ENCODING이 쓰는 이름 변환 (object.c L1311) */
int compareStringObjects(const robj *a, const robj *b);
int collateStringObjects(const robj *a, const robj *b);
int equalStringObjects(robj *a, robj *b);
void trimStringObjectIfNeeded(robj *o, int trim_small_values);
size_t kvobjAllocSize(kvobj *o);  /* ▶ MEMORY USAGE가 쓰는 크기 계산 (object.c L1362) */
size_t gcraTypeAllocSize(robj *o);
size_t gcraObjectLength(robj *o);

int objectSetLRUOrLFU(robj *val, long long lfu_freq, long long lru_idle,
                      long long lru_clock, int lru_multiplier);
void objectCommand(struct client *c);
void memoryCommand(struct client *c);

static inline void *kvobjGetAllocPtr(const kvobj *kv) {
    /* Return the base allocation pointer (start of the metadata prefix). */
    uint32_t numMetaBytes = __builtin_popcount(kv->metabits) * sizeof(uint64_t);
    return (char *)kv - numMetaBytes;
}

#endif /* __OBJECT_H */
