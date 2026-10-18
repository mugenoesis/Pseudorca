#!/usr/bin/env perl
# Adds the Siraya Tech settings the Open Resin Alliance ported to its DragonFruit slicer to resources/resin/materials/Siraya Tech.
#
# The plugin (https://github.com/Open-Resin-Alliance/df-plugin-sirayatech, MIT, "co-authored by Siraya Tech and the Open Resin Alliance,
# and officially supported by Siraya Tech") holds Siraya Tech's own 50 um Chitubox profiles, which it downloaded from
# https://siraya.tech/pages/print-settings-download, for the Anycubic, Elegoo and UniFormation printers: one file per printer
# (materials/<vendor>/<printer>.json) with a preset per resin. This adds those rows to the material files, creating a resin's file when
# the library did not have it. A resin and printer the library already has a row for at that layer height keeps the row it had.
#
# usage: resin-materials-siraya-ora.pl <checkout of df-plugin-sirayatech> <resources/resin> <retrieved date>
use strict;
use warnings;
use JSON::PP;
use File::Path qw(make_path);

my ($plugin, $resin_dir, $date) = @ARGV;
die "usage: $0 <df-plugin-sirayatech> <resources/resin> <date>\n" unless $plugin && $resin_dir && $date;
my $json = JSON::PP->new->pretty->canonical->indent_length(2)->space_before(1)->space_after(1);

sub slurp { my ($f) = @_; open(my $fh, '<', $f) or die "$f: $!"; local $/; return decode_json(<$fh>) }
sub norm { my ($s) = @_; $s = lc $s; $s =~ s/[^a-z0-9]+//g; return $s }

# Our printers, by the plugin's way of naming them: the file stem with dashes as spaces against "<vendor> <model>".
my %ours;
for my $p (@{ slurp("$resin_dir/index.json")->{printers} }) { $ours{ norm("$p->{vendor} $p->{model}") } = "$p->{vendor} $p->{model}" }
my %vendor = (anycubic => 'Anycubic', elegoo => 'Elegoo', uniformation => 'UniFormation');

# The resins the library already has (by name without the maker), so a preset lands in the file it belongs to. The plugin names a resin
# for every colour it covers; where the library named a table differently the two are matched by hand.
my $dir = "$resin_dir/materials/Siraya Tech";
make_path($dir);
my %file_of;
for my $f (glob("'$dir/*.json'")) {
    my $m = slurp($f);
    (my $short = $m->{name}) =~ s/^Siraya Tech //;
    $file_of{ norm($short) } = $f;
}
my %alias = (
    'bluclearv2emeraldblue'     => 'bluandbluclearv2',
    'bluobsidianblackv3'        => 'bluobsidianblackandbluv3',
    'buildsonicgreysmokyblack'  => 'build',
    'sculptdarkgrey'            => 'sculptgrey',
);

# Anything that is not one of the preset's own settings is not carried over (the plugin's bottle price is a default, not Siraya Tech's).
sub row_of {
    my ($p, $out, $printer) = @_;
    my $s = $p->{localSettingsByOutput}{$out} // return;
    my %r = (
        printers => [$printer], layer_height => $s->{layerHeightMm} + 0,
        exposure_time => $s->{normalExposureSec} + 0, initial_exposure_time => $s->{bottomExposureSec} + 0,
        bottom_layer_count => $s->{bottomLayerCount} + 0, faded_layers => ($s->{transitionLayerCount} // 0) + 0,
        delay_before_exposure => ($s->{waitTimeBeforeCureSec} // 0) + 0,
        notes => "Siraya Tech's Chitubox 50 um profile, ported by the Open Resin Alliance",
        source => 'https://github.com/Open-Resin-Alliance/df-plugin-sirayatech (MIT), from https://siraya.tech/pages/print-settings-download',
        retrieved => $date,
    );
    # a two-stage lift or retract is its total distance at the speed that takes as long
    for my $pair ([lift => 'lift'], [retract => 'retract']) {
        my ($name) = @$pair;
        my $d1 = $s->{ $name eq 'lift' ? 'liftDistanceMm' : 'retractDistanceMm' };
        my $v1 = $s->{ $name eq 'lift' ? 'liftSpeedMmMin' : 'retractSpeedMmMin' };
        my $d2 = $s->{ $name eq 'lift' ? 'liftDistance2Mm' : 'retractDistance2Mm' } // 0;
        my $v2 = $s->{ $name eq 'lift' ? 'liftSpeed2MmMin' : 'retractSpeed2MmMin' } // 0;
        if ($name eq 'lift') {
            next unless defined $d1 && defined $v1;
            if ($d2 > 0 && $v2 > 0) { my $t = $d1 / $v1 + $d2 / $v2; $r{lift_distance} = $d1 + $d2; $r{lift_speed} = sprintf('%.0f', ($d1 + $d2) / $t) + 0 }
            else { $r{lift_distance} = $d1 + 0; $r{lift_speed} = $v1 + 0 }
        } else {
            $r{retract_speed} = $v1 + 0 if defined $v1;
        }
    }
    my $pwm = $s->{projectorPwmPercent};
    $r{light_pwm} = $pwm + 0 if defined $pwm && $pwm != 100;
    return \%r;
}

# The plugin names a preset after the printer's profile for the other printers, with the layer height or a hint about supports after a dash;
# the resin is the part before it.
sub clean_name {
    my ($n) = @_;
    $n =~ s/\s*-\s*\(?\s*use\s+extra\s+heavy\s+support.*$//i;
    $n =~ s/\s*-\s*50\s*um.*$//i;
    $n =~ s/\s*\*\d+$//;
    $n =~ s/\s*-\s*$//;
    return $n;
}
$alias{simplegreyclearsmokyblack} = 'simpleclearg' . 'reysmokyblack'; # the same resin, its colours listed in another order

my (%unmapped, $added, $kept, %new_files);
for my $v (sort keys %vendor) {
    my $vdir = "$plugin/materials/$v";
    for my $f (sort glob("'$vdir/*.json'")) {
        (my $stem = $f) =~ s{.*/}{}; $stem =~ s/\.json$//;
        my $printer = $ours{ norm("$vendor{$v} $stem") };
        unless ($printer) { $unmapped{"$vendor{$v} $stem"} = 1; next }
        my @presets = sort { ($a->{name} =~ /-/ ? 1 : 0) <=> ($b->{name} =~ /-/ ? 1 : 0) } @{ slurp($f) };
        for my $p (@presets) {
            next if $p->{name} =~ /^Standard Resin/i;
            my $resin = clean_name($p->{name});
            my ($out) = sort keys %{ $p->{localSettingsByOutput} };
            my $row = row_of($p, $out, $printer) or next;
            my $key = norm($resin);
            $key = $alias{$key} if $alias{$key};
            my $file = $file_of{$key};
            my $m;
            if ($file) { $m = slurp($file) }
            else {
                (my $base = $resin) =~ s{\s*/\s*}{, }g; $base =~ s/[()]//g; $base =~ s/\s+/ /g;
                $file = "$dir/$base.json"; $file_of{$key} = $file;
                my $family = $p->{resinFamily} // ''; $family = '' if $family eq 'other';
                $m = { type => 'material', vendor => 'Siraya Tech', name => "Siraya Tech $base", resin_type => $family, settings => [] };
                $new_files{$file} = 1;
            }
            my $dup = grep { abs($_->{layer_height} - $row->{layer_height}) < 1e-6 && grep { $_ eq $printer } @{ $_->{printers} } } @{ $m->{settings} };
            if ($dup) { $kept++; next }
            push @{ $m->{settings} }, $row; $added++;
            $m->{_touched} = 1;
            open(my $o, '>', $file) or die "$file: $!";
            delete $m->{_touched};
            print $o $json->encode($m);
            close $o;
        }
    }
}
print "rows added: $added; rows the library already had: $kept; resins created: ", scalar(keys %new_files), "\n";
print "printers the library has no entry for: ", join(', ', sort keys %unmapped), "\n";
