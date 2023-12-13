//
// File descriptors
//

#include "../inc/cdefs.h"
#include "../inc/defs.h"
#include "../inc/proc.h"
#include <cdefs.h>
#include <defs.h>
#include <fcntl.h>
#include <file.h>
#include <fs.h>
#include <param.h>
#include <proc.h>
#include <sleeplock.h>
#include <spinlock.h>
#include <stat.h>

struct devsw devsw[NDEV];

static struct file_info file_table[NFILE];
struct spinlock file_table_lock;

/** Open the file specified by path in the given access mode.
 * @param access_mode The file access mode.
 * @param path The path to the file to open.
 * @return The file descriptor index for the current process.
 */ 
int file_open(int access_mode, char *path) {
    struct proc *my_proc = myproc(); 

    // Finds an open spot in the process file table
    int proc_ftable_index = 0;
    while (proc_ftable_index < NOFILE && my_proc->files[proc_ftable_index] != NULL) {
        ++proc_ftable_index;   
    }

    // Error if the process file table is full
    if (proc_ftable_index == NOFILE) {
        return -1;
    }

    // Finds an open entry in the global file table
    int global_ftable_index = 0;
    while (global_ftable_index < NFILE && file_table[global_ftable_index].ref_count != 0) {
        ++global_ftable_index;   
    }

    struct inode *inode_ptr = iopen(path);
    if (inode_ptr == NULL && (access_mode & 0xF00) == O_CREATE) {
        inode_ptr = concurrent_icreate(path);
    }

    if (inode_ptr == NULL) {
        return -1;
    }
    
    acquire(&file_table_lock);
    file_table[global_ftable_index] = (struct file_info){ 
      .node=inode_ptr,
      .pipe=NULL,
      .isPipe=0,
      .offset=0,
      .mode=access_mode,
      .ref_count=1,
      .path=path,
      .gfd=global_ftable_index
    };
    release(&file_table_lock);

    my_proc->files[proc_ftable_index] = &file_table[global_ftable_index];

    return proc_ftable_index;
}

int file_dup(int fd_copy) {
  struct proc *my_proc = (struct proc *)myproc();
  struct file_info *fi = myproc()->files[fd_copy];

  // Finds an open spot in the process file table
  int fd = -1;
  for (int i = 0; i < NOFILE && fd == -1; i++) {
    if (my_proc->files[i] == NULL) {
      fd = i;
    }
  }
  
  if (fd == -1) return -1;

  if (fi->isPipe) {
    acquire(&fi->pipe->lock);
    if(fi->mode==O_RDONLY) fi->pipe->read_count++;
    if(fi->mode==O_WRONLY) fi->pipe->write_count++;
    release(&fi->pipe->lock);
  } else {
    if (fi->node == NULL) return -1;
  }

  acquire(&file_table_lock);
  my_proc->files[fd] = fi;
  fi->ref_count++;
  release(&file_table_lock);

  return fd;
}

int file_stat(int fd, struct stat *stat_ptr) {
  struct proc *my_proc = (struct proc *)myproc();
  acquire(&file_table_lock);
  if (my_proc->files[fd] == NULL) {
    // no open file at this descriptor
    release(&file_table_lock);
    return -1;
  }
  if (my_proc->files[fd]->node == NULL) {
    // underlying inode ptr is null
    return -1;
  }
  struct inode *node = my_proc->files[fd]->node;
  release(&file_table_lock);
  concurrent_stati(node, stat_ptr);
  return 0;
}

int pipe_write(int fd, char *buf, int nr_bytes) {
  if (nr_bytes > PIPE_BUFFER_SIZE) return -1;
  struct file_info *file = myproc()->files[fd];
  struct pipe *pipe = file->pipe;
  acquire(&pipe->lock);
  if (pipe->read_count==0) {
    release(&pipe->lock);
    return -1;
  }
  while(PIPE_BUFFER_SIZE - pipe->write_offset + pipe->read_offset < nr_bytes) {
    if (pipe->read_count == 0) {
      release(&pipe->lock);
      return -1;
    }
    sleep(&pipe->notFull, &pipe->lock);
  }
  memmove((void *)(pipe->buffer+(pipe->write_offset%PIPE_BUFFER_SIZE)),(void *)buf, nr_bytes);
  pipe->write_offset += nr_bytes;
  wakeup(&pipe->notEmpty);
  release(&pipe->lock);
  return nr_bytes;
}

int file_write(int fd, char *buf, int nr_bytes) {
  struct proc *my_proc = (struct proc *)myproc();
  struct file_info *file = my_proc->files[fd];
  if (file == NULL) {
    return -1;
  }
  if (file->mode == O_RDONLY) {
    return -1;
  }

  if(file->isPipe) {
    return pipe_write(fd, buf, nr_bytes);
  }

  acquiresleep(&file->lock);
  int offset = concurrent_writei(file->node, buf, file->offset, nr_bytes);
  releasesleep(&file->lock);

  acquire(&file_table_lock);
  my_proc->files[fd]->offset += offset;
  release(&file_table_lock);
  return offset;
}


int pipe_read(int fd, char *buf, int nr_bytes) {
  struct file_info *file = myproc()->files[fd];
  struct pipe *pipe = file->pipe;
  acquire(&pipe->lock);
  
  if (pipe->write_count == 0) {
    int write_read_diff = pipe->write_offset - pipe->read_offset;
    if (nr_bytes > write_read_diff) {
      nr_bytes = write_read_diff;
    }
    goto read;
  }
  
  while (pipe->write_offset - pipe->read_offset < nr_bytes) {
    if (pipe->write_count == 0)  {
      nr_bytes = pipe->write_offset - pipe->read_offset;
      goto read;
    }
    sleep(&pipe->notEmpty, &pipe->lock);
  }
read:
  memmove((void *)buf,(void *)(pipe->buffer+(pipe->read_offset%PIPE_BUFFER_SIZE)), nr_bytes);
  pipe->read_offset += nr_bytes;
  wakeup(&pipe->notFull);
  release(&pipe->lock);
  return nr_bytes;
}

int file_read(int fd, char *buf, int nr_bytes) {
  struct proc *my_proc = (struct proc *)myproc();
  struct file_info *fi = myproc()->files[fd];
  if (fi == NULL) {
    // no open file at this descriptor
    return -1;
  }
  if (fi->mode == O_WRONLY)
    return -1;
  if(fi->isPipe) {
    return pipe_read(fd, buf, nr_bytes);
  }
  acquiresleep(&fi->lock);
  int offset = concurrent_readi(fi->node, buf, fi->offset, nr_bytes);
  releasesleep(&fi->lock);

  acquire(&file_table_lock);
  my_proc->files[fd]->offset += offset;
  release(&file_table_lock);
  return offset;
}

int file_close(int fd) {
  struct proc *my_proc = myproc();
  struct file_info *fi = my_proc->files[fd];

  acquire(&file_table_lock);
  if (fi->isPipe) {
    if (fi->pipe == NULL) {
      return -1;
    }

    acquire(&fi->pipe->lock);
    if (fi->mode == O_RDONLY) {
      if (--fi->pipe->read_count == 0) {
        if (fi->pipe->write_count == 0) {
          release(&fi->pipe->lock);
          kfree((char *) fi->pipe);
        } else {
          wakeup(&fi->pipe->notFull);
          release(&fi->pipe->lock);
        }
        *fi = (struct file_info) { 0 };
      }
    } else {
      if (--fi->pipe->write_count == 0) {
        if (fi->pipe->read_count == 0) {
          release(&fi->pipe->lock);
          kfree((char *) fi->pipe);
        } else {
          wakeup(&fi->pipe->notEmpty);
          release(&fi->pipe->lock);
        }
        *fi = (struct file_info) { 0 };
      }
    }
    if (fi->pipe != NULL) {
      release(&fi->pipe->lock);
    }

  } else {

    // Clean up if this is the last reference to the file_info
    // cprintf("file_close: ref_count = %d\n", fi->ref_count);
    acquiresleep(&fi->lock);
    if (--fi->ref_count <= 0) {
        // Release the inode if this is the last reference to it
        irelease(fi->node);
        //cprintf("file_close: inode ref = %d\n", fi->node->ref);
          
        *fi = (struct file_info) { 0 };
    }
    releasesleep(&fi->lock);
  }
  release(&file_table_lock);

  // Remove the file_info from the process's file table
  my_proc->files[fd] = NULL;
  return 0;
}

int pipe(int *fd_arr) {
  struct pipe *pipe;
  if((pipe = (struct pipe *)kalloc()) == 0) {
      return -1;
  }
  pipe->read_count = 1;
  pipe->write_count = 1;
  pipe->read_offset = 0;
  pipe->write_offset = 0;
  initlock(&pipe->lock, "pipelock");
  
  int j = 0;
  for (int i = 0; i < NOFILE; i++) {
    if (myproc()->files[i] == NULL) {
      fd_arr[j++] = i;
      if(j == 2) break;
    }
  }
  if (fd_arr[1] == -1) {
    kfree((char *) pipe);
    return -1;
  }

  j = 0;
  int gfd[2] = {-1,-1};
  acquire(&file_table_lock);
  for (int i = 0; i < NFILE; i++) {
    if (file_table[i].ref_count)
      continue;
    gfd[j++] = i;
    if(j == 2) break;
  }
  if (gfd[1] == -1) {
    // no free global file descriptor
    release(&file_table_lock);
    kfree((char *) pipe);
    return -1;
  }
  
  struct file_info fi_read = {
    .node = NULL,
    .pipe = pipe,
    .isPipe = 1,
    .offset = 0,
    .mode = O_RDONLY,
    .ref_count = 1,
    .path = NULL,
    .gfd = gfd[0]
    };
  struct file_info fi_write = {
    .node = NULL,
    .pipe = pipe,
    .isPipe = 1,
    .offset = 0,
    .mode = O_WRONLY,
    .ref_count = 1,
    .path = NULL,
    .gfd = gfd[1]
  };
  file_table[gfd[0]] = fi_read;
  file_table[gfd[1]] = fi_write;
  myproc()->files[fd_arr[0]] = &(file_table[gfd[0]]);
  myproc()->files[fd_arr[1]] = &(file_table[gfd[1]]);
  release(&file_table_lock);
  return 0;
}

int file_unlink(char *path) {
  return iunlink(path);
}

