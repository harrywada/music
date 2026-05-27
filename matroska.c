#include <unistd.h> /* read(3p). */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include "ebml.h"

#include <stdint.h>
#include <sys/types.h>
#include "matroska.h"

#include <sys/types.h>
#include "utils.h"

int
mkv_readseekinfo(int fd, struct mkv_seekinfo *sk_info)
{
	uint_least32_t sk_id = 0;
	off_t sk_pos = 0;

	off_t begin, end;
	unsigned int i;

	begin = sk_info->segment = pos(fd); /* SeekHead _SHOULD_ be at the beginning of the segment. */
	if ((end = ebml_descend(fd, MKV_SEEKHEAD)) == -1)
		goto err;

	while (pos(fd) < end) {
		ebml_descend(fd, MKV_SEEK);

		for (i = 0; i < 2; i += 1) switch (ebml_peek(fd)) { /* XXX Can there be redundant (3+) elements? */
		case MKV_SEEKID: {
			uint64_t tmp;
			if (!ebml_readuint(fd, MKV_SEEKID, &tmp))
				goto err;
			sk_id = (uint_least32_t) tmp;
			break;
		}
		case MKV_SEEKPOSITION:
			if (!ebml_readuint(fd, MKV_SEEKPOSITION, (uint64_t *) &sk_pos))
				goto err;
			break;
		default:
			ebml_skip(fd, EBML_ANY_ELEMENT);
			break;
		}

		switch (sk_id) {
		case MKV_INFO:
			sk_info->info = sk_pos;
			break;
		case MKV_CLUSTER:
			sk_info->clusters = sk_pos;
			break;
		case MKV_TRACKS:
			sk_info->tracks = sk_pos;
			break;
		case MKV_CUES:
			sk_info->cues = sk_pos;
			break;
		case MKV_ATTACHMENTS:
			sk_info->attachments = sk_pos;
			break;
		case MKV_CHAPTERS:
			sk_info->chapters = sk_pos;
			break;
		case MKV_TAGS:
			sk_info->tags = sk_pos;
			break;
		default:
			goto err;
		}
	}

	return 1;

err:
	seek(fd, begin);
	return 0;
}

int
mkv_readinfo(int fd, struct mkv_info *info)
{
	off_t begin, end;

	begin = pos(fd);
	if ((end = ebml_descend(fd, MKV_INFO)) == -1)
		goto err;

	info->ts_scale = 1000000;

	while (pos(fd) < end) switch (ebml_peek(fd)) {
	case MKV_TIMESTAMPSCALE:
		if (!ebml_readuint(fd, MKV_TIMESTAMPSCALE, &info->ts_scale))
			goto err;
		return 1;
	default:
		ebml_skip(fd, EBML_ANY_ELEMENT);
		break;
	}

	return 1;

err:
	seek(fd, begin);
	return 0;
}

int
mkv_readtrackentry(int fd, struct mkv_track *track)
{
	off_t begin, end;

	begin = pos(fd);
	if ((end = ebml_descend(fd, MKV_TRACKENTRY)) == -1)
		goto err;

	/* Defaults for non-mandatory elements. */
	track->channels = 1;
	track->enabled = 1;
	track->rate = 8000;

	while (pos(fd) < end) switch (ebml_peek(fd)) {
	case MKV_TRACKNUMBER:
		if (!ebml_readuint(fd, MKV_TRACKNUMBER, &track->num))
			goto err;
		break;
	case MKV_TRACKUID:
		if (!ebml_readuint(fd, MKV_TRACKUID, &track->uid))
			goto err;
		break;
	case MKV_TRACKTYPE: {
		uint8_t tmp;
		if (!ebml_readuint(fd, MKV_TRACKTYPE, &tmp))
			goto err;
		track->type = (typeof(track->type)) tmp;
		break;
	}
	case MKV_FLAGENABLED:
		if (!ebml_readuint(fd, MKV_FLAGENABLED, &track->enabled))
			goto err;
		break;
	case MKV_AUDIO:
		ebml_descend(fd, MKV_AUDIO);
		break;
	case MKV_SAMPLINGFREQUENCY:
		if (!ebml_readfloat(fd, MKV_SAMPLINGFREQUENCY, &track->rate))
			goto err;
		break;
	case MKV_CHANNELS:
		if (!ebml_readuint(fd, MKV_CHANNELS, &track->channels))
			goto err;
		break;
	case MKV_BITDEPTH:
		if (!ebml_readuint(fd, MKV_BITDEPTH, &track->bps))
			goto err;
		break;
	default:
		ebml_skip(fd, EBML_ANY_ELEMENT);
		break;
	}

	return 1;

err:
	seek(fd, begin);
	return 0;
}

int
mkv_readcuepoint(int fd, struct mkv_cue *cue)
{
	off_t begin, end, track_end;
	unsigned int track_i = 0;

	uint64_t tmp;

	begin = pos(fd);
	if ((end = ebml_descend(fd, MKV_CUEPOINT)) == -1)
		goto err;

	while (pos(fd) < end) switch (ebml_peek(fd)) {
	case MKV_CUETIME:
		if (!ebml_readuint(fd, MKV_CUETIME, &cue->time))
			goto err;
		break;
	case MKV_CUETRACKPOSITIONS:
		if (track_i >= TRACKS_MAX)
			goto err;

		if ((track_end = ebml_descend(fd, MKV_CUETRACKPOSITIONS)) == -1)
			goto err;

		/* Defaults for non-mandatory elements. */
		cue->tracks[track_i].relpos = -1;

		while (pos(fd) < track_end) switch (ebml_peek(fd)) {
		case MKV_CUETRACK:
			if (!ebml_readuint(fd, MKV_CUETRACK, &cue->tracks[track_i].num))
				goto err;
			break;
		case MKV_CUECLUSTERPOSITION:
			if (!ebml_readuint(fd, MKV_CUECLUSTERPOSITION, &tmp))
				goto err;
			cue->tracks[track_i].pos = tmp;
			break;
		case MKV_CUERELATIVEPOSITION:
			if (!ebml_readuint(fd, MKV_CUERELATIVEPOSITION, &tmp))
				goto err;
			cue->tracks[track_i].relpos = tmp;
			break;
		default:
			ebml_skip(fd, EBML_ANY_ELEMENT);
			break;
		}

		track_i += 1;
		break;
	default:
		/* Only CueTime and CueTrackPositions are allowed. */
		goto err;
	}

	return 1;

err:
	seek(fd, begin);
	return 0;
}

int
mkv_readblock(int fd, struct mkv_block *block)
{
	off_t begin, end;

	uint16_t framesize;
	struct vint ebml_framesize;
	unsigned int offset;
	int i;

	begin = pos(fd);

	switch (ebml_peek(fd)) {
	case MKV_BLOCK:
		if ((end = ebml_descend(fd, MKV_BLOCK)) == -1)
			goto err;
		break;
	case MKV_SIMPLEBLOCK:
		if ((end = ebml_descend(fd, MKV_SIMPLEBLOCK)) == -1)
			goto err;
		break;
	default:
		goto err;
	}


	if (!vint_read(fd, &block->track))
		goto err;
	if (read(fd, &block->timecode, 2) != 2)
		goto err;
	if (read(fd, &block->flags, 1) != 1)
		goto err;

	switch (block->flags & MKV_BLOCKLACINGMASK) {
	case 0: /* No lacing. */
		block->frames_n = 1;
		block->frames_sz = end - pos(fd);
		break;
	case 2: /* Xiph lacing. */
		if (read(fd, &block->frames_n, 1) != 1)
			goto err;
		block->frames_sz = 0;
		for (i = 0; i < block->frames_n; i += 1) {
			uint8_t b;
			framesize = 0;
			do {
				if (read(fd, &b, 1) != 1)
					goto err;
				framesize += b;
			} while (b == 0xff);
			block->frames_sz += framesize;
		}
		block->frames_sz += end - (pos(fd) + block->frames_sz);
		block->frames_n += 1;
		break;
	case 4: /* Fixed-size lacing. */
		if (read(fd, &block->frames_n, 1) != 1)
			goto err;
		block->frames_n += 1;
		block->frames_sz = end - pos(fd);
		break;
	case 6: /* EBML lacing. */
		if (read(fd, &block->frames_n, 1) != 1)
			goto err;
		if (!vint_read(fd, &ebml_framesize))
			goto err;
		block->frames_sz = framesize = vint_value(ebml_framesize);
		for (i = 0; i < block->frames_n - 1; i += 1) {
			if (!vint_read(fd, &ebml_framesize))
				goto err;
			offset = (1 << ((7 * ebml_framesize.size) - 1)) - 1;
			framesize = vint_value(ebml_framesize) - offset + framesize;
			block->frames_sz += framesize;
		}
		block->frames_sz += end - (pos(fd) + block->frames_sz);
		block->frames_n += 1;
		break;
	default:
		ebml_skip(fd, EBML_ANY_ELEMENT); /* XXX Is this correct? Shouldn't it just seek to the end or error? */
		break;
	}

	return 1;

err:
	seek(fd, begin);
	return 0;
}

int
mkv_readchapteratom(int fd, struct mkv_chapter *chapter)
{
	off_t begin, end;
	int track_i;

	for (track_i = 0; track_i < TRACKS_MAX; track_i += 1)
		chapter->track_uids[track_i] = 0;
	chapter->end = 0; /* Default, may not be set. */
	track_i = 0;

	begin = pos(fd);
	if ((end = ebml_descend(fd, MKV_CHAPTERATOM)) == -1)
		goto err;

	while (pos(fd) < end) switch (ebml_peek(fd)) {
	case MKV_CHAPTERUID:
		if (!ebml_readuint(fd, MKV_CHAPTERUID, &chapter->uid))
			goto err;
		break;
	case MKV_CHAPTERTIMESTART:
		if (!ebml_readuint(fd, MKV_CHAPTERTIMESTART, &chapter->start))
			goto err;
		break;
	case MKV_CHAPTERTIMEEND:
		if (!ebml_readuint(fd, MKV_CHAPTERTIMEEND, &chapter->end))
			goto err;
		break;
	case MKV_CHAPTERTRACK:
		ebml_descend(fd, MKV_CHAPTERTRACK);
		while (track_i < TRACKS_MAX && ebml_peek(fd) == MKV_CHAPTERTRACKUID)
			if (!ebml_readuint(fd, MKV_CHAPTERTRACKUID, &chapter->track_uids[track_i++]))
				goto err;
		break;
	default:
		ebml_skip(fd, EBML_ANY_ELEMENT);
		break;
	}

	return 1;

err:
	seek(fd, begin);
	return 0;
}

int
mkv_findchapter(int fd, uint64_t id, struct mkv_chapter *c)
{
	if (ebml_descend(fd, MKV_CHAPTERS) == -1)
		return 0;

	for (;;) {
		off_t end;

		if ((end = ebml_descend(fd, MKV_EDITIONENTRY)) == -1)
			return 0;
		while (pos(fd) < end) {
			if (ebml_peek(fd) != MKV_CHAPTERATOM) {
				ebml_skip(fd, EBML_ANY_ELEMENT);
				continue;
			}
			if (!mkv_readchapteratom(fd, c))
				return 0;
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
			if (t->uid == uids[i])
				return 1;
	}
}

int
mkv_findcue(int fd, uint64_t start, uint64_t ts_scale, uint32_t track, off_t *out)
{
	struct mkv_cue cue = {0}, next;
	int found = 0;

	if (ebml_descend(fd, MKV_CUES) == -1)
		return 0;

	while (mkv_readcuepoint(fd, &next)) {
		if (next.time * ts_scale > start)
			break;
		cue = next;
		found = 1;
	}

	if (!found)
		return 0;

	for (int i = 0; i < TRACKS_MAX; i += 1) {
		if (cue.tracks[i].num == track) {
			*out = cue.tracks[i].pos;
			return 1;
		}
	}
	return 0;
}

ssize_t
mkv_nextframe(int fd, struct mkv_cursor *cursor, const struct mkv_range *range)
{
	const off_t begin = pos(fd);

	for (;;) switch (ebml_peek(fd)) {
	case MKV_CLUSTER:
		if (ebml_descend(fd, MKV_CLUSTER) == -1)
			goto err;
		if (!ebml_readuint(fd, MKV_CLUSTERTIMESTAMP, &cursor->cluster.ts))
			goto err;
		break;

	case MKV_BLOCKGROUP:
		ebml_descend(fd, MKV_BLOCKGROUP);
		break;

	case MKV_SIMPLEBLOCK:
		[[fallthrough]];
	case MKV_BLOCK: {
		struct mkv_block b;

		if (!mkv_readblock(fd, &b))
			goto err;

		if (vint_value(b.track) != range->track) {
			seek(fd, pos(fd) + b.frames_sz);
			break;
		}

		const uint64_t ts = range->ts_scale
		                  * (cursor->cluster.ts + (uint64_t) b.timecode);
		if (ts < range->start) {
			seek(fd, pos(fd) + b.frames_sz);
			break;
		}
		if (range->end && ts >= range->end) {
			seek(fd, pos(fd) + b.frames_sz);
			return 0;
		}

		return (ssize_t) b.frames_sz;
	}

	case 0:
		[[fallthrough]];
	default:
		return -1;
	}

err:
	seek(fd, begin);
	return -1;
}
