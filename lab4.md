### Question 1

`dirlookup` search for a directory entry (file) within a given directory path. It takes the directory inode `dp`, a file name to look up, and a pointer `poff` to store the byte offset of the found entry.

- iterates through the directory entries (struct dirent) within the directory.
- use `readi` to read from the directory entry into `de`.
- checks if `de.inum = 0`, where 0 means the entry is unused or deleted. In this case, it skips the dirent.
- compares the name of the current directory entry (de.name) with the target file name.
- If an entry is found, sets the poff pointer to the byte offset of the entry.
- returns the in memory inode of the found entry using `iget`.

### Question 2

### Question 3
- Daniel ~20 hour

### Question 4
Daniel: I found this lab harder than the previous ones. The concurrency issues where hard to debug. Also some hints for the specifications to pass the stress tests would have been helpful especially the fact that the directory entries needed to be reused. Other than that, the lab was very interesting and I learned a lot about concurreny and file system implementation.