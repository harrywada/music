/* Include: ebml.h, stdbool.h, stdint.h, sys/types.h. */

#define MKV_SEGMENT 0x18538067

#define MKV_INFO 0x1549a966
#define MKV_TIMESTAMPSCALE 0x2ad7b1

#define MKV_SEEKHEAD 0x114d9b74
#define MKV_SEEK 0x4dbb
#define MKV_SEEKID 0x53ab
#define MKV_SEEKPOSITION 0x53ac

#define MKV_CLUSTER 0x1f43b675
#define MKV_CLUSTERTIMESTAMP 0xe7
#define MKV_SIMPLEBLOCK 0xa3
#define MKV_BLOCKGROUP 0xa0
#define MKV_BLOCK 0xa1
#define MKV_BLOCKLACINGMASK 0x6
#define MKV_BLOCKDURATION 0x9b

#define MKV_TRACKS 0x1654ae6b
#define MKV_TRACKENTRY 0xae
#define MKV_TRACKNUMBER 0xd7
#define MKV_TRACKUID 0x73c5
#define MKV_TRACKTYPE 0x83
#define MKV_FLAGENABLED 0xb9
#define MKV_AUDIO 0xe1
#define MKV_SAMPLINGFREQUENCY 0xb5
#define MKV_CHANNELS 0x9f
#define MKV_BITDEPTH 0x6264

#define MKV_CUES 0x1c53bb6b
#define MKV_CUEPOINT 0xbb
#define MKV_CUETIME 0xb3
#define MKV_CUETRACKPOSITIONS 0xb7
#define MKV_CUETRACK 0xf7
#define MKV_CUECLUSTERPOSITION 0xf1
#define MKV_CUERELATIVEPOSITION 0xf0

#define MKV_ATTACHMENTS 0x1941a469

#define MKV_CHAPTERS 0x1043a770
#define MKV_EDITIONENTRY 0x45b9
#define MKV_CHAPTERATOM 0xb6
#define MKV_CHAPTERUID 0x73c4
#define MKV_CHAPTERTIMESTART 0x91
#define MKV_CHAPTERTIMEEND 0x92
#define MKV_CHAPTERTRACK 0x8f
#define MKV_CHAPTERTRACKUID 0x89

#define MKV_TAGS 0x1254c367

struct mkv_seekinfo {
	off_t segment;
	off_t info;
	off_t clusters;
	off_t tracks;
	off_t cues;
	off_t attachments;
	off_t chapters;
	off_t tags;
};

struct mkv_info {
	uint64_t ts_scale;
};

struct mkv_cluster {
	uint64_t ts;
};

struct mkv_track {
	uint64_t uid;
	enum : uint8_t {
		VIDEO = 1,
		AUDIO = 2,
		COMPLEX = 3,
		LOGO = 16,
		SUBTITLE = 17,
		BUTTONS = 18,
		CONTROL = 32,
		METADATA = 33,
	} type;
	bool     enabled;
	uint32_t num;
	double   rate;
	uint8_t  channels;
	uint8_t  bps;
};

struct mkv_cue {
	uint64_t time;
	struct {
		off_t    pos, relpos;
		uint32_t num; /* Zero for null tracks. */
	} tracks[TRACKS_MAX];
};

struct mkv_block {
	struct vint track;
	int16_t timecode;
	uint8_t flags;
	uint8_t frames_n;
	size_t frames_sz; /* Aggregate size of all frames in the block. */
};

struct mkv_chapter {
	uint64_t uid;
	uint64_t start, end; /* Zero `end` value implies end of the track. */
	uint64_t track_uids[TRACKS_MAX]; /* If all 0, all tracks apply. */
};

[[gnu::fd_arg_read(1)]]
int mkv_readseekinfo(int, struct mkv_seekinfo *);
[[gnu::fd_arg_read(1)]]
int mkv_readinfo(int, struct mkv_info *);
[[gnu::fd_arg_read(1)]]
int mkv_readtrackentry(int, struct mkv_track *);
[[gnu::fd_arg_read(1)]]
int mkv_readcuepoint(int, struct mkv_cue *);
[[gnu::fd_arg_read(1)]]
int mkv_readblock(int, struct mkv_block *); /* Only reads header; data remains. */
[[gnu::fd_arg_read(1)]]
int mkv_readchapteratom(int, struct mkv_chapter *);
