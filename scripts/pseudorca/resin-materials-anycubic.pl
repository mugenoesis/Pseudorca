#!/usr/bin/env perl
# Builds resources/resin/materials/Anycubic from Anycubic's "Resin Settings for Anycubic 3D Printers" page
# (https://store.anycubic.com/blogs/news/resin-settings-for-anycubic-3d-printers): one table for each printer, one column for
# each of Anycubic's resins. The page says the numbers come from the Anycubic Laboratory and are for reference.
#
# usage: resin-materials-anycubic.pl <saved page .html> <output dir> <retrieved date>
use strict;
use warnings;
use JSON::PP;
use File::Path qw(make_path);

my ($page, $dst, $date) = @ARGV;
die "usage: $0 <page.html> <output dir> <date>\n" unless $page && $dst && $date;
my $url = 'https://store.anycubic.com/blogs/news/resin-settings-for-anycubic-3d-printers';

open(my $fh, '<', $page) or die "$page: $!";
my $h = do { local $/; <$fh> };
$h =~ s/<script.*?<\/script>//gs; $h =~ s/<style.*?<\/style>//gs;

sub clean { my $c = shift; $c =~ s/\xC2\xA0/ /g; $c =~ s/<[^>]*>/ /g; $c =~ s/&nbsp;/ /g; $c =~ s/&amp;/&/g; $c =~ s/\s+/ /g; $c =~ s/^ | $//g; $c }
sub number { my ($s) = @_; return $s =~ /^\s*([0-9]+(?:\.[0-9]+)?)\s*(?:s|mm|mm\/s)?\s*$/ ? $1 + 0 : undef }

# resin type by the name of the resin
sub resin_type {
    my $n = lc shift;
    return 'water-washable' if $n =~ /water wash/ && $n !~ /abs/;
    return 'abs-like' if $n =~ /abs-like/;
    return 'tough' if $n =~ /tough/;
    return 'plant-based' if $n =~ /plant/;
    return 'standard' if $n =~ /standard/;
    return 'clear' if $n =~ /clear/;
    return '';
}

my (%resins, @printers);
# walk the page: a heading names the printer, the next table holds its settings
my $pos = 0;
while ($h =~ /(.*?)(<table.*?<\/table>)/sg) {
    my ($before, $table) = ($1, $2);
    # the heading is the last line of text before the table
    $before =~ s/<(?:br|\/p|\/h\d|\/li|\/div|\/tr)[^>]*>/\n/gi;
    $before =~ s/<[^>]*>//g;
    my ($heading) = reverse grep { /\S/ } split /\n/, $before;
    next unless defined $heading;
    my $printer = clean($heading);
    $printer =~ s/^Anycubic Resin Settings for\s*//; $printer =~ s/^Anycubic //;
    next unless $printer =~ /^(?:Photon|Mono)/;
    my $name = "Anycubic $printer";
    $name =~ s/\s+/ /g;
    my %rows;
    my @names;
    while ($table =~ /<tr[^>]*>(.*?)<\/tr>/sg) {
        my @c; my $r = $1;
        while ($r =~ /<t[dh][^>]*>(.*?)<\/t[dh]>/sg) { push @c, clean($1) }
        my $label = shift @c;
        next unless defined $label;
        if ($label eq 'Resin Type') { @names = @c; next }
        $rows{$label} = \@c;
    }
    for my $i (0 .. $#names) {
        my %v;
        $v{layer_height}          = number($rows{'Layer Thickness'}[$i] // '');
        $v{exposure_time}         = number($rows{'Exposure Time'}[$i] // '');
        $v{delay_before_exposure} = number($rows{'Light-Off Time'}[$i] // '');
        $v{initial_exposure_time} = number($rows{'Bottom Exposure Time'}[$i] // '');
        $v{bottom_layer_count}    = number($rows{'Bottom Layer'}[$i] // '');
        $v{lift_distance}         = number($rows{'Z Lift Distance'}[$i] // '');
        my $ls = number($rows{'Z Lift Speed'}[$i] // '');
        my $rs = number($rows{'Z Retract Speed'}[$i] // '');
        $v{lift_speed}    = $ls * 60 if defined $ls;      # mm/s on the page, mm/min here
        $v{retract_speed} = $rs * 60 if defined $rs;
        next unless defined $v{layer_height} && defined $v{exposure_time} && defined $v{initial_exposure_time};
        $v{bottom_layer_count} = int($v{bottom_layer_count}) if defined $v{bottom_layer_count};
        delete $v{$_} for grep { !defined $v{$_} } keys %v;
        push @{ $resins{ $names[$i] }{rows} }, { printers => [$name], %v, source => $url, retrieved => $date,
            notes => "Anycubic Laboratory reference settings for the factory default light energy" };
    }
}

my $json = JSON::PP->new->pretty->canonical->indent_length(2);
make_path("$dst/Anycubic");
my $count = 0;
for my $resin (sort keys %resins) {
    (my $file = $resin) =~ s/\+/ Plus/g; $file =~ s/[^A-Za-z0-9 .-]/ /g;
    $file =~ s/\s+/ /g; $file =~ s/^ | $//g;
    my $m = { type => 'material', name => "Anycubic $resin", vendor => 'Anycubic', settings => $resins{$resin}{rows} };
    my $t = resin_type($resin);
    $m->{resin_type} = $t if $t;
    open(my $o, '>', "$dst/Anycubic/$file.json") or die "$file: $!";
    print $o $json->encode($m);
    close $o;
    $count++;
}
print "$count resins\n";
