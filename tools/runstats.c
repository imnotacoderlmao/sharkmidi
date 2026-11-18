// runstats: how compressible are the runs of eventArr, and how predictable are their lengths?
// build (from the despair dir):
//   gcc -O2 -fopenmp -I. tools/runstats.c parse/parser.c parse/midistorage.c third_party/conmidi_digit_formatter.c -lm -o runstats
// run:  ./runstats file.mid [more.mid ...]
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>
#include <sys/wait.h>
#include "parse/midistorage.h"
#include "parse/parser.h"

// stubs for what parser.c expects from the player
int64_t playednotes = 0, notespersec = 0;
void Playback_StopAndWait(void) {}

typedef struct { uint64_t runs, events, bytes; } Cls;      // 0 note on, 1 note off, 2 other
static Cls cls[3];
static uint64_t lenhist[18];                               // run length 1,2,3,4,5-8,9-16,17-32,... (log buckets)
static uint64_t exact[17];                                 // exact lengths 1..16 (index n), [0] unused
static uint64_t deltaHist[129];                            // sorted-unique key deltas, first key counted apart
static uint64_t firstHist[128];
static uint64_t dupKeys, ascRuns, runs_ge2, bitmapWins, bitmapBytes, curBytesBig;
static uint64_t lastLenHit, lastLenTot, len1, mergeable, groups, multiRunGroups;
static uint64_t mult[3][18];                               // per class: copies of one key inside a run, 1..16, 17+
static uint64_t adjDup[3];                                 // events whose key equals the one right before it in the run
static uint64_t uniqTotal, multAll[18];
static uint64_t hdrHist[65536], prevHdrHist[65536];
#define HT (1u << 20)
static uint64_t htKey[HT], htCnt[HT];                      // (previous header << 16 | header) -> count, key stored +1
static uint32_t prevHdr;
static uint8_t cnt[128];
static void ht_add(uint64_t k)
{
    uint64_t h = (k * 0x9E3779B97F4A7C15ull) >> 44;
    for (;; h = (h + 1) & (HT - 1))
    {
        if (htKey[h] == k + 1) { htCnt[h]++; return; }
        if (!htKey[h]) { htKey[h] = k + 1; htCnt[h] = 1; return; }
    }
}
static uint32_t stamp[65536], gen;
static uint64_t bitmapSavedBytes;

static int bucket(uint64_t n) { if (n <= 4) return (int)n - 1; int b = 4; uint64_t lim = 8; while (n > lim && b < 17) { lim <<= 1; b++; } return b; }
static double H(const uint64_t* h, int n, uint64_t* tot)
{
    uint64_t t = 0; for (int i = 0; i < n; i++) t += h[i];
    *tot = t; double e = 0;
    for (int i = 0; i < n; i++) if (h[i]) { double p = (double)h[i] / t; e -= p * log2(p); }
    return e;
}

static void do_group(const uint8_t* p, const uint8_t* end, uint32_t* prevLen)
{
    gen++; groups++;
    int nr = 0;
    while (p < end)
    {
        uint32_t st = p[0], third = p[1]; p += 2;
        const uint8_t* s = p; uint64_t lo = 0, hi = 0; uint32_t n = 0, asc = 1, prev = 0;
        uint8_t b;
        int ci = ((st & 0xF0) == 0x90) ? 0 : ((st & 0xF0) == 0x80) ? 1 : 2;
        do { b = *p++; uint32_t k = b & 0x7F; if (n && k <= prev) asc = 0; if (n && k == prev) adjDup[ci]++; prev = k; cnt[k]++; if (k < 64) lo |= 1ULL << k; else hi |= 1ULL << (k - 64); n++; } while (!(b & 0x80));
        for (const uint8_t* q = s; q < p; q++) { uint32_t k = *q & 0x7F; if (cnt[k]) { uint32_t m = cnt[k]; if (m > 17) m = 17; mult[ci][m]++; multAll[m]++; uniqTotal++; cnt[k] = 0; } }
        { uint32_t hh = (st << 8) | third; hdrHist[hh]++; prevHdrHist[prevHdr]++; ht_add(((uint64_t)prevHdr << 16) | hh); prevHdr = hh; }
        int c = ((st & 0xF0) == 0x90) ? 0 : ((st & 0xF0) == 0x80) ? 1 : 2;
        cls[c].runs++; cls[c].events += n; cls[c].bytes += 2 + n;
        lenhist[bucket(n)]++; if (n <= 16) exact[n]++;
        if (n == 1) len1++;
        lastLenTot++; lastLenHit += (n == *prevLen); *prevLen = n;
        uint32_t uniq = (uint32_t)(__builtin_popcountll(lo) + __builtin_popcountll(hi));
        dupKeys += n - uniq;
        if (n >= 2) { runs_ge2++; ascRuns += asc; }
        // sorted unique deltas
        int last = -1;
        for (int w = 0; w < 2; w++)
        {
            uint64_t m = w ? hi : lo;
            while (m) { int k = __builtin_ctzll(m) + 64 * w; m &= m - 1; if (last < 0) firstHist[k]++; else deltaHist[k - last - 1]++; last = k; }
        }
        deltaHist[128] += n - uniq;                       // duplicates, delta "0 more": counted in a spare bin, cost ~ log2(1/p)
        // size if the run were a 128-bit bitmap (only when there are no duplicates and it is smaller than the bytes)
        uint64_t bytesNow = n;
        if (!(n - uniq) && n > 16) { bitmapWins++; bitmapSavedBytes += n - 16; bytesNow = 16; }
        bitmapBytes += bytesNow; curBytesBig += n;
        // mergeable with a non-adjacent earlier run of the same group
        uint32_t hdr = (st << 8) | third;
        if (stamp[hdr] == gen) mergeable++;
        stamp[hdr] = gen;
        nr++;
    }
    if (nr > 1) multiRunGroups++;
}

int main(int argc, char** argv)
{
    if (argc < 2) { puts("usage: runstats file.mid [more.mid ...]"); return 1; }
    if (argc > 2)
    {
        // several files: one run of this program per file, so each starts from a clean state and gives its memory back
        for (int i = 1; i < argc; i++)
        {
            printf("\n-- %s --\n", argv[i]); fflush(stdout);
            pid_t pid = fork();
            if (pid == 0) { execl("/proc/self/exe", argv[0], argv[i], (char*)NULL); _exit(127); }
            int status = 0; waitpid(pid, &status, 0);
            if (status != 0) printf("(failed: status %d)\n", status);
        }
        return 0;
    }
    LoadMIDI(argv[1]);
    if (!eventArr || activetickcount == 0) { puts("nothing loaded"); return 1; }
    const uint8_t* ev = eventArr;
    int64_t total = timingArr[activetickcount].event_offset;
    uint32_t prevLen = 0;
    if (trackGroups.runs && trackGroups.count > 0)
    {
        int64_t off = 0;
        for (int64_t g = 0; g < trackGroups.count; g++)
        {
            do_group(ev + off, ev + off + trackGroups.runs[g], &prevLen);
            off += trackGroups.runs[g];
        }
    }
    else { puts("(no track index: the whole file is treated as one group)"); do_group(ev, ev + total, &prevLen); }

    uint64_t runs = cls[0].runs + cls[1].runs + cls[2].runs, evs = cls[0].events + cls[1].events + cls[2].events;
    uint64_t bytes = cls[0].bytes + cls[1].bytes + cls[2].bytes;
    printf("\n== runstats: %s ==\n", argv[1]);
    printf("event bytes %.1f MB, events %lu, runs %lu (%.2f events/run), groups %lu (%.1f%% with more than one run)\n",
        bytes / 1048576.0, (unsigned long)evs, (unsigned long)runs, (double)evs / runs, (unsigned long)groups, 100.0 * multiRunGroups / groups);
    const char* nm[3] = {"note on", "note off", "other"};
    for (int c = 0; c < 3; c++)
        if (cls[c].runs) printf("  %-8s runs %12lu  events %12lu  avg len %.2f  header share of its bytes %.1f%%\n", nm[c], (unsigned long)cls[c].runs, (unsigned long)cls[c].events, (double)cls[c].events / cls[c].runs, 100.0 * 2 * cls[c].runs / cls[c].bytes);
    printf("\nrun length (events):\n  ");
    for (int n = 1; n <= 16; n++) printf("%d:%.1f%% ", n, 100.0 * exact[n] / runs);
    printf("\n  buckets 17-32 .. : "); for (int b = 6; b < 18; b++) if (lenhist[b]) printf("<=%d:%.2f%% ", 1 << (b - 1), 100.0 * lenhist[b] / runs);
    printf("\n  single-event runs: %.1f%% of runs, %.1f%% of events\n", 100.0 * len1 / runs, 100.0 * len1 / evs);
    printf("  'same length as the previous run' (what a last-value predictor would hit): %.1f%%\n", 100.0 * lastLenHit / lastLenTot);

    printf("\nkeys inside runs:\n");
    printf("  duplicate keys inside a run: %.2f%% of events\n", 100.0 * dupKeys / evs);
    printf("  runs of 2+ that are already strictly ascending: %.1f%% (%lu runs)\n", runs_ge2 ? 100.0 * ascRuns / runs_ge2 : 0.0, (unsigned long)runs_ge2);
    {
        printf("  adjacent duplicates (same key as the event right before it): note on %.1f%%, note off %.1f%% of events\n",
            cls[0].events ? 100.0 * adjDup[0] / cls[0].events : 0.0, cls[1].events ? 100.0 * adjDup[1] / cls[1].events : 0.0);
        const char* nm2[2] = {"note on ", "note off"};
        for (int c = 0; c < 2; c++)
        {
            uint64_t u = 0; for (int m = 1; m <= 17; m++) u += mult[c][m];
            printf("  copies of one key inside a run, %s: ", nm2[c]);
            for (int m = 1; m <= 6; m++) printf("x%d:%.1f%% ", m, u ? 100.0 * mult[c][m] / u : 0.0);
            uint64_t more = 0; for (int m = 7; m <= 17; m++) more += mult[c][m];
            printf("x7+:%.2f%%   (%lu distinct keys; %.1f%% of events are a 2nd+ copy)\n", u ? 100.0 * more / u : 0.0, (unsigned long)u, cls[c].events ? 100.0 * (cls[c].events - u) / cls[c].events : 0.0);
        }
    }
    uint64_t t1, t2, t3;
    double hf = H(firstHist, 128, &t1), hd = H(deltaHist, 128, &t2); (void)t3;
    double bitsFirst = (double)t1 * hf, bitsDelta = (double)t2 * hd;
    printf("  entropy of the lowest key of a run: %.2f bits (vs 7);  of the gap to the next key after sorting: %.2f bits\n", hf, hd);
    uint64_t tm; double hm = H(multAll + 1, 17, &tm);
    // headers: order 0 entropy of the 16 bit header, and conditioned on the header of the run before
    uint64_t th; double h0 = H(hdrHist, 65536, &th);
    double hpair = 0; { uint64_t tot = 0; for (uint64_t i = 0; i < HT; i++) tot += htCnt[i]; for (uint64_t i = 0; i < HT; i++) if (htCnt[i]) { double p = (double)htCnt[i] / tot; hpair -= p * log2(p); } }
    uint64_t tp; double hprev = H(prevHdrHist, 65536, &tp); double hcond = hpair - hprev;
    printf("  run headers: %.2f bits as a symbol, %.2f bits given the run before (vs 16)\n", h0, hcond);
    printf("  copies per distinct key: %.2f bits\n", hm);
    double ideal = (2.0 * runs + (bitsFirst + bitsDelta + (double)uniqTotal * hm) / 8.0);
    double idealH = ((double)runs * hcond + (bitsFirst + bitsDelta + (double)uniqTotal * hm) / 8.0 * 8.0) / 8.0;
    printf("\nestimated event bytes (headers stay 2 bytes/run, keys change):\n");
    printf("  now                              %8.1f MB\n", bytes / 1048576.0);
    printf("  hybrid: 16-byte bitmap when it is shorter (no duplicates, run > 16 keys): %8.1f MB  (%lu runs qualify)\n", (2.0 * runs + bitmapBytes + (double)(evs - curBytesBig)) / 1048576.0, (unsigned long)bitmapWins);
    printf("  sorted + distinct keys gap coded + copy counts, at the entropy limit (2-byte headers):     %8.1f MB\n", ideal / 1048576.0);
    printf("  same, with the headers coded given the run before:                                         %8.1f MB\n", idealH / 1048576.0);
    printf("  runs that could merge with an earlier run of the same group if event order within a group were free: %.1f%% (upper bound: on/off order of one key matters)\n", 100.0 * mergeable / runs);
    return 0;
}
