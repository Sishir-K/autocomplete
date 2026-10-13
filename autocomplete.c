/*
 * Live autocomplete: cache + N-gram language model + dictionary.
 *
 *   UP / DOWN  = move selection
 *   TAB        = accept selected suggestion
 *   ESC        = dismiss popup
 *   Ctrl+C in this console = quit (the model and cache are saved)
 *
 * The program keeps a small shadow copy of what you typed (with a cursor), so
 * it follows Backspace, Delete, Left and Right. Going back into an earlier
 * word brings its suggestions back.
 *
 * LOOKUP ORDER
 *   1. cache.txt     words you actually type often ("word count" per line).
 *                    Created automatically if it does not exist.
 *                    The N-gram model's predictions are searched together
 *                    with the cache (both are "learned from you").
 *   2. dictionary.txt only used for the slots the cache could not fill.
 *
 * N-GRAM MODEL (machine learning by counting)
 *   - TRAINING: count how often each word follows the previous 1 or 2 words.
 *       P(w | w2 w1) = count(w2 w1 w) / count(w2 w1 *)    (trigram)
 *       P(w | w1)    = count(w1 w)     / count(w1 *)       (bigram)
 *   - SMOOTHING: the trigram, bigram and unigram probabilities are blended
 *     (interpolation). The more often a context was seen, the more it is
 *     trusted; unseen contexts fall back to shorter ones.
 *   - The counts live in ngrams.txt and keep updating while you type.
 *
 * Options:
 *   --any          work in every app (handy for testing in Notepad)
 *   --dark         force dark popup
 *   --light        force light popup
 *   --nonext       do not predict the next word after a space
 *   --nolearn      do not learn from typing / do not write any files
 *   --order N      N-gram order: 2 = bigram, 3 = trigram (default 3)
 *   --train FILE   train the model on a text file, save it, and exit
 *   (default: follows the Windows light/dark app setting)
 *
 * Popup labels:  "tri 62%"  P(word | previous two words)
 *                "bi 45%"   P(word | previous word)
 *                "typed 12" times you typed it (cache)
 *                "0.8"      plain dictionary frequency
 */
/*
 * BUILD (this program needs user32, gdi32 and advapi32):
 *   MinGW / GCC :  gcc autocomplete.c -o autocomplete.exe -O2 -luser32 -lgdi32 -ladvapi32
 *   Visual Studio:  cl autocomplete.c      (libraries are linked automatically below)
 */
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601     /* Windows 7+: older MinGW headers need this */
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")
#endif

#define ALPHABET 26
#define MAX_WORD 100
#define KEY_SZ (2 * MAX_WORD + 2)     /* "older newer" context key */
#define MAX_SUGGESTIONS 10
#define MAX_BUF 1024

/* files (all live next to the .exe) */
#define NGRAM_FILE "ngrams.txt"
#define CACHE_FILE "cache.txt"

/* cache settings */
#define CACHE_MIN_USE 2.0f      /* a cached word is suggested once typed this often */
#define CACHE_KEEP_MIN 2.0f     /* only words typed this often are saved to disk */
#define MAX_CACHE_SAVE 2000     /* the cache file keeps the top N words */

/* N-gram settings */
#define CTX_BUCKETS 16384       /* hash table size for contexts */
#define BI_K 2.0f               /* smoothing: trust a context more as evidence grows */
#define MAX_CAND 40             /* candidates taken from the cache / dictionary */
#define MAX_BI_CAND 25          /* candidates taken per N-gram context */
#define POOL_CAP (2 * MAX_BI_CAND + 2 * MAX_CAND)
#define NEXT_MAX 5              /* suggestions shown for next-word prediction */
#define BI_MIN_COUNT 2.0f       /* next-word popup needs a pair seen at least this often */

/* sizes at 96 DPI; scaled automatically on high-DPI screens */
#define POP_W 320
#define ROW_H 40
#define FONT_PX 20

#ifndef WS_EX_NOACTIVATE
#define WS_EX_NOACTIVATE 0x08000000L
#endif

/* ===================== Theme ===================== */

typedef struct Theme {
    COLORREF bg, border, text, subtext, selBg, selText, selSub;
} Theme;

static const Theme THEME_LIGHT = {
    RGB(252, 252, 252), RGB(160, 160, 160), RGB(30, 30, 30),
    RGB(130, 130, 130), RGB(0, 120, 215), RGB(255, 255, 255), RGB(210, 230, 250)
};

static const Theme THEME_DARK = {
    RGB(32, 32, 36), RGB(75, 75, 82), RGB(236, 236, 240),
    RGB(140, 140, 150), RGB(0, 120, 215), RGB(255, 255, 255), RGB(200, 225, 250)
};

static Theme gTheme;
static int gThemeMode = 0;   /* 0 = follow Windows, 1 = light, 2 = dark */

static int systemUsesDark(void)
{
    DWORD value = 1, size = sizeof(value);
    HKEY key;

    if (RegOpenKeyExA(HKEY_CURRENT_USER,
            "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &key) == ERROR_SUCCESS) {
        RegQueryValueExA(key, "AppsUseLightTheme", NULL, NULL,
                         (LPBYTE)&value, &size);
        RegCloseKey(key);
    }
    return value == 0;
}

static void applyTheme(void)
{
    int dark = (gThemeMode == 2) || (gThemeMode == 0 && systemUsesDark());
    gTheme = dark ? THEME_DARK : THEME_LIGHT;
}

/* ===================== Trie (used by dictionary AND cache) ===================== */

typedef struct Word {
    char word[MAX_WORD];
    float frequency;
} Word;

typedef struct TrieNode {
    struct TrieNode *child[ALPHABET];
    Word *data;
    int isEnd;
} TrieNode;

static TrieNode *gRoot;                /* dictionary.txt */
static TrieNode *gCacheRoot;           /* cache.txt */
static double gTotalFreq = 0.0;        /* sum of all dictionary frequencies */
static double gCacheTotal = 0.0;       /* sum of all cache counts */
static int gCacheDirty = 0;            /* 1 = cache changed since loading */

static int isLetter(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static TrieNode *createNode(void)
{
    TrieNode *node = (TrieNode *)malloc(sizeof(TrieNode));
    if (node == NULL) {
        printf("Memory allocation failed!\n");
        exit(1);
    }
    node->data = NULL;
    node->isEnd = 0;
    for (int i = 0; i < ALPHABET; i++)
        node->child[i] = NULL;
    return node;
}

/* returns 1 if the word was added, 0 if it was skipped */
static int insert(TrieNode *root, const char *word, float frequency)
{
    if (word[0] == '\0')
        return 0;

    /* validate first, so a bad word never leaves half a path behind */
    for (int i = 0; word[i] != '\0'; i++)
        if (!isLetter(word[i]))
            return 0;

    TrieNode *current = root;

    for (int i = 0; word[i] != '\0'; i++) {
        int index = tolower((unsigned char)word[i]) - 'a';
        if (current->child[index] == NULL)
            current->child[index] = createNode();
        current = current->child[index];
    }

    current->isEnd = 1;
    if (current->data == NULL) {
        current->data = (Word *)malloc(sizeof(Word));
        if (current->data == NULL) {
            printf("Memory allocation failed!\n");
            exit(1);
        }
    }
    strcpy(current->data->word, word);
    current->data->frequency = frequency;
    return 1;
}

static void loadDictionary(TrieNode *root, const char *filename)
{
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        printf("\nERROR: Cannot open %s\n", filename);
        printf("Put dictionary.txt in the same folder as the .exe\n");
        exit(1);
    }

    char word[MAX_WORD];
    float frequency;
    long count = 0, skipped = 0;

    while (fscanf(file, "%99s %f", word, &frequency) == 2) {
        if (insert(root, word, frequency)) {
            count++;
            if (frequency > 0)
                gTotalFreq += frequency;
        } else
            skipped++;
    }
    fclose(file);

    printf("Dictionary loaded: %ld words", count);
    if (skipped > 0)
        printf(" (%ld skipped: only letters a-z are supported)", skipped);
    printf("\n");

    if (count == 0)
        printf("WARNING: dictionary is empty, there will be no suggestions.\n");
}

static TrieNode *findPrefix(TrieNode *root, const char *prefix)
{
    TrieNode *current = root;
    for (int i = 0; prefix[i] != '\0'; i++) {
        char c = (char)tolower((unsigned char)prefix[i]);
        if (c < 'a' || c > 'z')
            return NULL;
        int index = c - 'a';
        if (current->child[index] == NULL)
            return NULL;
        current = current->child[index];
    }
    return current;
}

/* the entry for exactly this word, or NULL */
static Word *findWord(TrieNode *root, const char *word)
{
    if (word[0] == '\0')
        return NULL;
    TrieNode *node = findPrefix(root, word);
    return (node != NULL && node->isEnd) ? node->data : NULL;
}

/* keep the `limit` highest-frequency words, sorted high to low */
static void addCandidate(Word *list[], int *count, int limit, Word *word)
{
    if (*count < limit) {
        list[*count] = word;
        (*count)++;
    } else {
        if (word->frequency <= list[limit - 1]->frequency)
            return;
        list[limit - 1] = word;
    }

    for (int i = *count - 1; i > 0; i--) {
        if (list[i]->frequency > list[i - 1]->frequency) {
            Word *temp = list[i];
            list[i] = list[i - 1];
            list[i - 1] = temp;
        } else
            break;
    }
}

static void collectCandidates(TrieNode *node, Word *list[], int *count, int limit)
{
    if (node == NULL)
        return;
    if (node->isEnd && node->data != NULL)
        addCandidate(list, count, limit, node->data);
    for (int i = 0; i < ALPHABET; i++)
        if (node->child[i] != NULL)
            collectCandidates(node->child[i], list, count, limit);
}

/* ===================== Small string helpers ===================== */

static char *dupStr(const char *s)
{
    char *p = (char *)malloc(strlen(s) + 1);
    if (p == NULL) {
        printf("Memory allocation failed!\n");
        exit(1);
    }
    strcpy(p, s);
    return p;
}

static void lowerCopy(char *dst, const char *src, size_t size)
{
    size_t i = 0;
    for (; src[i] != '\0' && i < size - 1; i++)
        dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = '\0';
}

/* path of a file that lives next to the .exe (out must hold MAX_PATH chars) */
static void exeFilePath(char *out, const char *name)
{
    GetModuleFileNameA(NULL, out, MAX_PATH);
    char *slash = strrchr(out, '\\');
    if (slash)
        strcpy(slash + 1, name);
    else
        strcpy(out, name);
}

/* ===================== Cache (words you type often) ===================== */

/* add `delta` to the typed-count of a (lowercase) word */
static void cacheAdd(const char *word, float delta)
{
    Word *w = findWord(gCacheRoot, word);
    if (w != NULL)
        w->frequency += delta;
    else if (!insert(gCacheRoot, word, delta))
        return;
    gCacheTotal += delta;
    gCacheDirty = 1;
}

/* load cache.txt; if the file does not exist, create an empty one */
static void loadCache(const char *filename)
{
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        file = fopen(filename, "w");
        if (file != NULL) {
            fclose(file);
            printf("Cache file not found: created empty %s (it fills as you type).\n",
                   CACHE_FILE);
        } else {
            printf("Could not create %s: the cache will only live in memory.\n",
                   CACHE_FILE);
        }
        return;
    }

    char word[MAX_WORD];
    float count;
    long loaded = 0;

    while (fscanf(file, "%99s %f", word, &count) == 2) {
        if (count > 0 && insert(gCacheRoot, word, count)) {
            gCacheTotal += count;
            loaded++;
        }
    }
    fclose(file);

    gCacheDirty = 0;
    printf("Cache loaded: %ld frequently typed words\n", loaded);
}

/* keep only the most frequently typed words; write via a temp file */
static void saveCache(const char *filename)
{
    static Word *list[MAX_CACHE_SAVE];
    int n = 0;
    collectCandidates(gCacheRoot, list, &n, MAX_CACHE_SAVE);

    char tmp[MAX_PATH + 8];
    sprintf(tmp, "%s.tmp", filename);

    FILE *file = fopen(tmp, "w");
    if (file == NULL) {
        printf("Could not save %s\n", filename);
        return;
    }

    int written = 0;
    for (int i = 0; i < n; i++) {
        if (list[i]->frequency >= CACHE_KEEP_MIN) {
            fprintf(file, "%s %.0f\n", list[i]->word, list[i]->frequency);
            written++;
        }
    }
    fclose(file);

    if (MoveFileExA(tmp, filename, MOVEFILE_REPLACE_EXISTING))
        printf("Cache saved to %s (%d words)\n", filename, written);
    else
        printf("Could not save %s\n", filename);
}

/* ===================== N-gram model ===================== */

/*
 * A "context" is the 1 or 2 words before the word being predicted:
 *      "the"  or  "in the"
 * For each context we store the words that followed it and how many times.
 * That table of counts IS the trained model.
 */

typedef struct Follower {
    char *word;
    float count;
    struct Follower *link;
} Follower;

typedef struct Ctx {
    char *key;                /* "w1" or "w2 w1" (older word first) */
    float total;              /* sum of all follower counts */
    Follower *nexts;
    struct Ctx *link;         /* hash bucket chain */
} Ctx;

typedef struct Suggestion {
    char word[MAX_WORD];
    float freq;               /* dictionary frequency (0 if not in dictionary) */
    float cacheCnt;           /* how often you typed it (cache) */
    float p3;                 /* P(word | previous two words), 0 if unknown */
    float p2;                 /* P(word | previous word), 0 if unknown */
    int tier;                 /* 0 = learned (cache / N-gram), 1 = dictionary only */
    double score;
} Suggestion;

static Ctx *gCtx[CTX_BUCKETS];
static int gNgramDirty = 0;   /* 1 = counts changed since loading */

/* options */
static int gNextWord = 1;     /* predict the next word after a space */
static int gLearn = 1;        /* learn from typing */
static int gOrder = 3;        /* 2 = bigram, 3 = trigram */

static unsigned hashKey(const char *s)
{
    unsigned h = 5381;
    while (*s)
        h = h * 33u + (unsigned char)*s++;
    return h % CTX_BUCKETS;
}

static void makeKey(char *out, const char *older, const char *newer)
{
    sprintf(out, "%s %s", older, newer);
}

/* `key` must already be lowercase */
static Ctx *ctxFind(const char *key)
{
    for (Ctx *c = gCtx[hashKey(key)]; c != NULL; c = c->link)
        if (strcmp(c->key, key) == 0)
            return c;
    return NULL;
}

static float ctxCount(const Ctx *cx, const char *word)
{
    if (cx == NULL)
        return 0.0f;
    for (const Follower *f = cx->nexts; f != NULL; f = f->link)
        if (_stricmp(f->word, word) == 0)
            return f->count;
    return 0.0f;
}

/* add `count` observations of "context -> next". Returns 1 if accepted. */
static int ctxAdd(const char *key, const char *next, float count)
{
    if (key[0] == '\0' || next[0] == '\0' || count <= 0)
        return 0;
    for (int i = 0; key[i] != '\0'; i++)
        if (!isLetter(key[i]) && key[i] != ' ')
            return 0;
    for (int i = 0; next[i] != '\0'; i++)
        if (!isLetter(next[i]))
            return 0;

    char k[KEY_SZ], n[MAX_WORD];
    lowerCopy(k, key, sizeof(k));
    lowerCopy(n, next, sizeof(n));

    Ctx *cx = ctxFind(k);
    if (cx == NULL) {
        cx = (Ctx *)malloc(sizeof(Ctx));
        if (cx == NULL) {
            printf("Memory allocation failed!\n");
            exit(1);
        }
        cx->key = dupStr(k);
        cx->total = 0;
        cx->nexts = NULL;
        unsigned h = hashKey(k);
        cx->link = gCtx[h];
        gCtx[h] = cx;
    }

    Follower *f = cx->nexts;
    while (f != NULL && strcmp(f->word, n) != 0)
        f = f->link;

    if (f == NULL) {
        f = (Follower *)malloc(sizeof(Follower));
        if (f == NULL) {
            printf("Memory allocation failed!\n");
            exit(1);
        }
        f->word = dupStr(n);
        f->count = 0;
        f->link = cx->nexts;
        cx->nexts = f;
    }

    f->count += count;
    cx->total += count;
    gNgramDirty = 1;
    return 1;
}

/*
 * ngrams.txt: one n-gram per line, last number is the count
 *      i am 60              (bigram)
 *      thank you very 12    (trigram)
 */
static void loadNgrams(const char *filename)
{
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        printf("No %s found: the N-gram model starts empty and learns as you type.\n",
               NGRAM_FILE);
        return;
    }

    char line[512];
    char t[4][MAX_WORD];
    char key[KEY_SZ];
    long loaded = 0;

    while (fgets(line, sizeof(line), file) != NULL) {
        int k = sscanf(line, "%99s %99s %99s %99s", t[0], t[1], t[2], t[3]);
        if (k == 3) {
            if (ctxAdd(t[0], t[1], (float)atof(t[2])))
                loaded++;
        } else if (k == 4) {
            makeKey(key, t[0], t[1]);
            if (ctxAdd(key, t[2], (float)atof(t[3])))
                loaded++;
        }
    }
    fclose(file);

    gNgramDirty = 0;
    printf("N-gram model loaded: %ld n-grams\n", loaded);
}

static void saveNgrams(const char *filename)
{
    char tmp[MAX_PATH + 8];
    sprintf(tmp, "%s.tmp", filename);

    FILE *file = fopen(tmp, "w");
    if (file == NULL) {
        printf("Could not save %s\n", filename);
        return;
    }

    long written = 0;
    for (int i = 0; i < CTX_BUCKETS; i++)
        for (Ctx *c = gCtx[i]; c != NULL; c = c->link)
            for (Follower *f = c->nexts; f != NULL; f = f->link) {
                fprintf(file, "%s %s %.2f\n", c->key, f->word, f->count);
                written++;
            }
    fclose(file);

    if (MoveFileExA(tmp, filename, MOVEFILE_REPLACE_EXISTING))
        printf("N-gram model saved to %s (%ld n-grams)\n", filename, written);
    else
        printf("Could not save %s\n", filename);
}

/* a word we trust: in the dictionary, or typed often enough to be cached */
static int isKnown(const char *w)
{
    if (findWord(gRoot, w) != NULL)
        return 1;
    Word *c = findWord(gCacheRoot, w);
    return c != NULL && c->frequency >= CACHE_MIN_USE;
}

/*
 * Learn one finished word typed after the words w1 (previous) and w2 (the one
 * before). nctx says how many of them exist (0, 1 or 2).
 * check = 1: skip words we don't know (typos). check = 0: trust the user.
 */
static void learnWord(const char *word, int nctx, const char *w1, const char *w2,
                      int check)
{
    if (!gLearn)
        return;

    char w[MAX_WORD];
    lowerCopy(w, word, sizeof(w));

    if (strlen(w) >= 2)
        cacheAdd(w, 1.0f);

    if (nctx >= 1 && (!check || (isKnown(w) && isKnown(w1))))
        ctxAdd(w1, w, 1.0f);

    if (gOrder >= 3 && nctx >= 2 &&
        (!check || (isKnown(w) && isKnown(w1) && isKnown(w2)))) {
        char key[KEY_SZ];
        makeKey(key, w2, w1);
        ctxAdd(key, w, 1.0f);
    }
}

/*
 * TRAINING: read a plain text file and count every word / word pair / word
 * triple in it. Only dictionary words are used. Spaces and line breaks keep
 * the context; any other punctuation (and unknown words) break it, the same
 * way it works while typing.
 */
static void trainFromFile(const char *filename)
{
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        printf("ERROR: cannot open training file %s\n", filename);
        return;
    }

    char w[MAX_WORD], h1[MAX_WORD] = "", h2[MAX_WORD] = "", key[KEY_SZ];
    int wl = 0, tooLong = 0, c;
    long used = 0, skipped = 0;

    printf("Training on %s ...\n", filename);

    do {
        c = fgetc(file);

        if (c != EOF && c < 128 && isLetter((char)c)) {
            if (wl < MAX_WORD - 1)
                w[wl++] = (char)tolower(c);
            else
                tooLong = 1;
            continue;
        }

        if (wl > 0) {                       /* a word just ended */
            w[wl] = '\0';
            if (tooLong || findWord(gRoot, w) == NULL) {
                h1[0] = h2[0] = '\0';       /* unknown word breaks the context */
                skipped++;
            } else {
                cacheAdd(w, 1.0f);
                if (h1[0] != '\0') {
                    ctxAdd(h1, w, 1.0f);
                    if (gOrder >= 3 && h2[0] != '\0') {
                        makeKey(key, h2, h1);
                        ctxAdd(key, w, 1.0f);
                    }
                }
                strcpy(h2, h1);
                strcpy(h1, w);
                used++;
            }
            wl = 0;
            tooLong = 0;
        }

        if (c != EOF && c != ' ' && c != '\t' && c != '\r' && c != '\n')
            h1[0] = h2[0] = '\0';           /* punctuation: new sentence */
    } while (c != EOF);

    fclose(file);
    printf("Training done: %ld words used, %ld skipped (not in dictionary).\n",
           used, skipped);
}

/* ---------- ranking ---------- */

static int cmpSuggestion(const void *a, const void *b)
{
    const Suggestion *x = (const Suggestion *)a;
    const Suggestion *y = (const Suggestion *)b;
    if (x->tier != y->tier)
        return x->tier - y->tier;
    double d = y->score - x->score;
    return (d > 0) - (d < 0);
}

/* returns the pool index of `word`, adding it if new (-1 if the pool is full) */
static int poolAdd(Suggestion *pool, int *n, const char *word)
{
    for (int i = 0; i < *n; i++)
        if (_stricmp(pool[i].word, word) == 0)
            return i;
    if (*n >= POOL_CAP)
        return -1;
    memset(&pool[*n], 0, sizeof(Suggestion));
    strcpy(pool[*n].word, word);
    return (*n)++;
}

/* add the best followers of a context that match the prefix */
static void gatherFollowers(const Ctx *cx, const char *prefix, size_t plen,
                            int nextMode, Suggestion *pool, int *n)
{
    if (cx == NULL)
        return;

    const Follower *best[MAX_BI_CAND];
    int nb = 0;

    for (const Follower *f = cx->nexts; f != NULL; f = f->link) {
        if (nextMode && f->count < BI_MIN_COUNT)
            continue;
        if (plen > 0 && _strnicmp(f->word, prefix, plen) != 0)
            continue;

        if (nb < MAX_BI_CAND)
            best[nb++] = f;
        else if (f->count > best[nb - 1]->count)
            best[nb - 1] = f;
        else
            continue;

        for (int i = nb - 1; i > 0 && best[i]->count > best[i - 1]->count; i--) {
            const Follower *t = best[i];
            best[i] = best[i - 1];
            best[i - 1] = t;
        }
    }

    for (int i = 0; i < nb; i++)
        poolAdd(pool, n, best[i]->word);
}

/*
 * Compute probabilities and the final score for pool[from..to).
 *
 *   P1 = dictionary probability + cache probability          (unigram)
 *   I2 = c2 * P(w | w1)    + (1 - c2) * P1                   (bigram blend)
 *   I3 = c3 * P(w | w2 w1) + (1 - c3) * I2                   (trigram blend)
 *
 * c = total / (total + BI_K): a context seen once or twice is trusted a
 * little, a context seen hundreds of times is trusted almost completely.
 */
static void fillAttrs(Suggestion *pool, int from, int to, TrieNode *root,
                      const Ctx *c2, const Ctx *c3)
{
    double conf2 = c2 ? (double)c2->total / ((double)c2->total + BI_K) : 0.0;
    double conf3 = c3 ? (double)c3->total / ((double)c3->total + BI_K) : 0.0;

    for (int i = from; i < to; i++) {
        Suggestion *s = &pool[i];

        Word *dw = findWord(root, s->word);
        Word *cw = findWord(gCacheRoot, s->word);
        s->freq = dw ? dw->frequency : 0.0f;
        s->cacheCnt = cw ? cw->frequency : 0.0f;
        s->p2 = c2 ? ctxCount(c2, s->word) / c2->total : 0.0f;
        s->p3 = c3 ? ctxCount(c3, s->word) / c3->total : 0.0f;

        double p1 = 0.0;
        if (gTotalFreq > 0)
            p1 += s->freq / gTotalFreq;
        if (gCacheTotal > 0)
            p1 += s->cacheCnt / gCacheTotal;

        double i2 = conf2 * s->p2 + (1.0 - conf2) * p1;
        s->score = conf3 * s->p3 + (1.0 - conf3) * i2;

        s->tier = (s->cacheCnt >= CACHE_MIN_USE || s->p2 > 0 || s->p3 > 0) ? 0 : 1;
    }
}

/*
 * Builds the ranked suggestion list.
 *
 *   prefix : letters typed so far (may be "" in next-word mode)
 *   w1, w2 : the previous word and the one before it; nctx says how many exist
 *
 * Search order:
 *   1. cache + N-gram followers   (what you type / what usually comes next)
 *   2. dictionary                 (only if slots are still free)
 */
static int buildSuggestions(TrieNode *root, const char *prefix,
                            const char *w1, const char *w2, int nctx,
                            int limit, int nextMode, Suggestion out[])
{
    Suggestion pool[POOL_CAP];
    int n = 0;
    size_t plen = strlen(prefix);

    const Ctx *c2 = (nctx >= 1) ? ctxFind(w1) : NULL;
    const Ctx *c3 = NULL;
    if (gOrder >= 3 && nctx >= 2) {
        char key[KEY_SZ];
        makeKey(key, w2, w1);
        c3 = ctxFind(key);
    }

    /* 1a) N-gram followers: trigram context first, then bigram */
    gatherFollowers(c3, prefix, plen, nextMode, pool, &n);
    gatherFollowers(c2, prefix, plen, nextMode, pool, &n);

    /* 1b) cache: words you typed often that start with the prefix */
    if (plen > 0) {
        TrieNode *node = findPrefix(gCacheRoot, prefix);
        if (node != NULL) {
            Word *cand[MAX_CAND];
            int nc = 0;
            collectCandidates(node, cand, &nc, MAX_CAND);
            for (int i = 0; i < nc; i++)
                if (cand[i]->frequency >= CACHE_MIN_USE)
                    poolAdd(pool, &n, cand[i]->word);
        }
    }

    fillAttrs(pool, 0, n, root, c2, c3);

    int learned = 0;
    for (int i = 0; i < n; i++)
        if (pool[i].tier == 0)
            learned++;

    /* 2) dictionary: only when the cache / model could not fill the popup */
    if (plen > 0 && learned < limit) {
        TrieNode *node = findPrefix(root, prefix);
        if (node != NULL) {
            Word *cand[MAX_CAND];
            int nc = 0;
            collectCandidates(node, cand, &nc, MAX_CAND);

            int from = n;
            for (int i = 0; i < nc; i++)
                poolAdd(pool, &n, cand[i]->word);
            fillAttrs(pool, from, n, root, c2, c3);
        }
    }

    qsort(pool, (size_t)n, sizeof(Suggestion), cmpSuggestion);

    if (n > limit)
        n = limit;
    for (int i = 0; i < n; i++)
        out[i] = pool[i];
    return n;
}

/* ===================== Global state ===================== */

static HWND gPopup;
static HHOOK gKbHook, gMouseHook;
static DWORD gMainThread;
static volatile LONG gDone = 0;   /* set once everything is saved */
static int gAnyApp = 0;
static HWND gLastFg = NULL;

/* shadow copy of recently typed text, with a cursor (0..gLen) */
static char gBuf[MAX_BUF];
static int gLen = 0;
static int gCur = 0;

/* the word that ends at the cursor (derived from the buffer) */
static char gPrefix[MAX_WORD];
static int gPrefixLen = 0;
static int gCapFirst = 0;

static Suggestion gSug[MAX_SUGGESTIONS];
static int gSugCount = 0;
static int gSel = 0;
static int gDismissed = 0;
static DWORD gSwallow = 0;

/* popup metrics (scaled for DPI in initScale) */
static int gPopW = POP_W, gRowH = ROW_H, gFontH = -FONT_PX;

/* ===================== Helpers ===================== */

static int wordIsForeground(void)
{
    if (gAnyApp)
        return 1;
    HWND fg = GetForegroundWindow();
    if (!fg)
        return 0;
    char cls[64];
    GetClassNameA(fg, cls, sizeof(cls));
    return strcmp(cls, "OpusApp") == 0;   /* Microsoft Word's window class */
}

static void notifyPopup(void)
{
    if (gPopup)
        PostMessage(gPopup, WM_APP, 0, 0);
}

/* ----- shadow buffer ----- */

/* forget everything: we no longer know where the cursor is */
static void bufReset(void)
{
    gLen = 0;
    gCur = 0;
    gPrefixLen = 0;
    gPrefix[0] = '\0';
    gSugCount = 0;
    gSel = 0;
    gDismissed = 0;
    notifyPopup();
}

static void bufInsert(char c)
{
    if (gLen >= MAX_BUF - 1) {            /* full: forget the oldest char */
        memmove(gBuf, gBuf + 1, (size_t)(gLen - 1));
        gLen--;
        if (gCur > 0)
            gCur--;
    }
    memmove(gBuf + gCur + 1, gBuf + gCur, (size_t)(gLen - gCur));
    gBuf[gCur] = c;
    gCur++;
    gLen++;
}

static void bufBackspace(void)
{
    if (gCur > 0) {
        memmove(gBuf + gCur - 1, gBuf + gCur, (size_t)(gLen - gCur));
        gCur--;
        gLen--;
    }
}

static void bufDelete(void)
{
    if (gCur < gLen) {
        memmove(gBuf + gCur, gBuf + gCur + 1, (size_t)(gLen - gCur - 1));
        gLen--;
    }
}

/*
 * The word ending at the cursor. Returns its length (0 = none).
 * Only returns a word when the cursor is at the END of it, so we never
 * "complete" while you're in the middle of a word.
 */
static int currentWord(char *out, int *capFirst)
{
    int start = gCur;

    while (start > 0 && isLetter(gBuf[start - 1]))
        start--;

    int len = gCur - start;
    if (len <= 0 || len >= MAX_WORD)
        return 0;

    if (gCur < gLen && isLetter(gBuf[gCur]))
        return 0;

    for (int i = 0; i < len; i++)
        out[i] = (char)tolower((unsigned char)gBuf[start + i]);
    out[len] = '\0';

    *capFirst = (gBuf[start] >= 'A' && gBuf[start] <= 'Z');
    return len;
}

/*
 * Finds the word just before position *pos and moves *pos to its start.
 * It only counts if separated by spaces: digits and punctuation are stored
 * as '.', which acts as a sentence break so we never pair across it.
 */
static int wordBefore(int *pos, char *out)
{
    int i = *pos;
    int sawSpace = 0;

    while (i > 0 && gBuf[i - 1] == ' ') {
        i--;
        sawSpace = 1;
    }
    if (!sawSpace)
        return 0;

    int end = i;
    while (i > 0 && isLetter(gBuf[i - 1]))
        i--;

    int len = end - i;
    if (len <= 0 || len >= MAX_WORD)
        return 0;

    for (int k = 0; k < len; k++)
        out[k] = (char)tolower((unsigned char)gBuf[i + k]);
    out[len] = '\0';
    *pos = i;
    return 1;
}

/*
 * The words before position `pos` (start of the current word):
 * w1 = the previous word, w2 = the one before that.
 * Returns how many were found (0, 1 or 2).
 */
static int contextWords(int pos, char *w1, char *w2)
{
    w1[0] = '\0';
    w2[0] = '\0';
    if (!wordBefore(&pos, w1))
        return 0;
    if (!wordBefore(&pos, w2))
        return 1;
    return 2;
}

/*
 * Called when a word has just been finished by typing: put it in the cache
 * and learn "previous words -> this word". Unknown words (typos) are not
 * added to the N-gram model.
 */
static void learnWordAtCursor(void)
{
    if (!gLearn || gCur != gLen)
        return;

    char w[MAX_WORD], w1[MAX_WORD], w2[MAX_WORD];
    int cf = 0;
    int len = currentWord(w, &cf);
    if (len <= 0)
        return;

    int nctx = contextWords(gCur - len, w1, w2);
    learnWord(w, nctx, w1, w2, 1);
}

static void refreshSuggestions(void)
{
    char w[MAX_WORD];
    int cf = 0;
    int len = currentWord(w, &cf);

    gPrefixLen = len;
    gCapFirst = cf;
    if (len > 0)
        strcpy(gPrefix, w);
    else
        gPrefix[0] = '\0';

    /* just after a space (not inside a word): predict the next word */
    int nextMode = 0;
    if (len == 0 && gNextWord && gCur > 0 && gBuf[gCur - 1] == ' ' &&
        !(gCur < gLen && isLetter(gBuf[gCur])))
        nextMode = 1;

    gSugCount = 0;
    if (!gDismissed && (len > 0 || nextMode)) {
        char w1[MAX_WORD], w2[MAX_WORD];
        int nctx = contextWords(gCur - len, w1, w2);

        gSugCount = buildSuggestions(gRoot, gPrefix, w1, w2, nctx,
                                     nextMode ? NEXT_MAX : MAX_SUGGESTIONS,
                                     nextMode, gSug);

        /* nothing useful to offer if the only match is exactly what's typed */
        if (gSugCount == 1 && _stricmp(gSug[0].word, gPrefix) == 0)
            gSugCount = 0;
    }
    gSel = 0;
    notifyPopup();
}

/* Erase `eraseCount` letters in the app, then type the word (+ a space) */
static void typeCompletion(const char *word, int eraseCount, int addSpace)
{
    size_t wl = strlen(word);
    size_t cap = ((size_t)eraseCount + wl + 1) * 2;
    INPUT *in = (INPUT *)calloc(cap, sizeof(INPUT));
    if (!in)
        return;

    size_t n = 0;

    for (int i = 0; i < eraseCount; i++) {
        in[n].type = INPUT_KEYBOARD;
        in[n].ki.wVk = VK_BACK;
        n++;
        in[n].type = INPUT_KEYBOARD;
        in[n].ki.wVk = VK_BACK;
        in[n].ki.dwFlags = KEYEVENTF_KEYUP;
        n++;
    }

    size_t total = wl + (addSpace ? 1 : 0);
    for (size_t i = 0; i < total; i++) {
        char ch = (i < wl) ? word[i] : ' ';

        in[n].type = INPUT_KEYBOARD;
        in[n].ki.wScan = (WORD)(unsigned char)ch;
        in[n].ki.dwFlags = KEYEVENTF_UNICODE;
        n++;
        in[n].type = INPUT_KEYBOARD;
        in[n].ki.wScan = (WORD)(unsigned char)ch;
        in[n].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        n++;
    }

    SendInput((UINT)n, in, sizeof(INPUT));
    free(in);
}

static void acceptSuggestion(void)
{
    char word[MAX_WORD];
    strcpy(word, gSug[gSel].word);

    /* learn what you chose (before the buffer changes) */
    if (gLearn) {
        char w1[MAX_WORD], w2[MAX_WORD];
        int nctx = contextWords(gCur - gPrefixLen, w1, w2);
        learnWord(word, nctx, w1, w2, 0);
    }

    if (gCapFirst)
        word[0] = (char)toupper((unsigned char)word[0]);

    /* only add a space when we're at the end of the text, not mid-sentence */
    int addSpace = (gCur == gLen);
    int erase = gPrefixLen;

    typeCompletion(word, erase, addSpace);

    /* keep the shadow buffer in sync with what we just typed */
    for (int i = 0; i < erase; i++)
        bufBackspace();
    for (const char *p = word; *p; p++)
        bufInsert(*p);
    if (addSpace)
        bufInsert(' ');

    refreshSuggestions();
}

/* ===================== Keyboard / mouse hooks ===================== */

/* keys that type exactly one visible character that is not a letter */
static int isCharKey(DWORD vk)
{
    return vk == VK_SPACE ||
           (vk >= '0' && vk <= '9') ||
           (vk >= 0x60 && vk <= 0x6F) ||     /* numpad digits and operators */
           (vk >= 0xBA && vk <= 0xC0) ||     /* ; = , - . / ` */
           (vk >= 0xDB && vk <= 0xDF) ||     /* [ \ ] ' etc. */
           vk == 0xE2;                       /* <> key on some layouts */
}

/* returns 1 if the key must be swallowed (not delivered to Word) */
static int handleKeyDown(DWORD vk)
{
    int ctrl  = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    int alt   = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    int shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    int caps  = (GetKeyState(VK_CAPITAL) & 1) != 0;

    /* modifier keys alone don't change anything */
    switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_CAPITAL:
        return 0;
    }

    /* any key except ESC re-enables a popup you dismissed earlier */
    if (vk != VK_ESCAPE)
        gDismissed = 0;

    /* popup is open: navigation keys */
    if (gSugCount > 0 && !ctrl && !alt) {
        if (vk == VK_TAB && !shift) {
            acceptSuggestion();
            return 1;
        }
        if (vk == VK_DOWN) {
            gSel = (gSel + 1) % gSugCount;
            notifyPopup();
            return 1;
        }
        if (vk == VK_UP) {
            gSel = (gSel - 1 + gSugCount) % gSugCount;
            notifyPopup();
            return 1;
        }
        if (vk == VK_ESCAPE) {
            gDismissed = 1;      /* hide until the word changes */
            gSugCount = 0;
            notifyPopup();
            return 1;
        }
    }

    /* shortcuts (paste, undo, select all, Alt codes...) change text we can't see */
    if (ctrl || alt) {
        bufReset();
        return 0;
    }

    /* letters */
    if (vk >= 'A' && vk <= 'Z') {
        char c = (char)vk;                 /* vk is the uppercase letter */
        if (shift == caps)
            c = (char)tolower((unsigned char)c);
        bufInsert(c);
        refreshSuggestions();
        return 0;
    }

    /* space, digits, punctuation: end the word but keep tracking */
    if (isCharKey(vk)) {
        learnWordAtCursor();                     /* the word just finished */
        bufInsert(vk == VK_SPACE ? ' ' : '.');   /* any non-letter works */
        refreshSuggestions();
        return 0;
    }

    if (vk == VK_BACK) {
        bufBackspace();
        refreshSuggestions();
        return 0;
    }

    if (vk == VK_DELETE) {
        bufDelete();
        refreshSuggestions();
        return 0;
    }

    if (vk == VK_LEFT || vk == VK_RIGHT) {
        if (shift) {                         /* selecting text */
            bufReset();
        } else if (vk == VK_LEFT && gCur > 0) {
            gCur--;
            refreshSuggestions();
        } else if (vk == VK_RIGHT && gCur < gLen) {
            gCur++;
            refreshSuggestions();
        } else {
            bufReset();                      /* moved past what we know */
        }
        return 0;
    }

    /* Enter finishes a word too */
    if (vk == VK_RETURN)
        learnWordAtCursor();

    /* Enter, Tab, Home, End, Up/Down, Esc, F-keys...: position now unknown */
    bufReset();
    return 0;
}

static LRESULT CALLBACK kbProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lParam;

        if (!(k->flags & LLKHF_INJECTED)) {   /* ignore our own SendInput */
            DWORD vk = k->vkCode;
            int isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);

            if (!isDown) {
                if (gSwallow != 0 && gSwallow == vk) {
                    gSwallow = 0;
                    return 1;
                }
            } else {
                /* switched windows since the last key: drop old context */
                HWND fg = GetForegroundWindow();
                if (fg != gLastFg) {
                    gLastFg = fg;
                    bufReset();
                }

                if (wordIsForeground() && handleKeyDown(vk)) {
                    gSwallow = vk;
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(NULL, code, wParam, lParam);
}

static LRESULT CALLBACK mouseProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION &&
        (wParam == WM_LBUTTONDOWN || wParam == WM_RBUTTONDOWN ||
         wParam == WM_MBUTTONDOWN))
        bufReset();   /* cursor moved, we no longer know the current word */
    return CallNextHookEx(NULL, code, wParam, lParam);
}

/* ===================== Popup window ===================== */

static void initScale(void)
{
    HDC dc = GetDC(NULL);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc)
        ReleaseDC(NULL, dc);
    if (dpi < 96)
        dpi = 96;

    gPopW = MulDiv(POP_W, dpi, 96);
    gRowH = MulDiv(ROW_H, dpi, 96);
    gFontH = -MulDiv(FONT_PX, dpi, 96);
}

/* pt = bottom-left of the text cursor, lineH = its height */
static void getAnchor(POINT *pt, int *lineH)
{
    GUITHREADINFO gi;
    ZeroMemory(&gi, sizeof(gi));
    gi.cbSize = sizeof(gi);

    HWND fg = GetForegroundWindow();
    DWORD tid = fg ? GetWindowThreadProcessId(fg, NULL) : 0;

    if (tid && GetGUIThreadInfo(tid, &gi) && gi.hwndCaret &&
        gi.rcCaret.bottom > gi.rcCaret.top) {
        *lineH = gi.rcCaret.bottom - gi.rcCaret.top;
        pt->x = gi.rcCaret.left;
        pt->y = gi.rcCaret.bottom;
        ClientToScreen(gi.hwndCaret, pt);
        pt->y += 4;
        return;
    }

    /* Word doesn't always expose its caret: fall back to the mouse position */
    GetCursorPos(pt);
    *lineH = 20;
    pt->x += 12;
    pt->y += 20;
}

static void updatePopup(HWND h)
{
    if (gSugCount <= 0) {
        ShowWindow(h, SW_HIDE);
        return;
    }

    int height = gRowH * gSugCount + 4;
    POINT pt;
    int lineH;
    getAnchor(&pt, &lineH);

    /* use the monitor the text is on, not just the primary screen */
    RECT wa = { 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    MONITORINFO mi;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfo(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi))
        wa = mi.rcWork;

    if (pt.y + height > wa.bottom)           /* no room below: flip above the line */
        pt.y = pt.y - lineH - 8 - height;
    if (pt.y < wa.top)
        pt.y = wa.top;
    if (pt.x + gPopW > wa.right)
        pt.x = wa.right - gPopW;
    if (pt.x < wa.left)
        pt.x = wa.left;

    SetWindowPos(h, HWND_TOPMOST, pt.x, pt.y, gPopW, height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(h, NULL, TRUE);
}

static void paintPopup(HWND h)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);

    RECT rc;
    GetClientRect(h, &rc);

    HBRUSH bg = CreateSolidBrush(gTheme.bg);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);

    HFONT font = CreateFontA(gFontH, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);

    for (int i = 0; i < gSugCount; i++) {
        RECT r = { 2, 2 + i * gRowH, rc.right - 2, 2 + (i + 1) * gRowH };

        if (i == gSel) {
            HBRUSH sel = CreateSolidBrush(gTheme.selBg);
            FillRect(dc, &r, sel);
            DeleteObject(sel);
            SetTextColor(dc, gTheme.selText);
        } else {
            SetTextColor(dc, gTheme.text);
        }

        RECT tr = r;
        tr.left += 8;
        DrawTextA(dc, gSug[i].word, -1, &tr,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT);

        SetTextColor(dc, i == gSel ? gTheme.selSub : gTheme.subtext);

        /* show the strongest evidence for this word */
        char label[32];
        if (gSug[i].p3 > 0)
            sprintf(label, "tri %.0f%%", gSug[i].p3 * 100.0f);
        else if (gSug[i].p2 > 0)
            sprintf(label, "bi %.0f%%", gSug[i].p2 * 100.0f);
        else if (gSug[i].cacheCnt >= 1)
            sprintf(label, "typed %.0f", gSug[i].cacheCnt);
        else
            sprintf(label, "%.1f", gSug[i].freq);

        tr = r;
        tr.right -= 8;
        DrawTextA(dc, label, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
    }

    SelectObject(dc, oldFont);
    DeleteObject(font);

    HBRUSH border = CreateSolidBrush(gTheme.border);
    FrameRect(dc, &rc, border);
    DeleteObject(border);

    EndPaint(h, &ps);
}

static LRESULT CALLBACK popupProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_APP:
        updatePopup(h);
        return 0;
    case WM_PAINT:
        paintPopup(h);
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_SETTINGCHANGE:   /* user switched Windows light/dark */
        if (gThemeMode == 0) {
            applyTheme();
            InvalidateRect(h, NULL, TRUE);
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static HWND createPopup(HINSTANCE inst)
{
    WNDCLASSEXA wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = popupProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "CAutoCompletePopup";
    RegisterClassExA(&wc);

    return CreateWindowExA(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           "CAutoCompletePopup", "", WS_POPUP,
                           0, 0, gPopW, gRowH, NULL, NULL, inst, NULL);
}

/* ===================== main ===================== */

/*
 * Ctrl+C, closing the console window, logoff and shutdown all land here.
 * We ask the main thread to quit and, for the "closing" events, wait a few
 * seconds so it can save the cache and the model before Windows ends us.
 */
static BOOL WINAPI ctrlHandler(DWORD type)
{
    PostThreadMessage(gMainThread, WM_QUIT, 0, 0);

    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT ||
        type == CTRL_SHUTDOWN_EVENT) {
        for (int i = 0; i < 80 && !gDone; i++)
            Sleep(50);
    }
    return TRUE;
}

int main(int argc, char **argv)
{
    /* without this, Windows blurs the popup and misplaces it on scaled displays */
    SetProcessDPIAware();
    initScale();

    const char *trainFile = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--any") == 0)
            gAnyApp = 1;
        else if (strcmp(argv[i], "--dark") == 0)
            gThemeMode = 2;
        else if (strcmp(argv[i], "--light") == 0)
            gThemeMode = 1;
        else if (strcmp(argv[i], "--nonext") == 0)
            gNextWord = 0;
        else if (strcmp(argv[i], "--nolearn") == 0)
            gLearn = 0;
        else if (strcmp(argv[i], "--order") == 0 && i + 1 < argc) {
            gOrder = atoi(argv[++i]);
            if (gOrder < 2) gOrder = 2;
            if (gOrder > 3) gOrder = 3;
        } else if (strcmp(argv[i], "--train") == 0 && i + 1 < argc)
            trainFile = argv[++i];
    }

    applyTheme();

    /* dictionary.txt, ngrams.txt and cache.txt live next to the .exe */
    char dictPath[MAX_PATH], ngramPath[MAX_PATH], cachePath[MAX_PATH];
    exeFilePath(dictPath, "dictionary.txt");
    exeFilePath(ngramPath, NGRAM_FILE);
    exeFilePath(cachePath, CACHE_FILE);

    gRoot = createNode();
    gCacheRoot = createNode();

    printf("Loading dictionary...\n");
    loadDictionary(gRoot, dictPath);
    loadNgrams(ngramPath);
    loadCache(cachePath);          /* creates cache.txt if it is missing */

    /* training mode: learn from a text file, save, and exit */
    if (trainFile != NULL) {
        trainFromFile(trainFile);
        saveNgrams(ngramPath);
        saveCache(cachePath);
        return 0;
    }

    HINSTANCE inst = GetModuleHandle(NULL);
    gPopup = createPopup(inst);
    if (!gPopup) {
        printf("Could not create popup window.\n");
        return 1;
    }

    gMainThread = GetCurrentThreadId();
    SetConsoleCtrlHandler(ctrlHandler, TRUE);

    gKbHook = SetWindowsHookExA(WH_KEYBOARD_LL, kbProc, inst, 0);
    gMouseHook = SetWindowsHookExA(WH_MOUSE_LL, mouseProc, inst, 0);
    if (!gKbHook || !gMouseHook) {
        printf("Could not install keyboard hook (error %lu).\n", GetLastError());
        return 1;
    }

    printf("\nAutocomplete is running%s.\n",
           gAnyApp ? " in ALL apps" : " in Microsoft Word");
    printf("Open Word and start typing.\n");
    printf("  UP/DOWN = select   TAB = complete   ESC = dismiss\n");
    printf("  N-gram order: %d   Next-word prediction: %s   Learning: %s\n",
           gOrder, gNextWord ? "on" : "off", gLearn ? "on" : "off");
    printf("  Press Ctrl+C here to quit.\n");

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnhookWindowsHookEx(gKbHook);
    UnhookWindowsHookEx(gMouseHook);
    DestroyWindow(gPopup);

    if (gLearn) {
        if (gNgramDirty)
            saveNgrams(ngramPath);
        if (gCacheDirty)
            saveCache(cachePath);
    }
    InterlockedExchange(&gDone, 1);
    return 0;
}