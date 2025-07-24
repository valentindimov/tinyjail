# tinyjail - Containers in less than 100 KB!
`tinyjail` is a small Linux utility that supports a range of functionalities used for creating containers:

* Linux namespace isolation
* Resource limits using cgroups v2
* Basic container networking

## Acknowledgement
A special thanks goes to [TNG Technology Consuling GmbH](https://www.tngtech.com/en/) for their support in developing this tool.

## Building
The build script [build.sh](./build.sh) will build the utility, both as a statically-linked binary and a dynamic library you can link into your own projects. 
If you prefer, you can also directly use the [tinyjail.c](src/tinyjail.c) and [tinyjail.h](src/tinyjail.h) files as part of your source code.
[main.c](src/main.c) is just the command-line frontend of the binary.

You will need to install musl (on Linux: `sudo apt-get update -y && sudo apt-get install -y musl-dev musl-tools`) to run the build script. 
The binary should also build just fine with regular `gcc` or `clang` - however, static linking is discouraged with glibc.

## Binary usage
The static binary `build/tinyjail` produced by the build script (whose main function is in [main.c](./main.c)) can be used to start containers as well. 
Refer to the usage string produced by the binary for command-line arguments.

## System requirements
`tinyjail` requires cgroups v2 to be enabled on the system, at least for the `cpu`, `pids`, and `memory` controllers. 
You can disable the legacy cgroups v1 system by adding the `cgroup_no_v1=all` boot option to your kernel command line.

It also requires both `/proc/` and `/sys/fs/cgroup/` to be mounted, with the `proc` and `cgroup2` filesystems respectively.

## Container directory, UID and GID mapping
Your container's root directory is the filesystem root inside the container.
`tinyjail` will set the container process's UID and GID to the <b>owner UID and GID of the root directory</b>, and map them to the UID and GID 0 inside the container.
All other UIDs and GIDs will show up as `nobody` (65534) inside the container.

Note: Take care that the directory is owned by the user you want the container to run as.
Especially when you create mountpoints (e.g. mounting ISO files or creating tmpfs mounts) it may actually be owned by root in the end.
Additionally, if the root directory is a mount point, make sure it has private propagation, otherwise pivot_root won't work.

## Networking
If you do not specify `--network-bridge`, your container will have no network access, only a loopback device.
Otherwise, `tinyjail` will create a virtual Ethernet device for your container and connect it to the specified bridge device.
Giving your container's network device an IP address and default gateway is optional - however, if you specify either, you must also specify a bridge device.

### Example Container Networking Setup With Bridge
The following snippet of commands will create a bridge device called `tinyjailbr`, give your host the address `10.0.100.1/24`, and set up IP forwarding and NAT for access to the Internet.

```bash
# Set up bridge device
ip link add tinyjailbr type bridge
ip addr add 10.0.100.1/24 dev tinyjailbr
ip link set tinyjailbr up

# Enable IP forwarding
sysctl net.ipv4.ip_forward=1

# Enable SNAT from the container subnet
iptables -t nat -A POSTROUTING -s 10.0.100.0/24 ! -d 10.0.100.0/24 -j MASQUERADE
```

Then, you can start your container like so:

```bash
sudo ./tinyjail --network-bridge tinyjailbr --ip-address 10.0.100.2/24 --default-route 10.0.100.1 --root <container root directory> -- <your command>
```

From inside the container, you should be able to access the Internet, e.g. to ping `8.8.8.8`.

### Example Container Networking Setup Without Bridge
If you do not need to have multiple containers communicating over a bridge but just need a single container to have Internet access, you can just give the address `10.0.100.1/24` to the host end of the vEth pair instead:

```bash
# Enable IP forwarding
sysctl net.ipv4.ip_forward=1
# Enable SNAT from the container subnet
iptables -t nat -A POSTROUTING -s 10.0.100.0/24 ! -d 10.0.100.0/24 -j MASQUERADE
# Run the container
sudo ./tinyjail --ip-address 10.0.100.2/24 --peer-ip-address 10.0.100.1/24 --default-route 10.0.100.1 --root <container root directory> -- <your command>
```