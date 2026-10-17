#!/usr/bin/env perl
# Writes resources/resin/materials-index.json from the material files under resources/resin/materials.
#
# usage: resin-materials-index.pl <resources/resin>
use strict;
use warnings;
use JSON::PP;
use File::Find;

my ($dir) = @ARGV;
die "usage: $0 <resources/resin>\n" unless $dir;
my $json = JSON::PP->new->pretty->canonical->indent_length(2);
my @index;
find({ no_chdir => 1, wanted => sub {
    return unless /\.json$/;
    open(my $fh, '<', $_) or die "$_: $!";
    my $m = decode_json(do { local $/; <$fh> });
    return unless ($m->{type} // '') eq 'material';
    (my $rel = $File::Find::name) =~ s{^\Q$dir\E/}{};
    push @index, { name => $m->{name}, vendor => $m->{vendor}, resin_type => $m->{resin_type} // '', family => $m->{family} // '', color => $m->{color} // '', file => $rel,
                   rows => scalar @{ $m->{settings} } };
} }, "$dir/materials");
@index = sort { $a->{name} cmp $b->{name} } @index;
open(my $o, '>', "$dir/materials-index.json") or die;
print $o $json->encode({ materials => \@index });
close $o;
print scalar(@index), " materials\n";
