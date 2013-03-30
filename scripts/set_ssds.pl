#!/usr/bin/perl

use strict;

my @dev_names=("b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o", "p", "q");

for (my $i = 1; $i <= 16; $i++) {
	my $dev_idx = $i - 1;
	my $dev_file = "/dev/sd$dev_names[$dev_idx]1";
	my $dir = "/mnt/ssd${i}";
	print "dev: $dev_file, dir: $dir\n" ;
	system("mkdir -p $dir");
	system("mount $dev_file $dir");
	system("chown zhengda.zhengda $dir/test");
	system("echo noop > /sys/block/sd$dev_names[$dev_idx]/queue/scheduler");
	system("cat /sys/block/sd$dev_names[$dev_idx]/queue/scheduler");
}

