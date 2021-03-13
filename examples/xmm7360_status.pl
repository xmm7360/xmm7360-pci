#!/usr/bin/perl

# ============================================================================
#   that's what we need
# ============================================================================

use FindBin ;
use strict;
use warnings;
use Net::DBus;
use Gtk3 '-init';
use Glib 'TRUE', 'FALSE';

# ============================================================================
#   setup global variables
# ============================================================================

our $bus      = Net::DBus -> system;
our $serv     = $bus  -> get_service("org.freedesktop.NetworkManager");
our $manager  = $serv -> get_object("/org/freedesktop/NetworkManager/Settings");
our $manager2 = $serv -> get_object("/org/freedesktop/NetworkManager", "org.freedesktop.NetworkManager");
our $connection;
our $devpath;
$| = 1;

# ----------------------------------------------------------------------------
#
#   network_status - callback for checking network status
#                    returns nothing valueable
#
#   in  %$widgets      GTK widgets we care about
#

sub network_status
    {
    my $widgets = shift;

    my $myconn  = undef;
    my $propm   = $serv -> get_object('/org/freedesktop/NetworkManager','org.freedesktop.DBus.Properties');
    if ($propm)
        {
        my $nm = $propm -> GetAll('org.freedesktop.NetworkManager') ;
        if ($nm)
            {
            my $act_con = $nm -> {ActiveConnections};
            foreach my $path (@$act_con)
                {
                my $con;
                my $act_con_prop;
                eval {$act_con_prop = $serv -> get_object($path, 'org.freedesktop.DBus.Properties');};
                next if (! $act_con_prop);
                eval {$con = $act_con_prop -> GetAll('org.freedesktop.NetworkManager.Connection.Active');};
                next if (!$con);
                if ($con -> {Id} eq 'xmm7360')
                    {
                    $con -> {Path} = $path;
                    $myconn = $con;
                    #print "path=$path\n";
                    last;
                    }
                }
            }     
        } 
 
   if (! defined $myconn)
        {
        print ".";
        my $conns;
        my $devs = $manager2 -> GetDevices;
        foreach my $dev (@$devs)
            {
            my $dev_prop = $serv -> get_object($dev, 'org.freedesktop.DBus.Properties');
            my $props    = $dev_prop -> GetAll ('org.freedesktop.NetworkManager.Device');
            next if ($props->{Interface} ne 'wwan0');
            next if (!$props->{Managed});
            $conns = $props -> {AvailableConnections};
            $devpath = $dev;
            }
        if($conns->[0])
            {
            $connection = $conns->[0];
            $widgets->{status_icon} -> set_from_file ('/usr/share/icons/HighContrast/16x16/status/network-cellular-no-route.png');
            $widgets->{status_icon} -> set_tooltip_text('modem is disconnected');
            $widgets->{con_item} -> show;
            $widgets->{dis_item} -> hide;            
            }
        else
            {
            $widgets->{status_icon} -> set_from_file ('/usr/share/icons/HighContrast/16x16/status/network-cellular-offline.png');
            }
        }
    elsif ($myconn -> {State} == 2)
        {
        print "_";
        $widgets->{status_icon} -> set_from_file ('/usr/share/icons/HighContrast/16x16/status/network-cellular-4g.png');
        $widgets->{con_item} -> hide;
        $widgets->{dis_item} -> show;
        $connection = $myconn -> {Path};
        $widgets->{status_icon} -> set_tooltip_text('modem is connected');
        }
    else
        {
        print "-";
        $widgets->{con_item} -> show;
        $widgets->{dis_item} -> hide;
        $widgets->{status_icon} -> set_from_file ('/usr/share/icons/HighContrast/16x16/status/network-cellular-no-route.png');
        $widgets->{status_icon} -> set_tooltip_text('modem is disconnected');
        }
    while (Gtk3::events_pending()) {Gtk3::main_iteration();};
    print " ";
    }

# ----------------------------------------------------------------------------
#
#   activate       - activate connection
#                    returns nothing
#  

sub activate
    {
    $manager2 -> ActivateConnection ($connection, $devpath, '/');
    }


# ----------------------------------------------------------------------------
#
#   deactivate     - deactivate connection
#                    returns nothing
#

sub deactivate
    {
    $manager2 -> DeactivateConnection ($connection);
    }

# ============================================================================
#   setup and run the GTK3 Application
# ============================================================================
    
my $icon = Gtk3::StatusIcon -> new_from_file('/usr/share/icons/Adwaita/16x16/devices/auth-sim-symbolic.symbolic.png');
my $menu = Gtk3::Menu -> new();


$menu -> set_size_request(140, 120);


my $disc_item = Gtk3::MenuItem -> new('Disconnect');
$menu -> append($disc_item);
$disc_item -> signal_connect( activate => sub { deactivate (); } );

my $con_item = Gtk3::MenuItem -> new('Connect');
$menu -> append($con_item);
$con_item -> signal_connect( activate => sub { activate (); } );

#we should never quit the application
my $item = Gtk3::ImageMenuItem->new_from_stock('gtk-quit');
$item->signal_connect( activate => sub { Gtk3->main_quit ; } );
$menu->append($item);

$menu -> show_all();

$icon -> signal_connect(
    'popup-menu' => sub {
        my ( $show, $button, $event_time ) = @_;
        $menu -> popup( undef, undef, \&Gtk3::StatusIcon::position_menu,
            $show, $button, $event_time );
    } ,
);

Glib::Timeout -> add_seconds (1, sub {network_status({status_icon => $icon, dis_item => $disc_item, con_item => $con_item}); print '.';} );
Gtk3 -> main();

exec ('perl', '/usr/bin/xmm7360_status.pl');