/*
 * game_of_life_test.c - Conway's Game of Life blinker oscillator on Nautilus/Wasm3.
 *
 * Simulates a self-contained WASI application that:
 *   1. Validates its runtime environment (args, environ)
 *   2. Establishes a nanosecond timing baseline
 *   3. Runs 3 generations of the blinker on a 5x5 grid
 *   4. Stages gen-0 through the host scratch buffer, verifies gen-2 matches
 *      (period-2 mid-check), then verifies gen-3 matches expected_gen1
 *   5. Yields between pipeline phases and checks time advanced
 *   6. Prints the starting and ending grid states to stdout
 *   7. Verifies descriptor properties and probes for preopened directories
 *
 * The blinker has period 2: even generations are horizontal, odd are vertical.
 * Running 3 steps lands on a vertical end state visually distinct from the
 * horizontal start (3 x 25 cells x 8 neighbours = 600 comparisons total).
 *
 * Start (gen 0) -- horizontal blinker:
 *   . . . . .   (row 0)
 *   . X X X .   (row 1)
 *   . . . . .   (row 2)
 *   . . . . .   (row 3)
 *   . . . . .   (row 4)
 *
 * End (gen 3) -- vertical blinker (differs visually from start):
 *   . . X . .   (row 0)
 *   . . X . .   (row 1)
 *   . . X . .   (row 2)
 *   . . . . .   (row 3)
 *   . . . . .   (row 4)
 *
 * Error code groups:
 *   -10 to -16   args / environ initialisation
 *   -20 to -23   clock_time_get / clock_res_get baseline
 *   -30 to -34   Game of Life computation + scratch round-trips
 *   -40 to -43   sched_yield + time-advance cross-check
 *   -50 to -53   fd_write ASCII-art output (start + end states only)
 *   -60 to -65   fd verification (fdstat, seek, read, prestat, close)
 *
 * Compiled with:
 *   $WASI_SDK/bin/clang --sysroot=$SYSROOT -O2 -nostdlib \
 *     -Wl,--no-entry -Wl,--export=game_of_life_test \
 *     -o game_of_life_test.wasm game_of_life_test.c
 */

#include <stdint.h>

/* ---- WASI imports ---- */

__attribute__((import_module("wasi_snapshot_preview1"), import_name("args_sizes_get")))
int wasi_args_sizes_get(uint32_t *argc, uint32_t *buf_size);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("args_get")))
int wasi_args_get(uint32_t *argv, uint8_t *buf);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("environ_sizes_get")))
int wasi_environ_sizes_get(uint32_t *count, uint32_t *buf_size);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("environ_get")))
int wasi_environ_get(uint32_t *env, uint8_t *buf);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("clock_time_get")))
int wasi_clock_time_get(uint32_t clock_id, uint64_t precision, uint64_t *time_out);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("clock_res_get")))
int wasi_clock_res_get(uint32_t clock_id, uint64_t *res_out);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("sched_yield")))
int wasi_sched_yield(void);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("fd_write")))
int wasi_fd_write(uint32_t fd, const void *iovs, uint32_t iovs_len, uint32_t *nwritten);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("fd_fdstat_get")))
int wasi_fd_fdstat_get(uint32_t fd, void *stat);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("fd_seek")))
int wasi_fd_seek(uint32_t fd, int64_t offset, uint32_t whence, uint64_t *newoffset);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("fd_read")))
int wasi_fd_read(uint32_t fd, const void *iovs, uint32_t iovs_len, uint32_t *nread);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("fd_prestat_get")))
int wasi_fd_prestat_get(uint32_t fd, void *prestat);

__attribute__((import_module("wasi_snapshot_preview1"), import_name("fd_close")))
int wasi_fd_close(uint32_t fd);

/* ---- naut custom imports ---- */

__attribute__((import_module("naut"), import_name("scratch_write")))
uint32_t naut_scratch_write(const void *ptr, uint32_t len);

__attribute__((import_module("naut"), import_name("scratch_read")))
uint32_t naut_scratch_read(void *ptr, uint32_t max);

typedef struct { uint32_t buf; uint32_t len; } iovec_t;

/* -----------------------------------------------------------------------
 * Game of Life helpers
 * ----------------------------------------------------------------------- */

#define G 5  /* grid side length; total cells = G*G = 25 */

/*
 * Advance one generation on a finite G x G grid (no wrap-around).
 * Rules: live cell survives with 2 or 3 live neighbours;
 *        dead cell is born with exactly 3 live neighbours.
 */
static void life_step(const uint8_t *cur, uint8_t *nxt)
{
    for (int r = 0; r < G; r++) {
        for (int c = 0; c < G; c++) {
            int n = 0;
            for (int dr = -1; dr <= 1; dr++) {
                for (int dc = -1; dc <= 1; dc++) {
                    if (dr == 0 && dc == 0) continue;
                    int nr = r + dr, nc = c + dc;
                    if ((unsigned)nr < G && (unsigned)nc < G)
                        n += cur[nr * G + nc];
                }
            }
            uint8_t alive = cur[r * G + c];
            nxt[r * G + c] = (alive ? (n == 2 || n == 3) : (n == 3)) ? 1 : 0;
        }
    }
}

/*
 * Write one G x G grid to stdout as ASCII art.
 * Each row is exactly 2*G bytes: cell char then space, last space -> newline.
 * Returns total bytes written or -1 on error.
 */
static int write_grid(const uint8_t *grid)
{
    char row[G * 2];
    uint32_t total = 0;
    for (int r = 0; r < G; r++) {
        for (int c = 0; c < G; c++) {
            row[c * 2]     = grid[r * G + c] ? 'X' : '.';
            row[c * 2 + 1] = (c < G - 1) ? ' ' : '\n';
        }
        iovec_t iov = { (uint32_t)(uintptr_t)row, G * 2 };
        uint32_t nw = 0;
        if (wasi_fd_write(1, &iov, 1, &nw) != 0 || nw != G * 2) return -1;
        total += nw;
    }
    return (int)total;
}

/* -----------------------------------------------------------------------
 * Entry point
 * ----------------------------------------------------------------------- */

__attribute__((visibility("default")))
__attribute__((export_name("game_of_life_test")))
int game_of_life_test(unsigned int unused)
{
    (void)unused;

    /* ---- Phase 1: runtime environment check ---- */
    {
        uint32_t argc = 0, buf_size = 0;
        if (wasi_args_sizes_get(&argc, &buf_size) != 0) return -10;
        if (argc     != 1) return -11;
        if (buf_size != 5) return -12;

        uint32_t argv[1] = {0};
        uint8_t  abuf[8] = {0};
        if (wasi_args_get(argv, abuf) != 0) return -13;
        if (abuf[0] != 'w' || abuf[1] != 'a' || abuf[2] != 's' ||
            abuf[3] != 'm' || abuf[4] != '\0') return -14;

        uint32_t env_count = 1, env_size = 1;
        if (wasi_environ_sizes_get(&env_count, &env_size) != 0) return -15;
        if (env_count != 0 || env_size != 0) return -16;
    }

    /* ---- Phase 2: timing baseline ---- */
    {
        uint64_t t = 0;
        if (wasi_clock_time_get(0, 0, &t) != 0 || t == 0) return -20;
        if (wasi_clock_time_get(1, 0, &t) != 0 || t == 0) return -21;
        uint64_t res = 0;
        if (wasi_clock_res_get(0, &res) != 0 || res != 1) return -22;
        if (wasi_clock_res_get(1, &res) != 0 || res != 1) return -23;
    }

    /* ---- Phase 3: Game of Life (3 generations) ---- */

    /*
     * gen 0 -- horizontal blinker (start, printed):
     *   . . . . .
     *   . X X X .
     *   . . . . .
     *   . . . . .
     *   . . . . .
     *
     * gen 1 -- vertical blinker (verified, not printed):
     *   . . X . .
     *   . . X . .
     *   . . X . .
     *   . . . . .
     *   . . . . .
     *
     * gen 2 -- horizontal blinker (= gen 0, verified via scratch, not printed):
     *   . . . . .
     *   . X X X .
     *   . . . . .
     *   . . . . .
     *   . . . . .
     *
     * gen 3 -- vertical blinker (= gen 1, end state, printed):
     *   . . X . .
     *   . . X . .
     *   . . X . .
     *   . . . . .
     *   . . . . .
     */
    static const uint8_t gen0[G * G] = {
        0, 0, 0, 0, 0,
        0, 1, 1, 1, 0,
        0, 0, 0, 0, 0,
        0, 0, 0, 0, 0,
        0, 0, 0, 0, 0,
    };
    static const uint8_t expected_gen1[G * G] = {
        0, 0, 1, 0, 0,
        0, 0, 1, 0, 0,
        0, 0, 1, 0, 0,
        0, 0, 0, 0, 0,
        0, 0, 0, 0, 0,
    };

    uint8_t gen1[G * G], gen2[G * G], gen3[G * G];

    {
        /* Save gen0; after two steps, verify gen2 == gen0 (period-2 mid-check). */
        if (naut_scratch_write(gen0, G * G) != G * G) return -30;

        life_step(gen0, gen1);
        for (int i = 0; i < G * G; i++)
            if (gen1[i] != expected_gen1[i]) return -31;

        life_step(gen1, gen2);

        uint8_t gen0_saved[G * G] = {0};
        if (naut_scratch_read(gen0_saved, G * G) != G * G) return -32;
        for (int i = 0; i < G * G; i++)
            if (gen2[i] != gen0_saved[i]) return -33;

        /* Third step: gen3 must equal gen1 (vertical blinker again). */
        life_step(gen2, gen3);
        for (int i = 0; i < G * G; i++)
            if (gen3[i] != expected_gen1[i]) return -34;
    }

    /* ---- Phase 4: yield between phases + time-advance check ---- */
    {
        uint64_t t1 = 0, t2 = 0;
        if (wasi_clock_time_get(1, 0, &t1) != 0) return -40;
        for (int i = 0; i < 5; i++)
            if (wasi_sched_yield() != 0) return -41;
        if (wasi_clock_time_get(1, 0, &t2) != 0) return -42;
        if (t2 <= t1) return -43;
    }

    /* ---- Phase 5: print start (gen 0) and end (gen 3) states only ---- */
    {
        static const char hdr_start[] = "Start:\n";  /* 7 bytes */
        static const char hdr_end[]   = "End:\n";    /* 5 bytes */
        uint32_t nw = 0;

        iovec_t h = { (uint32_t)(uintptr_t)hdr_start, 7 };
        if (wasi_fd_write(1, &h, 1, &nw) != 0 || nw != 7) return -50;
        if (write_grid(gen0) != G * G * 2) return -51;

        h = (iovec_t){ (uint32_t)(uintptr_t)hdr_end, 5 };
        if (wasi_fd_write(1, &h, 1, &nw) != 0 || nw != 5) return -52;
        if (write_grid(gen3) != G * G * 2) return -53;
    }

    /* ---- Phase 6: descriptor verification ---- */
    {
        uint8_t stat[24] = {0};
        if (wasi_fd_fdstat_get(1, stat) != 0) return -60;
        if (stat[0] != 2) return -61;  /* WASI_FILETYPE_CHARACTER_DEVICE */

        uint64_t newoffset = 0;
        if (wasi_fd_seek(1, 0, 0, &newoffset) != 29) return -62;  /* ESPIPE */

        iovec_t dummy = {0, 0};
        uint32_t nread = 0;
        if (wasi_fd_read(1, &dummy, 1, &nread) != 8) return -63;  /* EBADF */

        uint8_t prestat[8] = {0};
        if (wasi_fd_prestat_get(3, prestat) != 8) return -64;  /* EBADF -- no preopens */

        if (wasi_fd_close(1) != 0) return -65;
    }

    return 0;
}
