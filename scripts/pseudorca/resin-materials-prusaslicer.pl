#!/usr/bin/env perl
# Adds the resins PrusaSlicer ships SLA profiles for (PrusaResearchSLA.ini of PrusaSlicer 2.9.6, AGPL-3.0, github.com/prusa3d/PrusaSlicer)
# to resources/resin/materials, as rows for the Prusa SL1 and SL1S SPEED.
#
# Each concrete [sla_material:...] section is one resin at one layer height for one of the two printers: the printer comes from the
# section's (inherited) compatible_printers_condition (printer_model=="SL1" or "SL1S"), the layer height from its compatible_prints_condition.
# What they hold that a row can use is the exposure and the bottom exposure; the SL1 tilts, so there is no lift. The maker comes from
# material_vendor, and a resin the library already has by name (the Siraya Tech ones) gets the rows added to its file. A row for that resin,
# printer and layer height that is already there is kept.
#
# usage: resin-materials-prusaslicer.pl <PrusaResearchSLA.ini of the 2.9.6 tag> <resources/resin> <retrieved date>
use strict;
use warnings;
use JSON::PP;
use File::Path qw(make_path);

my ($ini, $dir, $date) = @ARGV;
die "usage: $0 <PrusaResearchSLA.ini> <resources/resin> <date>\n" unless $ini && $dir && $date;
my $json = JSON::PP->new->pretty->canonical->indent_length(2)->space_before(1)->space_after(1);
my $source = 'https://github.com/prusa3d/PrusaSlicer/blob/version_2.9.6/resources/profiles/PrusaResearchSLA.ini';
my %printer = (SL1 => 'Prusa SL1', SL1S => 'Prusa SL1S SPEED');

my (%sec, @order);
{
    open(my $fh, '<', $ini) or die "$ini: $!";
    my $cur;
    while (my $l = <$fh>) {
        $l =~ s/\r?\n$//;
        if ($l =~ /^\[sla_material:(.+)\]$/) { $cur = $1; push @order, $cur unless $sec{$cur}; $sec{$cur} ||= {}; }
        elsif ($l =~ /^\[/) { undef $cur }
        elsif (defined $cur && $l =~ /^(\w+)\s*=\s*(.*)$/) { $sec{$cur}{$1} = $2 }
    }
}
sub flatten {
    my ($n, $d) = @_;
    my $s = $sec{$n} or return {};
    return {} if $d > 8;
    my %out;
    for my $p (split /[;,]/, $s->{inherits} // '') { $p =~ s/^\s+|\s+$//g; %out = (%out, %{ flatten($p, $d + 1) }) if length $p }
    %out = (%out, %$s);
    return \%out;
}
sub slurp { my ($f) = @_; open(my $fh, '<', $f) or die "$f: $!"; local $/; return decode_json(<$fh>) }

# the resins the library has, by name
my %file_of;
for my $f (glob("'$dir/materials/*/*.json'")) { my $m = slurp($f); $file_of{ $m->{name} } = $f }

my (%skipped, $added, $kept, %created, %by_file);
for my $name (@order) {
    next if $name =~ /^\*/;
    my $v = flatten($name, 0);
    next unless defined $v->{exposure_time} && defined $v->{initial_exposure_time};
    my ($model) = ($v->{compatible_printers_condition} // '') =~ /printer_model\s*==\s*"(\w+)"/;
    my $printer = $model ? $printer{$model} : undef;
    unless ($printer) { $skipped{"printer condition of $name"} = 1; next }
    my ($h) = ($v->{compatible_prints_condition} // '') =~ /layer_height\s*==\s*([\d.]+)/;
    $h //= $v->{initial_layer_height};
    unless (defined $h && $h > 0) { $skipped{"layer height of $name"} = 1; next }
    my $maker = $v->{material_vendor} // next;
    (my $base = $name) =~ s/\s*@\s*[\d.]+(\s+SL1S)?\s*$//;
    my $full = index(lc $base, lc $maker) == 0 ? $base : "$maker $base";
    my $row = { printers => [$printer], layer_height => $h + 0, exposure_time => $v->{exposure_time} + 0,
                initial_exposure_time => $v->{initial_exposure_time} + 0,
                notes => "PrusaSlicer 2.9.6 profile for the " . ($model eq 'SL1S' ? 'SL1S SPEED' : 'SL1') . ' (a tilting printer: no lift)',
                source => $source, retrieved => $date };
    my $file = $file_of{$full};
    my $m;
    if ($file) { $m = $by_file{$file} // slurp($file) }
    else {
        (my $vdir = $maker) =~ s{[/\\]}{ }g;
        (my $fname = $base) =~ s{[/\\]}{ }g;
        make_path("$dir/materials/$vdir");
        $file = "$dir/materials/$vdir/$fname.json";
        $file_of{$full} = $file; $created{$file} = 1;
        my $type = lc($v->{material_type} // ''); $type = '' if $type eq 'model';
        $m = { type => 'material', vendor => $maker, name => $full, resin_type => $type, settings => [] };
    }
    my $dup = grep { abs($_->{layer_height} - $row->{layer_height}) < 1e-6 && grep { $_ eq $printer } @{ $_->{printers} } } @{ $m->{settings} };
    if ($dup) { $kept++; $by_file{$file} = $m; next }
    push @{ $m->{settings} }, $row; $added++;
    $by_file{$file} = $m;
}
for my $file (keys %by_file) {
    my $m = $by_file{$file};
    @{ $m->{settings} } = sort { $a->{layer_height} <=> $b->{layer_height} || $a->{printers}[0] cmp $b->{printers}[0] } @{ $m->{settings} } if $created{$file};
    open(my $o, '>', $file) or die "$file: $!";
    print $o $json->encode($m);
    close $o;
}
print "rows added: $added; already there: ", $kept // 0, "; resins created: ", scalar(keys %created), "\n";
print "skipped: ", join('; ', sort keys %skipped), "\n" if %skipped;
