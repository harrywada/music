#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <stdbool.h>
#include "ebml.h"
#include "matroska.h"
#include "matroska_utils.h"
#include "utils.h"

int
mkv_findchapter(int fd, uint64_t id, struct mkv_chapter *c)
{
	if (ebml_descend(fd, MKV_CHAPTERS) == -1)
		return 0;

	for (;;) {
		off_t end;
		uint64_t edition_uid = 0;

		if ((end = ebml_descend(fd, MKV_EDITIONENTRY)) == -1)
			return 0;
		while (pos(fd) < end) {
			if (ebml_peek(fd) == MKV_EDITIONUID) {
				ebml_readuint(fd, MKV_EDITIONUID, &edition_uid);
				continue;
			}
			if (ebml_peek(fd) != MKV_CHAPTERATOM) {
				ebml_skip(fd, EBML_ANY_ELEMENT);
				continue;
			}
			if (!mkv_readchapteratom(fd, c))
				return 0;
			c->edition_uid = edition_uid;
			if (c->uid == id)
				return 1;
		}
	}
}

int
mkv_findtrack(int fd, const uint64_t uids[static TRACKS_MAX], struct mkv_track *t)
{
	bool any = true;
	for (int i = 0; i < TRACKS_MAX; i += 1)
		if (uids[i]) { any = false; break; }

	if (ebml_descend(fd, MKV_TRACKS) == -1)
		return 0;
	for (;;) {
		if (!mkv_readtrackentry(fd, t))
			return 0;
		if (!t->enabled || t->type != AUDIO)
			continue;
		if (any)
			return 1;
		for (int i = 0; i < TRACKS_MAX; i += 1)
			if (t->num == uids[i] || t->uid == uids[i])
				return 1;
	}
}

int
mkv_findcue(int fd, uint64_t start, uint64_t ts_scale, uint32_t track, off_t *out)
{
	off_t candidate = -1;
	int found = 0;

	if (ebml_descend(fd, MKV_CUES) == -1)
		return 0;

	for (;;) {
		struct mkv_cue next = {0};
		if (!mkv_readcuepoint(fd, &next))
			break;
		if (next.time * ts_scale > start)
			break;
		for (int i = 0; i < TRACKS_MAX; i += 1) {
			if (next.tracks[i].num == track) {
				candidate = next.tracks[i].pos;
				found = 1;
				break;
			}
		}
	}

	if (!found)
		return 0;

	*out = candidate;
	return 1;
}

static size_t
pcm_bytes_between(uint64_t start, uint64_t end, uint64_t sample_rate,
                  size_t frame_sz)
{
	return (size_t)((end - start) * sample_rate / 1000000000ULL)
	     * frame_sz;
}

static void
skip_payload_and_group(int fd, struct mkv_cursor *cursor, size_t n)
{
	seek(fd, pos(fd) + (off_t) n);
	if (cursor->blockgroup_end > 0) {
		seek(fd, cursor->blockgroup_end);
		cursor->blockgroup_end = 0;
	}
}

ssize_t
mkv_nextframe(int fd, struct mkv_cursor *cursor, const struct mkv_range *range)
{
	const off_t begin = pos(fd);

	if (cursor->pending_skip > 0) {
		seek(fd, pos(fd) + (off_t) cursor->pending_skip);
		cursor->pending_skip = 0;
		if (cursor->blockgroup_end > 0) {
			seek(fd, cursor->blockgroup_end);
			cursor->blockgroup_end = 0;
		}
		return 0;
	}
	if (cursor->blockgroup_end > 0) {
		seek(fd, cursor->blockgroup_end);
		cursor->blockgroup_end = 0;
	}

	for (;;) {
		if (cursor->blockgroup_end > 0 && pos(fd) >= cursor->blockgroup_end)
			cursor->blockgroup_end = 0;

		switch (ebml_peek(fd)) {
	case MKV_CLUSTER:
		if (ebml_descend(fd, MKV_CLUSTER) == -1)
			goto err;
		if (!ebml_readuint(fd, MKV_CLUSTERTIMESTAMP, &cursor->cluster.ts))
			goto err;
		break;

	case MKV_BLOCKGROUP:
		if ((cursor->blockgroup_end = ebml_descend(fd, MKV_BLOCKGROUP)) == -1)
			goto err;
		break;

	case MKV_SIMPLEBLOCK:
		[[fallthrough]];
	case MKV_BLOCK: {
		struct mkv_block b;

		if (!mkv_readblock(fd, &b))
			goto err;

		if (vint_value(b.track) != range->track) {
			skip_payload_and_group(fd, cursor, b.frames_sz);
			break;
		}

		const uint64_t ts = range->ts_scale
		                  * (cursor->cluster.ts + (uint64_t) b.timecode);

		if (ts < range->start) {
			if (range->sample_rate == 0 || range->frame_sz == 0) {
				skip_payload_and_group(fd, cursor, b.frames_sz);
				break;
			}
			const size_t skip =
			    pcm_bytes_between(ts, range->start,
			                      range->sample_rate, range->frame_sz);
			if (skip >= b.frames_sz) {
				skip_payload_and_group(fd, cursor, b.frames_sz);
				break;
			}
			const size_t tail = b.frames_sz - skip;
			size_t play = tail, end_skip = 0;
			if (range->end) {
				const size_t cap =
				    pcm_bytes_between(range->start, range->end,
				                      range->sample_rate, range->frame_sz);
				if (cap < tail) { play = cap; end_skip = tail - cap; }
			}
			if (play == 0) {
				skip_payload_and_group(fd, cursor, b.frames_sz);
				return 0;
			}
			cursor->leading_skip = skip;
			cursor->block_ts     = range->start;
			cursor->pending_skip = end_skip;
			return (ssize_t) play;
		}

		if (range->end && ts >= range->end) {
			skip_payload_and_group(fd, cursor, b.frames_sz);
			return 0;
		}

		cursor->leading_skip = 0;
		cursor->block_ts     = ts;

		if (range->end && range->sample_rate > 0 && range->frame_sz > 0) {
			const size_t max =
			    pcm_bytes_between(ts, range->end,
			                      range->sample_rate, range->frame_sz);
			if (max == 0) {
				skip_payload_and_group(fd, cursor, b.frames_sz);
				return 0;
			}
			if (max < b.frames_sz) {
				cursor->pending_skip = b.frames_sz - max;
				return (ssize_t) max;
			}
		}

		cursor->pending_skip = 0;
		return (ssize_t) b.frames_sz;
	}

	case 0:
		[[fallthrough]];
	default:
		if (cursor->blockgroup_end > 0 && pos(fd) < cursor->blockgroup_end) {
			ebml_skip(fd, EBML_ANY_ELEMENT);
			break;
		}
		return -1;
		}
	}

err:
	seek(fd, begin);
	return -1;
}

int
mkv_visitchapters(int fd,
    int (*cb)(const struct mkv_chapter *, void *), void *userdata)
{
	off_t chapters_end, edition_end;
	int visited = 0;

	if ((chapters_end = ebml_descend(fd, MKV_CHAPTERS)) == -1)
		return 0;

	while (pos(fd) < chapters_end) {
		uint64_t edition_uid = 0;

		if (ebml_peek(fd) != MKV_EDITIONENTRY) {
			ebml_skip(fd, EBML_ANY_ELEMENT);
			continue;
		}
		if ((edition_end = ebml_descend(fd, MKV_EDITIONENTRY)) == -1)
			return visited;

		while (pos(fd) < edition_end) {
			if (ebml_peek(fd) == MKV_EDITIONUID) {
				ebml_readuint(fd, MKV_EDITIONUID, &edition_uid);
				continue;
			}
			if (ebml_peek(fd) != MKV_CHAPTERATOM) {
				ebml_skip(fd, EBML_ANY_ELEMENT);
				continue;
			}

			struct mkv_chapter c;
			if (!mkv_readchapteratom(fd, &c))
				return visited;
			c.edition_uid = edition_uid;

			off_t resume = pos(fd);
			visited = 1;
			if (!cb(&c, userdata))
				return 1;
			seek(fd, resume);
		}
	}

	return visited;
}

/* Parse one AttachedFile element body (fd at the first child, file_end is
 * the element boundary).  Returns 1 if the file is an image and *out was
 * filled, 0 if it is not an image, -1 on a parse error. */
static int
read_attached_file(int fd, off_t file_end, struct mkv_attachment *out)
{
	char   mime[64]      = {0};
	char   filename[256] = {0};
	off_t  data_off      = 0;
	size_t data_sz       = 0;

	while (pos(fd) < file_end) {
		off_t  el_end;
		size_t n;

		switch (ebml_peek(fd)) {
		case MKV_FILEMEDIATYPE:
			if ((el_end = ebml_descend(fd, MKV_FILEMEDIATYPE)) == -1)
				return -1;
			n = (size_t)(el_end - pos(fd));
			if (n >= sizeof mime) n = sizeof mime - 1;
			if (read(fd, mime, n) != (ssize_t)n) return -1;
			mime[n] = '\0';
			seek(fd, el_end);
			break;
		case MKV_FILENAME:
			if ((el_end = ebml_descend(fd, MKV_FILENAME)) == -1)
				return -1;
			n = (size_t)(el_end - pos(fd));
			if (n >= sizeof filename) n = sizeof filename - 1;
			if (read(fd, filename, n) != (ssize_t)n) return -1;
			filename[n] = '\0';
			seek(fd, el_end);
			break;
		case MKV_FILEDATA:
			if ((el_end = ebml_descend(fd, MKV_FILEDATA)) == -1)
				return -1;
			data_off = pos(fd);
			data_sz  = (size_t)(el_end - data_off);
			seek(fd, el_end);
			break;
		default:
			ebml_skip(fd, EBML_ANY_ELEMENT);
			break;
		}
	}

	if (strncmp(mime, "image/", 6) != 0 || data_sz == 0)
		return 0;

	*out = (struct mkv_attachment){
		.data_off = data_off,
		.data_sz  = data_sz,
	};
	if (filename[0])
		memcpy(out->filename, filename, sizeof out->filename);
	else
		snprintf(out->filename, sizeof out->filename,
		         "cover.%s", strchr(mime, '/') + 1);
	memcpy(out->mime, mime, sizeof out->mime);
	return 1;
}

int
mkv_findcoverart(int fd, struct mkv_attachment *out)
{
	off_t begin = pos(fd);
	off_t att_end, file_end;

	if ((att_end = ebml_descend(fd, MKV_ATTACHMENTS)) == -1)
		goto err;

	while (pos(fd) < att_end) {
		if (ebml_peek(fd) != MKV_ATTACHEDFILE) {
			ebml_skip(fd, EBML_ANY_ELEMENT);
			continue;
		}
		if ((file_end = ebml_descend(fd, MKV_ATTACHEDFILE)) == -1)
			goto err;

		int r = read_attached_file(fd, file_end, out);
		if (r > 0)
			return 1;
		if (r < 0)
			goto err;

		seek(fd, file_end);
	}

err:
	seek(fd, begin);
	return 0;
}
