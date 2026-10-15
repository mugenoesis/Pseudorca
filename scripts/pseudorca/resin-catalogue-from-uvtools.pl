#!/usr/bin/env perl
# Builds resources/resin from UVtools' PrusaSlicer printer profiles (github.com/sn4k3/UVtools, PrusaSlicer/printer/*.ini,
# AGPL-3.0). One machine preset per printer (Orca JSON, loadable with --load-settings) plus index.json, which carries what
# a machine preset cannot: the vendor, the file format the printer wants and whether we can write it yet.
#
# usage: resin-catalogue-from-uvtools.pl <dir with the .ini files> <resources/resin>
use strict;
use warnings;
use File::Path qw(make_path);

my ($src, $dst) = @ARGV;
die "usage: $0 <ini dir> <output dir>\n" unless $src && $dst;

# FILEFORMAT_ tag of the printer notes -> the id of the archive writer and whether it exists.
my %writer = (
    'CTB' => 'CTB', 'ZIP' => 'ZIP', 'CXDLP' => 'CXDLP', 'SVGX' => 'SVGX', 'GKTWO.CTB' => 'CTBGKTWO', 'ENCRYPTED.CTB' => 'CTBENCRYPTED', 'GOO' => 'GOO', 'PHOTON' => 'CBDDLP',
    'PWMO' => 'pwmo', 'PWMX' => 'pwmx', 'PWMS' => 'pwms', 'PHROZENZIP' => 'PHROZENZIP', 'CHITUBOXZIP' => 'CHITUBOXZIP',
);
my %supported = map { $_ => 1 } values %writer;
$supported{TINYMAKER} = 1;
$supported{$_} = 1 for qw(pw0 pwx pwmo pwmx pwms pmsq dlp pwma pm3 pm3m pwmb dl2p pmx2 pm3r pm3n pm4n pm5 px6s pm5s m5sp);

sub num { my $v = shift; return defined $v ? $v + 0 : undef }

my @index;
opendir(my $dh, $src) or die "$src: $!";
for my $file (sort grep { /\.ini$/ } readdir $dh) {
    (my $name = $file) =~ s/\.ini$//;
    next if $name =~ /^UVtools /;                       # the Prusa SL1 profiles UVtools uses as bases
    open(my $fh, '<', "$src/$file") or die "$file: $!";
    my %kv;
    while (<$fh>) { chomp; $kv{$1} = $2 if /^(\w+) = (.*)$/ }
    close $fh;
    next unless ($kv{printer_technology} // '') eq 'SLA';

    my $notes = $kv{printer_notes} // '';
    $notes =~ s/\\n/\n/g;
    my ($tag) = $notes =~ /^FILEFORMAT_(\S+)/m;
    my ($version) = $notes =~ /^FILEVERSION_(\S+)/m;
    my %cv;
    if ($notes =~ /START_CUSTOM_VALUES\n(.*?)END_CUSTOM_VALUES/s) {
        for my $l (split /\n/, $1) { $cv{$1} = $2 if $l =~ /^(\w+)_(-?[\d.]+)$/ }
    }
    my ($vendor, $model) = split / /, $name, 2;
    $model //= $vendor;
    my $id = $writer{$tag // ''} // lc(($tag // 'none') =~ s/\.//gr);
    $id = $vendor eq 'Phrozen' ? 'PHROZENZIP' : $name =~ /Klipper/ ? 'klipperzip' : 'CHITUBOXZIP' if $id eq 'ZIP';
    my $ok = $supported{$id} ? 1 : 0;

    my %m = (
        type => 'machine', from => 'system', name => $name,
        printer_technology => 'SLA', printer_model => $model,
        display_width => $kv{display_width}, display_height => $kv{display_height},
        display_pixels_x => $kv{display_pixels_x}, display_pixels_y => $kv{display_pixels_y},
        display_orientation => $kv{display_orientation}, display_mirror_x => $kv{display_mirror_x},
        display_mirror_y => $kv{display_mirror_y}, printable_height => $kv{max_print_height},
        gamma_correction => $kv{gamma_correction} // '1',
        sla_archive_format => $id,
    );
    my %map = (lift_distance => 'LiftHeight', lift_speed => 'LiftSpeed', retract_speed => 'RetractSpeed',
               light_pwm => 'LightPWM', delay_before_exposure => 'WaitTimeBeforeCure');
    for my $k (sort keys %map) { $m{$k} = $cv{$map{$k}} if defined $cv{$map{$k}} }
    my $after = $cv{WaitTimeAfterCure} // $cv{LightOffDelay};
    $m{delay_after_exposure} = $after if defined $after;

    my $ratio = ($m{display_width} / $m{display_pixels_x}) / ($m{display_height} / $m{display_pixels_y});
    my $nonsquare = abs($ratio - 1) > 0.05 ? 1 : 0;
    write_machine($vendor, $model, \%m);
    push @index, { vendor => $vendor, model => $model, file => "printers/$vendor/$model.json",
                   format => $id, format_version => $version, supported => $ok ? \1 : \0, nonsquare => $nonsquare ? \1 : \0 };
}
closedir $dh;

# Not in UVtools' list: the open source TinyMaker; its numbers come from the firmware and the owner. The screen's
# physical size is the build volume the owner measured (30.6 x 40.8 x 60 mm over a 320 x 240 pixel panel).
write_machine('TinyMaker', 'TinyMaker', {
    type => 'machine', from => 'system', name => 'TinyMaker TinyMaker', printer_technology => 'SLA',
    printer_model => 'TinyMaker', display_width => '40.8', display_height => '30.6',
    display_pixels_x => '320', display_pixels_y => '240', display_orientation => 'landscape',
    display_mirror_x => '0', display_mirror_y => '0', printable_height => '60', gamma_correction => '1',
    sla_archive_format => 'TINYMAKER', layer_height => '0.05', initial_layer_height => '0.05',
});
push @index, { vendor => 'TinyMaker', model => 'TinyMaker', file => 'printers/TinyMaker/TinyMaker.json',
               format => 'TINYMAKER', format_version => undef, supported => \1, nonsquare => \0 };

@index = sort { "$a->{vendor} $a->{model}" cmp "$b->{vendor} $b->{model}" } @index;
open(my $out, '>', "$dst/index.json") or die;
print $out "{\n  \"printers\": [\n";
print $out join(",\n", map { "    " . entry($_) } @index), "\n  ]\n}\n";
close $out;
print scalar(@index), " printers\n";

sub write_machine {
    my ($vendor, $model, $m) = @_;
    make_path("$dst/printers/$vendor");
    open(my $o, '>', "$dst/printers/$vendor/$model.json") or die "$vendor/$model: $!";
    my @order = qw(type from name printer_technology printer_model);
    my %seen = map { $_ => 1 } @order;
    push @order, grep { !$seen{$_} } sort keys %$m;
    print $o "{\n", join(",\n", map { "  \"$_\": \"$m->{$_}\"" } grep { defined $m->{$_} } @order), "\n}\n";
    close $o;
}

sub entry {
    my $e = shift;
    return sprintf('{ "vendor": "%s", "model": "%s", "file": "%s", "format": "%s", "format_version": %s, "supported": %s, "nonsquare_pixels": %s }',
        $e->{vendor}, $e->{model}, $e->{file}, $e->{format},
        defined $e->{format_version} ? "\"$e->{format_version}\"" : 'null', ${ $e->{supported} } ? 'true' : 'false', ${ $e->{nonsquare} } ? 'true' : 'false');
}
