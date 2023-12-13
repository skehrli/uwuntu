#pragma once

#include "extent.h"
#include "param.h"

// On-disk file system format.
// Both the kernel and user programs use this header file.

#define INODEFILEINO 0 // inode file inum
#define ROOTINO 1      // root i-number
#define BSIZE 512      // block size

#define LOG_VALID 1  // log was committed
#define LOG_INVALID 0 // log was not committed

// Disk layout:
// [ boot block | super block | free bit map | log |
//                                          inode file | data blocks]
//
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
struct superblock {
  uint size;       // Size of file system image (blocks)
  uint nblocks;    // Number of data blocks
  uint bmapstart;  // Block number of first free map block
  uint logstart;   // Block number of first log block
  uint inodestart; // Block number of the start of inode file
};

struct logheader {
  int commit;  // 1 if the log was committed, 0 otherwise
  int size;    // Number of blocks in the log
  int disk_loc[LOGSIZE]; // Disk locations of the blocks in the log
  char pad[BSIZE - 2 * sizeof(int) - LOGSIZE * sizeof(int)];
};

// On-disk inode structure
// bytes = 2 + 2 + 4 + 30 * 8 = 248
// pad to make it a power of 2 --> +8 --> 256
struct dinode {
  short type;         // File type (device, directory, regular file)
  short devid;        // Device number (T_DEV only)
  uint size;          // Size of file (bytes)
  struct extent data[EXTENTS]; // Data blocks of file on disk
  char pad[8];       // So disk inodes fit contiguosly in a block
};

// offset of inode in inodefile
#define INODEOFF(inum) ((inum) * sizeof(struct dinode))

// Bitmap bits per block
#define BPB (BSIZE * 8)

// Block of free map containing bit for block b
#define BBLOCK(b, sb) ((b) / BPB + (sb).bmapstart)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

struct dirent {
  ushort inum;
  char name[DIRSIZ];
};
