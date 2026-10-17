#!/usr/bin/env perl
# Builds resources/resin/materials/Elegoo from the spreadsheet of resin setting suggestions Elegoo publishes
# (https://www.elegoo.com/blogs/3d-printer-user-guide/official-suggestions-for-elegoo-resin-on-elegoo-printers): one row for each
# printer, resin, color and layer height. Elegoo gives bottom and normal exposure as ranges (25-30 s); the middle of the
# range is used and the row says so. A lift in two stages ("3+4" mm at "65+180" mm/min) becomes one lift of the total distance at the
# one speed that takes as long. Rows for the fast printing modes of the Mars 4 Ultra and Saturn 3 Ultra are left out.
#
# usage: resin-materials-elegoo.pl <the unzipped .xlsx> <output dir> <retrieved date>
use strict;
use warnings;
use JSON::PP;
use File::Path qw(make_path);

my ($xl, $dst, $date) = @ARGV;
die "usage: $0 <unzipped xlsx dir> <output dir> <date>\n" unless $xl && $dst && $date;
my $url = 'https://www.elegoo.com/blogs/3d-printer-user-guide/official-suggestions-for-elegoo-resin-on-elegoo-printers';

# Elegoo's machine type -> the printers of resources/resin/printers
my %printer = (
    'Jupiter' => ['Elegoo Jupiter'], 'Jupiter 2' => ['Elegoo Jupiter 2'], 'Mars' => ['Elegoo Mars'],
    'Mars 2 Pro/Mars 2' => ['Elegoo Mars 2', 'Elegoo Mars 2 Pro'], 'Mars 3 / Mars 3 Pro' => ['Elegoo Mars 3'],
    'Mars 4' => ['Elegoo Mars 4'], 'Mars 4 MAX' => ['Elegoo Mars 4 Max'], 'Mars 4 DLP' => ['Elegoo Mars 4 DLP'],
    'Mars 4 Ultra（normal resin+normal speed）' => ['Elegoo Mars 4 Ultra'], 'Mars 5' => ['Elegoo Mars 5'],
    'Mars 5 Ultra' => ['Elegoo Mars 5 Ultra'], 'Saturn / Saturn S' => ['Elegoo Saturn', 'Elegoo Saturn S'],
    'Saturn 2' => ['Elegoo Saturn 2'], 'Saturn 3' => ['Elegoo Saturn 3'],
    'Saturn 3 Ultra（normal resin+normal speed）' => ['Elegoo Saturn 3 Ultra'], 'Saturn 4' => ['Elegoo Saturn 4'],
    'Saturn 4 Ultra' => ['Elegoo Saturn 4 Ultra 12K'], 'Saturn 4 Ultra 16K' => ['Elegoo Saturn 4 Ultra 16K'],
    'Saturn 8K' => ['Elegoo Saturn 8K'],
);

sub resin_type {
    my $n = lc shift;
    return 'water-washable' if $n =~ /water wash/ && $n !~ /abs/;
    return 'abs-like' if $n =~ /abs-like/;
    return 'tough' if $n =~ /tough/;
    return 'plant-based' if $n =~ /plant/;
    return 'dental' if $n =~ /dental/;
    return 'standard' if $n =~ /standard/;
    return '';
}

# --- the spreadsheet
local $/;
open(my $s, '<', "$xl/xl/sharedStrings.xml") or die "sharedStrings: $!"; my $ss = <$s>; close $s;
my @str;
while ($ss =~ /<si>(.*?)<\/si>/sg) {
    my $t = join('', $1 =~ /<t[^>]*>(.*?)<\/t>/sg);
    $t =~ s/&amp;/&/g; $t =~ s/&lt;/</g; $t =~ s/&gt;/>/g; $t =~ s/&quot;/"/g; $t =~ s/&#10;/ /g;
    push @str, $t;
}
open(my $f, '<', "$xl/xl/worksheets/sheet1.xml") or die "sheet1: $!"; my $x = <$f>; close $f;
my @rows;
while ($x =~ /<row [^>]*r="(\d+)"[^>]*>(.*?)<\/row>/sg) {
    my ($r, $cells) = ($1, $2); my %c;
    while ($cells =~ /<c r="([A-Z]+)\d+"([^>]*?)(?:\/>|>(.*?)<\/c>)/sg) {
        my ($col, $attr, $body) = ($1, $2, $3 // '');
        my ($v) = $body =~ /<v>(.*?)<\/v>/s; next unless defined $v;
        $v = $str[$v] if $attr =~ /t="s"/;
        $v =~ s/^\s+|\s+$//g; $v =~ s/\s+/ /g;
        $c{$col} = $v;
    }
    push @rows, \%c if $r > 2 && keys %c;
}

sub range_mid {   # "25-30" -> 27.5, "2.5" -> 2.5
    my ($v) = @_;
    return undef unless defined $v;
    return ($1 + $2) / 2 if $v =~ /^(\d+(?:\.\d+)?)-(\d+(?:\.\d+)?)$/;
    return $v + 0 if $v =~ /^\d+(?:\.\d+)?$/;
    return undef;
}
sub stages {   # "3+4" -> (3, 4); "5" -> (5)
    my ($v) = @_;
    return () unless defined $v && $v =~ /^\d+(?:\.\d+)?(?:\+\d+(?:\.\d+)?)*$/;
    return split /\+/, $v;
}
# The one speed that takes as long as the stages do: the distances over the time of each stage. A stage of speed 0 is not there.
sub one_speed {
    my ($dist, $speed, $reverse) = @_;
    my @d = stages($dist); my @s = stages($speed);
    return undef unless @d && @s;
    @d = reverse @d if $reverse;
    my ($total, $time) = (0, 0);
    for my $i (0 .. $#s) {
        next unless $s[$i] > 0;
        my $di = $d[$i] // ($i == 0 ? $d[0] : 0);
        $total += $di; $time += $di / $s[$i];
    }
    return undef unless $time > 0;
    return sprintf("%.1f", $total / $time) + 0;
}

my %materials;
for my $r (@rows) {
    my $machine = $r->{A} // next;
    my $names = $printer{$machine} or next;
    next if ($r->{C} // '') =~ /Fast Speed/i;
    my $resin = $r->{B} // next;
    $resin =~ s/\s+/ /g; $resin =~ s/ \+$/+/;     # "ABS-Like Resin 3.0 +" -> "ABS-Like Resin 3.0+"
    my $color = $r->{D} // '';
    my $e = range_mid($r->{F}); my $b = range_mid($r->{E});
    my $lh = $r->{G};
    next unless defined $e && defined $b && defined $lh && $lh =~ /^\d+(?:\.\d+)?$/;
    my %row = (printers => [@$names], layer_height => $lh + 0, exposure_time => sprintf("%.2f", $e) + 0,
               initial_exposure_time => sprintf("%.2f", $b) + 0, source => $url, retrieved => $date);
    my @notes = ("Elegoo's spreadsheet of resin setting suggestions");
    push @notes, "exposure ranges given as their middle" if ($r->{E} // '') =~ /-/ || ($r->{F} // '') =~ /-/;
    my $lift = 0; $lift += $_ for stages($r->{H});
    my $ls = one_speed($r->{H}, $r->{I}, 0); my $rs = one_speed($r->{H}, $r->{J}, 1);
    if ($lift > 0 && defined $ls && defined $rs) {
        $row{lift_distance} = $lift; $row{lift_speed} = $ls; $row{retract_speed} = $rs;
        push @notes, "a two-stage lift is given as its total distance and the speed that takes as long" if ($r->{H} // '') =~ /\+/;
    }
    $row{delay_before_exposure} = $r->{K} + 0 if defined $r->{K} && $r->{K} =~ /^\d+(?:\.\d+)?$/;
    $row{notes} = join('; ', @notes);
    my $name = "Elegoo $resin" . ($color ne '' ? " $color" : '');
    my $m = $materials{$name} //= { resin => $resin, color => $color, rows => [], seen => {} };
    my $key = join('|', @$names, $row{layer_height});
    push @{ $m->{rows} }, \%row unless $m->{seen}{$key}++;
}

my $json = JSON::PP->new->pretty->canonical->indent_length(2);
make_path("$dst/Elegoo");
my $count = 0;
for my $name (sort keys %materials) {
    my $m = $materials{$name};
    next unless @{ $m->{rows} };
    (my $file = $name) =~ s/^Elegoo //;
    $file =~ s/\+/ Plus/g; $file =~ s/[^A-Za-z0-9 .-]/ /g; $file =~ s/\s+/ /g; $file =~ s/^ | $//g;
    my $out = { type => 'material', name => $name, vendor => 'Elegoo', family => $m->{resin}, settings => $m->{rows} };
    $out->{color} = $m->{color} if $m->{color} ne '';
    my $t = resin_type($m->{resin});
    $out->{resin_type} = $t if $t;
    die "two materials write $file\n" if -e "$dst/Elegoo/$file.json";
    open(my $o, '>', "$dst/Elegoo/$file.json") or die "$file: $!";
    print $o $json->encode($out);
    close $o;
    $count++;
}
print "$count materials\n";
