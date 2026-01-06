# Ramdisk FUSE Project

## Compilation
```bash
gcc -Wall -D_FILE_OFFSET_BITS=64 FUSE/fuse_ramdisk.c -lfuse -o FUSE/fuse_ramdisk
```

## Running the Program
```bash
sudo fusermount -u /tmp/mojdysk
mkdir -p /tmp/mojdysk
sudo ./FUSE/fuse_ramdisk -o allow_other -f /tmp/mojdysk
```

## Usage and Logging
In a new terminal, you can interact with the disk contents.
Logs will be displayed in the first terminal window. You can also view them using the `dmesg` command.
