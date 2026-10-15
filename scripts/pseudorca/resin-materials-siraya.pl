#!/usr/bin/env perl
# Builds resources/resin/materials/Siraya Tech from the exposure tables Siraya Tech publishes in its resin user guides
# (https://siraya.tech/pages/<name>-user-guide). The guides list the settings of the Chitubox profiles the company ships for
# each printer; the numbers are the maker's published recommendations and every settings row links back to its page.
#
# usage: resin-materials-siraya.pl <dir with the saved guide pages as s_<name>.html> <output dir> <retrieved date>
use strict;
use warnings;
use JSON::PP;
use File::Path qw(make_path);

my ($src, $dst, $date) = @ARGV;
die "usage: $0 <pages dir> <output dir> <date>\n" unless $src && $dst && $date;

# How Siraya Tech names a printer -> the names in resources/resin/printers (vendor and model). Printers the catalogue has no
# entry for (Mars Pro, Phenom Prime, E6, E10, X156, SL1, ...) are left out.
my %printer = (
    'Mars' => ['Elegoo Mars'], 'Elegoo Mars' => ['Elegoo Mars'], 'Mars 2' => ['Elegoo Mars 2'],
    'Mars 2 Pro' => ['Elegoo Mars 2 Pro'], 'Mars 3' => ['Elegoo Mars 3'], 'Saturn' => ['Elegoo Saturn'],
    'Photon' => ['Anycubic Photon'], 'Photon S' => ['Anycubic Photon S'], 'Photon-S' => ['Anycubic Photon S'],
    'Mono' => ['Anycubic Photon Mono'], 'Photon Mono' => ['Anycubic Photon Mono'],
    'Mono SE' => ['Anycubic Photon Mono SE'], 'Mono X' => ['Anycubic Photon Mono X'], 'Photon Mono X' => ['Anycubic Photon Mono X'],
    'Sonic Mini 4K' => ['Phrozen Sonic Mini 4K'], 'Might 4K' => ['Phrozen Sonic Mighty 4K'], 'Mega 8K' => ['Phrozen Sonic Mega 8K'],
    'Sonic Mini' => ['Phrozen Sonic Mini'], 'Shuffle' => ['Phrozen Shuffle'], 'Shuffle XL' => ['Phrozen Shuffle XL'],
    'Phrozen Transform' => ['Phrozen Transform'], 'Transform' => ['Phrozen Transform'],
    'Peopoly Phenom' => ['Peopoly Phenom'], 'Phenom Noir' => ['Peopoly Phenom Noir'],
    'X1' => ['EPAX X1'], 'EPA X1' => ['EPAX X1'], 'X10' => ['EPAX X10'],
    'LD-002H' => ['Creality LD-002H'], 'LD-002R' => ['Creality LD-002R'],
    'Halot One' => ['Creality Halot One CL-60'], 'Halot Sky' => ['Creality Halot Sky CL-89'],
    'D7' => ['Wanhao D7'], 'Inkspire' => ['Zortrax Inkspire'],
);

# The resins and where their tables are: 'B' tables list one printer per column with every setting, 'A' tables one printer per row.
my @resins = (
    { name => 'Siraya Tech Easy Grey',           type => 'plant-based', page => 'easy-user-guide', kind => 'B', tables => [1, 2] },
    { name => 'Siraya Tech Easy Obsidian Black', type => 'plant-based', page => 'easy-user-guide', kind => 'B', tables => [3, 4, 5] },
    { name => 'Siraya Tech Fast Navy Grey',      type => 'abs-like',    page => 'fast-navy-grey-user-guide', kind => 'B', tables => [3, 4, 6, 7, 8] },
    { name => 'Siraya Tech Fast Navy Grey',      type => 'abs-like',    page => 'fast-navy-grey-user-guide', kind => 'A', tables => [2] },
    { name => 'Siraya Tech Build',               type => 'engineering', page => 'build-user-guide', kind => 'A', tables => [1] },
    { name => 'Siraya Tech Blu and Blu Clear V2', type => 'tough',      page => 'blu-user-guide', kind => 'A', tables => [1] },
    { name => 'Siraya Tech Blu Obsidian Black and Blu V3', type => 'tough', page => 'blu-user-guide', kind => 'A', tables => [3] },
    { name => 'Siraya Tech Tenacious Clear',     type => 'flexible',    page => 'tenacious-user-guide', kind => 'A', tables => [1] },
    { name => 'Siraya Tech Tenacious Obsidian Black', type => 'flexible', page => 'tenacious-user-guide', kind => 'A', tables => [2] },
    { name => 'Siraya Tech Sculpt Grey',         type => 'high-temp',   page => 'sculpt-user-guide', kind => 'A', tables => [2] },
    { name => 'Siraya Tech Sculpt Clear',        type => 'high-temp',   page => 'sculpt-user-guide', kind => 'A', tables => [3] },
);

my %b_keys = (
    'Layer height (mm)' => 'layer_height', 'Bottom layer count' => 'bottom_layer_count', 'Exposure time (s)' => 'exposure_time',
    'Bottom exposure time (s)' => 'initial_exposure_time', 'Rest Time After Retract (s)' => 'delay_before_exposure',
    'Lift Distance (mm)' => 'lift_distance', 'Lift Speed (mm/min)' => 'lift_speed', 'Retract Speed (mm/min)' => 'retract_speed',
    'Transition Layer Count' => 'faded_layers',
);

sub tables_of {
    my ($file) = @_;
    open(my $fh, '<', $file) or die "$file: $!";
    local $/;
    my $h = <$fh>;
    my @tables;
    while ($h =~ /(<table.*?<\/table>)/sg) {
        my $t = $1; my @rows;
        while ($t =~ /<tr[^>]*>(.*?)<\/tr>/sg) {
            my $r = $1; my @c;
            while ($r =~ /<t[dh][^>]*>(.*?)<\/t[dh]>/sg) {
                my $c = $1; $c =~ s/<[^>]*>/ /g; $c =~ s/&nbsp;/ /g; $c =~ s/&amp;/&/g; $c =~ s/\s+/ /g; $c =~ s/^ | $//g;
                push @c, $c;
            }
            push @rows, \@c;
        }
        push @tables, \@rows;
    }
    return @tables;
}

sub number { my ($s) = @_; return $s =~ /^\s*([0-9]+(?:\.[0-9]+)?)\s*(?:\(\s*s\s*\)|s|S)?\s*$/ ? $1 + 0 : undef }

my %out;   # resin name -> { meta, rows => [] }
my %tables_cache;
for my $r (@resins) {
    my $page = $r->{page};
    my $url = "https://siraya.tech/pages/$page";
    my @tables = @{ $tables_cache{$page} //= [ tables_of("$src/s_$page.html") ] };
    my $res = $out{ $r->{name} } //= { type => $r->{type}, rows => [] };
    for my $ti (@{ $r->{tables} }) {
        my @rows = @{ $tables[$ti - 1] // [] };
        if ($r->{kind} eq 'B') {
            my ($printer_row) = grep { ($_->[0] // '') eq 'Printer' } @rows;
            next unless $printer_row;
            my @labels = @$printer_row[1 .. $#$printer_row];
            my @col = map { {} } @labels;
            for my $row (@rows) {
                my $key = $b_keys{ $row->[0] // '' } or next;
                for my $j (0 .. $#labels) { my $v = number($row->[$j + 1] // ''); $col[$j]{$key} = $v if defined $v }
            }
            for my $j (0 .. $#labels) {
                my $names = $printer{ $labels[$j] } or next;
                my %s = %{ $col[$j] };
                next unless defined $s{layer_height} && defined $s{exposure_time} && defined $s{initial_exposure_time};
                $s{bottom_layer_count} = int($s{bottom_layer_count}) if defined $s{bottom_layer_count};
                $s{faded_layers} = int($s{faded_layers}) if defined $s{faded_layers};
                push @{ $res->{rows} }, { printers => [@$names], %s, source => $url, retrieved => $date,
                                          notes => "Chitubox profile values in Siraya Tech's guide" };
            }
        } else {
            for my $row (@rows) {
                my ($label, $layer, @v) = @$row;
                next unless defined $label && $printer{$label} && defined $layer && $layer =~ /^\s*(\d+)\s*um\s*$/i;
                my $lh = $1 / 1000;
                # room temperature group: exposure, number of bottom layers, bottom exposure
                my ($e, $n, $i) = (number($v[3] // ''), number($v[4] // ''), number($v[5] // ''));
                next unless defined $e && defined $n && defined $i;
                push @{ $res->{rows} }, { printers => [@{ $printer{$label} }], layer_height => $lh, exposure_time => $e,
                    bottom_layer_count => int($n), initial_exposure_time => $i, source => $url, retrieved => $date,
                    notes => "Recommended exposure at room temperature in Siraya Tech's guide" };
            }
        }
    }
}

my $json = JSON::PP->new->pretty->canonical->indent_length(2);
make_path("$dst/Siraya Tech");
my @index;
for my $name (sort keys %out) {
    my $res = $out{$name};
    next unless @{ $res->{rows} };
    # the same printer and layer height can come from two tables; the first one wins
    my (%seen, @rows);
    for my $row (@{ $res->{rows} }) {
        my $key = join('|', @{ $row->{printers} }, $row->{layer_height});
        push @rows, $row unless $seen{$key}++;
    }
    (my $file = $name) =~ s/^Siraya Tech //;
    my $material = { type => 'material', name => $name, vendor => 'Siraya Tech', resin_type => $res->{type}, settings => \@rows };
    open(my $o, '>', "$dst/Siraya Tech/$file.json") or die "$file: $!";
    print $o $json->encode($material);
    close $o;
    push @index, { name => $name, vendor => 'Siraya Tech', resin_type => $res->{type}, file => "materials/Siraya Tech/$file.json", rows => scalar @rows };
}
open(my $o, '>', "$dst/materials-index.json") or die;
print $o $json->encode({ materials => \@index });
close $o;
print scalar(@index), " resins\n";
