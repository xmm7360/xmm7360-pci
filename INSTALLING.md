# Installing

> Note
>
> The first time you run `make load`, you will see: `rmmod: ERROR: Module xmm7360 is not currently loaded`.
> This means the module was not already loaded; which is fine since we're loading it for the first time.

## Dependencies

- build-essential
- python3-pyroute2
- python3-configargparse

## Using managment script

> Only tested on Ubuntu 20.04.

```
$ cp xmm7360.ini.sample xmm7360.ini  # edit at least the apn in the configuration file
$ sudo ./scripts/lte.sh setup
$ lte up  # should auto-elevate when run
```

## Ubuntu 20.04

```
mkdir ~/tmp/
cd ~/tmp/
sudo apt install build-essential python3-pyroute2 python3-configargparse git
git clone https://github.com/xmm7360/xmm7360-pci.git
cd xmm7360-pci
make && make load
cp xmm7360.ini.sample xmm7360.ini  # edit at least the apn in the configuration file
sudo python3 rpc/open_xdatachannel.py
sudo ip link set wwan0 up
```

The script should set nameservers from your network providers, but you might have to set your own:

```
echo "nameserver 1.1.1.1" | sudo tee -a /etc/resolv.conf
echo "nameserver 8.8.8.8" | sudo tee -a /etc/resolv.conf
echo "nameserver 9.9.9.9" | sudo tee -a /etc/resolv.conf
```

# Installing with DKMS
Using DKMS (https://wiki.archlinux.org/title/Dynamic_Kernel_Module_Support) allows you to automate the **compilation** and **signing** of kernel modules, for example whenever you update your kernel.

DKMS also has many other features, like the auto-generation of `.deb` for a particular kernel. See `man dkms` for more.

## Compiling xmm7360.ko with DKMS
The following steps replaces the commands from `Installing/Ubuntu 20.04`, up to and including `make && make load`. **Do not run `lte setup`.**

First, install DKMS and other dependencies:
```bash
sudo apt install dkms
sudo apt install build-essential python3-pyroute2 python3-configargparse git
```

Then, install the source code to `/usr/src/`:
```bash
TMP=$(mktemp -d)
cd $TMP

# Clone the Repository
git clone https://github.com/xmm7360/xmm7360-pci.git 
cd xmm7360-pci
## For a particular branch/commit, run 'git checkout <REF>' here

# Feed Commit ID as Package Version to dkms.conf
COMMIT_ID=$(git rev-parse HEAD)
sed "s/COMMIT_ID_VERSION/$COMMIT_ID/g" dkms.tmpl.conf > dkms.conf

# Install in /usr/src
sudo cp -r ./ /usr/src/xmm7360-pci-$COMMIT_ID/
```

Now, you can use DKMS to automatically build and sign the kernel module for the curent kernel with one simple command:
```bash
sudo dkms install xmm7360-pci/$COMMIT_ID
```

You can now manually load the kernel module with:
```bash
sudo modprobe xmm7360
```

To load the `xmm7360` module automatically on boot, create the following file:

`/etc/modules-load.d/xmm7360.conf`
```bash
xmm7360
```

**Make sure to test your setup before auto-loading the module**.

## Running w/DKMS Install
**Do not run `lte setup`**. Instead, just run `sudo ./scripts/lte.sh up` after loading the kernel module.

# Signing for Secure Boot
If you use Secure Boot, you'll get `Operation not permitted` when executing `modprobe xmm7360` (ex. through `make load`). This is because Secure Boot requires all kernel code to be signed by a trusted party.

DKMS can easily manage signing kernel modules for you. All you have to do, is generate the signing keypair, and enroll it into your BIOS.

First, you need to get openssl and mokutil.
```bash
sudo apt install openssl mokutil
```

Next, generate the signing keys into `/root`:
```bash
openssl req -new -x509 -newkey rsa:2048 \
-keyout /root/mok.priv \
-outform DER -out /root/mok.der \
-nodes -days 36500 \
-subj "/CN=user Kmod Signing MOK"
```

Enroll the public key into the firmware:
```
sudo mokutil --import /root/mok.der
```

Now, restart your computer. Your BIOS should prompt you to accept the key, before booting into the system.

**If you use DKMS** you only need to configure DKMS to use the pre-shipped signing tool to sign kernel modules whenever it builds them:

`/etc/dkms/framework.conf`
```bash
...
## Script to sign modules during build, script is called with kernel version
## and module name
sign_tool="/etc/dkms/sign_helper.sh"
```

If it for some reason doesn't exist, create it:

`/etc/dkms/sign_helper.sh`
```bash
#!/bin/sh
/lib/modules/"$1"/build/scripts/sign-file sha512 /root/mok.priv /root/mok.der "$2"
```

**If you don't use DKMS** you can try (untested) running the above script using:
- `$1 => linux-image-<version>`
- `$2 => /path/to/xmm7360.ko`
