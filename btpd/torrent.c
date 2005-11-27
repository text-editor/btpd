#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/sha.h>

#include "btpd.h"
#include "tracker_req.h"
#include "stream.h"

static int
ro_fd_cb(const char *path, int *fd, void *arg)
{
    struct torrent *tp = arg;
    return vopen(fd, O_RDONLY, "%s/content/%s", tp->relpath, path);
}

static int
wo_fd_cb(const char *path, int *fd, void *arg)
{
    struct torrent *tp = arg;
    return vopen(fd, O_WRONLY|O_CREAT, "%s/content/%s", tp->relpath, path);
}

static int
torrent_load3(const char *file, struct metainfo *mi, char *mem, size_t memsiz)
{
    struct torrent *tp = btpd_calloc(1, sizeof(*tp));

    tp->relpath = strdup(file);
    if (tp->relpath == NULL)
	btpd_err("Out of memory.\n");

    tp->piece_count = btpd_calloc(mi->npieces, sizeof(tp->piece_count[0]));
    tp->busy_field = btpd_calloc(ceil(mi->npieces / 8.0), 1);

    BTPDQ_INIT(&tp->peers);
    BTPDQ_INIT(&tp->getlst);

    tp->imem = mem;
    tp->isiz = memsiz;

    tp->piece_field = tp->imem;
    tp->block_field =
	(uint8_t *)tp->imem + (size_t)ceil(mi->npieces / 8.0);

    for (uint32_t i = 0; i < mi->npieces; i++)
	if (has_bit(tp->piece_field, i))
	    tp->have_npieces++;

    tp->meta = *mi;
    free(mi);

    btpd_add_torrent(tp);

    tracker_req(tp, TR_STARTED);

    return 0;
}

static int
torrent_load2(const char *name, struct metainfo *mi)
{
    int error, ifd;
    struct stat sb;
    char *mem;
    size_t memsiz;
    const char *file = name;

    if ((error = vopen(&ifd, O_RDWR, "%s/resume", file)) != 0) {
	btpd_log(BTPD_L_ERROR, "Error opening %s.i: %s.\n",
	    file, strerror(error));
	return error;
    }

    if (fstat(ifd, &sb) == -1) {
	error = errno;
	btpd_log(BTPD_L_ERROR, "Error stating %s.i: %s.\n",
	    file, strerror(error));
	close(ifd);
	return error;
    }

    memsiz =
	ceil(mi->npieces / 8.0) +
	mi->npieces * ceil(mi->piece_length / (double)(1 << 17));

    if (sb.st_size != memsiz) {
	btpd_log(BTPD_L_ERROR, "File has wrong size: %s.i.\n", file);
	close(ifd);
	return EINVAL;
    }

    mem = mmap(NULL, memsiz, PROT_READ|PROT_WRITE, MAP_SHARED, ifd, 0);
    if (mem == MAP_FAILED)
	btpd_err("Error mmap'ing %s.i: %s.\n", file, strerror(errno));

    close(ifd);

    if ((error = torrent_load3(file, mi, mem, memsiz) != 0)) {
	munmap(mem, memsiz);
	return error;
    }

    return 0;
}

int
torrent_load(const char *name)
{
    struct metainfo *mi;
    int error;
    char file[PATH_MAX];
    snprintf(file, PATH_MAX, "%s/torrent", name);

    if ((error = load_metainfo(file, -1, 0, &mi)) != 0) {
	btpd_log(BTPD_L_ERROR, "Couldn't load metainfo file %s: %s.\n",
	    file, strerror(error));
	return error;
    }

    if (btpd_get_torrent(mi->info_hash) != NULL) {
	btpd_log(BTPD_L_BTPD, "%s has same hash as an already loaded torrent.\n", file);
	error = EEXIST;
    }

    if (error == 0)
	error = torrent_load2(name, mi);

    if (error != 0) {
	clear_metainfo(mi);
	free(mi);
    }

    return error;
}

void
torrent_unload(struct torrent *tp)
{
    struct peer *peer;
    struct piece *piece;

    btpd_log(BTPD_L_BTPD, "Unloading %s.\n", tp->relpath);

    tracker_req(tp, TR_STOPPED);

    peer = BTPDQ_FIRST(&tp->peers);
    while (peer != NULL) {
        struct peer *next = BTPDQ_NEXT(peer, p_entry);
	BTPDQ_REMOVE(&tp->peers, peer, p_entry);
	BTPDQ_INSERT_TAIL(&net_unattached, peer, p_entry);
        peer->flags &= ~PF_ATTACHED;
        peer = next;
    }

    peer = BTPDQ_FIRST(&net_unattached);
    while (peer != NULL) {
	struct peer *next = BTPDQ_NEXT(peer, p_entry);
	if (peer->tp == tp)
	    peer_kill(peer);
	peer = next;
    }

    while ((piece = BTPDQ_FIRST(&tp->getlst)) != NULL)
	piece_free(piece);

    free(tp->piece_count);
    free(tp->busy_field);
    free((void *)tp->relpath);
    clear_metainfo(&tp->meta);

    munmap(tp->imem, tp->isiz);

    btpd_del_torrent(tp);
    free(tp);
}

off_t
torrent_bytes_left(struct torrent *tp)
{
    if (tp->have_npieces == 0)
	return tp->meta.total_length;
    else if (has_bit(tp->piece_field, tp->meta.npieces - 1)) {
	return tp->meta.total_length - 
	    ((tp->have_npieces - 1) * tp->meta.piece_length +
	    tp->meta.total_length % tp->meta.piece_length);
    } else
	return tp->meta.total_length -
	    tp->have_npieces * tp->meta.piece_length;
}

char *
torrent_get_bytes(struct torrent *tp, off_t start, size_t len)
{
    char *buf = btpd_malloc(len);
    struct bt_stream_ro *bts;
    if ((bts = bts_open_ro(&tp->meta, start, ro_fd_cb, tp)) == NULL)
	btpd_err("Out of memory.\n");
    if (bts_read_ro(bts, buf, len) != 0)
	btpd_err("Io error.\n");
    bts_close_ro(bts);
    return buf;
}

void
torrent_put_bytes(struct torrent *tp, const char *buf, off_t start, size_t len)
{
    int err;
    struct bt_stream_wo *bts;
    if ((bts = bts_open_wo(&tp->meta, start, wo_fd_cb, tp)) == NULL)
	btpd_err("Out of memory.\n");
    if ((err = bts_write_wo(bts, buf, len)) != 0)
	btpd_err("Io error1: %s\n", strerror(err));
    if ((err = bts_close_wo(bts)) != 0)
	btpd_err("Io error2: %s\n", strerror(err));
}

int
torrent_has_peer(struct torrent *tp, const uint8_t *id)
{
    int has = 0;
    struct peer *p = BTPDQ_FIRST(&tp->peers);
    while (p != NULL) {
	if (bcmp(p->id, id, 20) == 0) {
	    has = 1;
	    break;
	}
	p = BTPDQ_NEXT(p, p_entry);
    }
    return has;
}

off_t
torrent_piece_size(struct torrent *tp, uint32_t index)
{
    if (index < tp->meta.npieces - 1)
	return tp->meta.piece_length;
    else {
	off_t allbutlast = tp->meta.piece_length * (tp->meta.npieces - 1);
	return tp->meta.total_length - allbutlast;
    }
}

uint32_t
torrent_block_size(struct piece *pc, uint32_t index)
{
    if (index < pc->nblocks - 1)
	return PIECE_BLOCKLEN;
    else {
	uint32_t allbutlast = PIECE_BLOCKLEN * (pc->nblocks - 1);
	return torrent_piece_size(pc->tp, pc->index) - allbutlast;
    }
}

int
torrent_has_all(struct torrent *tp)
{
    return tp->have_npieces == tp->meta.npieces;
}
