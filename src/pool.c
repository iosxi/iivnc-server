/* ==================================================================
 * pool.c - 符号化の作業スレッド
 *
 *  論理プロセッサの数 - 1 本(最大 15)の作業スレッドを置き、
 *  「束」(番号 0..count-1 の仕事)を取り合って片付ける。
 *  束を出した送り手のスレッドも、待つ間は自分で仕事を取って手伝う。
 *  番号ごとに終わった印を立てるので、送り手は終わった順ではなく
 *  番号順に、終わったものから送り出せる(符号化と送信が重なる)。
 *
 *  作業領域(deflate の表と JPEG の量子化表)はスレッドごとに持つ。
 *  送り手のスレッドは pool_caller_slot() で番号を借りる。
 * ================================================================== */

#include "iivnc.h"
#include "jpegenc.h"

#pragma comment(lib, "synchronization.lib")

#define MAX_THREADS 15
#define MAX_SLOTS   64

static CRITICAL_SECTION   g_cs;
static CONDITION_VARIABLE g_cv;
static PoolBatch         *g_head;
static int                g_nthreads;
static void              *g_zwork[MAX_SLOTS];
static void              *g_jpeg[MAX_SLOTS];
static volatile LONG      g_slotUsed[MAX_SLOTS];

/* 束から 1 つ取る。取れなければ -1。g_cs の中で呼ぶ */
static void unlink_locked(PoolBatch *b)
{
    PoolBatch **pp;
    for (pp = &g_head; *pp; pp = &(*pp)->link)
        if (*pp == b) { *pp = b->link; break; }
}

static int claim_locked(PoolBatch *b)
{
    int idx;
    if (b->next >= b->count) { unlink_locked(b); return -1; }
    idx = (int)b->next++;
    if (b->next >= b->count) unlink_locked(b);
    return idx;
}

static void run_item(PoolBatch *b, int idx, int slot)
{
    b->fn(b->ctx, idx, slot);
    InterlockedExchange(&b->done[idx], 1);
    WakeByAddressAll((PVOID)&b->done[idx]);
    if (InterlockedDecrement(&b->remaining) == 0) WakeByAddressAll((PVOID)&b->remaining);
}

static DWORD WINAPI worker(void *arg)
{
    int slot = (int)(INT_PTR)arg;
    for (;;) {
        PoolBatch *b;
        int idx = -1;
        EnterCriticalSection(&g_cs);
        for (;;) {
            b = g_head;
            if (b) { idx = claim_locked(b); if (idx >= 0) break; continue; }
            SleepConditionVariableCS(&g_cv, &g_cs, INFINITE);
        }
        LeaveCriticalSection(&g_cs);
        run_item(b, idx, slot);
    }
}

void pool_init(void)
{
    SYSTEM_INFO si;
    int i;
    InitializeCriticalSection(&g_cs);
    InitializeConditionVariable(&g_cv);
    GetSystemInfo(&si);
    g_nthreads = (int)si.dwNumberOfProcessors - 1;
    if (g_nthreads < 1) g_nthreads = 1;
    if (g_nthreads > MAX_THREADS) g_nthreads = MAX_THREADS;
    for (i = 0; i < g_nthreads; i++) {
        HANDLE h;
        g_slotUsed[i] = 1;
        g_zwork[i] = zd_work_new();
        g_jpeg[i]  = jpe_new();
        h = CreateThread(NULL, 0, worker, (void *)(INT_PTR)i, 0, NULL);
        if (h) CloseHandle(h);
    }
    log_printf(L"作業スレッド %d 本", g_nthreads);
}

int pool_workers(void) { return MAX_SLOTS; }

int pool_caller_slot(void)
{
    int i;
    for (i = g_nthreads; i < MAX_SLOTS; i++) {
        if (InterlockedCompareExchange(&g_slotUsed[i], 1, 0) == 0) {
            if (!g_zwork[i]) g_zwork[i] = zd_work_new();
            if (!g_jpeg[i]) g_jpeg[i] = jpe_new();
            return i;
        }
    }
    return -1;
}

void pool_release_slot(int slot)
{
    if (slot >= g_nthreads && slot < MAX_SLOTS) InterlockedExchange(&g_slotUsed[slot], 0);
}

void *pool_zwork(int slot) { return g_zwork[slot]; }
void *pool_jpeg(int slot)  { return g_jpeg[slot]; }

void pool_start(PoolBatch *b)
{
    int i;
    b->next = 0;
    b->remaining = b->count;
    b->link = NULL;
    for (i = 0; i < b->count; i++) b->done[i] = 0;
    if (!b->count) return;
    EnterCriticalSection(&g_cs);
    if (!g_head) g_head = b;
    else {
        PoolBatch *p = g_head;
        while (p->link) p = p->link;
        p->link = b;
    }
    LeaveCriticalSection(&g_cs);
    WakeAllConditionVariable(&g_cv);
}

static int claim_from(PoolBatch *b)
{
    int idx;
    if (b->next >= b->count) return -1;
    EnterCriticalSection(&g_cs);
    idx = claim_locked(b);
    LeaveCriticalSection(&g_cs);
    return idx;
}

void pool_wait_item(PoolBatch *b, int index, int slot)
{
    while (!b->done[index]) {
        int idx = slot >= 0 ? claim_from(b) : -1;
        if (idx >= 0) { run_item(b, idx, slot); continue; }
        {
            LONG zero = 0;
            WaitOnAddress((volatile VOID *)&b->done[index], &zero, sizeof(LONG), INFINITE);
        }
    }
}

void pool_finish(PoolBatch *b, int slot)
{
    for (;;) {
        LONG rem = b->remaining;
        int idx;
        if (!rem) break;
        idx = slot >= 0 ? claim_from(b) : -1;
        if (idx >= 0) { run_item(b, idx, slot); continue; }
        WaitOnAddress((volatile VOID *)&b->remaining, &rem, sizeof(LONG), INFINITE);
    }
}

void pool_run(PoolBatch *b, int slot)
{
    pool_start(b);
    pool_finish(b, slot);
}
