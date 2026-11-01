/* suites: engine selection, filters and asset-pack requirements. */
#include "suites.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const char* name;
    const char* const* packs; /* NULL-terminated */
} suite_reads_t;

static suite_reads_t reads[SUITE_READS_MAX];
static const char* waiting[SUITE_WAITING_MAX];

static int read_count;
static int waiting_count;
static const char* current;

void
suite_reads(const char* name, const char* const* packs) {
    if (read_count >= SUITE_READS_MAX) {
        suites_record_drop();
        printf("SUITE OVERFLOW: the packs '%s' reads were not registered (max %d)\n", name, SUITE_READS_MAX);
        return;
    }
    reads[read_count++] = (suite_reads_t){name, packs};
}

static bool
suite_reads_pack(const char* name, const char* pack) {
    for (int i = 0; name != NULL && i < read_count; i++) {
        if (strcmp(reads[i].name, name) != 0) {
            continue;
        }
        for (const char* const* p = reads[i].packs; *p != NULL; p++) {
            if (strcmp(*p, pack) == 0) {
                return true;
            }
        }
    }
    return false;
}

bool
suites_current_reads(const char* pack) {
    return suite_reads_pack(current, pack);
}

const char*
suites_current(void) {
    return current;
}

void
suites_wait_on(const char* pack) {
    if (waiting_count >= SUITE_WAITING_MAX) {
        suites_record_drop();
        printf("SUITE OVERFLOW: pack '%s' waits, past SUITE_WAITING_MAX %d\n", pack, SUITE_WAITING_MAX);
        return;
    }
    waiting[waiting_count++] = pack;
}

static bool
suite_waits(const char* name) {
    for (int i = 0; i < waiting_count; i++) {
        if (suite_reads_pack(name, waiting[i])) {
            return true;
        }
    }
    return false;
}

void
suites_print_waiting(void) {
    if (waiting_count == 0) {
        return;
    }
    printf("waiting on lock:");
    for (int i = 0; i < waiting_count; i++) {
        printf(" %s", waiting[i]);
    }
    printf(" (skipped suites:");
    int skipped = 0;
    for (size_t i = 0; i < suites_count(); i++) {
        const suite_entry_t* suite = suites_at(i);
        if (!suite->on_request && suite_waits(suite->name)) {
            printf(" %s", suite->name);
            skipped++;
        }
    }
    printf("%s)\n", skipped == 0 ? " none" : "");
}

void
suites_each(suite_each_fn each, void* context) {
    for (size_t i = 0; i < suites_count(); i++) {
        const suite_entry_t* suite = suites_at(i);
        each(suite->name, suite->on_request, context);
    }
}

void
suites_run_all(void) {
    for (size_t i = 0; i < suites_count(); i++) {
        const suite_entry_t* suite = suites_at(i);
        if (!suite->on_request && !suite_waits(suite->name)) {
            current = suite->name;
            suite->fn();
            current = NULL;
        }
    }
}

static char patterns[SUITE_FILTER_MAX][SUITE_FILTER_LEN];
static int pattern_count;
static int pattern_hits[SUITE_FILTER_MAX];
static int selected_count;
static bool filtering;

/* Splits "a,b,c" into patterns. A refusal prints the offender, so the host
 * can tell a refused filter from one that matched nothing. */
static bool
parse_patterns(const char* list) {
    pattern_count = 0;
    while (*list != '\0') {
        const char* comma = strchr(list, ',');
        const size_t length = comma != NULL ? (size_t)(comma - list) : strlen(list);
        if (length == 0 || length >= SUITE_FILTER_LEN || pattern_count >= SUITE_FILTER_MAX) {
            printf("SUITE_FILTER_REFUSED pattern=%.*s\n", (int)(length < SUITE_FILTER_LEN ? length : SUITE_FILTER_LEN),
                   list);
            pattern_count = 0;
            return false;
        }
        memcpy(patterns[pattern_count], list, length);
        patterns[pattern_count][length] = '\0';
        pattern_count++;
        list += length + (comma != NULL ? 1 : 0);
    }
    return true;
}

/* The length of the segment starting at `at`, up to the next separator. */
static size_t
segment_length(const char* at) {
    const char* separator = strchr(at, SUITE_TEST_SEPARATOR);
    return separator != NULL ? (size_t)(separator - at) : strlen(at);
}

static bool
span_contains(const char* span, size_t span_length, const char* part, size_t part_length) {
    for (size_t at = 0; part_length > 0 && at + part_length <= span_length; at++) {
        if (memcmp(span + at, part, part_length) == 0) {
            return true;
        }
    }
    return false;
}

/* Matches the pattern's segments greedily, each at the earliest name segment
 * it can take, which finds an in-order match whenever there is one. */
bool
suite_test_matches(const char* pattern, const char* name) {
    const size_t function_length = segment_length(name);
    size_t wanted = segment_length(pattern);
    if (span_contains(name, function_length, pattern, wanted)) {
        if (pattern[wanted] == '\0') {
            return true;
        }
        pattern += wanted + 1;
        wanted = segment_length(pattern);
    }
    for (const char* at = name + function_length; *at != '\0';) {
        at++;
        const size_t length = segment_length(at);
        if (wanted > 0 && wanted == length && memcmp(at, pattern, length) == 0) {
            if (pattern[wanted] == '\0') {
                return true;
            }
            pattern += wanted + 1;
            wanted = segment_length(pattern);
        }
        at += length;
    }
    return false;
}

bool
suites_test_runs(const char* test_name) {
    if (!filtering) {
        selected_count++;
        return true;
    }
    bool matches = false;
    for (int i = 0; i < pattern_count; i++) {
        if (suite_test_matches(patterns[i], test_name)) {
            pattern_hits[i]++;
            matches = true;
        }
    }
    printf("SUITE_TEST name=%s selected=%d\n", test_name, matches ? 1 : 0);
    selected_count += matches ? 1 : 0;
    return matches;
}

static int
unmatched_patterns(void) {
    int unmatched = 0;
    for (int i = 0; i < pattern_count; i++) {
        unmatched += pattern_hits[i] == 0;
    }
    return unmatched;
}

void
suites_run_request(const char* request, suite_run_t* run) {
    *run = (suite_run_t){0};
    const char* space = strchr(request, ' ');
    const size_t name_length = space != NULL ? (size_t)(space - request) : strlen(request);
    const char* list = space != NULL ? space + 1 : "";
    if (name_length > SUITE_NAME_MAX) {
        return;
    }
    memcpy(run->name, request, name_length);

    filtering = false;
    selected_count = 0;
    memset(pattern_hits, 0, sizeof pattern_hits);
    for (size_t i = 0; i < suites_count(); i++) {
        const suite_entry_t* suite = suites_at(i);
        if (strcmp(suite->name, run->name) != 0) {
            continue;
        }
        run->found = true;
        filtering = list[0] != '\0';
        if (filtering && !parse_patterns(list)) {
            filtering = false;
            run->refused = true;
            return;
        }
        current = suite->name;
        suite->fn();
        current = NULL;
        run->selected = selected_count;
        run->unmatched = filtering ? unmatched_patterns() : 0;
        break;
    }
    filtering = false;
    pattern_count = 0;
}

void
suites_print_run(const suite_run_t* run) {
    printf("\nRUNSUITE_COMPLETE name=%s found=%d selected=%d unmatched=%d\n", run->name, run->found ? 1 : 0,
           run->selected, run->unmatched);
}
