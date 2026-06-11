#!/bin/sh

set -eu

TAGS='REPLAYGAIN_GAIN REPLAYGAIN_PEAK EBU_R128_LOUDNESS
EBU_R128_MAX_TRUE_PEAK EBU_R128_LOUDNESS_RANGE
EBU_R128_MAX_MOMENTARY_LOUDNESS EBU_R128_MAX_SHORT_LOUDNESS'

die()
{
	printf '%s\n' "$*" >&2
	exit 1
}

usage()
{
	die "usage: $0 FILE"
}

need()
{
	command -v "$1" >/dev/null 2>&1 || die "missing dependency: $1"
}

xml_escape()
{
	sed \
		-e 's/&/\&amp;/g' \
		-e 's/</\&lt;/g' \
		-e 's/>/\&gt;/g'
}

binary_float()
{
	perl -MMIME::Base64=encode_base64 -e '
		print encode_base64(pack("d>", $ARGV[0]), "");
	' -- "$1"
}

replaygain()
{
	file=$1
	stream=$2

	ffmpeg -nostdin -hide_banner -nostats -i "$file" -map "0:a:$stream" \
		-filter:a replaygain -f null - 2>&1 |
	awk '
		/track_gain =/ {
			for (i = 1; i <= NF; i++)
				if ($i == "=")
					gain = $(i + 1);
		}
		/track_peak =/ {
			for (i = 1; i <= NF; i++)
				if ($i == "=")
					peak = $(i + 1);
		}
		END {
			if (gain == "" || peak == "")
				exit 1;
			printf "%s\t%s\n", gain, peak;
		}
	' || return 1
}

ebur128()
{
	file=$1
	stream=$2

	ffmpeg -nostdin -hide_banner -nostats -i "$file" -map "0:a:$stream" \
		-filter:a ebur128=peak=true:framelog=info -f null - 2>&1 |
	awk '
		/Parsed_ebur128/ && / t: / {
			for (i = 1; i <= NF; i++) {
				if ($i == "M:") {
					if (!seen_m || $(i + 1) + 0 > max_m) {
						max_m = $(i + 1) + 0;
						seen_m = 1;
					}
				}
				if ($i == "S:") {
					if (!seen_s || $(i + 1) + 0 > max_s) {
						max_s = $(i + 1) + 0;
						seen_s = 1;
					}
				}
			}
		}
		/Summary:/ {
			summary = 1;
		}
		summary && /^[[:space:]]*I:/ {
			integrated = $2;
		}
		summary && /^[[:space:]]*LRA:/ {
			lra = $2;
		}
		summary && /^[[:space:]]*Peak:/ {
			true_peak = $2;
		}
		END {
			if (!seen_m || !seen_s || integrated == "" ||
			    lra == "" || true_peak == "")
				exit 1;
			printf "%s\t%s\t%s\t%s\t%s\n",
			    integrated, true_peak, lra, max_m, max_s;
		}
	' || return 1
}

extract_tags()
{
	mkvextract "$1" tags /dev/stdout
}

verify_no_existing_tags()
{
	perl -0 - "$1" "$2" $TAGS <<'EOF'
use strict;
use warnings;

my ($xml_file, $uids_file, @names) = @ARGV;
open my $xfh, "<:raw", $xml_file or die "$xml_file: $!\n";
my $xml = do { local $/; <$xfh> };
open my $ufh, "<", $uids_file or die "$uids_file: $!\n";
my %uids = map { chomp; $_ => 1 } <$ufh>;

for my $tag ($xml =~ m{<Tag>.*?</Tag>}sg) {
	next unless $tag =~ m{<TrackUID>([^<]+)</TrackUID>};
	next unless $uids{$1};
	for my $name (@names) {
		die "track $1 already has $name\n"
		    if $tag =~ m{<Name>\Q$name\E</Name>};
	}
}
EOF
}

append_tags()
{
	perl -0 - "$1" "$2" "$3" <<'EOF'
use strict;
use warnings;

my ($source, $add, $out) = @ARGV;
open my $sfh, "<:raw", $source or die "$source: $!\n";
my $xml = do { local $/; <$sfh> };
open my $afh, "<:raw", $add or die "$add: $!\n";
my $append = do { local $/; <$afh> };

die "tag XML has no closing Tags element\n"
    unless $xml =~ s{\s*</Tags>\s*\z}{\n$append</Tags>\n}s;

open my $ofh, ">:raw", $out or die "$out: $!\n";
print {$ofh} $xml;
EOF
}

verify_result()
{
	before_tracks=$1
	after_tracks=$2
	before_tags=$3
	after_tags=$4
	append_tags=$5

	cmp -s "$before_tracks" "$after_tracks" ||
		die "audio track identity changed"

	perl -0 - "$before_tags" "$after_tags" "$append_tags" <<'EOF'
use strict;
use warnings;
use MIME::Base64 ();

my ($before_xml_file, $after_xml_file, $append_file) = @ARGV;

open my $bfh, "<:raw", $before_xml_file or die "$before_xml_file: $!\n";
open my $afh, "<:raw", $after_xml_file or die "$after_xml_file: $!\n";
open my $pfh, "<:raw", $append_file or die "$append_file: $!\n";
my $before = do { local $/; <$bfh> };
my $after = do { local $/; <$afh> };
my $append = do { local $/; <$pfh> };

my @replaygain_names = qw(
    REPLAYGAIN_GAIN REPLAYGAIN_PEAK EBU_R128_LOUDNESS
    EBU_R128_MAX_TRUE_PEAK EBU_R128_LOUDNESS_RANGE
    EBU_R128_MAX_MOMENTARY_LOUDNESS EBU_R128_MAX_SHORT_LOUDNESS
);

sub is_replaygain_tag {
	my ($tag) = @_;
	for my $name (@replaygain_names) {
		return 1 if $tag =~ m{<Name>\Q$name\E</Name>};
	}
	return 0;
}

sub trim {
	my ($value) = @_;
	$value =~ s/\A\s+//;
	$value =~ s/\s+\z//;
	return $value;
}

sub binary_value {
	my ($attrs, $value) = @_;
	$value =~ s/\s+//g;

	if ($attrs =~ /\bformat\s*=\s*["']hex["']/i) {
		return lc $value;
	}

	return unpack "H*", MIME::Base64::decode_base64($value);
}

sub target_signature {
	my ($tag) = @_;
	die "tag has no Targets element\n"
	    unless $tag =~ m{<Targets>(.*?)</Targets>}s;
	my $targets = $1;
	my @targets;

	while ($targets =~ m{<([A-Za-z0-9_]+)>(.*?)</\1>}sg) {
		my ($name, $value) = ($1, trim($2));
		push @targets, "$name=$value";
	}

	return join "\034", sort @targets;
}

sub simple_values {
	my ($tag) = @_;
	my @values;

	while ($tag =~ m{<Simple>(.*?)</Simple>}sg) {
		my $simple = $1;
		next unless $simple =~ m{<Name>(.*?)</Name>}s;
		my $name = trim($1);

		if ($simple =~ m{<String>(.*?)</String>}s) {
			push @values, "String:$name=" . trim($1);
		} elsif ($simple =~ m{<Binary([^>]*)>(.*?)</Binary>}s) {
			push @values, "Binary:$name=" . binary_value($1, $2);
		}
	}

	return sort @values;
}

sub preservation_signature {
	my ($tag) = @_;
	return join "\035", target_signature($tag), simple_values($tag);
}

my %after_counts;
for my $tag ($after =~ m{<Tag>.*?</Tag>}sg) {
	$after_counts{preservation_signature($tag)}++;
}

for my $tag ($before =~ m{<Tag>.*?</Tag>}sg) {
	my $signature = preservation_signature($tag);
	die "original tag target/value data missing after edit\n"
	    unless $after_counts{$signature};
	$after_counts{$signature}--;
}

sub tag_uid {
	my ($tag) = @_;
	die "new tag has no TrackUID\n"
	    unless $tag =~ m{<TrackUID>([^<]+)</TrackUID>};
	return $1;
}

sub tag_values {
	my ($tag) = @_;
	my %values;

	while ($tag =~ m{<Simple>(.*?)</Simple>}sg) {
		my $simple = $1;
		next unless $simple =~ m{<Name>(.*?)</Name>}s;
		my $name = trim($1);

		if ($simple =~ m{<String>(.*?)</String>}s) {
			$values{$name} = "String:" . trim($1);
			next;
		}
		next unless $simple =~ m{<Binary([^>]*)>(.*?)</Binary>}s;
		$values{$name} = "Binary:" . binary_value($1, $2);
	}

	return %values;
}

my @expected_new = $append =~ m{<Tag>.*?</Tag>}sg;
my @actual_new = grep { is_replaygain_tag($_) } $after =~ m{<Tag>.*?</Tag>}sg;
die "no replaygain tags were appended\n" unless @expected_new;
die "expected " . scalar(@expected_new) . " replaygain tags, found " .
    scalar(@actual_new) . "\n" unless @expected_new == @actual_new;

my %expected_by_uid;
for my $tag (@expected_new) {
	$expected_by_uid{tag_uid($tag)} = $tag;
}

for my $tag (@actual_new) {
	my $uid = tag_uid($tag);
	die "unexpected replaygain tag for TrackUID $uid\n"
	    unless exists $expected_by_uid{$uid};
	die "new tag is not TrackUID-only\n"
	    if $tag =~ m{<(?:ChapterUID|EditionUID|AttachmentUID|TargetTypeValue)>};
	my %actual_values = tag_values($tag);
	my %expected_values = tag_values($expected_by_uid{$uid});

	for my $name (@replaygain_names) {
		my $count = () = $tag =~ m{<Name>\Q$name\E</Name>}g;
		die "new tag has $count instances of $name\n" unless $count == 1;
		die "new tag value differs for $name on TrackUID $uid\n"
		    unless exists $actual_values{$name} &&
		    exists $expected_values{$name} &&
		    $actual_values{$name} eq $expected_values{$name};
	}
}
EOF
}

[ "$#" -eq 1 ] || usage
file=$1
[ -f "$file" ] || die "not a file: $file"

need awk
need base64
need cmp
need cp
need jq
need mktemp
need mv
need perl
need rm
need sed
need ffmpeg
need mkvextract
need mkvmerge
need mkvpropedit

dir=$(dirname -- "$file")
base=$(basename -- "$file")
tmpdir=$(mktemp -d "$dir/.tag_replaygain.XXXXXX")
tmpcopy=$tmpdir/$base
before_json=$tmpdir/audio-before.tsv
after_json=$tmpdir/audio-after.tsv
uids=$tmpdir/audio-uids.txt
before_xml=$tmpdir/tags-before.xml
after_xml=$tmpdir/tags-after.xml
append_xml=$tmpdir/tags-append.xml
expected_xml=$tmpdir/tags-expected.xml

cleanup()
{
	rm -rf "$tmpdir"
}
trap cleanup EXIT HUP INT TERM

mkvmerge -J "$file" |
	jq -r '
		.tracks[]
		| select(.type == "audio")
		| [
		    .id,
		    .properties.uid,
		    .properties.codec_id,
		    (.properties.audio_channels // ""),
		    (.properties.audio_sampling_frequency // "")
		  ]
		| @tsv
	' >"$before_json"

[ -s "$before_json" ] || die "no audio tracks found"
awk -F '\t' '{ print $2 }' "$before_json" >"$uids"

extract_tags "$file" >"$before_xml"
verify_no_existing_tags "$before_xml" "$uids"

: >"$append_xml"
stream=0
while IFS='	' read -r _track_id uid _codec _channels _rate; do
	rg=$(replaygain "$file" "$stream") ||
		die "failed to calculate ReplayGain for audio stream $stream"
	ebu=$(ebur128 "$file" "$stream") ||
		die "failed to calculate EBU R128 for audio stream $stream"

	gain=$(printf '%s\n' "$rg" | awk -F '\t' '{ print $1 }')
	peak=$(printf '%s\n' "$rg" | awk -F '\t' '{ print $2 }')
	integrated=$(printf '%s\n' "$ebu" | awk -F '\t' '{ print $1 }')
	true_peak=$(printf '%s\n' "$ebu" | awk -F '\t' '{ print $2 }')
	lra=$(printf '%s\n' "$ebu" | awk -F '\t' '{ print $3 }')
	max_m=$(printf '%s\n' "$ebu" | awk -F '\t' '{ print $4 }')
	max_s=$(printf '%s\n' "$ebu" | awk -F '\t' '{ print $5 }')

	gain_xml=$(printf '%s dB' "$gain" | xml_escape)
	peak_xml=$(printf '%s' "$peak" | xml_escape)

	{
		printf '  <Tag>\n'
		printf '    <Targets>\n'
		printf '      <TrackUID>%s</TrackUID>\n' "$uid"
		printf '    </Targets>\n'
		printf '    <Simple>\n'
		printf '      <Name>REPLAYGAIN_GAIN</Name>\n'
		printf '      <String>%s</String>\n' "$gain_xml"
		printf '    </Simple>\n'
		printf '    <Simple>\n'
		printf '      <Name>REPLAYGAIN_PEAK</Name>\n'
		printf '      <String>%s</String>\n' "$peak_xml"
		printf '    </Simple>\n'
		printf '    <Simple>\n'
		printf '      <Name>EBU_R128_LOUDNESS</Name>\n'
		printf '      <Binary>%s</Binary>\n' "$(binary_float "$integrated")"
		printf '    </Simple>\n'
		printf '    <Simple>\n'
		printf '      <Name>EBU_R128_MAX_TRUE_PEAK</Name>\n'
		printf '      <Binary>%s</Binary>\n' "$(binary_float "$true_peak")"
		printf '    </Simple>\n'
		printf '    <Simple>\n'
		printf '      <Name>EBU_R128_LOUDNESS_RANGE</Name>\n'
		printf '      <Binary>%s</Binary>\n' "$(binary_float "$lra")"
		printf '    </Simple>\n'
		printf '    <Simple>\n'
		printf '      <Name>EBU_R128_MAX_MOMENTARY_LOUDNESS</Name>\n'
		printf '      <Binary>%s</Binary>\n' "$(binary_float "$max_m")"
		printf '    </Simple>\n'
		printf '    <Simple>\n'
		printf '      <Name>EBU_R128_MAX_SHORT_LOUDNESS</Name>\n'
		printf '      <Binary>%s</Binary>\n' "$(binary_float "$max_s")"
		printf '    </Simple>\n'
		printf '  </Tag>\n'
	} >>"$append_xml"

	stream=$((stream + 1))
done <"$before_json"

append_tags "$before_xml" "$append_xml" "$expected_xml"
cp -p -- "$file" "$tmpcopy"
mkvpropedit "$tmpcopy" --tags all:"$expected_xml" >/dev/null

mkvmerge -J "$tmpcopy" |
	jq -r '
		.tracks[]
		| select(.type == "audio")
		| [
		    .id,
		    .properties.uid,
		    .properties.codec_id,
		    (.properties.audio_channels // ""),
		    (.properties.audio_sampling_frequency // "")
		  ]
		| @tsv
	' >"$after_json"
extract_tags "$tmpcopy" >"$after_xml"

verify_result "$before_json" "$after_json" "$before_xml" "$after_xml" \
	"$append_xml" "$expected_xml"

mv -- "$tmpcopy" "$file"
trap - EXIT HUP INT TERM
cleanup
