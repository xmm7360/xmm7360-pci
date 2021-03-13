#!/usr/bin/perl

use strict;

my $busid;
print "reset modem\n";
open FH,'lspci -n|';
while (<FH>)
    {
    if(/^([^\s+]+)\s+.*8086:7360/)
       {
       $busid = $1;
       }
    }
close FH;

exit 1 if ! $busid;

my $cfg = "/sys/bus/pci/devices/0000:$busid/config";
exit 1 if (! -e $cfg);

system ('dd', "if=$cfg", 'of=/var/lib/hardware/xmm_cfg', 'bs=256', 'count=1', 'status=none');
system ('modprobe', 'acpi_call');
system ('rmmod', 'xmm7360');
system ("echo '\_SB.PCI0.RP07.PXSX._RST' | tee /proc/acpi/call");
sleep 1;
system ('dd', "of=$cfg", 'if=/var/lib/hardware/xmm_cfg', 'bs=256', 'count=1', 'status=none');
system ('modprobe', 'xmm7360');
sleep 3;
    