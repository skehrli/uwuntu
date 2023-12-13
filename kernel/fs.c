// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xk/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include <cdefs.h>
#include <defs.h>
#include <file.h>
#include <fs.h>
#include <mmu.h>
#include <param.h>
#include <proc.h>
#include <sleeplock.h>
#include <spinlock.h>
#include <stat.h>

#include <buf.h>

// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb;

// Log API

// Begin a log transaction
static void log_begin_tx() {
  struct buf* log_header_buf = bread(ROOTDEV, sb.logstart);

  struct logheader log_header;
  log_header.commit = LOG_INVALID;
  log_header.size = 0;

  memmove(&log_header_buf->data, &log_header, sizeof(log_header));

  bwrite(log_header_buf);
  brelse(log_header_buf);
}

// Write a block to the log
static void log_write(struct buf* buf) {
  struct buf* log_header_buf = bread(ROOTDEV, sb.logstart);

  struct logheader log_header;
  memmove(&log_header, &log_header_buf->data, sizeof(struct logheader));

  if (log_header.commit != LOG_INVALID) {
    panic("log_write: writing to log when commit is not invalid");
  }

  if (log_header.size >= LOGSIZE-1) {
    panic("log_write: log is full");
  }

  // Write data block to log
  struct buf* data_blk = bread(ROOTDEV, sb.logstart + log_header.size + 1);
  memmove(&data_blk->data, &buf->data, BSIZE);
  bwrite(data_blk);

  // Update header and write it to disk
  log_header.disk_loc[log_header.size] = buf->blockno;
  log_header.size++;
  memmove(&log_header_buf->data, &log_header, sizeof(struct logheader));
  bwrite(log_header_buf);

  brelse(data_blk);
  brelse(log_header_buf);
}

// Completes the transaction and flushes it to disk
static void log_commit_tx() {
  struct buf* log_header_buf = bread(ROOTDEV, sb.logstart);
  struct logheader log_header;

  memmove(&log_header, &log_header_buf->data, sizeof(struct logheader));

  if (log_header.commit != LOG_INVALID) {
    panic("log_commit_tx: commit flag is not invalid before commit");
  }

  if (log_header.size >= LOGSIZE) {
    panic("log_commit_tx: log is full");
  }

  // Update commit to VALID and write header to disk
  log_header.commit = LOG_VALID;
  memmove(&log_header_buf->data, &log_header, sizeof(struct logheader));
  bwrite(log_header_buf);
  brelse(log_header_buf);

  // Update log header
  log_header_buf = bread(ROOTDEV, sb.logstart);
  memmove(&log_header, &log_header_buf->data, sizeof(struct logheader));

  if (log_header.commit != LOG_VALID) {
    panic("log_commit_tx: commit flag is not valid during commit");
  }

  if (log_header.size >= LOGSIZE) {
    panic("log_commit_tx: log is full");
  }

  // Transfer blocks
  for (int i = 0; i < log_header.size; i++) {
    struct buf* data_buf = bread(ROOTDEV, log_header.disk_loc[i]); // The actual disk block
    struct buf* log_buf = bread(ROOTDEV, sb.logstart + i + 1); // The corresponding block in the log

    // Write to correct disk location
    memmove(&data_buf->data, &log_buf->data, BSIZE);
    bwrite(data_buf);

    brelse(data_buf);
    brelse(log_buf);
  }

  // Complete transaction by setting header flag to INVALID
  log_header.commit = LOG_INVALID;
  memmove(&log_header_buf->data, &log_header, sizeof(struct logheader));
  bwrite(log_header_buf);

  brelse(log_header_buf);
}

static void log_apply() {
  cprintf("log_apply: start\n");

  struct buf* log_header_buf = bread(ROOTDEV, sb.logstart);
  struct logheader log_header;

  memmove(&log_header, &log_header_buf->data, sizeof(struct logheader));

  // If commited, apply the log to the disk
  if (log_header.commit == LOG_VALID) {
    // Transfer blocks
    for (int i = 0; i < log_header.size; i++) {
      struct buf* data_buff = bread(ROOTDEV, log_header.disk_loc[i]); // on disk
      struct buf* log_buff = bread(ROOTDEV, sb.logstart + i + 1); // in log

      // Write to correct disk location
      memmove(&data_buff->data, &log_buff->data, BSIZE);
      bwrite(data_buff);

      brelse(data_buff);
      brelse(log_buff);
    }
  }
  cprintf("log_apply: commit=%d\n", log_header.commit);

  log_header.commit = LOG_INVALID;
  log_header.size = 0;

  memmove(&log_header_buf->data, &log_header, sizeof(struct logheader));
  bwrite(log_header_buf);
  brelse(log_header_buf);
}

// Read the super block.
void readsb(int dev, struct superblock *sb) {
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// mark [start, end] bit in bp->data to 1 if used is true, else 0
static void bmark(struct buf *bp, uint start, uint end, bool used)
{
  int m, bi;
  for (bi = start; bi <= end; bi++) {
    m = 1 << (bi % 8);
    if (used) {
      bp->data[bi/8] |= m;  // Mark block in use.
    } else {
      if((bp->data[bi/8] & m) == 0)
        panic("freeing free block");
      bp->data[bi/8] &= ~m; // Mark block as free.
    }
  }
  bp->flags |= B_DIRTY; // mark our update
  log_write(bp);
}

// Blocks.

// Allocate n disk blocks, no promise on content of allocated disk blocks
// Returns the beginning block number of a consecutive chunk of n blocks
static uint balloc(uint dev, uint n)
{
  int b, bi, m;
  struct buf *bp;

  bp = 0;
  for (b = 0; b < sb.size; b += BPB) {
    bp = bread(dev, BBLOCK(b, sb)); // look through each bitmap sector

    uint sz = 0;
    uint i = 0;
    for (bi = 0; bi < BPB && b + bi < sb.size; bi++) {
      m = 1 << (bi % 8);
      if ((bp->data[bi/8] & m) == 0) {  // Is block free?
        sz++;
        if (sz == 1) // reset starting blk
          i = bi;
        if (sz == n) { // found n blks
          bmark(bp, i, bi, true); // mark data block as used
          brelse(bp);
          return b+i;
        }
      } else { // reset search
        sz = 0;
        i =0;
      }
    }
    brelse(bp);
  }
  panic("balloc: can't allocate contiguous blocks");
}

// Free n disk blocks starting from b
static void bfree(int dev, uint b, uint n)
{
  struct buf *bp;

  assertm(n >= 1, "freeing less than 1 block");
  assertm(BBLOCK(b, sb) == BBLOCK(b+n-1, sb), "returned blocks live in different bitmap sectors");

  bp = bread(dev, BBLOCK(b, sb));
  bmark(bp, b % BPB, (b+n-1) % BPB, false);
  brelse(bp);
}


// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// range of blocks holding the file's content.
//
// The inodes themselves are contained in a file known as the
// inodefile. This allows the number of inodes to grow dynamically
// appending to the end of the inode file. The inodefile has an
// inum of 0 and starts at sb.startinode.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->flags.
//
// Since there is no writing to the file system there is no need
// for the callers to worry about coherence between the disk
// and the in memory copy, although that will become important
// if writing to the disk is introduced.
//
// Clients use iget() to populate an inode with valid information
// from the disk. idup() can be used to add an in memory reference
// to and inode. irelease() will decrement the in memory reference count
// and will free the inode if there are no more references to it,
// freeing up space in the cache for the inode to be used again.



struct {
  struct spinlock lock;
  struct inode inode[NINODE];
  struct inode inodefile;
} icache;

// Find the inode file on the disk and load it into memory
// should only be called once, but is idempotent.
static void init_inodefile(int dev) {
  struct buf *b;
  struct dinode di;

  b = bread(dev, sb.inodestart);
  memmove(&di, b->data, sizeof(struct dinode));

  icache.inodefile.inum = INODEFILEINO;
  icache.inodefile.dev = dev;
  icache.inodefile.type = di.type;
  icache.inodefile.valid = 1;
  icache.inodefile.ref = 1;

  icache.inodefile.devid = di.devid;
  icache.inodefile.size = di.size;
  icache.inodefile.data[0] = di.data[0];
  for (int i = 1; i < EXTENTS; i++) {
    di.data[i].startblkno = 0;
    di.data[i].nblocks = 0;
    icache.inodefile.data[i] = di.data[i];
  }

  brelse(b);
}

void iinit(int dev) {
  int i;

  initlock(&icache.lock, "icache");
  for (i = 0; i < NINODE; i++) {
    initsleeplock(&icache.inode[i].lock, "inode");
  }
  initsleeplock(&icache.inodefile.lock, "inodefile");

  readsb(dev, &sb);
  cprintf("sb: size %d nblocks %d bmap start %d logstart %d inodestart %d\n", sb.size,
          sb.nblocks, sb.bmapstart, sb.logstart, sb.inodestart);
  
  log_apply();
  init_inodefile(dev);
}


// Reads the dinode with the passed inum from the inode file.
// Threadsafe, will acquire sleeplock on inodefile inode if not held.
static void read_dinode(uint inum, struct dinode *dip) {
  int holding_inodefile_lock = holdingsleep(&icache.inodefile.lock);
  if (!holding_inodefile_lock)
    locki(&icache.inodefile);

  readi(&icache.inodefile, (char *)dip, INODEOFF(inum), sizeof(*dip));

  if (!holding_inodefile_lock)
    unlocki(&icache.inodefile);

}

// Update the dinode by writing inode to disk
void update_dinode(struct inode* ip){
  int holding_inodefile_lock = holdingsleep(&icache.inodefile.lock);
  if (!holding_inodefile_lock)
    locki(&icache.inodefile);

  struct dinode curr_dinode;
  curr_dinode.type = ip->type;
  curr_dinode.devid = ip->devid;
  curr_dinode.size = ip->size;
  memmove(&curr_dinode.data, &ip->data, sizeof(struct extent) * EXTENTS);

  if (writei(&icache.inodefile, (char *) &curr_dinode, INODEOFF(ip->inum), sizeof(curr_dinode)) < 0) {
      panic("update_dinode: error writing inode to disk");
  }

  if (!holding_inodefile_lock)
    unlocki(&icache.inodefile);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not read
// the inode from from disk.
static struct inode *iget(uint dev, uint inum) {
  struct inode *ip, *empty;

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for (ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++) {
    if (ip->ref > 0 && ip->dev == dev && ip->inum == inum) {
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if (empty == 0 && ip->ref == 0) // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode cache entry.
  if (empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->ref = 1;
  ip->valid = 0;
  ip->dev = dev;
  ip->inum = inum;

  release(&icache.lock);

  return ip;
}


// looks up a path, if valid, populate its inode struct
struct inode *iopen(char *path) {
  struct inode* inode = namei(path);

  if (inode != NULL) {
    /*
    cprintf("=========== iopen: OPEN FILE %s ===========\n", path);
    char name[DIRSIZ];
    struct inode *parent_dir = nameiparent(path, name);
    struct dirent dirent;
    for (int off = 0; off < parent_dir->size; off += sizeof(dirent)) {
      if (concurrent_readi(parent_dir, (char *)&dirent, off, sizeof(dirent)) != sizeof(dirent))
        panic("dirlink read");
      if (dirent.inum == 0)
          continue;
      cprintf("name %s\n",dirent.name);
    }
    */
    
    locki(inode);
    unlocki(inode);
    //cprintf("iopen: %s, inum %d\n", path, inode->inum);
  }

  return inode;
}

struct inode *concurrent_icreate(char *path) {
  struct inode *inode;

  log_begin_tx();
  inode = icreate(path);
  log_commit_tx();

  return inode;
}

struct inode *icreate(char *path) {
  struct inode *inodefile = &icache.inodefile;

  // Create a new dinode
  struct dinode dinode;
  dinode.type = T_FILE;
  dinode.devid = 0;
  dinode.size = 0;
  memset(dinode.data, 0, sizeof(dinode.data));

  // inodefile is an array of dinodes
  // Search for the first inode in inodefile that is not in use
  int inum = inodefile->size / sizeof(dinode);
  for (int i = 0; i < NINODE; i++) {
    struct dinode dinode;
    read_dinode(i, &dinode);
    //cprintf("searching for inum: i %d, size %d\n", i, dinode.size);

    if (dinode.size == -1) {
      inum = i;
      break;
    }
  }
  concurrent_writei(inodefile, (char *)&dinode, INODEOFF(inum), sizeof(dinode));

  // Create a new directory entry and write it to the parent directory
  char name[DIRSIZ];
  struct inode *parent_dir = nameiparent(path, name);
  struct dirent dirent;

  // Search for the first empty directory entry
  uint off;
  for (off = 0; off < parent_dir->size; off += sizeof(dirent)) {
    if (concurrent_readi(parent_dir, (char *)&dirent, off, sizeof(dirent)) != sizeof(dirent))
      panic("dirlink read");
    if (dirent.inum == 0)
        break;
  }
  // Create the new directory entry
  dirent.inum = inum;
  strncpy(dirent.name, name, DIRSIZ);
  concurrent_writei(parent_dir, (char *)&dirent, off, sizeof(dirent));

  // Update the size of the inodefile and parent directory
  acquire(&icache.lock);
  if (inum >= inodefile->size / sizeof(dinode)) {
    inodefile->size += sizeof(dinode);
  }
  if (parent_dir->size == off) {
    parent_dir->size += sizeof(dirent);
  }
  release(&icache.lock);

  struct inode *inode = iget(inodefile->dev, inum);

  locki(inode);
  unlocki(inode);

  //cprintf("icreate END: %s, inum %d ,", path, inum);
  return inode;
}

int iunlink(char *path) {
  struct inode *inodefile = &icache.inodefile;
  struct inode *inode = namei(path);

  // The file does not exist
  if (inode == NULL) {
    //cprintf("iunlink error inode is NULL: %s\n", path);
    return -1;
  }
  // The path represents a directory or device
  /*
  if (inode->type != T_FILE) {
    cprintf("iunlink error inode is not a file: %s\n", path);
    return -1;
  }
  */

  acquire(&icache.lock);
  inode->ref--;  // namei() incremented ref, undo that
  
  // The file currently has an open reference to it
  if (inode->ref > 0) {
    //cprintf("iunlink error inode has %d open references: %s\n", inode->ref, path);
    release(&icache.lock);
    return -1;
  }
  release(&icache.lock);

  //cprintf("iunlink valid: %s, inum %d\n", path, inode->inum);

  // Remove the directory entry from the parent directory
  char name[DIRSIZ];
  struct inode *parent_dir = nameiparent(path, name);
  struct dirent dirent;
  uint off;
  for (off = 0; off < parent_dir->size; off += sizeof(dirent)) {
    if (concurrent_readi(parent_dir, (char *)&dirent, off, sizeof(dirent)) != sizeof(dirent))
      panic("dirlink read");
    if (dirent.inum == 0)
        continue;
    if (namecmp(dirent.name, name) == 0) {
      dirent.inum = 0;
      dirent.name[0] = '\0';
      concurrent_writei(parent_dir, (char *)&dirent, off, sizeof(dirent));
      break;
    }
  }

  // Remove the dinode from the inodefile
  struct dinode dinode;
  read_dinode(inode->inum, &dinode);
  dinode.size = -1;
  for (int i = 0; i < EXTENTS; i++) {
    if (dinode.data[i].nblocks != 0) {
      bfree(inode->dev, dinode.data[i].startblkno, dinode.data[i].nblocks);
    }
    dinode.data[i].startblkno = 0;
    dinode.data[i].nblocks = 0;
  }
  //cprintf("iunlink: set dinode %d size to -1\n", inode->inum);
  concurrent_writei(inodefile, (char *)&dinode, INODEOFF(inode->inum), sizeof(dinode));

  return 0;
}


// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode *idup(struct inode *ip) {
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
void irelease(struct inode *ip) {
  acquire(&icache.lock);
  // inode has no other references release
  if (ip->ref == 1)
    ip->type = 0;
  ip->ref--;
  release(&icache.lock);
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void locki(struct inode *ip) {
  struct dinode dip;

  if(ip == 0 || ip->ref < 1)
    panic("locki");

  acquiresleep(&ip->lock);

  if (ip->valid == 0) {

    if (ip != &icache.inodefile)
      locki(&icache.inodefile);
    read_dinode(ip->inum, &dip);
    if (ip != &icache.inodefile)
      unlocki(&icache.inodefile);

    ip->type = dip.type;
    ip->devid = dip.devid;

    ip->size = dip.size;
    for (int i = 0; i < EXTENTS; i++) {
      ip->data[i] = dip.data[i];
    }

    ip->valid = 1;

    if (ip->type == 0)
      panic("iget: no type");
  }
}

// Unlock the given inode.
void unlocki(struct inode *ip) {
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1) {
    cprintf("unlocki: panic ip %p, holding %d, ref %d\n", ip, ip->ref);
    panic("unlocki");
  }

  releasesleep(&ip->lock);
}

// threadsafe stati.
void concurrent_stati(struct inode *ip, struct stat *st) {
  locki(ip);
  stati(ip, st);
  unlocki(ip);
}

// Copy stat information from inode.
// Caller must hold ip->lock.
void stati(struct inode *ip, struct stat *st) {
  if (!holdingsleep(&ip->lock))
    panic("not holding lock");

  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->size = ip->size;
}

// threadsafe readi.
int concurrent_readi(struct inode *ip, char *dst, uint off, uint n) {
  int retval;

  locki(ip);
  retval = readi(ip, dst, off, n);
  unlocki(ip);

  return retval;
}

// Read data from inode.
// Returns number of bytes read.
// Caller must hold ip->lock.
int readi(struct inode *ip, char *dst, uint off, uint n) {
  uint tot, m;
  struct buf *bp;

  if (!holdingsleep(&ip->lock))
    panic("not holding lock");

  if (ip->type == T_DEV) {
    if (ip->devid < 0 || ip->devid >= NDEV || !devsw[ip->devid].read)
      return -1;
    return devsw[ip->devid].read(ip, dst, n);
  }

  if (off > ip->size || off + n < off)
    return -1;
  if (off + n > ip->size)
    n = ip->size - off;

  //cprintf("readi: size %d, off %d, n %d\n", ip->size, off, n);
  // Search for the extent that contains the starting block
  int idx_extent = 0;
  struct extent *cur_extent = ip->data;
  int total_size_extent = 0;
  while (total_size_extent + cur_extent->nblocks * BSIZE < off) {
    total_size_extent += cur_extent->nblocks * BSIZE;
    idx_extent++;
    cur_extent++;
    //cprintf("readi: search extent, idx_extent %d, total_size_extent %d, startblkno %d, nblocks %d\n", idx_extent, total_size_extent, cur_extent->startblkno, cur_extent->nblocks);
  }

  // Offset into the extent
  uint off_extent = off - total_size_extent;

  for (tot = 0; tot < n; tot += m, off_extent += m, dst += m) {
    // Reached the end of the extent
    if (off_extent >= cur_extent->nblocks * BSIZE) {
      // Move to the next extent, reset offset and m
      cur_extent++;
      off_extent = 0;
      m = 0;
      //cprintf("readi: move to next extent, off_extent %d, startblkno %d, nblocks %d\n", off_extent, cur_extent->startblkno, cur_extent->nblocks);
    } else {
      // Read from the current extent
      bp = bread(ip->dev, cur_extent->startblkno + off_extent / BSIZE);
      m = min(n - tot, BSIZE - off_extent % BSIZE);
      //cprintf("readi: sizeof(bp->data) %d, n %d, off_extent %d\n", sizeof(bp->data), n, off_extent);
      memmove(dst, bp->data + off_extent % BSIZE, m);
      brelse(bp);
    }
  }
  //cprintf("readi: size %d, off %d, n %d, tot %d, total_size_extent %d, off_extent %d\n", ip->size, off, n, tot, total_size_extent, off_extent);
  return n;
}

// threadsafe writei.
int concurrent_writei(struct inode *ip, char *src, uint off, uint n) {
  int retval;

  locki(ip);
  retval = writei(ip, src, off, n);
  unlocki(ip);

  return retval;
}

int log_concurrent_writei(struct inode *ip, char *src, uint off, uint n) {
  int retval;

  locki(ip);
  log_begin_tx();
  retval = writei(ip, src, off, n);
  log_commit_tx();
  unlocki(ip);

  return retval;
}

// Write data to inode.
// Returns number of bytes written.
// Caller must hold ip->lock.
int writei(struct inode *ip, char *src, uint off, uint n) {
  if (!holdingsleep(&ip->lock))
    panic("not holding lock");

  if (ip->type == T_DEV) {
    if (ip->devid < 0 || ip->devid >= NDEV || !devsw[ip->devid].write)
      return -1;
    return devsw[ip->devid].write(ip, src, n);
  }

  if (off + n < off) {
    cprintf("writei: error off + n < off\n");
    return -1;
  }

  // Determine the total capacity of the inode with all its extents
  // and search for the extent that contains the starting block
  int size = ip->size;
  int capacity = 0;
  int idx_extent = 0;
  int total_size_extent = 0;

  for (int i = 0; i < EXTENTS; i++) {
    int nblocks = ip->data[i].nblocks;
    if (nblocks == 0) break;

    capacity += nblocks * BSIZE;

    if (off >= capacity) {
      // The offset is beyond the current extent
      idx_extent++;
      total_size_extent += nblocks * BSIZE;
    }
  }

  if (off > size) {
    off = size;
  }
  // Offset into the extent
  int off_extent = 0;
  int diff = off - total_size_extent;
  if (diff > 0) {
    off_extent = diff;
  } 

  int n_overwrite = size - off;
  if (n_overwrite > n && n_overwrite > 0) {
    n_overwrite = n;
  }
  int n_append = off + n - size;
  //assertm(n_overwrite + n_append == n, "n_overwrite + n_append != n");
  //cprintf("writei: INUM %d, idx_extent %d, size %d, off %d, off_extent %d, n %d, n_overwrite %d, n_append %d, capacity %d\n", ip->inum, idx_extent, size, off, off_extent, n, n_overwrite, n_append, capacity);

  int retval = 0;
  if (n_overwrite > 0) {
    log_writei_file(ip, src, off_extent, n_overwrite, idx_extent);
    retval += n_overwrite;
  }

  if (n_append > 0) {
    int n_balloc = off + n - capacity;
    log_writei_append(ip, src, off_extent, n_append, n_balloc, idx_extent);
    retval += n_append;
    ip->size += n_append;
    update_dinode(ip);
  }

  return retval;
}

int writei_file(struct inode *ip, char *src, int off_extent, int n, int idx_extent) {
  int tot, m;
  struct buf *bp;
  struct extent *cur_extent = ip->data + idx_extent;

  for (tot = 0; tot < n; tot += m, off_extent += m, src += m) {
    // Reached the end of the extent
    if (off_extent >= cur_extent->nblocks * BSIZE) {
      // Move to the next extent, reset offset and m
      
      //if (cur_extent->nblocks != 0) cprintf("writei_file: INUM %d, prev extent m %d, off_extent %d, startblkno %d, nblocks %d\n", ip->inum, m, off_extent, cur_extent->startblkno, cur_extent->nblocks);
      cur_extent++;
      off_extent = 0;
      m = 0;
      //if (cur_extent->nblocks != 0) cprintf("writei_file: INUM %d, next extent m %d, off_extent %d, startblkno %d, nblocks %d\n", ip->inum, m, off_extent, cur_extent->startblkno, cur_extent->nblocks);
    } else {
      // Write to the current extent
      if (cur_extent->startblkno >= FSSIZE) {
        for (int i = 0; i < EXTENTS; i++) {
          //if (cur_extent->nblocks != 0) cprintf("writei_file: INUM %d, startblkno %d, nblocks %d. off_extent %d, idx_extend %d\n", ip->inum, ip->data[i].startblkno, ip->data[i].nblocks, off_extent, idx_extent);
        }
        //cprintf("writei_file: startblkno %d >= FSSIZE %d\n", cur_extent->startblkno, FSSIZE);
      }
      bp = bread(ip->dev, cur_extent->startblkno + off_extent / BSIZE);
      m = min(n - tot, BSIZE - off_extent % BSIZE);
      memmove(bp->data + off_extent % BSIZE, src, m);
      bwrite(bp);
      brelse(bp);
    }
  }
  return n;
}

int writei_append(struct inode *ip, char *src, int off_extent, int n_append, int n_balloc, int idx_extent) {
  struct extent *cur_extent = ip->data + idx_extent;

  if (n_balloc > 0) {
    // Allocate new blocks
    int nblocks = n_balloc / BSIZE + (n_balloc % BSIZE == 0 ? 0 : 1);
    int startblkno = balloc(ip->dev, nblocks);

    // The inode has already an extent at idx_extent, move to the next extent
    int allocated_extent = idx_extent;
    while (cur_extent->nblocks != 0) {
      allocated_extent++;
      cur_extent++;
    }
    
    // Update inode
    cprintf("writei_append: INUM %d, allocate %d bytes for extent %d, nblocks %d, startblkno %d\n", ip->inum, n_balloc, allocated_extent, nblocks, startblkno);
    cur_extent->startblkno = startblkno;
    cur_extent->nblocks = nblocks;
  }

  int tot, m;
  struct buf *bp;
  cur_extent = ip->data + idx_extent;

  for (tot = 0; tot < n_append; tot += m, off_extent += m, src += m) {
    // Reached the end of the extent
    if (off_extent >= cur_extent->nblocks * BSIZE) {
      // Move to the next extent, reset offset and m
      
      //if (cur_extent->nblocks != 0) cprintf("writei_file: INUM %d, prev extent m %d, off_extent %d, startblkno %d, nblocks %d\n", ip->inum, m, off_extent, cur_extent->startblkno, cur_extent->nblocks);
      cur_extent++;
      off_extent = 0;
      m = 0;
      //if (cur_extent->nblocks != 0) cprintf("writei_file: INUM %d, next extent m %d, off_extent %d, startblkno %d, nblocks %d\n", ip->inum, m, off_extent, cur_extent->startblkno, cur_extent->nblocks);
    } else {
      // Write to the current extent
      if (cur_extent->startblkno >= FSSIZE) {
        for (int i = 0; i < EXTENTS; i++) {
          //if (cur_extent->nblocks != 0) cprintf("writei_file: INUM %d, startblkno %d, nblocks %d. off_extent %d, idx_extend %d\n", ip->inum, ip->data[i].startblkno, ip->data[i].nblocks, off_extent, idx_extent);
        }
        //cprintf("writei_file: startblkno %d >= FSSIZE %d\n", cur_extent->startblkno, FSSIZE);
      }
      bp = bread(ip->dev, cur_extent->startblkno + off_extent / BSIZE);
      m = min(n_append - tot, BSIZE - off_extent % BSIZE);
      memmove(bp->data + off_extent % BSIZE, src, m);
      bwrite(bp);
      brelse(bp);
    }
  }

  // TODO: update dinode in inodefile
  return n_append;
}

int log_writei_file(struct inode *ip, char *src, int off_extent, int n, int idx_extent) {
  int tot, m;
  struct buf *bp;
  struct extent *cur_extent = ip->data + idx_extent;

  for (tot = 0; tot < n; tot += m, off_extent += m, src += m) {
    // Reached the end of the extent
    if (off_extent >= cur_extent->nblocks * BSIZE) {
      // Move to the next extent, reset offset and m
      
      //if (cur_extent->nblocks != 0) cprintf("writei_file: INUM %d, prev extent m %d, off_extent %d, startblkno %d, nblocks %d\n", ip->inum, m, off_extent, cur_extent->startblkno, cur_extent->nblocks);
      cur_extent++;
      off_extent = 0;
      m = 0;
      //if (cur_extent->nblocks != 0) cprintf("writei_file: INUM %d, next extent m %d, off_extent %d, startblkno %d, nblocks %d\n", ip->inum, m, off_extent, cur_extent->startblkno, cur_extent->nblocks);
    } else {
      // Write to the current extent
      if (cur_extent->startblkno >= FSSIZE) {
        for (int i = 0; i < EXTENTS; i++) {
          //if (cur_extent->nblocks != 0) cprintf("writei_file: INUM %d, startblkno %d, nblocks %d. off_extent %d, idx_extend %d\n", ip->inum, ip->data[i].startblkno, ip->data[i].nblocks, off_extent, idx_extent);
        }
        //cprintf("writei_file: startblkno %d >= FSSIZE %d\n", cur_extent->startblkno, FSSIZE);
      }
      bp = bread(ip->dev, cur_extent->startblkno + off_extent / BSIZE);
      m = min(n - tot, BSIZE - off_extent % BSIZE);
      memmove(bp->data + off_extent % BSIZE, src, m);
      log_write(bp);
      brelse(bp);
    }
  }
  return n;
}

int log_writei_append(struct inode *ip, char *src, int off_extent, int n_append, int n_balloc, int idx_extent) {
  struct extent *cur_extent = ip->data + idx_extent;

  if (n_balloc > 0) {
    // Allocate new blocks
    int nblocks = n_balloc / BSIZE + (n_balloc % BSIZE == 0 ? 0 : 1);
    int startblkno = balloc(ip->dev, nblocks);

    // The inode has already an extent at idx_extent, move to the next extent
    int allocated_extent = idx_extent;
    while (cur_extent->nblocks != 0) {
      allocated_extent++;
      cur_extent++;
    }
    
    // Update inode
    cprintf("writei_append: INUM %d, allocate %d bytes for extent %d, nblocks %d, startblkno %d\n", ip->inum, n_balloc, allocated_extent, nblocks, startblkno);
    cur_extent->startblkno = startblkno;
    cur_extent->nblocks = nblocks;
  }

  int tot, m;
  struct buf *bp;
  cur_extent = ip->data + idx_extent;

  for (tot = 0; tot < n_append; tot += m, off_extent += m, src += m) {
    // Reached the end of the extent
    if (off_extent >= cur_extent->nblocks * BSIZE) {
      // Move to the next extent, reset offset and m
      
      //if (cur_extent->nblocks != 0) cprintf("writei_file: INUM %d, prev extent m %d, off_extent %d, startblkno %d, nblocks %d\n", ip->inum, m, off_extent, cur_extent->startblkno, cur_extent->nblocks);
      cur_extent++;
      off_extent = 0;
      m = 0;
      //if (cur_extent->nblocks != 0) cprintf("writei_file: INUM %d, next extent m %d, off_extent %d, startblkno %d, nblocks %d\n", ip->inum, m, off_extent, cur_extent->startblkno, cur_extent->nblocks);
    } else {
      // Write to the current extent
      if (cur_extent->startblkno >= FSSIZE) {
        for (int i = 0; i < EXTENTS; i++) {
          //if (cur_extent->nblocks != 0) cprintf("writei_file: INUM %d, startblkno %d, nblocks %d. off_extent %d, idx_extend %d\n", ip->inum, ip->data[i].startblkno, ip->data[i].nblocks, off_extent, idx_extent);
        }
        //cprintf("writei_file: startblkno %d >= FSSIZE %d\n", cur_extent->startblkno, FSSIZE);
      }
      bp = bread(ip->dev, cur_extent->startblkno + off_extent / BSIZE);
      m = min(n_append - tot, BSIZE - off_extent % BSIZE);
      memmove(bp->data + off_extent % BSIZE, src, m);
      log_write(bp);
      brelse(bp);
    }
  }

  return n_append;
}

// Directories

int namecmp(const char *s, const char *t) { return strncmp(s, t, DIRSIZ); }

struct inode *rootlookup(char *name) {
  return dirlookup(namei("/"), name, 0);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode *dirlookup(struct inode *dp, char *name, uint *poff) {
  uint off, inum;
  struct dirent de;

  if (dp->type != T_DIR)
    panic("dirlookup not DIR");

  for (off = 0; off < dp->size; off += sizeof(de)) {
    if (readi(dp, (char *)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if (de.inum == 0)
      continue;
    if (namecmp(name, de.name) == 0) {
      // entry matches path element
      if (poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char *skipelem(char *path, char *name) {
  char *s;
  int len;

  while (*path == '/')
    path++;
  if (*path == 0)
    return 0;
  s = path;
  while (*path != '/' && *path != 0)
    path++;
  len = path - s;
  if (len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while (*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name. Returns NULL if not found
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode *namex(char *path, int nameiparent, char *name) {
  struct inode *ip, *next;

  if (*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(namei("/"));

  while ((path = skipelem(path, name)) != 0) {
    locki(ip);
    if (ip->type != T_DIR) {
      unlocki(ip);
      goto notfound;
    }

    // Stop one level early.
    if (nameiparent && *path == '\0') {
      unlocki(ip);
      return ip;
    }

    if ((next = dirlookup(ip, name, 0)) == 0) {
      unlocki(ip);
      goto notfound;
    }

    unlocki(ip);
    irelease(ip);
    ip = next;
  }
  if (nameiparent)
    goto notfound;

  return ip;

notfound:
  irelease(ip);
  return 0;
}

/*
See namex
*/
struct inode *namei(char *path) {
  char name[DIRSIZ];
  return namex(path, 0, name);
}

/*
See namex
*/
struct inode *nameiparent(char *path, char *name) {
  return namex(path, 1, name);
}
