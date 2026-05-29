#include <stdlib.h>
#include <unistd.h>
#include "cmds.h"
#include "state.h"

#suite cmds

#tcase cmd_exit

const struct {
	struct state s1, s2;
	unsigned int argc;
	const char **argv;
} cmd_exit_works_cases[] = {
	{ .s1 = { .mode = LOOP },    .s2 = { .mode = EXITING }, .argc = 0 },
	{ .s1 = { .mode = EXITING }, .s2 = { .mode = EXITING }, .argc = 0 },
	{ .s1 = { .mode = CONSUME }, .s2 = { .mode = EXITING },
	  .argc = 1,
	  .argv = (const char **) (const char *[]) { "exit", nullptr } },
};

#test-loop(0, 3) cmd_exit_works
	auto s1 = cmd_exit_works_cases[_i].s1;
	auto s2 = cmd_exit_works_cases[_i].s2;
	auto argc = cmd_exit_works_cases[_i].argc;
	const char **argv = cmd_exit_works_cases[_i].argv;
	auto s = cmd_exit(s1, argc, argv);
	ck_assert_int_eq(s.mode, s2.mode);

#tcase cmd_queue

#test cmd_queue_pushes_song
	struct state s;
	ck_assert(mkstate(&s));
	const char *args[] = { "/a/b.mka#123", nullptr };
	s = cmd_queue(s, 1, args);
	ck_assert_uint_eq(qsize(s.queue), 1);
	cleanup_state(&s);

#test cmd_queue_ignores_wrong_argc
	struct state s;
	ck_assert(mkstate(&s));
	const char *args0[] = { nullptr };
	struct state r0 = cmd_queue(s, 0, args0);
	ck_assert_uint_eq(qsize(r0.queue), 0);
	const char *args2[] = { "/a/b.mka#123", "extra", nullptr };
	struct state r2 = cmd_queue(s, 2, args2);
	ck_assert_uint_eq(qsize(r2.queue), 0);
	/* r0, r2, and s all share the same queue.data — clean up once. */
	cleanup_state(&s);

#test cmd_queue_ignores_malformed_song
	struct state s;
	ck_assert(mkstate(&s));
	const char *args[] = { "/a/b.mka", nullptr }; /* Missing #uid. */
	struct state r = cmd_queue(s, 1, args);
	ck_assert_uint_eq(qsize(r.queue), 0);
	cleanup_state(&s);

#tcase cmd_pause

#test cmd_pause_sets_paused
	struct state s = { .play = PLAYING };
	auto r = cmd_pause(s, 0, nullptr);
	ck_assert_int_eq(r.play, PAUSED);

#test cmd_pause_is_idempotent
	struct state s = { .play = PAUSED };
	auto r = cmd_pause(s, 0, nullptr);
	ck_assert_int_eq(r.play, PAUSED);

#tcase cmd_play

#test cmd_play_sets_playing_from_stopped
	struct state s = { .play = STOPPED };
	auto r = cmd_play(s, 0, nullptr);
	ck_assert_int_eq(r.play, PLAYING);

#test cmd_play_sets_playing_from_paused
	struct state s = { .play = PAUSED };
	auto r = cmd_play(s, 0, nullptr);
	ck_assert_int_eq(r.play, PLAYING);

#tcase insert

#test cmd_insert_inserts_at_index_zero
	struct state s;
	ck_assert(mkstate(&s));
	const char *args[] = { "0 /a/b.mka#1", nullptr };
	s = cmd_insert(s, 1, args);
	ck_assert_uint_eq(qsize(s.queue), 1);
	cleanup_state(&s);

#test cmd_insert_inserts_at_negative_index
	struct state s;
	ck_assert(mkstate(&s));
	const char *args[] = { "-1 /a/b.mka#1", nullptr };
	s = cmd_insert(s, 1, args);
	ck_assert_uint_eq(qsize(s.queue), 1);
	cleanup_state(&s);

#test cmd_insert_ignores_wrong_argc
	struct state s;
	ck_assert(mkstate(&s));
	const char *args2[] = { "0", "/a/b.mka#1", nullptr };
	struct state r2 = cmd_insert(s, 2, args2);
	ck_assert_uint_eq(qsize(r2.queue), 0);
	const char *args0[] = { nullptr };
	struct state r0 = cmd_insert(s, 0, args0);
	ck_assert_uint_eq(qsize(r0.queue), 0);
	cleanup_state(&s);

#test cmd_insert_ignores_malformed_song
	struct state s;
	ck_assert(mkstate(&s));
	const char *args[] = { "0 /a/b.mka", nullptr }; /* Missing #uid. */
	struct state r = cmd_insert(s, 1, args);
	ck_assert_uint_eq(qsize(r.queue), 0);
	cleanup_state(&s);

#tcase skip

#test cmd_skip_pops_queue_stays_playing
	struct state s;
	ck_assert(mkstate(&s));
	const char *qa[] = { "/a.mka#1", nullptr };
	const char *qb[] = { "/b.mka#2", nullptr };
	s = cmd_queue(s, 1, qa);
	s = cmd_queue(s, 1, qb);
	s.play = PLAYING;
	s = cmd_skip(s, 0, nullptr);
	ck_assert_uint_eq(qsize(s.queue), 1);
	ck_assert_int_eq(s.play, PLAYING);
	cleanup_state(&s);

#test cmd_skip_stops_when_last_song_skipped
	struct state s;
	ck_assert(mkstate(&s));
	const char *qa[] = { "/a.mka#1", nullptr };
	s = cmd_queue(s, 1, qa);
	s.play = PLAYING;
	s = cmd_skip(s, 0, nullptr);
	ck_assert_uint_eq(qsize(s.queue), 0);
	ck_assert_int_eq(s.play, STOPPED);
	cleanup_state(&s);

#test cmd_skip_stops_when_queue_empty
	struct state s;
	ck_assert(mkstate(&s));
	s.play = PLAYING;
	s = cmd_skip(s, 0, nullptr);
	ck_assert_int_eq(s.play, STOPPED);
	cleanup_state(&s);

#test cmd_skip_loop_moves_song_to_back
	struct state s;
	ck_assert(mkstate(&s));
	s.mode = LOOP;
	const char *qa[] = { "/a.mka#1", nullptr };
	const char *qb[] = { "/b.mka#2", nullptr };
	s = cmd_queue(s, 1, qa);
	s = cmd_queue(s, 1, qb);
	s.play = PLAYING;
	s = cmd_skip(s, 0, nullptr);
	ck_assert_uint_eq(qsize(s.queue), 2);
	ck_assert_int_eq(s.play, PLAYING);
	cleanup_state(&s);

#test cmd_skip_loop_single_song_stays_playing
	struct state s;
	ck_assert(mkstate(&s));
	s.mode = LOOP;
	const char *qa[] = { "/a.mka#1", nullptr };
	s = cmd_queue(s, 1, qa);
	s.play = PLAYING;
	s = cmd_skip(s, 0, nullptr);
	ck_assert_uint_eq(qsize(s.queue), 1);
	ck_assert_int_eq(s.play, PLAYING);
	cleanup_state(&s);

#tcase clear

#test cmd_clear_no_op_on_empty
	struct state s;
	ck_assert(mkstate(&s));
	s = cmd_clear(s, 0, nullptr);
	ck_assert_uint_eq(qsize(s.queue), 0);
	cleanup_state(&s);

#test cmd_clear_no_op_on_single_song
	struct state s;
	ck_assert(mkstate(&s));
	const char *qa[] = { "/a.mka#1", nullptr };
	s = cmd_queue(s, 1, qa);
	s = cmd_clear(s, 0, nullptr);
	ck_assert_uint_eq(qsize(s.queue), 1);
	cleanup_state(&s);

#test cmd_clear_removes_tail_songs
	struct state s;
	ck_assert(mkstate(&s));
	const char *qa[] = { "/a.mka#1", nullptr };
	const char *qb[] = { "/b.mka#2", nullptr };
	const char *qc[] = { "/c.mka#3", nullptr };
	s = cmd_queue(s, 1, qa);
	s = cmd_queue(s, 1, qb);
	s = cmd_queue(s, 1, qc);
	s = cmd_clear(s, 0, nullptr);
	ck_assert_uint_eq(qsize(s.queue), 1);
	cleanup_state(&s);

#test cmd_list_reflects_clear
	struct state s;
	ck_assert(mkstate(&s));
	const char *qa[] = { "/a.mka#1", nullptr };
	const char *qb[] = { "/b.mka#2", nullptr };
	s = cmd_queue(s, 1, qa);
	s = cmd_queue(s, 1, qb);
	s = cmd_clear(s, 0, nullptr);
	int pfd[2];
	ck_assert_int_eq(pipe(pfd), 0);
	cmd_list(s, pfd[1]);
	close(pfd[1]);
	char buf[64] = { 0 };
	ssize_t nr = read(pfd[0], buf, sizeof buf - 1);
	close(pfd[0]);
	ck_assert(nr > 0);
	ck_assert_str_eq(buf, "/a.mka#1\n");
	cleanup_state(&s);

#tcase stop

#test cmd_stop_sets_stopped
	struct state s = { .play = PLAYING };
	auto r = cmd_stop(s, 0, nullptr);
	ck_assert_int_eq(r.play, STOPPED);

#test cmd_stop_preserves_queue
	struct state s;
	ck_assert(mkstate(&s));
	const char *qa[] = { "/a.mka#1", nullptr };
	s = cmd_queue(s, 1, qa);
	s.play = PLAYING;
	s = cmd_stop(s, 0, nullptr);
	ck_assert_int_eq(s.play, STOPPED);
	ck_assert_uint_eq(qsize(s.queue), 1);
	cleanup_state(&s);

#tcase toggle

const struct {
	enum playstate in, out;
} cmd_toggle_cases[] = {
	{ PLAYING, PAUSED  },
	{ PAUSED,  PLAYING },
	{ STOPPED, STOPPED },
};

#test-loop(0, 3) cmd_toggle_works
	struct state s = { .play = cmd_toggle_cases[_i].in };
	auto r = cmd_toggle(s, 0, nullptr);
	ck_assert_int_eq(r.play, cmd_toggle_cases[_i].out);

#tcase cmd_list

#test cmd_list_empty_outputs_nothing
	struct state s;
	ck_assert(mkstate(&s));
	int pfd[2];
	ck_assert_int_eq(pipe(pfd), 0);
	cmd_list(s, pfd[1]);
	close(pfd[1]);
	char buf[1] = { 0 };
	ssize_t nr = read(pfd[0], buf, sizeof buf);
	close(pfd[0]);
	ck_assert(nr == 0);
	cleanup_state(&s);

#test cmd_list_outputs_path_and_uid
	struct state s;
	ck_assert(mkstate(&s));
	const char *args[] = { "/music/a.mka#42", nullptr };
	s = cmd_queue(s, 1, args);
	int pfd[2];
	ck_assert_int_eq(pipe(pfd), 0);
	cmd_list(s, pfd[1]);
	close(pfd[1]);
	char buf[64] = { 0 };
	ssize_t nr = read(pfd[0], buf, sizeof buf - 1);
	close(pfd[0]);
	ck_assert(nr > 0);
	ck_assert_str_eq(buf, "/music/a.mka#42\n");
	cleanup_state(&s);

#test cmd_list_outputs_all_songs_in_order
	struct state s;
	ck_assert(mkstate(&s));
	const char *qa[] = { "/a.mka#1", nullptr };
	const char *qb[] = { "/b.mka#2", nullptr };
	const char *qc[] = { "/c.mka#3", nullptr };
	s = cmd_queue(s, 1, qa);
	s = cmd_queue(s, 1, qb);
	s = cmd_queue(s, 1, qc);
	int pfd[2];
	ck_assert_int_eq(pipe(pfd), 0);
	cmd_list(s, pfd[1]);
	close(pfd[1]);
	char buf[128] = { 0 };
	ssize_t nr = read(pfd[0], buf, sizeof buf - 1);
	close(pfd[0]);
	ck_assert(nr > 0);
	ck_assert_str_eq(buf, "/a.mka#1\n/b.mka#2\n/c.mka#3\n");
	cleanup_state(&s);

#test cmd_list_reflects_skip
	struct state s;
	ck_assert(mkstate(&s));
	const char *qa[] = { "/a.mka#1", nullptr };
	const char *qb[] = { "/b.mka#2", nullptr };
	s = cmd_queue(s, 1, qa);
	s = cmd_queue(s, 1, qb);
	s = cmd_skip(s, 0, nullptr);
	int pfd[2];
	ck_assert_int_eq(pipe(pfd), 0);
	cmd_list(s, pfd[1]);
	close(pfd[1]);
	char buf[64] = { 0 };
	ssize_t nr = read(pfd[0], buf, sizeof buf - 1);
	close(pfd[0]);
	ck_assert(nr > 0);
	ck_assert_str_eq(buf, "/b.mka#2\n");
	cleanup_state(&s);

#test cmd_list_reflects_loop_skip
	struct state s;
	ck_assert(mkstate(&s));
	s.mode = LOOP;
	const char *qa[] = { "/a.mka#1", nullptr };
	const char *qb[] = { "/b.mka#2", nullptr };
	s = cmd_queue(s, 1, qa);
	s = cmd_queue(s, 1, qb);
	s = cmd_skip(s, 0, nullptr);
	int pfd[2];
	ck_assert_int_eq(pipe(pfd), 0);
	cmd_list(s, pfd[1]);
	close(pfd[1]);
	char buf[64] = { 0 };
	ssize_t nr = read(pfd[0], buf, sizeof buf - 1);
	close(pfd[0]);
	ck_assert(nr > 0);
	ck_assert_str_eq(buf, "/b.mka#2\n/a.mka#1\n");
	cleanup_state(&s);
