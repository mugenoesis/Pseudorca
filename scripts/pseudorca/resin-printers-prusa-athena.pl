#!/usr/bin/env perl
# Adds the resin printers the first catalogue left out, from two open sources:
#
# - Prusa's SL1 and SL1S SPEED. UVtools' PrusaSlicer printer folder (github.com/sn4k3/UVtools, AGPL-3.0) has them as "UVtools Prusa SL1" and
#   "UVtools Prusa SL1S SPEED", the profiles its others inherit from; resin-catalogue-from-uvtools.pl skips them for that reason, but they are
#   the real printers. They are checked against Encrust's profile library (github.com/vvvlladimir/Encrust, AGPL-3.0), which gives the same screens.
#   Both write the SL1 archive, which is what Prusa's printers read.
# - Concepts3D Athena II 12K and 16K, from the Open Resin Alliance's DragonFruit athena plugin (github.com/Open-Resin-Alliance/DragonFruit,
#   AGPL-3.0): resolution and pixel size (19 x 24 um and 14 x 19 um, so the pixels are not square) and the build height. They print through
#   NanoDLP, which has no writer here yet, so they are listed as printers that wait for one.
#
# usage: resin-printers-prusa-athena.pl <UVtools PrusaSlicer/printer dir> <DragonFruit checkout> <resources/resin>
use strict;
use warnings;
use JSON::PP;
use File::Path qw(make_path);

my ($uvtools, $dragonfruit, $dir) = @ARGV;
die "usage: $0 <uvtools printer dir> <dragonfruit dir> <resources/resin>\n" unless $uvtools && $dragonfruit && $dir;
sub slurp { my ($f) = @_; open(my $fh, '<', $f) or die "$f: $!"; local $/; return <$fh> }
sub norm { my ($s) = @_; $s = lc $s; $s =~ s/[^a-z0-9]+//g; return $s }

my $index = decode_json(slurp("$dir/index.json"));
my %have = map { norm("$_->{vendor} $_->{model}") => 1 } @{ $index->{printers} };
my @added;

sub add_printer {
    my ($vendor, $model, $m, $format, $supported, $nonsquare) = @_;
    return if $have{ norm("$vendor $model") };
    make_path("$dir/printers/$vendor");
    my @order = qw(type from name printer_technology printer_model);
    my %seen = map { $_ => 1 } @order;
    push @order, grep { !$seen{$_} } sort keys %$m;
    open(my $o, '>', "$dir/printers/$vendor/$model.json") or die "$vendor/$model: $!";
    print $o "{\n", join(",\n", map { "  \"$_\": \"$m->{$_}\"" } grep { defined $m->{$_} } @order), "\n}\n";
    close $o;
    push @{ $index->{printers} }, { vendor => $vendor, model => $model, file => "printers/$vendor/$model.json", format => $format,
                                    format_version => undef, supported => $supported, nonsquare => $nonsquare };
    push @added, "$vendor $model";
}

# Prusa
for my $p (['SL1', 'UVtools Prusa SL1.ini'], ['SL1S SPEED', 'UVtools Prusa SL1S SPEED.ini']) {
    my ($model, $file) = @$p;
    my %kv;
    for (split /\n/, slurp("$uvtools/$file")) { $kv{$1} = $2 if /^(\w+) = (.*)$/ }
    my %m = (
        type => 'machine', from => 'system', name => "Prusa $model", printer_technology => 'SLA', printer_model => $model,
        printer_settings_id => "Prusa $model", display_width => $kv{display_width}, display_height => $kv{display_height},
        display_pixels_x => $kv{display_pixels_x}, display_pixels_y => $kv{display_pixels_y}, display_orientation => $kv{display_orientation},
        display_mirror_x => $kv{display_mirror_x}, display_mirror_y => $kv{display_mirror_y}, printable_height => $kv{max_print_height},
        gamma_correction => $kv{gamma_correction} // '1', sla_archive_format => 'SL1',
    );
    add_printer('Prusa', $model, \%m, 'SL1', 1, 0);
}

# Concepts3D Athena II
my $athena = decode_json(slurp("$dragonfruit/plugins/athena/printers/printers.json"));
for my $o (@$athena) {
    next unless $o->{manufacturer} eq 'Concepts3D' && $o->{name} =~ /^Athena II (12K|16K)$/;
    my ($px, $py) = ($o->{pixelSize}{x}, $o->{pixelSize}{y});
    my ($rx, $ry) = ($o->{display}{resolutionX}, $o->{display}{resolutionY});
    my %m = (
        type => 'machine', from => 'system', name => "Concepts3D $o->{name}", printer_technology => 'SLA', printer_model => $o->{name},
        printer_settings_id => "Concepts3D $o->{name}", display_width => sprintf('%.3f', $rx * $px / 1000) + 0,
        display_height => sprintf('%.3f', $ry * $py / 1000) + 0, display_pixels_x => $rx, display_pixels_y => $ry,
        display_orientation => 'landscape', display_mirror_x => 0, display_mirror_y => 0, printable_height => $o->{buildVolumeMm}{height} + 0,
        gamma_correction => 1, sla_archive_format => 'nanodlp',
    );
    $m{$_} = "$m{$_}" for keys %m;
    add_printer('Concepts3D', $o->{name}, \%m, 'nanodlp', 0, abs($px / $py - 1) > 0.05 ? 1 : 0);
}

my @sorted = sort { "$a->{vendor} $a->{model}" cmp "$b->{vendor} $b->{model}" } @{ $index->{printers} };
sub flag { return $_[0] ? 'true' : 'false' }
sub entry {
    my $e = shift;
    return sprintf('{ "vendor": "%s", "model": "%s", "file": "%s", "format": "%s", "format_version": %s, "supported": %s, "nonsquare_pixels": %s }',
        $e->{vendor}, $e->{model}, $e->{file}, $e->{format}, defined $e->{format_version} ? "\"$e->{format_version}\"" : 'null',
        flag($e->{supported}), flag($e->{nonsquare} // $e->{nonsquare_pixels}));
}
open(my $out, '>', "$dir/index.json") or die;
print $out "{\n  \"printers\": [\n", join(",\n", map { "    " . entry($_) } @sorted), "\n  ]\n}\n";
close $out;
print "printers added: ", join(', ', @added), "\n";
