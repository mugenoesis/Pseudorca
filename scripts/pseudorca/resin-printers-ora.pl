#!/usr/bin/env perl
# Adds the resin printers the Open Resin Alliance's DragonFruit plugins describe and resources/resin does not have yet.
#
# The plugins (https://github.com/Open-Resin-Alliance/df-plugin-anycubic, -elegoo and -uniformation, all MIT) give each printer's
# resolution, pixel size (non-square for some), build height and output file format. A printer the catalogue already has is left
# alone: the catalogue's numbers come from UVtools' profiles and where the two disagree (the screen of the Photon Mono M5 family, the
# resolution of the Photon M3 Premium, the mirror flags, which mean something else in DragonFruit) the UVtools ones are kept.
# A new printer takes the lift, delay and mirror defaults of the printer next to it in the same family (it is named below), and its own
# screen from the plugin: width and height are the resolution times the pixel size.
#
# usage: resin-printers-ora.pl <dir holding the checkouts of df-plugin-anycubic, -elegoo, -uniformation> <resources/resin>
use strict;
use warnings;
use JSON::PP;
use File::Path qw(make_path);

my ($plugins, $dir) = @ARGV;
die "usage: $0 <plugins dir> <resources/resin>\n" unless $plugins && $dir;
sub slurp { my ($f) = @_; open(my $fh, '<', $f) or die "$f: $!"; local $/; return decode_json(<$fh>) }
sub norm { my ($s) = @_; $s = lc $s; $s =~ s/[^a-z0-9]+//g; return $s }

my %vendor = ('ELEGOO' => 'Elegoo', 'Anycubic' => 'Anycubic', 'UniFormation' => 'UniFormation');
# The plugin's entries that are not new printers, or that look wrong: skipped, with the reason.
my %skip = (
    'Elegoo Saturn 4 Ultra'               => 'the catalogue has it as Saturn 4 Ultra 12K',
    'Anycubic Photon Mono X 6K / M3 Plus' => 'two printers the catalogue has (Photon Mono X 6K and Photon M3 Plus)',
    'Elegoo Saturn 2 14K'                 => 'its 13320 x 5120 screen at 16.8 um is the Photon Mono M5s Pro\'s, which looks like a mix-up',
);
# What a new printer takes its defaults from.
my %like = (
    'Elegoo Jupiter SE' => 'Elegoo Jupiter', 'Elegoo Mars 3 Pro' => 'Elegoo Mars 3',
    'UniFormation GK3' => 'UniFormation GKtwo', 'UniFormation GK3 Pro' => 'UniFormation GKtwo', 'UniFormation GK3 Ultra' => 'UniFormation GKtwo',
);

my $index = slurp("$dir/index.json");
my %have = map { norm("$_->{vendor} $_->{model}") => 1 } @{ $index->{printers} };
my @added;
for my $plugin (qw(df-plugin-anycubic df-plugin-elegoo df-plugin-uniformation)) {
    for my $f (sort glob("'$plugins/$plugin/printers/*.json'")) {
        for my $o (@{ slurp($f) }) {
            my $vendor = $vendor{ $o->{manufacturer} } // $o->{manufacturer};
            my $full = "$vendor $o->{name}";
            next if $have{ norm($full) };
            if ($skip{$full}) { print "skipped $full: $skip{$full}\n"; next }
            my $like = $like{$full} or do { print "no family printer named for $full: skipped\n"; next };
            my ($lv, $lm) = split / /, $like, 2;
            my $src = slurp("$dir/printers/$lv/$lm.json");
            my ($px, $py) = ($o->{pixelSize}{x}, $o->{pixelSize}{y});
            my ($rx, $ry) = ($o->{display}{resolutionX}, $o->{display}{resolutionY});
            my %m = %$src;
            $m{name} = $full; $m{printer_model} = $o->{name}; $m{printer_settings_id} = $full;
            $m{display_pixels_x} = $rx; $m{display_pixels_y} = $ry;
            $m{display_width}  = sprintf('%.3f', $rx * $px / 1000) + 0;
            $m{display_height} = sprintf('%.3f', $ry * $py / 1000) + 0;
            $m{printable_height} = $o->{buildVolumeMm}{height} + 0;
            $m{sla_archive_format} = 'CTBENCRYPTED';   # every one of them is ".ctb v5" in the plugin
            $m{$_} = "$m{$_}" for keys %m;
            my $nonsquare = abs(($m{display_width} / $rx) / ($m{display_height} / $ry) - 1) > 0.05 ? 1 : 0;
            make_path("$dir/printers/$vendor");
            my @order = qw(type from name printer_technology printer_model);
            my %seen = map { $_ => 1 } @order;
            push @order, grep { !$seen{$_} } sort keys %m;
            open(my $out, '>', "$dir/printers/$vendor/$o->{name}.json") or die "$o->{name}: $!";
            print $out "{\n", join(",\n", map { "  \"$_\": \"$m{$_}\"" } grep { defined $m{$_} } @order), "\n}\n";
            close $out;
            push @{ $index->{printers} }, { vendor => $vendor, model => $o->{name}, file => "printers/$vendor/$o->{name}.json", format => 'CTBENCRYPTED',
                                            format_version => undef, supported => 1, nonsquare => $nonsquare };
            push @added, $full;
        }
    }
}
my @sorted = sort { "$a->{vendor} $a->{model}" cmp "$b->{vendor} $b->{model}" } @{ $index->{printers} };
sub entry {
    my $e = shift;
    my $b = sub { ($_[0] && $_[0] ne 'false' && !(ref $_[0] eq 'JSON::PP::Boolean' && !$_[0])) ? 'true' : 'false' };
    return sprintf('{ "vendor": "%s", "model": "%s", "file": "%s", "format": "%s", "format_version": %s, "supported": %s, "nonsquare_pixels": %s }',
        $e->{vendor}, $e->{model}, $e->{file}, $e->{format}, defined $e->{format_version} ? "\"$e->{format_version}\"" : 'null',
        $b->($e->{supported}), $b->($e->{nonsquare} // $e->{nonsquare_pixels}));
}
open(my $out, '>', "$dir/index.json") or die;
print $out "{\n  \"printers\": [\n", join(",\n", map { "    " . entry($_) } @sorted), "\n  ]\n}\n";
close $out;
print "printers added: ", join(', ', @added), "\n";
