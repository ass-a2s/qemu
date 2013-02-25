/*
 * QEMU backup test suit
 *
 * Copyright (C) 2013 Proxmox Server Solutions
 *
 * Authors:
 *  Dietmar Maurer (dietmar@proxmox.com)
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 *
 */

#include <sys/time.h>
#include <sys/types.h>
#include <stdarg.h>
#include <stdio.h>
#include <getopt.h>
#include <libgen.h>

#include "qemu-common.h"
#include "block/block.h"

#include "vma.h"

static int opt_debug;
static int opt_loop;

#define DPRINTF(fmt, ...) \
    do { if (opt_debug) { printf(fmt, ## __VA_ARGS__); } } while (0)

#define CLUSTER(x) (x*BACKUP_CLUSTER_SIZE)

#define RUN_TEST(testfunc, speed) \
    backup_test(#testfunc " speed " #speed, speed, testfunc);


static unsigned char buf_sec_pattern_cd[BDRV_SECTOR_SIZE];
static unsigned char buf_sec_pattern_32[BDRV_SECTOR_SIZE];

#define TEST_IMG_SIZE (6*1024*1024+BDRV_SECTOR_SIZE)
#define TEST_IMG_NAME "backuptest.raw"
#define TEST_IMG_RESTORE_NAME "backuptest.raw.restore"
#define TEST_VMA_NAME "backuptest.vma"

typedef struct BackupCB {
    VmaWriter *vmaw;
    uint8_t dev_id;
} BackupCB;

static int backup_dump_cb(void *opaque, BlockDriverState *bs,
                          int64_t cluster_num, unsigned char *buf)
{
    BackupCB *bcb = opaque;

    DPRINTF("backup_dump_cb C%" PRId64 " %d\n", cluster_num, bcb->dev_id);

    size_t zb = 0;
    if (vma_writer_write(bcb->vmaw, bcb->dev_id, cluster_num, buf, &zb) < 0) {
        printf("backup_dump_cb vma_writer_write failed\n");
        return -1;
    }

    return 0;
}

static void backup_complete_cb(void *opaque, int ret)
{
    BackupCB *bcb = opaque;

    DPRINTF("backup_complete_cb %d %d\n", bcb->dev_id, ret);

    if (ret < 0) {
        vma_writer_set_error(bcb->vmaw, "backup_complete_cb %d", ret);
    }

    if (vma_writer_close_stream(bcb->vmaw, bcb->dev_id) <= 0) {
        Error *err = NULL;
        if (vma_writer_close(bcb->vmaw, &err) != 0) {
            g_error("vma_writer_close failed %s", error_get_pretty(err));
        }
    }
    DPRINTF("backup_complete_cb finish\n");
}

static void write_sec_pattern_cd(BlockDriverState *bs, int64_t offset)
{
    int ret;

    DPRINTF("write_sec_pattern_cd %" PRId64 "\n", offset);

    if (offset & 0x1ff) {
        g_error("write_sec_pattern_cd offset %" PRId64
                " is not sector aligned\n", offset);
    }

    ret = bdrv_write(bs, offset >> 9, buf_sec_pattern_cd, 1);
    if (ret < 0) {
        g_error("write_sec_pattern_cd %" PRId64 " failed", offset);
    }

}

static void read_sec(BlockDriverState *bs, int64_t offset, unsigned char *buf)
{
    DPRINTF("read_sec C%" PRId64 " start %" PRId64 "\n",
            offset>>VMA_CLUSTER_BITS, offset);

    if (offset & 0x1ff) {
        g_error("read_sec offset %" PRId64 " is not sector aligned\n", offset);
    }

    if (bdrv_read(bs, offset >> 9, buf, 1) < 0) {
        g_error("bdrv_read failed");
    }
}

static bool request_term;

typedef struct TestCB {
    Coroutine *co;
    BlockDriverState *bs;
    bool finished;
} TestCB;

static TestCB *enter_test_co(BlockDriverState *bs, CoroutineEntry *entry)
{
    TestCB *cb = g_new0(TestCB, 1);
    cb->bs = bs;
    cb->co = qemu_coroutine_create(entry);
    qemu_coroutine_enter(cb->co, cb);
    return cb;
}

static void test_co_sleep(double sec)
{
    co_sleep_ns(rt_clock, (int64_t)(sec*1000000000));
};

static void test_co_yield(void)
{
    co_sleep_ns(rt_clock, (int64_t)(1000));
};

static void coroutine_fn run_co_test1(void *opaque)
{
    assert(opaque);
    TestCB *cb = (TestCB *)opaque;

    test_co_sleep(0.2);
    write_sec_pattern_cd(cb->bs, 5*BACKUP_CLUSTER_SIZE);
    test_co_sleep(0.2);
    write_sec_pattern_cd(cb->bs, 10*BACKUP_CLUSTER_SIZE);
    test_co_sleep(0.2);
    write_sec_pattern_cd(cb->bs, 10*BACKUP_CLUSTER_SIZE);

    cb->finished = true;
}

static void coroutine_fn run_co_test2(void *opaque)
{
    assert(opaque);
    TestCB *cb = (TestCB *)opaque;
    unsigned char buf[512];

    test_co_sleep(0.2);
    read_sec(cb->bs, 5*BACKUP_CLUSTER_SIZE, buf);
    write_sec_pattern_cd(cb->bs, 6*BACKUP_CLUSTER_SIZE);

    cb->finished = true;
}

static void coroutine_fn run_co_random_read(void *opaque)
{
    assert(opaque);
    TestCB *cb = (TestCB *)opaque;
    int64_t sectors = bdrv_getlength(cb->bs)/BDRV_SECTOR_SIZE - 1;
    unsigned char buf[512];

    while (1) {
        test_co_yield();
        if (request_term) {
            DPRINTF("finish run_co_random_read\n");
            break;
        }
        int64_t s = (rand()*sectors)/RAND_MAX;
        read_sec(cb->bs, s*BDRV_SECTOR_SIZE, buf);
    }

    cb->finished = true;
}

static void coroutine_fn run_co_random_write(void *opaque)
{
    assert(opaque);
    TestCB *cb = (TestCB *)opaque;
    int64_t sectors = bdrv_getlength(cb->bs)/BDRV_SECTOR_SIZE;

    while (1) {
        test_co_yield();
        if (request_term) {
            DPRINTF("finish run_co_random_write\n");
            break;
        }
        int64_t s = (rand()*sectors)/RAND_MAX;
        write_sec_pattern_cd(cb->bs, s*BDRV_SECTOR_SIZE);
    }

    cb->finished = true;
}

static void fill_test_sector(void *buf, size_t sector_num)
{
    int64_t *i64buf = (int64_t *)buf;
    int i;

    int data = sector_num;
    if (sector_num >= 8 && sector_num < 8*(2*16+2)) {
        data = 0;  /* add zero region for testing */
    }


    if (sector_num >= 20*BACKUP_BLOCKS_PER_CLUSTER &&
        sector_num <= 23*BACKUP_BLOCKS_PER_CLUSTER) {
        data = 0;  /* another zero region for testing unallocated regions */
    }

    for (i = 0; i < (512/sizeof(int64_t)); i++) {
        i64buf[i] = data;
    }
}

static void verify_archive(const char *archive, size_t size)
{
    Error *errp = NULL;

    VmaReader *vmar = vma_reader_create(archive, &errp);

    if (!vmar) {
        g_error("%s", error_get_pretty(errp));
    }

    VmaDeviceInfo *di = vma_reader_get_device_info(vmar, 1);
    if (!di || strcmp((char *)di->devname, "hda") || di->size != size) {
        g_error("got wrong device info");
    }

    unlink(TEST_IMG_RESTORE_NAME);

    int flags = BDRV_O_NATIVE_AIO|BDRV_O_RDWR|BDRV_O_CACHE_WB;

    bdrv_img_create(TEST_IMG_RESTORE_NAME, "raw", NULL, NULL, NULL,
                    size, flags, &errp);
    if (error_is_set(&errp)) {
        g_error("can't create file %s: %s", TEST_IMG_RESTORE_NAME,
                error_get_pretty(errp));
    }

    BlockDriverState *bs = NULL;
    if (bdrv_file_open(&bs, TEST_IMG_RESTORE_NAME, flags)) {
        g_error("can't open file %s", TEST_IMG_RESTORE_NAME);
    }
    if (vma_reader_register_bs(vmar, 1, bs, false, &errp) < 0) {
        g_error("%s", error_get_pretty(errp));
    }

    if (vma_reader_restore(vmar, -1, false, &errp) < 0) {
        g_error("restore failed - %s", error_get_pretty(errp));
    }

    size_t i;
    size_t sectors = size/BDRV_SECTOR_SIZE;
    int64_t buf[512/sizeof(int64_t)];
    int64_t buf2[512/sizeof(int64_t)];

    for (i = 0; i < sectors; i++) {
        if (bdrv_read(bs, i, (uint8_t *)buf, 1) != 0) {
            g_error("bdrv_read failed");
        }
        fill_test_sector(buf2, i);
        if (bcmp(buf, buf2, sizeof(buf))) {
            g_error("data is different at sector %" PRId64, i);
        }
    }

    vma_reader_destroy(vmar);

    unlink(TEST_IMG_RESTORE_NAME);
}

static void prepare_vm_image(const char *filename, size_t sectors)
{
    int fd = open(filename, O_RDWR|O_CREAT|O_TRUNC, 0644);
    if (fd < 0) {
        g_error("can't open file %s\n", filename);
    }

    size_t i;
    int64_t buf[512/sizeof(int64_t)];

    for (i = 0; i < sectors; i++) {
        if (i >= 20*BACKUP_BLOCKS_PER_CLUSTER &&
            i <= 23*BACKUP_BLOCKS_PER_CLUSTER) {
            continue; /* create a hole */
        }

        fill_test_sector(buf, i);

        int res = 0;
        while (1) {
            res = pwrite(fd, buf, sizeof(buf), i*512);
            if (!(res < 0 && errno == EINTR)) {
                break;
            }
        }
        if (res != sizeof(buf)) {
            g_error("can't initialize file %s - %s %d\n",
                    filename, g_strerror(errno), res);
        }
    }

    if (close(fd) != 0) {
        g_error("close failed");
    }
}

static GList *simple_test(BlockDriverState *bs)
{
    GList *cb_list = NULL;

    cb_list = g_list_append(cb_list, enter_test_co(bs, run_co_test1));
    cb_list = g_list_append(cb_list, enter_test_co(bs, run_co_test2));

    return cb_list;
}

static GList *random_read_write_test(BlockDriverState *bs)
{
    GList *cb_list = NULL;

    cb_list = g_list_append(cb_list, enter_test_co(bs, run_co_random_read));
    cb_list = g_list_append(cb_list, enter_test_co(bs, run_co_random_read));
    cb_list = g_list_append(cb_list, enter_test_co(bs, run_co_random_write));
    cb_list = g_list_append(cb_list, enter_test_co(bs, run_co_random_write));

    return cb_list;
}

static void backup_test(const char *testname, int64_t speed,
                        GList *(*start_test_cb)(BlockDriverState *bs))
{
    BlockDriverState *bs = bdrv_new("hda");

    static int test_counter;

    test_counter++;

    printf("starting test #%d '%s'\n", test_counter, testname);

    const char *filename = TEST_IMG_NAME;

    prepare_vm_image(TEST_IMG_NAME, TEST_IMG_SIZE/BDRV_SECTOR_SIZE);

    int flags = BDRV_O_NATIVE_AIO|BDRV_O_RDWR|BDRV_O_CACHE_WB;

    if (bdrv_open(bs, filename, flags, NULL) < 0) {
        g_error("can't open device %s\n", filename);
    }

    Error *err = NULL;
    uuid_t uuid;
    uuid_generate(uuid);

    unlink(TEST_VMA_NAME);

    VmaWriter *vmaw = vma_writer_create(TEST_VMA_NAME, uuid, &err);
    if (!vmaw) {
        g_error("%s", error_get_pretty(err));
    }

    BackupCB bcb;
    bcb.vmaw = vmaw;
    bcb.dev_id = vma_writer_register_stream(vmaw, bdrv_get_device_name(bs),
                                            bdrv_getlength(bs));
    if (backup_job_create(bs, backup_dump_cb, backup_complete_cb, &bcb,
                          speed) < 0) {
        g_error("backup_job_create failed");
    } else {
        backup_job_start(bs, false);
    }

    request_term = false;

    GList *cb_list = start_test_cb(bs);

    while (1) {
        main_loop_wait(false);

        VmaStatus vmastat;
        vma_writer_get_status(vmaw, &vmastat);
        if (vmastat.closed) {
            break;
        }
    }

    request_term = true;

    while (1) {
        GList *l = cb_list;
        bool active = 0;
        while (l && l->data) {
            TestCB *cb = (TestCB *)l->data;
            l = g_list_next(l);
            if (!cb->finished) {
                active = true;
                break;
            }
        }
        if (!active) {
            DPRINTF("All test coroutines finished\n");
            break;
        }
        main_loop_wait(false);
    }

    /* Make sure all outstanding requests complete */
    bdrv_drain_all();

    VmaStatus vmastat;
    vma_writer_get_status(vmaw, &vmastat);
    DPRINTF("statistic %" PRId64 " %" PRId64 "\n", vmastat.stream_info[1].size,
            vmastat.stream_info[1].transferred);
    assert(vmastat.stream_info[1].size == vmastat.stream_info[1].transferred);

    vma_writer_destroy(vmaw);

    bdrv_delete(bs);

    /* start verification */
    verify_archive(TEST_VMA_NAME, TEST_IMG_SIZE);

    bdrv_close_all();

    unlink(TEST_IMG_NAME);
    unlink(TEST_VMA_NAME);

    printf("finish test #%d '%s' OK\n", test_counter, testname);
}

static void help(void)
{
    const char *help_msg =
        "usage: backup-test [options]\n"
        "\n"
        "backup-test        run default regression test (fast)\n"
        "backup-test -l     run long running test loop (endless)\n"
        "\n"
        "use option -d to turn on verbose debug output\n"
        ;

    printf("%s", help_msg);
    exit(1);
}

int main(int argc, char **argv)
{
    int c;

    /* Note: GLib needs to be running in multithreaded mode in order
     * for the GSlice allocator to be thread-safe
     */
    g_thread_init(NULL);

    for (;;) {
        c = getopt(argc, argv, "hdl");
        if (c == -1) {
            break;
        }
        switch (c) {
        case '?':
        case 'h':
            help();
            break;
        case 'd':
            opt_debug = 1;
            break;
        case 'l':
            opt_loop = 1;
            break;
        default:
            g_assert_not_reached();
        }
    }

    memset(buf_sec_pattern_cd, 0xcd, sizeof(buf_sec_pattern_cd));
    memset(buf_sec_pattern_32, 0x32, sizeof(buf_sec_pattern_32));

    srand(1234);

    /* ignore SIGPIPE */
    struct sigaction act;
    sigfillset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_handler = SIG_IGN;
    sigaction(SIGPIPE, &act, NULL);

    qemu_init_main_loop();

    bdrv_init();

    if (opt_loop) { /* endless test loop */
        while (1) {
            RUN_TEST(random_read_write_test, 0);
        }
        return 0;
    }

    if (opt_debug) { /* run simple test (rate limited) */
        RUN_TEST(simple_test, 1024*1024);
        return 0;
    }

    /* run default regression tests at full speed */

    RUN_TEST(simple_test, 0);
    RUN_TEST(random_read_write_test, 0);

    return 0;
}
